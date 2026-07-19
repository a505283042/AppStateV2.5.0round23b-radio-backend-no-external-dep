#include "player_snapshot.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

#include "app_flags.h"
#include "audio/audio.h"
#include "audio/audio_output_route.h"
#include "audio/audio_service.h"
#include "lyrics/lyrics.h"
#include "net_music/net_music_catalog.h"
#include "player_assets.h"
#include "player_control.h"
#include "player_playlist.h"
#include "player_recover.h"
#include "player_source.h"
#include "player_state.h"
#include "storage/storage.h"
#include "storage/storage_catalog_v3.h"
#include "storage/system_paths.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

static const char* kPrefsNs = "playerst";
static const char* kMetaKey = "meta_v1";
static const uint8_t kLocalSnapshotVersion = 2;
static const uint8_t kNetSnapshotVersion = 1;
static const uint8_t kMetaVersion = 1;
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

struct PlayerNetPersistSnapshotBlob {
    uint8_t version;
    uint8_t play_mode;
    uint8_t user_paused;
    uint8_t reserved0;
    int32_t track_idx;
    uint32_t total_count;
    uint32_t duration_ms;
    char encoded_path[320];
    char url[384];
    char title[128];
    char artist[96];
    char album[96];
    char format[16];
};

struct PlayerSnapshotMetaBlob {
    uint8_t version;
    uint8_t last_source;
    uint8_t normal_volume;
    uint8_t ui_view;
};

bool s_local_valid = false;
bool s_net_valid = false;
bool s_meta_valid = false;
bool s_restore_armed = false;
bool s_global_state_applied = false;
PlayerPersistSnapshot s_local{};
PlayerNetPersistSnapshot s_net{};
PlayerSnapshotSource s_last_source = PlayerSnapshotSource::None;
uint8_t s_normal_volume = 80;
uint8_t s_ui_view = (uint8_t)UI_VIEW_INFO;
uint32_t s_restore_not_before_ms = 0;

static play_mode_t snapshot_sanitize_local_mode(int raw)
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

static play_mode_t snapshot_sanitize_net_mode(int raw)
{
    switch (raw) {
        case PLAY_MODE_ALL_RND:
        case PLAY_MODE_ARTIST_RND:
        case PLAY_MODE_ALBUM_RND:
            return PLAY_MODE_ALL_RND;
        case PLAY_MODE_ALL_SEQ:
        case PLAY_MODE_ARTIST_SEQ:
        case PLAY_MODE_ALBUM_SEQ:
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

static void snapshot_sanitize_ui_view(uint8_t& view)
{
    if (view != (uint8_t)UI_VIEW_ROTATE &&
        view != (uint8_t)UI_VIEW_INFO &&
        view != (uint8_t)UI_VIEW_COVER_PANEL) {
        view = (uint8_t)UI_VIEW_INFO;
    }
}

static int snapshot_resolve_local_track_idx(const PlayerPersistSnapshot& snap)
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

static uint32_t snapshot_fnv1a(const String& text)
{
    uint32_t hash = 2166136261u;
    const size_t len = text.length();
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint8_t)text[i];
        hash *= 16777619u;
    }
    return hash;
}

static void snapshot_net_key(char* out, size_t out_size)
{
    if (!out || out_size == 0) return;
    const String base = net_music_catalog_base_url();
    const uint32_t hash = snapshot_fnv1a(base.length() ? base : String("default"));
    snprintf(out, out_size, "nas_%08lX", (unsigned long)hash);
}

static void snapshot_log_local(const char* prefix, const PlayerPersistSnapshot& snap)
{
    LOGD("[快照] %s 本地：模式=%u 分组=%d 歌曲=%d 路径=%s 暂停=%d",
         prefix,
         (unsigned)snap.play_mode,
         snap.current_group_idx,
         snap.track_idx,
         snap.track_path.c_str(),
         (int)snap.user_paused);
}

static void snapshot_log_net(const char* prefix, const PlayerNetPersistSnapshot& snap)
{
    LOGD("[快照] %s NAS：模式=%u 歌曲=%d/%lu 标题=%s 路径=%s 暂停=%d",
         prefix,
         (unsigned)snap.play_mode,
         snap.track_idx,
         (unsigned long)snap.total_count,
         snap.title.c_str(),
         snap.encoded_path.c_str(),
         (int)snap.user_paused);
}

