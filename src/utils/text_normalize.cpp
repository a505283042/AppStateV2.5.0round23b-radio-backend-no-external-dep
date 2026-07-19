#include "utils/text_normalize.h"

namespace {

enum class NormalizeAction : uint8_t {
    None = 0,
    ReplaceWithSpace,
    Remove,
};

static bool match2(const String& text,
                   size_t pos,
                   uint8_t a,
                   uint8_t b)
{
    return pos + 1u < text.length() &&
           (uint8_t)text[pos] == a &&
           (uint8_t)text[pos + 1u] == b;
}

static bool match3(const String& text,
                   size_t pos,
                   uint8_t a,
                   uint8_t b,
                   uint8_t c)
{
    return pos + 2u < text.length() &&
           (uint8_t)text[pos] == a &&
           (uint8_t)text[pos + 1u] == b &&
           (uint8_t)text[pos + 2u] == c;
}

static size_t normalization_token(const String& text,
                                  size_t pos,
                                  NormalizeAction* out_action)
{
    if (out_action) *out_action = NormalizeAction::None;
    if (pos >= text.length()) return 0;

    const uint8_t c = (uint8_t)text[pos];

    // 标题字段不应包含换行或制表符，统一显示为空格。
    if (c == '\t' || c == '\r' || c == '\n') {
        if (out_action) *out_action = NormalizeAction::ReplaceWithSpace;
        return 1;
    }

    // U+00A0 NO-BREAK SPACE；U+00AD SOFT HYPHEN 直接移除。
    if (match2(text, pos, 0xC2u, 0xA0u)) {
        if (out_action) *out_action = NormalizeAction::ReplaceWithSpace;
        return 2;
    }
    if (match2(text, pos, 0xC2u, 0xADu)) {
        if (out_action) *out_action = NormalizeAction::Remove;
        return 2;
    }

    // U+1680 OGHAM SPACE MARK。
    if (match3(text, pos, 0xE1u, 0x9Au, 0x80u)) {
        if (out_action) *out_action = NormalizeAction::ReplaceWithSpace;
        return 3;
    }

    if (pos + 2u < text.length() &&
        c == 0xE2u && (uint8_t)text[pos + 1u] == 0x80u) {
        const uint8_t tail = (uint8_t)text[pos + 2u];

        // U+2000..U+200A 各种排版空格。
        if (tail >= 0x80u && tail <= 0x8Au) {
            if (out_action) *out_action = NormalizeAction::ReplaceWithSpace;
            return 3;
        }

        // U+200B..U+200F：零宽空格、连接控制和方向标记，显示时移除。
        if (tail >= 0x8Bu && tail <= 0x8Fu) {
            if (out_action) *out_action = NormalizeAction::Remove;
            return 3;
        }

        // U+2028/U+2029 行、段分隔符统一为空格。
        if (tail == 0xA8u || tail == 0xA9u) {
            if (out_action) *out_action = NormalizeAction::ReplaceWithSpace;
            return 3;
        }

        // U+202A..U+202E 双向排版控制移除；U+202F 为窄不换行空格。
        if (tail >= 0xAAu && tail <= 0xAEu) {
            if (out_action) *out_action = NormalizeAction::Remove;
            return 3;
        }
        if (tail == 0xAFu) {
            if (out_action) *out_action = NormalizeAction::ReplaceWithSpace;
            return 3;
        }
    }

    if (pos + 2u < text.length() &&
        c == 0xE2u && (uint8_t)text[pos + 1u] == 0x81u) {
        const uint8_t tail = (uint8_t)text[pos + 2u];

        // U+205F MEDIUM MATHEMATICAL SPACE。
        if (tail == 0x9Fu) {
            if (out_action) *out_action = NormalizeAction::ReplaceWithSpace;
            return 3;
        }

        // U+2060..U+206F 单词连接和双向隔离控制，显示时移除。
        if (tail >= 0xA0u && tail <= 0xAFu) {
            if (out_action) *out_action = NormalizeAction::Remove;
            return 3;
        }
    }

    // U+3000 IDEOGRAPHIC SPACE。
    if (match3(text, pos, 0xE3u, 0x80u, 0x80u)) {
        if (out_action) *out_action = NormalizeAction::ReplaceWithSpace;
        return 3;
    }

    // 字符串中间的 UTF-8 BOM / U+FEFF 直接移除。
    if (match3(text, pos, 0xEFu, 0xBBu, 0xBFu)) {
        if (out_action) *out_action = NormalizeAction::Remove;
        return 3;
    }

    return 1;
}

static bool needs_normalization(const String& text)
{
    for (size_t i = 0; i < text.length();) {
        NormalizeAction action = NormalizeAction::None;
        const size_t token_bytes = normalization_token(text, i, &action);
        if (action != NormalizeAction::None) return true;
        i += token_bytes > 0u ? token_bytes : 1u;
    }
    return false;
}

} // namespace

String text_normalize_display_spaces(const String& text,
                                     uint32_t* out_changed)
{
    uint32_t changed = 0;
    String out;
    out.reserve(text.length());

    for (size_t i = 0; i < text.length();) {
        NormalizeAction action = NormalizeAction::None;
        const size_t token_bytes = normalization_token(text, i, &action);

        if (action == NormalizeAction::ReplaceWithSpace) {
            out += ' ';
            ++changed;
            i += token_bytes;
            continue;
        }
        if (action == NormalizeAction::Remove) {
            ++changed;
            i += token_bytes;
            continue;
        }

        out += text[i];
        ++i;
    }

    if (out_changed) *out_changed = changed;
    return out;
}

uint32_t text_normalize_display_spaces_inplace(String& text)
{
    // 绝大多数标题没有特殊空白；先做无分配扫描，避免列表绘制时反复申请临时 String。
    if (!needs_normalization(text)) return 0;

    uint32_t changed = 0;
    String normalized = text_normalize_display_spaces(text, &changed);
    if (changed > 0u) {
        text = normalized;
    }
    return changed;
}
