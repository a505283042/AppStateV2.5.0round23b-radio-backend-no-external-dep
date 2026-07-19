#pragma once

#include <stddef.h>
#include <stdint.h>

enum class LyricsTextEncoding : uint8_t {
    Utf8 = 0,
    Utf8Bom,
    Gb18030,
    UnsupportedUtf16,
    Unknown,
};

struct LyricsTextNormalizeResult {
    LyricsTextEncoding encoding = LyricsTextEncoding::Unknown;
    size_t source_bytes = 0;
    size_t utf8_bytes = 0;
    uint32_t replacement_count = 0;
    bool converted = false;
    bool stripped_bom = false;
    bool buffer_in_psram = false;
};

/**
 * @brief 将接管的歌词文本统一规范化为 UTF-8。
 *
 * 输入缓冲必须可由 free() 释放。函数成功后仍由调用方持有；转换时会释放
 * 原缓冲并替换为 PSRAM 优先的新缓冲。UTF-8 文本不会重复分配。
 */
bool lyrics_text_normalize_owned_buffer(
    char** inout_content,
    size_t* inout_len,
    LyricsTextNormalizeResult* out_result = nullptr);

const char* lyrics_text_encoding_name(LyricsTextEncoding encoding);
