/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <cassert>
#include "lvgl.h"
#include "boost/thread.hpp"
#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "Main"
#include "esp_lib_utils.h" // 包含 ESP 库的通用工具函数
#include "modules/audio_sys.h" // 包含音频系统相关函数
#include "modules/display.hpp" // 包含显示相关函数
#include "modules/services.hpp" // 包含服务相关函数
#include "modules/audio.hpp" // 包含音频相关函数
#include "modules/system.hpp" // 包含系统相关函数
#include "modules/file_system.hpp" // 包含文件系统相关函数(SD卡/flash文件系统)
#include "modules/led_indicator.h" // 包含 LED 指示灯相关函数
#include "blufi_app.h" // 包含 BluFi 应用程序相关函数
#include "esp_brookesia.hpp" //Brookesia 框架（可能是本项目使用的应用框架）。
#include "ai_framework/agent/audio_processor.h" //AI 音频处理相关。

constexpr bool EXAMPLE_SHOW_MEM_INFO = true;

extern "C" void app_main()
{
    restart_usb_serial_jtag();
    printf("Project version: %s\n", CONFIG_APP_PROJECT_VER);

    assert(services_init() && "Initialize services failed");
    auto default_dummy_draw = !system_check_is_developer_mode();
    assert(display_init(default_dummy_draw) && "Initialize display failed");
    assert(led_indicator_init() && "Initialize led indicator failed");
    if (!file_system_init())
    {
        ESP_UTILS_LOGE("Initialize file system failed, related features will be disabled");
    }
    /* Initialize audio if BluFi is disabled */
    // if (!is_blufi_enabled)
    // {
    //     assert(audio_init() && "Initialize audio failed");
    // }
    // else
    {
        // Disable audio to focus on BluFi
        audio_manager_suspend(true);
    }
    assert(system_init() && "Initialize system failed");
    {
        auto &storage_service = esp_brookesia::services::StorageNVS::requestInstance();
        esp_brookesia::services::StorageNVS::Value blufi_sw_flag = 0;
        if (!storage_service.getLocalParam(esp_brookesia::systems::speaker::Manager::SETTINGS_BLUFI_SWITCH, blufi_sw_flag))
        {
            storage_service.setLocalParam(esp_brookesia::systems::speaker::Manager::SETTINGS_BLUFI_SWITCH, blufi_sw_flag);
        }
        if (std::get<int>(blufi_sw_flag))
        {
            blufi_app_start();
        }
    }

    if constexpr (EXAMPLE_SHOW_MEM_INFO)
    {
        esp_utils::thread_config_guard thread_config({
            .name = "mem_info",
            .stack_size = 4096,
        });
        boost::thread([=]()
                      {
            while (1) {
                esp_utils_mem_print_info();

                audio_sys_get_real_time_stats();

                boost::this_thread::sleep_for(boost::chrono::seconds(5));
            } })
            .detach();
    }
}
