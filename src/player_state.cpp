#include "player_state.h"

#include <Arduino.h>

#include "app_flags.h"
#include "audio/audio.h"
#include "audio/audio_service.h"
#include "nfc/nfc.h"
#include "lyrics/lyrics.h"
#include "player_assets.h"
#include "player_binding.h"
#include "player_control.h"
#include "player_list_select.h"
#include "player_playlist.h"
#include "player_recover.h"
#include "player_snapshot.h"
#include "player_source.h"
#include "audio/audio_radio_backend.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_types_v3.h"
#include "ui/ui.h"
#include "utils/log.h"

static constexpr int  V3_TEST_START_INDEX = 0;

static bool s_started = false;
static int  s_cur = 0;
static bool s_next_play_from_nfc = false;

static bool player_play_trackinfo_core(const TrackInfo& t,
                                       int idx_for_state,
                                       int library_total_hint,
                                       bool verbose,
                                       bool force_cover);
static int player_clamp_idx_for_dispatch(int idx);
bool player_play_idx_v3(uint32_t idx, bool verbose, bool force_cover);
static int player_assets_hook_get_current_track_idx();
static void player_assets_hook_on_current_cover_ready(int track_idx);
static void player_assets_init_once();
static bool player_list_select_hook_play_track_dispatch(int idx, bool verbose, bool force_cover);
static bool player_list_select_hook_play_radio_dispatch(int idx);
static bool player_control_hook_play_track_dispatch(int idx, bool verbose, bool force_cover);
static bool player_control_hook_enter_list_select();
static void player_list_select_init_once();
static void player_control_init_once();
static void player_recover_init_once();
static void player_binding_init_once();

static int player_track_count_for_dispatch()
{
    return (int)storage_catalog_v3_track_count();
}

static int player_clamp_idx_for_dispatch(int idx)
{
    const int total = player_track_count_for_dispatch();
    if (total <= 0) return -1;
    if (idx < 0) return 0;
    if (idx >= total) return total - 1;
    return idx;
}

// 当前封面缓存属于哪一首；-1 表示未知/需要重解码
static int s_cover_idx = -1;


static bool s_player_assets_hooks_inited = false;
static bool s_player_list_hooks_inited = false;
static bool s_player_control_hooks_inited = false;
static bool s_player_recover_hooks_inited = false;
static bool s_player_binding_hooks_inited = false;


static int player_assets_hook_get_current_track_idx()
{
    return s_cur;
}

static void player_assets_hook_on_current_cover_ready(int track_idx)
{
    s_cover_idx = track_idx;
}

static void player_assets_init_once()
{
    if (s_player_assets_hooks_inited) return;

    PlayerAssetsHooks hooks{};
    hooks.get_current_track_idx = &player_assets_hook_get_current_track_idx;
    hooks.get_next_track_for_cover_prefetch = &player_playlist_get_next_for_cover_prefetch;
    hooks.on_current_cover_ready = &player_assets_hook_on_current_cover_ready;
    player_assets_setup_hooks(hooks);
    s_player_assets_hooks_inited = true;
}


static bool player_list_select_hook_play_track_dispatch(int idx, bool verbose, bool force_cover)
{
    if (idx < 0) return false;
    return player_play_idx_v3((uint32_t)idx, verbose, force_cover);
}

static bool player_list_select_hook_play_radio_dispatch(int idx)
{
    if (idx < 0) return false;
    return player_play_radio_index(idx);
}

static void player_list_select_init_once()
{
    if (s_player_list_hooks_inited) return;

    PlayerListSelectHooks hooks{};
    hooks.play_track_dispatch = &player_list_select_hook_play_track_dispatch;
    hooks.play_radio_dispatch = &player_list_select_hook_play_radio_dispatch;
    player_list_select_setup_hooks(hooks);
    s_player_list_hooks_inited = true;
}

static bool player_control_hook_play_track_dispatch(int idx, bool verbose, bool force_cover)
{
    if (idx < 0) return false;
    return player_play_idx_v3((uint32_t)idx, verbose, force_cover);
}

static bool player_control_hook_enter_list_select()
{
    player_list_select_init_once();
    return player_list_select_enter(g_play_mode);
}

