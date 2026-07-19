#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <vector>

/**
 * @brief NAS/HTTP 网络歌曲条目。
 *
 * 注意：encoded_path 是历史字段名。
 * net_music.txt 第二列既可以写 UTF-8 原始相对路径，也兼容旧的 %XX 编码路径；
 * 不包含 base url，真正播放时统一生成合法 HTTP URL。
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

struct NetMusicSearchHit {
  uint32_t idx = 0;
  NetMusicItem item;
};

/** NAS 曲库源。只加载当前选中源的 net_music.txt。 */
struct NetMusicSourceInfo {
  String name;
  String relative_path;
  String list_name;
  bool valid = false;
};

/**
 * @brief 搜索 NAS 歌曲。
 *
 * 会扫描 offset 索引对应的列表行，但不会常驻加载全部歌曲信息。
 * query 会匹配 title / artist / album。
 *
 * @return 实际匹配总数，不一定等于 out->size()。
 */
uint32_t net_music_catalog_search(const String& query,
                                  uint16_t limit,
                                  std::vector<NetMusicSearchHit>* out);

/**
 * @brief 加载 NAS 根地址和曲库源配置，开机阶段调用。
 *
 * 配置文件：
 * - /System/net_music_base.txt
 * - /System/net_music_sources.txt
 *
 * 这里只读取很小的配置，不下载任何歌曲列表。
 */
bool net_music_catalog_load_base();

/** 已配置的 NAS 曲库源数量。配置缺失时自动提供一个兼容旧版的根目录源。 */
uint8_t net_music_catalog_source_count();

/** 读取指定 NAS 曲库源。 */
bool net_music_catalog_source_get(uint8_t idx, NetMusicSourceInfo* out);

/** 当前选中的 NAS 曲库源索引。 */
uint8_t net_music_catalog_active_source_index();

/** 当前选中的 NAS 曲库源名称。 */
String net_music_catalog_active_source_name();

/**
 * @brief 切换 NAS 曲库源。
 *
 * 切换时会立即释放旧 net_music.txt 和偏移表，但不会同时加载其它源。
 */
bool net_music_catalog_select_source(uint8_t idx);

/**
 * @brief 从 NAS 下载 net_music.txt 到内存，并建立行偏移索引。
 *
 * 注意：
 * - 不读取 /System/net_music.txt
 * - 不写入 /System/net_music.txt
 * - 不写入 /System/net_music.tmp
 * - 打开 NAS 时不会和本地播放抢 TF 卡
 */
bool net_music_catalog_load();

/** 当前网络歌曲索引是否已加载。 */
bool net_music_catalog_is_loaded();

/** 网络歌曲数量。 */
uint32_t net_music_catalog_count();

/** 按全局 index 读取某一首歌，内部通过 offset seek，不全量加载列表。 */
bool net_music_catalog_get(uint32_t idx, NetMusicItem* out);

/** base_url + 路径；原始 UTF-8 路径会在这里编码，旧 %XX 路径不会二次编码。 */
String net_music_catalog_build_url(const NetMusicItem& item);

/** 当前选中曲库源的 base URL，末尾始终带 /。 */
String net_music_catalog_base_url();

/** 最近一次错误。 */
String net_music_catalog_error();

/** 列表来源说明；当前 NAS 列表只保存在内存中，不落盘。 */
const char* net_music_catalog_path();

/** base URL 文件路径。 */
const char* net_music_catalog_base_path();

/** NAS 曲库源配置文件路径。 */
const char* net_music_catalog_sources_path();

/** 清空索引和状态。 */
void net_music_catalog_clear();