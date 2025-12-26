/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mutex>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_gmf_element.h"
#include "esp_gmf_oal_sys.h"
#include "esp_gmf_oal_thread.h"
#include "esp_gmf_oal_mem.h"
#include "esp_coze_chat.h"
#include "esp_coze_utils.h"
#include "http_client_request.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "boost/thread.hpp"
#include "private/esp_brookesia_ai_agent_utils.hpp"
#include "audio_processor.h"
#include "function_calling.hpp"
#include "coze_chat_app.hpp"

/*
 * 说话超时与静音延时相关配置：
 * - SPEAKING_TIMEOUT_MS：说话状态的最大持续时间，超时后自动退出说话态（毫秒）
 * - SPEAKING_MUTE_DELAY_MS：聊天完成后延迟一段时间再解除静音，避免尾音（毫秒）
 */
#define SPEAKING_TIMEOUT_MS (2000)
#define SPEAKING_MUTE_DELAY_MS (2000)

/*
 * 音频上行读取块大小：每次从录音环形缓冲区读取的字节数，建议保持与编码块对齐
 */
#define AUDIO_RECORDER_READ_SIZE (1024)

/*
 * 聊天中断参数：
 * - COZE_INTERRUPT_TIMES：发送取消上行音频的次数，用于尽快打断云端识别
 * - COZE_INTERRUPT_INTERVAL_MS：连续取消之间的间隔（毫秒）
 */
#define COZE_INTERRUPT_TIMES (20)
#define COZE_INTERRUPT_INTERVAL_MS (100)

using namespace esp_brookesia::ai_framework;

/*
 * Coze 聊天应用的运行时上下文：
 * - chat：Coze 聊天句柄
 * - chat_mutex：保护聊天会话的递归互斥锁，避免多线程并发操作会话
 * - chat_start：聊天是否已启动（完成初始化与开始）
 * - chat_pause：聊天是否处于暂停（暂停期间不向云端发送音频）
 * - chat_sleep：聊天是否处于休眠（休眠期间不处理唤醒与上行）
 * - speaking：设备是否处于说话态（用于控制本地播放与静音钳制）
 * - wakeup：是否处于“唤醒态”（语音识别窗口，允许向云端发送音频）
 * - wakeup_start：是否刚触发唤醒开始（用于抑制立即发送音频）
 * - websocket_connected：WebSocket 是否连接成功
 * - speaking_timeout_timer：说话态超时定时器，防止长时间占用音频链路
 * - read_thread：音频上行读取线程，将编码后的数据送云端
 * - btn_thread/btn_evt_q：按键线程与事件队列（保留扩展输入）
 */
struct coze_chat_t
{
    esp_coze_chat_handle_t chat;               // 聊天会话句柄
    std::recursive_mutex chat_mutex;           // 递归互斥锁，保护 chat 的并发访问
    bool chat_start;                           // 聊天是否已启动
    bool chat_pause;                           // 聊天是否暂停（不发送音频）
    bool chat_sleep;                           // 聊天是否休眠（不处理唤醒）
    bool speaking;                             // 是否处于说话态
    bool wakeup;                               // 是否处于唤醒态
    bool wakeup_start;                         // 是否刚进入唤醒开始
    bool websocket_connected;                  // WebSocket 是否连接
    esp_timer_handle_t speaking_timeout_timer; // 说话态超时定时器
    esp_gmf_oal_thread_t read_thread;          // 音频上行读取线程句柄
    esp_gmf_oal_thread_t btn_thread;           // 按键线程句柄（预留）
    QueueHandle_t btn_evt_q;                   // 按键事件队列（预留）
};

// 全局 Coze 聊天上下文实例，保存会话与状态
static struct coze_chat_t coze_chat = {};
// 获取访问令牌（access_token）的授权 URL，遵循 Coze 平台接口
static const char *coze_authorization_url = "https://api.coze.cn/api/permission/oauth2/token";

// 信号通道：用于向 UI/上层模块广播状态变化与事件
boost::signals2::signal<void(const std::string &emoji)> coze_chat_emoji_signal; // 发送表情事件
boost::signals2::signal<void(bool is_speaking)> coze_chat_speaking_signal;      // 说话态变化事件
boost::signals2::signal<void(void)> coze_chat_response_signal;                  // 触发 AI 响应（提示音等）
boost::signals2::signal<void(bool is_wake_up)> coze_chat_wake_up_signal;        // 唤醒态变化事件
boost::signals2::signal<void(void)> coze_chat_websocket_disconnected_signal;    // WebSocket 断开通知
boost::signals2::signal<void(int code)> coze_chat_error_signal;                 // 错误码通知

