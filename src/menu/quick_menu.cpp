#include "menu/quick_menu.h"

#include <Arduino.h>

#include "menu/quick_menu_pages.h"
#include "menu/quick_menu_page_nfc.h"
#include "utils/log.h"

// quick_menu.cpp 在匿名命名空间前面就会调用页面定义表，
// 这里显式声明一次，避免本地头文件包含顺序不一致时编译器看不到声明。
const QuickMenuPageDef& quick_menu_get_page_def(QuickMenuPage page);

namespace {

static constexpr uint32_t MENU_AUTO_EXIT_MS = 30000;
static constexpr uint32_t MENU_CONFIRM_GUARD_MS = 250;
static constexpr uint32_t MENU_DYNAMIC_REFRESH_MS = 1000;

static bool s_active = false;
static QuickMenuPage s_page = QuickMenuPage::Root;
static int s_selected = 0;

static constexpr uint8_t MENU_PAGE_STATE_COUNT = 16;
static uint8_t s_selected_by_page[MENU_PAGE_STATE_COUNT] = {};
static uint32_t s_last_dynamic_refresh_ms = 0;

static uint8_t page_state_index(QuickMenuPage page)
{
    const uint8_t idx = static_cast<uint8_t>(page);
    return idx < MENU_PAGE_STATE_COUNT ? idx : 0;
}

static void save_current_selection()
{
    s_selected_by_page[page_state_index(s_page)] = s_selected;
}

static void reset_menu_session_selection()
{
    for (uint8_t i = 0; i < MENU_PAGE_STATE_COUNT; ++i) {
        s_selected_by_page[i] = 0;
    }
}

static uint8_t restore_selection_for_page(QuickMenuPage page)
{
    const QuickMenuPageDef& def = quick_menu_get_page_def(page);

    if (def.item_count == 0) {
        return 0;
    }

    const uint8_t saved = s_selected_by_page[page_state_index(page)];

    if (saved >= def.item_count) {
        return static_cast<uint8_t>(def.item_count - 1);
    }

    return saved;
}

static uint32_t s_last_action_ms = 0;
static uint32_t s_revision = 1;
static uint32_t s_confirm_guard_until_ms = 0;

// 整屏重绘序号。
// NFC列表内部换页时 QuickMenuPage 可能不变，所以额外用这个序号通知 UI 必须清屏重画。
static uint32_t s_full_refresh_seq = 1;

void mark_dirty()
{
    ++s_revision;
    if (s_revision == 0) {
        s_revision = 1;
    }
}

void touch_menu()
{
    s_last_action_ms = millis();
}

void arm_confirm_guard()
{
    s_confirm_guard_until_ms = millis() + MENU_CONFIRM_GUARD_MS;
}

bool confirm_guard_active()
{
    return static_cast<int32_t>(millis() - s_confirm_guard_until_ms) < 0;
}

static bool quick_menu_page_is_dynamic(QuickMenuPage page)
{
    switch (page) {
        case QuickMenuPage::Playback:
        case QuickMenuPage::Source:
        case QuickMenuPage::Network:
        case QuickMenuPage::MemoryInfo:
        case QuickMenuPage::StackInfo:
        case QuickMenuPage::BatteryInfo:
            return true;

        default:
            return false;
    }
}

const QuickMenuPageDef& current_page_def()
{
    return quick_menu_get_page_def(s_page);
}

static void open_page(QuickMenuPage page)
{
    const QuickMenuPage prev_page = s_page;

    save_current_selection();

    // 从 NFC管理 重新进入 NFC列表管理 时，从第一页开始。
    // 从 NFC详情 返回/跳回 NFC列表管理 时，不重置页码，方便继续看刚才那一页。
    if (page == QuickMenuPage::NfcList && prev_page != QuickMenuPage::NfcDetail) {
        quick_menu_nfc_list_reset_page();
    }

    s_page = page;

    // 从上一级进入下一级菜单时，始终从第一行开始。
    // 不再恢复该子页面上次停留的位置，避免重新进入时默认落在“返回”项。
    s_selected = 0;
    s_selected_by_page[page_state_index(page)] = 0;
    s_last_dynamic_refresh_ms = 0;

    touch_menu();
    mark_dirty();
    if (page == QuickMenuPage::NfcList) {
        ++s_full_refresh_seq;
        if (s_full_refresh_seq == 0) {
            s_full_refresh_seq = 1;
        }
    }
    arm_confirm_guard();
}

static uint8_t nfc_list_first_select_index(const QuickMenuPageDef& def)
{
    if (def.item_count <= 1) {
        return 0;
    }

    // NFC列表第0行通常是“绑定数量 / 页码”状态行，
    // 翻到新页后优先选中第1行，也就是第一条绑定记录。
    return 1;
}

static uint8_t nfc_list_last_select_index(const QuickMenuPageDef& def)
{
    if (def.item_count == 0) {
        return 0;
    }

    return static_cast<uint8_t>(def.item_count - 1);
}

static void move_selection(int8_t delta)
{
    const QuickMenuPageDef& def = current_page_def();

    if (def.item_count == 0) {
        return;
    }

    // NFC列表管理是“同一个 QuickMenuPage 内部翻页”。
    // 普通菜单只知道 item_count，不知道 NFC 列表页码，
    // 所以这里单独处理旋钮越界翻页。
    if (s_page == QuickMenuPage::NfcList) {
        const bool at_first = s_selected <= 0 || (def.item_count <= 2 && delta < 0);
        const bool at_last = s_selected >= static_cast<int>(def.item_count - 1);

        // 反向旋转到第一页之前：翻到上一页。
        // 特别修复：最后一页只有“返回”时，s_selected 永远是0，
        // 如果不在这里处理，就只能在“返回”上打转，回不到上一页。
        if (delta < 0 && at_first) {
            if (quick_menu_nfc_list_prev_page()) {
                const QuickMenuPageDef& new_def = current_page_def();
                s_selected = nfc_list_last_select_index(new_def);
                save_current_selection();

                touch_menu();
                quick_menu_request_full_refresh();
                return;
            }
        }

        // 正向旋转越过本页最后一项：翻到下一页。
        if (delta > 0 && at_last) {
            if (quick_menu_nfc_list_next_page()) {
                const QuickMenuPageDef& new_def = current_page_def();
                s_selected = nfc_list_first_select_index(new_def);
                save_current_selection();

                touch_menu();
                quick_menu_request_full_refresh();
                return;
            }
        }
    }

    if (delta > 0) {
        s_selected = static_cast<uint8_t>((s_selected + 1) % def.item_count);
    } else {
        s_selected = (s_selected == 0)
            ? static_cast<uint8_t>(def.item_count - 1)
            : static_cast<uint8_t>(s_selected - 1);
    }

    save_current_selection();

    touch_menu();
    mark_dirty();
}

static void go_back()
{
    const QuickMenuPageDef& def = current_page_def();

    if (s_page == QuickMenuPage::Root) {
        // 根菜单已经没有上一级：无论是短按返回键，还是确认“返回”项，
        // 都直接退出快捷菜单回到播放器界面。
        quick_menu_exit();
        return;
    }

    save_current_selection();

    s_page = def.parent;
    s_selected = restore_selection_for_page(s_page);
    s_last_dynamic_refresh_ms = 0;

    touch_menu();
    mark_dirty();
    arm_confirm_guard();
}

const QuickMenuItem* selected_item()
{
    const QuickMenuPageDef& info = current_page_def();

    if (info.item_count == 0 || info.items == nullptr) {
        return nullptr;
    }

    if (s_selected < 0 || s_selected >= info.item_count) {
        s_selected = 0;
    }

    return &info.items[s_selected];
}

void confirm_current()
{
    const QuickMenuItem* item = selected_item();
    if (item == nullptr) {
        return;
    }

    touch_menu();

    switch (item->type) {
        case QuickMenuItemType::SubPage:
            open_page(item->child);
            return;

        case QuickMenuItemType::Back:
            // 菜单里的“返回”项保留原语义：根菜单确认“返回”可退出播放器界面。
            go_back();
            return;

        case QuickMenuItemType::Status:
            LOGD("[菜单] 状态 item: %s", item->label ? item->label : "");
            mark_dirty();
            return;

        case QuickMenuItemType::Placeholder:
            LOGW("[菜单] 占位图 item: %s", item->label ? item->label : "");
            mark_dirty();
            return;

        case QuickMenuItemType::Action:
        case QuickMenuItemType::Toggle:
            if (item->on_confirm != nullptr) {
                const bool changed = item->on_confirm();
                LOGD("[菜单] action item=%s changed=%d",
                     item->label ? item->label : "",
                     changed ? 1 : 0);
                mark_dirty();
                return;
            }

            LOGW("[菜单] item 尚未接入: %s", item->label ? item->label : "");
            mark_dirty();
            return;

        default:
            return;
    }
}

const char* item_value(const QuickMenuItem& item)
{
    if (item.get_value != nullptr) {
        return item.get_value();
    }

    if (item.static_value != nullptr) {
        return item.static_value;
    }

    return "";
}

} // namespace

