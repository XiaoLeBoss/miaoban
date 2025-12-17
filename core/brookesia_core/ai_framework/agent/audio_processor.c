/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <string.h>
#include <math.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include "esp_gmf_element.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_io.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_ringbuffer.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_g711_enc.h"
#include "esp_gmf_audio_helper.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_gmf_setup_pool.h"
#include "esp_gmf_setup_peripheral.h"
#include "esp_codec_dev.h"
#include "esp_gmf_fifo.h"

#include "esp_gmf_ringbuffer.h"

#ifndef CONFIG_KEY_PRESS_DIALOG_MODE
#include "esp_vad.h"
#include "esp_afe_config.h"
#include "esp_gmf_afe_manager.h"
#include "esp_gmf_afe.h"
#endif /* CONFIG_KEY_PRESS_DIALOG_MODE */
#include "audio_processor.h"

// 人声检测（VAD）使能：用于检测语音开始/结束事件
#define VAD_ENABLE (true)
// 命令词识别（Multinet）使能：在唤醒后或纯VAD模式下识别本地命令词
#define VCMD_ENABLE (true)
// 播放器输出FIFO的块数量
#define DEFAULT_FIFO_NUM (5)

// 播放输出默认音量（百分比）
#define DEFAULT_PLAYBACK_VOLUME (50)

// GMF AFE馈送/提取任务与流水线任务的默认优先级/栈配置（可根据项目负载调优）
#define DEFAULT_FEED_TASK_PRIO (6)
#define DEFAULT_FEED_TASK_STACK_SIZE (5 * 1024)
#define DEFAULT_FETCH_TASK_PRIO (6)
#define DEFAULT_FETCH_TASK_STACK_SIZE (5 * 1024)
#define DEFAULT_GMF_TASK_PRIO (6)
#define DEFAULT_GMF_TASK_STACK_SIZE (5 * 1024)
#define DEFAULT_PLAYBACK_TASK_PRIO (7)
#define DEFAULT_PLAYBACK_TASK_SIZE (12 * 1024)

// 唤醒保持时长（毫秒）：在此窗口内保持“唤醒态”，便于连续命令
#define AFE_WAKEUP_END_MS (30000)

static char *TAG = "AUDIO_PROCESSOR";

typedef struct
{
    esp_asp_handle_t player;
    enum audio_player_state_e state;
} audio_prompt_t;

typedef struct
{
    esp_gmf_fifo_handle_t fifo;
    recorder_event_callback_t cb;
    void *ctx;
    enum audio_player_state_e state;
#ifndef CONFIG_KEY_PRESS_DIALOG_MODE
    esp_gmf_pipeline_handle_t pipe;
    esp_gmf_afe_manager_handle_t afe_manager;
    afe_config_t *afe_cfg;
    esp_gmf_task_handle_t task;
#endif /* CONFIG_KEY_PRESS_DIALOG_MODE */
} audio_recordert_t;

typedef struct
{
    esp_asp_handle_t player;
    esp_gmf_fifo_handle_t fifo;
    enum audio_player_state_e state;
} audio_playback_t;

typedef struct
{
    esp_codec_dev_handle_t play_dev;
    esp_codec_dev_handle_t rec_dev;
    esp_gmf_pool_handle_t pool;
} audio_manager_t;

#define AUDIO_BUFFER_SIZE 1024 * sizeof(int16_t)
#define GAUSSIAN_SIGMA 1.0 // Gaussian filter standard deviation
/*
 * 重要变量说明
 * - out_rb：录音侧编码器输出的环形缓冲区句柄，上层通过它读取编码后的数据
 * - gaussian_weights：高斯滤波权重（预留，当前未使用）
 * - audio_manager：音频设备与资源池管理对象，封装codec与GMF资源
 * - audio_recorder：录音器上下文，包含AFE管理器与流水线等句柄
 * - audio_playback：播放器上下文，包含FIFO与播放器句柄
 * - audio_prompt：提示音播放器上下文，用于播放短提示音
 */
static esp_gmf_rb_handle_t out_rb = NULL;
float *gaussian_weights;

static audio_manager_t audio_manager;
static audio_recordert_t audio_recorder;
static audio_playback_t audio_playback;
static audio_prompt_t audio_prompt;

/*
 * 功能：初始化音频管理器，完成外设与编解码设备的创建与注册
 * 参数：
 *  - info：外设硬件信息指针，用于初始化I2S/I2C/PA等资源
 *  - play_dev：输出设备句柄回传指针（可为NULL）
 *  - rec_dev：输入设备句柄回传指针（可为NULL）
 * 返回：ESP_OK 表示成功；ESP_FAIL 表示初始化失败
 * 注意事项：
 *  - 将设备句柄注册到GMF资源池，后续流水线会依赖该池中的IO与元素
 */
