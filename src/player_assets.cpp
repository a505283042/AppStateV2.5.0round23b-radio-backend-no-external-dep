#include "player_assets.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdlib.h>

#include "ui/ui.h"
#include "audio/audio_service.h"
#include "audio/audio.h"
#include "lyrics/lyrics.h"
#include "utils/log.h"

static constexpr uint32_t kPlayerAssetTaskStackBytes = 5120; // 播放器资源任务栈大小

#ifndef PLAYER_ASSET_TASK_PRIO // 播放器资源任务优先级
#define PLAYER_ASSET_TASK_PRIO 1 // 播放器资源任务优先级，1 表示普通任务，0 表示实时任务
#endif

static QueueHandle_t s_asset_q = nullptr; // 播放器资源任务队列句柄
static TaskHandle_t s_asset_task = nullptr; // 播放器资源任务句柄
static volatile uint32_t s_asset_req_id = 0; // 播放器资源任务请求 ID
static PlayerAssetsHooks s_hooks{}; // 播放器资源任务钩子函数

static bool s_cover_prefetch_pending = false; // 播放器封面预取任务标志
static uint32_t s_cover_prefetch_not_before_ms = 0; // 封面预取任务开始时间戳
static int s_cover_prefetch_track_idx = -1; // 封面预取任务目标索引
static TrackInfo s_cover_prefetch_track; // 封面预取任务目标轨道信息

// 当前封面缓存
struct PrimedCurrentCover {
    bool valid = false;
    int track_idx = -1;
    uint8_t* buf = nullptr;
    size_t len = 0;
    bool is_png = false;
};

static PrimedCurrentCover s_primed_current_cover{};
// 延迟当前封面应用描述
struct DeferredCurrentCoverApply {
    bool active = false;
    int track_idx = -1;
    uint32_t due_ms = 0;
};
// 延迟当前封面应用
static DeferredCurrentCoverApply s_deferred_current_cover_apply{};

// 检查播放器资源任务是否当前有效
static bool player_assets_is_job_current(const PlayerDeferredAssetJob& job)
{
    if (job.req_id != s_asset_req_id) return false;
    if (s_hooks.get_current_track_idx) {
        return s_hooks.get_current_track_idx() == job.track_idx;
    }
    return true;
}

// 尝试准备默认封面缓存
static bool player_try_prepare_default_cover_cache(int track_idx)
{
    if (track_idx < 0) return false;
    if (ui_cover_cache_is_ready(track_idx)) return true;

    uint8_t* buf = nullptr;
    size_t len = 0;
    bool is_png = false;
    const bool ok = audio_service_fetch_cover(COVER_FILE_FALLBACK,
                                              nullptr,
                                              "/System/default_cover.jpg",
                                              0,
                                              0,
                                              &buf,
                                              &len,
                                              &is_png,
                                              true);
    bool scaled_ok = false;
    if (ok && buf && len > 0) {
        scaled_ok = ui_cover_scale_to_cache_from_buffer(buf, len, is_png, track_idx);
    }
    if (buf) {
        ui_cover_free_allocated(buf);
        buf = nullptr;
    }
    return scaled_ok;
}