static bool snapshot_read_local_blob(Preferences& pref, PlayerPersistSnapshot& out)
{
    char key[16] = {0};
    (void)storage_copy_card_snapshot_key(key, sizeof(key));
    if (!pref.isKey(key)) return false;

    const size_t len = pref.getBytesLength(key);
    if (len != sizeof(PlayerPersistSnapshotBlob)) return false;

    PlayerPersistSnapshotBlob blob{};
    if (pref.getBytes(key, &blob, sizeof(blob)) != sizeof(blob)) {
        LOGW("[快照] 本地 blob 读取大小不匹配");
        return false;
    }
    if (blob.version != kLocalSnapshotVersion && blob.version != 1) {
        LOGW("[快照] 本地 blob 版本不支持：%u", (unsigned)blob.version);
        return false;
    }

    out.version = blob.version;
    out.volume = blob.volume > 100 ? 100 : blob.volume;
    out.play_mode = blob.play_mode;
    out.current_group_idx = (int)blob.current_group_idx;
    out.track_idx = (int)blob.track_idx;
    out.ui_view = blob.ui_view;
    out.user_paused = blob.user_paused != 0;
    out.track_path = String(blob.track_path);
    snapshot_sanitize_ui_view(out.ui_view);
    return true;
}

static bool snapshot_read_net_blob(Preferences& pref, PlayerNetPersistSnapshot& out)
{
    char key[16] = {0};
    snapshot_net_key(key, sizeof(key));
    if (!pref.isKey(key)) return false;
    if (pref.getBytesLength(key) != sizeof(PlayerNetPersistSnapshotBlob)) return false;

    PlayerNetPersistSnapshotBlob blob{};
    if (pref.getBytes(key, &blob, sizeof(blob)) != sizeof(blob)) {
        LOGW("[快照] NAS blob 读取大小不匹配：key=%s", key);
        return false;
    }
    if (blob.version != kNetSnapshotVersion) {
        LOGW("[快照] NAS blob 版本不支持：%u", (unsigned)blob.version);
        return false;
    }

    out.version = blob.version;
    out.play_mode = blob.play_mode;
    out.track_idx = (int)blob.track_idx;
    out.total_count = blob.total_count;
    out.duration_ms = blob.duration_ms;
    out.user_paused = blob.user_paused != 0;
    out.encoded_path = String(blob.encoded_path);
    out.url = String(blob.url);
    out.title = String(blob.title);
    out.artist = String(blob.artist);
    out.album = String(blob.album);
    out.format = String(blob.format);
    return out.track_idx >= 0;
}

static bool snapshot_read_meta_blob(Preferences& pref)
{
    if (!pref.isKey(kMetaKey) ||
        pref.getBytesLength(kMetaKey) != sizeof(PlayerSnapshotMetaBlob)) {
        return false;
    }

    PlayerSnapshotMetaBlob blob{};
    if (pref.getBytes(kMetaKey, &blob, sizeof(blob)) != sizeof(blob) ||
        blob.version != kMetaVersion) {
        return false;
    }

    s_last_source = (blob.last_source == (uint8_t)PlayerSnapshotSource::NetTrack)
        ? PlayerSnapshotSource::NetTrack
        : (blob.last_source == (uint8_t)PlayerSnapshotSource::LocalTrack
            ? PlayerSnapshotSource::LocalTrack
            : PlayerSnapshotSource::None);
    s_normal_volume = blob.normal_volume > 100 ? 100 : blob.normal_volume;
    s_ui_view = blob.ui_view;
    snapshot_sanitize_ui_view(s_ui_view);
    return true;
}

static bool snapshot_write_local_blob(Preferences& pref)
{
    if (!s_local_valid) return true;

    PlayerPersistSnapshotBlob blob{};
    blob.version = kLocalSnapshotVersion;
    blob.volume = s_normal_volume;
    blob.play_mode = s_local.play_mode;
    blob.ui_view = s_ui_view;
    blob.current_group_idx = (int16_t)s_local.current_group_idx;
    blob.track_idx = (int32_t)s_local.track_idx;
    blob.user_paused = s_local.user_paused ? 1 : 0;
    s_local.track_path.toCharArray(blob.track_path, sizeof(blob.track_path));

    char key[16] = {0};
    (void)storage_copy_card_snapshot_key(key, sizeof(key));
    const size_t written = pref.putBytes(key, &blob, sizeof(blob));
    if (written != sizeof(blob)) {
        LOGE("[快照] 本地保存失败：key=%s 写入=%u 期望=%u",
             key, (unsigned)written, (unsigned)sizeof(blob));
        return false;
    }
    return true;
}

