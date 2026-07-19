#include "player_control.h"
#include <WiFi.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "audio/audio.h"
#include "audio/audio_output_route.h"
#include "audio/audio_service.h"
#include "app_flags.h"
#include "keys/keys.h"
#include "player_playlist.h"
#include "player_assets.h"
#include "radio/radio_catalog.h"
#include "net_music/net_music_catalog.h"
#include "net_music/net_music_embedded_cover.h"
#include "player_state.h"
#include "player_source.h"
#include "player_snapshot.h"
#include "lyrics/lyrics.h"
#include "audio/audio_radio_backend.h"
#include "storage/storage.h"
#include "storage/storage_catalog_v3.h"
#include "ui/ui.h"
#include "utils/log.h"
#include "app_diagnostics.h"
#include "hal/hall_control.h"

namespace {

PlayerControlHooks s_hooks{};

static void log_ptr_region_control(const char* label, const void* ptr, size_t bytes)
{
#if APP_DIAG_RAM_ATTRIBUTION
    LOGI("[内存归因] %s ptr=%p bytes=%lu internal=%d psram=%d",
         label,
         ptr,
         (unsigned long)bytes,
         ptr ? (esp_ptr_internal(ptr) ? 1 : 0) : 0,
         ptr ? (esp_ptr_external_ram(ptr) ? 1 : 0) : 0);
#else
    (void)label;
    (void)ptr;
    (void)bytes;
#endif
}

bool s_user_paused = false;
bool s_manual_stop_latched = false;
uint32_t s_pause_time_ms = 0;
constexpr uint32_t LOCAL_AUTO_NEXT_MIN_PLAY_MS = 1200;
constexpr uint32_t LOCAL_END_EARLY_WINDOW_MS = 3000;
constexpr uint32_t LOCAL_IO_ERROR_PROBE_GRACE_MS = 1500;

struct LocalEndRecoveryState {
    uint32_t end_serial = 0;
    uint32_t first_seen_ms = 0;
    bool storage_probe_attempted = false;
};

LocalEndRecoveryState s_local_end_recovery;

static void control_reset_local_end_recovery()
{
    s_local_end_recovery = LocalEndRecoveryState{};
}

struct RadioReturnContext {
    bool valid = false;
    int track_idx = -1;
    play_mode_t mode = PLAY_MODE_ALL_SEQ;
    int group_idx = -1;
    uint8_t volume = 0;
};

static RadioReturnContext s_radio_return;

static void player_save_radio_return_context_if_needed() {
    const PlayerSourceState source = player_source_get();
    const int cur = player_state_current_index();

    // 只在"从本地歌曲切进电台"时保存
    // 如果本来已经在电台里切台，不覆盖
    if (source.type == PlayerSourceType::NET_RADIO) {
        return;
    }
    if (cur < 0) {
        return;
    }

    s_radio_return.valid = true;
    s_radio_return.track_idx = cur;
    s_radio_return.mode = app_play_mode_get();
    s_radio_return.group_idx = player_playlist_get_current_group_idx();
    s_radio_return.volume = audio_get_volume();

    LOGD("[电台] 保存 返回 ctx 歌曲=%d 模式=%d 分组=%d vol=%u",
         cur,
         (int)s_radio_return.mode,
         s_radio_return.group_idx,
         (unsigned)s_radio_return.volume);
}

int control_current_track_idx()
{
    if (s_hooks.get_current_track_idx) return s_hooks.get_current_track_idx();
    return -1;
}

int control_track_count()
{
    if (s_hooks.get_track_count) return s_hooks.get_track_count();
    return 0;
}

bool control_play_track_dispatch(int idx, bool verbose, bool force_cover)
{
    if (idx < 0) return false;
    if (s_hooks.play_track_dispatch) {
        return s_hooks.play_track_dispatch(idx, verbose, force_cover);
    }
    return false;
}

bool control_enter_list_select_dispatch()
{
    if (s_hooks.enter_list_select) {
        return s_hooks.enter_list_select();
    }
    return false;
}

void control_update_track_pos_for_mode(int current_idx)
{
    const play_mode_t mode = app_play_mode_get();
    if (mode == PLAY_MODE_ARTIST_SEQ || mode == PLAY_MODE_ARTIST_RND ||
        mode == PLAY_MODE_ALBUM_SEQ || mode == PLAY_MODE_ALBUM_RND) {
        const PlayerPlaylistDisplayInfo display =
            player_playlist_get_display_info(current_idx, (int)storage_catalog_v3_track_count());
        ui_set_track_pos(display.display_pos, display.display_total);
    } else {
        ui_set_track_pos(current_idx, (int)storage_catalog_v3_track_count());
    }
}

void control_normalize_remote_mode_category(const char* source_label)
{
    const play_mode_t current_mode = app_play_mode_get();
    const bool random_mode = control_mode_is_random(current_mode);
    const play_mode_t normalized = random_mode ? PLAY_MODE_ALL_RND : PLAY_MODE_ALL_SEQ;
    if (current_mode == normalized) {
        return;
    }

    (void)app_play_mode_set(normalized, AppPlayModeChangeReason::RemoteNormalize);
    LOGI("[播放器] %s 仅支持全部列表，播放大类已归一为全部（%s）",
         source_label ? source_label : "网络音源",
         random_mode ? "随机" : "顺序");
}

void control_prepare_for_radio_source()
{
    net_music_embedded_cover_cancel();
    player_assets_cancel_pending_cover_prefetch();
    player_assets_invalidate_requests();
    g_lyricsDisplay.clear();
    ui_cover_cache_invalidate();
    ui_set_rotate_wait_prefetch(false);
}

static bool control_is_remote_logo(const String& s)
{
    return s.startsWith("http://") || s.startsWith("https://");
}

static bool control_apply_cover_file(const String& path)
{
    if (path.isEmpty()) return false;

    uint8_t* buf = nullptr;
    size_t len = 0;
    bool is_png = false;

    const bool ok = audio_service_fetch_cover(COVER_FILE_FALLBACK,
                                              "",
                                              path.c_str(),
                                              0,
                                              0,
                                              &buf,
                                              &len,
                                              &is_png,
                                              true);
    if (!ok || !buf || len == 0) {
        if (buf) free(buf);
        return false;
    }

    const bool scaled_ok = ui_cover_scale_from_buffer(buf, len, is_png);
    free(buf);

    if (scaled_ok) {
        ui_request_refresh_now();
    }
    return scaled_ok;
}

static void control_apply_radio_cover(const RadioItem& item)
{
    String logo = item.logo;
    logo.trim();

    if (logo.length() > 0 && !control_is_remote_logo(logo)) {
        if (control_apply_cover_file(logo)) {
            LOGD("[电台] 台标 applied: %s", logo.c_str());
            return;
        }
        LOGW("[电台] 台标 加载 失败: %s", logo.c_str());
    }

    (void)control_apply_cover_file("/System/default_cover.jpg");
}

static bool control_prepare_net_track_item(int& idx, NetMusicItem& item, String& url)
{
    if (idx < 0) return false;

    if (!net_music_catalog_is_loaded() || net_music_catalog_count() == 0) {
        LOGW("[网络歌曲] 目录 未加载 or 为空");
        return false;
    }

    idx = player_snapshot_resolve_net_track_index(idx);

    if (!net_music_catalog_get((uint32_t)idx, &item) || !item.valid) {
        LOGW("[网络歌曲] 未找到条目：索引=%d 错误=%s",
             idx,
             net_music_catalog_error().c_str());
        return false;
    }

    url = net_music_catalog_build_url(item);
    if (!url.length()) {
        LOGW("[网络歌曲] URL 构建失败：索引=%d", idx);
        return false;
    }

    return true;
}

struct NetTrackShuffleState {
    uint16_t* order = nullptr;
    uint32_t order_cap = 0;
    uint32_t pos = 0;
    uint32_t count = 0;
    bool ready = false;
};

NetTrackShuffleState s_net_track_shuffle;

int s_net_track_return_local_idx = -1;

struct NetTrackEofWatchState {
    int idx = -1;
    uint32_t last_play_ms = 0;
    uint32_t last_change_ms = 0;
    bool armed = false;
};

NetTrackEofWatchState s_net_track_eof_watch;

static constexpr uint32_t NET_TRACK_EOF_MIN_PLAY_MS = 5000;

// 没有 duration_ms 时，保留旧兜底：进度停滞 8 秒认为结束。
static constexpr uint32_t NET_TRACK_EOF_UNKNOWN_STALL_MS = 8000;

// 有 duration_ms 时，只在接近结尾时判断 EOF。
static constexpr uint32_t NET_TRACK_EOF_END_WINDOW_MS = 3000;

// 接近结尾后，播放进度停滞 1.5 秒即可切下一首。
static constexpr uint32_t NET_TRACK_EOF_KNOWN_STALL_MS = 1500;

static constexpr uint32_t NET_TRACK_START_TIMEOUT_MS = 25000;

struct NetTrackStartPending {
    uint32_t request_id = 0;
    uint32_t queued_ms = 0;
    int idx = -1;
    bool reset_shuffle = false;
};

NetTrackStartPending s_net_track_start_pending;

static void control_clear_net_track_start_pending()
{
    s_net_track_start_pending = NetTrackStartPending{};
}

// 网络歌曲异常中断时只锁定当前一次自动推进。
// 用户仍可通过“播放/下一首/上一首”主动重试，避免断网时主循环无限连接下一首。
static void control_latch_net_track_failure(const PlayerSourceState& source,
                                            const char* reason)
{
    if (source.type != PlayerSourceType::NET_TRACK) return;

    s_net_track_eof_watch.armed = false;
    control_clear_net_track_start_pending();
    net_music_embedded_cover_cancel();

    const String error = (reason && *reason)
        ? String(reason)
        : String("stream_interrupted");

    player_source_set_net_track_status(false, String("error"), error);
    ui_request_refresh_now();

    LOGW("[网络歌曲] 已停止自动下一首：索引=%d 原因=%s 播放=%lums",
         source.net_track_idx,
         error.c_str(),
         (unsigned long)audio_get_play_ms());
}

static bool control_net_track_is_near_natural_end(const PlayerSourceState& source)
{
    uint32_t duration_ms = source.net_track_duration_ms;
    if (duration_ms == 0) {
        duration_ms = audio_get_total_ms();
    }

    // 没有可靠总时长时，不把突然停止当成自然结束。
    // 这样服务器断开或 NAS 离线不会触发连续自动下一首。
    if (duration_ms == 0) return false;

    const uint32_t play_ms = audio_get_play_ms();
    return play_ms + NET_TRACK_EOF_END_WINDOW_MS >= duration_ms;
}

static bool control_is_net_track_random_mode()
{
    return control_mode_is_random(app_play_mode_get());
}

static int control_next_net_track_index_sequential(int current_idx, int step)
{
    const int count = (int)net_music_catalog_count();
    if (count <= 0) return -1;

    if (current_idx < 0 || current_idx >= count) {
        return step >= 0 ? 0 : count - 1;
    }

    int next = (current_idx + step) % count;
    if (next < 0) next += count;
    return next;
}

static void control_clear_net_track_shuffle()
{
    s_net_track_shuffle.pos = 0;
    s_net_track_shuffle.count = 0;
    s_net_track_shuffle.ready = false;
}

static bool control_reserve_net_track_shuffle_order(uint32_t required_count)
{
    if (required_count <= s_net_track_shuffle.order_cap) {
        return true;
    }

    if (required_count > UINT16_MAX) {
        LOGW("[网络歌曲] shuffle 已禁用: 数量 过大=%lu",
             (unsigned long)required_count);
        return false;
    }

    uint32_t new_cap = s_net_track_shuffle.order_cap ? s_net_track_shuffle.order_cap : 256;
    while (new_cap < required_count) {
        new_cap *= 2;
    }
    if (new_cap > UINT16_MAX) {
        new_cap = UINT16_MAX;
    }

    const size_t bytes = (size_t)new_cap * sizeof(uint16_t);
    void* p = heap_caps_realloc(s_net_track_shuffle.order,
                                bytes,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        LOGE("[网络歌曲] shuffle 顺序表 PSRAM 分配失败: 数量=%lu 字节=%lu",
             (unsigned long)new_cap,
             (unsigned long)bytes);
        return false;
    }

    s_net_track_shuffle.order = static_cast<uint16_t*>(p);
    s_net_track_shuffle.order_cap = new_cap;
    return true;
}

static int control_net_track_shuffle_index_at(uint32_t pos)
{
    if (!s_net_track_shuffle.ready || !s_net_track_shuffle.order) return -1;
    if (pos >= s_net_track_shuffle.count) return -1;
    return (int)s_net_track_shuffle.order[pos];
}

static void control_reset_net_track_shuffle(int start_idx)
{
    const uint32_t count = net_music_catalog_count();

    control_clear_net_track_shuffle();

    if (count == 0) {
        return;
    }

    if (count > 65535) {
        LOGW("[网络歌曲] shuffle 已禁用: 数量 过大=%lu",
             (unsigned long)count);
        return;
    }

    if (!control_reserve_net_track_shuffle_order(count)) {
        control_clear_net_track_shuffle();
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        s_net_track_shuffle.order[i] = (uint16_t)i;
    }

    // Fisher-Yates shuffle.
    for (int i = (int)count - 1; i > 0; --i) {
        const int j = (int)(esp_random() % (uint32_t)(i + 1));
        const uint16_t tmp = s_net_track_shuffle.order[i];
        s_net_track_shuffle.order[i] = s_net_track_shuffle.order[j];
        s_net_track_shuffle.order[j] = tmp;
    }

    // 手动选择某首歌时，把它放到本轮随机序列第一位。
    if (start_idx >= 0 && (uint32_t)start_idx < count) {
        for (uint32_t i = 0; i < count; ++i) {
            if (s_net_track_shuffle.order[i] == (uint16_t)start_idx) {
                const uint16_t tmp = s_net_track_shuffle.order[0];
                s_net_track_shuffle.order[0] = s_net_track_shuffle.order[i];
                s_net_track_shuffle.order[i] = tmp;
                break;
            }
        }
    }

    s_net_track_shuffle.pos = 0;
    s_net_track_shuffle.count = count;
    s_net_track_shuffle.ready = true;

    log_ptr_region_control("net_shuffle.order",
                           s_net_track_shuffle.order,
                           (size_t)s_net_track_shuffle.order_cap * sizeof(uint16_t));

    LOGD("[网络歌曲] shuffle re设置 method=fisher 启动=%d 数量=%lu",
         start_idx,
         (unsigned long)count);
}

static bool control_sync_net_track_shuffle_to_current(int current_idx)
{
    const uint32_t count = net_music_catalog_count();
    if (count == 0) return false;

    if (!s_net_track_shuffle.ready ||
        s_net_track_shuffle.count != count ||
        !s_net_track_shuffle.order) {
        control_reset_net_track_shuffle(current_idx);
        return s_net_track_shuffle.ready;
    }

    const int expected = control_net_track_shuffle_index_at(s_net_track_shuffle.pos);
    if (expected == current_idx) {
        return true;
    }

    // 如果当前播放 index 和随机 pos 不一致，先尝试在当前洗牌表里定位。
    for (uint32_t i = 0; i < s_net_track_shuffle.count; ++i) {
        if (s_net_track_shuffle.order[i] == (uint16_t)current_idx) {
            s_net_track_shuffle.pos = i;
            LOGD("[网络歌曲] shuffle 同步 当前=%d pos=%lu",
                 current_idx,
                 (unsigned long)i);
            return true;
        }
    }

    control_reset_net_track_shuffle(current_idx);
    return s_net_track_shuffle.ready;
}

static int control_resolve_next_net_track_index(int current_idx, int step)
{
    const int count = (int)net_music_catalog_count();
    if (count <= 0) return -1;

    if (!control_is_net_track_random_mode()) {
        return control_next_net_track_index_sequential(current_idx, step);
    }

    if (count == 1) return 0;

    if (!control_sync_net_track_shuffle_to_current(current_idx)) {
        return control_next_net_track_index_sequential(current_idx, step);
    }

    const int move_count = step >= 0 ? step : -step;
    const bool forward = step >= 0;

    for (int i = 0; i < move_count; ++i) {
        if (forward) {
            if (s_net_track_shuffle.pos + 1 >= s_net_track_shuffle.count) {
                const int avoid_idx = current_idx;

                // 一轮随机播放完，重新洗牌。
                control_reset_net_track_shuffle(-1);

                // 尽量避免新一轮第一首和刚播完的是同一首。
                if (s_net_track_shuffle.ready &&
                    s_net_track_shuffle.count > 1 &&
                    control_net_track_shuffle_index_at(0) == avoid_idx) {
                    const uint16_t tmp = s_net_track_shuffle.order[0];
                    s_net_track_shuffle.order[0] = s_net_track_shuffle.order[1];
                    s_net_track_shuffle.order[1] = tmp;
                }
            } else {
                s_net_track_shuffle.pos++;
            }
        } else {
            if (s_net_track_shuffle.pos == 0) {
                s_net_track_shuffle.pos = s_net_track_shuffle.count - 1;
            } else {
                s_net_track_shuffle.pos--;
            }
        }
    }

    const int next = control_net_track_shuffle_index_at(s_net_track_shuffle.pos);

    LOGD("[网络歌曲] shuffle resolve cur=%d step=%d pos=%lu -> %d",
         current_idx,
         step,
         (unsigned long)s_net_track_shuffle.pos,
         next);

    return next;
}

static void control_reset_net_track_eof_watch(int idx)
{
    s_net_track_eof_watch.idx = idx;
    s_net_track_eof_watch.last_play_ms = audio_get_play_ms();
    s_net_track_eof_watch.last_change_ms = millis();
    s_net_track_eof_watch.armed = true;

    LOGD("[网络歌曲] 播放结束监测已重置：索引=%d 播放=%lums",
         idx,
         (unsigned long)s_net_track_eof_watch.last_play_ms);
}

static bool control_net_track_eof_watch_triggered(const PlayerSourceState& source)
{
    if (source.type != PlayerSourceType::NET_TRACK) return false;
    if (source.net_track_idx < 0) return false;
    if (audio_service_is_paused()) return false;
    if (!audio_service_is_playing()) return false;

    const uint32_t now = millis();
    AudioNetworkStateSnapshot network{};
    if (audio_service_get_network_state(&network) && network.reconnecting) {
        // FLAC Range 续传期间播放进度会暂时停止，不能把它误判为文件自然结束。
        s_net_track_eof_watch.last_change_ms = now;
        return false;
    }
    const uint32_t play_ms = audio_get_play_ms();

    // 优先使用 NET_TRACK 元数据里的时长；如果没有，再用 audio 层总时长。
    uint32_t duration_ms = source.net_track_duration_ms;
    if (duration_ms == 0) {
        duration_ms = audio_get_total_ms();
    }

    if (!s_net_track_eof_watch.armed ||
        s_net_track_eof_watch.idx != source.net_track_idx) {
        control_reset_net_track_eof_watch(source.net_track_idx);
        return false;
    }

    if (play_ms != s_net_track_eof_watch.last_play_ms) {
        s_net_track_eof_watch.last_play_ms = play_ms;
        s_net_track_eof_watch.last_change_ms = now;
        return false;
    }

    if (play_ms < NET_TRACK_EOF_MIN_PLAY_MS) {
        return false;
    }

    const uint32_t stalled_ms = now - s_net_track_eof_watch.last_change_ms;

    if (duration_ms > 0) {
        const bool near_end =
            (play_ms + NET_TRACK_EOF_END_WINDOW_MS >= duration_ms);

        // 有总时长时，播放进度还没接近结尾，不要误判为 EOF。
        if (!near_end) {
            return false;
        }

        if (stalled_ms < NET_TRACK_EOF_KNOWN_STALL_MS) {
            return false;
        }

        LOGW("[网络歌曲] 播放结束时长监测触发：索引=%d 播放=%lums 总时长=%lums 卡住=%lums",
             source.net_track_idx,
             (unsigned long)play_ms,
             (unsigned long)duration_ms,
             (unsigned long)stalled_ms);

        s_net_track_eof_watch.armed = false;
        return true;
    }

    // 没有时长信息时，保留旧的长停滞兜底。
    if (stalled_ms < NET_TRACK_EOF_UNKNOWN_STALL_MS) {
        return false;
    }

    LOGW("[网络歌曲] 播放结束卡住监测触发：索引=%d 播放=%lums 卡住=%lums",
         source.net_track_idx,
         (unsigned long)play_ms,
         (unsigned long)stalled_ms);

    s_net_track_eof_watch.armed = false;
    return true;
}
} // namespace

