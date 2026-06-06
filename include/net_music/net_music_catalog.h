#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * @brief NAS/HTTP 网络歌曲条目。
 *
 * 注意：encoded_path 是已经 URL 编码后的相对路径，
 * 不包含 base url。
 */
struct NetMusicItem {
  String title;
  String encoded_path;
  String format;
  String artist;
  String album;
  uint32_t duration_ms = 0;
  bool valid = false;
};

/** 加载 /System/net_music_base.txt 和 /System/net_music.txt，并建立行偏移索引。 */
bool net_music_catalog_load();

/** 当前网络歌曲索引是否已加载。 */
bool net_music_catalog_is_loaded();

/** 网络歌曲数量。 */
uint32_t net_music_catalog_count();

/** 按全局 index 读取某一首歌，内部通过 offset seek，不全量加载列表。 */
bool net_music_catalog_get(uint32_t idx, NetMusicItem* out);

/** base_url + encoded_path，生成最终播放 URL。 */
String net_music_catalog_build_url(const NetMusicItem& item);

/** 当前 base url。 */
String net_music_catalog_base_url();

/** 最近一次错误。 */
String net_music_catalog_error();

/** 列表文件路径。 */
const char* net_music_catalog_path();

/** base url 文件路径。 */
const char* net_music_catalog_base_path();

/** 清空索引和状态。 */
void net_music_catalog_clear();