esp_err_t audio_manager_init(esp_gmf_setup_periph_hardware_info *info, void **play_dev, void **rec_dev)
{
    // 初始化外设与音频编解码设备，并将设备句柄注册到GMF资源池
    esp_gmf_setup_periph(info);
    esp_gmf_setup_periph_codec(&audio_manager.play_dev, &audio_manager.rec_dev);
    esp_gmf_pool_init(&audio_manager.pool);
    pool_register_io(audio_manager.pool);
    pool_register_audio_codecs(audio_manager.pool);
    pool_register_audio_effects(audio_manager.pool);
    pool_register_codec_dev_io(audio_manager.pool, audio_manager.play_dev, audio_manager.rec_dev);
    esp_codec_dev_set_out_vol(audio_manager.play_dev, DEFAULT_PLAYBACK_VOLUME);
    // esp_codec_dev_set_in_gain(audio_manager.rec_dev, 21);

    if (play_dev != NULL)
    {
        *play_dev = audio_manager.play_dev;
    }
    if (rec_dev != NULL)
    {
        *rec_dev = audio_manager.rec_dev;
    }

    return ESP_OK;
}

/*
 * 功能：反初始化音频管理器，释放资源池与编解码设备
 * 参数：无
 * 返回：ESP_OK 表示成功
 * 注意事项：先卸载资源池项，再关闭codec设备
 */
esp_err_t audio_manager_deinit()
{
    pool_unregister_audio_codecs();
    esp_gmf_pool_deinit(audio_manager.pool);
    esp_gmf_teardown_periph_codec(audio_manager.play_dev, audio_manager.rec_dev);
    return ESP_OK;
}

/*
 * 功能：挂起/恢复 AFE 管理器任务，节约功耗或避免与播放冲突
 * 参数：
 *  - suspend：true 挂起；false 恢复
 * 返回：ESP_OK 表示成功；错误码表示失败
 * 注意事项：仅对录音侧AFE任务生效
 */
esp_err_t audio_manager_suspend(bool suspend)
{
    return esp_gmf_afe_manager_suspend(audio_recorder.afe_manager, suspend);
}

/*
 * 功能：提示音播放器输出回调，将数据写入codec输出设备
 * 参数：
 *  - data/data_size：待输出的音频数据及长度
 *  - ctx：未使用
 * 返回：0 表示成功
 * 注意事项：此回调用于短提示音播放链路
 */
static int prompt_out_data_callback(uint8_t *data, int data_size, void *ctx)
{
    esp_codec_dev_write(audio_manager.play_dev, data, data_size);
    return 0;
}

/*
 * 功能：提示音播放器事件回调，处理音乐信息与状态变更
 * 参数：
 *  - event：播放器事件包（类型、载荷）
 *  - ctx：未使用
 * 返回：0 表示处理完成
 * 注意事项：当提示音结束或错误时，自动恢复静音钳制以避免爆音
 */
static int prompt_event_callback(esp_asp_event_pkt_t *event, void *ctx)
{
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO)
    {
        esp_asp_music_info_t info = {0};
        memcpy(&info, event->payload, event->payload_size);
        ESP_LOGI(TAG, "Get info, rate:%d, channels:%d, bits:%d", info.sample_rate, info.channels, info.bits);
    }
    else if (event->type == ESP_ASP_EVENT_TYPE_STATE)
    {
        esp_asp_state_t st = 0;
        memcpy(&st, event->payload, event->payload_size);
        ESP_LOGI(TAG, "Get State, %d,%s", st, esp_audio_simple_player_state_to_str(st));
        if (((st == ESP_ASP_STATE_STOPPED) || (st == ESP_ASP_STATE_FINISHED) || (st == ESP_ASP_STATE_ERROR)))
        {
            audio_prompt.state = AUDIO_PLAYER_STATE_IDLE;
            audio_prompt_play_mute(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            audio_prompt_play_mute(false);
        }
    }
    return 0;
}

#ifndef CONFIG_KEY_PRESS_DIALOG_MODE
/*
 * 功能：录音流水线事件回调，打印流水线运行状态用于调试
 * 参数：
 *  - event：GMF流水线事件包
 *  - ctx：用户上下文（未使用）
 * 返回：ESP_OK 表示处理完成
 * 注意事项：仅打印日志，不改变业务状态
 */
static esp_err_t recorder_pipeline_event(esp_gmf_event_pkt_t *event, void *ctx)
{
    ESP_LOGD(TAG, "CB: RECV Pipeline EVT: el:%s-%p, type:%d, sub:%s, payload:%p, size:%d,%p",
             OBJ_GET_TAG(event->from), event->from, event->type, esp_gmf_event_get_state_str(event->sub),
             event->payload, event->payload_size, ctx);
    return 0;
}

