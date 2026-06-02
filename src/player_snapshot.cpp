#include "player_snapshot.h"

#include <Preferences.h>
#include <string.h>

#include "app_flags.h"
#include "audio/audio.h"
#include "audio/audio_service.h"
#include "lyrics/lyrics.h"
#include "player_source.h"
#include "player_assets.h"
#include "player_control.h"
#include "player_playlist.h"
#include "player_recover.h"
#include "player_state.h"
#include "storage/storage_catalog_v3.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

static const char* kPrefsNs = "playerst";
static const char* kPrefsBlobKey = "snap";
static const uint8_t kSnapshotVersion = 2;
static const uint32_t kDeferredRestoreDelayMs = 450;

struct PlayerPersistSnapshotBlob {
    uint8_t version;
    uint8_t volume;
    uint8_t play_mode;
    uint8_t ui_view;
    int16_t current_group_idx;
    int32_t track_idx;
    uint8_t user_paused;
    char track_path[256];
};

bool s_has_pending = false;
bool s_restore_armed = false;
PlayerPersistSnapshot s_pending{};
uint32_t s_restore_not_before_ms = 0;


static play_mode_t snapshot_sanitize_mode(int raw)
{
    switch (raw) {
        case PLAY_MODE_ALL_SEQ:
        case PLAY_MODE_ALL_RND:
        case PLAY_MODE_ARTIST_SEQ:
        case PLAY_MODE_ARTIST_RND:
        case PLAY_MODE_ALBUM_SEQ:
        case PLAY_MODE_ALBUM_RND:
            return (play_mode_t)raw;
        default:
            return PLAY_MODE_ALL_SEQ;
    }
}

static int snapshot_sanitize_group_idx(play_mode_t mode, int group_idx)
{
    if (player_playlist_is_artist_mode(mode)) {
        const int count = (int)player_playlist_artist_groups().size();
        return (group_idx >= 0 && group_idx < count) ? group_idx : 0;
    }
    if (player_playlist_is_album_mode(mode)) {
        const int count = (int)player_playlist_album_groups().size();
        return (group_idx >= 0 && group_idx < count) ? group_idx : 0;
    }
    return -1;
}

static int snapshot_resolve_track_idx(const PlayerPersistSnapshot& snap)
{
    int idx = -1;
    if (!snap.track_path.isEmpty()) {
        idx = player_recover_find_track_idx_by_path(snap.track_path);
    }
    if (idx < 0) {
        idx = snap.track_idx;
    }

    const int total = (int)storage_catalog_v3_track_count();
    if (total <= 0) return -1;
    if (idx < 0 || idx >= total) return -1;
    return idx;
}

static void snapshot_sanitize_loaded(PlayerPersistSnapshot& snap)
{
    if (snap.volume > 100) snap.volume = 100;
    if (snap.ui_view != (uint8_t)UI_VIEW_ROTATE &&
        snap.ui_view != (uint8_t)UI_VIEW_INFO &&
        snap.ui_view != (uint8_t)UI_VIEW_COVER_PANEL) {
        snap.ui_view = (uint8_t)UI_VIEW_INFO;
    }
}

static void snapshot_log_loaded(const char* prefix, const PlayerPersistSnapshot& snap)
{
    LOGI("[SNAPSHOT] %s: mode=%u group=%d track=%d path=%s vol=%u view=%u paused=%d",
         prefix,
         (unsigned)snap.play_mode,
         snap.current_group_idx,
         snap.track_idx,
         snap.track_path.c_str(),
         (unsigned)snap.volume,
         (unsigned)snap.ui_view,
         (int)snap.user_paused);
}

static bool snapshot_read_blob(Preferences& pref, PlayerPersistSnapshot& out)
{
    const size_t len = pref.getBytesLength(kPrefsBlobKey);
    if (len != sizeof(PlayerPersistSnapshotBlob)) {
        return false;
    }

    PlayerPersistSnapshotBlob blob{};
    const size_t read_len = pref.getBytes(kPrefsBlobKey, &blob, sizeof(blob));
    if (read_len != sizeof(blob)) {
        LOGW("[SNAPSHOT] blob read size mismatch: got=%u expect=%u",
             (unsigned)read_len, (unsigned)sizeof(blob));
        return false;
    }
    if (blob.version != kSnapshotVersion && blob.version != 1) {
        LOGW("[SNAPSHOT] blob version unsupported: %u", (unsigned)blob.version);
        return false;
    }

    out.version = blob.version;
    out.volume = blob.volume;
    out.play_mode = blob.play_mode;
    out.current_group_idx = (int)blob.current_group_idx;
    out.track_idx = (int)blob.track_idx;
    out.ui_view = blob.ui_view;
    out.user_paused = (blob.user_paused != 0);
    out.track_path = String(blob.track_path);
    snapshot_sanitize_loaded(out);
    return true;
}



