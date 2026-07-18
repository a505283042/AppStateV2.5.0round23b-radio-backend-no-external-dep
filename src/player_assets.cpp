#include "player_assets.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdlib.h>

#include "ui/ui.h"
#include "web/web_cover_cache.h"
#include "web/web_settings.h"
#include "ui/ui_internal.h"
#include "audio/audio_service.h"
#include "audio/audio.h"
#include "lyrics/lyrics.h"
#include "utils/log.h"

static constexpr uint32_t kPlayerAssetTaskStackBytes = 6144; // 播放器资源任务栈大小：封面/歌词处理峰值较高，预留到 6KB
static constexpr const char* kDefaultCoverPath = "/System/default_cover.jpg";

#ifndef PLAYER_ASSET_TASK_PRIO // 播放器资源任务优先级
#define PLAYER_ASSET_TASK_PRIO 1 // 播放器资源任务优先级，1 表示普通任务，0 表示实时任务
#endif

static QueueHandle_t s_asset_q = nullptr; // 播放器资源任务队列句柄
static TaskHandle_t s_asset_task = nullptr; // 播放器资源任务句柄
static uint32_t s_asset_req_id = 0; // 播放器资源任务请求 ID，由短临界区保护
static portMUX_TYPE s_asset_req_mux = portMUX_INITIALIZER_UNLOCKED;
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

    // 用于 next raw 缩放后生成 webcover 的 key。
    bool has_meta = false;
    CoverSource cover_source = COVER_NONE;
    char audio_path[PLAYER_ASSET_PATH_MAX] = {0};
    char cover_path[PLAYER_ASSET_PATH_MAX] = {0};
    uint32_t cover_offset = 0;
    uint32_t cover_size = 0;
};
static PrimedCurrentCover s_primed_current_cover{};
static PrimedCurrentCover s_primed_next_cover{};
static uint32_t s_primed_next_cover_revision = 1;
// 延迟当前封面应用描述
struct DeferredCurrentCoverApply {
    bool active = false;
    int track_idx = -1;
    uint32_t due_ms = 0;
};
// 延迟当前封面应用
static DeferredCurrentCoverApply s_deferred_current_cover_apply{};

// PlayerAssetTask 与主循环会同时访问请求编号、预读封面指针和延迟应用状态。
// 使用递归互斥量保护这些复合状态，禁止仅依赖 volatile 或裸指针约定。
static StaticSemaphore_t s_asset_state_mutex_storage{};
static SemaphoreHandle_t s_asset_state_mutex = nullptr;

static void player_assets_try_scale_primed_next_cover_after_current(const PlayerDeferredAssetJob& owner_job);
static void player_assets_free_primed_cover(PrimedCurrentCover& c);

static void player_assets_state_init_once()
{
    if (!s_asset_state_mutex) {
        // 该初始化由 player_assets_setup_hooks() 在后台任务启动前完成。
        s_asset_state_mutex =
            xSemaphoreCreateRecursiveMutexStatic(&s_asset_state_mutex_storage);
    }

    if (!s_asset_state_mutex) {
        LOGE("[播放器] 创建资源状态互斥量失败");
    }
}

static bool player_assets_state_lock(TickType_t wait_ticks = portMAX_DELAY)
{
    player_assets_state_init_once();
    return s_asset_state_mutex &&
           xSemaphoreTakeRecursive(s_asset_state_mutex, wait_ticks) == pdTRUE;
}

static void player_assets_state_unlock()
{
    if (s_asset_state_mutex) {
        xSemaphoreGiveRecursive(s_asset_state_mutex);
    }
}

static uint32_t player_assets_request_id_get()
{
    portENTER_CRITICAL(&s_asset_req_mux);
    const uint32_t request_id = s_asset_req_id;
    portEXIT_CRITICAL(&s_asset_req_mux);
    return request_id;
}