#include "esp_timer.h"

/*
 * 功能：编码器写端口获取写块，从环形缓冲区取得可写数据块
 * 参数：
 *  - handle：未使用
 *  - blk：数据块描述结构，回传缓冲区与大小
 *  - wanted_size：期望写入大小
 *  - block_ticks：阻塞等待ticks
 * 返回：实际返回的写入大小（通常为wanted_size）
 */
static int recorder_outport_acquire_write(void *handle, esp_gmf_data_bus_block_t *blk, int wanted_size, int block_ticks)
{
    esp_gmf_rb_acquire_write(out_rb, blk, wanted_size, block_ticks);
    return wanted_size;
}

/*
 * 功能：编码器写端口释放写块，将数据块提交到环形缓冲区
 * 参数：
 *  - handle：未使用
 *  - blk：数据块描述结构
 *  - block_ticks：阻塞等待ticks
 * 返回：释放的有效字节数
 * 注意事项：当valid_size为0时打印调试信息
 */
static int recorder_outport_release_write(void *handle, esp_gmf_data_bus_block_t *blk, int block_ticks)
{
    int ret = 0;
    if (blk->valid_size)
    {
        ret = blk->valid_size;
    }
    else
    {
        printf("||||| release write, valid_size: %d\n", blk->valid_size);
    }
    esp_gmf_rb_release_write(out_rb, blk, portMAX_DELAY);
    return ret;
}

/*
 * 功能：AFE读端口获取输入负载，从codec采集原始PCM数据
 * 参数：
 *  - handle：未使用
 *  - load：负载结构，包含目标缓冲与有效长度
 *  - wanted_size：期望读取字节数
 *  - block_ticks：阻塞等待ticks
 * 返回：实际读取字节数
 * 注意事项：本函数直接从codec设备读取，不经缓冲
 */
static int recorder_inport_acquire_read(void *handle, esp_gmf_payload_t *load, int wanted_size, int block_ticks)
{
    load->valid_size = wanted_size;
    esp_codec_dev_read(audio_manager.rec_dev, load->buf, wanted_size);
    return wanted_size;
}

/*
 * 功能：AFE读端口释放输入负载，返回有效大小
 * 参数：
 *  - handle：未使用
 *  - load：负载结构
 *  - block_ticks：阻塞等待ticks
 * 返回：有效数据大小
 */
static int recorder_inport_release_read(void *handle, esp_gmf_payload_t *load, int block_ticks)
{
    return load->valid_size;
}

/*
 * 功能：GMF AFE 事件回调，处理唤醒/VAD/命令词事件
 * 参数：
 *  - obj：AFE元素句柄
 *  - event：事件类型与载荷（唤醒信息/命令信息等）
 *  - user_data：用户上下文（未使用）
 * 返回：无
 * 业务逻辑：
 *  - 唤醒开始：进入命令模式，开启命令词检测
 *  - 唤醒结束：退出命令模式，取消命令词检测
 *  - 纯VAD模式：每段语音开始/结束分别开启/取消命令检测
 *  - 命令事件：根据短语ID或拼音串触发串口打印
 * 注意事项：保持唤醒窗口以支持连续命令
 */
