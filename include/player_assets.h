#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "storage/storage_types_v3.h"

#ifndef PLAYER_ASSET_PATH_MAX
#define PLAYER_ASSET_PATH_MAX 260
#endif

/**
 * @brief 播放资源补齐模块（歌词 / 总时长 / 封面 / 下一首封面预读）。
 *
 * 这个模块只负责“歌曲已经开始播放以后”的资源补齐与预取：
 * - 向 AudioTask 请求总时长、歌词文本、封面原始数据
 * - 在安全时机触发当前封面更新
 * - 管理下一首封面的延迟预读
 *
 * 它不负责：
 * - 切歌主流程
 * - playlist / 模式切换
 * - UI 页面调度
 */

/**
 * @brief 一次延迟资源请求的描述。
 *
 * track_idx / req_id 用于防止旧请求污染当前歌曲；
 * path / cover_* 信息由主播放链路在切歌时填好，避免后台任务再次直接碰业务状态。
 */
struct PlayerDeferredAssetJob {
    uint32_t req_id = 0;
    int track_idx = -1;
    bool need_total = false;
    bool need_lyrics = false;
    bool need_cover = false;
    bool suppress_next_prefetch = false;
    char lyrics_path[PLAYER_ASSET_PATH_MAX] = {0};
    char audio_path[PLAYER_ASSET_PATH_MAX] = {0};
    char cover_path[PLAYER_ASSET_PATH_MAX] = {0};
    CoverSource cover_source = COVER_NONE;
    uint32_t cover_offset = 0;
    uint32_t cover_size = 0;
};

/**
 * @brief 资源模块需要的少量回调。
 *
 * 这里故意只保留最小依赖，避免 player_assets 反向依赖整个 player_state。
 */
struct PlayerAssetsHooks {
    int (*get_current_track_idx)() = nullptr;
    bool (*get_next_track_for_cover_prefetch)(int current_idx, int& out_track_idx, TrackInfo& out_track) = nullptr;
    void (*on_current_cover_ready)(int track_idx) = nullptr;
};

/** 设置资源模块回调。通常在 player_state 初始化时调用一次。 */
void player_assets_setup_hooks(const PlayerAssetsHooks& hooks);
/** 把 job 清成空状态，避免复用旧请求内容。 */
void player_assets_reset_job(PlayerDeferredAssetJob& job);
/** 丢弃当前尚未执行的补齐 / 预读请求。通常在切歌或 stop 后调用。 */
void player_assets_discard_pending_jobs();
/**
 * @brief 根据当前 TrackInfo 组装一次延迟资源请求。
 * @return 返回 false 代表本次没有可补齐的内容，或关键信息不足。
 */
bool player_assets_prepare_deferred_request(const TrackInfo& t,
                                            int current_track_idx,
                                            bool need_total,
                                            bool need_lyrics,
                                            bool need_cover,
                                            PlayerDeferredAssetJob& job);
/** 提交一次延迟资源请求，真正执行由后台资源任务完成。 */
void player_assets_schedule(PlayerDeferredAssetJob& job);
/** 使旧请求失效，防止后台任务把旧歌资源回填到新歌上。 */
void player_assets_invalidate_requests();
/** 取消尚未开始的“下一首封面预读”。 */
void player_assets_cancel_pending_cover_prefetch();
/**
 * @brief 安排一次“下一首封面预读”。
 * @param delay_ms 延后启动时间，通常用于避开切歌刚开始的关键路径。
 */
void player_assets_arm_pending_cover_prefetch(const TrackInfo& t, int track_idx, uint32_t delay_ms);
/** 在 loop / player_state 里轮询触发，到了时机就真正启动预读。 */
void player_assets_try_run_pending_cover_prefetch();

/** 清空预装的当前歌曲封面原图。 */
void player_assets_clear_primed_current_cover();
/** 预装当前歌曲封面原图（用于 NFC 切歌前置）。 */
bool player_assets_prime_current_cover(int track_idx, uint8_t* buf, size_t len, bool is_png);

/** 预装下一首封面原图：保存 raw buffer，缩放、生成网页封面。 */
bool player_assets_prime_next_cover(const TrackInfo& t,
                                    int track_idx,
                                    uint8_t* buf,
                                    size_t len,
                                    bool is_png);

/** 如果当前 track 命中上一轮预装的 next cover，把它提升为 current cover。 */
bool player_assets_promote_next_cover_to_current(int track_idx);

/** 丢弃预装的下一首封面原图。 */
void player_assets_drop_primed_next_cover();

/** 设置当前曲封面延后应用（用于 NFC 切歌优化）。 */
void player_assets_set_deferred_current_cover_apply(int track_idx, uint32_t delay_ms);
/** 尝试应用延后的当前曲封面。 */
void player_assets_try_apply_deferred_current_cover(int current_track_idx);
/** 清空延后应用状态。 */
void player_assets_clear_deferred_current_cover_apply();

void player_assets_block_next_cover_prefetch_for(uint32_t delay_ms);

// 网页封面缓存：缓存浏览器可直接使用的 JPEG/PNG 原始字节（PSRAM）
bool player_assets_has_web_cover_blob(int track_idx,
                                      CoverSource cover_source,
                                      const char* audio_path,
                                      const char* cover_path,
                                      uint32_t cover_offset,
                                      uint32_t cover_size);

bool player_assets_copy_web_cover_blob(int track_idx,
                                       CoverSource cover_source,
                                       const char* audio_path,
                                       const char* cover_path,
                                       uint32_t cover_offset,
                                       uint32_t cover_size,
                                       uint8_t** out_buf,
                                       size_t* out_len,
                                       bool* out_is_png);

void player_assets_clear_web_cover_cache();