bool quick_menu_is_active()
{
    return s_active;
}

void quick_menu_enter()
{
    // 每次从播放器重新进入菜单，都从根菜单第一项开始。
    // 子菜单的选中记忆也只在本次菜单会话内有效。
    reset_menu_session_selection();

    s_active = true;
    s_page = QuickMenuPage::Root;
    s_selected = 0;
    s_last_dynamic_refresh_ms = 0;

    touch_menu();
    mark_dirty();
    arm_confirm_guard();
}

void quick_menu_open_page(QuickMenuPage page)
{
    if (!s_active) {
        return;
    }

    open_page(page);
}

void quick_menu_exit()
{
    if (!s_active) {
        return;
    }

    s_active = false;
    s_page = QuickMenuPage::Root;
    s_selected = 0;
    s_last_action_ms = 0;
    s_confirm_guard_until_ms = 0;
    ++s_full_refresh_seq;
    if (s_full_refresh_seq == 0) {
        s_full_refresh_seq = 1;
    }

    mark_dirty();

    LOGD("[菜单] 退出");
}

void quick_menu_tick()
{
    if (!s_active) {
        return;
    }

    const uint32_t now = millis();
    if (now - s_last_action_ms >= MENU_AUTO_EXIT_MS) {
        LOGD("[菜单] auto 退出");
        quick_menu_exit();
    }

    if (quick_menu_page_is_dynamic(s_page)) {
        if (s_last_dynamic_refresh_ms == 0 ||
            now - s_last_dynamic_refresh_ms >= MENU_DYNAMIC_REFRESH_MS) {
            s_last_dynamic_refresh_ms = now;

            // 只标记内容变化，不刷新用户操作时间。
            // 这样不会因为动态刷新导致菜单永远不自动退出。
            mark_dirty();
        }
    }
}