static void esp_gmf_afe_event_cb(esp_gmf_obj_handle_t obj, esp_gmf_afe_evt_t *event, void *user_data)
{
    audio_recorder.cb((void *)event, audio_recorder.ctx);
    switch (event->type)
    {
    case ESP_GMF_AFE_EVT_WAKEUP_START:
    {
        // 唤醒开始：开启命令词检测，允许在唤醒窗口内识别本地命令
        esp_gmf_afe_vcmd_detection_cancel(obj);
        esp_gmf_afe_vcmd_detection_begin(obj);
        esp_gmf_afe_wakeup_info_t *info = event->event_data;
        ESP_LOGI(TAG, "WAKEUP_START [%d : %d]", info->wake_word_index, info->wakenet_model_index);
        break;
    }
    case ESP_GMF_AFE_EVT_WAKEUP_END:
    {
        // 唤醒结束：取消命令词检测，退出命令模式
        esp_gmf_afe_vcmd_detection_cancel(obj);
        ESP_LOGI(TAG, "WAKEUP_END");
        break;
    }
    case ESP_GMF_AFE_EVT_VAD_START:
    {
        // 纯VAD模式（未启用唤醒词）下，在每段语音的开始开启一次命令检测
        if (!audio_recorder.afe_cfg->wakenet_init && VCMD_ENABLE)
        {
            esp_gmf_afe_vcmd_detection_cancel(obj);
            esp_gmf_afe_vcmd_detection_begin(obj);
        }
        ESP_LOGI(TAG, "VAD_START");
        break;
    }
    case ESP_GMF_AFE_EVT_VAD_END:
    {
        // 纯VAD模式下，语音段结束后取消命令检测（段落式识别）
        if (!audio_recorder.afe_cfg->wakenet_init && VCMD_ENABLE)
        {
            esp_gmf_afe_vcmd_detection_cancel(obj);
        }
        ESP_LOGI(TAG, "VAD_END");
        break;
    }
    case ESP_GMF_AFE_EVT_VCMD_DECT_TIMEOUT:
    {
        // 命令检测超时（未检测到有效命令）
        ESP_LOGI(TAG, "VCMD_DECT_TIMEOUT");
        break;
    }
    default:
    {
        // 命令词事件：event->type即为短语ID，同时提供拼音串与置信度
        esp_gmf_afe_vcmd_info_t *info = event->event_data;
        ESP_LOGW(TAG, "Command %d, phrase_id %d, prob %f, str: %s",
                 event->type, info->phrase_id, info->prob, info->str);
        if ((event->type == 20) || (strcmp(info->str, "da kai hong wai cai ji") == 0))
        {
            printf("trun on IR\r\n");
        }
        else if ((event->type == 21) || (strcmp(info->str, "guan bi hong wai cai ji") == 0))
        {
            printf("trun off IR\r\n");
        }
        else if ((event->type == 18) || (strcmp(info->str, "da kai dian deng") == 0))
        {

            printf("trun on light\r\n");
        }
        else if ((event->type == 19) || (strcmp(info->str, "guan bi dian deng") == 0))
        {

            printf("trun off light\r\n");
        }
        break;
        }
    }
}
#endif /* CONFIG_KEY_PRESS_DIALOG_MODE */

/*
 * 功能：手动触发唤醒事件，便于测试或业务层主动进入命令模式
 * 参数：无
 * 返回：ESP_OK 表示成功
 */
esp_err_t audio_gmf_trigger_wakeup()
{
    // 手动触发唤醒事件：用于测试或在业务层打开命令模式
    esp_gmf_trigger_wakeup(audio_processor_get_afe_handle());
    return ESP_OK;
}

/*
 * 功能：打开录音器，创建并运行GMF流水线（AFE→rate_cvt→encoder）
 * 参数：
 *  - cb：录音事件回调，接收AFE事件（唤醒/VAD/命令）
 *  - ctx：回调上下文
 * 返回：ESP_OK 表示成功；ESP_FAIL 表示创建失败
 * 业务逻辑：
 *  - 初始化AFE配置（VAD/WakeNet/AEC等）
 *  - 创建AFE管理器与AFE元素，设置事件回调
 *  - 注册输入/输出端口，绑定任务并运行流水线
 * 注意事项：
 *  - feed/fetch任务核绑定与优先级需与系统其他任务协调
 *  - rate_cvt下行编码示例为8kHz，实际采集仍为16kHz
 */
