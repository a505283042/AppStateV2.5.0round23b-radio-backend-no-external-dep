#include "menu/quick_menu_page_playback.h"

#include <stdio.h>

#include "menu/quick_menu.h"

#include "app_flags.h"
#include "app_power.h"
#include "app_state.h"
#include "hal/board_hw_control.h"
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
    return control_mode_is_random(g_play_mode) ? "随机" : "顺序";
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
        s_local_browse_category = mode_category(g_play_mode);
    }
    return s_local_browse_category;
}

const char* value_play_category()
{
    return category_label(mode_category(g_play_mode));
}

const char* value_local_browse_mode()
{
    return category_label(local_browse_category_ref());
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

bool action_toggle_play_order()
{
    ui_mode_switch_highlight();
    player_toggle_random();
    return true;
}

bool action_cycle_play_category()
{
    ui_mode_switch_highlight();
    player_cycle_mode_category();
    return true;
}

bool action_cycle_local_browse_mode()
{
    // 本地浏览方式只影响“当前源列表/本地列表”的浏览入口，
    // 不修改 g_play_mode，也不切换正在播放的播放大类。
    int& browse_category = local_browse_category_ref();
    browse_category = (browse_category + 1) % 3;
    return true;
}

bool action_open_current_source_list()
{
    const PlayerSourceState source = player_source_get();
    const bool ok = (source.type == PlayerSourceType::LOCAL_TRACK)
        ? player_list_select_enter(browse_mode_for_category(local_browse_category_ref()))
        : player_list_select_enter(g_play_mode);

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

bool action_cycle_sleep_timer()
{
    // 每次确认切换一个睡眠关机档位：关闭 -> 15 -> 30 -> 60 -> 90 -> 120 -> 关闭。
    app_power_sleep_timer_cycle_next();
    return true;
}

bool action_start_rescan()
{
    const bool ok = app_request_start_rescan();
    if (ok) {
        quick_menu_exit();
    }
    return ok;
}

const QuickMenuItem PLAYBACK_ITEMS[] = {
    {"播放顺序", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_play_order, action_toggle_play_order, true, false},
    {"播放大类", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_play_category, action_cycle_play_category, true, false},
    {"本地浏览方式", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_local_browse_mode, action_cycle_local_browse_mode, true, false},
    {"当前源列表", QuickMenuItemType::Action, QuickMenuPage::Playback, "", value_open, action_open_current_source_list, true, false},
    {"霍尔控制", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_hall_control_enabled, action_toggle_hall_control, true, false},
    {"电磁铁动作", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_solenoid_enabled, action_toggle_solenoid, true, false},
    {"睡眠关机", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_sleep_timer, action_cycle_sleep_timer, true, false},
    {"重扫曲库", QuickMenuItemType::Action, QuickMenuPage::Playback, "", value_execute, action_start_rescan, true, false},
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