static bool snapshot_write_net_blob(Preferences& pref)
{
    if (!s_net_valid) return true;

    PlayerNetPersistSnapshotBlob blob{};
    blob.version = kNetSnapshotVersion;
    blob.play_mode = s_net.play_mode;
    blob.user_paused = s_net.user_paused ? 1 : 0;
    blob.track_idx = (int32_t)s_net.track_idx;
    blob.total_count = s_net.total_count;
    blob.duration_ms = s_net.duration_ms;
    s_net.encoded_path.toCharArray(blob.encoded_path, sizeof(blob.encoded_path));
    s_net.url.toCharArray(blob.url, sizeof(blob.url));
    s_net.title.toCharArray(blob.title, sizeof(blob.title));
    s_net.artist.toCharArray(blob.artist, sizeof(blob.artist));
    s_net.album.toCharArray(blob.album, sizeof(blob.album));
    s_net.format.toCharArray(blob.format, sizeof(blob.format));

    char key[16] = {0};
    snapshot_net_key(key, sizeof(key));
    const size_t written = pref.putBytes(key, &blob, sizeof(blob));
    if (written != sizeof(blob)) {
        LOGE("[快照] NAS 保存失败：key=%s 写入=%u 期望=%u",
             key, (unsigned)written, (unsigned)sizeof(blob));
        return false;
    }
    return true;
}

static bool snapshot_write_meta_blob(Preferences& pref)
{
    PlayerSnapshotMetaBlob blob{};
    blob.version = kMetaVersion;
    blob.last_source = (uint8_t)s_last_source;
    blob.normal_volume = s_normal_volume;
    blob.ui_view = s_ui_view;

    const size_t written = pref.putBytes(kMetaKey, &blob, sizeof(blob));
    if (written != sizeof(blob)) {
        LOGE("[快照] 元数据保存失败：写入=%u 期望=%u",
             (unsigned)written, (unsigned)sizeof(blob));
        return false;
    }
    return true;
}

static void snapshot_capture_global_state()
{
    // 普通输出音量与蓝牙发射音量分开保存。
    // 蓝牙模式下这里读取进入蓝牙前的普通播放器音量，不会把 BT62SP 音量写进播放器快照。
    s_normal_volume = audio_output_route_get_normal_volume();
    if (s_normal_volume > 100) s_normal_volume = 100;
    s_ui_view = (uint8_t)ui_get_view();
    snapshot_sanitize_ui_view(s_ui_view);
    s_meta_valid = true;
}

static bool snapshot_capture_local_state()
{
    const int idx = player_state_current_index();
    if (idx < 0 || idx >= (int)storage_catalog_v3_track_count()) {
        return false;
    }

    PlayerPersistSnapshot snap{};
    snap.version = kLocalSnapshotVersion;
    snap.volume = s_normal_volume;
    snap.play_mode = (uint8_t)app_play_mode_get();
    snap.current_group_idx = player_playlist_get_current_group_idx();
    snap.track_idx = idx;
    snap.ui_view = s_ui_view;
    snap.user_paused = player_control_is_user_paused() || audio_service_is_paused();

    String path;
    if (player_recover_get_current_track_path(path)) {
        snap.track_path = path;
    } else if (s_local_valid && s_local.track_idx == idx) {
        snap.track_path = s_local.track_path;
    }

    s_local = snap;
    s_local_valid = true;
    s_last_source = PlayerSnapshotSource::LocalTrack;
    snapshot_log_local("捕获", s_local);
    return true;
}