esp_err_t audio_recorder_open(recorder_event_callback_t cb, void *ctx)
{
    esp_gmf_rb_create(1, 1024 * 3, &out_rb);
#if CONFIG_KEY_PRESS_DIALOG_MODE
    (void)cb;
    (void)ctx;
    return ESP_OK;
#else

    esp_gmf_setup_periph_hardware_info hardware_info = {};
    esp_gmf_get_periph_info(&hardware_info);

    // 加载esp-sr模型列表，包含WakeNet与Multinet等
    srmodel_list_t *models = esp_srmodel_init("model");
    const char *ch_format = hardware_info.codec.type == ESP_GMF_CODEC_TYPE_ES7210_IN_ES8311_OUT ? "RMNM" : "MR";
    // 初始化AFE配置：选择通道排列与高性能模式
    audio_recorder.afe_cfg = afe_config_init(ch_format, models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    audio_recorder.afe_cfg->vad_init = VAD_ENABLE;
    audio_recorder.afe_cfg->vad_mode = VAD_MODE_3;
    audio_recorder.afe_cfg->vad_min_speech_ms = 64;
    audio_recorder.afe_cfg->vad_min_noise_ms = 1000;
    audio_recorder.afe_cfg->agc_init = true;
    audio_recorder.afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    audio_recorder.afe_cfg->wakenet_init = true;
    audio_recorder.afe_cfg->aec_init = true;
    // 创建AFE管理器：负责喂入（feed）与取出（fetch）任务
    esp_gmf_afe_manager_cfg_t afe_manager_cfg = DEFAULT_GMF_AFE_MANAGER_CFG(audio_recorder.afe_cfg, NULL, NULL, NULL, NULL);
    afe_manager_cfg.feed_task_setting.prio = 5;
    afe_manager_cfg.feed_task_setting.stack_size = DEFAULT_FEED_TASK_STACK_SIZE;
    afe_manager_cfg.fetch_task_setting.prio = 5;
    afe_manager_cfg.fetch_task_setting.stack_size = DEFAULT_FETCH_TASK_STACK_SIZE;
    afe_manager_cfg.feed_task_setting.core = 0;
    afe_manager_cfg.fetch_task_setting.core = 1;
    esp_gmf_afe_manager_create(&afe_manager_cfg, &audio_recorder.afe_manager);
    esp_gmf_element_handle_t gmf_afe = NULL;
    // 创建GMF AFE元素：挂接事件回调，开启命令词检测与唤醒态窗口
    esp_gmf_afe_cfg_t gmf_afe_cfg = DEFAULT_GMF_AFE_CFG(audio_recorder.afe_manager, esp_gmf_afe_event_cb, NULL, models);
    gmf_afe_cfg.vcmd_detect_en = VCMD_ENABLE;
    gmf_afe_cfg.wakeup_end = AFE_WAKEUP_END_MS; // 唤醒保持时长，避免刚唤醒就静音导致退出命令模式
    esp_gmf_afe_init(&gmf_afe_cfg, &gmf_afe);
    esp_gmf_pool_register_element(audio_manager.pool, gmf_afe, NULL);
    // 构建录音流水线：AFE→采样率转换→编码器（G711A）
    const char *name[] = {"ai_afe", "rate_cvt", "encoder"};
    esp_gmf_pool_new_pipeline(audio_manager.pool, NULL, name, sizeof(name) / sizeof(char *), NULL, &audio_recorder.pipe);
    if (audio_recorder.pipe == NULL)
    {
        ESP_LOGE(TAG, "There is no pipeline");
        goto __quit;
    }
    // printf("---》 outport acquire: %p\n", recorder_outport_acquire_write);
    // 编码器写端口绑定到环形缓冲区，供上层读取数据
    esp_gmf_port_handle_t outport = NULL;
    outport = NEW_ESP_GMF_PORT_OUT_BYTE(recorder_outport_acquire_write,
                                        recorder_outport_release_write,
                                        NULL,
                                        &outport,
                                        0,
                                        100);
    esp_gmf_pipeline_reg_el_port(audio_recorder.pipe, "encoder", ESP_GMF_IO_DIR_WRITER, outport);

    // AFE读端口从codec设备采集原始PCM（16k，16bit，单声道）
    esp_gmf_port_handle_t import = NEW_ESP_GMF_PORT_IN_BYTE(recorder_inport_acquire_read,
                                                            recorder_inport_release_read,
                                                            NULL,
                                                            NULL,
                                                            2048,
                                                            100);
    esp_gmf_pipeline_reg_el_port(audio_recorder.pipe, "ai_afe", ESP_GMF_IO_DIR_READER, import);

    esp_gmf_obj_handle_t rate_cvt = NULL;
    esp_gmf_pipeline_get_el_by_name(audio_recorder.pipe, "rate_cvt", &rate_cvt);
    // 下行另路编码为8kHz（示例），与实际语音链路保持16kHz
    esp_gmf_rate_cvt_set_dest_rate(rate_cvt, 8000);

    esp_gmf_element_handle_t enc_handle = NULL;
    esp_gmf_pipeline_get_el_by_name(audio_recorder.pipe, "encoder", &enc_handle);

    // 配置G711A编码器的目标音频信息（示例用途）
    esp_gmf_info_sound_t info = {0};
    info.sample_rates = 8000;
    info.channels = 1;
    info.bits = 16;
    esp_gmf_audio_helper_reconfig_enc_by_type(ESP_AUDIO_TYPE_G711A, &info, (esp_audio_enc_config_t *)OBJ_GET_CFG(enc_handle));

    esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    cfg.ctx = NULL;
    cfg.cb = NULL;
    cfg.thread.core = 0;
    cfg.thread.prio = DEFAULT_GMF_TASK_PRIO;
    cfg.thread.stack = DEFAULT_GMF_TASK_STACK_SIZE;
    cfg.thread.stack_in_ext = true;
    esp_gmf_task_init(&cfg, &audio_recorder.task);
    esp_gmf_pipeline_bind_task(audio_recorder.pipe, audio_recorder.task);
    esp_gmf_pipeline_loading_jobs(audio_recorder.pipe);
    esp_gmf_pipeline_set_event(audio_recorder.pipe, recorder_pipeline_event, NULL);
    esp_gmf_pipeline_run(audio_recorder.pipe);

    audio_recorder.cb = cb;
    audio_recorder.ctx = ctx;
    return ESP_OK;
__quit:
    esp_gmf_pipeline_stop(audio_recorder.pipe);
    esp_gmf_task_deinit(audio_recorder.task);
    afe_config_free(audio_recorder.afe_cfg);
    esp_gmf_afe_manager_destroy(audio_recorder.afe_manager);
    return ESP_FAIL;
#endif /* CONFIG_KEY_PRESS_DIALOG_MODE */
}

/*
 * 功能：关闭录音器，销毁流水线与AFE管理器
 * 参数：无
 * 返回：ESP_OK 表示成功
 * 注意事项：避免重复关闭，按顺序释放资源
 */
esp_err_t audio_recorder_close(void)
{
    if (audio_recorder.state == AUDIO_PLAYER_STATE_CLOSED)
    {
        ESP_LOGW(TAG, "Audio recorded is ready closed");
        return ESP_OK;
    }
#ifndef CONFIG_KEY_PRESS_DIALOG_MODE
    esp_gmf_pipeline_destroy(audio_recorder.pipe);
    esp_gmf_task_deinit(audio_recorder.task);
    afe_config_free(audio_recorder.afe_cfg);
    esp_gmf_afe_manager_destroy(audio_recorder.afe_manager);
#endif /* CONFIG_KEY_PRESS_DIALOG_MODE */
    audio_recorder.state = AUDIO_PLAYER_STATE_CLOSED;
    return ESP_OK;
}

/*
 * 功能：读取编码后的录音数据（G711A），用于上层网络发送或存储
 * 参数：
 *  - data：目标缓冲区
 *  - data_size：期望读取字节数
 * 返回：实际读取字节数
 * 注意事项：从环形缓冲区以块方式读取，需负责释放临时内存
 */
esp_err_t audio_recorder_read_data(uint8_t *data, int data_size)
{
    // TODO: will support opus data
#if CONFIG_KEY_PRESS_DIALOG_MODE
    esp_codec_dev_read(audio_manager.rec_dev, data, data_size);
    return data_size;
#else
    // 从环形缓冲区读取编码后的数据块，供上层网络或存储使用
    esp_gmf_data_bus_block_t blk;
    blk.buf = malloc(data_size);
    blk.buf_length = data_size;
    blk.valid_size = 0;
    blk.is_last = false;
    esp_gmf_rb_acquire_read(out_rb, &blk, data_size, portMAX_DELAY);
    memcpy(data, blk.buf, blk.valid_size);

    esp_gmf_rb_release_read(out_rb, &blk, portMAX_DELAY);
    free(blk.buf);
    return blk.valid_size;
#endif /* CONFIG_KEY_PRESS_DIALOG_MODE */
}

/*
 * 功能：向播放FIFO写入数据，用于后续播放器消费
 * 参数：
 *  - data/data_size：待写入的音频数据及长度
 * 返回：ESP_OK 表示成功；ESP_FAIL 表示FIFO操作失败
 * 注意事项：写入为阻塞获取/释放，保证数据完整性
 */
esp_err_t audio_playback_feed_data(uint8_t *data, int data_size)
{
    esp_gmf_data_bus_block_t blk = {0};
    int ret = esp_gmf_fifo_acquire_write(audio_playback.fifo, &blk, data_size, portMAX_DELAY);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "Fifo acquire write failed (0x%x)", ret);
        return ESP_FAIL;
    }
    memcpy(blk.buf, data, data_size);
    blk.valid_size = data_size;
    esp_gmf_fifo_release_write(audio_playback.fifo, &blk, portMAX_DELAY);
    return ESP_OK;
}