static void snapshot_apply_light_state(const PlayerPersistSnapshot& snap)
{
    const play_mode_t mode = snapshot_sanitize_mode((int)snap.play_mode);

    audio_set_volume(snap.volume);
    ui_set_volume(snap.volume);

    if ((uint8_t)ui_get_view() != snap.ui_view) {
        ui_set_view((ui_player_view_t)snap.ui_view);
    }

    g_play_mode = mode;
    player_playlist_set_current_group_idx(snapshot_sanitize_group_idx(mode, snap.current_group_idx));
    player_playlist_force_rebuild();
    player_playlist_get_current();
}

} // namespace

bool player_snapshot_load_pending_from_nvs()
{
    s_has_pending = false;
    s_restore_armed = false;
    s_restore_not_before_ms = 0;
    s_pending = PlayerPersistSnapshot{};

    Preferences pref;
    if (!pref.begin(kPrefsNs, true)) {
        LOGW("[SNAPSHOT] load skipped: open NVS namespace failed");
        return false;
    }

    bool ok = false;
    if (snapshot_read_blob(pref, s_pending)) {
        ok = true;
        snapshot_log_loaded("pending loaded from NVS blob", s_pending);
    }
    pref.end();

    if (!ok) {
        LOGI("[SNAPSHOT] no saved player snapshot in NVS");
        return false;
    }

    s_has_pending = true;
    return true;
}

bool player_snapshot_save_to_nvs()
{
    PlayerPersistSnapshot snap{};
    snap.version = kSnapshotVersion;
    snap.volume = audio_get_volume();
    snap.play_mode = (uint8_t)g_play_mode;
    snap.current_group_idx = player_playlist_get_current_group_idx();
    snap.track_idx = player_state_current_index();
    snap.ui_view = (uint8_t)ui_get_view();
    snap.user_paused = player_control_is_user_paused() || audio_service_is_paused();

    String path;
    if (player_recover_get_current_track_path(path)) {
        snap.track_path = path;
    }

    PlayerPersistSnapshotBlob blob{};
    blob.version = snap.version;
    blob.volume = snap.volume;
    blob.play_mode = snap.play_mode;
    blob.ui_view = snap.ui_view;
    blob.current_group_idx = (int16_t)snap.current_group_idx;
    blob.track_idx = (int32_t)snap.track_idx;
    blob.user_paused = snap.user_paused ? 1 : 0;
    snap.track_path.toCharArray(blob.track_path, sizeof(blob.track_path));

    Preferences pref;
    if (!pref.begin(kPrefsNs, false)) {
        LOGE("[SNAPSHOT] save failed: open NVS namespace");
        return false;
    }

    const size_t written = pref.putBytes(kPrefsBlobKey, &blob, sizeof(blob));
    pref.end();
    if (written != sizeof(blob)) {
        LOGE("[SNAPSHOT] save failed: blob write size=%u expect=%u",
             (unsigned)written, (unsigned)sizeof(blob));
        return false;
    }

    s_pending = snap;
    s_has_pending = true;
    snapshot_log_loaded("saved to NVS blob", snap);
    return true;
}

bool player_snapshot_begin_restore_on_player_enter()
{
    if (!s_has_pending) return false;
    if (!storage_catalog_v3_ready() || storage_catalog_v3_track_count() == 0) {
        LOGW("[SNAPSHOT] restore skipped: catalog not ready");
        s_has_pending = false;
        return false;
    }

    snapshot_apply_light_state(s_pending);
    s_restore_armed = true;
    s_restore_not_before_ms = millis() + kDeferredRestoreDelayMs;
    LOGI("[SNAPSHOT] light restore applied, deferred track restore in %ums",
         (unsigned)kDeferredRestoreDelayMs);
    return true;
}

