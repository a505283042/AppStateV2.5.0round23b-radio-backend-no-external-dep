#include "player_control.h"
#include <vector>
#include <esp_system.h>

#include "audio/audio.h"
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
#include "lyrics/lyrics.h"
#include "audio/audio_radio_backend.h"
#include "storage/storage.h"
#include "storage/storage_catalog_v3.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

PlayerControlHooks s_hooks{};

bool s_user_paused = false;
bool s_manual_stop_latched = false;
uint32_t s_pause_time_ms = 0;
constexpr uint32_t LOCAL_AUTO_NEXT_MIN_PLAY_MS = 1200;

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
    s_radio_return.mode = g_play_mode;
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
    if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND ||
        g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
        const PlayerPlaylistDisplayInfo display =
            player_playlist_get_display_info(current_idx, (int)storage_catalog_v3_track_count());
        ui_set_track_pos(display.display_pos, display.display_total);
    } else {
        ui_set_track_pos(current_idx, (int)storage_catalog_v3_track_count());
    }
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

static bool control_prepare_net_track_item(int idx, NetMusicItem& item, String& url)
{
    if (idx < 0) return false;

    if (!net_music_catalog_is_loaded() || net_music_catalog_count() == 0) {
        LOGW("[网络歌曲] 目录 未加载 or 为空");
        return false;
    }

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
    std::vector<uint16_t> order;
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

static bool control_is_net_track_random_mode()
{
    return g_play_mode == PLAY_MODE_ALL_RND ||
           g_play_mode == PLAY_MODE_ARTIST_RND ||
           g_play_mode == PLAY_MODE_ALBUM_RND;
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
    s_net_track_shuffle.order.clear();
    s_net_track_shuffle.pos = 0;
    s_net_track_shuffle.count = 0;
    s_net_track_shuffle.ready = false;
}

static int control_net_track_shuffle_index_at(uint32_t pos)
{
    if (!s_net_track_shuffle.ready) return -1;
    if (pos >= s_net_track_shuffle.order.size()) return -1;
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

    s_net_track_shuffle.order.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        s_net_track_shuffle.order.push_back((uint16_t)i);
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
        s_net_track_shuffle.order.size() != count) {
        control_reset_net_track_shuffle(current_idx);
        return s_net_track_shuffle.ready;
    }

    const int expected = control_net_track_shuffle_index_at(s_net_track_shuffle.pos);
    if (expected == current_idx) {
        return true;
    }

    // 如果当前播放 index 和随机 pos 不一致，先尝试在当前洗牌表里定位。
    for (uint32_t i = 0; i < s_net_track_shuffle.order.size(); ++i) {
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

bool player_control_try_auto_next(bool entered, bool started)
{
    if (!entered) return false;
    if (s_user_paused) return false;

    const PlayerSourceState source = player_source_get();

    // 网络电台是连续流，不做自动下一台。
    if (source.type == PlayerSourceType::NET_RADIO) {
        return false;
    }

    // NAS/HTTP 网络歌曲：先处理 URLStream 不报 EOF 的情况。
    if (source.type == PlayerSourceType::NET_TRACK) {
        bool should_advance = false;

        if (!audio_service_is_playing()) {
            should_advance = true;
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

        if (!storage_is_ready() || storage_has_recent_io_error()) {
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
    if (audio_service_is_playing()) return false;

    // 从网络流切回本地时，解码器/I2S 刚复位的瞬间可能会出现短暂 not playing。
    // 不能立刻判定“歌曲结束”，否则会出现响一下就连续自动下一首。
    const uint32_t local_play_ms = audio_get_play_ms();
    if (local_play_ms < LOCAL_AUTO_NEXT_MIN_PLAY_MS) {
        LOGW("[播放器] auto 下一首 已抑制: 本地 play 过短 play_ms=%lu 来源=%s",
            (unsigned long)local_play_ms,
            player_source_type_key(source.type));
        return false;
    }

    const int track_count = control_track_count();
    if (track_count <= 0) return false;

    if (!storage_is_ready() || storage_has_recent_io_error()) {
        LOGW("[播放器] auto 下一首 被阻止: storage 未就绪 or IO error pending");
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
             (int)g_play_mode,
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

    // 保存当前播放状态到返回上下文
    player_save_radio_return_context_if_needed();

    if (player_source_get().type == PlayerSourceType::NET_RADIO) {
        audio_radio_backend_stop();
    }
    if (audio_service_is_playing() || audio_service_is_paused()) {
        audio_service_stop(true);
    }
    control_prepare_for_radio_source();

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
        player_source_set_radio_runtime(String(audio_radio_backend_name()), String(), 0, String("connecting"), true);
        LOGI("[电台] 播放电台 索引=%d 名称=%s 后端=%s", idx, item->name.c_str(), audio_radio_backend_name());
        return true;
    }

    player_source_set_radio_status(false, String("error"), String("backend_start_failed"));
    LOGW("[电台] 播放失败：索引=%d 名称=%s", idx, item->name.c_str());
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

    if (!s_radio_return.valid || s_radio_return.track_idx < 0) {
        LOGW("[电台] no 返回 context");
        return false;
    }

    g_play_mode = s_radio_return.mode;
    player_playlist_set_current_group_idx(s_radio_return.group_idx);
    player_playlist_force_rebuild();

    const bool ok = player_play_idx_v3((uint32_t)s_radio_return.track_idx, true, true);
    if (!ok) {
        LOGW("[电台] 恢复本地歌曲失败：索引=%d", s_radio_return.track_idx);
        return false;
    }

    audio_set_volume(s_radio_return.volume);
    ui_set_volume(s_radio_return.volume);

    LOGD("[电台] 已恢复本地歌曲：索引=%d", s_radio_return.track_idx);
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

    int target = s_net_track_return_local_idx;

    if (target < 0 || target >= track_count) {
        target = 0;
        LOGW("[网络歌曲] 返回本地失败，回退到 idx=0");
    }

    LOGD("[网络歌曲] 返回本地目标=%d", target);

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

    if (control_is_net_track_random_mode()) {
        g_play_mode = PLAY_MODE_ALL_SEQ;
        LOGD("[网络歌曲] 模式 -> all_seq");
    } else {
        g_play_mode = PLAY_MODE_ALL_RND;
        LOGD("[网络歌曲] 模式 -> all_rnd");
    }

    ui_set_play_mode(g_play_mode);
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

    if (before_source.type == PlayerSourceType::LOCAL_TRACK) {
        const int cur = control_current_track_idx();
        if (cur >= 0 && cur < control_track_count()) {
            s_net_track_return_local_idx = cur;
            LOGD("[网络歌曲] 记住本地返回位置：索引=%d", s_net_track_return_local_idx);
        }
    }

    if (player_source_get().type == PlayerSourceType::NET_RADIO) {
        audio_radio_backend_stop();
    }

    if (audio_service_is_playing() || audio_service_is_paused()) {
        audio_service_stop(true);
    }

    control_prepare_for_radio_source();

    player_source_set_net_track_stub(idx, item, url, String("connecting"), String());
    player_state_set_current_index(-1);
    player_control_reset_runtime_flags();
    audio_service_resume();

    ui_set_now_playing(item.title.c_str(), item.artist.c_str());
    ui_set_album(item.album);
    ui_set_track_pos(idx, (int)net_music_catalog_count());
    ui_set_play_mode(g_play_mode);
    ui_set_volume(audio_get_volume());
    

    // NAS 播放起播时先显示网络封面加载图，避免继续显示上一首封面。
    // 如果 /System/net_cover_loading.jpg 不存在，则回退默认封面。
    if (!control_apply_cover_file("/System/net_cover_loading.jpg")) {
        (void)control_apply_cover_file("/System/default_cover.jpg");
    }
    ui_request_refresh_now();

    uint32_t stream_start_offset = 0;
    (void)net_music_mp3_probe_audio_start_offset(url, &stream_start_offset);

    const bool ok = (stream_start_offset > 0)
        ? audio_service_play_stream_mp3_from_offset(url.c_str(), stream_start_offset, true)
        : audio_service_play_stream_mp3(url.c_str(), true);
    if (ok) {
        player_source_set_net_track_status(true, String("playing"), String());

        // NAS/HTTP 文件播放路径会先把 audio total 清零。
        // 这里用 net_music.txt 预生成的 duration_ms 写回 audio 层，
        // 这样屏幕 UI 里 audio_get_total_ms() 才能拿到总时长。
        audio_set_total_ms(item.duration_ms);

        if (reset_shuffle && control_is_net_track_random_mode()) {
            control_reset_net_track_shuffle(idx);
        }

        control_reset_net_track_eof_watch(idx);

        // 播放先启动，NAS MP3 内嵌封面通过 HTTP Range 后台解析和应用，
        // 避免起播被 ID3/APIC 网络读取拖慢。
        LOGI("[网络封面] 准备启动 NAS 内嵌封面任务 idx=%d url=%s", idx, url.c_str());
        net_music_embedded_cover_start(idx, url);

        LOGI("[网络歌曲] 播放网络歌曲 索引=%d 标题=%s 时长=%lums offset=%lu URL=%s",
            idx,
            item.title.c_str(),
            (unsigned long)item.duration_ms,
            (unsigned long)stream_start_offset,
            url.c_str());
        return true;
    }

    player_source_set_net_track_status(false, String("error"), String("stream_start_failed"));
    LOGW("[网络歌曲] 播放失败：索引=%d 标题=%s URL=%s",
         idx,
         item.title.c_str(),
         url.c_str());
    s_net_track_eof_watch.armed = false;
    return false;
}

bool player_play_net_track_index(int idx)
{
    return control_play_net_track_index_impl(idx, true);
}

void player_stop_net_track()
{
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
             (int)g_play_mode, player_playlist_get_current_group_idx(), cur);
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
             (int)g_play_mode, player_playlist_get_current_group_idx(), cur);
    }

    LOGI("[播放器] 上一首 -> #%d", prev);

    ui_notify_cover_panel_nav_feedback(-1);

    const bool ok = control_play_track_dispatch(prev, false, true);
    if (!ok) {
        ui_notify_cover_panel_nav_feedback(0);
    }
}

void player_toggle_play()
{
    const PlayerSourceState source = player_source_get();
    const int track_count = control_track_count();
    if (track_count <= 0 &&
        source.type != PlayerSourceType::NET_RADIO &&
        source.type != PlayerSourceType::NET_TRACK) {
        return;
    }
    if (g_rescanning) return;

    if (source.type == PlayerSourceType::NET_RADIO) {
        if (audio_radio_backend_toggle_pause()) {
            const bool paused = audio_radio_backend_is_paused();
            player_source_set_radio_status(true, paused ? String("paused") : String("playing"), String());
            LOGI("[电台] %s", paused ? "Paused" : "Resumed");
            return;
        }
        if (source.radio_idx >= 0) {
            (void)player_play_radio_index(source.radio_idx);
            return;
        }
    }

    if (audio_service_is_paused()) {
        audio_service_resume();
        s_user_paused = false;
        s_pause_time_ms = 0;

        if (source.type == PlayerSourceType::NET_TRACK) {
            player_source_set_net_track_status(true, String("playing"), String());
        }

        LOGI("[播放器] 已从暂停恢复");
        return;
    }

    if (audio_service_is_playing()) {
        audio_service_pause();
        s_user_paused = true;
        s_pause_time_ms = millis();
        if (source.type == PlayerSourceType::NET_TRACK) {
            player_source_set_net_track_status(true, String("paused"), String());
        }
        const uint32_t paused_at_ms = audio_get_play_ms();
        LOGI("[播放器] 暂停于 %u ms", paused_at_ms);
        return;
    }

    if (source.type == PlayerSourceType::NET_RADIO && source.radio_idx >= 0) {
        (void)player_play_radio_index(source.radio_idx);
        return;
    }

    if (source.type == PlayerSourceType::NET_TRACK && source.net_track_idx >= 0) {
        (void)player_play_net_track_index(source.net_track_idx);
        return;
    }

    const int cur = control_current_track_idx();
    if (cur >= 0) {
        LOGI("[播放器] 重新启动 当前 歌曲 #%d", cur);
        (void)control_play_track_dispatch(cur, false, true);
    }
}

void player_volume_step(int delta)
{
    int v = (int)audio_get_volume() + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    audio_set_volume((uint8_t)v);
    ui_set_volume((uint8_t)v);
    LOGD("[音量] %d%%", v);
}

void player_next_group()
{
    // NEXT 长按的统一语义：进入当前播放源/当前播放模式对应的列表。
    // - 本地全部播放：打开“全部歌曲”列表
    // - 歌手/专辑播放：打开对应分组列表
    // - 网络电台：打开电台列表
    // - NAS歌曲：打开 NAS 歌曲列表
    // 旧逻辑在“本地全部播放”时会跳 10 首，导致长按 NEXT/LIST 不能打开列表。
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
             (int)g_play_mode,
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
    g_play_mode = new_mode;

    if (current_idx >= 0) {
        (void)player_playlist_align_group_context_for_track(current_idx, verbose);
        player_playlist_update_for_current_track(current_idx, verbose);
    } else {
        player_playlist_force_rebuild();
        player_playlist_get_current();
    }

    control_update_track_pos_for_mode(current_idx);
    ui_set_play_mode(g_play_mode);
}

} // namespace

void player_toggle_random()
{
    const int category = control_mode_category(g_play_mode);
    const bool next_random = !control_mode_is_random(g_play_mode);
    const play_mode_t new_mode = control_make_mode(category, next_random);

    control_apply_mode_context(new_mode, control_current_track_idx(), false);

    LOGI("[播放器] 小类切换: %s", next_random ? "随机" : "顺序");
}

void player_cycle_mode_category()
{
    const int cur = control_current_track_idx();
    const bool is_random = control_mode_is_random(g_play_mode);
    const int old_category = control_mode_category(g_play_mode);
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