static void player_control_init_once()
{
    if (s_player_control_hooks_inited) return;

    PlayerControlHooks hooks{};
    hooks.get_current_track_idx = &player_assets_hook_get_current_track_idx;
    hooks.play_track_dispatch = &player_control_hook_play_track_dispatch;
    hooks.get_track_count = &player_track_count_for_dispatch;
    hooks.enter_list_select = &player_control_hook_enter_list_select;
    player_control_setup_hooks(hooks);
    s_player_control_hooks_inited = true;
}

static void player_recover_init_once()
{
    if (s_player_recover_hooks_inited) return;

    PlayerRecoverHooks hooks{};
    hooks.get_current_track_idx = &player_assets_hook_get_current_track_idx;
    hooks.play_track_dispatch = &player_control_hook_play_track_dispatch;
    player_recover_setup_hooks(hooks);
    s_player_recover_hooks_inited = true;
}

static void player_binding_init_once()
{
    if (s_player_binding_hooks_inited) return;

    PlayerBindingHooks hooks{};
    hooks.play_track_dispatch = &player_control_hook_play_track_dispatch;
    player_binding_setup_hooks(hooks);
    s_player_binding_hooks_inited = true;
}

static bool player_play_trackinfo_core(const TrackInfo& t,
                                       int idx_for_state,
                                       int library_total_hint,
                                       bool verbose,
                                       bool force_cover)
{
    const bool from_nfc = s_next_play_from_nfc;
    s_next_play_from_nfc = false;

    player_assets_init_once();
    player_control_init_once();
    player_recover_init_once();
    player_binding_init_once();

    if (player_source_get().type == PlayerSourceType::NET_RADIO) {
        audio_radio_backend_stop();
    }

    s_cur = idx_for_state;
    player_source_set_local_track(s_cur);

    // 先把“当前歌曲属于哪一组”对齐，再刷新当前模式的播放列表位置
    (void)player_playlist_align_group_context_for_track(s_cur, true);

    // 更新播放列表缓存和当前位置
    player_playlist_update_for_current_track(s_cur, true);

    // 切歌时重置暂停/手动停止状态，确保新歌能够正常播放
    player_control_on_track_started();
    audio_service_resume();  // 确保音频服务的暂停标志被清空

    LOGI("[PLAYER] play #%d: %s", s_cur, t.audio_path.c_str());

    if (verbose) {
        Serial.println("----- TRACK META CHECK -----");
        Serial.printf("path  : %s\n", t.audio_path.c_str());
        Serial.printf("ext   : %s\n", t.ext.c_str());
        Serial.printf("artist: %s\n", t.artist.c_str());
        Serial.printf("album : %s\n", t.album.c_str());
        Serial.printf("title : %s\n", t.title.c_str());
        Serial.println("---------------------------");

        Serial.printf("cover_source=%d offset=%u size=%u mime=%s path=%s\n",
                      (int)t.cover_source,
                      (unsigned)t.cover_offset,
                      (unsigned)t.cover_size,
                      t.cover_mime.c_str(),
                      t.cover_path.c_str());
    }

    const uint32_t t_switch_begin = millis();
    uint32_t t_after_stop = t_switch_begin;
    uint32_t t_after_ui_prepare = t_switch_begin;
    uint32_t t_after_lyrics_prefetch = t_switch_begin;

    // 切歌时必须先停音频，确保旧文件关闭；后续资源改为开播后再补。
    if (audio_service_is_playing()) {
        audio_service_stop(true);
    }
    t_after_stop = millis();

    // 新歌开始时，先取消上一首遗留的封面预取请求。
    player_assets_cancel_pending_cover_prefetch();

    // 当前曲目标记更新后，先清掉旧歌词；封面先沿用上一首/占位图，避免把“能后补”的事情挡在开播前。
    g_lyricsDisplay.clear();

    // 先尝试命中“上一轮预读好的下一首封面缓存”。命中后当前封面可瞬时切上来。
    const bool cover_cache_hit = ui_cover_apply_cached(s_cur);
    if (cover_cache_hit) {
        s_cover_idx = s_cur;
        ui_request_refresh_now();
        LOGI("[PLAYER] current cover cache hit track=%d", s_cur);
    }

    bool need_decode_cover = (force_cover || s_cover_idx != s_cur) && !cover_cache_hit;

    // 先提交文字、模式、序号，让 UI 立刻跟上；不再等待封面/歌词准备完。
    ui_set_now_playing(t.title.c_str(), t.artist.c_str());
    ui_set_album(t.album);

    // 根据播放模式显示正确的歌曲索引和总数
    int display_pos = s_cur;
    int display_total = (library_total_hint > 0) ? library_total_hint : (int)storage_catalog_v3_track_count();

    if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND ||
        g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
        const PlayerPlaylistDisplayInfo display = player_playlist_get_display_info(s_cur, library_total_hint);
        display_total = display.display_total;
        display_pos = display.display_pos;
    }

    ui_set_track_pos(display_pos, display_total);
    ui_set_play_mode(g_play_mode);
    ui_set_volume(audio_get_volume());
    t_after_ui_prepare = millis();

    PlayerDeferredAssetJob asset_job{};
    char* primed_lyrics_text = nullptr;
    size_t primed_lyrics_len = 0;
    bool lyrics_primed = false;
    
    const bool has_deferred_assets = player_assets_prepare_deferred_request(
        t,
        s_cur,
        true,
        t.lrc_path.length() > 0,
        need_decode_cover || cover_cache_hit,
        asset_job);
    asset_job.need_total = true; // 获取总时间数   /true打开    /false 关闭
    asset_job.need_lyrics = true;//  需要获取歌词   /true打开  /false 关闭
    asset_job.suppress_next_prefetch = true;// 不要立刻探取封面，等开播后再补
    t_after_lyrics_prefetch = millis();

    // 切到新歌时，先清当前 raw。next raw 不要直接清，先尝试提升。
    player_assets_clear_primed_current_cover();

    LOGI("[PLAYER] prime check track=%d cover_cache_hit=%d need_decode_cover=%d cover_source=%u cover_size=%u",
     s_cur,
     cover_cache_hit ? 1 : 0,
     need_decode_cover ? 1 : 0,
     (unsigned)asset_job.cover_source,
     (unsigned)asset_job.cover_size);

    const bool promoted_next_cover =
        (!cover_cache_hit && need_decode_cover) ?
        player_assets_promote_next_cover_to_current(s_cur) :
        false;

    LOGI("[PLAYER] promote next raw check track=%d promoted=%d",
        s_cur,
        promoted_next_cover ? 1 : 0);
    player_assets_clear_deferred_current_cover_apply();

    if (from_nfc && need_decode_cover && !cover_cache_hit) {
        player_assets_set_deferred_current_cover_apply(s_cur, 90);
    }

    // 先读下一首 raw 封面，再读当前首 raw 封面，最后再 audio_service_play()。
    // 这样文件访问顺序是：下一首 -> 当前首封面 -> 当前首播放。
    TrackInfo next_cover_track;
    int next_cover_idx = -1;
    bool next_cover_raw_primed = false;

    const uint32_t prep_before_next_ms = millis() - t_switch_begin;

    const bool got_next_cover =
        player_playlist_get_next_for_cover_prefetch(s_cur, next_cover_idx, next_cover_track);

    const bool next_cache_ready =
        got_next_cover && next_cover_idx >= 0 ?
        ui_cover_cache_is_ready(next_cover_idx) :
        false;

    const uint32_t next_raw_prep_limit_ms = 360u;// 360ms 内完成下一首封面预取，否则不提升下一首封面到当前首封面
    const uint32_t next_raw_size_limit = 96u * 1024u;// 96KB 最大封面大小

    const bool allow_prime_next_raw =
        prep_before_next_ms < next_raw_prep_limit_ms &&
        got_next_cover &&
        next_cover_idx >= 0 &&
        next_cover_idx != s_cur &&
        !next_cache_ready &&
        next_cover_track.cover_source != COVER_NONE &&
        next_cover_track.cover_size > 0 &&
        next_cover_track.cover_size <= next_raw_size_limit;

    LOGI("[PLAYER] next raw check cur=%d allow=%d got=%d target=%d from_nfc=%d prep=%lu limit=%lu cache=%d source=%u size=%u size_limit=%lu",
        s_cur,
        allow_prime_next_raw ? 1 : 0,
        got_next_cover ? 1 : 0,
        next_cover_idx,
        from_nfc ? 1 : 0,
        (unsigned long)prep_before_next_ms,
        (unsigned long)next_raw_prep_limit_ms,
        next_cache_ready ? 1 : 0,
        got_next_cover ? (unsigned)next_cover_track.cover_source : 0,
        got_next_cover ? (unsigned)next_cover_track.cover_size : 0,
        (unsigned long)next_raw_size_limit);

    if (allow_prime_next_raw) {

        uint8_t* next_cover_buf = nullptr;
        size_t next_cover_len = 0;
        bool next_cover_is_png = false;

        const uint32_t t_next_cover_begin = millis();

        const bool next_cover_fetch_ok =
            audio_service_fetch_cover(next_cover_track.cover_source,
                                    next_cover_track.audio_path.c_str(),
                                    next_cover_track.cover_path.c_str(),
                                    next_cover_track.cover_offset,
                                    next_cover_track.cover_size,
                                    &next_cover_buf,
                                    &next_cover_len,
                                    &next_cover_is_png,
                                    true);

        const uint32_t next_cover_cost = millis() - t_next_cover_begin;

        if (next_cover_fetch_ok &&
            next_cover_buf &&
            next_cover_len > 0 &&
            player_assets_prime_next_cover(next_cover_track,
                                        next_cover_idx,
                                        next_cover_buf,
                                        next_cover_len,
                                        next_cover_is_png)) {
            next_cover_buf = nullptr; // ownership moved
            next_cover_raw_primed = true;
        }

        if (next_cover_buf) {
            ui_cover_free_allocated(next_cover_buf);
            next_cover_buf = nullptr;
        }

        LOGI("[PLAYER] next cover raw prime before current target=%d ok=%d len=%u cost=%lu prep=%lu",
            next_cover_idx,
            next_cover_raw_primed ? 1 : 0,
            (unsigned)next_cover_len,
            (unsigned long)next_cover_cost,
            (unsigned long)(millis() - t_switch_begin));
    } else {
        // 本次不适合预读下一首，清掉旧 next，避免随机/点播/模式切换后误命中。
        player_assets_drop_primed_next_cover();
    }

    if (asset_job.need_lyrics && asset_job.lyrics_path[0]) {
        if (audio_service_fetch_lyrics(asset_job.lyrics_path,
                                    &primed_lyrics_text,
                                    &primed_lyrics_len,
                                    true) &&
            primed_lyrics_text &&
            primed_lyrics_len > 0) {
            if (g_lyricsDisplay.loadFromOwnedTextBuffer(primed_lyrics_text, primed_lyrics_len)) {
                primed_lyrics_text = nullptr; // ownership moved
                lyrics_primed = true;
                asset_job.need_lyrics = false; // 后台不要再读一次
                LOGI("[PLAYER] lyrics primed before play track=%d len=%u",
                    s_cur, (unsigned)primed_lyrics_len);
            }
        }
    }

    uint8_t* primed_cover_buf = nullptr;
    size_t primed_cover_len = 0;
    bool primed_cover_is_png = false;
    bool cover_primed = false;

    // 再读当前首 raw 封面。当前首一定马上要播放，所以放在 audio_service_play() 前面最后做。
    const bool allow_prime_current_cover_before_play =
        !promoted_next_cover &&
        asset_job.need_cover &&
        !cover_cache_hit &&
        asset_job.cover_source != COVER_NONE &&
        asset_job.cover_size > 0 &&
        asset_job.cover_size <= 96 * 1024;

    LOGI("[PLAYER] current raw check track=%d allow=%d promoted=%d need_cover=%d cache=%d source=%u size=%u",
        s_cur,
        allow_prime_current_cover_before_play ? 1 : 0,
        promoted_next_cover ? 1 : 0,
        asset_job.need_cover ? 1 : 0,
        cover_cache_hit ? 1 : 0,
        (unsigned)asset_job.cover_source,
        (unsigned)asset_job.cover_size);
        
    if (allow_prime_current_cover_before_play) {
        const uint32_t t_prime_cover_begin = millis();

        const bool primed_cover_fetch_ok =
            audio_service_fetch_cover(asset_job.cover_source,
                                    asset_job.audio_path,
                                    asset_job.cover_path,
                                    asset_job.cover_offset,
                                    asset_job.cover_size,
                                    &primed_cover_buf,
                                    &primed_cover_len,
                                    &primed_cover_is_png,
                                    true);

        const uint32_t prime_cover_cost = millis() - t_prime_cover_begin;

        if (primed_cover_fetch_ok &&
            primed_cover_buf &&
            primed_cover_len > 0 &&
            player_assets_prime_current_cover(s_cur,
                                            primed_cover_buf,
                                            primed_cover_len,
                                            primed_cover_is_png)) {
            primed_cover_buf = nullptr; // ownership moved
            cover_primed = true;

            LOGI("[PLAYER] current cover primed before play track=%d len=%u cost=%lu from_nfc=%d",
                s_cur,
                (unsigned)primed_cover_len,
                (unsigned long)prime_cover_cost,
                from_nfc ? 1 : 0);
        }

        if (primed_cover_buf) {
            ui_cover_free_allocated(primed_cover_buf);
            primed_cover_buf = nullptr;
        }
    }

    if (!audio_service_play(t.audio_path.c_str(), true)) {
        LOGE("[AUDIO] play failed");
        player_assets_clear_primed_current_cover();
        player_assets_drop_primed_next_cover();
        if (primed_cover_buf) {
            ui_cover_free_allocated(primed_cover_buf);
            primed_cover_buf = nullptr;
        }
        if (primed_lyrics_text) {
            ui_cover_free_allocated(reinterpret_cast<uint8_t*>(primed_lyrics_text));
            primed_lyrics_text = nullptr;
        }
        g_lyricsDisplay.clear();
        player_assets_reset_job(asset_job);
        return false;
    }

    const uint32_t t_after_play = millis();

    if (has_deferred_assets) {
        const bool req_lyrics = asset_job.need_lyrics;
        const bool req_cover = asset_job.need_cover;
        if (ui_get_view() == UI_VIEW_ROTATE) {
            ui_set_rotate_wait_prefetch(true);
        }
        player_assets_schedule(asset_job);
        LOGD("[PLAYER] deferred asset request armed track=%d lyrics=%d cover=%d",
             s_cur,
             req_lyrics ? 1 : 0,
             req_cover ? 1 : 0);
    } else {
        if (ui_get_view() == UI_VIEW_ROTATE) {
            ui_set_rotate_wait_prefetch(false);
        }
        player_assets_invalidate_requests();
        player_assets_reset_job(asset_job);
    }

    const uint32_t total_switch_ms = t_after_play - t_switch_begin;
    if (total_switch_ms >= 80) {
        LOGD("[PLAYER] switch timing stop=%lums ui_prepare=%lums lyrics_prefetch=%lums cover_prefetch_pre=%lums play=%lums to_audio=%lums deferred_cover=%d",
             (unsigned long)(t_after_stop - t_switch_begin),
             (unsigned long)(t_after_ui_prepare - t_after_stop),
             (unsigned long)(t_after_lyrics_prefetch - t_after_ui_prepare),
             0ul,
             (unsigned long)(t_after_play - t_after_lyrics_prefetch),
             (unsigned long)total_switch_ms,
             (has_deferred_assets && need_decode_cover) ? 1 : 0);
    }

    s_started = true;
    return true;
}