static bool control_play_net_track_index_impl(int idx, bool reset_shuffle);

void player_control_setup_hooks(const PlayerControlHooks& hooks)
{
    s_hooks = hooks;
}

void player_control_reset_runtime_flags()
{
    s_user_paused = false;
    s_manual_stop_latched = false;
    s_pause_time_ms = 0;
    control_reset_local_end_recovery();
}

void player_control_on_track_started()
{
    player_control_reset_runtime_flags();
}

void player_control_mark_user_paused()
{
    s_user_paused = true;
    s_pause_time_ms = millis();
}

bool player_control_is_user_paused()
{
    return s_user_paused;
}

void player_control_mark_manual_stop()
{
    s_user_paused = true;
    s_manual_stop_latched = true;
}

bool player_control_should_block_idle()
{
    return s_manual_stop_latched && !audio_service_is_playing();
}

static bool control_poll_net_track_start()
{
    if (s_net_track_start_pending.request_id == 0) return false;

    const PlayerSourceState source = player_source_get();
    if (source.type != PlayerSourceType::NET_TRACK ||
        source.net_track_idx != s_net_track_start_pending.idx) {
        control_clear_net_track_start_pending();
        return false;
    }

    if (millis() - s_net_track_start_pending.queued_ms >= NET_TRACK_START_TIMEOUT_MS) {
        (void)audio_service_stop(false);
        control_latch_net_track_failure(source, "startup_timeout");
        return true;
    }

    AudioNetworkStateSnapshot network{};
    if (!audio_service_get_network_state(&network) ||
        network.start_request_id != s_net_track_start_pending.request_id) {
        return false;
    }

    if (network.start_phase == AudioNetworkStartPhase::Connecting) {
        if (source.net_track_state != "connecting" || !source.net_track_active) {
            player_source_set_net_track_status(true, String("connecting"), String());
            ui_request_refresh_now();
        }
        return false;
    }

    if (network.start_phase == AudioNetworkStartPhase::Playing && network.active) {
        player_source_set_net_track_status(true, String("playing"), String());
        // 列表时长优先；FLAC 列表未提供时长时保留 dr_flac 从 STREAMINFO 得到的结果。
        if (source.net_track_duration_ms > 0) {
            audio_set_total_ms(source.net_track_duration_ms);
        }

        if (s_net_track_start_pending.reset_shuffle && control_is_net_track_random_mode()) {
            control_reset_net_track_shuffle(source.net_track_idx);
        }

        control_reset_net_track_eof_watch(source.net_track_idx);
        if (source.net_track_format == "mp3" ||
            source.net_track_format == "flac") {
            net_music_embedded_cover_start(source.net_track_idx,
                                           source.net_track_url,
                                           source.net_track_format);
        } else {
            net_music_embedded_cover_cancel();
        }

        LOGI("[网络歌曲] 异步起播完成 索引=%d 标题=%s 格式=%s 时长=%lums 请求=%lu URL=%s",
             source.net_track_idx,
             source.net_track_title.c_str(),
             source.net_track_format.c_str(),
             (unsigned long)source.net_track_duration_ms,
             (unsigned long)s_net_track_start_pending.request_id,
             source.net_track_url.c_str());

        control_clear_net_track_start_pending();
        ui_request_refresh_now();
        return true;
    }

    if (network.start_phase == AudioNetworkStartPhase::Failed ||
        network.start_phase == AudioNetworkStartPhase::Cancelled) {
        const char* reason = network.error[0] ? network.error : "stream_start_failed";
        control_latch_net_track_failure(source, reason);
        return true;
    }

    return false;
}

