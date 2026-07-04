#include "player_binding.h"

#include "app_flags.h"
#include "nfc/nfc_binding.h"
#include "player_playlist.h"
#include "player_recover.h"
#include "player_state.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_groups_v3.h"
#include "utils/log.h"

// 只给本文件用的内部变量和内部函数
namespace {

PlayerBindingHooks s_hooks{};

static int s_nfc_last_track_idx = -1;
static uint32_t s_nfc_last_track_ms = 0;

// NFC 防重入 helper 函数
static bool nfc_binding_should_suppress_duplicate(int track_idx)
{
    if (track_idx < 0) return false;
    
    uint32_t now = millis();
    if (track_idx == s_nfc_last_track_idx && (uint32_t)(now - s_nfc_last_track_ms) < 1000) {
        LOGW("[NFC] 已抑制重复歌曲触发：索引=%d 间隔=%ums", 
             track_idx, (unsigned)(now - s_nfc_last_track_ms));
        return true;
    }
    
    s_nfc_last_track_idx = track_idx;
    s_nfc_last_track_ms = now;
    return false;
}
// 触发播放轨道回调
bool binding_play_track_dispatch(int idx, bool verbose, bool force_cover)
{
    if (idx < 0) return false;
    if (s_hooks.play_track_dispatch) {
        return s_hooks.play_track_dispatch(idx, verbose, force_cover);
    }
    return false;
}
// 获取艺术家组
const std::vector<PlaylistGroup>& binding_artist_groups()
{
    return storage_catalog_v3_artist_groups();
}
// 获取专辑组
const std::vector<PlaylistGroup>& binding_album_groups()
{
    return storage_catalog_v3_album_groups();
}
// 是否保持随机播放标志
static bool nfc_binding_keep_random_flag()
{
    return g_play_mode == PLAY_MODE_ALL_RND ||
           g_play_mode == PLAY_MODE_ARTIST_RND ||
           g_play_mode == PLAY_MODE_ALBUM_RND;
}
// 获取NFC绑定类型的播放模式
static play_mode_t nfc_binding_mode_for_type(NfcBindType type)
{
    const bool is_random = nfc_binding_keep_random_flag();

    switch (type) {
        case NFC_BIND_TRACK:
            return is_random ? PLAY_MODE_ALL_RND : PLAY_MODE_ALL_SEQ;
        case NFC_BIND_ARTIST:
            return is_random ? PLAY_MODE_ARTIST_RND : PLAY_MODE_ARTIST_SEQ;
        case NFC_BIND_ALBUM:
            return is_random ? PLAY_MODE_ALBUM_RND : PLAY_MODE_ALBUM_SEQ;
        default:
            return is_random ? PLAY_MODE_ALL_RND : PLAY_MODE_ALL_SEQ;
    }
}
// 应用NFC绑定类型的播放模式
static void nfc_binding_apply_mode(NfcBindType type)
{
    g_play_mode = nfc_binding_mode_for_type(type);
    const bool is_random = nfc_binding_keep_random_flag();

    LOGD("[NFC] 应用播放模式：类型=%d -> 模式=%d 随机=%d",
         (int)type,
         (int)g_play_mode,
         is_random ? 1 : 0);
}

} //
// 设置NFC绑定回调
void player_binding_setup_hooks(const PlayerBindingHooks& hooks)
{
    s_hooks = hooks;
}
// 尝试处理NFC UID
bool player_binding_try_handle_nfc_uid(const String& uid)
{
    NfcBindingEntry entry;
    if (!nfc_binding_find(uid, entry)) {
        return false;
    }

    switch (entry.type) {
        case NFC_BIND_TRACK: {
            int idx = player_recover_find_track_idx_by_path(entry.key);
            if (idx >= 0) {
                LOGI("[NFC] UID 已匹配，播放歌曲 索引=%d 路径=%s", idx, entry.key.c_str());

                if (nfc_binding_should_suppress_duplicate(idx)) {
                    return true;
                }

                nfc_binding_apply_mode(NFC_BIND_TRACK);
                player_playlist_set_current_group_idx(-1);
                player_playlist_force_rebuild();
                player_state_mark_next_play_from_nfc();
                (void)binding_play_track_dispatch(idx, true, true);
            } else {
                LOGD("[NFC] 未找到单曲绑定：%s", entry.key.c_str());
            }
            break;
        }

        case NFC_BIND_ARTIST:
            (void)player_play_artist_binding(entry.key);
            break;

        case NFC_BIND_ALBUM:
            (void)player_play_album_binding(entry.key);
            break;

        default:
            LOGW("[NFC] 未知绑定类型");
            break;
    }

    return true;
}
// 尝试播放艺术家
bool player_play_artist_binding(const String& artist)
{
    String key = artist;
    key.trim();
    if (key.isEmpty()) {
        LOGW("[播放器] 歌手 binding 失败: 为空 歌手");
        return false;
    }

    LOGD("[播放器] 歌手 binding request: %s", key.c_str());

    const MusicCatalogV3& cat = storage_catalog_v3();
    const auto& groups = binding_artist_groups();
    for (int i = 0; i < (int)groups.size(); i++) {
        if (playlist_group_name_string(cat, groups[i]) == key) {
            LOGD("[播放器] 歌手 binding matched: 分组=%d name=%s first_索引=%d",
                 i, playlist_group_name_cstr(cat, groups[i]),
                 groups[i].track_indices.empty() ? -1 : (int)groups[i].track_indices[0]);
            nfc_binding_apply_mode(NFC_BIND_ARTIST);
            player_playlist_set_current_group_idx(i);
            player_playlist_force_rebuild();

            player_playlist_ensure_current();
            const int first_track = player_playlist_current_track_at(0);
            if (first_track >= 0) {
                if (nfc_binding_should_suppress_duplicate(first_track)) {
                    return true;
                }
                
                player_state_mark_next_play_from_nfc();
                (void)binding_play_track_dispatch(first_track, true, true);
                LOGI("[播放器] 歌手 binding success: %s, 分组=%d, 歌曲s=%d",
                     key.c_str(), i, (int)player_playlist_current_size());
                return true;
            }
        }
    }

    LOGD("[播放器] 歌手 binding 未找到: %s", key.c_str());
    return false;
}
// 尝试播放专辑
bool player_play_album_binding(const String& album)
{
    String key = album;
    key.trim();
    if (key.isEmpty()) {
        LOGW("[播放器] 专辑 binding 失败: 为空 专辑");
        return false;
    }

    LOGD("[播放器] 专辑 binding request: %s", key.c_str());

    const MusicCatalogV3& cat = storage_catalog_v3();
    const auto& groups = binding_album_groups();
    for (int i = 0; i < (int)groups.size(); i++) {
        String group_key = playlist_group_display_string(cat, groups[i]);
        if (group_key == key) {
            LOGD("[播放器] 专辑 binding matched: 分组=%d name=%s first_索引=%d",
                 i, group_key.c_str(),
                 groups[i].track_indices.empty() ? -1 : (int)groups[i].track_indices[0]);
            nfc_binding_apply_mode(NFC_BIND_ALBUM);
            player_playlist_set_current_group_idx(i);
            player_playlist_force_rebuild();

            player_playlist_ensure_current();
            const int first_track = player_playlist_current_track_at(0);
            if (first_track >= 0) {
                if (nfc_binding_should_suppress_duplicate(first_track)) {
                    return true;
                }
                
                player_state_mark_next_play_from_nfc();
                (void)binding_play_track_dispatch(first_track, true, true);
                LOGI("[播放器] 专辑 binding success: %s, 分组=%d, 歌曲s=%d",
                     key.c_str(), i, (int)player_playlist_current_size());
                return true;
            }
        }
    }

    LOGD("[播放器] 专辑 binding 未找到: %s", key.c_str());
    return false;
}
