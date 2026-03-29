#pragma once
#include <vector>
#include "storage/storage_types_v3.h"

/* 拆分多歌手字符串，如 "郭静/张韶涵/范玮琪" */
std::vector<String> storage_split_artists_v3(const String& artists_str);

/* 基于 V3 catalog 构建运行时分组 */
void storage_build_groups_v3(MusicCatalogV3& cat);