/*
 * 功能：打印 CozeChatAgentInfo 的关键字段，便于调试与审计
 * 参数：无（隐式使用 this 指针）
 * 返回：无
 * 使用示例：
 *   CozeChatAgentInfo info; info.dump();
 */
void CozeChatAgentInfo::dump() const
{
    ESP_UTILS_LOGI(
        "\n{ChatInfo}:\n"
        "\t-session_name: %s\n"
        "\t-device_id: %s\n"
        "\t-app_id: %s\n"
        "\t-user_id: %s\n"
        "\t-public_key: %s\n"
        "\t-private_key: %s\n"
        "\t-custom_consumer: %s\n",
        session_name.c_str(), device_id.c_str(), app_id.c_str(), user_id.c_str(), public_key.c_str(),
        private_key.c_str(), custom_consumer.c_str());
}

/*
 * 功能：校验 Agent 信息是否完整有效
 * 参数：无
 * 返回：true 表示有效；false 表示信息不完整
 * 使用示例：
 *   if (!info.isValid()) {  处理错误  }
 */
bool CozeChatAgentInfo::isValid() const
{
    return !session_name.empty() && !device_id.empty() && !user_id.empty() && !app_id.empty() &&
           !public_key.empty() && !private_key.empty();
}

/*
 * 功能：打印机器人信息，便于调试与日志跟踪
 * 参数：无（隐式使用 this 指针）
 * 返回：无
 */
void CozeChatRobotInfo::dump() const
{
    ESP_UTILS_LOGI(
        "\n{RobotInfo}:\n"
        "\t-name: %s\n"
        "\t-bot_id: %s\n"
        "\t-voice_id: %s\n"
        "\t-description: %s\n",
        name.c_str(), bot_id.c_str(), voice_id.c_str(), description.c_str());
}

/*
 * 功能：校验机器人信息是否完整有效
 * 参数：无
 * 返回：true 表示有效；false 表示不完整
 */
bool CozeChatRobotInfo::isValid() const
{
    return !name.empty() && !bot_id.empty() && !voice_id.empty() && !description.empty();
}

/*
 * 功能：切换设备的“说话态”，并管理唤醒保持与静音定时器
 * 参数：
 *  - is_speaking：目标说话态（true 进入说话，false 退出）
 *  - force：是否强制切换（忽略当前状态相同的早退）
 * 返回：无
 * 使用示例：
 *   change_speaking_state(true);  // 进入说话态，保持唤醒并启动定时器
 */
static void change_speaking_state(bool is_speaking, bool force = false)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    esp_err_t ret = ESP_OK;
    // std::unique_lock<std::recursive_mutex> lock(coze_chat.chat_mutex);

    // 若目标状态与当前一致且非强制，直接返回，避免重复操作
    if ((is_speaking == coze_chat.speaking) && !force)
    {
        if (is_speaking)
        {
            // 说话态下重启超时定时器，延长说话窗口
            ret = esp_timer_restart(coze_chat.speaking_timeout_timer, SPEAKING_TIMEOUT_MS * 1000);
            if (ret != ESP_OK)
            {
                ESP_UTILS_LOGE("Restart speaking timeout timer failed(%s)", esp_err_to_name(ret));
            }
        }
        return;
    }

    ESP_UTILS_LOGI("change_speaking_state: %d, force: %d", is_speaking, force);

    if (is_speaking)
    {
        // 进入说话态：保持 AFE 唤醒，启动一次性超时定时器
        if (esp_gmf_afe_keep_awake(audio_processor_get_afe_handle(), true) != ESP_OK)
        {
            ESP_UTILS_LOGE("Keep awake failed");
        }
        if (!esp_timer_is_active(coze_chat.speaking_timeout_timer))
        {
            ret = esp_timer_start_once(coze_chat.speaking_timeout_timer, SPEAKING_TIMEOUT_MS * 1000);
            if (ret != ESP_OK)
            {
                ESP_UTILS_LOGE("Start speaking timeout timer failed(%s)", esp_err_to_name(ret));
            }
        }
    }
    else
    {
        // 退出说话态：取消 AFE 唤醒保持，停止定时器
        if (esp_gmf_afe_keep_awake(audio_processor_get_afe_handle(), false) != ESP_OK)
        {
            ESP_UTILS_LOGE("Keep awake failed");
        }
        if (esp_timer_is_active(coze_chat.speaking_timeout_timer))
        {
            ret = esp_timer_stop(coze_chat.speaking_timeout_timer);
            if (ret != ESP_OK)
            {
                ESP_UTILS_LOGE("Stop speaking timeout timer failed(%s)", esp_err_to_name(ret));
            }
        }
    }
    coze_chat.speaking = is_speaking;
    // lock.unlock();

    coze_chat_speaking_signal(is_speaking);
}