/*
 * 功能：播放器读回调，从FIFO取出待播放的数据
 * 参数：
 *  - data/data_size：输出缓冲与期望长度
 *  - ctx：FIFO句柄
 * 返回：实际读取字节数
 */
static int playback_read_callback(uint8_t *data, int data_size, void *ctx)
{
    esp_gmf_data_bus_block_t blk = {0};
    int ret = esp_gmf_fifo_acquire_read(audio_playback.fifo, &blk, data_size,
                                        portMAX_DELAY);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "Fifo acquire read failed (0x%x)", ret);
    }
    memcpy(data, blk.buf, blk.valid_size);
    esp_gmf_fifo_release_read(audio_playback.fifo, &blk, 0);
    return blk.valid_size;
}

/*
 * 功能：播放器写回调，将数据写入codec输出设备
 * 参数：
 *  - data/data_size：输入数据与长度
 *  - ctx：codec输出设备句柄
 * 返回：写入字节数；错误返回-1
 * 注意事项：当提示音在播时跳过写入，避免音源冲突
 */
static int playback_write_callback(uint8_t *data, int data_size, void *ctx)
{
    if (audio_prompt.state == AUDIO_PLAYER_STATE_PLAYING)
    {
        ESP_LOGW(TAG, "Audio prompt is playing, skip\n");
        return data_size;
    }
    esp_codec_dev_handle_t dev = (esp_codec_dev_handle_t)ctx;
    int ret = esp_codec_dev_write(dev, data, data_size);
    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(TAG, "Write to codec dev failed (0x%x)\n", ret);
        return -1;
    }
    return data_size;
}

