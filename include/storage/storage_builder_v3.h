#pragma once
#include <vector>
#include "storage/storage_types_v3.h"
#include "storage/storage_scan_v3.h"

/* 从 V3 扫描临时结构直接构建 V3 Catalog */
bool storage_build_catalog_v3_from_temp(const std::vector<TrackBuildTempV3>& tracks,
                                        MusicCatalogV3& out_cat);