static uint32_t player_assets_request_id_advance()
{
    portENTER_CRITICAL(&s_asset_req_mux);
    ++s_asset_req_id;
    if (s_asset_req_id == 0) {
        ++s_asset_req_id;
    }
    const uint32_t request_id = s_asset_req_id;
    portEXIT_CRITICAL(&s_asset_req_mux);
    return request_id;
}

static bool player_assets_deferred_apply_matches(int track_idx)
{
    if (!player_assets_state_lock()) {
        return false;
    }

    const bool matches =
        s_deferred_current_cover_apply.active &&
        s_deferred_current_cover_apply.track_idx == track_idx;
    player_assets_state_unlock();
    return matches;
}

static void player_assets_next_cover_revision_advance_locked()
{
    ++s_primed_next_cover_revision;
    if (s_primed_next_cover_revision == 0) {
        ++s_primed_next_cover_revision;
    }
}

static bool player_assets_take_primed_current_cover(int track_idx,
                                                     PrimedCurrentCover& out)
{
    out = PrimedCurrentCover{};
    if (!player_assets_state_lock()) {
        return false;
    }

    const bool matches =
        s_primed_current_cover.valid &&
        s_primed_current_cover.track_idx == track_idx &&
        s_primed_current_cover.buf &&
        s_primed_current_cover.len > 0;

    if (matches) {
        out = s_primed_current_cover;
        s_primed_current_cover = PrimedCurrentCover{};
    }

    player_assets_state_unlock();
    return matches;
}

static bool player_assets_web_cover_enabled()
{
    const WebRuntimeSettings cfg = web_settings_get();
    return cfg.wifi_enabled && cfg.show_cover;
}

static void player_assets_try_store_web_cover_from_ui_cache(int track_idx,
                                                            CoverSource cover_source,
                                                            const char* audio_path,
                                                            const char* cover_path,
                                                            uint32_t cover_offset,
                                                            uint32_t cover_size)
{
    if (track_idx < 0) {
        LOGD("[播放器] 网页封面 跳过: 无效 歌曲=%d", track_idx);
        return;
    }

    // WiFi 关闭或 Web 封面显示关闭时，不需要预生成 172KB 左右的 Web BMP 缓存。
    // 这样开机恢复只做本机屏幕封面，少占 PSRAM，也少一次 sprite->BMP 转换。
    if (!player_assets_web_cover_enabled()) {
        LOGD("[播放器] 网页封面 跳过 已禁用 歌曲=%d", track_idx);
        return;
    }

    // 已经提前预生成过 webcover，就不要再从 UI cache 转 BMP。
    // 这一步可以避免 current cover cache hit 时重复生成 172KB BMP。
    if (web_cover_cache_has(track_idx,
                        cover_source,
                        audio_path,
                        cover_path,
                        cover_offset,
                        cover_size)) {
        LOGD("[播放器] 网页封面 跳过 缓存d 歌曲=%d", track_idx);
        return;
    }

    ui_lock();

    int slot = -1;
    if (s_coverCacheReady[0] && s_coverCacheTrackIdx[0] == track_idx) {
        slot = 0;
    } else if (s_coverCacheReady[1] && s_coverCacheTrackIdx[1] == track_idx) {
        slot = 1;
    }

    LOGD("[播放器] 网页封面 来自界面缓存 歌曲=%d slot=%d 就绪0=%d idx0=%d 就绪1=%d idx1=%d",
         track_idx,
         slot,
         s_coverCacheReady[0] ? 1 : 0,
         s_coverCacheTrackIdx[0],
         s_coverCacheReady[1] ? 1 : 0,
         s_coverCacheTrackIdx[1]);

    if (slot >= 0 && s_coverCacheSpr[slot]) {
        const bool ok = web_cover_cache_store_from_sprite(track_idx,
                                                          cover_source,
                                                          audio_path,
                                                          cover_path,
                                                          cover_offset,
                                                          cover_size,
                                                          *s_coverCacheSpr[slot]);
        LOGD("[播放器] 网页封面缓存写入：歌曲=%d 槽位=%d 成功=%d", track_idx, slot, ok ? 1 : 0);
    } else {
        LOGD("[播放器] 网页封面 写入跳过 歌曲=%d slot=%d spr0=%p spr1=%p",
             track_idx, slot, s_coverCacheSpr[0], s_coverCacheSpr[1]);
    }

    ui_unlock();
}