static bool control_allow_local_auto_next_after_end(const AudioPlaybackEndState& end_state)
{
    const uint32_t now = millis();
    if (end_state.serial != s_local_end_recovery.end_serial) {
        s_local_end_recovery.end_serial = end_state.serial;
        s_local_end_recovery.first_seen_ms = now;
        s_local_end_recovery.storage_probe_attempted = false;
    }

    const uint32_t play_ms = end_state.play_ms > 0
        ? end_state.play_ms
        : audio_get_play_ms();
    const uint32_t total_ms = end_state.total_ms > 0
        ? end_state.total_ms
        : audio_get_total_ms();
    const bool ended_early = total_ms > 0 &&
        play_ms + LOCAL_END_EARLY_WINDOW_MS < total_ms;

    const bool source_io_error =
        end_state.reason == AudioPlaybackEndReason::SourceIoError;
    StorageRuntimeSnapshot storage = storage_runtime_snapshot_get();
    const bool storage_suspect = storage.recent_io_error;

    if (source_io_error || storage_suspect) {
        // 先给 TF 热插拔探测一个短窗口，避免真正拔卡时连续切歌。
        if ((uint32_t)(now - s_local_end_recovery.first_seen_ms) <
            LOCAL_IO_ERROR_PROBE_GRACE_MS) {
            return false;
        }

        if (!storage.ready) {
            return false;
        }

        if (storage.recent_io_error &&
            !s_local_end_recovery.storage_probe_attempted) {
            s_local_end_recovery.storage_probe_attempted = true;
            const uint32_t observed_generation =
                storage.io_error_generation;

            if (storage_probe_alive()) {
                const bool cleared =
                    storage_clear_io_error_if_generation(
                        observed_generation);
                storage = storage_runtime_snapshot_get();

                if (cleared) {
                    LOGW("[播放器] TF 读取异常后物理探测仍在线，已清除本次瞬时错误并跳过当前歌曲");
                } else {
                    LOGW("[播放器] TF 物理探测仍在线，但探测期间出现了新的 IO 错误，继续等待热插拔流程");
                }
            } else {
                LOGW("[播放器] TF 读取异常且物理探测失败，等待热插拔流程处理");
                return false;
            }
        }

        if (storage_runtime_snapshot_get().recent_io_error) {
            return false;
        }
    }

    if (ended_early) {
        LOGW("[播放器] 本地音频提前结束：原因=%s 播放=%lums 总时长=%lums，将跳过当前歌曲",
             audio_playback_end_reason_label(end_state.reason),
             (unsigned long)play_ms,
             (unsigned long)total_ms);
    } else {
        LOGI("[播放器] 本地音频结束：原因=%s 播放=%lums 总时长=%lums",
             audio_playback_end_reason_label(end_state.reason),
             (unsigned long)play_ms,
             (unsigned long)total_ms);
    }

    return true;
}