/*
 * 功能：切换“唤醒态”，用于控制是否允许向云端发送音频
 * 参数：
 *  - is_wakeup：目标唤醒态（true 进入唤醒，false 退出）
 *  - force：是否强制切换
 * 返回：无
 */
static void change_wakeup_state(bool is_wakeup, bool force = false)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    // std::unique_lock<std::recursive_mutex> lock(coze_chat.chat_mutex);

    if ((is_wakeup == coze_chat.wakeup) && !force)
    {
        return;
    }

    ESP_UTILS_LOGI("change_wakeup_state: %d, force: %d", is_wakeup, force);

    coze_chat.wakeup = is_wakeup;
    // lock.unlock();

    coze_chat_wake_up_signal(is_wakeup);
}

/*
 * 功能：解析云端返回的错误 JSON，提取业务错误码
 * 参数：
 *  - data：JSON 字符串（包含 data.code 字段）
 * 返回：>=0 的错误码；-1 表示解析失败或格式异常
 * 使用示例：
 *   int code = parse_chat_error_code(json_str);
 */
static int parse_chat_error_code(const char *data)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    ESP_UTILS_CHECK_NULL_RETURN(data, -1, "Invalid data");

    // 解析根 JSON，失败直接返回错误
    cJSON *json_root = cJSON_Parse(data);
    ESP_UTILS_CHECK_NULL_RETURN(json_root, -1, "Failed to parse JSON data");

    esp_utils::function_guard delete_guard([&]()
                                           { cJSON_Delete(json_root); });

    // 进入 data 对象，读取业务字段
    cJSON *data_obj = cJSON_GetObjectItem(json_root, "data");
    ESP_UTILS_CHECK_NULL_RETURN(data_obj, -1, "No data found in JSON data");
    ESP_UTILS_CHECK_FALSE_RETURN(cJSON_IsObject(data_obj), -1, "data is not an object");

    // 提取 code 数值
    cJSON *code_item = cJSON_GetObjectItem(data_obj, "code");
    ESP_UTILS_CHECK_NULL_RETURN(code_item, -1, "No code found in JSON data");
    ESP_UTILS_CHECK_FALSE_RETURN(cJSON_IsNumber(code_item), -1, "code is not a number");

    return static_cast<int>(cJSON_GetNumberValue(code_item));
}

/*
 * 功能：处理 Coze 聊天事件（错误、开始/停止说话、完成、字幕、工具调用等）
 * 参数：
 *  - event：事件类型（见 esp_coze_chat_event_t 枚举）
 *  - data：事件携带的字符串数据（JSON 或字幕等）
 *  - ctx：用户上下文（未用）
 * 返回：无
 * 说明：根据不同事件更新本地状态并通知上层模块
 */
