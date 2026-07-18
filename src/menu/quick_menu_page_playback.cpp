#include "menu/quick_menu_page_playback.h"

#include <stdio.h>

#include "menu/quick_menu.h"

#include "app_flags.h"
#include "app_power.h"
#include "app_state.h"
#include "hal/board_hw_control.h"
#include "hal/ws2812_status.h"
#include "player_control.h"
#include "player_list_select.h"
#include "player_source.h"
#include "storage/storage.h"
#include "ui/ui.h"
#include "web/web_settings.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const char* bool_label(bool enabled)
{
    return enabled ? "开" : "关";
}

const char* value_play_order()
{
    return control_mode_is_random(app_play_mode_get()) ? "随机" : "顺序";
}

int mode_category(play_mode_t mode)
{
    switch (mode) {
        case PLAY_MODE_ARTIST_SEQ:
        case PLAY_MODE_ARTIST_RND:
            return 1;

        case PLAY_MODE_ALBUM_SEQ:
        case PLAY_MODE_ALBUM_RND:
            return 2;

        case PLAY_MODE_ALL_SEQ:
        case PLAY_MODE_ALL_RND:
        default:
            return 0;
    }
}

const char* category_label(int category)
{
    switch (category) {
        case 1: return "歌手";
        case 2: return "专辑";
        case 0:
        default: return "全部";
    }
}

play_mode_t browse_mode_for_category(int category)
{
    switch (category) {
        case 1: return PLAY_MODE_ARTIST_SEQ;
        case 2: return PLAY_MODE_ALBUM_SEQ;
        case 0:
        default: return PLAY_MODE_ALL_SEQ;
    }
}

int& local_browse_category_ref()
{
    // -1 表示首次进入前还没有用户选择，默认跟随当前播放大类显示。
    static int s_local_browse_category = -1;
    if (s_local_browse_category < 0) {
        s_local_browse_category = mode_category(app_play_mode_get());
    }
    return s_local_browse_category;
}

const char* label_play_category()
{
    switch (player_source_type_get()) {
        case PlayerSourceType::NET_TRACK:
            return "NAS播放范围";

        case PlayerSourceType::NET_RADIO:
            return "电台播放范围";

        case PlayerSourceType::LOCAL_TRACK:
        default:
            return "播放大类";
    }
}

const char* value_play_category()
{
    switch (player_source_type_get()) {
        case PlayerSourceType::NET_TRACK:
            return "固定全部歌曲";

        case PlayerSourceType::NET_RADIO:
            return "固定电台列表";

        case PlayerSourceType::LOCAL_TRACK:
        default:
            return category_label(mode_category(app_play_mode_get()));
    }
}

const char* label_local_browse_mode()
{
    switch (player_source_type_get()) {
        case PlayerSourceType::NET_TRACK:
            return "NAS浏览范围";

        case PlayerSourceType::NET_RADIO:
            return "电台浏览范围";

        case PlayerSourceType::LOCAL_TRACK:
        default:
            return "本地浏览方式";
    }
}

const char* value_local_browse_mode()
{
    switch (player_source_type_get()) {
        case PlayerSourceType::NET_TRACK:
            return "固定全部歌曲";

        case PlayerSourceType::NET_RADIO:
            return "固定电台列表";

        case PlayerSourceType::LOCAL_TRACK:
        default:
            return category_label(local_browse_category_ref());
    }
}

const char* value_open()
{
    return "打开";
}

const char* value_execute()
{
    return "执行";
}

const char* value_sleep_timer()
{
    static char buf[16];

    if (!app_power_sleep_timer_is_active()) {
        return "关闭";
    }

    const uint32_t remain = app_power_sleep_timer_remaining_seconds();
    if (remain >= 60) {
        const uint32_t minutes = (remain + 59UL) / 60UL;
        snprintf(buf, sizeof(buf), "剩%lu分", (unsigned long)minutes);
    } else {
        snprintf(buf, sizeof(buf), "剩%lu秒", (unsigned long)remain);
    }

    return buf;
}

const char* value_tf_status()
{
    return storage_is_ready() ? "已就绪" : "未就绪";
}

const char* value_hall_control_enabled()
{
    return bool_label(web_settings_get().hall_control_enabled);
}

const char* value_solenoid_enabled()
{
    return bool_label(web_settings_get().solenoid_enabled);
}

const char* value_status_led_enabled()
{
    return bool_label(web_settings_get().status_led_enabled);
}

const char* value_status_led_brightness()
{
    return status_led_brightness_label(web_settings_get().status_led_brightness);
}

bool action_toggle_play_order()
{
    ui_mode_switch_highlight();
    player_toggle_random();
    return true;
}

bool action_cycle_play_category()
{
    const PlayerSourceType source_type = player_source_type_get();
    if (source_type == PlayerSourceType::NET_TRACK ||
        source_type == PlayerSourceType::NET_RADIO) {
        player_cycle_mode_category();
        return false;
    }

    ui_mode_switch_highlight();
    player_cycle_mode_category();
    return true;
}