bool player_control_try_auto_next(bool entered, bool started)
{
    if (!entered) return false;

    if (control_poll_net_track_start()) {
        return true;
    }

    const PlayerSourceState source = player_source_get();

    AudioSeekStateSnapshot seek_state{};
    if (audio_service_get_seek_state(&seek_state) && seek_state.seeking) {
        // 跳转会清空旧 PCM 并可能重开 HTTP Range，此时进度短暂停止不是 EOF。
        if (source.type == PlayerSourceType::NET_TRACK && source.net_track_idx >= 0) {
            control_reset_net_track_eof_watch(source.net_track_idx);
        }
        return false;
    }

    // NAS FLAC 由 HTTP Range 音源自行执行断流续传。续传期间保持当前歌曲，
    // 禁止播放器层因为 WiFi 短暂断开而提前 stop，也禁止误判为自然播放结束。
    if (source.type == PlayerSourceType::NET_TRACK &&
        source.net_track_format == "flac") {
        AudioNetworkStateSnapshot network{};
        if (audio_service_get_network_state(&network)) {
            if (network.reconnecting) {
                if (source.net_track_state != "reconnecting" || !source.net_track_active) {
                    player_source_set_net_track_status(true, String("reconnecting"), String());
                    ui_request_refresh_now();
                }
                return false;
            }

            if (source.net_track_state == "reconnecting" &&
                network.active && audio_service_is_playing()) {
                player_source_set_net_track_status(true, String("playing"), String());
                control_reset_net_track_eof_watch(source.net_track_idx);
                ui_request_refresh_now();
            }
        }
    }

    // NAS MP3 与 FLAC 都由各自的 Range 音源执行断流续传。
    // 播放器层不能在 WiFi 短暂断开时主动 stop，否则会取消音源内部的重连操作。

    if (s_user_paused) return false;

    // 网络电台是连续流，不做自动下一台。
    if (source.type == PlayerSourceType::NET_RADIO) {
        return false;
    }

    // NAS/HTTP 网络歌曲：先处理 URLStream 不报 EOF 的情况。
    if (source.type == PlayerSourceType::NET_TRACK) {
        // 异步 HTTP 起播尚未完成时，不能把“当前还没播放”误判成中途断流。
        if (source.net_track_state == "connecting") {
            return false;
        }

        // 起播失败、断网或流异常已经写入 error 状态时，禁止自动推进。
        // 手动播放/下一首仍会重新进入 control_play_net_track_index_impl()，不受此保护影响。
        if (!source.net_track_active ||
            source.net_track_state == "error" ||
            source.net_track_state == "stopped") {
            return false;
        }

        bool should_advance = false;

        if (!audio_service_is_playing()) {
            AudioNetworkStateSnapshot network{};
            (void)audio_service_get_network_state(&network);

            const bool flac_retry_exhausted =
                source.net_track_format == "flac" &&
                strcmp(network.error, "flac_reconnect_exhausted") == 0;
            const bool mp3_retry_exhausted =
                source.net_track_format == "mp3" &&
                strcmp(network.error, "mp3_reconnect_exhausted") == 0;

            const AudioPlaybackEndState end_state = audio_get_last_end_state();
            const bool natural_eof =
                end_state.reason == AudioPlaybackEndReason::NaturalEof;

            // NAS MP3/FLAC 已完成 1/2/4/8/15 秒全部续传尝试后，才允许跳过当前曲目。
            // 普通瞬时断流先由音源内部续传，避免直接停住或连续扫完整个列表。
            if (flac_retry_exhausted || mp3_retry_exhausted) {
                LOGW("[网络歌曲] NAS %s 续传失败达到上限，跳过当前歌曲：索引=%d",
                     source.net_track_format.c_str(),
                     source.net_track_idx);
                should_advance = true;
            } else if (natural_eof) {
                if (!control_net_track_is_near_natural_end(source)) {
                    LOGW("[网络歌曲] HTTP 文件已完整读取但早于列表时长结束：索引=%d 播放=%lums 列表=%lums，将自动下一首",
                         source.net_track_idx,
                         (unsigned long)end_state.play_ms,
                         (unsigned long)source.net_track_duration_ms);
                }
                should_advance = true;
            } else if (!control_net_track_is_near_natural_end(source)) {
                const char* reason = "stream_interrupted";
                if (network.error[0] != '\0') {
                    reason = network.error;
                } else if (network.waiting_for_data) {
                    reason = "stream_timeout";
                }

                control_latch_net_track_failure(source, reason);
                return false;
            } else {
                should_advance = true;
            }
        } else if (control_net_track_eof_watch_triggered(source)) {
            // URLStream 对普通 HTTP 文件播完后可能不返回 EOF，
            // 这里主动停掉当前流，再切下一首。
            audio_service_stop(true);
            should_advance = true;
        }

        if (!should_advance) {
            return false;
        }

        if (!net_music_catalog_is_loaded() || net_music_catalog_count() == 0) {
            LOGW("[网络歌曲] auto 下一首 被阻止: 目录 为空");
            return false;
        }

        const StorageRuntimeSnapshot storage =
            storage_runtime_snapshot_get();
        if (!storage.ready || storage.recent_io_error) {
            LOGW("[网络歌曲] auto 下一首 被阻止: storage 未就绪 or IO error pending");
            return false;
        }

        const int next = control_resolve_next_net_track_index(source.net_track_idx, +1);
        if (next < 0) {
            LOGW("[网络歌曲] auto 下一首 失败: 无效 下一首 index");
            return false;
        }

        LOGD("[网络歌曲] 自动下一首 %d -> %d", source.net_track_idx, next);
        return control_play_net_track_index_impl(next, false);
    }

    // 本地歌曲仍然保留原来的 started 保护。
    if (!started) return false;
    if (audio_service_is_playing()) {
        control_reset_local_end_recovery();
        return false;
    }

    const AudioPlaybackEndState end_state = audio_get_last_end_state();

    // 从网络流切回本地时，解码器/I2S 刚复位的瞬间可能短暂显示 not playing。
    // 现在只有 AudioTask 已发布明确结束事件时才允许推进；没有结束事件仍保留最短播放保护。
    const uint32_t local_play_ms = audio_get_play_ms();
    if (end_state.reason == AudioPlaybackEndReason::None &&
        local_play_ms < LOCAL_AUTO_NEXT_MIN_PLAY_MS) {
        LOGW("[播放器] 自动下一首已抑制：尚无结束事件 play_ms=%lu 来源=%s",
             (unsigned long)local_play_ms,
             player_source_type_key(source.type));
        return false;
    }

    if (end_state.reason == AudioPlaybackEndReason::None ||
        end_state.reason == AudioPlaybackEndReason::Stopped) {
        // 没有明确结束事件，或属于显式停止/切换音源/关机，均不能自动下一首。
        return false;
    }

    const int track_count = control_track_count();
    if (track_count <= 0) return false;

    if (!control_allow_local_auto_next_after_end(end_state)) {
        return false;
    }

    if (!storage_is_ready()) {
        LOGW("[播放器] 本地自动下一首被阻止：TF 未就绪");
        return false;
    }

    const int cur = control_current_track_idx();
    int next = 0;
    bool anchored = false;
    if (!player_playlist_resolve_step(cur, +1, next, &anchored)) {
        return false;
    }

    if (anchored) {
        LOGW("[播放器] AUTO NEXT 锚定到播放列表开头, 模式=%d 分组=%d cur=%d",
             (int)app_play_mode_get(),
             player_playlist_get_current_group_idx(),
             cur);
    }

    return control_play_track_dispatch(next, false, true);
}