static void audio_event_callback(esp_coze_chat_event_t event, char *data, void *ctx)
{
    if (event == ESP_COZE_CHAT_EVENT_CHAT_ERROR)
    {
        ESP_UTILS_LOGE("chat error: %s", data);

        int code = parse_chat_error_code(data);
        ESP_UTILS_CHECK_FALSE_EXIT(code != -1, "Failed to parse chat error code");

        coze_chat_error_signal(code);
    }
    else if (event == ESP_COZE_CHAT_EVENT_CHAT_SPEECH_STARTED)
    {
        ESP_UTILS_LOGI("chat start");
        coze_chat.wakeup_start = false;
    }
    else if (event == ESP_COZE_CHAT_EVENT_CHAT_SPEECH_STOPED)
    {
        ESP_UTILS_LOGI("chat stop");
        // change_speaking_state(true);
    }
    else if (event == ESP_COZE_CHAT_EVENT_CHAT_COMPLETED)
    {
        boost::thread([&]()
                      {
            boost::this_thread::sleep_for(boost::chrono::milliseconds(SPEAKING_MUTE_DELAY_MS));
            change_speaking_state(false); })
            .detach();
        ESP_UTILS_LOGI("chat complete");
    }
    else if (event == ESP_COZE_CHAT_EVENT_CHAT_CUSTOMER_DATA)
    {
        // cjson format data
        ESP_UTILS_LOGI("Customer data: %s", data);

        // call func
        cJSON *json_data = cJSON_Parse(data);
        if (json_data == NULL)
        {
            ESP_UTILS_LOGE("Failed to parse JSON data");
            return;
        }
        // debug
        cJSON *json_item = NULL;
        cJSON_ArrayForEach(json_item, json_data)
        {
            char *key = json_item->string;
            char *value = cJSON_Print(json_item);
            if (key && value)
            {
                ESP_UTILS_LOGI("Key: %s, Value: %s", key, value);
                cJSON_free(value);
            }
        }
        //

        cJSON *data_json = cJSON_GetObjectItem(json_data, "data");
        if (data_json == NULL)
        {
            ESP_UTILS_LOGE("No data found in JSON data");
            cJSON_Delete(json_data);
            return;
        }

        cJSON *required_action = cJSON_GetObjectItem(data_json, "required_action");
        if (required_action == NULL)
        {
            ESP_UTILS_LOGE("No required_action found in JSON data");
            cJSON_Delete(json_data);
            return;
        }

        cJSON *submit_tool_outputs = cJSON_GetObjectItem(required_action, "submit_tool_outputs");
        if (submit_tool_outputs == NULL)
        {
            ESP_UTILS_LOGE("No submit_tool_outputs found in JSON data");
            cJSON_Delete(json_data);
            return;
        }

        cJSON *tool_calls = cJSON_GetObjectItem(submit_tool_outputs, "tool_calls");
        if (tool_calls == NULL || !cJSON_IsArray(tool_calls))
        {
            ESP_UTILS_LOGE("No tool_calls found or tool_calls is not an array");
            cJSON_Delete(json_data);
            return;
        }

        cJSON *first_tool_call = cJSON_GetArrayItem(tool_calls, 0);
        if (first_tool_call == NULL)
        {
            ESP_UTILS_LOGE("No first tool call found in tool_calls");
            cJSON_Delete(json_data);
            return;
        }

        char *function_str = cJSON_Print(first_tool_call);
        if (function_str)
        {
            ESP_UTILS_LOGI("Function JSON: %s", function_str);
            free(function_str);
        }
        else
        {
            ESP_UTILS_LOGE("Failed to print function JSON");
        }

        FunctionDefinitionList::requestInstance().invokeFunction(first_tool_call);

        cJSON_Delete(json_data);
    }
    else if (event == ESP_COZE_CHAT_EVENT_CHAT_SUBTITLE_EVENT)
    {
        // 处理字幕事件：当字幕包裹在中文括号内且格式为 :emoji: 时，提取并广播
        if (strncmp(data, "（", 3) == 0 && strncmp(data + strlen(data) - 3, "）", 3) == 0)
        {
            std::string emoji_str(data + 3);
            emoji_str = emoji_str.substr(0, emoji_str.length() - 3);
            if (emoji_str.front() == ':' && emoji_str.back() == ':')
            {
                emoji_str = emoji_str.substr(1, emoji_str.length() - 2);
                ESP_UTILS_LOGI("Emoji: %s\n", emoji_str.c_str());
                coze_chat_emoji_signal(emoji_str);
            }
        }
    }
}

/*
 * 功能：处理 WebSocket 连接事件，维护连接状态并在断开时通知上层
 * 参数：
 *  - event：WebSocket 事件结构体
 * 返回：无
 */
static void websocket_event_callback(esp_coze_ws_event_t *event)
{
    switch (event->event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_UTILS_LOGI("Websocket connected");
        coze_chat.websocket_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        ESP_UTILS_LOGE("Websocke_signalt disconnected or error");
        coze_chat.websocket_connected = false;
        coze_chat_websocket_disconnected_signal();
        break;
    default:
        break;
    }
}

/*
 * 功能：云端下行语音数据回调（TTS 播放），根据状态决定是否播放并维持说话态
 * 参数：
 *  - data/len：下行音频数据及长度
 *  - ctx：未用
 * 返回：无
 */
static void audio_data_callback(char *data, int len, void *ctx)
{
    ESP_UTILS_LOGD("audio_data_callback");
    // 聊天未暂停/未休眠且处于说话态时，将数据送入本地播放链路
    if (!coze_chat.chat_pause && !coze_chat.chat_sleep && coze_chat.speaking)
    {
        audio_playback_feed_data((uint8_t *)data, len);
    }
    // 在非“刚唤醒”阶段，且未暂停/未休眠时，维持说话态以保持唤醒窗口
    if (!coze_chat.wakeup_start && !coze_chat.chat_pause && !coze_chat.chat_sleep)
    {
        change_speaking_state(true);
    }
}

