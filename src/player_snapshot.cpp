#include "player_snapshot.h"

#include <Preferences.h>
#include <string.h>

#include "app_flags.h"
#include "audio/audio.h"
#include "audio/audio_service.h"
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
static const char* kPrefsInit = "init";   // round14 兼容读取
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

static bool snapshot_mode_is_random(play_mode_t mode)
{
    return mode == PLAY_MODE_ALL_RND ||
           mode == PLAY_MODE_ARTIST_RND ||
           mode == PLAY_MODE_ALBUM_RND;
}

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
    if (snap.ui_view != (uint8_t)UI_VIEW_ROTATE && snap.ui_view != (uint8_t)UI_VIEW_INFO) {
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

static bool snapshot_read_legacy_keys(Preferences& pref, PlayerPersistSnapshot& out)
{
    const bool inited = pref.getBool(kPrefsInit, false);
    if (!inited) {
        return false;
    }

    out.version = pref.getUChar("ver", 1);
    out.volume = pref.getUChar("vol", 100);
    out.play_mode = pref.getUChar("mode", (uint8_t)PLAY_MODE_ALL_SEQ);
    out.current_group_idx = pref.getInt("group", -1);
    out.track_idx = pref.getInt("track", -1);
    out.track_path = pref.getString("path", "");
    out.ui_view = pref.getUChar("view", (uint8_t)UI_VIEW_INFO);
    out.user_paused = pref.getBool("paused", true);
    snapshot_sanitize_loaded(out);
    return true;
}

static void snapshot_apply_light_state(const PlayerPersistSnapshot& snap)
{
    const play_mode_t mode = snapshot_sanitize_mode((int)snap.play_mode);

    audio_set_volume(snap.volume);
    ui_set_volume(snap.volume);

    if ((uint8_t)ui_get_view() != snap.ui_view) {
        ui_toggle_view();
    }

    g_play_mode = mode;
    g_random_play = snapshot_mode_is_random(mode);
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
    } else if (snapshot_read_legacy_keys(pref, s_pending)) {
        ok = true;
        snapshot_log_loaded("pending loaded from legacy NVS keys", s_pending);
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

    if (!player_play_idx_v3((uint32_t)track_idx, true, true)) {
        LOGE("[SNAPSHOT] deferred restore play failed: idx=%d", track_idx);
        return PLAYER_SNAPSHOT_RESTORE_FAILED;
    }

    // 启动恢复时默认停在暂停态，避免上电立即出声。
    player_control_mark_user_paused();
    audio_service_pause();

    LOGI("[SNAPSHOT] deferred restore done: mode=%d group=%d track=%d path=%s vol=%u view=%u",
         (int)g_play_mode,
         player_playlist_get_current_group_idx(),
         track_idx,
         snap.track_path.c_str(),
         (unsigned)snap.volume,
         (unsigned)snap.ui_view);
    return PLAYER_SNAPSHOT_RESTORE_DONE;
}