/*
 * 功能：播放器事件回调，处理音乐信息与状态变更
 * 参数：
 *  - event：事件包
 *  - ctx：未使用或用于业务状态
 * 返回：0 表示处理完成
 */
static int playback_event_callback(esp_asp_event_pkt_t *event, void *ctx)
{
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO)
    {
        esp_asp_music_info_t info;
        memcpy(&info, event->payload, event->payload_size);
        ESP_LOGI(TAG, "Get info, rate:%d, channels:%d, bits:%d", info.sample_rate,
                 info.channels, info.bits);
    }
    else if (event->type == ESP_ASP_EVENT_TYPE_STATE)
    {
        esp_asp_state_t st = ESP_ASP_STATE_NONE;
        memcpy(&st, event->payload, event->payload_size);
        ESP_LOGI(TAG, "Get State, %d,%s", st,
                 esp_audio_simple_player_state_to_str(st));
        if (ctx && ((st == ESP_ASP_STATE_STOPPED) || (st == ESP_ASP_STATE_FINISHED) || (st == ESP_ASP_STATE_ERROR)))
        {
            audio_prompt.state = AUDIO_PLAYER_STATE_IDLE;
        }
    }
    return 0;
}

/*
 * 功能：打开播放器，创建FIFO与播放器对象
 * 参数：无
 * 返回：ESP_OK 表示成功；错误码表示失败
 * 注意事项：设置事件回调与任务参数后进入空闲态
 */
esp_err_t audio_playback_open(void)
{
    esp_err_t err = ESP_GMF_ERR_OK;

    do
    {
        err = esp_gmf_fifo_create(DEFAULT_FIFO_NUM, 1, &audio_playback.fifo);
        if (err != ESP_GMF_ERR_OK)
        {
            ESP_LOGE(TAG, "oai_plr_dec_fifo init failed (0x%x)", err);
            break;
        }
        esp_asp_cfg_t player_cfg = {0};
        player_cfg.in.cb = playback_read_callback;
        player_cfg.in.user_ctx = audio_playback.fifo;
        player_cfg.out.cb = playback_write_callback;
        player_cfg.out.user_ctx = audio_manager.play_dev;
        player_cfg.task_prio = DEFAULT_PLAYBACK_TASK_PRIO;
        player_cfg.task_stack = DEFAULT_PLAYBACK_TASK_SIZE;
        player_cfg.task_core = 1;

        err = esp_audio_simple_player_new(&player_cfg, &audio_playback.player);
        if (err != ESP_GMF_ERR_OK)
        {
            ESP_LOGE(TAG, "simple_player init failed (0x%x)", err);
            break;
        }
        err = esp_audio_simple_player_set_event(audio_playback.player,
                                                playback_event_callback, NULL);
        if (err != ESP_GMF_ERR_OK)
        {
            ESP_LOGE(TAG, "set_event failed (0x%x)", err);
            break;
        }
        audio_playback.state = AUDIO_PLAYER_STATE_IDLE;
        return ESP_OK;

    } while (0);

    if (audio_playback.player)
    {
        esp_audio_simple_player_destroy(audio_playback.player);
    }
    if (audio_playback.fifo)
    {
        esp_gmf_fifo_destroy(audio_playback.fifo);
    }
    return err;
}

/*
 * 功能：关闭播放器，释放播放器与FIFO资源
 * 参数：无
 * 返回：ESP_OK 表示成功；ESP_FAIL 表示失败
 * 注意事项：如在播放中需先停止
 */
esp_err_t audio_playback_close(void)
{
    if (audio_playback.state == AUDIO_PLAYER_STATE_CLOSED)
    {
        ESP_LOGW(TAG, "Aduio playback is realdy closed");
        return ESP_OK;
    }
    if (audio_playback.state == AUDIO_PLAYER_STATE_PLAYING)
    {
        audio_playback_stop();
    }
    esp_err_t err = esp_audio_simple_player_destroy(audio_playback.player);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio playback closing failed");
        return ESP_FAIL;
    }
    audio_playback.state = AUDIO_PLAYER_STATE_CLOSED;
    return ESP_OK;
}

/*
 * 功能：启动播放器，加载并播放指定音源
 * 参数：无
 * 返回：ESP_OK 表示成功；ESP_FAIL 表示失败
 * 注意事项：示例使用raw路径与G711A信息；实际工程可替换
 */