/*
 * 功能：生成指定长度的随机字符串（用于 JWT jti 等）
 * 参数：
 *  - output：输出缓冲区（长度为 length + 1）
 *  - length：生成的随机字符个数
 * 返回：无（通过 output 返回结果）
 */
void generate_random_string(char *output, size_t length)
{
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    size_t charset_size = sizeof(charset) - 1;
    for (size_t i = 0; i < length; i++)
    {
        int key = esp_random() % charset_size;
        output[i] = charset[key];
    }
    output[length] = '\0';
}

/*
 * 功能：基于 Agent 信息构建 JWT 并向授权服务器申请 access_token
 * 参数：
 *  - agent_info：代理信息（包含 app_id、公私钥、设备信息等）
 * 返回：分配的 access_token 字符串（由调用者负责 free）；NULL 表示失败
 * 使用示例：
 *   char *token = coze_get_access_token(info);
 */
static char *coze_get_access_token(const CozeChatAgentInfo &agent_info)
{
    // 构建 JWT payload：包含签发方、受众、签发/过期时间、随机 jti、会话上下文等
    cJSON *payload_json = cJSON_CreateObject();
    if (!payload_json)
    {
        ESP_UTILS_LOGE("Failed to create payload_json");
        return NULL;
    }
    char random_str[33] = {0};
    generate_random_string(random_str, 32);
    time_t now = time(NULL);
    cJSON_AddStringToObject(payload_json, "iss", agent_info.app_id.c_str());
    cJSON_AddStringToObject(payload_json, "aud", "api.coze.cn");
    cJSON_AddNumberToObject(payload_json, "iat", now);
    cJSON_AddNumberToObject(payload_json, "exp", now + 6000);
    cJSON_AddStringToObject(payload_json, "jti", random_str);
    cJSON_AddStringToObject(payload_json, "session_name", agent_info.session_name.c_str());
    cJSON *session_context_json = cJSON_CreateObject();
    cJSON *device_info_json = cJSON_CreateObject();
    cJSON_AddStringToObject(device_info_json, "device_id", agent_info.device_id.c_str());
    cJSON_AddStringToObject(device_info_json, "custom_consumer", agent_info.custom_consumer.c_str());
    cJSON_AddItemToObject(session_context_json, "device_info", device_info_json);
    cJSON_AddItemToObject(payload_json, "session_context", session_context_json);

    char *payload_str = cJSON_PrintUnformatted(payload_json);
    if (!payload_str)
    {
        ESP_UTILS_LOGE("Failed to print payload_json");
        cJSON_Delete(payload_json);
        return NULL;
    }
    ESP_UTILS_LOGD("payload_str: %s\n", payload_str);
    char *formatted_payload_str = cJSON_Print(payload_json);
    if (formatted_payload_str)
    {
        ESP_UTILS_LOGD("formatted_payload_str: %s\n", formatted_payload_str);
        free(formatted_payload_str);
    }

    // 使用私钥签发 JWT，得到授权凭据
    char *jwt = coze_jwt_create_handler(
        agent_info.public_key.c_str(), payload_str, (const uint8_t *)agent_info.private_key.c_str(),
        strlen(agent_info.private_key.c_str()));
    cJSON_Delete(payload_json);
    free(payload_str);

    if (!jwt)
    {
        ESP_UTILS_LOGE("Failed to create JWT");
        // payload_json and payload_str already freed above
        return NULL;
    }

    // 组装 Authorization 头（Bearer 模式）
    char *authorization = (char *)calloc(1, strlen(jwt) + 16);
    if (!authorization)
    {
        ESP_UTILS_LOGE("Failed to allocate authorization");
        free(jwt);
        return NULL;
    }
    sprintf(authorization, "Bearer %s", jwt);
    ESP_UTILS_LOGD("Authorization: %s", authorization);

    cJSON *http_req_json = cJSON_CreateObject();
    if (!http_req_json)
    {
        ESP_UTILS_LOGE("Failed to create http_req_json");
        free(jwt);
        free(authorization);
        return NULL;
    }
    cJSON_AddNumberToObject(http_req_json, "duration_seconds", 86399);
    cJSON_AddStringToObject(http_req_json, "grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer");
    char *http_req_json_str = cJSON_PrintUnformatted(http_req_json);
    if (!http_req_json_str)
    {
        ESP_UTILS_LOGE("Failed to print http_req_json");
        free(jwt);
        free(authorization);
        cJSON_Delete(http_req_json);
        return NULL;
    }

    // 构造 HTTP 请求头
    http_req_header_t header[] = {
        {"Content-Type", "application/json"},
        {"Authorization", authorization},
        {NULL, NULL}};

    http_response_t response = {0};
    // 请求授权服务，获取访问令牌
    esp_err_t ret = http_client_post(coze_authorization_url, header, http_req_json_str, &response);
    if (ret != ESP_OK)
    {
        ESP_UTILS_LOGE("HTTP POST failed");
        return NULL;
    }

    char *access_token = NULL;
    if (response.body)
    {
        // 解析响应 JSON，提取 access_token、expires_in、token_type 等
        ESP_UTILS_LOGD("response: %s\n", response.body);

        cJSON *root = cJSON_Parse(response.body);
        if (root)
        {
            cJSON *access_token_item = cJSON_GetObjectItem(root, "access_token");
            if (cJSON_IsString(access_token_item) && access_token_item->valuestring != NULL)
            {
                ESP_UTILS_LOGD("access_token: %s\n", access_token_item->valuestring);
                access_token = strdup(access_token_item->valuestring);
            }
            else
            {
                ESP_UTILS_LOGE("access_token is invalid or not exist");
            }

            cJSON *expires_in_item = cJSON_GetObjectItem(root, "expires_in");
            if (cJSON_IsNumber(expires_in_item))
            {
                ESP_UTILS_LOGD("expires_in: %d\n", expires_in_item->valueint);
            }

            cJSON *token_type_item = cJSON_GetObjectItem(root, "token_type");
            if (cJSON_IsString(token_type_item))
            {
                ESP_UTILS_LOGD("token_type: %s\n", token_type_item->valuestring);
            }

            cJSON_Delete(root);
        }
        else
        {
            ESP_UTILS_LOGE("Failed to parse JSON response");
        }
    }

    // 释放中间内存，返回 access_token
    free(jwt);
    free(authorization);
    cJSON_Delete(http_req_json);
    free(http_req_json_str);
    if (response.body)
    {
        free(response.body);
    }

    return access_token;
}

