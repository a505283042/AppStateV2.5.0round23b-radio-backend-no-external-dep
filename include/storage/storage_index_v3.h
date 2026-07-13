#pragma once
#include "storage/storage_types_v3.h"

/* 分配 / 释放 catalog 内存 */
void storage_catalog_v3_free(MusicCatalogV3& cat);

/* 保存 / 加载 Index V3 */
bool storage_index_save_v3(const MusicCatalogV3& cat,
                           const char* index_path = "/System/music_index_v3.bin");

bool storage_index_load_v3(MusicCatalogV3& out_cat,
                           const char* index_path = "/System/music_index_v3.bin");

/* 最近一次加载是否来自旧的无 CRC 索引，需要安全重写升级。 */
bool storage_index_last_load_needs_rewrite_v3(void);

/* 最近一次加载来源：final / tmp-recovered / bak-recovered / bak-fallback / none。 */
const char* storage_index_last_load_source_v3(void);