#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_blufi_api.h"
#include "esp_blufi.h"
#include "blufi_example.h"
#include "blufi_app.h"
#include "ai_framework/agent/audio_processor.h"
#include "esp_heap_caps.h"
// #ifdef __cplusplus
// extern "C" bool audio_init();
// extern "C" esp_err_t coze_chat_app_init(void);
// #endif

static const char *TAG = "BLUFI_APP";

static wifi_config_t sta_config;
static wifi_config_t ap_config;
static bool gl_sta_connected = false;
static bool gl_sta_got_ip = false;
static bool ble_is_connected = false;
static uint8_t gl_sta_bssid[6];
static uint8_t gl_sta_ssid[32];
static int gl_sta_ssid_len;
static wifi_sta_list_t gl_sta_list;
static bool gl_sta_is_connecting = false;
static EventGroupHandle_t wifi_event_group;
static const int CONNECTED_BIT = BIT0;
static bool blufi_started = false;
static bool bt_controller_inited = false;
static bool audio_paused_for_ble = false;

/**
 * @brief 确保 WiFi 已启动
 *
 * @param void 无参数
 *
 * @return bool 启动成功或已启动返回 true，否则返回 false
 *
 * @note 如果 WiFi 未启动，尝试启动它；如果处于非法状态则返回失败
 */
static bool ensure_wifi_started(void)
{
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK)
    {
        return true;
    }
    return (err == ESP_ERR_INVALID_STATE);
}

/**
 * @brief 记录 WiFi 连接信息回调（示例）
 *
 * @param[in] rssi 信号强度
 * @param[in] reason 连接失败原因
 *
 * @return void 无返回值
 */
static void example_record_wifi_conn_info(int rssi, uint8_t reason)
{
    (void)rssi;
    (void)reason;
}

/**
 * @brief 尝试连接 WiFi
 *
 * @param void 无参数
 *
 * @return void 无返回值
 *
 * @note 设置连接状态标志
 */
static void example_wifi_connect(void)
{
    gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
}

/**
 * @brief 获取 SoftAP 当前连接数
 *
 * @param void 无参数
 *
 * @return int 连接的 STA 数量
 */
static int softap_get_current_connection_number(void)
{
    if (esp_wifi_ap_get_sta_list(&gl_sta_list) == ESP_OK)
    {
        return gl_sta_list.num;
    }
    return 0;
}

/**
 * @brief IP 事件处理器
 *
 * @param[in] arg 用户参数
 * @param[in] event_base 事件基
 * @param[in] event_id 事件 ID
 * @param[in] event_data 事件数据
 *
 * @return void 无返回值
 *
 * @note 处理 STA 获取 IP 事件，向 BluFi 发送连接报告
 */
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;
    if (event_id == IP_EVENT_STA_GOT_IP)
    {
        wifi_mode_t mode;
        esp_blufi_extra_info_t info;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        esp_wifi_get_mode(&mode);
        memset(&info, 0, sizeof(info));
        memcpy(info.sta_bssid, gl_sta_bssid, 6);
        info.sta_bssid_set = true;
        info.sta_ssid = gl_sta_ssid;
        info.sta_ssid_len = gl_sta_ssid_len;
        gl_sta_got_ip = true;
        if (ble_is_connected)
        {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, softap_get_current_connection_number(), &info);
        }
    }
}

/**
 * @brief WiFi 事件处理器
 *
 * @param[in] arg 用户参数
 * @param[in] event_base 事件基
 * @param[in] event_id 事件 ID
 * @param[in] event_data 事件数据
 *
 * @return void 无返回值
 *
 * @note 处理 WiFi 启动、连接、断开等事件
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        example_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
    {
        gl_sta_connected = true;
        gl_sta_is_connecting = false;
        wifi_event_sta_connected_t *ev = (wifi_event_sta_connected_t *)event_data;
        memcpy(gl_sta_bssid, ev->bssid, 6);
        memcpy(gl_sta_ssid, ev->ssid, ev->ssid_len);
        gl_sta_ssid_len = ev->ssid_len;
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED:
        gl_sta_connected = false;
        gl_sta_got_ip = false;
        memset(gl_sta_ssid, 0, sizeof(gl_sta_ssid));
        memset(gl_sta_bssid, 0, sizeof(gl_sta_bssid));
        gl_sta_ssid_len = 0;
        if (wifi_event_group)
        {
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        }
        break;
    default:
        break;
    }
}

/**
 * @brief BluFi 事件回调函数
 *
 * @param[in] event BluFi 事件类型
 * @param[in] param BluFi 事件参数
 *
 * @return void 无返回值
 *
 * @note 处理 BluFi 协议的各种事件，如初始化完成、连接、配网信息接收等
 */