static bool snapshot_capture_net_state(const PlayerSourceState& source)
{
    if (source.net_track_idx < 0) return false;

    PlayerNetPersistSnapshot snap{};
    snap.version = kNetSnapshotVersion;
    snap.play_mode = (uint8_t)snapshot_sanitize_net_mode((int)app_play_mode_get());
    snap.track_idx = source.net_track_idx;
    snap.total_count = net_music_catalog_is_loaded() ? net_music_catalog_count() : 0;
    snap.duration_ms = source.net_track_duration_ms;
    snap.url = source.net_track_url;
    snap.title = source.net_track_title;
    snap.artist = source.net_track_artist;
    snap.album = source.net_track_album;
    snap.format = source.net_track_format;
    snap.user_paused = player_control_is_user_paused() || audio_service_is_paused();

    NetMusicItem item{};
    if (net_music_catalog_is_loaded() &&
        net_music_catalog_get((uint32_t)source.net_track_idx, &item) &&
        item.valid) {
        snap.encoded_path = item.encoded_path;
        if (snap.title.isEmpty()) snap.title = item.title;
        if (snap.artist.isEmpty()) snap.artist = item.artist;
        if (snap.album.isEmpty()) snap.album = item.album;
        if (snap.format.isEmpty()) snap.format = item.format;
        if (snap.duration_ms == 0) snap.duration_ms = item.duration_ms;
    } else if (s_net_valid && s_net.track_idx == source.net_track_idx) {
        snap.encoded_path = s_net.encoded_path;
    }

    s_net = snap;
    s_net_valid = true;
    s_last_source = PlayerSnapshotSource::NetTrack;
    snapshot_log_net("捕获", s_net);
    return true;
}

static void snapshot_apply_global_state()
{
    if (s_global_state_applied) return;
    s_global_state_applied = true;

    if (s_meta_valid) {
        (void)audio_output_route_set_user_volume(s_normal_volume);
        if ((uint8_t)ui_get_view() != s_ui_view) {
            ui_set_view((ui_player_view_t)s_ui_view);
        }
        LOGD("[快照] 已恢复全局状态：普通音量=%u 视图=%u 最后音源=%u",
             (unsigned)s_normal_volume,
             (unsigned)s_ui_view,
             (unsigned)s_last_source);
    }
}

static bool snapshot_apply_default_cover()
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
        if (buf) free(buf);
        return false;
    }
    const bool scaled = ui_cover_scale_from_buffer(buf, len, is_png);
    free(buf);
    return scaled;
}

} // namespace

bool player_snapshot_load_pending_from_nvs()
{
    s_local_valid = false;
    s_net_valid = false;
    s_meta_valid = false;
    s_restore_armed = false;
    s_global_state_applied = false;
    s_restore_not_before_ms = 0;
    s_local = PlayerPersistSnapshot{};
    s_net = PlayerNetPersistSnapshot{};
    s_last_source = PlayerSnapshotSource::None;
    s_normal_volume = 80;
    s_ui_view = (uint8_t)UI_VIEW_INFO;

    Preferences pref;
    if (!pref.begin(kPrefsNs, true)) {
        LOGW("[快照] 加载已跳过：打开 NVS namespace 失败");
        return false;
    }

    s_local_valid = snapshot_read_local_blob(pref, s_local);
    s_net_valid = snapshot_read_net_blob(pref, s_net);
    s_meta_valid = snapshot_read_meta_blob(pref);
    pref.end();

    // 兼容旧版：只有本地 blob、没有 meta 时，从旧 blob 迁移全局音量和视图。
    if (!s_meta_valid && s_local_valid) {
        s_normal_volume = s_local.volume > 100 ? 100 : s_local.volume;
        s_ui_view = s_local.ui_view;
        snapshot_sanitize_ui_view(s_ui_view);
        s_last_source = PlayerSnapshotSource::LocalTrack;
        s_meta_valid = true;
    }

    if (s_local_valid) snapshot_log_local("已加载", s_local);
    if (s_net_valid) snapshot_log_net("已加载", s_net);
    LOGD("[快照] 加载结果：本地=%d NAS=%d meta=%d 最后音源=%u 普通音量=%u",
         s_local_valid ? 1 : 0,
         s_net_valid ? 1 : 0,
         s_meta_valid ? 1 : 0,
         (unsigned)s_last_source,
         (unsigned)s_normal_volume);

    // 开机阶段仍只自动恢复本地 UI，不主动加载 NAS 列表或连接网络。
    return s_local_valid || s_net_valid || s_meta_valid;
}

bool player_snapshot_capture_current_source()
{
    snapshot_capture_global_state();
    const PlayerSourceState source = player_source_get();

    if (source.type == PlayerSourceType::LOCAL_TRACK) {
        return snapshot_capture_local_state();
    }
    if (source.type == PlayerSourceType::NET_TRACK) {
        return snapshot_capture_net_state(source);
    }

    // 网络电台不覆盖本地/NAS 两套音乐快照，也不改变最后音乐音源。
    return false;
}

