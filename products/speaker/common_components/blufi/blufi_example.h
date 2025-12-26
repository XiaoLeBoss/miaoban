/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */


#pragma once

#define BLUFI_EXAMPLE_TAG "BLUFI_EXAMPLE"
#define BLUFI_INFO(fmt, ...)   ESP_LOGI(BLUFI_EXAMPLE_TAG, fmt, ##__VA_ARGS__)
#define BLUFI_ERROR(fmt, ...)  ESP_LOGE(BLUFI_EXAMPLE_TAG, fmt, ##__VA_ARGS__)

/**
 * @brief DH 协商数据处理函数
 * 
 * @param[in] data 接收到的数据
 * @param[in] len 数据长度
 * @param[out] output_data 输出数据指针
 * @param[out] output_len 输出数据长度
 * @param[out] need_free 是否需要释放输出数据
 * 
 * @return void
 */
void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free);

/**
 * @brief AES 加密函数
 * 
 * @param[in] iv8 初始化向量字节
 * @param[in,out] crypt_data 数据指针
 * @param[in] crypt_len 数据长度
 * 
 * @return int 加密后长度，失败返回 -1
 */
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

/**
 * @brief AES 解密函数
 * 
 * @param[in] iv8 初始化向量字节
 * @param[in,out] crypt_data 数据指针
 * @param[in] crypt_len 数据长度
 * 
 * @return int 解密后长度，失败返回 -1
 */
int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

/**
 * @brief CRC 校验和计算
 * 
 * @param[in] iv8 初始化向量（未使用）
 * @param[in] data 数据指针
 * @param[in] len 数据长度
 * 
 * @return uint16_t CRC16 校验和
 */
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);

/**
 * @brief 初始化 BluFi 安全模块
 * 
 * @return int 成功返回 0，失败返回错误码
 */
int blufi_security_init(void);

/**
 * @brief 反初始化 BluFi 安全模块
 */
void blufi_security_deinit(void);

/**
 * @brief 注册 GAP 回调函数
 * 
 * @return int 成功返回 0，失败返回错误码
 */
int esp_blufi_gap_register_callback(void);

/**
 * @brief 初始化 BluFi 主机
 * 
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 */
esp_err_t esp_blufi_host_init(void);

/**
 * @brief 初始化 BluFi 主机和回调
 * 
 * @param[in] callbacks 回调函数结构体
 * 
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 */
esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *callbacks);

/**
 * @brief 反初始化 BluFi 主机
 * 
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 */
esp_err_t esp_blufi_host_deinit(void);

/**
 * @brief 初始化蓝牙控制器
 * 
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 */
esp_err_t esp_blufi_controller_init(void);

/**
 * @brief 反初始化蓝牙控制器
 * 
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 */
esp_err_t esp_blufi_controller_deinit(void);
