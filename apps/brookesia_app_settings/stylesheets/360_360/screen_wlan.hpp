/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_brookesia.hpp"
#include "esp_brookesia_app_settings_ui.hpp"
#include "assets/esp_brookesia_app_settings_assets.h"

namespace esp_brookesia::apps
{

    constexpr SettingsUI_WidgetCellConf SETTINGS_UI_360_360_SCREEN_WLAN_ELEMENT_CONF_SW(const char *label_text)
    {
        return {
            .left_main_label_text = label_text,
            .flags = {
                .enable_left_main_label = 1,
            },
        };
    }

    constexpr SettingsUI_WidgetCellConf SETTINGS_UI_360_360_SCREEN_WLAN_ELEMENT_CONF_CONNECTED_AP()
    {
        return {
            .left_main_label_text = "",
            .left_minor_label_text = "",
            .flags = {
                .enable_left_main_label = 1,
                .enable_left_minor_label = 1,
                .enable_clickable = 0,
            },
        };
    }


    constexpr SettingsUI_WidgetCellConf SETTINGS_UI_360_360_SCREEN_WLAN_ELEMENT_CONF_PROVISIONING_BLUFI_SW()
    {
        return {
            .left_main_label_text = "BluFi Mode",
            .flags = {
                .enable_left_main_label = 1,
            },
        };
    }

    constexpr SettingsUI_ScreenWlanData SETTINGS_UI_360_360_SCREEN_WLAN_DATA()
    {
        return {
            {
                {
                    .title_text = "",
                    .flags = {
                        .enable_title = 0,
                    },
                },
                {
                    .title_text = "Connected network",
                    .flags = {
                        .enable_title = 1,
                    },
                },
                {
                    .title_text = "Available networks",
                    .flags = {
                        .enable_title = 1,
                    },
                },
                {
                    .title_text = "Provisioning",
                    .flags = {
                        .enable_title = 1,
                    },
                },
            },
            {
                SETTINGS_UI_360_360_SCREEN_WLAN_ELEMENT_CONF_SW("WLAN"),
                SETTINGS_UI_360_360_SCREEN_WLAN_ELEMENT_CONF_CONNECTED_AP(),
                SETTINGS_UI_360_360_SCREEN_WLAN_ELEMENT_CONF_PROVISIONING_BLUFI_SW(),
            },
            {
                gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_app_icon_wlan_level1_36_36),
                gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_app_icon_wlan_level2_36_36),
                gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_app_icon_wlan_level3_36_36),
            },
            gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_app_icon_wlan_lock_48_48),
            gui::StyleColor{},
            gui::StyleColor{},
            gui::StyleSize::RECT(200, 24),
        };
    }

} // namespace esp_brookesia::apps