bool player_play_radio_index(int idx)
{
    if (idx < 0) return false;
    const RadioItem* item = radio_catalog_get((size_t)idx);
    if (!item || !item->valid) return false;

    // 进入电台前先把当前本地或 NAS 状态保存到各自内存快照；电台不会覆盖这两套状态。
    (void)player_snapshot_capture_current_source();

    // 保留旧返回上下文作为兼容回退。
    player_save_radio_return_context_if_needed();

    if (player_source_type_get() == PlayerSourceType::NET_RADIO) {
        audio_radio_backend_stop();
    }
    if (audio_service_is_playing() || audio_service_is_paused()) {
        audio_service_stop(true);
    }
    control_prepare_for_radio_source();
    control_normalize_remote_mode_category("网络收音机");

    player_source_set_radio_stub(idx, *item, String("connecting"), String());
    player_state_set_current_index(-1);
    player_control_reset_runtime_flags();

    ui_set_now_playing(item->name.c_str(), "网络电台");
    ui_set_album(item->region);
    ui_set_track_pos(idx, (int)radio_catalog_count());
    control_apply_radio_cover(*item);
    ui_request_refresh_now();

    const bool ok = audio_radio_backend_start(*item);
    if (ok) {
        player_source_set_radio_status(true, String("connecting"), String());
        player_source_set_radio_runtime(audio_radio_backend_name(),
                                        item->name,
                                        String(),
                                        0,
                                        "connecting",
                                        true,
                                        String());
        LOGI("[电台] 播放电台 索引=%d 名称=%s 后端=%s", idx, item->name.c_str(), audio_radio_backend_name());
        return true;
    }

    const RadioBackendStatus backend_status = audio_radio_backend_get_status();
    const String error = backend_status.error.length()
        ? backend_status.error
        : String("backend_start_failed");
    player_source_set_radio_status(false, String("error"), error);
    LOGW("[电台] 播放失败：索引=%d 名称=%s 原因=%s",
         idx,
         item->name.c_str(),
         error.c_str());
    return false;
}

