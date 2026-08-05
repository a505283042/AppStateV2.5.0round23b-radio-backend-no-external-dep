#include "player_state.h"

#include <Arduino.h>

#include "app_flags.h"
#include "app_alarm.h"
#include "audio/audio.h"
#include "audio/audio_output_route.h"
#include "audio/audio_service.h"
#include "nfc/nfc.h"
#include "nfc/nfc_binding.h"
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
#include "storage/storage.h"
#include "storage/storage_config_writer.h"
#include "storage/system_paths.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

static constexpr uint8_t ALARM_VOLUME_RAMP_START = 5;
static constexpr uint8_t ALARM_VOLUME_RAMP_STEP = 2;
static constexpr uint32_t ALARM_VOLUME_RAMP_INTERVAL_MS = 500;

struct AlarmVolumeRampState {
    bool active = false;
    uint8_t current = 0;
    uint8_t target = 0;
    uint32_t next_step_ms = 0;
};

static AlarmVolumeRampState s_alarm_volume_ramp{};

static void player_cancel_alarm_wakeup_volume_ramp(const char* reason)
{
    if (!s_alarm_volume_ramp.active) {
        return;
    }

    LOGI("[闹钟] 音量渐增已停止：%s 当前=%u 目标=%u",
         reason ? reason : "未指定",
         (unsigned)s_alarm_volume_ramp.current,
         (unsigned)s_alarm_volume_ramp.target);
    s_alarm_volume_ramp = AlarmVolumeRampState{};
}

static bool player_begin_alarm_wakeup_volume_ramp()
{
    const uint8_t target = app_alarm_wakeup_volume() > 100
        ? 100
        : app_alarm_wakeup_volume();
    const uint8_t start = target > ALARM_VOLUME_RAMP_START
        ? ALARM_VOLUME_RAMP_START
        : target;

    if (!audio_output_route_set_user_volume(start)) {
        LOGW("[闹钟] 初始音量设置失败：起始=%u 目标=%u",
             (unsigned)start,
             (unsigned)target);
        s_alarm_volume_ramp = AlarmVolumeRampState{};
        return false;
    }

    s_alarm_volume_ramp.current = start;
    s_alarm_volume_ramp.target = target;
    s_alarm_volume_ramp.active = start < target;
    s_alarm_volume_ramp.next_step_ms = millis() + ALARM_VOLUME_RAMP_INTERVAL_MS;

    LOGI("[闹钟] 已设置渐增音量：起始=%u 目标=%u 步进=%u/%lums",
         (unsigned)start,
         (unsigned)target,
         (unsigned)ALARM_VOLUME_RAMP_STEP,
         (unsigned long)ALARM_VOLUME_RAMP_INTERVAL_MS);
    return true;
}

static void player_alarm_wakeup_volume_ramp_tick()
{
    if (!s_alarm_volume_ramp.active) {
        return;
    }

    // 等实际音频开始后再递增，避免启动和曲库恢复耗时消耗掉渐增过程。
    if (!audio_service_is_playing() || audio_service_is_paused()) {
        return;
    }

    // 用户通过旋钮或 Web 主动改音量后，不再用闹钟渐增覆盖用户选择。
    if (!audio_output_route_is_bluetooth_tx() ||
        audio_output_route_bluetooth_tx_volume_known()) {
        const uint8_t actual = audio_output_route_get_user_volume();
        if (actual != s_alarm_volume_ramp.current) {
            player_cancel_alarm_wakeup_volume_ramp("检测到用户或输出路线修改音量");
            return;
        }
    }

    const uint32_t now = millis();
    if ((int32_t)(now - s_alarm_volume_ramp.next_step_ms) < 0) {
        return;
    }

    uint16_t next = (uint16_t)s_alarm_volume_ramp.current + ALARM_VOLUME_RAMP_STEP;
    if (next > s_alarm_volume_ramp.target) {
        next = s_alarm_volume_ramp.target;
    }

    if (!audio_output_route_set_user_volume((uint8_t)next)) {
        s_alarm_volume_ramp.next_step_ms = now + ALARM_VOLUME_RAMP_INTERVAL_MS;
        LOGW("[闹钟] 音量渐增请求失败，将重试：当前=%u 目标=%u",
             (unsigned)s_alarm_volume_ramp.current,
             (unsigned)s_alarm_volume_ramp.target);
        return;
    }

    s_alarm_volume_ramp.current = (uint8_t)next;
    s_alarm_volume_ramp.next_step_ms = now + ALARM_VOLUME_RAMP_INTERVAL_MS;
    if (s_alarm_volume_ramp.current >= s_alarm_volume_ramp.target) {
        LOGI("[闹钟] 音量渐增完成：目标=%u",
             (unsigned)s_alarm_volume_ramp.target);
        s_alarm_volume_ramp = AlarmVolumeRampState{};
    }
}

} // namespace