static void example_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event)
    {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        ble_is_connected = true;
        esp_blufi_adv_stop();
        blufi_security_init();
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        ble_is_connected = false;
        blufi_security_deinit();
        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        ESP_ERROR_CHECK(esp_wifi_set_mode(param->wifi_mode.op_mode));
        (void)ensure_wifi_started();
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        esp_wifi_disconnect();
        (void)ensure_wifi_started();
        example_wifi_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
    {
        wifi_mode_t mode;
        esp_blufi_extra_info_t info;
        esp_wifi_get_mode(&mode);
        if (gl_sta_connected)
        {
            memset(&info, 0, sizeof(info));
            memcpy(info.sta_bssid, gl_sta_bssid, 6);
            info.sta_bssid_set = true;
            info.sta_ssid = gl_sta_ssid;
            info.sta_ssid_len = gl_sta_ssid_len;
            esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, softap_get_current_connection_number(), &info);
        }
        else if (gl_sta_is_connecting)
        {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_get_current_connection_number(), NULL);
        }
        else
        {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_get_current_connection_number(), NULL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        sta_config.sta.bssid_set = 1;
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len < sizeof(sta_config.sta.ssid))
        {
            memcpy(sta_config.sta.ssid, param->sta_ssid.ssid, param->sta_ssid.ssid_len);
            sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
            esp_wifi_set_storage(WIFI_STORAGE_FLASH);
            esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        }
        else
        {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        }
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len < sizeof(sta_config.sta.password))
        {
            memcpy(sta_config.sta.password, param->sta_passwd.passwd, param->sta_passwd.passwd_len);
            sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
            esp_wifi_set_storage(WIFI_STORAGE_FLASH);
            esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        }
        else
        {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        }
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
        if (param->softap_ssid.ssid_len < sizeof(ap_config.ap.ssid))
        {
            memcpy(ap_config.ap.ssid, param->softap_ssid.ssid, param->softap_ssid.ssid_len);
            ap_config.ap.ssid[param->softap_ssid.ssid_len] = '\0';
            ap_config.ap.ssid_len = param->softap_ssid.ssid_len;
            esp_wifi_set_storage(WIFI_STORAGE_FLASH);
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        }
        else
        {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        }
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
        if (param->softap_passwd.passwd_len < sizeof(ap_config.ap.password))
        {
            memcpy(ap_config.ap.password, param->softap_passwd.passwd, param->softap_passwd.passwd_len);
            ap_config.ap.password[param->softap_passwd.passwd_len] = '\0';
            esp_wifi_set_storage(WIFI_STORAGE_FLASH);
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        }
        else
        {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        }
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
        if (param->softap_max_conn_num.max_conn_num <= 4)
        {
            ap_config.ap.max_connection = param->softap_max_conn_num.max_conn_num;
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        }
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
        if (param->softap_auth_mode.auth_mode < WIFI_AUTH_MAX)
        {
            ap_config.ap.authmode = param->softap_auth_mode.auth_mode;
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        }
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
        if (param->softap_channel.channel <= 13)
        {
            ap_config.ap.channel = param->softap_channel.channel;
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        }
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
    {
        wifi_scan_config_t scanConf = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time = {.active = {.min = 100, .max = 300}},
        };
        if (esp_wifi_scan_start(&scanConf, true) != ESP_OK)
        {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        ESP_LOG_BUFFER_HEX(TAG, param->custom_data.data, param->custom_data.data_len);
        break;
    default:
        break;
    }
}

static esp_blufi_callbacks_t example_callbacks = {
    .event_cb = example_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

/**
 * @brief 启动 BluFi 应用程序
 *
 * @param void 无参数
 *
 * @return void 无返回值
 *
 * @note 初始化 WiFi、事件循环、蓝牙控制器和 Host，并注册 BluFi 回调
 */
void blufi_app_start(void)
{
    if (blufi_started)
    {
        return;
    }
    wifi_event_group = xEventGroupCreate();
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "event loop create failed: %d", err);
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t external_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "SRAM free %uB (largest %uB), PSRAM free %uB (largest %uB)",
             (unsigned)internal_free, (unsigned)internal_largest,
             (unsigned)external_free, (unsigned)external_largest);
    if (internal_largest < 36000)
    {
        ESP_LOGW(TAG, "Largest SRAM block < 36KB; try closing audio tasks");
        audio_prompt_close();
        audio_playback_close();
        audio_recorder_close();
        audio_manager_suspend(true);
        audio_paused_for_ble = true;
        internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        external_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "After audio close: SRAM free %uB (largest %uB), PSRAM free %uB (largest %uB)",
                 (unsigned)internal_free, (unsigned)internal_largest,
                 (unsigned)external_free, (unsigned)external_largest);
    }
    if (!bt_controller_inited)
    {
        ESP_ERROR_CHECK(esp_blufi_controller_init());
        bt_controller_inited = true;
    }
#endif
    ESP_ERROR_CHECK(esp_blufi_host_and_cb_init(&example_callbacks));
    blufi_started = true;
}

/**
 * @brief 停止 BluFi 应用程序
 *
 * @param void 无参数
 *
 * @return void 无返回值
 *
 * @note 停止并释放 BluFi 相关的资源，包括蓝牙协议栈、事件处理器等
 */
void blufi_app_stop(void)
{
    // 入口保护：未启动则直接返回，避免重复释放
    if (!blufi_started)
    {
        return;
    }
    // 若当前仍存在 BLE 连接，先主动断开，确保后续资源可安全释放
    if (ble_is_connected)
    {
        esp_blufi_disconnect();
        ble_is_connected = false;
    }

    // 反注册 WiFi/IP 事件处理器，停止接收事件回调
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler);

    // 关闭 BLUFI Host
    (void)esp_blufi_host_deinit();
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    // 若蓝牙控制器已初始化，执行反初始化以释放 SRAM
    if (bt_controller_inited)
    {
        esp_blufi_controller_deinit();
        bt_controller_inited = false;
    }
#endif

    // 删除事件组资源
    if (wifi_event_group)
    {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }

    // 标记 BluFi 已停止
    blufi_started = false;

    /*
    if (audio_paused_for_ble) {
        ESP_LOGI(TAG, "Resuming audio...");
        audio_manager_suspend(false);
        // audio_init(); // 如果需要重新初始化硬件
        // coze_chat_app_init(); // 如果需要重新初始化业务
        audio_paused_for_ble = false;
    }
    */
}