PlayerSnapshotRestorePollResult player_snapshot_poll_restore()
{
    if (!s_restore_armed) {
        return PLAYER_SNAPSHOT_RESTORE_NONE;
    }

    const uint32_t now = millis();
    if ((int32_t)(now - s_restore_not_before_ms) < 0) {
        return PLAYER_SNAPSHOT_RESTORE_WAITING;
    }

    s_restore_armed = false;

    if (!storage_catalog_v3_ready() || storage_catalog_v3_track_count() == 0) {
        LOGW("[SNAPSHOT] deferred restore failed: catalog not ready");
        s_has_pending = false;
        return PLAYER_SNAPSHOT_RESTORE_FAILED;
    }

    const PlayerPersistSnapshot snap = s_pending;
    const int track_idx = snapshot_resolve_track_idx(snap);
    s_has_pending = false;
    if (track_idx < 0) {
        LOGW("[SNAPSHOT] deferred restore skipped: saved track missing, path=%s idx=%d",
             snap.track_path.c_str(), snap.track_idx);
        return PLAYER_SNAPSHOT_RESTORE_FAILED;
    }

    TrackInfo t;
    if (!storage_catalog_v3_get_trackinfo((uint32_t)track_idx, t, "/Music")) {
        LOGE("[SNAPSHOT] deferred UI-only restore expand trackinfo failed: idx=%d", track_idx);
        return PLAYER_SNAPSHOT_RESTORE_FAILED;
    }

    // 开机恢复只恢复 UI，不启动音频。
    // 关键：不要调用 player_play_idx_v3()，否则会先出声再被暂停。
    player_control_mark_user_paused();

    // 同步当前索引，保证后续按 PLAY 能从这首歌开始。
    player_state_set_current_index(track_idx);

    player_source_set_local_track(track_idx);

    (void)player_playlist_align_group_context_for_track(track_idx, true);
    player_playlist_update_for_current_track(track_idx, true);

    // 恢复标题 / 歌手 / 专辑 / 模式 / 音量
    ui_set_now_playing(t.title.c_str(), t.artist.c_str());
    ui_set_album(t.album);
    ui_set_play_mode(g_play_mode);
    ui_set_volume(audio_get_volume());

    // 恢复曲目位置显示
    {
        const int total = (int)storage_catalog_v3_track_count();
        int display_pos = track_idx;
        int display_total = total;

        if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND ||
            g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
            const PlayerPlaylistDisplayInfo display =
                player_playlist_get_display_info(track_idx, total);
            display_total = display.display_total;
            display_pos = display.display_pos;
        }

        ui_set_track_pos(display_pos, display_total);
    }

    // 清空旧歌词，避免残留上一首
    g_lyricsDisplay.clear();

    // 先尝试应用封面缓存
    const bool cover_cache_hit = ui_cover_apply_cached(track_idx);
    if (cover_cache_hit) {
        ui_request_refresh_now();
    }

    // 只补当前歌曲的歌词 / 封面，不取总时长，不预读下一首
    PlayerDeferredAssetJob asset_job{};
    const bool need_decode_cover = !cover_cache_hit;

    const bool has_deferred_assets = player_assets_prepare_deferred_request(
        t,
        track_idx,
        false,                         // need_total：不开音频，不取总时长
        t.lrc_path.length() > 0,        // need_lyrics
        need_decode_cover,              // need_cover
        asset_job);

    const bool allow_boot_next_prefetch =
        storage_catalog_v3_track_count() > 1;

    asset_job.need_total = false;
    asset_job.need_lyrics = (t.lrc_path.length() > 0);
    asset_job.need_cover = need_decode_cover;

    // 开机没有播放音频，可以允许预读下一首封面。
    // 但仍然由 PlayerAssetTask 异步执行，不要在恢复流程里同步解码。
    asset_job.suppress_next_prefetch = !allow_boot_next_prefetch;

    if (has_deferred_assets) {
        player_assets_schedule(asset_job);
    } else if (allow_boot_next_prefetch) {
        // 当前首没有封面/歌词需要补，也仍然发一个空 job，
        // 目的只是进入 PlayerAssetTask，让它执行“下一首封面预读”。
        player_assets_reset_job(asset_job);
        asset_job.track_idx = track_idx;
        asset_job.need_total = false;
        asset_job.need_lyrics = false;
        asset_job.need_cover = false;
        asset_job.suppress_next_prefetch = false;

        player_assets_schedule(asset_job);

        LOGI("[SNAPSHOT] boot next-cover-only prefetch job armed track=%d", track_idx);
    }

    ui_request_refresh_now();

    LOGI("[SNAPSHOT] deferred UI-only restore done: mode=%d group=%d track=%d path=%s vol=%u view=%u cover_hit=%d lyrics=%d cover=%d",
        (int)g_play_mode,
        player_playlist_get_current_group_idx(),
        track_idx,
        snap.track_path.c_str(),
        (unsigned)snap.volume,
        (unsigned)snap.ui_view,
        cover_cache_hit ? 1 : 0,
        asset_job.need_lyrics ? 1 : 0,
        asset_job.need_cover ? 1 : 0);

    return PLAYER_SNAPSHOT_RESTORE_DONE;
}