esp_err_t audio_playback_run(void)
{
    if (audio_playback.state == AUDIO_PLAYER_STATE_PLAYING)
    {
        ESP_LOGW(TAG, "Audio playback is realdy running");
        return ESP_OK;
    }
    esp_asp_music_info_t music_info;
    music_info.sample_rate = 16000;
    music_info.channels = 1;
    music_info.bits = 16;
    music_info.bitrate = 0;

    esp_err_t err = esp_audio_simple_player_run(audio_playback.player, "raw://sdcard/coze.opus",
                                                &music_info);
    if (err != ESP_GMF_ERR_OK)
    {
        ESP_LOGE(TAG, "run failed (0x%x)", err);
        return ESP_FAIL;
    }
    audio_playback.state = AUDIO_PLAYER_STATE_PLAYING;
    return ESP_OK;
}

/*
 * 功能：停止播放器，恢复到空闲态
 * 参数：无
 * 返回：ESP_OK 表示成功；ESP_FAIL 表示失败
 */
esp_err_t audio_playback_stop(void)
{
    if (audio_playback.state == AUDIO_PLAYER_STATE_IDLE)
    {
        ESP_LOGW(TAG, "Audio playback is already stopped");
        return ESP_OK;
    }
    esp_err_t ret = esp_audio_simple_player_stop(audio_playback.player);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio playback stop failed");
        return ESP_FAIL;
    }
    audio_playback.state = AUDIO_PLAYER_STATE_IDLE;
    return ESP_OK;
}

/*
 * 功能：打开提示音播放器，创建播放器对象并设置事件回调
 * 参数：无
 * 返回：ESP_OK 表示成功；错误码表示失败
 */
esp_err_t audio_prompt_open(void)
{
    esp_asp_cfg_t cfg = {
        .in.cb = NULL,
        .in.user_ctx = NULL,
        .out.cb = prompt_out_data_callback,
        .out.user_ctx = NULL,
        .task_prio = 5,
    };
    esp_gmf_err_t err = esp_audio_simple_player_new(&cfg, &audio_prompt.player);
    err = esp_audio_simple_player_set_event(audio_prompt.player, prompt_event_callback, NULL);
    audio_prompt.state = AUDIO_PLAYER_STATE_IDLE;
    return err;
}

esp_err_t audio_prompt_close(void)
{
    if (audio_prompt.state == AUDIO_PLAYER_STATE_PLAYING)
    {
        esp_audio_simple_player_stop(audio_prompt.player);
    }
    esp_err_t err = esp_audio_simple_player_destroy(audio_prompt.player);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio prompt closing failed");
        return ESP_FAIL;
    }
    audio_prompt.state = AUDIO_PLAYER_STATE_CLOSED;
    return ESP_OK;
}

esp_err_t audio_prompt_play(const char *url)
{
    if (audio_prompt.state == AUDIO_PLAYER_STATE_PLAYING)
    {
        ESP_LOGE(TAG, "audio_prompt is already playing");
        return ESP_OK;
    }
    esp_audio_simple_player_run(audio_prompt.player, url, NULL);
    audio_prompt.state = AUDIO_PLAYER_STATE_PLAYING;
    return ESP_OK;
}

esp_err_t audio_prompt_stop(void)
{
    if (audio_prompt.state == AUDIO_PLAYER_STATE_IDLE)
    {
        ESP_LOGW(TAG, "audio_prompt_stop, but state is idle");
        return ESP_FAIL;
    }
    esp_audio_simple_player_stop(audio_prompt.player);
    audio_prompt.state = AUDIO_PLAYER_STATE_IDLE;
    return ESP_OK;
}

esp_err_t audio_prompt_play_with_block(const char *url, int timeout_ms)
{
    ESP_LOGI(TAG, "audio_prompt_play_with_block, url: %s, timeout_ms: %d", url, timeout_ms);

    if (timeout_ms < 0)
    {
        timeout_ms = 60 * 60 * 1000;
    }

    int64_t start_time = esp_timer_get_time();
    while (audio_prompt.state == AUDIO_PLAYER_STATE_PLAYING)
    {
        if (esp_timer_get_time() - start_time > (int64_t)timeout_ms * 1000)
        {
            ESP_LOGE(
                TAG, "Play audio(%s) timeout(%dms), state(%d), start_time(%d), end_time(%d)",
                url, timeout_ms, audio_prompt.state, (int)(start_time / 1000), (int)(esp_timer_get_time() / 1000));
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return audio_prompt_play(url);
}

esp_gmf_element_handle_t audio_processor_get_afe_handle(void)
{
    esp_gmf_element_handle_t safe = NULL;
    esp_gmf_pipeline_get_el_by_name(audio_recorder.pipe, "ai_afe", &safe);
    return safe;
}

esp_err_t audio_prompt_play_mute(bool enable_mute)
{
    ESP_LOGI(TAG, "audio_prompt_play_mute, enable_mute: %d", enable_mute);

    esp_codec_dev_set_out_mute(audio_manager.play_dev, enable_mute);
    return ESP_OK;
}