void quick_menu_request_refresh()
{
    if (!s_active) {
        return;
    }

    // 只刷新显示内容，不延长菜单自动退出时间。
    mark_dirty();
}

void quick_menu_request_full_refresh()
{
    if (!s_active) {
        return;
    }

    ++s_full_refresh_seq;
    if (s_full_refresh_seq == 0) {
        s_full_refresh_seq = 1;
    }

    mark_dirty();
}

uint32_t quick_menu_get_full_refresh_seq()
{
    return s_full_refresh_seq;
}

void quick_menu_handle_key(QuickMenuKey key)
{
    if (!s_active) {
        return;
    }

    if (key == QuickMenuKey::Confirm && confirm_guard_active()) {
        touch_menu();
        LOGW("[菜单] confirm 已忽略 by guard");
        return;
    }

    switch (key) {
        case QuickMenuKey::Up: {
            // NFC列表页只显示实际存在的条目。
            // 在当前页第一条绑定处继续向上拨动旋钮时，自动翻到上一页。
            const QuickMenuPageDef& def = current_page_def();
            if (s_page == QuickMenuPage::NfcList && def.item_count > 2 && s_selected == 1) {
                if (quick_menu_nfc_list_prev_page()) {
                    s_selected = 1;
                    touch_menu();
                    quick_menu_request_full_refresh();
                    return;
                }
            }
            move_selection(-1);
            return;
        }

        case QuickMenuKey::Down: {
            // NFC列表页有两种形态：
            // 1) 非末页：绑定数量 + 4条绑定，没有“返回”；最后一条绑定是 def.item_count - 1。
            // 2) 末页：绑定数量 + 若干绑定 + 返回；最后一条绑定是“返回”前一行。
            const QuickMenuPageDef& def = current_page_def();
            if (s_page == QuickMenuPage::NfcList && def.item_count > 1) {
                int last_binding_index = static_cast<int>(def.item_count) - 1;
                if (def.items[def.item_count - 1].type == QuickMenuItemType::Back) {
                    last_binding_index = static_cast<int>(def.item_count) - 2;
                }

                if (s_selected >= 1 && s_selected == last_binding_index) {
                    if (quick_menu_nfc_list_next_page()) {
                        s_selected = 1;
                        touch_menu();
                        quick_menu_request_full_refresh();
                        return;
                    }
                }
            }
            move_selection(+1);
            return;
        }

        case QuickMenuKey::PageUp:
            if (s_page == QuickMenuPage::NfcList) {
                if (quick_menu_nfc_list_prev_page()) {
                    s_selected = 1;
                    touch_menu();
                    quick_menu_request_full_refresh();
                } else {
                    move_selection(-1);
                }
            } else {
                move_selection(-1);
            }
            return;

        case QuickMenuKey::PageDown:
            if (s_page == QuickMenuPage::NfcList) {
                if (quick_menu_nfc_list_next_page()) {
                    s_selected = 1;
                    touch_menu();
                    quick_menu_request_full_refresh();
                } else {
                    move_selection(+1);
                }
            } else {
                move_selection(+1);
            }
            return;

        case QuickMenuKey::Confirm:
            confirm_current();
            return;

        case QuickMenuKey::Back:
            // MODE 短按返回上一级；如果已在根菜单，则退出到播放器界面。
            go_back();
            return;

        case QuickMenuKey::Exit:
            quick_menu_exit();
            return;

        default:
            return;
    }
}

QuickMenuPage quick_menu_get_page()
{
    return s_page;
}

const char* quick_menu_get_page_title()
{
    return current_page_def().title;
}

uint8_t quick_menu_get_item_count()
{
    return current_page_def().item_count;
}

int quick_menu_get_selected_index()
{
    return s_selected;
}

uint32_t quick_menu_get_revision()
{
    return s_revision;
}

bool quick_menu_get_item_view(uint8_t index, QuickMenuItemView& out)
{
    const QuickMenuPageDef& info = current_page_def();

    if (index >= info.item_count || info.items == nullptr) {
        return false;
    }

    const QuickMenuItem& item = info.items[index];

    out.label = item.get_label ? item.get_label() : (item.label ? item.label : "");
    out.value = item_value(item);
    out.type = item.type;
    out.selected = static_cast<int>(index) == s_selected;
    out.enabled = item.enabled;
    out.placeholder = item.placeholder || item.type == QuickMenuItemType::Placeholder;

    return true;
}