#pragma once

// TF 卡 /System 目录统一布局。
// 所有模块应引用这里的路径，避免配置、资源、索引和诊断文件再次混放。
namespace SystemPaths {

static constexpr const char* kRoot = "/System";
static constexpr const char* kConfigDir = "/System/config";
static constexpr const char* kAssetsDir = "/System/assets";
static constexpr const char* kLibraryDir = "/System/library";
static constexpr const char* kReportsDir = "/System/reports";
static constexpr const char* kCrashDir = "/System/crash";

static constexpr const char* kNetMusicBase = "/System/config/net_music_base.txt";
static constexpr const char* kNetMusicSources = "/System/config/net_music_sources.txt";
static constexpr const char* kNfcMap = "/System/config/nfc_map.txt";
static constexpr const char* kRadioList = "/System/config/radio_list.txt";

static constexpr const char* kDefaultCover = "/System/assets/default_cover.jpg";
static constexpr const char* kNetCoverLoading = "/System/assets/net_cover_loading.jpg";

static constexpr const char* kMusicIndexV3 = "/System/library/music_index_v3.bin";
static constexpr const char* kMusicManifestV1 = "/System/library/music_manifest_v1.bin";

static constexpr const char* kMusicScanReport = "/System/reports/music_scan_report.json";
static constexpr const char* kMusicScanTracks = "/System/reports/music_scan_tracks.csv";

static constexpr const char* kPanicSummary = "/System/crash/panic_summary.txt";

}  // namespace SystemPaths

// 创建分类目录，并把旧版直接位于 /System 下的文件迁移到新目录。
// 必须在 TF 卡挂载成功后调用；函数内部自行获取 SD 互斥锁。
bool storage_system_layout_prepare();
