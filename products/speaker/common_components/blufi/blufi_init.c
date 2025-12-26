/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include "esp_err.h"
#include "esp_blufi_api.h"
#include "esp_log.h"
#include "esp_blufi.h"
#include "esp_idf_version.h"
#include "blufi_example.h"
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif
#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "console/console.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
static bool nimble_running = false;
#endif

#ifdef CONFIG_BT_BLUEDROID_ENABLED
/**
 * @brief 初始化 Bluedroid 主机
 *
 * @param void 无参数
 *
 * @return esp_err_t 成功返回 ESP_OK，失败返回 ESP_FAIL
 *
 * @note 初始化并使能 Bluedroid 栈，打印蓝牙地址
 */
esp_err_t esp_blufi_host_init(void)
{
    int ret;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&cfg);
#else
    ret = esp_bluedroid_init();
#endif
    if (ret)
    {
        BLUFI_ERROR("%s init bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ret = esp_bluedroid_enable();
    if (ret)
    {
        BLUFI_ERROR("%s init bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    BLUFI_INFO("BD ADDR: " ESP_BD_ADDR_STR "\n", ESP_BD_ADDR_HEX(esp_bt_dev_get_address()));

    return ESP_OK;
}

/**
 * @brief 反初始化 Bluedroid 主机
 *
 * @param void 无参数
 *
 * @return esp_err_t 成功返回 ESP_OK，失败返回 ESP_FAIL
 *
 * @note 禁用并反初始化 Bluedroid 栈，同时反初始化 BluFi Profile
 */
esp_err_t esp_blufi_host_deinit(void)
{
    int ret;
    ret = esp_blufi_profile_deinit();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_bluedroid_disable();
    if (ret)
    {
        BLUFI_ERROR("%s deinit bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ret = esp_bluedroid_deinit();
    if (ret)
    {
        BLUFI_ERROR("%s deinit bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 注册 GAP 回调函数
 *
 * @param void 无参数
 *
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 *
 * @note 注册 GAP 事件处理函数并初始化 BluFi Profile
 */
esp_err_t esp_blufi_gap_register_callback(void)
{
    int rc;
    rc = esp_ble_gap_register_callback(esp_blufi_gap_event_handler);
    if (rc)
    {
        return rc;
    }
    return esp_blufi_profile_init();
}

/**
 * @brief 初始化 BluFi 主机和回调
 *
 * @param[in] example_callbacks BluFi 回调函数结构体指针
 *
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 *
 * @note 初始化主机，注册 BluFi 回调，注册 GAP 回调
 */
esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *example_callbacks)
{
    esp_err_t ret = ESP_OK;

    ret = esp_blufi_host_init();
    if (ret)
    {
        BLUFI_ERROR("%s initialise host failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_blufi_register_callbacks(example_callbacks);
    if (ret)
    {
        BLUFI_ERROR("%s blufi register failed, error code = %x\n", __func__, ret);
        return ret;
    }

    ret = esp_blufi_gap_register_callback();
    if (ret)
    {
        BLUFI_ERROR("%s gap register failed, error code = %x\n", __func__, ret);
        return ret;
    }

    return ESP_OK;
}

#endif /* CONFIG_BT_BLUEDROID_ENABLED */

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
/**
 * @brief 初始化蓝牙控制器
 *
 * @param void 无参数
 *
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 *
 * @note 初始化并使能蓝牙控制器（BLE 模式）
 */
esp_err_t esp_blufi_controller_init()
{
    esp_err_t ret = ESP_OK;
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE)
    {
        BLUFI_INFO("BT Controller already initialized\n");
        return ESP_OK;
    }

#if CONFIG_IDF_TARGET_ESP32
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
#endif

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret)
    {
        BLUFI_ERROR("%s initialize bt controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

#if CONFIG_IDF_TARGET_ESP32 && CONFIG_BT_BLUEDROID_ENABLED
    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
#else
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
#endif
    if (ret)
    {
        BLUFI_ERROR("%s enable bt controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }
    return ret;
}
#endif

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
/**
 * @brief 反初始化蓝牙控制器
 *
 * @param void 无参数
 *
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 *
 * @note 禁用并反初始化蓝牙控制器
 */
esp_err_t esp_blufi_controller_deinit()
{
    esp_err_t ret = ESP_OK;
    ret = esp_bt_controller_disable();
    if (ret)
    {
        BLUFI_ERROR("%s disable bt controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_deinit();
    if (ret)
    {
        BLUFI_ERROR("%s deinit bt controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    return ret;
}
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
void ble_store_config_init(void);
/**
 * @brief NimBLE 重置回调
 *
 * @param[in] reason 重置原因
 *
 * @return void 无返回值
 *
 * @note 记录重置日志
 */
static void blufi_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

/**
 * @brief NimBLE 同步回调
 *
 * @param void 无参数
 *
 * @return void 无返回值
 *
 * @note 初始化 BluFi Profile
 */
static void
blufi_on_sync(void)
{
    esp_blufi_profile_init();
}

/**
 * @brief NimBLE 主机任务
 *
 * @param[in] param 任务参数
 *
 * @return void 无返回值
 *
 * @note 运行 NimBLE 端口循环
 */
void bleprph_host_task(void *param)
{
    ESP_LOGI("BLUFI_EXAMPLE", "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

/**
 * @brief 初始化 NimBLE 主机
 *
 * @param void 无参数
 *
 * @return esp_err_t 成功返回 ESP_OK，失败返回 ESP_FAIL
 *
 * @note 初始化 NimBLE 栈，配置回调，启动主机任务
 */
esp_err_t esp_blufi_host_init(void)
{
    esp_err_t err;
    err = esp_nimble_init();
    if (err)
    {
        BLUFI_ERROR("%s failed: %s\n", __func__, esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* Initialize the NimBLE host configuration. */
    ble_hs_cfg.reset_cb = blufi_on_reset;
    ble_hs_cfg.sync_cb = blufi_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = 4;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
#endif
#ifdef CONFIG_EXAMPLE_MITM
    ble_hs_cfg.sm_mitm = 1;
#endif
#ifdef CONFIG_EXAMPLE_USE_SC
    ble_hs_cfg.sm_sc = 1;
#else
    ble_hs_cfg.sm_sc = 0;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_our_key_dist = 1;
    ble_hs_cfg.sm_their_key_dist = 1;
#endif
#endif

    int rc;
    rc = esp_blufi_gatt_svr_init();
    assert(rc == 0);

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set(BLUFI_DEVICE_NAME);
    assert(rc == 0);
#endif

    /* XXX Need to have template for store */
    ble_store_config_init();

    esp_blufi_btc_init();

    err = esp_nimble_enable(bleprph_host_task);
    if (err)
    {
        BLUFI_ERROR("%s failed: %s\n", __func__, esp_err_to_name(err));
        return ESP_FAIL;
    }

    nimble_running = true;

    return ESP_OK;
}

/**
 * @brief 反初始化 NimBLE 主机
 *
 * @param void 无参数
 *
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 *
 * @note 停止 NimBLE 端口，反初始化栈和 Profile
 */
esp_err_t esp_blufi_host_deinit(void)
{
    esp_err_t ret = ESP_OK;

    esp_blufi_gatt_svr_deinit();
    if (nimble_running)
    {
        ret = nimble_port_stop();
        if (ret != ESP_OK)
        {
            return ret;
        }
        if (ret == 0)
        {
            esp_nimble_deinit();
        }
        nimble_running = false;
    }

    ret = esp_blufi_profile_deinit();
    if (ret != ESP_OK)
    {
        return ret;
    }

    esp_blufi_btc_deinit();

    return ret;
}

/**
 * @brief 注册 GAP 回调（NimBLE）
 *
 * @param void 无参数
 *
 * @return esp_err_t 固定返回 ESP_OK
 *
 * @note NimBLE 模式下 GAP 回调注册在初始化时完成
 */
esp_err_t esp_blufi_gap_register_callback(void)
{
    return ESP_OK;
}

/**
 * @brief 初始化 BluFi 主机和回调（NimBLE）
 *
 * @param[in] example_callbacks BluFi 回调函数结构体指针
 *
 * @return esp_err_t 成功返回 ESP_OK，失败返回错误码
 *
 * @note 注册 BluFi 回调，注册 GAP 回调，初始化主机
 */
esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *example_callbacks)
{
    esp_err_t ret = ESP_OK;

    ret = esp_blufi_register_callbacks(example_callbacks);
    if (ret)
    {
        BLUFI_ERROR("%s blufi register failed, error code = %x\n", __func__, ret);
        return ret;
    }

    ret = esp_blufi_gap_register_callback();
    if (ret)
    {
        BLUFI_ERROR("%s gap register failed, error code = %x\n", __func__, ret);
        return ret;
    }

    ret = esp_blufi_host_init();
    if (ret)
    {
        BLUFI_ERROR("%s initialise host failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    return ret;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
