#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "storage/storage_types_v3.h"

// 初始化封面缓冲区（固定大小，避免 PSRAM 碎片）
// 在系统启动时调用一次，分配 400KB 缓冲区
bool cover_init_buffer(void);

// 把封面读到内存（buf 由内部静态缓冲提供）
// out_ptr 指向内部缓冲，下一次调用会覆盖
bool cover_load_to_memory(SdFat& sd, const TrackInfo& t, const uint8_t*& out_ptr, size_t& out_len, bool& out_is_png);

// 确保封面缓冲区足够大（内部使用）
bool cover_ensure_buffer(size_t need);

// 获取封面缓冲区指针（内部使用）
uint8_t* cover_get_buffer();

// 把封面读到一块独立分配的缓冲（调用方负责释放）
bool cover_load_alloc(SdFat& sd, const TrackInfo& t, uint8_t*& out_buf, size_t& out_len, bool& out_is_png);

// 释放 cover_load_alloc 分配的缓冲
void cover_free_alloc(uint8_t* p);