static void player_mark_alarm_wakeup_handled(const char* reason)
{
    app_alarm_mark_wakeup_handled(reason);
}

static bool player_handle_alarm_wake_only_boot()
{
    if (!app_alarm_should_block_player_boot_autoplay()) {
        return false;
    }

    // 闹钟开机只保留一个弹窗提示，避免“整页提示 + 弹窗”重复。
    ui_show_alarm_wakeup_popup("闹钟已唤醒", "只开机，不播放");
    player_mark_alarm_wakeup_handled("只开机");
    return true;
}

static void player_auto_resume_alarm_after_snapshot()
{
    if (!app_alarm_should_auto_resume_last()) {
        return;
    }

    player_begin_alarm_wakeup_volume_ramp();
    // 闹钟开机只保留一个弹窗提示，播放器界面继续负责显示封面/播放状态。
    ui_show_alarm_wakeup_popup("闹钟已响", "正在恢复播放");
    player_toggle_play(PlayerToggleTrigger::Alarm);
    player_mark_alarm_wakeup_handled("恢复上次播放");
}

static const char* player_nfc_bind_type_label(NfcBindType type)
{
    switch (type) {
        case NFC_BIND_TRACK:  return "单曲";
        case NFC_BIND_ARTIST: return "歌手";
        case NFC_BIND_ALBUM:  return "专辑";
        default:              return "未知";
    }
}

static void player_show_nfc_scan_result_popup(const String& uid,
                                              const String& card_type,
                                              PlayerSourceType source_type)
{
    const bool network_source = source_type == PlayerSourceType::NET_TRACK ||
                                source_type == PlayerSourceType::NET_RADIO;

    NfcBindingEntry entry;
    if (nfc_binding_find(uid, entry)) {
        String name = entry.display;
        name.trim();
        if (name.isEmpty()) {
            name = entry.key;
        }

        String bind_type = player_nfc_bind_type_label(entry.type);
        if (network_source) {
            bind_type = String("回本地·") + bind_type;
            LOGI("[NFC] 网络音源刷到已绑定卡，将切回本地播放：UID=%s 类型=%s",
                 uid.c_str(),
                 player_nfc_bind_type_label(entry.type));
        }

        ui_show_nfc_scan_popup(uid,
                               card_type,
                               bind_type,
                               name,
                               true);
    } else {
        String hint = "按住旋钮+长按上一曲绑定";
        if (source_type == PlayerSourceType::NET_TRACK) {
            hint = "NAS播放时不能绑定";
        } else if (source_type == PlayerSourceType::NET_RADIO) {
            hint = "电台播放时不能绑定";
        }

        ui_show_nfc_scan_popup(uid,
                               card_type,
                               "未绑定",
                               hint,
                               false);
    }
}

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
static int player_assets_hook_get_current_track_idx();
static void player_assets_hook_on_current_cover_ready(int track_idx);
static void player_assets_init_once();
static bool player_list_select_hook_play_track_dispatch(int idx,
                                                        int group_idx,
                                                        bool verbose,
                                                        bool force_cover);
static bool player_list_select_hook_play_radio_dispatch(int idx);
static bool player_list_select_hook_play_net_track_dispatch(int idx);
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