/*
 * 功能：录音端 AFE 事件回调（唤醒/VAD/命令等），用于驱动聊天与本地行为
 * 参数：
 *  - event：AFE 事件（类型与载荷）
 *  - ctx：用户上下文（未用）
 * 返回：无
 * 使用说明：
 *  - 唤醒开始：中断云端上行、进入唤醒态，抑制立即发音；触发响应信号
 *  - 唤醒结束：退出唤醒态，恢复正常状态
 *  - VAD：打印语音段起止日志
 *  - 命令事件（默认分支）：保留扩展点用于联动本地命令
 */
static void recorder_event_callback_fn(void *event, void *ctx)
{
    // 若聊天未启动或处于暂停态，此处直接返回以避免不必要处理
    if (!coze_chat.chat_start)
    {
        ESP_UTILS_LOGD("chat is not started, skip AFE event");
        return;
    }

    esp_gmf_afe_evt_t *afe_evt = (esp_gmf_afe_evt_t *)event;
    switch (afe_evt->type)
    {
    case ESP_GMF_AFE_EVT_WAKEUP_START:
    {
        ESP_UTILS_LOGI("wakeup start");
        // 若云端连接正常且未休眠，则中断当前音频上行，避免与本地唤醒冲突
        if (coze_chat.websocket_connected && !coze_chat.chat_sleep)
        {
            coze_chat_app_interrupt();
        }
        change_speaking_state(false); // 进入唤醒时立刻退出说话态
        change_wakeup_state(true);
        coze_chat.wakeup_start = true;
        coze_chat_response_signal();
        break;
    }
    case ESP_GMF_AFE_EVT_WAKEUP_END:
        ESP_UTILS_LOGI("wakeup end");
        change_speaking_state(false);
        change_wakeup_state(false);
        coze_chat_app_resume();
        break;
    case ESP_GMF_AFE_EVT_VAD_START:
        ESP_UTILS_LOGI("vad start");
        break;
    case ESP_GMF_AFE_EVT_VAD_END:
        ESP_UTILS_LOGI("vad end");
        break;
    case ESP_GMF_AFE_EVT_VCMD_DECT_TIMEOUT:
        ESP_UTILS_LOGI("vcmd detect timeout");
        break;
    default:
    {
        // 命令事件扩展点：可在此解析 info->str 或 phrase_id 并执行本地动作
        // esp_gmf_afe_vcmd_info_t *info = event->event_data;
        // ESP_UTILS_LOGW("Command %d, phrase_id %d, prob %f, str: %s", afe_evt->type, info->phrase_id, info->prob, info->str);
        coze_chat_app_pause();
        break;
    }
    }
}

/*
 * 功能：读取编码后的上行音频数据，并在允许条件下发送至云端
 * 参数：
 *  - pv：线程入口参数（传入 coze_chat 的地址）
 * 返回：无（任务循环）
 */