// 检查播放器资源任务是否当前有效
static bool player_assets_is_job_current(const PlayerDeferredAssetJob& job)
{
    if (job.req_id != player_assets_request_id_get()) return false;
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
                                              kDefaultCoverPath,
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

static bool player_assets_apply_default_cover_for_current(const PlayerDeferredAssetJob& job)
{
    if (!player_assets_is_job_current(job)) {
        return false;
    }

    const bool default_ok = player_try_prepare_default_cover_cache(job.track_idx);
    if (!default_ok) {
        LOGW("[播放器] 当前 封面 默认 prepare 失败 歌曲=%d", job.track_idx);
        return false;
    }

    if (!player_assets_is_job_current(job)) {
        return false;
    }

    const bool apply_ok = ui_cover_apply_cached(job.track_idx);
    if (!apply_ok) {
        LOGW("[播放器] 当前 封面 默认 apply 失败 歌曲=%d", job.track_idx);
        return false;
    }

    if (s_hooks.on_current_cover_ready) {
        s_hooks.on_current_cover_ready(job.track_idx);
    }

    ui_request_refresh_now();

    LOGD("[播放器] 当前 封面 回退 默认 歌曲=%d", job.track_idx);
    return true;
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
            if (player_assets_deferred_apply_matches(job.track_idx)) {
                LOGD("[播放器] defer 当前 封面 apply 歌曲=%d", job.track_idx);
            } else if (ui_cover_apply_cached(job.track_idx)) {
                current_cover_cache_hit = true;

                player_assets_try_store_web_cover_from_ui_cache(job.track_idx,
                                                                job.cover_source,
                                                                job.audio_path,
                                                                job.cover_path,
                                                                job.cover_offset,
                                                                job.cover_size);

                if (s_hooks.on_current_cover_ready) {
                    s_hooks.on_current_cover_ready(job.track_idx);
                }
                ui_request_refresh_now();
                LOGD("[播放器] 当前 封面 缓存 命中 歌曲=%d", job.track_idx);
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
            if (job.cover_source == COVER_NONE) {
                // 当前歌曲明确没有封面：直接准备并应用默认封面
                current_cover_cache_hit = player_assets_apply_default_cover_for_current(job);
            } else {
                PrimedCurrentCover primed_cover{};
                if (player_assets_take_primed_current_cover(job.track_idx,
                                                            primed_cover)) {
                    cover_buf = primed_cover.buf;
                    cover_len = primed_cover.len;
                    cover_is_png = primed_cover.is_png;
                    primed_cover.buf = nullptr;

                    LOGD("[播放器] 当前封面预读命中 歌曲=%d le数量=%u",
                         job.track_idx,
                         (unsigned)cover_len);
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
        }
        t_after_fetch_cover = millis();

        if (cover_buf && cover_len > 0 && player_assets_is_job_current(job)) {
            const bool scaled_ok = ui_cover_scale_to_cache_from_buffer(cover_buf, cover_len, cover_is_png, job.track_idx);

            if (scaled_ok) {
                player_assets_try_store_web_cover_from_ui_cache(job.track_idx,
                                                                job.cover_source,
                                                                job.audio_path,
                                                                job.cover_path,
                                                                job.cover_offset,
                                                                job.cover_size);
            }

            ui_cover_free_allocated(cover_buf);
            cover_buf = nullptr;
            t_after_cover_scale = millis();

            if (scaled_ok && player_assets_is_job_current(job)) {
                if (player_assets_deferred_apply_matches(job.track_idx)) {
                    LOGD("[播放器] defer 当前 封面 apply 歌曲=%d", job.track_idx);
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

            // 当前封面读取/缩放失败，也不能继续显示上一首封面。
            // 当前任务仍有效时，回退到默认封面。
            if (job.need_cover && !current_cover_cache_hit && player_assets_is_job_current(job)) {
                current_cover_cache_hit = player_assets_apply_default_cover_for_current(job);
            }

            t_after_cover_scale = t_after_fetch_cover;
        }

        // 当前封面和当前 webcover 都处理完成后，再尝试把下一首 raw 缩放进 UI cache。
        // 这里只做下一首 UI cache，不生成 webcover，不刷新屏幕。
        player_assets_try_scale_primed_next_cover_after_current(job);

        t_after_prefetch_fetch = t_after_cover_scale;
        t_after_prefetch_scale = t_after_cover_scale;

        const char* next_prefetch_state = "none";

        const ui_player_view_t view_now = ui_get_view();
        const uint32_t play_ms_before_prefetch = audio_get_play_ms();

        const bool allow_next_prefetch =
            player_assets_is_job_current(job) &&
            (view_now == UI_VIEW_ROTATE ||
            view_now == UI_VIEW_INFO ||
            view_now == UI_VIEW_COVER_PANEL) &&
            !job.suppress_next_prefetch;

        if (allow_next_prefetch) {
            TrackInfo next_track;
            int next_track_idx = -1;

            if (!s_hooks.get_next_track_for_cover_prefetch) {
                prefetch_reason = "no_hook";
                LOGW("[播放器] 下一首 封面 prefetch 已跳过: no ho成功 歌曲=%d", job.track_idx);
            } else if (s_hooks.get_next_track_for_cover_prefetch(job.track_idx, next_track_idx, next_track)) {
                prefetched_track_idx = next_track_idx;
                if (ui_cover_cache_is_ready(next_track_idx)) {
                    prefetch_reason = "cache_hit";
                    next_prefetch_state = "ready_real";
                    LOGD("[播放器] 下一首 封面 缓存 命中 歌曲=%d", next_track_idx);
                } else if (next_track.cover_source == COVER_NONE) {
                    t_after_prefetch_fetch = millis();
                    const bool default_ok = player_try_prepare_default_cover_cache(next_track_idx);
                    t_after_prefetch_scale = millis();
                    if (default_ok) {
                        prefetch_reason = "default_ready";
                        next_prefetch_state = "ready_default";
                        LOGD("[播放器] 下一首 封面 回退 默认 歌曲=%d", next_track_idx);
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

                        if (next_ok) {
                            player_assets_try_store_web_cover_from_ui_cache(next_track_idx,
                                                                            next_track.cover_source,
                                                                            next_track.audio_path.c_str(),
                                                                            next_track.cover_path.c_str(),
                                                                            next_track.cover_offset,
                                                                            next_track.cover_size);
                        }

                        ui_cover_free_allocated(next_cover_buf);
                        next_cover_buf = nullptr;
                        t_after_prefetch_scale = millis();
                        if (next_ok) {
                            prefetch_reason = "ready";
                            next_prefetch_state = "ready_real";
                            LOGD("[播放器] 下一首 封面 prefetch 就绪 歌曲=%d", next_track_idx);
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
                        LOGD("[播放器] 下一首 封面 回退 默认 歌曲=%d", next_track_idx);
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

        if (player_assets_is_job_current(job) &&
        (view_now == UI_VIEW_ROTATE || view_now == UI_VIEW_COVER_PANEL)) {
            ui_set_rotate_wait_prefetch(false);
            LOGD("[播放器] 旋转 prefetch complete audio_ms=%lu prefetch=%s 状态=%s 下一首=%d",
                 (unsigned long)audio_get_play_ms(),
                 prefetch_reason,
                 next_prefetch_state,
                 prefetched_track_idx);
        }

        const uint32_t total_ms = t_after_prefetch_scale - t0;
        if (total_ms >= 20) {
            LOGD("[播放器] 延迟 as设置s req=%lu 总计_fetch=%lums 总计_ms=%u 歌词_fetch=%lums 歌词_parse=%lums 封面_fetch=%lums 封面_缩放=%lums 下一首_封面_fetch=%lums 下一首_封面_缩放=%lums 总计=%lums 缓存_命中=%d prefetch=%s prefetch_状态=%s 下一首=%d play_ms_before_prefetch=%lu",
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
    if (!player_assets_state_lock()) {
        return;
    }

    if (s_asset_task) {
        player_assets_state_unlock();
        return;
    }

    if (!s_asset_q) {
        s_asset_q = xQueueCreate(1, sizeof(PlayerDeferredAssetJob));
    }
    if (!s_asset_q) {
        player_assets_state_unlock();
        LOGE("[播放器] 创建延迟资源队列失败");
        return;
    }

    const BaseType_t task_ok =
        xTaskCreatePinnedToCore(player_asset_task_entry,
                                "PlayerAssetTask",
                                kPlayerAssetTaskStackBytes,
                                nullptr,
                                PLAYER_ASSET_TASK_PRIO,
                                &s_asset_task,
                                1);
    if (task_ok != pdPASS) {
        s_asset_task = nullptr;
    }
    player_assets_state_unlock();

    if (task_ok != pdPASS) {
        LOGE("[播放器] 创建资源任务失败");
    }
}
// 设置播放器资源回调
void player_assets_setup_hooks(const PlayerAssetsHooks& hooks)
{
    player_assets_state_init_once();
    if (!player_assets_state_lock()) {
        return;
    }
    s_hooks = hooks;
    player_assets_state_unlock();
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

    if (need_cover) {
        const bool has_real_cover =
            t.cover_source == COVER_FILE_FALLBACK ||
            ((t.cover_source == COVER_MP3_APIC || t.cover_source == COVER_FLAC_PICTURE) &&
            t.cover_size > 0);

        if (has_real_cover) {
            job.need_cover = true;
            job.cover_source = t.cover_source;
            job.cover_offset = t.cover_offset;
            job.cover_size = t.cover_size;

            strncpy(job.audio_path, t.audio_path.c_str(), sizeof(job.audio_path) - 1);
            job.audio_path[sizeof(job.audio_path) - 1] = '\0';

            strncpy(job.cover_path, t.cover_path.c_str(), sizeof(job.cover_path) - 1);
            job.cover_path[sizeof(job.cover_path) - 1] = '\0';
        } else if (t.cover_source == COVER_NONE) {
            // 当前歌曲明确没有封面，也要发一个 cover job，
            // 让 PlayerAssetTask 给当前 track 准备默认封面。
            job.need_cover = true;
            job.cover_source = COVER_NONE;
            job.cover_offset = 0;
            job.cover_size = 0;

            strncpy(job.audio_path, t.audio_path.c_str(), sizeof(job.audio_path) - 1);
            job.audio_path[sizeof(job.audio_path) - 1] = '\0';
            job.cover_path[0] = '\0';
        }
    }

    return job.need_total || job.need_lyrics || job.need_cover;
}
// 取消待处理的封面预取任务
void player_assets_cancel_pending_cover_prefetch()
{
    if (!player_assets_state_lock()) {
        return;
    }
    s_cover_prefetch_pending = false;
    s_cover_prefetch_not_before_ms = 0;
    s_cover_prefetch_track_idx = -1;
    s_cover_prefetch_track = TrackInfo();
    player_assets_state_unlock();
}

// 安排播放器资源请求
void player_assets_schedule(PlayerDeferredAssetJob& job)
{
    player_asset_task_start_once();
    if (!s_asset_q) {
        player_assets_reset_job(job);
        return;
    }

    job.req_id = player_assets_request_id_advance();

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
    (void)player_assets_request_id_advance();
    player_assets_discard_pending_jobs();
    player_assets_clear_primed_current_cover();
    player_assets_drop_primed_next_cover();
    player_assets_clear_deferred_current_cover_apply();
}

// 释放一个已经由当前调用方独占的封面缓冲。
static void player_assets_free_primed_cover(PrimedCurrentCover& c)
{
    if (c.buf) {
        ui_cover_free_allocated(c.buf);
        c.buf = nullptr;
    }
    c = PrimedCurrentCover{};
}

// 清除当前封面
void player_assets_clear_primed_current_cover()
{
    PrimedCurrentCover old_cover{};
    if (!player_assets_state_lock()) {
        return;
    }
    old_cover = s_primed_current_cover;
    s_primed_current_cover = PrimedCurrentCover{};
    player_assets_state_unlock();
    player_assets_free_primed_cover(old_cover);
}

// 设置当前封面
bool player_assets_prime_current_cover(int track_idx, uint8_t* buf, size_t len, bool is_png)
{
    if (!buf || len == 0 || track_idx < 0) return false;

    PrimedCurrentCover next_cover{};
    next_cover.valid = true;
    next_cover.track_idx = track_idx;
    next_cover.buf = buf;
    next_cover.len = len;
    next_cover.is_png = is_png;

    PrimedCurrentCover old_cover{};
    if (!player_assets_state_lock()) {
        return false;
    }
    old_cover = s_primed_current_cover;
    s_primed_current_cover = next_cover;
    player_assets_state_unlock();

    player_assets_free_primed_cover(old_cover);
    return true;
}

bool player_assets_prime_next_cover(const TrackInfo& t,
                                    int track_idx,
                                    uint8_t* buf,
                                    size_t len,
                                    bool is_png)
{
    if (!buf || len == 0 || track_idx < 0) {
        return false;
    }

    PrimedCurrentCover next_cover{};
    next_cover.valid = true;
    next_cover.track_idx = track_idx;
    next_cover.buf = buf;
    next_cover.len = len;
    next_cover.is_png = is_png;
    next_cover.has_meta = true;
    next_cover.cover_source = t.cover_source;

    strncpy(next_cover.audio_path,
            t.audio_path.c_str(),
            sizeof(next_cover.audio_path) - 1);
    next_cover.audio_path[sizeof(next_cover.audio_path) - 1] = '\0';

    strncpy(next_cover.cover_path,
            t.cover_path.c_str(),
            sizeof(next_cover.cover_path) - 1);
    next_cover.cover_path[sizeof(next_cover.cover_path) - 1] = '\0';

    next_cover.cover_offset = t.cover_offset;
    next_cover.cover_size = t.cover_size;

    PrimedCurrentCover old_cover{};
    if (!player_assets_state_lock()) {
        return false;
    }
    old_cover = s_primed_next_cover;
    s_primed_next_cover = next_cover;
    player_assets_next_cover_revision_advance_locked();
    player_assets_state_unlock();

    player_assets_free_primed_cover(old_cover);

    LOGD("[播放器] 下一首 封面 primed raw 歌曲=%d le数量=%u 来源=%u 大小=%u",
         track_idx,
         (unsigned)len,
         (unsigned)t.cover_source,
         (unsigned)t.cover_size);

    return true;
}

bool player_assets_promote_next_cover_to_current(int track_idx)
{
    PrimedCurrentCover old_current{};
    size_t promoted_len = 0;
    bool promoted = false;

    if (!player_assets_state_lock()) {
        return false;
    }

    // 切歌尝试本身也会使后台缩放中的旧 raw 失去恢复资格。
    player_assets_next_cover_revision_advance_locked();

    if (s_primed_next_cover.valid &&
        s_primed_next_cover.track_idx == track_idx &&
        s_primed_next_cover.buf &&
        s_primed_next_cover.len > 0) {
        old_current = s_primed_current_cover;
        s_primed_current_cover = s_primed_next_cover;
        promoted_len = s_primed_current_cover.len;
        s_primed_next_cover = PrimedCurrentCover{};
        promoted = true;
    }

    player_assets_state_unlock();
    player_assets_free_primed_cover(old_current);

    if (promoted) {
        LOGD("[播放器] 下一首 封面 已提升为当前 歌曲=%d le数量=%u",
             track_idx,
             (unsigned)promoted_len);
        return true;
    }

    // 后台任务可能刚刚完成同一首的缩放并释放 raw；此时 UI cache 已经可直接使用。
    return ui_cover_cache_is_ready(track_idx);
}

void player_assets_drop_primed_next_cover()
{
    PrimedCurrentCover old_cover{};
    if (!player_assets_state_lock()) {
        return;
    }
    old_cover = s_primed_next_cover;
    s_primed_next_cover = PrimedCurrentCover{};
    player_assets_next_cover_revision_advance_locked();
    player_assets_state_unlock();
    player_assets_free_primed_cover(old_cover);
}

static void player_assets_try_scale_primed_next_cover_after_current(const PlayerDeferredAssetJob& owner_job)
{
    if (!player_assets_is_job_current(owner_job)) {
        return;
    }

    int target_idx = -1;
    size_t target_len = 0;
    if (!player_assets_state_lock()) {
        return;
    }
    if (s_primed_next_cover.valid &&
        s_primed_next_cover.buf &&
        s_primed_next_cover.len > 0 &&
        s_primed_next_cover.track_idx >= 0) {
        target_idx = s_primed_next_cover.track_idx;
        target_len = s_primed_next_cover.len;
    }
    player_assets_state_unlock();

    if (target_idx < 0 || target_len == 0) {
        return;
    }

    if (ui_cover_cache_is_ready(target_idx)) {
        PrimedCurrentCover completed_cover{};
        if (!player_assets_state_lock()) {
            return;
        }
        if (s_primed_next_cover.valid &&
            s_primed_next_cover.track_idx == target_idx) {
            completed_cover = s_primed_next_cover;
            s_primed_next_cover = PrimedCurrentCover{};
        }
        player_assets_state_unlock();

        if (!completed_cover.valid) {
            return;
        }

        LOGD("[播放器] 跳过下一首封面缩放：目标已就绪=%d", target_idx);
        if (completed_cover.has_meta) {
            player_assets_try_store_web_cover_from_ui_cache(
                target_idx,
                completed_cover.cover_source,
                completed_cover.audio_path,
                completed_cover.cover_path,
                completed_cover.cover_offset,
                completed_cover.cover_size);
        }
        player_assets_free_primed_cover(completed_cover);
        return;
    }

    // 下一首后台缩放 + webcover 都比较吃 CPU/PSRAM，先限制小封面。
    if (target_len > 96 * 1024) {
        LOGD("[播放器] 跳过下一首封面缩放/网页缓存：目标=%d 长度过大=%u",
             target_idx,
             (unsigned)target_len);
        return;
    }

    // 当前封面和当前 webcover 完成后，再稍微让一让；等待期间不占用资源状态锁。
    vTaskDelay(pdMS_TO_TICKS(350));
    if (!player_assets_is_job_current(owner_job)) {
        return;
    }

    // 把 raw 所有权转移到任务局部变量。之后主循环可以立即发布新封面或请求清理，
    // 不需要等待耗时的图片缩放完成，也不会释放任务正在读取的指针。
    PrimedCurrentCover processing_cover{};
    uint32_t take_revision = 0;
    if (!player_assets_state_lock()) {
        return;
    }
    if (s_primed_next_cover.valid &&
        s_primed_next_cover.buf &&
        s_primed_next_cover.len > 0 &&
        s_primed_next_cover.track_idx == target_idx) {
        processing_cover = s_primed_next_cover;
        s_primed_next_cover = PrimedCurrentCover{};
        take_revision = s_primed_next_cover_revision;
    }
    player_assets_state_unlock();

    if (!processing_cover.valid) {
        return;
    }

    const uint32_t t0 = millis();
    LOGD("[播放器] 开始缩放下一首封面：目标=%d 长度=%u",
         target_idx,
         (unsigned)processing_cover.len);

    const bool scaled_ok =
        ui_cover_scale_to_cache_from_buffer(processing_cover.buf,
                                            processing_cover.len,
                                            processing_cover.is_png,
                                            target_idx);
    const uint32_t scale_cost = millis() - t0;

    LOGD("[播放器] 下一首封面缩放完成：目标=%d 成功=%d 耗时=%lums",
         target_idx,
         scaled_ok ? 1 : 0,
         (unsigned long)scale_cost);

    if (!scaled_ok) {
        bool restored = false;
        if (player_assets_is_job_current(owner_job) &&
            player_assets_state_lock()) {
            if (!s_primed_next_cover.valid &&
                s_primed_next_cover_revision == take_revision) {
                s_primed_next_cover = processing_cover;
                processing_cover = PrimedCurrentCover{};
                restored = true;
            }
            player_assets_state_unlock();
        }

        if (!restored) {
            player_assets_free_primed_cover(processing_cover);
        }
        return;
    }

    if (processing_cover.has_meta && player_assets_web_cover_enabled()) {
        const uint32_t t_web0 = millis();
        player_assets_try_store_web_cover_from_ui_cache(
            target_idx,
            processing_cover.cover_source,
            processing_cover.audio_path,
            processing_cover.cover_path,
            processing_cover.cover_offset,
            processing_cover.cover_size);
        LOGD("[播放器] 下一首网页封面已预构建：目标=%d 耗时=%lums",
             target_idx,
             (unsigned long)(millis() - t_web0));
    }

    player_assets_free_primed_cover(processing_cover);
}

// 设置延迟应用当前封面
void player_assets_set_deferred_current_cover_apply(int track_idx, uint32_t delay_ms)
{
    if (!player_assets_state_lock()) {
        return;
    }
    s_deferred_current_cover_apply.active = (track_idx >= 0);
    s_deferred_current_cover_apply.track_idx = track_idx;
    s_deferred_current_cover_apply.due_ms = millis() + delay_ms;
    player_assets_state_unlock();
}

// 清除延迟应用当前封面
void player_assets_clear_deferred_current_cover_apply()
{
    if (!player_assets_state_lock()) {
        return;
    }
    s_deferred_current_cover_apply = DeferredCurrentCoverApply{};
    player_assets_state_unlock();
}

// 尝试应用延迟应用当前封面
void player_assets_try_apply_deferred_current_cover(int current_track_idx)
{
    DeferredCurrentCoverApply pending{};
    if (!player_assets_state_lock()) {
        return;
    }
    pending = s_deferred_current_cover_apply;
    player_assets_state_unlock();

    if (!pending.active) return;
    if (current_track_idx != pending.track_idx) {
        player_assets_clear_deferred_current_cover_apply();
        return;
    }

    if ((int32_t)(millis() - pending.due_ms) < 0) {
        return;
    }

    if (ui_cover_apply_cached(current_track_idx)) {
        if (s_hooks.on_current_cover_ready) {
            s_hooks.on_current_cover_ready(current_track_idx);
        }
        LOGD("[播放器] 延迟 当前 封面 applied 歌曲=%d", current_track_idx);

        // 只清除本次已应用的请求，不能覆盖等待期间新发布的延迟请求。
        if (player_assets_state_lock()) {
            if (s_deferred_current_cover_apply.active &&
                s_deferred_current_cover_apply.track_idx == pending.track_idx &&
                s_deferred_current_cover_apply.due_ms == pending.due_ms) {
                s_deferred_current_cover_apply = DeferredCurrentCoverApply{};
            }
            player_assets_state_unlock();
        }
    }
}

void player_assets_clear_web_cover_cache()
{
    web_cover_cache_clear();
}