bool action_cycle_local_browse_mode()
{
    const PlayerSourceType source_type = player_source_type_get();
    if (source_type == PlayerSourceType::NET_TRACK ||
        source_type == PlayerSourceType::NET_RADIO) {
        // 网络音源只有一个线性列表，不允许切换到歌手或专辑分类。
        return false;
    }

    // 本地浏览方式只影响“当前源列表/本地列表”的浏览入口，
    // 不修改当前播放模式，也不切换正在播放的播放大类。
    int& browse_category = local_browse_category_ref();
    browse_category = (browse_category + 1) % 3;
    return true;
}

bool action_open_current_source_list()
{
    const PlayerSourceState source = player_source_get();
    const bool ok = (source.type == PlayerSourceType::LOCAL_TRACK)
        ? player_list_select_enter(browse_mode_for_category(local_browse_category_ref()))
        : player_list_select_enter(app_play_mode_get());

    // 从“播放控制”进入列表时保留快捷菜单会话。
    // 这样列表里短按 MODE 只退回"播放控制"菜单，长按 MODE 才退出到播放器界面，
    // 行为和"播放源"里的本地/电台/NAS列表一致。
    return ok;
}

bool action_toggle_hall_control()
{
    WebRuntimeSettings ws = web_settings_get();
    ws.hall_control_enabled = !ws.hall_control_enabled;
    web_settings_set(ws);
    (void)web_settings_save();
    return true;
}

bool action_toggle_solenoid()
{
    WebRuntimeSettings ws = web_settings_get();
    ws.solenoid_enabled = !ws.solenoid_enabled;
    web_settings_set(ws);

    // 关闭时立即停止一次输出，保证不会残留在通电状态。
    if (!ws.solenoid_enabled) {
        (void)board_hw_solenoid_stop();
    }

    (void)web_settings_save();
    return true;
}

bool action_toggle_status_led()
{
    WebRuntimeSettings ws = web_settings_get();
    ws.status_led_enabled = !ws.status_led_enabled;
    web_settings_set(ws);

    if (!ws.status_led_enabled) {
        ws2812_status_off();
    } else {
        ws2812_status_force_refresh();
    }

    (void)web_settings_save();
    return true;
}

bool action_cycle_status_led_brightness()
{
    WebRuntimeSettings ws = web_settings_get();
    const uint8_t current = static_cast<uint8_t>(ws.status_led_brightness);
    ws.status_led_brightness = static_cast<StatusLedBrightness>((current + 1U) % 3U);
    web_settings_set(ws);
    ws2812_status_force_refresh();
    (void)web_settings_save();
    return true;
}

bool action_cycle_sleep_timer()
{
    // 每次确认切换一个睡眠关机档位：关闭 -> 15 -> 30 -> 60 -> 90 -> 关闭。
    app_power_sleep_timer_cycle_next();
    return true;
}

bool action_start_incremental_rescan()
{
    const bool ok = app_request_start_rescan(AppRescanMode::Incremental);
    if (ok) {
        quick_menu_exit();
    }
    return ok;
}

bool action_start_full_rescan()
{
    const bool ok = app_request_start_rescan(AppRescanMode::Full);
    if (ok) {
        quick_menu_exit();
    }
    return ok;
}

const QuickMenuItem PLAYBACK_ITEMS[] = {
    {"播放顺序", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_play_order, action_toggle_play_order, true, false},
    {"播放大类", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_play_category, action_cycle_play_category, true, false, label_play_category},
    {"本地浏览方式", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_local_browse_mode, action_cycle_local_browse_mode, true, false, label_local_browse_mode},
    {"当前源列表", QuickMenuItemType::Action, QuickMenuPage::Playback, "", value_open, action_open_current_source_list, true, false},
    {"霍尔控制", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_hall_control_enabled, action_toggle_hall_control, true, false},
    {"状态灯", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_status_led_enabled, action_toggle_status_led, true, false},
    {"状态灯亮度", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_status_led_brightness, action_cycle_status_led_brightness, true, false},
    {"电磁铁动作", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_solenoid_enabled, action_toggle_solenoid, true, false},
    {"睡眠关机", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_sleep_timer, action_cycle_sleep_timer, true, false},
    {"增量重扫", QuickMenuItemType::Action, QuickMenuPage::Playback, "", value_execute, action_start_incremental_rescan, true, false},
    {"强制全量重扫", QuickMenuItemType::Action, QuickMenuPage::Playback, "", value_execute, action_start_full_rescan, true, false},
    {"TF卡状态", QuickMenuItemType::Status, QuickMenuPage::Playback, "", value_tf_status, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

} // namespace

const QuickMenuPageDef& quick_menu_get_playback_page()
{
    static const QuickMenuPageDef page = {
        "播放控制",
        QuickMenuPage::Playback,
        QuickMenuPage::Root,
        PLAYBACK_ITEMS,
        MENU_COUNT(PLAYBACK_ITEMS),
    };

    return page;
}