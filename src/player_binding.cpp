#include "player_binding.h"

#include "app_flags.h"
#include "nfc/nfc_binding.h"
#include "player_playlist.h"
#include "player_recover.h"
#include "player_state.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_groups_v3.h"
#include "utils/log.h"

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
        LOGW("[NFC] duplicate track trigger suppressed idx=%d dt=%ums", 
             track_idx, (unsigned)(now - s_nfc_last_track_ms));
        return true;
    }
    
    s_nfc_last_track_idx = track_idx;
    s_nfc_last_track_ms = now;
    return false;
}

bool binding_play_track_dispatch(int idx, bool verbose, bool force_cover)
{
    if (idx < 0) return false;
    if (s_hooks.play_track_dispatch) {
        return s_hooks.play_track_dispatch(idx, verbose, force_cover);
    }
    return false;
}

const std::vector<PlaylistGroup>& binding_artist_groups()
{
    return storage_catalog_v3_artist_groups();
}

const std::vector<PlaylistGroup>& binding_album_groups()
{
    return storage_catalog_v3_album_groups();
}

} // namespace

void player_binding_setup_hooks(const PlayerBindingHooks& hooks)
{
    s_hooks = hooks;
}

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
                LOGI("[NFC] uid matched, play track idx=%d path=%s", idx, entry.key.c_str());

                if (nfc_binding_should_suppress_duplicate(idx)) {
                    return true;
                }

                g_play_mode = PLAY_MODE_ALL_SEQ;
                player_playlist_set_current_group_idx(-1);
                player_playlist_force_rebuild();
                player_state_mark_next_play_from_nfc();
                (void)binding_play_track_dispatch(idx, true, true);
            } else {
                LOGI("[NFC] track binding not found: %s", entry.key.c_str());
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
            LOGI("[NFC] unknown binding type");
            break;
    }

    return true;
}

bool player_play_artist_binding(const String& artist)
{
    String key = artist;
    key.trim();
    if (key.isEmpty()) {
        LOGI("[PLAYER] artist binding failed: empty artist");
        return false;
    }

    LOGI("[PLAYER] artist binding request: %s", key.c_str());

    const MusicCatalogV3& cat = storage_catalog_v3();
    const auto& groups = binding_artist_groups();
    for (int i = 0; i < (int)groups.size(); i++) {
        if (playlist_group_name_string(cat, groups[i]) == key) {
            LOGI("[PLAYER] artist binding matched: group=%d name=%s first_idx=%d",
                 i, playlist_group_name_cstr(cat, groups[i]),
                 groups[i].track_indices.empty() ? -1 : (int)groups[i].track_indices[0]);
            g_play_mode = PLAY_MODE_ARTIST_SEQ;
            player_playlist_set_current_group_idx(i);
            player_playlist_force_rebuild();

            const auto& playlist = player_playlist_get_current();
            if (!playlist.empty()) {
                if (nfc_binding_should_suppress_duplicate(playlist[0])) {
                    return true;
                }
                
                player_state_mark_next_play_from_nfc();
                (void)binding_play_track_dispatch(playlist[0], true, true);
                LOGI("[PLAYER] artist binding success: %s, group=%d, tracks=%d",
                     key.c_str(), i, (int)playlist.size());
                return true;
            }
        }
    }

    LOGI("[PLAYER] artist binding not found: %s", key.c_str());
    return false;
}

bool player_play_album_binding(const String& album)
{
    String key = album;
    key.trim();
    if (key.isEmpty()) {
        LOGI("[PLAYER] album binding failed: empty album");
        return false;
    }

    LOGI("[PLAYER] album binding request: %s", key.c_str());

    const MusicCatalogV3& cat = storage_catalog_v3();
    const auto& groups = binding_album_groups();
    for (int i = 0; i < (int)groups.size(); i++) {
        String group_key = playlist_group_display_string(cat, groups[i]);
        if (group_key == key) {
            LOGI("[PLAYER] album binding matched: group=%d name=%s first_idx=%d",
                 i, group_key.c_str(),
                 groups[i].track_indices.empty() ? -1 : (int)groups[i].track_indices[0]);
            g_play_mode = PLAY_MODE_ALBUM_SEQ;
            player_playlist_set_current_group_idx(i);
            player_playlist_force_rebuild();

            const auto& playlist = player_playlist_get_current();
            if (!playlist.empty()) {
                if (nfc_binding_should_suppress_duplicate(playlist[0])) {
                    return true;
                }
                
                player_state_mark_next_play_from_nfc();
                (void)binding_play_track_dispatch(playlist[0], true, true);
                LOGI("[PLAYER] album binding success: %s, group=%d, tracks=%d",
                     key.c_str(), i, (int)playlist.size());
                return true;
            }
        }
    }

    LOGI("[PLAYER] album binding not found: %s", key.c_str());
    return false;
}