void player_stop_radio()
{
    audio_radio_backend_stop();
    player_source_clear_radio();
}

bool player_return_from_radio_to_local() {
    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::NET_RADIO) {
        audio_radio_backend_stop();
    } else if (source.type == PlayerSourceType::NET_TRACK) {
        audio_service_stop(true);
    } else {
        return false;
    }

    // 网络流刚关闭后给 AudioTask / WiFiClient / I2S DMA 一个很短的收尾窗口，
    // 避免立刻打开本地文件时误触发 EOF 或自动下一首。
    delay(30);

    (void)player_snapshot_apply_local_context();

    int target = player_snapshot_local_track_index();
    if (target < 0 && s_radio_return.valid) {
        target = s_radio_return.track_idx;
    }
    if (target < 0) {
        LOGW("[电台] 没有可恢复的本地快照");
        return false;
    }

    const bool ok = player_play_idx_v3((uint32_t)target, true, true);
    if (!ok) {
        LOGW("[电台] 恢复本地歌曲失败：索引=%d", target);
        return false;
    }

    LOGD("[电台] 已恢复本地歌曲：索引=%d", target);
    return true;
}

bool player_return_from_network_to_local()
{
    const PlayerSourceState source = player_source_get();

    if (source.type == PlayerSourceType::NET_RADIO) {
        return player_return_from_radio_to_local();
    }

    if (source.type != PlayerSourceType::NET_TRACK) {
        LOGW("[网络歌曲] 返回 本地 已忽略: 来源=%s",
             player_source_type_key(source.type));
        return false;
    }

    const int track_count = control_track_count();
    if (track_count <= 0) {
        LOGW("[网络歌曲] 返回 本地 失败: no 本地 歌曲s");
        return false;
    }

    // 切回本地前先捕获当前 NAS 曲目和 NAS 播放模式。
    (void)player_snapshot_capture_current_source();
    (void)player_snapshot_apply_local_context();

    int target = player_snapshot_local_track_index();
    if (target < 0) {
        target = s_net_track_return_local_idx;
    }

    if (target < 0 || target >= track_count) {
        target = 0;
        LOGW("[网络歌曲] 返回本地没有有效快照，回退到 idx=0");
    }

    LOGD("[网络歌曲] 返回本地目标=%d", target);

    control_clear_net_track_start_pending();
    audio_service_stop(true);

    // 清掉 NET_TRACK，避免 EOF watchdog 或自动下一首继续把 NAS 歌曲拉起来。
    player_source_clear_net_track();

    // 网络 HTTP 文件切回本地时同样留一个短收尾窗口。
    delay(30);

    return control_play_track_dispatch(target, false, true);
}

bool player_net_track_toggle_order_random()
{
    const PlayerSourceState source = player_source_get();
    if (source.type != PlayerSourceType::NET_TRACK) {
        return false;
    }

    const play_mode_t next_mode = control_is_net_track_random_mode()
        ? PLAY_MODE_ALL_SEQ
        : PLAY_MODE_ALL_RND;
    (void)app_play_mode_set(next_mode, AppPlayModeChangeReason::PlayerControl);
    LOGD("[网络歌曲] 模式 -> %s",
         next_mode == PLAY_MODE_ALL_RND ? "all_rnd" : "all_seq");
    ui_request_refresh_now();
    return true;
}

static bool control_play_net_track_index_impl(int idx, bool reset_shuffle)
{
    NetMusicItem item{};
    String url;

    if (!control_prepare_net_track_item(idx, item, url)) {
        return false;
    }

    const PlayerSourceState before_source = player_source_get();

    if (before_source.type != PlayerSourceType::NET_TRACK) {
        // 从本地/电台进入 NAS 前，先保留原音源状态，再恢复 NAS 自己的顺序/随机模式。
        (void)player_snapshot_capture_current_source();
        (void)player_snapshot_apply_net_context();
    }

    if (before_source.type == PlayerSourceType::LOCAL_TRACK) {
        const int cur = control_current_track_idx();
        if (cur >= 0 && cur < control_track_count()) {
            s_net_track_return_local_idx = cur;
            LOGD("[网络歌曲] 记住本地返回位置：索引=%d", s_net_track_return_local_idx);
        }
    }

    if (player_source_type_get() == PlayerSourceType::NET_RADIO) {
        audio_radio_backend_stop();
    }

    if (audio_service_is_playing() || audio_service_is_paused()) {
        audio_service_stop(true);
    }

    control_prepare_for_radio_source();
    control_normalize_remote_mode_category("NAS播放");

    player_source_set_net_track_stub(idx, item, url, String("connecting"), String());
    player_state_set_current_index(-1);
    player_control_reset_runtime_flags();

    // 新流的播放命令会在 AudioTask 内复位暂停状态并按安全时序打开功放。
    ui_set_now_playing(item.title.c_str(), item.artist.c_str());
    ui_set_album(item.album);
    ui_set_track_pos(idx, (int)net_music_catalog_count());
    ui_set_play_mode(app_play_mode_get());
    audio_output_route_sync_ui_volume();


    // MP3 后台会继续探测 APIC，因此先显示网络封面加载图。
    // NAS FLAC 起播先使用默认封面，后台找到 PICTURE 后再无阻塞替换。
    if (item.format == "flac") {
        (void)control_apply_cover_file("/System/default_cover.jpg");
    } else if (!control_apply_cover_file("/System/net_cover_loading.jpg")) {
        (void)control_apply_cover_file("/System/default_cover.jpg");
    }
    ui_request_refresh_now();

    uint32_t request_id = 0;
    const bool queued = item.format == "flac"
        ? audio_service_play_stream_flac_async(url.c_str(), &request_id)
        : audio_service_play_stream_mp3_auto_offset_async(url.c_str(), &request_id);
    if (!queued || request_id == 0) {
        player_source_set_net_track_status(false, String("error"), String("queue_failed"));
        LOGW("[网络歌曲] 起播请求入队失败：索引=%d 标题=%s URL=%s",
             idx,
             item.title.c_str(),
             url.c_str());
        s_net_track_eof_watch.armed = false;
        control_clear_net_track_start_pending();
        return false;
    }

    s_net_track_start_pending.request_id = request_id;
    s_net_track_start_pending.queued_ms = millis();
    s_net_track_start_pending.idx = idx;
    s_net_track_start_pending.reset_shuffle = reset_shuffle;

    player_source_set_net_track_status(true, String("connecting"), String());
    LOGI("[网络歌曲] 起播请求已入队：索引=%d 标题=%s 格式=%s 请求=%lu URL=%s",
         idx,
         item.title.c_str(),
         item.format.c_str(),
         (unsigned long)request_id,
         url.c_str());
    return true;
}

bool player_play_net_track_index(int idx)
{
    return control_play_net_track_index_impl(idx, true);
}

void player_stop_net_track()
{
    (void)player_snapshot_capture_current_source();
    control_clear_net_track_start_pending();
    net_music_embedded_cover_cancel();
    audio_service_stop(true);
    player_source_clear_net_track();
}

