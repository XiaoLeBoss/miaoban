/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_random.h"
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif

#include "esp_blufi_api.h"
#include "blufi_example.h"

#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "mbedtls/md5.h"
#include "esp_crc.h"

/*
   The SEC_TYPE_xxx is for self-defined packet data type in the procedure of "BLUFI negotiate key"
   If user use other negotiation procedure to exchange(or generate) key, should redefine the type by yourself.
 */
#define SEC_TYPE_DH_PARAM_LEN   0x00
#define SEC_TYPE_DH_PARAM_DATA  0x01
#define SEC_TYPE_DH_P           0x02
#define SEC_TYPE_DH_G           0x03
#define SEC_TYPE_DH_PUBLIC      0x04


struct blufi_security {
#define DH_SELF_PUB_KEY_LEN     128
    uint8_t  self_public_key[DH_SELF_PUB_KEY_LEN];
#define SHARE_KEY_LEN           128
    uint8_t  share_key[SHARE_KEY_LEN];
    size_t   share_len;
#define PSK_LEN                 16
    uint8_t  psk[PSK_LEN];
    uint8_t  *dh_param;
    int      dh_param_len;
    uint8_t  iv[16];
    mbedtls_dhm_context dhm;
    mbedtls_aes_context aes;
};
static struct blufi_security *blufi_sec;

/**
 * @brief 随机数生成回调函数
 * 
 * @param[in] rng_state 随机数生成器状态（未使用）
 * @param[out] output 生成的随机数输出缓冲区
 * @param[in] len 需要生成的随机数长度
 * 
 * @return int 固定返回 0
 * 
 * @note 使用 esp_fill_random 生成硬件随机数
 */
static int myrand( void *rng_state, unsigned char *output, size_t len )
{
    esp_fill_random(output, len);
    return( 0 );
}

extern void btc_blufi_report_error(esp_blufi_error_state_t state);

/**
 * @brief DH 协商数据处理函数
 * 
 * @param[in] data 接收到的数据
 * @param[in] len 数据长度
 * @param[out] output_data 输出数据指针（用于发送回手机）
 * @param[out] output_len 输出数据长度
 * @param[out] need_free 是否需要释放输出数据内存
 * 
 * @return void 无返回值
 * 
 * @note 处理 DH 参数协商，计算共享密钥，生成 AES 密钥
 */