bool player_snapshot_save_to_nvs()
{
    (void)player_snapshot_capture_current_source();
    snapshot_capture_global_state();

    Preferences pref;
    if (!pref.begin(kPrefsNs, false)) {
        LOGE("[快照] 保存失败：打开 NVS namespace");
        return false;
    }

    const bool local_ok = snapshot_write_local_blob(pref);
    const bool net_ok = snapshot_write_net_blob(pref);
    const bool meta_ok = snapshot_write_meta_blob(pref);
    pref.end();

    LOGI("[快照] 双快照保存：本地=%d/%d NAS=%d/%d meta=%d 最后音源=%u 普通音量=%u",
         s_local_valid ? 1 : 0,
         local_ok ? 1 : 0,
         s_net_valid ? 1 : 0,
         net_ok ? 1 : 0,
         meta_ok ? 1 : 0,
         (unsigned)s_last_source,
         (unsigned)s_normal_volume);
    return local_ok && net_ok && meta_ok;
}

bool player_snapshot_reload_net_context_for_active_source()
{
    s_net_valid = false;
    s_net = PlayerNetPersistSnapshot{};

    Preferences pref;
    if (!pref.begin(kPrefsNs, true)) {
        LOGW("[快照] NAS 曲库切换后加载失败：打开 NVS namespace");
        return false;
    }

    s_net_valid = snapshot_read_net_blob(pref, s_net);
    pref.end();

    if (s_net_valid) {
        snapshot_log_net("曲库切换加载", s_net);
    } else {
        LOGD("[快照] 当前 NAS 曲库没有历史播放快照");
    }
    return s_net_valid;
}

bool player_snapshot_apply_local_context()
{
    if (!s_local_valid) return false;

    const play_mode_t mode = snapshot_sanitize_local_mode((int)s_local.play_mode);
    (void)app_play_mode_set(mode, AppPlayModeChangeReason::SnapshotRestore);
    player_playlist_set_current_group_idx(
        snapshot_sanitize_group_idx(mode, s_local.current_group_idx));
    player_playlist_force_rebuild();
    player_playlist_ensure_current();

    LOGD("[快照] 已恢复本地上下文：模式=%d 分组=%d 歌曲=%d",
         (int)mode,
         player_playlist_get_current_group_idx(),
         player_snapshot_local_track_index());
    return true;
}

bool player_snapshot_apply_net_context()
{
    // NAS 只支持顺序/随机两种模式，不能继承本地的歌手/专辑模式。
    const play_mode_t mode = s_net_valid
        ? snapshot_sanitize_net_mode((int)s_net.play_mode)
        : PLAY_MODE_ALL_SEQ;
    (void)app_play_mode_set(mode, AppPlayModeChangeReason::SnapshotRestore);

    if (s_net_valid) {
        LOGD("[快照] 已恢复 NAS 上下文：模式=%d 歌曲=%d",
             (int)mode,
             s_net.track_idx);
    } else {
        LOGD("[快照] NAS 尚无历史状态，使用默认顺序模式");
    }
    return s_net_valid;
}

int player_snapshot_local_track_index()
{
    return s_local_valid ? snapshot_resolve_local_track_idx(s_local) : -1;
}

int player_snapshot_net_track_index()
{
    return s_net_valid ? s_net.track_idx : -1;
}

int player_snapshot_resolve_net_track_index(int requested_idx)
{
    if (!s_net_valid ||
        requested_idx < 0 ||
        requested_idx != s_net.track_idx ||
        s_net.encoded_path.isEmpty() ||
        !net_music_catalog_is_loaded()) {
        return requested_idx;
    }

    NetMusicItem item{};
    if (net_music_catalog_get((uint32_t)requested_idx, &item) &&
        item.valid &&
        item.encoded_path == s_net.encoded_path) {
        return requested_idx;
    }

    const uint32_t count = net_music_catalog_count();
    for (uint32_t i = 0; i < count; ++i) {
        NetMusicItem candidate{};
        if (net_music_catalog_get(i, &candidate) &&
            candidate.valid &&
            candidate.encoded_path == s_net.encoded_path) {
            LOGI("[快照] NAS 索引已按路径校正：%d -> %lu",
                 requested_idx,
                 (unsigned long)i);
            s_net.track_idx = (int)i;
            s_net.total_count = count;
            return (int)i;
        }
    }

    LOGW("[快照] NAS 保存路径已不存在，继续使用原索引=%d 路径=%s",
         requested_idx,
         s_net.encoded_path.c_str());
    return requested_idx;
}

PlayerSnapshotSource player_snapshot_last_source()
{
    return s_last_source;
}