void player_next_track()
{
    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::NET_RADIO) {
        const int count = (int)radio_catalog_count();
        if (count <= 0) return;

        int next_radio = source.radio_idx >= 0 ? (source.radio_idx + 1) % count : 0;

        ui_notify_cover_panel_nav_feedback(1);

        const bool ok = player_play_radio_index(next_radio);
        if (!ok) {
            ui_notify_cover_panel_nav_feedback(0);
        }

        return;
    }

    if (source.type == PlayerSourceType::NET_TRACK) {
        const int next = control_resolve_next_net_track_index(source.net_track_idx, +1);
        if (next < 0) return;

        ui_notify_cover_panel_nav_feedback(1);

        const bool ok = control_play_net_track_index_impl(next, false);
        if (!ok) {
            ui_notify_cover_panel_nav_feedback(0);
        }

        return;
    }

    const int total = control_track_count();
    if (total <= 0) return;

    const int cur = control_current_track_idx();
    int next = 0;
    bool anchored = false;
    if (!player_playlist_resolve_step(cur, +1, next, &anchored)) {
        return;
    }

    if (anchored) {
        LOGW("[播放器] NEXT 锚定到播放列表开头, 模式=%d 分组=%d cur=%d",
             (int)app_play_mode_get(), player_playlist_get_current_group_idx(), cur);
    }

    LOGI("[播放器] 下一首 -> #%d", next);

    ui_notify_cover_panel_nav_feedback(1);

    const bool ok = control_play_track_dispatch(next, false, true);
    if (!ok) {
        ui_notify_cover_panel_nav_feedback(0);
    }
}

void player_prev_track()
{
    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::NET_RADIO) {
        const int count = (int)radio_catalog_count();
        if (count <= 0) return;

        int prev_radio = source.radio_idx >= 0 ? (source.radio_idx - 1 + count) % count : 0;

        ui_notify_cover_panel_nav_feedback(-1);

        const bool ok = player_play_radio_index(prev_radio);
        if (!ok) {
            ui_notify_cover_panel_nav_feedback(0);
        }

        return;
    }

    if (source.type == PlayerSourceType::NET_TRACK) {
        const int prev = control_resolve_next_net_track_index(source.net_track_idx, -1);
        if (prev < 0) return;

        ui_notify_cover_panel_nav_feedback(-1);

        const bool ok = control_play_net_track_index_impl(prev, false);
        if (!ok) {
            ui_notify_cover_panel_nav_feedback(0);
        }

        return;
    }

    const int total = control_track_count();
    if (total <= 0) return;

    const int cur = control_current_track_idx();
    int prev = 0;
    bool anchored = false;
    if (!player_playlist_resolve_step(cur, -1, prev, &anchored)) {
        return;
    }

    if (anchored) {
        LOGW("[播放器] PREV 锚定到播放列表末尾, 模式=%d 分组=%d cur=%d",
             (int)app_play_mode_get(), player_playlist_get_current_group_idx(), cur);
    }

    LOGI("[播放器] 上一首 -> #%d", prev);

    ui_notify_cover_panel_nav_feedback(-1);

    const bool ok = control_play_track_dispatch(prev, false, true);
    if (!ok) {
        ui_notify_cover_panel_nav_feedback(0);
    }
}

static const char* control_toggle_trigger_label(PlayerToggleTrigger trigger)
{
    switch (trigger) {
        case PlayerToggleTrigger::PlayKey:        return "play_key";
        case PlayerToggleTrigger::Hall:           return "hall";
        case PlayerToggleTrigger::Web:            return "web";
        case PlayerToggleTrigger::Alarm:          return "alarm";
        case PlayerToggleTrigger::NfcAdminResume: return "nfc_admin";
        case PlayerToggleTrigger::Unknown:
        default:                                  return "unknown";
    }
}

PlayerPlaybackState player_playback_state_get()
{
    if (audio_service_is_paused()) {
        return PlayerPlaybackState::Paused;
    }
    if (audio_service_is_playing()) {
        return PlayerPlaybackState::Playing;
    }
    return PlayerPlaybackState::Stopped;
}

bool player_set_paused(bool paused, PlayerToggleTrigger trigger)
{
    const PlayerSourceState source = player_source_get();
    const PlayerPlaybackState current = player_playback_state_get();

    LOGI("[播放器] 设置暂停=%d：来源=%s 音源=%d playing=%d paused=%d",
         paused ? 1 : 0,
         control_toggle_trigger_label(trigger),
         static_cast<int>(source.type),
         current == PlayerPlaybackState::Playing ? 1 : 0,
         current == PlayerPlaybackState::Paused ? 1 : 0);

    if (app_rescan_state_get().rescanning) {
        return false;
    }

    if (paused) {
        if (current == PlayerPlaybackState::Paused) {
            return true;
        }
        if (current != PlayerPlaybackState::Playing) {
            return false;
        }

        if (!audio_service_pause(true)) {
            LOGW("[播放器] 暂停命令执行失败");
            return false;
        }

        s_user_paused = true;
        s_pause_time_ms = millis();

        if (source.type == PlayerSourceType::NET_RADIO) {
            player_source_set_radio_status(true, String("paused"), String());
        } else if (source.type == PlayerSourceType::NET_TRACK) {
            player_source_set_net_track_status(true, String("paused"), String());
        }

        LOGI("[播放器] 已暂停于 %u ms", (unsigned)audio_get_play_ms());
        return true;
    }

    // 磁铁仍靠近时，所有入口都只能保持暂停；霍尔离开后的恢复请求不会命中此条件。
    if (hall_control_blocks_resume()) {
        LOGI("[HALL] 磁铁仍靠近，拒绝恢复请求：来源=%s",
             control_toggle_trigger_label(trigger));
        return false;
    }

    if (current == PlayerPlaybackState::Playing) {
        return true;
    }

    if (current == PlayerPlaybackState::Paused) {
        if (!audio_service_resume(true)) {
            LOGW("[播放器] 恢复命令执行失败");
            return false;
        }

        s_user_paused = false;
        s_pause_time_ms = 0;

        if (source.type == PlayerSourceType::NET_RADIO) {
            player_source_set_radio_status(true, String("playing"), String());
        } else if (source.type == PlayerSourceType::NET_TRACK) {
            player_source_set_net_track_status(true, String("playing"), String());
        }

        LOGI("[播放器] 已从暂停恢复");
        return true;
    }

    // 已停止时按当前音源重新起播。网络音源可以没有本地曲库。
    if (source.type == PlayerSourceType::NET_RADIO && source.radio_idx >= 0) {
        return player_play_radio_index(source.radio_idx);
    }

    if (source.type == PlayerSourceType::NET_TRACK && source.net_track_idx >= 0) {
        return player_play_net_track_index(source.net_track_idx);
    }

    if (control_track_count() <= 0) {
        return false;
    }

    const int cur = control_current_track_idx();
    if (cur < 0) {
        return false;
    }

    LOGI("[播放器] 重新启动当前歌曲 #%d", cur);
    return control_play_track_dispatch(cur, false, true);
}

void player_toggle_play(PlayerToggleTrigger trigger)
{
    const PlayerPlaybackState current = player_playback_state_get();
    if (current == PlayerPlaybackState::Playing) {
        (void)player_set_paused(true, trigger);
        return;
    }

    (void)player_set_paused(false, trigger);
}