// 播放器资源任务入口
static void player_asset_task_entry(void*)
{
    for (;;) {
        PlayerDeferredAssetJob job{};
        if (!s_asset_q || xQueueReceive(s_asset_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!player_assets_is_job_current(job)) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
        if (!player_assets_is_job_current(job)) {
            continue;
        }

        const uint32_t t0 = millis();
        uint32_t t_after_fetch_total = t0;
        uint32_t t_after_fetch_lyrics = t0;
        uint32_t t_after_parse = t0;
        uint32_t t_after_fetch_cover = t0;
        uint32_t t_after_cover_scale = t0;
        uint32_t t_after_prefetch_fetch = t0;
        uint32_t t_after_prefetch_scale = t0;
        bool current_cover_cache_hit = false;
        int prefetched_track_idx = -1;
        const char* prefetch_reason = "none";

        if (job.need_cover) {
            if (s_deferred_current_cover_apply.active && 
                s_deferred_current_cover_apply.track_idx == job.track_idx) {
                LOGI("[PLAYER] defer current cover apply track=%d", job.track_idx);
            } else if (ui_cover_apply_cached(job.track_idx)) {
                current_cover_cache_hit = true;
                if (s_hooks.on_current_cover_ready) {
                    s_hooks.on_current_cover_ready(job.track_idx);
                }
                ui_request_refresh_now();
                LOGD("[PLAYER] current cover cache hit track=%d", job.track_idx);
            }
        }

        uint32_t fetched_total_ms = 0;
        if (job.need_total && job.audio_path[0]) {
            (void)audio_service_fetch_total_ms(job.audio_path, &fetched_total_ms, true);
            if (player_assets_is_job_current(job) && fetched_total_ms > 0) {
                audio_set_total_ms(fetched_total_ms);
                ui_request_refresh_now();
            }
        }
        t_after_fetch_total = millis();

        char* lyrics_text = nullptr;
        size_t lyrics_len = 0;
        if (job.need_lyrics && job.lyrics_path[0]) {
            (void)audio_service_fetch_lyrics(job.lyrics_path, &lyrics_text, &lyrics_len, true);
        }
        t_after_fetch_lyrics = millis();

        if (lyrics_text && lyrics_len > 0) {
            (void)g_lyricsDisplay.loadFromOwnedTextBuffer(lyrics_text, lyrics_len);
            lyrics_text = nullptr;

            if (!player_assets_is_job_current(job)) {
                g_lyricsDisplay.clear();
                continue;
            }
            ui_request_refresh_now();
        }
        t_after_parse = millis();

        uint8_t* cover_buf = nullptr;
        size_t cover_len = 0;
        bool cover_is_png = false;
        if (job.need_cover && !current_cover_cache_hit) {
            if (s_primed_current_cover.valid && 
                s_primed_current_cover.track_idx == job.track_idx && 
                s_primed_current_cover.buf && 
                s_primed_current_cover.len > 0) {
                cover_buf = s_primed_current_cover.buf;
                cover_len = s_primed_current_cover.len;
                cover_is_png = s_primed_current_cover.is_png;

                s_primed_current_cover.buf = nullptr;
                s_primed_current_cover.valid = false;
                s_primed_current_cover.track_idx = -1;
                s_primed_current_cover.len = 0;
                s_primed_current_cover.is_png = false;

                LOGI("[PLAYER] primed current cover hit track=%d len=%u", 
                     job.track_idx, (unsigned)cover_len);
            } else {
                (void)audio_service_fetch_cover(job.cover_source,
                                                job.audio_path,
                                                job.cover_path,
                                                job.cover_offset,
                                                job.cover_size,
                                                &cover_buf,
                                                &cover_len,
                                                &cover_is_png,
                                                true);
            }
        }
        t_after_fetch_cover = millis();

        if (cover_buf && cover_len > 0 && player_assets_is_job_current(job)) {
            const bool scaled_ok = ui_cover_scale_to_cache_from_buffer(cover_buf, cover_len, cover_is_png, job.track_idx);
            ui_cover_free_allocated(cover_buf);
            cover_buf = nullptr;
            t_after_cover_scale = millis();

            if (scaled_ok && player_assets_is_job_current(job)) {
                if (s_deferred_current_cover_apply.active && 
                    s_deferred_current_cover_apply.track_idx == job.track_idx) {
                    LOGI("[PLAYER] defer current cover apply track=%d", job.track_idx);
                } else {
                    (void)ui_cover_apply_cached(job.track_idx);
                    if (s_hooks.on_current_cover_ready) {
                        s_hooks.on_current_cover_ready(job.track_idx);
                    }
                    ui_request_refresh_now();
                }
            }
        } else {
            if (cover_buf) {
                ui_cover_free_allocated(cover_buf);
                cover_buf = nullptr;
            }
            t_after_cover_scale = t_after_fetch_cover;
        }

        t_after_prefetch_fetch = t_after_cover_scale;
        t_after_prefetch_scale = t_after_cover_scale;

        const char* next_prefetch_state = "none";

        const ui_player_view_t view_now = ui_get_view();
        const uint32_t play_ms_before_prefetch = audio_get_play_ms();

        const bool allow_next_prefetch = player_assets_is_job_current(job) &&
                                        (view_now == UI_VIEW_ROTATE || view_now == UI_VIEW_INFO) &&
                                        !job.suppress_next_prefetch;
        if (allow_next_prefetch) {
            TrackInfo next_track;
            int next_track_idx = -1;
            if (s_hooks.get_next_track_for_cover_prefetch &&
                s_hooks.get_next_track_for_cover_prefetch(job.track_idx, next_track_idx, next_track)) {
                prefetched_track_idx = next_track_idx;
                if (ui_cover_cache_is_ready(next_track_idx)) {
                    prefetch_reason = "cache_hit";
                    next_prefetch_state = "ready_real";
                    LOGD("[PLAYER] next cover cache hit track=%d", next_track_idx);
                } else if (next_track.cover_source == COVER_NONE) {
                    t_after_prefetch_fetch = millis();
                    const bool default_ok = player_try_prepare_default_cover_cache(next_track_idx);
                    t_after_prefetch_scale = millis();
                    if (default_ok) {
                        prefetch_reason = "default_ready";
                        next_prefetch_state = "ready_default";
                        LOGD("[PLAYER] next cover fallback default track=%d", next_track_idx);
                    } else {
                        prefetch_reason = "default_fail";
                        next_prefetch_state = "failed_retryable";
                    }
                } else if (next_track.cover_source == COVER_FILE_FALLBACK ||
                           ((next_track.cover_source == COVER_MP3_APIC || next_track.cover_source == COVER_FLAC_PICTURE) && next_track.cover_size > 0)) {
                    uint8_t* next_cover_buf = nullptr;
                    size_t next_cover_len = 0;
                    bool next_cover_is_png = false;
                    const bool fetch_ok = audio_service_fetch_cover(next_track.cover_source,
                                                                    next_track.audio_path.c_str(),
                                                                    next_track.cover_path.c_str(),
                                                                    next_track.cover_offset,
                                                                    next_track.cover_size,
                                                                    &next_cover_buf,
                                                                    &next_cover_len,
                                                                    &next_cover_is_png,
                                                                    true);
                    t_after_prefetch_fetch = millis();

                    if (fetch_ok && next_cover_buf && next_cover_len > 0 && player_assets_is_job_current(job)) {
                        const bool next_ok = ui_cover_scale_to_cache_from_buffer(next_cover_buf, next_cover_len, next_cover_is_png, next_track_idx);
                        ui_cover_free_allocated(next_cover_buf);
                        next_cover_buf = nullptr;
                        t_after_prefetch_scale = millis();
                        if (next_ok) {
                            prefetch_reason = "ready";
                            next_prefetch_state = "ready_real";
                            LOGD("[PLAYER] next cover prefetch ready track=%d", next_track_idx);
                        } else {
                            prefetch_reason = "scale_fail_retry";
                            next_prefetch_state = "failed_retryable";
                        }
                    } else {
                        if (next_cover_buf) {
                            ui_cover_free_allocated(next_cover_buf);
                            next_cover_buf = nullptr;
                        }
                        t_after_prefetch_scale = t_after_prefetch_fetch;
                        prefetch_reason = fetch_ok ? "fetch_empty_retry" : "fetch_fail_retry";
                        next_prefetch_state = "failed_retryable";
                    }
                } else {
                    t_after_prefetch_fetch = millis();
                    const bool default_ok = player_try_prepare_default_cover_cache(next_track_idx);
                    t_after_prefetch_scale = millis();
                    if (default_ok) {
                        prefetch_reason = "default_ready";
                        next_prefetch_state = "ready_default";
                        LOGD("[PLAYER] next cover fallback default track=%d", next_track_idx);
                    } else {
                        prefetch_reason = "default_fail";
                        next_prefetch_state = "failed_retryable";
                    }
                }
            } else {
                prefetch_reason = "no_next";
            }
        } else {
        if (!player_assets_is_job_current(job)) {
            prefetch_reason = "stale";
        } else if (job.suppress_next_prefetch) {
            prefetch_reason = "disabled_for_nfc_job";
        } else {
            prefetch_reason = "view_other";
        }
    }

        if (player_assets_is_job_current(job) && view_now == UI_VIEW_ROTATE) {
            ui_set_rotate_wait_prefetch(false);
            LOGD("[PLAYER] rotate prefetch complete audio_ms=%lu prefetch=%s state=%s next=%d",
                 (unsigned long)audio_get_play_ms(),
                 prefetch_reason,
                 next_prefetch_state,
                 prefetched_track_idx);
        }

        const uint32_t total_ms = t_after_prefetch_scale - t0;
        if (total_ms >= 20) {
            LOGD("[PLAYER] deferred assets req=%lu total_fetch=%lums total_ms=%u lyrics_fetch=%lums lyrics_parse=%lums cover_fetch=%lums cover_scale=%lums next_cover_fetch=%lums next_cover_scale=%lums total=%lums cache_hit=%d prefetch=%s prefetch_state=%s next=%d play_ms_before_prefetch=%lu",
                 (unsigned long)job.req_id,
                 (unsigned long)(t_after_fetch_total - t0),
                 (unsigned)fetched_total_ms,
                 (unsigned long)(t_after_fetch_lyrics - t_after_fetch_total),
                 (unsigned long)(t_after_parse - t_after_fetch_lyrics),
                 (unsigned long)(t_after_fetch_cover - t_after_parse),
                 (unsigned long)(t_after_cover_scale - t_after_fetch_cover),
                 (unsigned long)(t_after_prefetch_fetch - t_after_cover_scale),
                 (unsigned long)(t_after_prefetch_scale - t_after_prefetch_fetch),
                 (unsigned long)total_ms,
                 current_cover_cache_hit ? 1 : 0,
                 prefetch_reason,
                 next_prefetch_state,
                 prefetched_track_idx,
                 (unsigned long)play_ms_before_prefetch);
        }
    }
}

// 启动播放器资源任务
static void player_asset_task_start_once()
{
    if (s_asset_task) return;

    if (!s_asset_q) {
        s_asset_q = xQueueCreate(1, sizeof(PlayerDeferredAssetJob));
    }
    if (!s_asset_q) {
        LOGE("[PLAYER] create deferred asset queue failed");
        return;
    }

    xTaskCreatePinnedToCore(player_asset_task_entry,
                            "PlayerAssetTask",
                            kPlayerAssetTaskStackBytes,
                            nullptr,
                            PLAYER_ASSET_TASK_PRIO,
                            &s_asset_task,
                            1);
}
// 设置播放器资源回调
void player_assets_setup_hooks(const PlayerAssetsHooks& hooks)
{
    s_hooks = hooks;
}
// 重置播放器资源请求
void player_assets_reset_job(PlayerDeferredAssetJob& job)
{
    memset(&job, 0, sizeof(job));
    job.track_idx = -1;
    job.cover_source = COVER_NONE;
}
// 放弃所有待处理的播放器资源请求
void player_assets_discard_pending_jobs()
{
    if (!s_asset_q) return;

    PlayerDeferredAssetJob stale{};
    while (xQueueReceive(s_asset_q, &stale, 0) == pdTRUE) {
    }
    player_assets_clear_primed_current_cover();
    player_assets_clear_deferred_current_cover_apply();
}
// 准备播放器资源请求
bool player_assets_prepare_deferred_request(const TrackInfo& t,
                                            int current_track_idx,
                                            bool need_total,
                                            bool need_lyrics,
                                            bool need_cover,
                                            PlayerDeferredAssetJob& job)
{
    player_assets_reset_job(job);
    job.track_idx = current_track_idx;

    if (need_total && t.audio_path.length() > 0) {
        job.need_total = true;
        strncpy(job.audio_path, t.audio_path.c_str(), sizeof(job.audio_path) - 1);
        job.audio_path[sizeof(job.audio_path) - 1] = '\0';
    }

    if (need_lyrics && t.lrc_path.length() > 0) {
        job.need_lyrics = true;
        strncpy(job.lyrics_path, t.lrc_path.c_str(), sizeof(job.lyrics_path) - 1);
        job.lyrics_path[sizeof(job.lyrics_path) - 1] = '\0';
    }

    if (need_cover && (t.cover_source == COVER_FILE_FALLBACK ||
                       ((t.cover_source == COVER_MP3_APIC || t.cover_source == COVER_FLAC_PICTURE) && t.cover_size > 0))) {
        job.need_cover = true;
        job.cover_source = t.cover_source;
        job.cover_offset = t.cover_offset;
        job.cover_size = t.cover_size;
        strncpy(job.audio_path, t.audio_path.c_str(), sizeof(job.audio_path) - 1);
        job.audio_path[sizeof(job.audio_path) - 1] = '\0';
        strncpy(job.cover_path, t.cover_path.c_str(), sizeof(job.cover_path) - 1);
        job.cover_path[sizeof(job.cover_path) - 1] = '\0';
    }

    return job.need_total || job.need_lyrics || job.need_cover;
}
// 取消待处理的封面预取任务
void player_assets_cancel_pending_cover_prefetch()
{
    s_cover_prefetch_pending = false;
    s_cover_prefetch_not_before_ms = 0;
    s_cover_prefetch_track_idx = -1;
    s_cover_prefetch_track = TrackInfo();
}
// 安排待处理的封面预取任务
void player_assets_arm_pending_cover_prefetch(const TrackInfo& t, int track_idx, uint32_t delay_ms)
{
    (void)t;
    (void)track_idx;
    (void)delay_ms;
    player_assets_cancel_pending_cover_prefetch();
}
// 尝试运行待处理的封面预取任务
void player_assets_try_run_pending_cover_prefetch()
{
    return;
}
// 安排播放器资源请求
void player_assets_schedule(PlayerDeferredAssetJob& job)
{
    player_asset_task_start_once();
    if (!s_asset_q) {
        player_assets_reset_job(job);
        return;
    }

    job.req_id = ++s_asset_req_id;

    player_assets_discard_pending_jobs();
    if (xQueueOverwrite(s_asset_q, &job) != pdPASS) {
        player_assets_reset_job(job);
        return;
    }

    player_assets_reset_job(job);
}
// 使所有待处理的播放器资源请求无效
void player_assets_invalidate_requests()
{
    ++s_asset_req_id;
    player_assets_discard_pending_jobs();
    player_assets_clear_primed_current_cover();
    player_assets_clear_deferred_current_cover_apply();
}
// 释放当前封面
static void player_assets_free_primed_current_cover()
{
    if (s_primed_current_cover.buf) {
        ui_cover_free_allocated(s_primed_current_cover.buf);
        s_primed_current_cover.buf = nullptr;
    }
    s_primed_current_cover.valid = false;
    s_primed_current_cover.track_idx = -1;
    s_primed_current_cover.len = 0;
    s_primed_current_cover.is_png = false;
}
// 清除当前封面
void player_assets_clear_primed_current_cover()
{
    player_assets_free_primed_current_cover();
}
// 设置当前封面
bool player_assets_prime_current_cover(int track_idx, uint8_t* buf, size_t len, bool is_png)
{
    player_assets_free_primed_current_cover();
    if (!buf || len == 0 || track_idx < 0) return false;

    s_primed_current_cover.valid = true;
    s_primed_current_cover.track_idx = track_idx;
    s_primed_current_cover.buf = buf;
    s_primed_current_cover.len = len;
    s_primed_current_cover.is_png = is_png;
    return true;
}
// 设置延迟应用当前封面
void player_assets_set_deferred_current_cover_apply(int track_idx, uint32_t delay_ms)
{
    s_deferred_current_cover_apply.active = (track_idx >= 0);
    s_deferred_current_cover_apply.track_idx = track_idx;
    s_deferred_current_cover_apply.due_ms = millis() + delay_ms;
}
// 清除延迟应用当前封面
void player_assets_clear_deferred_current_cover_apply()
{
    s_deferred_current_cover_apply.active = false;
    s_deferred_current_cover_apply.track_idx = -1;
    s_deferred_current_cover_apply.due_ms = 0;
}
// 尝试应用延迟应用当前封面
void player_assets_try_apply_deferred_current_cover(int current_track_idx)
{
    if (!s_deferred_current_cover_apply.active) return;
    if (current_track_idx != s_deferred_current_cover_apply.track_idx) {
        player_assets_clear_deferred_current_cover_apply();
        return;
    }

    if ((int32_t)(millis() - s_deferred_current_cover_apply.due_ms) < 0) {
        return;
    }

    if (ui_cover_apply_cached(current_track_idx)) {
        if (s_hooks.on_current_cover_ready) {
            s_hooks.on_current_cover_ready(current_track_idx);
        }
        LOGI("[PLAYER] deferred current cover applied track=%d", current_track_idx);
        player_assets_clear_deferred_current_cover_apply();
    }
}