static void audio_data_read_task(void *pv)
{
    coze_chat_t *coze_chat = (coze_chat_t *)pv;

    uint8_t *data = (uint8_t *)esp_gmf_oal_calloc(1, AUDIO_RECORDER_READ_SIZE);
    int ret = 0;
    size_t base64_len = 0;
    unsigned char *base64_data = NULL;
    size_t output_len = 0;

    while (true)
    {
        ret = audio_recorder_read_data(data, AUDIO_RECORDER_READ_SIZE);
        // 仅在聊天启动、处于唤醒态、未暂停/未休眠、且当前非说话态时上传音频
        if (coze_chat->chat_start && coze_chat->wakeup && !coze_chat->chat_pause && !coze_chat->chat_sleep && !coze_chat->speaking)
        {
            // Calculate required buffer size for Base64 (4 * (n + 2) / 3) + 1 for null terminator
            base64_len = ((ret + 2) / 3) * 4 + 1;
            base64_data = (unsigned char *)esp_gmf_oal_calloc(1, base64_len);

            if (base64_data)
            {
                if (mbedtls_base64_encode(base64_data, base64_len, &output_len, data, ret) == 0)
                {
                    esp_coze_chat_send_audio_data(coze_chat->chat, (char *)base64_data, output_len);
                }
                else
                {
                    ESP_UTILS_LOGE("Base64 encode failed");
                }
                esp_gmf_oal_free(base64_data);
            }
            else
            {
                ESP_UTILS_LOGE("Failed to allocate memory for base64 data");
            }
        }
        // heap_caps_check_integrity_all(true);
    }
}

/*
 * 功能：打开音频管线（录音与播放），并启用 AFE 事件回调
 * 参数：无
 * 返回：无
 */
static void audio_pipe_open(void)
{
    vTaskDelay(pdMS_TO_TICKS(800)); // 延迟以错峰初始化，避免资源竞争
    audio_recorder_open(recorder_event_callback_fn, NULL);
    audio_playback_open();
    audio_playback_run();
}

// static void audio_pipe_close(void)
// {
//     audio_playback_stop();
//     audio_playback_close();
//     audio_recorder_close();
// }

/*
 * 功能：初始化 Coze 聊天应用（创建定时器、打开音频管线、启动读取线程）
 * 参数：无
 * 返回：ESP_OK 表示成功；其它错误码表示失败
 * 使用示例：
 *   ESP_ERROR_CHECK(coze_chat_app_init());
 */
extern "C" esp_err_t coze_chat_app_init(void)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    // 创建“说话态超时”定时器：到期后自动退出说话态
    esp_timer_create_args_t timer_args = {
        .callback = [](void *arg)
        {
            ESP_UTILS_LOGI("speaking timeout start");
            boost::thread([&]()
                          { change_speaking_state(false); })
                .detach();
            ESP_UTILS_LOGI("speaking timeout end");
        },
        .arg = &coze_chat,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "speaking_timeout",
        .skip_unhandled_events = false};
    esp_timer_create(&timer_args, &coze_chat.speaking_timeout_timer);

    audio_pipe_open();

    // 启动音频读取线程：持续从录音器读取并在允许条件下上传
    esp_gmf_oal_thread_create(
        &coze_chat.read_thread, "audio_data_read", audio_data_read_task, (void *)&coze_chat, 3096, 12, true, 1);

    return ESP_OK;
}

/*
 * 功能：启动 Coze 聊天（获取 token、初始化会话、设置参数并开始）
 * 参数：
 *  - agent_info：代理身份信息
 *  - robot_info：机器人配置（bot/voice）
 * 返回：ESP_OK 表示启动成功；ESP_FAIL 表示失败
 */
