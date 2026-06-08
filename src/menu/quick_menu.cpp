#include "menu/quick_menu.h"

#include <Arduino.h>

#include "menu/quick_menu_pages.h"
#include "utils/log.h"

namespace {

static constexpr uint32_t MENU_AUTO_EXIT_MS = 30000;
static constexpr uint32_t MENU_CONFIRM_GUARD_MS = 250;

static bool s_active = false;
static QuickMenuPage s_page = QuickMenuPage::Root;
static int s_selected = 0;
static uint32_t s_last_action_ms = 0;
static uint32_t s_revision = 1;
static uint32_t s_confirm_guard_until_ms = 0;

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

const QuickMenuPageDef& current_page_def()
{
    return quick_menu_get_page_def(s_page);
}

void open_page(QuickMenuPage page)
{
    s_page = page;
    s_selected = 0;

    touch_menu();
    mark_dirty();
    arm_confirm_guard();

    const QuickMenuPageDef& info = current_page_def();
    LOGI("[MENU] open page=%s count=%u", info.title, info.item_count);
}

void move_selection(int delta)
{
    const QuickMenuPageDef& info = current_page_def();
    if (info.item_count == 0) {
        s_selected = 0;
        return;
    }

    const int old_selected = s_selected;

    s_selected += delta;

    if (s_selected < 0) {
        s_selected = info.item_count - 1;
    } else if (s_selected >= info.item_count) {
        s_selected = 0;
    }

    touch_menu();

    if (s_selected != old_selected) {
        mark_dirty();
    }
}

void go_back()
{
    const QuickMenuPageDef& info = current_page_def();

    if (s_page == QuickMenuPage::Root) {
        quick_menu_exit();
        return;
    }

    open_page(info.parent);
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
            go_back();
            return;

        case QuickMenuItemType::Status:
            LOGI("[MENU] status item: %s", item->label ? item->label : "");
            mark_dirty();
            return;

        case QuickMenuItemType::Placeholder:
            LOGW("[MENU] placeholder item: %s", item->label ? item->label : "");
            mark_dirty();
            return;

        case QuickMenuItemType::Action:
        case QuickMenuItemType::Toggle:
            if (item->on_confirm != nullptr) {
                const bool changed = item->on_confirm();
                LOGI("[MENU] action item=%s changed=%d",
                     item->label ? item->label : "",
                     changed ? 1 : 0);
                mark_dirty();
                return;
            }

            LOGW("[MENU] item not wired yet: %s", item->label ? item->label : "");
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
    s_active = true;
    open_page(QuickMenuPage::Root);
    LOGI("[MENU] enter");
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

    mark_dirty();

    LOGI("[MENU] exit");
}

void quick_menu_tick()
{
    if (!s_active) {
        return;
    }

    const uint32_t now = millis();
    if (now - s_last_action_ms >= MENU_AUTO_EXIT_MS) {
        LOGI("[MENU] auto exit");
        quick_menu_exit();
    }
}

void quick_menu_handle_key(QuickMenuKey key)
{
    if (!s_active) {
        return;
    }

    if (key == QuickMenuKey::Confirm && confirm_guard_active()) {
        touch_menu();
        LOGW("[MENU] confirm ignored by guard");
        return;
    }

    switch (key) {
        case QuickMenuKey::Up:
            move_selection(-1);
            return;

        case QuickMenuKey::Down:
            move_selection(+1);
            return;

        case QuickMenuKey::Confirm:
            confirm_current();
            return;

        case QuickMenuKey::Back:
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

    out.label = item.label ? item.label : "";
    out.value = item_value(item);
    out.selected = static_cast<int>(index) == s_selected;
    out.enabled = item.enabled;
    out.placeholder = item.placeholder || item.type == QuickMenuItemType::Placeholder;

    return true;
}