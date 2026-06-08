#include "menu/quick_menu_page_network.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

#include "web/web_server.h"
#include "web/web_settings.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const char* bool_label(bool enabled)
{
    return enabled ? "开" : "关";
}

const char* value_execute()
{
    return "执行";
}

const char* value_wifi_enabled()
{
    return bool_label(web_wifi_is_enabled());
}

bool action_toggle_wifi()
{
    web_wifi_toggle();
    return true;
}

const char* value_sta_status()
{
    if (!web_wifi_is_enabled()) {
        return "WiFi关";
    }

    return WiFi.status() == WL_CONNECTED ? "已连接" : "未连接";
}

const char* value_ap_status()
{
    if (!web_wifi_is_enabled()) {
        return "关闭";
    }

    const wifi_mode_t mode = WiFi.getMode();
    return (mode == WIFI_AP || mode == WIFI_AP_STA) ? "开启" : "关闭";
}

const char* value_current_ip()
{
    static char buf[24];

    if (!web_wifi_is_enabled()) {
        return "-";
    }

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(buf, sizeof(buf), "%s", WiFi.localIP().toString().c_str());
        return buf;
    }

    const wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_AP || mode == WIFI_AP_STA) {
        snprintf(buf, sizeof(buf), "%s", WiFi.softAPIP().toString().c_str());
        return buf;
    }

    return "-";
}

const char* value_web_addr()
{
    static char buf[40];

    const char* ip = value_current_ip();
    if (!ip || strcmp(ip, "-") == 0) {
        return "不可用";
    }

    snprintf(buf, sizeof(buf), "http://%s", ip);
    return buf;
}

const char* value_show_wifi_info()
{
    return bool_label(web_settings_get().show_wifi_info);
}

bool action_toggle_show_wifi_info()
{
    WebRuntimeSettings ws = web_settings_get();
    ws.show_wifi_info = !ws.show_wifi_info;
    web_settings_set(ws);
    return web_settings_save();
}

bool action_retry_wifi()
{
    return web_server_retry_sta_from_config();
}

const QuickMenuItem NETWORK_ITEMS[] = {
    {"WiFi", QuickMenuItemType::Toggle, QuickMenuPage::Network, "", value_wifi_enabled, action_toggle_wifi, true, false},
    {"STA状态", QuickMenuItemType::Status, QuickMenuPage::Network, "", value_sta_status, nullptr, true, false},
    {"AP模式", QuickMenuItemType::Status, QuickMenuPage::Network, "", value_ap_status, nullptr, true, false},
    {"当前IP", QuickMenuItemType::Status, QuickMenuPage::Network, "", value_current_ip, nullptr, true, false},
    {"Web地址", QuickMenuItemType::Status, QuickMenuPage::Network, "", value_web_addr, nullptr, true, false},
    {"显示WiFi信息", QuickMenuItemType::Toggle, QuickMenuPage::Network, "", value_show_wifi_info, action_toggle_show_wifi_info, true, false},
    {"重连WiFi", QuickMenuItemType::Action, QuickMenuPage::Network, "", value_execute, action_retry_wifi, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

} // namespace

const QuickMenuPageDef& quick_menu_get_network_page()
{
    static const QuickMenuPageDef page = {
        "网络设置",
        QuickMenuPage::Network,
        QuickMenuPage::Root,
        NETWORK_ITEMS,
        MENU_COUNT(NETWORK_ITEMS),
    };

    return page;
}