void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free)
{
    if (data == NULL || len < 3) {
        BLUFI_ERROR("BLUFI Invalid data format");
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    int ret;
    uint8_t type = data[0];

    if (blufi_sec == NULL) {
        BLUFI_ERROR("BLUFI Security is not initialized");
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    switch (type) {
    case SEC_TYPE_DH_PARAM_LEN:
        blufi_sec->dh_param_len = ((data[1]<<8)|data[2]);
        if (blufi_sec->dh_param) {
            free(blufi_sec->dh_param);
            blufi_sec->dh_param = NULL;
        }
        blufi_sec->dh_param = (uint8_t *)malloc(blufi_sec->dh_param_len);
        if (blufi_sec->dh_param == NULL) {
            blufi_sec->dh_param_len = 0;  /* Reset length to avoid using unallocated memory */
            btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            BLUFI_ERROR("%s, malloc failed\n", __func__);
            return;
        }
        break;
    case SEC_TYPE_DH_PARAM_DATA:{
        if (blufi_sec->dh_param == NULL) {
            BLUFI_ERROR("%s, blufi_sec->dh_param == NULL\n", __func__);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        if (len < (blufi_sec->dh_param_len + 1)) {
            BLUFI_ERROR("%s, invalid dh param len\n", __func__);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        uint8_t *param = blufi_sec->dh_param;
        memcpy(blufi_sec->dh_param, &data[1], blufi_sec->dh_param_len);
        ret = mbedtls_dhm_read_params(&blufi_sec->dhm, &param, &param[blufi_sec->dh_param_len]);
        if (ret) {
            BLUFI_ERROR("%s read param failed %d\n", __func__, ret);
            btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
            return;
        }
        free(blufi_sec->dh_param);
        blufi_sec->dh_param = NULL;

        const int dhm_len = mbedtls_dhm_get_len(&blufi_sec->dhm);

        if (dhm_len > DH_SELF_PUB_KEY_LEN) {
            BLUFI_ERROR("%s dhm len not support %d\n", __func__, dhm_len);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        ret = mbedtls_dhm_make_public(&blufi_sec->dhm, dhm_len, blufi_sec->self_public_key, DH_SELF_PUB_KEY_LEN, myrand, NULL);
        if (ret) {
            BLUFI_ERROR("%s make public failed %d\n", __func__, ret);
            btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
            return;
        }

        ret = mbedtls_dhm_calc_secret( &blufi_sec->dhm,
                blufi_sec->share_key,
                SHARE_KEY_LEN,
                &blufi_sec->share_len,
                myrand, NULL);
        if (ret) {
            BLUFI_ERROR("%s mbedtls_dhm_calc_secret failed %d\n", __func__, ret);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        ret = mbedtls_md5(blufi_sec->share_key, blufi_sec->share_len, blufi_sec->psk);

        if (ret) {
            BLUFI_ERROR("%s mbedtls_md5 failed %d\n", __func__, ret);
            btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);
            return;
        }

        mbedtls_aes_setkey_enc(&blufi_sec->aes, blufi_sec->psk, PSK_LEN * 8);

        /* alloc output data */
        *output_data = &blufi_sec->self_public_key[0];
        *output_len = dhm_len;
        *need_free = false;

    }
        break;
    case SEC_TYPE_DH_P:
        break;
    case SEC_TYPE_DH_G:
        break;
    case SEC_TYPE_DH_PUBLIC:
        break;
    }
}

/**
 * @brief AES 加密函数
 * 
 * @param[in] iv8 初始化向量的第 8 字节（用于构造 IV）
 * @param[in,out] crypt_data 需要加密的数据（加密后原地替换）
 * @param[in] crypt_len 数据长度
 * 
 * @return int 加密后的数据长度，失败返回 -1
 * 
 * @note 使用 CFB128 模式进行 AES 加密
 */
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    int ret;
    size_t iv_offset = 0;
    uint8_t iv0[16];

    if (!blufi_sec) {
        return -1;
    }

    memcpy(iv0, blufi_sec->iv, sizeof(blufi_sec->iv));
    iv0[0] = iv8;   /* set iv8 as the iv0[0] */

    ret = mbedtls_aes_crypt_cfb128(&blufi_sec->aes, MBEDTLS_AES_ENCRYPT, crypt_len, &iv_offset, iv0, crypt_data, crypt_data);
    if (ret) {
        return -1;
    }

    return crypt_len;
}

/**
 * @brief AES 解密函数
 * 
 * @param[in] iv8 初始化向量的第 8 字节（用于构造 IV）
 * @param[in,out] crypt_data 需要解密的数据（解密后原地替换）
 * @param[in] crypt_len 数据长度
 * 
 * @return int 解密后的数据长度，失败返回 -1
 * 
 * @note 使用 CFB128 模式进行 AES 解密
 */
int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    int ret;
    size_t iv_offset = 0;
    uint8_t iv0[16];

    if (!blufi_sec) {
        return -1;
    }

    memcpy(iv0, blufi_sec->iv, sizeof(blufi_sec->iv));
    iv0[0] = iv8;   /* set iv8 as the iv0[0] */

    ret = mbedtls_aes_crypt_cfb128(&blufi_sec->aes, MBEDTLS_AES_DECRYPT, crypt_len, &iv_offset, iv0, crypt_data, crypt_data);
    if (ret) {
        return -1;
    }

    return crypt_len;
}

/**
 * @brief CRC 校验和计算函数
 * 
 * @param[in] iv8 初始化向量（此处未使用）
 * @param[in] data 需要计算校验和的数据
 * @param[in] len 数据长度
 * 
 * @return uint16_t 计算得到的 CRC16 校验和
 * 
 * @note 使用 ESP 硬件 CRC16-BE 算法
 */
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len)
{
    /* This iv8 ignore, not used */
    return esp_crc16_be(0, data, len);
}

/**
 * @brief 初始化 BluFi 安全模块
 * 
 * @param void 无参数
 * 
 * @return esp_err_t 成功返回 0 (ESP_OK)，失败返回 ESP_FAIL
 * 
 * @note 分配内存并初始化 DHM 和 AES 上下文
 */
esp_err_t blufi_security_init(void)
{
    blufi_sec = (struct blufi_security *)malloc(sizeof(struct blufi_security));
    if (blufi_sec == NULL) {
        return ESP_FAIL;
    }

    memset(blufi_sec, 0x0, sizeof(struct blufi_security));

    mbedtls_dhm_init(&blufi_sec->dhm);
    mbedtls_aes_init(&blufi_sec->aes);

    memset(blufi_sec->iv, 0x0, sizeof(blufi_sec->iv));
    return 0;
}

/**
 * @brief 反初始化 BluFi 安全模块
 * 
 * @param void 无参数
 * 
 * @return void 无返回值
 * 
 * @note 释放所有分配的内存和安全上下文
 */
void blufi_security_deinit(void)
{
    if (blufi_sec == NULL) {
        return;
    }
    if (blufi_sec->dh_param){
        free(blufi_sec->dh_param);
        blufi_sec->dh_param = NULL;
    }
    mbedtls_dhm_free(&blufi_sec->dhm);
    mbedtls_aes_free(&blufi_sec->aes);

    memset(blufi_sec, 0x0, sizeof(struct blufi_security));

    free(blufi_sec);
    blufi_sec = NULL;
}