void player_volume_step(int delta)
{
    const bool ok = audio_output_route_step_user_volume(delta);
    if (audio_output_route_is_bluetooth_tx() &&
        !audio_output_route_bluetooth_tx_volume_known()) {
        LOGD("[音量] 蓝牙音量等待查询：增量=%d 请求=%s", delta, ok ? "已受理" : "失败");
        return;
    }

    LOGD("[音量] 用户音量=%u%% 路线=%s",
         (unsigned)audio_output_route_get_user_volume(),
         audio_output_route_label());
}

bool player_seek_window_get(PlayerSeekWindow* out_window)
{
    if (!out_window) {
        return false;
    }

    *out_window = PlayerSeekWindow{};
    const uint32_t total_ms = audio_get_total_ms();
    if (total_ms == 0 || !audio_service_is_seekable()) {
        return false;
    }

    out_window->total_ms = total_ms;
    out_window->current_ms = audio_get_play_ms();
    if (out_window->current_ms > total_ms) {
        out_window->current_ms = total_ms;
    }
    out_window->playback_revision = audio_service_playback_revision();
    return out_window->playback_revision != 0;
}

bool player_seek_to_ms_async(uint32_t target_ms,
                             uint32_t expected_playback_revision,
                             uint32_t* out_request_id)
{
    if (out_request_id) {
        *out_request_id = 0;
    }

    const uint32_t total_ms = audio_get_total_ms();
    if (total_ms == 0 || !audio_service_is_seekable() ||
        expected_playback_revision == 0) {
        LOGW("[播放器] 实体按键跳转不可用：总时长=%lums 可跳转=%d 世代=%lu",
             (unsigned long)total_ms,
             audio_service_is_seekable() ? 1 : 0,
             (unsigned long)expected_playback_revision);
        return false;
    }

    if (target_ms >= total_ms) {
        target_ms = total_ms > 500 ? total_ms - 500 : 0;
    }

    uint32_t request_id = 0;
    const bool ok = audio_service_seek_ms_async_if_revision(target_ms,
                                                            expected_playback_revision,
                                                            &request_id);
    if (ok) {
        if (out_request_id) {
            *out_request_id = request_id;
        }
        LOGI("[播放器] 实体按键跳转已提交：请求=%lu 目标=%lums 世代=%lu",
             (unsigned long)request_id,
             (unsigned long)target_ms,
             (unsigned long)expected_playback_revision);
    } else {
        LOGW("[播放器] 实体按键跳转提交失败：目标=%lums 世代=%lu",
             (unsigned long)target_ms,
             (unsigned long)expected_playback_revision);
    }
    return ok;
}

void player_next_group()
{
    // 编码器按住 + NEXT 长按：进入当前播放源/当前播放模式对应的列表。
    // - 本地全部播放：打开“全部歌曲”列表
    // - 歌手/专辑播放：打开对应分组列表
    // - 网络电台：打开电台列表
    // - NAS歌曲：打开 NAS 歌曲列表
    // 普通 NEXT 长按已经改为快进；只有组合键进入列表。
    if (control_enter_list_select_dispatch()) {
        return;
    }

    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::NET_RADIO) {
        LOGW("[列表] 电台播放中，但无法进入电台列表");
    } else if (source.type == PlayerSourceType::NET_TRACK) {
        LOGW("[列表] NAS歌曲播放中，但无法进入NAS歌曲列表");
    } else {
        LOGW("[列表] 本地播放中，但无法进入歌曲列表 模式=%d 数量=%d",
             (int)app_play_mode_get(),
             control_track_count());
    }
}

bool control_mode_is_random(play_mode_t mode)
{
    return mode == PLAY_MODE_ALL_RND ||
           mode == PLAY_MODE_ARTIST_RND ||
           mode == PLAY_MODE_ALBUM_RND;
}

namespace {

play_mode_t control_make_mode(int category, bool is_random)
{
    switch (category) {
        case 0: return is_random ? PLAY_MODE_ALL_RND    : PLAY_MODE_ALL_SEQ;
        case 1: return is_random ? PLAY_MODE_ARTIST_RND : PLAY_MODE_ARTIST_SEQ;
        case 2: return is_random ? PLAY_MODE_ALBUM_RND  : PLAY_MODE_ALBUM_SEQ;
        default: return is_random ? PLAY_MODE_ALL_RND   : PLAY_MODE_ALL_SEQ;
    }
}

int control_mode_category(play_mode_t mode)
{
    if (player_playlist_is_artist_mode(mode)) return 1;
    if (player_playlist_is_album_mode(mode)) return 2;
    return 0;
}

void control_apply_mode_context(play_mode_t new_mode, int current_idx, bool verbose)
{
    (void)app_play_mode_set(new_mode, AppPlayModeChangeReason::PlayerControl);

    if (current_idx >= 0) {
        (void)player_playlist_align_group_context_for_track(current_idx, verbose);
        player_playlist_update_for_current_track(current_idx, verbose);
    } else {
        player_playlist_force_rebuild();
        player_playlist_ensure_current();
    }

    control_update_track_pos_for_mode(current_idx);
}

} // namespace

void player_toggle_random()
{
    const PlayerSourceType source_type = player_source_type_get();
    const play_mode_t current_mode = app_play_mode_get();
    const bool next_random = !control_mode_is_random(current_mode);

    if (source_type == PlayerSourceType::NET_TRACK ||
        source_type == PlayerSourceType::NET_RADIO) {
        // 网络音源只允许在“全部顺序/全部随机”之间切换，
        // 不触碰本地歌手/专辑播放列表上下文。
        const play_mode_t new_mode = next_random ? PLAY_MODE_ALL_RND : PLAY_MODE_ALL_SEQ;
        (void)app_play_mode_set(new_mode, AppPlayModeChangeReason::PlayerControl);
        ui_request_refresh_now();
        LOGI("[播放器] 网络音源播放顺序切换: %s",
             next_random ? "随机" : "顺序");
        return;
    }

    const int category = control_mode_category(current_mode);
    const play_mode_t new_mode = control_make_mode(category, next_random);

    control_apply_mode_context(new_mode, control_current_track_idx(), false);

    LOGI("[播放器] 小类切换: %s", next_random ? "随机" : "顺序");
}

void player_cycle_mode_category()
{
    const PlayerSourceType source_type = player_source_type_get();
    if (source_type == PlayerSourceType::NET_TRACK ||
        source_type == PlayerSourceType::NET_RADIO) {
        control_normalize_remote_mode_category(
            source_type == PlayerSourceType::NET_TRACK ? "NAS播放" : "网络收音机");
        LOGI("[播放器] 当前网络音源不支持切换歌手/专辑大类");
        ui_request_refresh_now();
        return;
    }

    const int cur = control_current_track_idx();
    const play_mode_t current_mode = app_play_mode_get();
    const bool is_random = control_mode_is_random(current_mode);
    const int old_category = control_mode_category(current_mode);
    const int new_category = (old_category + 1) % 3;
    const play_mode_t new_mode = control_make_mode(new_category, is_random);

    control_apply_mode_context(new_mode, cur, true);

    const char* cat_name = "全部";
    switch (new_category) {
        case 1: cat_name = "歌手"; break;
        case 2: cat_name = "专辑"; break;
        default: break;
    }

    LOGI("[播放器] 大类切换: %s (%s)", cat_name, is_random ? "随机" : "顺序");
}