// 统一播放入口：切歌时会 stop->解封面->play；恢复播放时尽量复用封面
bool player_play_idx_v3(uint32_t idx, bool verbose, bool force_cover)
{
    if (!storage_catalog_v3_ready()) {
        LOGE("[PLAYER] V3 catalog not ready");
        return false;
    }

    const uint32_t total = storage_catalog_v3_track_count();
    if (total == 0) {
        LOGE("[PLAYER] no tracks in V3 catalog");
        return false;
    }

    if (idx >= total) idx = total - 1;

    TrackInfo t;
    if (!storage_catalog_v3_get_trackinfo(idx, t, "/Music")) {
        LOGE("[PLAYER] expand trackinfo failed, idx=%u", (unsigned)idx);
        return false;
    }

    return player_play_trackinfo_core(t, (int)idx, (int)total, verbose, force_cover);
}

void player_state_run(void)
{
    static bool entered = false;
    static bool boot_restore_pending = false;

    int track_count = player_track_count_for_dispatch();

    if (!entered) {
        entered = true;
        LOGI("[PLAYER] enter");
        ui_enter_player();
        s_cover_idx = -1;
        player_list_select_init_once();
        player_control_init_once();
        player_list_select_reset();

        LOGI("[SCAN] albums=%d tracks=%d",
             (int)storage_catalog_v3_album_count(),
             track_count);

        if (track_count <= 0) {
            LOGE("[PLAYER] no tracks");
            return;
        }

        player_playlist_seed_rng_once();
        player_recover_init_once();
        player_binding_init_once();
        
        player_playlist_force_rebuild();
        player_playlist_get_current();
        player_source_reset();

        boot_restore_pending = player_snapshot_begin_restore_on_player_enter();
        if (boot_restore_pending) {
            return;
        }

        int start_idx = player_clamp_idx_for_dispatch(V3_TEST_START_INDEX);
        if (start_idx < 0) {
            LOGE("[PLAYER] no playable tracks");
            return;
        }

        if (!player_play_idx_v3((uint32_t)start_idx, true, true)) {
            LOGE("[PLAYER] boot play failed");
        }
        return;
    }

    if (boot_restore_pending) {
        const PlayerSnapshotRestorePollResult restore_res = player_snapshot_poll_restore();
        if (restore_res == PLAYER_SNAPSHOT_RESTORE_WAITING) {
            return;
        }

        boot_restore_pending = false;
        if (restore_res == PLAYER_SNAPSHOT_RESTORE_DONE) {
            return;
        }

        LOGW("[PLAYER] boot restore fallback to default track");
        int start_idx = player_clamp_idx_for_dispatch(V3_TEST_START_INDEX);
        if (start_idx < 0) {
            LOGE("[PLAYER] no playable tracks after restore fallback");
            return;
        }
        if (!player_play_idx_v3((uint32_t)start_idx, true, true)) {
            LOGE("[PLAYER] boot play failed after restore fallback");
        }
        return;
    }

    if (g_rescan_done) {
        s_cover_idx = -1;
        player_list_select_reset();
    }
    if (player_recover_try_handle_rescan_done()) {
        return;
    }

    if (g_rescanning) return;

    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::NET_RADIO && source.radio_active) {
        audio_radio_backend_loop();
        const RadioBackendStatus rb = audio_radio_backend_get_status();
        String state = rb.paused ? String("paused") : (rb.connecting ? String("connecting") : (rb.running ? String("playing") : String("stopped")));
        player_source_set_radio_runtime(String(audio_radio_backend_name()), rb.stream_title, rb.bitrate, state, rb.active);
        if (!rb.station.isEmpty() && rb.station != source.radio_name) {
            RadioItem item{};
            item.valid = true;
            item.name = rb.station;
            item.url = source.radio_url;
            item.format = source.radio_format;
            item.region = source.radio_region;
            item.logo = source.radio_logo;
            player_source_set_radio_stub(source.radio_idx, item, state, rb.error);
            player_source_set_radio_runtime(String(audio_radio_backend_name()), rb.stream_title, rb.bitrate, state, rb.active);
        }
        if (!rb.active && !rb.connecting && !rb.running && !rb.paused) {
            player_source_set_radio_status(false, String("stopped"), rb.error);
        }
    }

    if (!g_rescanning) {
        nfc_poll();

        String uid;
        if (nfc_take_last_uid(uid)) {
            Serial.printf("[PLAYER] NFC uid=%s\n", uid.c_str());

            if (!player_binding_try_handle_nfc_uid(uid)) {
                LOGI("[NFC] no binding for uid=%s", uid.c_str());
            }
        }
    }

    player_list_select_tick();

    if (player_control_should_block_idle()) {
        return;
    }

    if (player_control_try_auto_next(entered, s_started)) {
        return;
    }

    // UiTask 已负责旋转推屏
}


int player_state_current_index(void)
{
    return s_cur;
}

void player_state_set_current_index(int idx)
{
    s_cur = idx;
}

void player_state_mark_next_play_from_nfc()
{
    s_next_play_from_nfc = true;
}
