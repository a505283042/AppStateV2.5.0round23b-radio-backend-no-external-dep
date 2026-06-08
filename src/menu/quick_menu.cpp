#include "menu/quick_menu.h"

#include <Arduino.h>
#include "utils/log.h"

/**
 * 第一阶段只做菜单骨架：
 * - 支持进入 / 退出
 * - 支持上下选择
 * - 支持一级页面 / 二级页面
 * - 支持返回上一级
 *
 * 具体功能暂时不接入。
 * 后续再把 WiFi、播放顺序、背光、重扫、NFC 等功能逐个挂上来。
 */

namespace {

enum class MenuItemType : uint8_t {
    SubPage,
    Action,
    Toggle,
    Status,
    Placeholder,
    Back,
};

struct MenuEntry {
    const char* label;
    const char* value;
    MenuItemType type;
    QuickMenuPage child;
};

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

static constexpr uint32_t MENU_AUTO_EXIT_MS = 30000;

static bool s_active = false;
static QuickMenuPage s_page = QuickMenuPage::Root;
static int s_selected = 0;
static uint32_t s_last_action_ms = 0;

static const MenuEntry ROOT_ITEMS[] = {
    {"播放控制", "", MenuItemType::SubPage, QuickMenuPage::Playback},
    {"播放源", "", MenuItemType::SubPage, QuickMenuPage::Source},
    {"显示设置", "", MenuItemType::SubPage, QuickMenuPage::Display},
    {"网络设置", "", MenuItemType::SubPage, QuickMenuPage::Network},
    {"音频输出", "", MenuItemType::SubPage, QuickMenuPage::AudioOutput},
    {"蓝牙设置", "", MenuItemType::SubPage, QuickMenuPage::Bluetooth},
    {"NFC", "", MenuItemType::SubPage, QuickMenuPage::Nfc},
    {"系统信息", "", MenuItemType::SubPage, QuickMenuPage::SystemInfo},
    {"返回", "", MenuItemType::Back, QuickMenuPage::Root},
};

static const MenuEntry PLAYBACK_ITEMS[] = {
    {"播放顺序", "顺序/随机", MenuItemType::Toggle, QuickMenuPage::Playback},
    {"本地浏览方式", "全部/歌手/专辑", MenuItemType::Toggle, QuickMenuPage::Playback},
    {"当前源列表", "待接入", MenuItemType::Placeholder, QuickMenuPage::Playback},
    {"重扫曲库", "待接入", MenuItemType::Placeholder, QuickMenuPage::Playback},
    {"TF卡状态", "只读", MenuItemType::Status, QuickMenuPage::Playback},
    {"返回", "", MenuItemType::Back, QuickMenuPage::Root},
};

static const MenuEntry SOURCE_ITEMS[] = {
    {"本地音乐", "待接入", MenuItemType::Placeholder, QuickMenuPage::Source},
    {"网络电台", "待接入", MenuItemType::Placeholder, QuickMenuPage::Source},
    {"NAS音乐", "待接入", MenuItemType::Placeholder, QuickMenuPage::Source},
    {"返回", "", MenuItemType::Back, QuickMenuPage::Root},
};

static const MenuEntry DISPLAY_ITEMS[] = {
    {"显示类型", "待接入", MenuItemType::Placeholder, QuickMenuPage::Display},
    {"封面显示", "开/关", MenuItemType::Toggle, QuickMenuPage::Display},
    {"下一句歌词", "开/关", MenuItemType::Toggle, QuickMenuPage::Display},
    {"背光", "开/关", MenuItemType::Toggle, QuickMenuPage::Display},
    {"返回", "", MenuItemType::Back, QuickMenuPage::Root},
};

static const MenuEntry NETWORK_ITEMS[] = {
    {"WiFi", "开/关", MenuItemType::Toggle, QuickMenuPage::Network},
    {"STA状态", "只读", MenuItemType::Status, QuickMenuPage::Network},
    {"AP模式", "只读", MenuItemType::Status, QuickMenuPage::Network},
    {"当前IP", "只读", MenuItemType::Status, QuickMenuPage::Network},
    {"Web地址", "只读", MenuItemType::Status, QuickMenuPage::Network},
    {"显示WiFi信息", "开/关", MenuItemType::Toggle, QuickMenuPage::Network},
    {"重连WiFi", "待接入", MenuItemType::Placeholder, QuickMenuPage::Network},
    {"返回", "", MenuItemType::Back, QuickMenuPage::Root},
};

static const MenuEntry AUDIO_OUTPUT_ITEMS[] = {
    {"输出路径", "占位", MenuItemType::Placeholder, QuickMenuPage::AudioOutput},
    {"功放", "开/关", MenuItemType::Toggle, QuickMenuPage::AudioOutput},
    {"功放静音", "开/关", MenuItemType::Toggle, QuickMenuPage::AudioOutput},
    {"蓝牙发射", "开/关", MenuItemType::Toggle, QuickMenuPage::AudioOutput},
    {"输出测试", "占位", MenuItemType::Placeholder, QuickMenuPage::AudioOutput},
    {"返回", "", MenuItemType::Back, QuickMenuPage::Root},
};

static const MenuEntry BLUETOOTH_ITEMS[] = {
    {"蓝牙电源", "开/关", MenuItemType::Toggle, QuickMenuPage::Bluetooth},
    {"蓝牙角色查询", "占位", MenuItemType::Placeholder, QuickMenuPage::Bluetooth},
    {"输入模式查询", "占位", MenuItemType::Placeholder, QuickMenuPage::Bluetooth},
    {"蓝牙名称查询", "占位", MenuItemType::Placeholder, QuickMenuPage::Bluetooth},
    {"允许配对", "占位", MenuItemType::Placeholder, QuickMenuPage::Bluetooth},
    {"SW配对确认", "占位", MenuItemType::Placeholder, QuickMenuPage::Bluetooth},
    {"WKP休眠唤醒", "占位", MenuItemType::Placeholder, QuickMenuPage::Bluetooth},
    {"蓝牙重启", "占位", MenuItemType::Placeholder, QuickMenuPage::Bluetooth},
    {"返回", "", MenuItemType::Back, QuickMenuPage::Root},
};

static const MenuEntry NFC_ITEMS[] = {
    {"当前曲绑定NFC", "待接入", MenuItemType::Placeholder, QuickMenuPage::Nfc},
    {"当前歌手绑定NFC", "待接入", MenuItemType::Placeholder, QuickMenuPage::Nfc},
    {"当前专辑绑定NFC", "待接入", MenuItemType::Placeholder, QuickMenuPage::Nfc},
    {"NFC绑定列表", "占位", MenuItemType::Placeholder, QuickMenuPage::Nfc},
    {"清除当前绑定", "占位", MenuItemType::Placeholder, QuickMenuPage::Nfc},
    {"返回", "", MenuItemType::Back, QuickMenuPage::Root},
};

static const MenuEntry SYSTEM_INFO_ITEMS[] = {
    {"固件版本", "只读", MenuItemType::Status, QuickMenuPage::SystemInfo},
    {"内存状态", "只读", MenuItemType::Status, QuickMenuPage::SystemInfo},
    {"栈状态", "只读", MenuItemType::Status, QuickMenuPage::SystemInfo},
    {"MCP23017状态", "只读", MenuItemType::Status, QuickMenuPage::SystemInfo},
    {"I2C/SPI状态", "占位", MenuItemType::Placeholder, QuickMenuPage::SystemInfo},
    {"电池状态", "只读", MenuItemType::Status, QuickMenuPage::SystemInfo},
    {"恢复默认设置", "占位", MenuItemType::Placeholder, QuickMenuPage::SystemInfo},
    {"返回", "", MenuItemType::Back, QuickMenuPage::Root},
};

struct PageInfo {
    const char* title;
    QuickMenuPage parent;
    const MenuEntry* items;
    uint8_t count;
};

PageInfo get_page_info(QuickMenuPage page)
{
    switch (page) {
        case QuickMenuPage::Playback:
            return {"播放控制", QuickMenuPage::Root, PLAYBACK_ITEMS, MENU_COUNT(PLAYBACK_ITEMS)};

        case QuickMenuPage::Source:
            return {"播放源", QuickMenuPage::Root, SOURCE_ITEMS, MENU_COUNT(SOURCE_ITEMS)};

        case QuickMenuPage::Display:
            return {"显示设置", QuickMenuPage::Root, DISPLAY_ITEMS, MENU_COUNT(DISPLAY_ITEMS)};

        case QuickMenuPage::Network:
            return {"网络设置", QuickMenuPage::Root, NETWORK_ITEMS, MENU_COUNT(NETWORK_ITEMS)};

        case QuickMenuPage::AudioOutput:
            return {"音频输出", QuickMenuPage::Root, AUDIO_OUTPUT_ITEMS, MENU_COUNT(AUDIO_OUTPUT_ITEMS)};

        case QuickMenuPage::Bluetooth:
            return {"蓝牙设置", QuickMenuPage::Root, BLUETOOTH_ITEMS, MENU_COUNT(BLUETOOTH_ITEMS)};

        case QuickMenuPage::Nfc:
            return {"NFC", QuickMenuPage::Root, NFC_ITEMS, MENU_COUNT(NFC_ITEMS)};

        case QuickMenuPage::SystemInfo:
            return {"系统信息", QuickMenuPage::Root, SYSTEM_INFO_ITEMS, MENU_COUNT(SYSTEM_INFO_ITEMS)};

        case QuickMenuPage::Root:
        default:
            return {"快捷菜单", QuickMenuPage::Root, ROOT_ITEMS, MENU_COUNT(ROOT_ITEMS)};
    }
}

void touch_menu()
{
    s_last_action_ms = millis();
}

void open_page(QuickMenuPage page)
{
    s_page = page;
    s_selected = 0;
    touch_menu();

    const PageInfo info = get_page_info(page);
    LOGI("[MENU] open page=%s count=%u", info.title, info.count);
}

void move_selection(int delta)
{
    const PageInfo info = get_page_info(s_page);
    if (info.count == 0) {
        s_selected = 0;
        return;
    }

    s_selected += delta;

    if (s_selected < 0) {
        s_selected = info.count - 1;
    } else if (s_selected >= info.count) {
        s_selected = 0;
    }

    touch_menu();
}

void go_back()
{
    if (s_page == QuickMenuPage::Root) {
        quick_menu_exit();
        return;
    }

    open_page(get_page_info(s_page).parent);
}

void confirm_current()
{
    const PageInfo info = get_page_info(s_page);
    if (info.count == 0) {
        return;
    }

    if (s_selected < 0 || s_selected >= info.count) {
        s_selected = 0;
    }

    const MenuEntry& item = info.items[s_selected];
    touch_menu();

    switch (item.type) {
        case MenuItemType::SubPage:
            open_page(item.child);
            return;

        case MenuItemType::Back:
            go_back();
            return;

        case MenuItemType::Status:
            LOGI("[MENU] status item: %s", item.label);
            return;

        case MenuItemType::Placeholder:
            LOGW("[MENU] placeholder item: %s", item.label);
            return;

        case MenuItemType::Action:
        case MenuItemType::Toggle:
        default:
            LOGW("[MENU] item not wired yet: %s", item.label);
            return;
    }
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
    return get_page_info(s_page).title;
}

uint8_t quick_menu_get_item_count()
{
    return get_page_info(s_page).count;
}

int quick_menu_get_selected_index()
{
    return s_selected;
}

bool quick_menu_get_item_view(uint8_t index, QuickMenuItemView& out)
{
    const PageInfo info = get_page_info(s_page);

    if (index >= info.count) {
        return false;
    }

    const MenuEntry& item = info.items[index];

    out.label = item.label;
    out.value = item.value;
    out.selected = static_cast<int>(index) == s_selected;
    out.placeholder = item.type == MenuItemType::Placeholder;
    out.enabled = item.type != MenuItemType::Placeholder;

    return true;
}