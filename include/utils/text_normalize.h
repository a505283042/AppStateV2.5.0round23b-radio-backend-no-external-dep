#pragma once

#include <Arduino.h>

// 将字体中常见缺字的 Unicode 空白统一为 ASCII 空格，
// 并移除零宽空格/BOM/Word Joiner。返回替换或移除的字符数量。
uint32_t text_normalize_display_spaces_inplace(String& text);

// 返回规范化后的副本。
String text_normalize_display_spaces(const String& text,
                                     uint32_t* out_changed = nullptr);