bool player_snapshot_begin_restore_on_player_enter()
{
    snapshot_apply_global_state();

    if (!s_local_valid) return false;
    if (!storage_catalog_v3_ready() || storage_catalog_v3_track_count() == 0) {
        LOGW("[快照] 本地恢复已跳过：目录未就绪");
        return false;
    }

    (void)player_snapshot_apply_local_context();
    s_restore_armed = true;
    s_restore_not_before_ms = millis() + kDeferredRestoreDelayMs;
    LOGD("[快照] 已应用本地轻量恢复，%u ms 后恢复当前歌曲",
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
        LOGW("[快照] 延迟恢复失败：目录未就绪");
        return PLAYER_SNAPSHOT_RESTORE_FAILED;
    }

    const PlayerPersistSnapshot snap = s_local;
    const int track_idx = snapshot_resolve_local_track_idx(snap);
    if (track_idx < 0) {
        LOGW("[快照] 延迟恢复已跳过：保存的歌曲不存在，路径=%s 索引=%d",
             snap.track_path.c_str(), snap.track_idx);
        return PLAYER_SNAPSHOT_RESTORE_FAILED;
    }

    TrackInfo t;
    if (!storage_catalog_v3_get_trackinfo((uint32_t)track_idx, t, "/Music")) {
        LOGE("[快照] 延迟 UI-only 恢复展开歌曲信息失败：索引=%d", track_idx);
        return PLAYER_SNAPSHOT_RESTORE_FAILED;
    }

    // 开机恢复只恢复 UI，不启动音频。
    player_control_mark_user_paused();
    player_state_set_current_index(track_idx);
    player_source_set_local_track(track_idx);

    (void)player_playlist_align_group_context_for_track(track_idx, true);
    player_playlist_update_for_current_track(track_idx, true);

    ui_set_now_playing(t.title.c_str(), t.artist.c_str());
    ui_set_album(t.album);
    ui_set_play_mode(app_play_mode_get());
    audio_output_route_sync_ui_volume();

    {
        const int total = (int)storage_catalog_v3_track_count();
        int display_pos = track_idx;
        int display_total = total;

        const play_mode_t mode = app_play_mode_get();
        if (mode == PLAY_MODE_ARTIST_SEQ || mode == PLAY_MODE_ARTIST_RND ||
            mode == PLAY_MODE_ALBUM_SEQ || mode == PLAY_MODE_ALBUM_RND) {
            const PlayerPlaylistDisplayInfo display =
                player_playlist_get_display_info(track_idx, total);
            display_total = display.display_total;
            display_pos = display.display_pos;
        }

        ui_set_track_pos(display_pos, display_total);
    }

    g_lyricsDisplay.clear();

    const bool cover_cache_hit = ui_cover_apply_cached(track_idx);
    if (cover_cache_hit) {
        ui_request_refresh_now();
    } else {
        (void)snapshot_apply_default_cover();
    }

    PlayerDeferredAssetJob asset_job{};
    const bool need_decode_cover = !cover_cache_hit;

    const bool has_deferred_assets = player_assets_prepare_deferred_request(
        t,
        track_idx,
        false,
        t.lrc_path.length() > 0,
        need_decode_cover,
        asset_job);

    const bool allow_boot_next_prefetch = storage_catalog_v3_track_count() > 1;

    asset_job.need_total = false;
    asset_job.need_lyrics = (t.lrc_path.length() > 0);
    asset_job.need_cover = need_decode_cover;
    asset_job.suppress_next_prefetch = !allow_boot_next_prefetch;

    if (has_deferred_assets) {
        player_assets_schedule(asset_job);
    } else if (allow_boot_next_prefetch) {
        player_assets_reset_job(asset_job);
        asset_job.track_idx = track_idx;
        asset_job.need_total = false;
        asset_job.need_lyrics = false;
        asset_job.need_cover = false;
        asset_job.suppress_next_prefetch = false;
        player_assets_schedule(asset_job);
    }

    ui_request_refresh_now();

    LOGD("[快照] 延迟本地 UI 恢复完成：模式=%d 分组=%d 歌曲=%d 路径=%s 普通音量=%u 视图=%u",
         (int)app_play_mode_get(),
         player_playlist_get_current_group_idx(),
         track_idx,
         snap.track_path.c_str(),
         (unsigned)s_normal_volume,
         (unsigned)s_ui_view);

    return PLAYER_SNAPSHOT_RESTORE_DONE;
}
