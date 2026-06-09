#pragma once

#include <stdint.h>

enum class QuickMenuKey : uint8_t {
    Up,
    Down,
    Confirm,
    Back,
    Exit,
};

enum class QuickMenuPage : uint8_t {
    Root,
    Playback,
    Source,
    Display,
    Network,
    AudioOutput,
    Bluetooth,
    Nfc,
    SystemInfo,
    MemoryInfo,
    StackInfo,
    BatteryInfo,
};

enum class QuickMenuItemType : uint8_t {
    SubPage,
    Action,
    Toggle,
    Status,
    Placeholder,
    Back,
};

struct QuickMenuItemView {
    const char* label = "";
    const char* value = "";
    bool selected = false;
    bool enabled = true;
    bool placeholder = false;
};

using QuickMenuValueGetter = const char* (*)();
using QuickMenuConfirmHandler = bool (*)();

struct QuickMenuItem {
    const char* label;
    QuickMenuItemType type;
    QuickMenuPage child;
    const char* static_value;
    QuickMenuValueGetter get_value;
    QuickMenuConfirmHandler on_confirm;
    bool enabled;
    bool placeholder;
};

struct QuickMenuPageDef {
    const char* title;
    QuickMenuPage page;
    QuickMenuPage parent;
    const QuickMenuItem* items;
    uint8_t item_count;
};