extern "C" esp_err_t coze_chat_app_start(const CozeChatAgentInfo &agent_info, const CozeChatRobotInfo &robot_info)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    char *token_str = NULL;
    token_str = coze_get_access_token(agent_info);
    if (token_str == NULL)
    {
        ESP_UTILS_LOGE("Failed to get access token");
        return ESP_FAIL;
    }

    esp_coze_chat_config_t chat_config = ESP_COZE_CHAT_DEFAULT_CONFIG();
    chat_config.enable_subtitle = true;
    chat_config.subscribe_event = (const char *[]){
        "conversation.chat.requires_action", NULL};
    chat_config.user_id = const_cast<char *>(agent_info.user_id.c_str());
    chat_config.bot_id = const_cast<char *>(robot_info.bot_id.c_str());
    chat_config.voice_id = const_cast<char *>(robot_info.voice_id.c_str());
    chat_config.access_token = token_str;
    chat_config.uplink_audio_type = ESP_COZE_CHAT_AUDIO_TYPE_G711A;
    chat_config.audio_callback = audio_data_callback;
    chat_config.event_callback = audio_event_callback;
    chat_config.ws_event_callback = websocket_event_callback;
    // chat_config.websocket_buffer_size = 4096;
    // chat_config.mode = ESP_COZE_CHAT_NORMAL_MODE;

    // 初始化并启动聊天，期间使用互斥锁保护会话对象
    std::lock_guard lock(coze_chat.chat_mutex);
    esp_err_t ret = esp_coze_chat_init(&chat_config, &coze_chat.chat);
    ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, ret, "esp_coze_chat_init failed(%s)", esp_err_to_name(ret));

    static auto func_call = FunctionDefinitionList::requestInstance().getJson();

    esp_coze_parameters_kv_t param[] = {
        {"func_call", const_cast<char *>(func_call.c_str())},
        {NULL, NULL}};
    ret = esp_coze_set_chat_config_parameters(coze_chat.chat, param);
    ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, ret, "esp_coze_set_chat_config_parameters failed(%s)", esp_err_to_name(ret));
    ret = esp_coze_chat_start(coze_chat.chat);
    ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, ret, "esp_coze_chat_start failed(%s)", esp_err_to_name(ret));

    coze_chat.chat_start = true;

    free(token_str);

    return ESP_OK;
}

/*
 * 功能：停止并销毁 Coze 聊天会话，释放资源
 * 参数：无
 * 返回：ESP_OK 表示成功；其它错误码表示失败
 */
extern "C" esp_err_t coze_chat_app_stop(void)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    std::lock_guard lock(coze_chat.chat_mutex);

    esp_err_t ret = esp_coze_chat_stop(coze_chat.chat);
    ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, ret, "esp_coze_chat_stop failed(%s)", esp_err_to_name(ret));

    ret = esp_coze_chat_deinit(coze_chat.chat);
    ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, ret, "esp_coze_chat_deinit failed(%s)", esp_err_to_name(ret));
    coze_chat.chat = NULL;

    coze_chat.chat_start = false;

    return ESP_OK;
}

/*
 * 功能：恢复聊天（退出暂停态），允许继续上行音频
 * 参数：无
 * 返回：无
 */
extern "C" void coze_chat_app_resume(void)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    coze_chat.chat_pause = false;
}

/*
 * 功能：暂停聊天（中断上行并退出说话态），用于执行本地命令或避免云端应答
 * 参数：无
 * 返回：无
 */
extern "C" void coze_chat_app_pause(void)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    if (coze_chat.websocket_connected)
    {
        coze_chat_app_interrupt();
    }
    // esp_gmf_afe_reset_state(audio_processor_get_afe_handle());
    coze_chat.chat_pause = true;
    change_speaking_state(false);
    // change_wakeup_state(false);
}

/*
 * 功能：显式进入唤醒态（退出休眠），用于允许上行音频
 * 参数：无
 * 返回：无
 */
extern "C" void coze_chat_app_wakeup(void)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    coze_chat.chat_sleep = false;
    change_wakeup_state(true);
}

/*
 * 功能：进入休眠态（中断云端并退出唤醒/说话态），节约资源
 * 参数：无
 * 返回：无
 */
extern "C" void coze_chat_app_sleep(void)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    if (coze_chat.websocket_connected)
    {
        coze_chat_app_interrupt();
    }
    coze_chat.chat_sleep = true;
    change_wakeup_state(false);
    change_speaking_state(false);
}

/*
 * 功能：向云端发送“取消上行音频”指令，快速打断云端识别/应答
 * 参数：无
 * 返回：无（在独立线程中异步执行）
 */
void coze_chat_app_interrupt(void)
{
    ESP_UTILS_LOG_TRACE_GUARD();

    boost::thread([&]()
                  {
        ESP_UTILS_LOG_TRACE_GUARD();
        for (int i = 0; i < COZE_INTERRUPT_TIMES; i++) {
            {
                std::lock_guard lock(coze_chat.chat_mutex); // 保护会话，避免并发操作
                if ((coze_chat.chat == NULL) || !coze_chat.websocket_connected) {
                    break;
                }
                esp_coze_chat_send_audio_cancel(coze_chat.chat);
            }
            boost::this_thread::sleep_for(boost::chrono::milliseconds(COZE_INTERRUPT_INTERVAL_MS));
        } })
        .detach();
}