static bool player_list_select_hook_play_track_dispatch(int idx,
                                                        int group_idx,
                                                        bool verbose,
                                                        bool force_cover)
{
    if (idx < 0) return false;
    return player_request_user_track_play(idx,
                                          PlayerUserTrackContext::KeepCurrent,
                                          group_idx,
                                          verbose,
                                          force_cover);
}

static bool player_list_select_hook_play_radio_dispatch(int idx)
{
    if (idx < 0) return false;
    return player_request_user_radio_play(idx);
}

static bool player_list_select_hook_play_net_track_dispatch(int idx)
{
    if (idx < 0) return false;
    return player_request_user_net_track_play(idx);
}

static void player_list_select_init_once()
{
    if (s_player_list_hooks_inited) return;

    PlayerListSelectHooks hooks{};
    hooks.play_track_dispatch = &player_list_select_hook_play_track_dispatch;
    hooks.play_radio_dispatch = &player_list_select_hook_play_radio_dispatch;
    hooks.play_net_track_dispatch = &player_list_select_hook_play_net_track_dispatch;
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
    return player_list_select_enter(app_play_mode_get());
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

    const PlayerSourceState before_source = player_source_get();
    if (before_source.type != PlayerSourceType::LOCAL_TRACK) {
        // 从 NAS/电台切回本地时，先保留 NAS 状态，再恢复本地自己的模式和分组。
        (void)player_snapshot_capture_current_source();
        (void)player_snapshot_apply_local_context();
    }

    if (before_source.type == PlayerSourceType::NET_RADIO) {
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
    // 不在这里调用 audio_service_resume()。
    // 切歌播放命令会在 AudioTask 内部清除暂停状态，并在软件预填充完成后再取消静音。
    // 如果这里提前 resume，会先把功放打开，然后 AudioTask 又立即静音，造成 OFF->ON->OFF 的抖动。

    LOGI("[播放器] 播放 #%d：%s", s_cur, t.audio_path.c_str());

    if (verbose) {
        Serial.println("----- TRACK META CHECK -----");
        Serial.printf("路径  : %s\n", t.audio_path.c_str());
        Serial.printf("ext   : %s\n", t.ext.c_str());
        Serial.printf("歌手: %s\n", t.artist.c_str());
        Serial.printf("专辑 : %s\n", t.album.c_str());
        Serial.printf("title : %s\n", t.title.c_str());
        Serial.println("---------------------------");

        Serial.printf("封面_来源=%d 偏移=%u 大小=%u mime=%s 路径=%s\n",
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
    bool old_audio_closed = true;
    if (audio_service_is_playing() || audio_service_is_paused()) {
        old_audio_closed = audio_service_stop(true);
    }
    t_after_stop = millis();

    // 新歌开始时，先取消上一首遗留的封面预取请求。
    player_assets_cancel_pending_cover_prefetch();

    // 只有确认旧音频已关闭才允许写TF卡；停止超时时继续保留PSRAM内容。
    if (old_audio_closed) {
        (void)storage_config_commit_pending(4000);
    } else if (storage_config_has_pending()) {
        LOGW("[配置文件] 本次切歌未确认旧音频关闭，跳过待写配置提交");
    }

    // 当前曲目标记更新后，先清掉旧歌词；封面先沿用上一首/占位图，避免把“能后补”的事情挡在开播前。
    g_lyricsDisplay.clear();

    // 先尝试命中“上一轮预读好的下一首封面缓存”。命中后当前封面可瞬时切上来。
    const bool cover_cache_hit = ui_cover_apply_cached(s_cur);
    if (cover_cache_hit) {
        s_cover_idx = s_cur;
        ui_request_refresh_now();
        LOGD("[播放器] 当前 封面 缓存 命中 歌曲=%d", s_cur);
    }

    bool need_decode_cover = (force_cover || s_cover_idx != s_cur) && !cover_cache_hit;

    // 先提交文字、模式、序号，让 UI 立刻跟上；不再等待封面/歌词准备完。
    ui_set_now_playing(t.title.c_str(), t.artist.c_str());
    // 手动切歌/自动下一曲时，给全屏旋转视图一个短暂的歌名/歌手提示。
    // NFC 刷卡播放已经有“刷卡结果弹窗”，这里不再重复弹歌名提示。
    if (!from_nfc) {
        ui_show_track_change_popup(t.title.c_str(), t.artist.c_str());
    }
    ui_set_album(t.album);

    // 根据播放模式显示正确的歌曲索引和总数
    int display_pos = s_cur;
    int display_total = (library_total_hint > 0) ? library_total_hint : (int)storage_catalog_v3_track_count();

    const play_mode_t play_mode = app_play_mode_get();
    if (play_mode == PLAY_MODE_ARTIST_SEQ || play_mode == PLAY_MODE_ARTIST_RND ||
        play_mode == PLAY_MODE_ALBUM_SEQ || play_mode == PLAY_MODE_ALBUM_RND) {
        const PlayerPlaylistDisplayInfo display = player_playlist_get_display_info(s_cur, library_total_hint);
        display_total = display.display_total;
        display_pos = display.display_pos;
    }

    ui_set_track_pos(display_pos, display_total);
    ui_set_play_mode(play_mode);
    audio_output_route_sync_ui_volume();
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

    LOGD("[播放器] prime check 歌曲=%d 封面_缓存_命中=%d need_解码_封面=%d 封面_来源=%u 封面_大小=%u",
     s_cur,
     cover_cache_hit ? 1 : 0,
     need_decode_cover ? 1 : 0,
     (unsigned)asset_job.cover_source,
     (unsigned)asset_job.cover_size);

    const bool promoted_next_cover =
        (!cover_cache_hit && need_decode_cover) ?
        player_assets_promote_next_cover_to_current(s_cur) :
        false;

    LOGD("[播放器] promote 下一首 原始检查 歌曲=%d promoted=%d",
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

    LOGD("[播放器] 下一首原始封面检查：当前=%d 允许=%d 获取=%d 目标=%d 来自NFC=%d 准备=%lums 限制=%lums 缓存=%d 来源=%u 大小=%u 大小限制=%lu",
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

        LOGD("[播放器] 当前播放前预读下一首原始封面：目标=%d 成功=%d 长度=%u 耗时=%lums 准备=%lums",
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
                LOGD("[播放器] 播放前歌词已预读 歌曲=%d le数量=%u",
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

    LOGD("[播放器] 当前 原始检查 歌曲=%d 允许=%d promoted=%d need_封面=%d 缓存=%d 来源=%u 大小=%u",
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
            primed_cover_len > 0) {
            // 当前封面未命中缓存时，直接在播放前完成解码/缩放并写入 UI cache。
            // 这样会让本次切歌出声稍晚一点，但能避免 [AUDIO] play 后
            // 当前封面缩放和 FLAC 开头解码同时抢 CPU/PSRAM，造成音频顿一下。
            const uint32_t t_scale_begin = millis();
            const bool scaled_ok = ui_cover_scale_to_cache_from_buffer(primed_cover_buf,
                                                                       primed_cover_len,
                                                                       primed_cover_is_png,
                                                                       s_cur);
            const uint32_t scale_cost = millis() - t_scale_begin;

            if (scaled_ok) {
                cover_primed = true;
                // 屏幕封面已经在播放前缩放进 UI cache，但不要关闭 asset_job.need_cover。
                // 后台资源任务还需要走一次 ui_cover_apply_cached()，把 UI cache 转成网页端 BMP 缓存。
                // 否则 NFC 刷卡播放时本机有封面，网页端 cover_ready_for_web 一直为 false，显示“封面加载中”。
                need_decode_cover = false;
                (void)ui_cover_apply_cached(s_cur);
                s_cover_idx = s_cur;
                ui_request_refresh_now();
                player_assets_clear_deferred_current_cover_apply();

                LOGD("[播放器] 当前 封面 播放前已缩放 歌曲=%d le数量=%u fetch=%lu 缩放=%lu from_nfc=%d",
                    s_cur,
                    (unsigned)primed_cover_len,
                    (unsigned long)prime_cover_cost,
                    (unsigned long)scale_cost,
                    from_nfc ? 1 : 0);
            } else if (player_assets_prime_current_cover(s_cur,
                                                        primed_cover_buf,
                                                        primed_cover_len,
                                                        primed_cover_is_png)) {
                // 缩放失败时退回旧路径，把 raw 交给资源任务，避免封面完全丢失。
                primed_cover_buf = nullptr; // ownership moved
                cover_primed = true;
                LOGW("[播放器] 当前 封面 缩放 before 播放失败, defer raw 歌曲=%d le数量=%u fetch=%lu",
                     s_cur,
                     (unsigned)primed_cover_len,
                     (unsigned long)prime_cover_cost);
            }
        }

        if (primed_cover_buf) {
            ui_cover_free_allocated(primed_cover_buf);
            primed_cover_buf = nullptr;
        }
    }

    if (!audio_service_play(t.audio_path.c_str(), true)) {
        LOGE("[音频] 播放失败");
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
        if (ui_get_view() == UI_VIEW_ROTATE || ui_get_view() == UI_VIEW_COVER_PANEL) {
            ui_set_rotate_wait_prefetch(true);
        }
        player_assets_schedule(asset_job);
        LOGD("[播放器] 延迟 as设置 request armed 歌曲=%d 歌词=%d 封面=%d",
             s_cur,
             req_lyrics ? 1 : 0,
             req_cover ? 1 : 0);
    } else {
        if (ui_get_view() == UI_VIEW_ROTATE || ui_get_view() == UI_VIEW_COVER_PANEL) {
            ui_set_rotate_wait_prefetch(false);
        }
        player_assets_invalidate_requests();
        player_assets_reset_job(asset_job);
    }

    const uint32_t total_switch_ms = t_after_play - t_switch_begin;
    if (total_switch_ms >= 80) {
        LOGD("[播放器] 切换耗时 停止=%lums 界面准备=%lums 歌词预取=%lums 封面预取=%lums play=%lums 送入音频=%lums 延迟封面=%d",
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
        LOGE("[播放器] V3 目录 未就绪");
        return false;
    }

    const uint32_t total = storage_catalog_v3_track_count();
    if (total == 0) {
        LOGE("[播放器] no 歌曲s in V3 目录");
        return false;
    }

    if (idx >= total) idx = total - 1;

    TrackInfo t;
    if (!storage_catalog_v3_get_trackinfo(idx, t, "/Music")) {
        LOGE("[播放器] 展开歌曲信息失败：索引=%u", (unsigned)idx);
        return false;
    }

    return player_play_trackinfo_core(t, (int)idx, (int)total, verbose, force_cover);
}

static bool player_apply_default_cover_for_idle_ui()
{
    uint8_t* buf = nullptr;
    size_t len = 0;
    bool is_png = false;

    const bool ok = audio_service_fetch_cover(COVER_FILE_FALLBACK,
                                              "",
                                              SystemPaths::kDefaultCover,
                                              0,
                                              0,
                                              &buf,
                                              &len,
                                              &is_png,
                                              true);
    if (!ok || !buf || len == 0) {
        if (buf) ui_cover_free_allocated(buf);
        return false;
    }

    const bool scaled = ui_cover_scale_from_buffer(buf, len, is_png);
    ui_cover_free_allocated(buf);
    return scaled;
}

bool player_prepare_local_track_ui(int idx)
{
    if (!storage_catalog_v3_ready()) {
        LOGE("[播放器] 准备停播界面失败：V3 曲库未就绪");
        return false;
    }

    const int total = (int)storage_catalog_v3_track_count();
    if (idx < 0 || idx >= total) {
        LOGE("[播放器] 准备停播界面失败：索引越界 idx=%d total=%d", idx, total);
        return false;
    }

    TrackInfo t;
    if (!storage_catalog_v3_get_trackinfo((uint32_t)idx, t, "/Music")) {
        LOGE("[播放器] 准备停播界面失败：展开歌曲信息失败 idx=%d", idx);
        return false;
    }

    player_assets_init_once();
    player_control_init_once();
    player_recover_init_once();
    player_binding_init_once();

    // 这里只预选歌曲并恢复播放器页面，不启动音频。
    player_control_mark_user_paused();
    s_cur = idx;
    player_source_set_local_track(idx);

    (void)player_playlist_align_group_context_for_track(idx, true);
    player_playlist_update_for_current_track(idx, true);

    ui_set_now_playing(t.title.c_str(), t.artist.c_str());
    ui_set_album(t.album);
    ui_set_play_mode(app_play_mode_get());
    audio_output_route_sync_ui_volume();

    const PlayerPlaylistDisplayInfo display =
        player_playlist_get_display_info(idx, total);
    ui_set_track_pos(display.display_pos, display.display_total);

    g_lyricsDisplay.clear();
    player_assets_invalidate_requests();

    const bool cover_cache_hit = ui_cover_apply_cached(idx);
    if (cover_cache_hit) {
        s_cover_idx = idx;
    } else {
        s_cover_idx = -1;
        (void)player_apply_default_cover_for_idle_ui();
    }

    PlayerDeferredAssetJob asset_job{};
    const bool need_decode_cover = !cover_cache_hit;
    const bool has_deferred_assets = player_assets_prepare_deferred_request(
        t,
        idx,
        false,
        t.lrc_path.length() > 0,
        need_decode_cover,
        asset_job);

    const bool allow_next_prefetch = total > 1;
    asset_job.need_total = false;
    asset_job.need_lyrics = t.lrc_path.length() > 0;
    asset_job.need_cover = need_decode_cover;
    asset_job.suppress_next_prefetch = !allow_next_prefetch;

    if (has_deferred_assets) {
        player_assets_schedule(asset_job);
    } else if (allow_next_prefetch) {
        player_assets_reset_job(asset_job);
        asset_job.track_idx = idx;
        asset_job.need_total = false;
        asset_job.need_lyrics = false;
        asset_job.need_cover = false;
        asset_job.suppress_next_prefetch = false;
        player_assets_schedule(asset_job);
    }

    ui_request_refresh_now();
    LOGI("[播放器] 已预选歌曲并进入停播界面：索引=%d 标题=%s",
         idx,
         t.title.c_str());
    return true;
}

void player_state_run(void)
{
    player_alarm_wakeup_volume_ramp_tick();

    static bool entered = false;
    static bool boot_restore_pending = false;

    int track_count = player_track_count_for_dispatch();

    if (!entered) {
        entered = true;
        LOGI("[播放器] 进入播放器状态");
        ui_enter_player();
        s_cover_idx = -1;
        player_assets_init_once();
        player_list_select_init_once();
        player_control_init_once();
        player_list_select_reset();

        if (player_handle_alarm_wake_only_boot()) {
            return;
        }

        LOGD("[扫描] 专辑=%d 歌曲=%d",
             (int)storage_catalog_v3_album_count(),
             track_count);

        if (track_count <= 0) {
            LOGE("[播放器] no 歌曲s");

            if (!storage_is_ready()) {
                ui_show_player_placeholder("未插入TF卡", "插卡后自动加载");
            } else {
                ui_show_player_placeholder("没有歌曲", "请检查 /Music 目录");
            }

            if (app_alarm_wakeup_pending()) {
                player_mark_alarm_wakeup_handled("无可播放内容");
            }
            return;
        }

        player_playlist_seed_rng_once();
        player_recover_init_once();
        player_binding_init_once();
        
        player_source_reset();

        // 如果有开机快照，player_snapshot_begin_restore_on_player_enter()
        // 内部会先恢复播放模式 / 分组，并重建一次播放列表。
        // 因此这里不要提前重建，避免启动时 playlist_pos 重复构建两次。
        boot_restore_pending = player_snapshot_begin_restore_on_player_enter();
        if (boot_restore_pending) {
            return;
        }

        // 没有快照时才按默认播放模式构建启动播放列表。
        player_playlist_force_rebuild();
        player_playlist_ensure_current();

        int start_idx = player_clamp_idx_for_dispatch(V3_TEST_START_INDEX);
        if (start_idx < 0) {
            LOGE("[播放器] no playable 歌曲s");
            return;
        }

        if (app_alarm_should_auto_resume_last()) {
            player_begin_alarm_wakeup_volume_ramp();
            ui_show_alarm_wakeup_popup("闹钟已响", "正在播放默认歌曲");
        }
        if (!player_play_idx_v3((uint32_t)start_idx, true, true)) {
            player_cancel_alarm_wakeup_volume_ramp("默认歌曲起播失败");
            LOGE("[播放器] boot 播放失败");
        }
        if (app_alarm_should_auto_resume_last()) {
            player_mark_alarm_wakeup_handled("无快照时播放默认歌曲");
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
            player_auto_resume_alarm_after_snapshot();
            return;
        }

        LOGW("[播放器] 开机恢复失败，回退到默认歌曲");
        int start_idx = player_clamp_idx_for_dispatch(V3_TEST_START_INDEX);
        if (start_idx < 0) {
            LOGE("[播放器] 恢复回退后仍没有可播放歌曲");
            return;
        }
        if (app_alarm_should_auto_resume_last()) {
            player_begin_alarm_wakeup_volume_ramp();
            ui_show_alarm_wakeup_popup("闹钟已响", "正在播放默认歌曲");
        }
        if (!player_play_idx_v3((uint32_t)start_idx, true, true)) {
            player_cancel_alarm_wakeup_volume_ramp("恢复回退起播失败");
            LOGE("[播放器] boot 播放失败 恢复回退后");
        }
        if (app_alarm_should_auto_resume_last()) {
            player_mark_alarm_wakeup_handled("恢复失败后播放默认歌曲");
        }
        return;
    }

    const AppRescanState rescan = app_rescan_state_get();
    if (rescan.done) {
        s_cover_idx = -1;
        player_list_select_reset();
    }
    if (player_recover_try_handle_rescan_done()) {
        return;
    }

    if (rescan.rescanning) return;

    // 播放器主循环只读取纯数值运行态，避免每轮复制包含多个 String 的完整音源状态。
    const PlayerSourceRuntimeState source = player_source_runtime_get();
    if (source.type == PlayerSourceType::NET_RADIO && source.radio_active) {
        audio_radio_backend_loop();
        const RadioBackendStatus rb = audio_radio_backend_get_status();
        const char* state = !rb.error.isEmpty()
            ? "error"
            : (rb.paused
                ? "paused"
                : (rb.retrying
                    ? "reconnecting"
                    : (rb.connecting ? "connecting" : (rb.running ? "playing" : "stopped"))));

        // 电台名称和运行态统一在 player_source 内部按变化更新，
        // 不再为名称比较复制整份 PlayerSourceState，也不再每轮构造临时 String。
        player_source_set_radio_runtime(audio_radio_backend_name(),
                                        rb.station,
                                        rb.stream_title,
                                        rb.bitrate,
                                        state,
                                        rb.active,
                                        rb.error);
    }

    if (!app_rescan_state_get().rescanning) {
        nfc_poll();

        String uid;
        String card_type;
        if (nfc_take_last_card_info(uid, card_type)) {
            Serial.printf("[播放器] NFC uid=%s type=%s\n", uid.c_str(), card_type.c_str());

            // 先记住刷卡瞬间的来源，再执行绑定播放。网络绑定会立即切回本地，
            // 因此弹窗必须在动作之后显示，同时使用动作前来源生成“回本地”提示。
            const PlayerSourceType source_before_scan = player_source_type_get();
            const bool handled = player_binding_try_handle_nfc_uid(uid);

            player_show_nfc_scan_result_popup(uid,
                                              card_type,
                                              source_before_scan);

            if (!handled) {
                LOGI("[NFC] UID 未绑定：%s", uid.c_str());
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
