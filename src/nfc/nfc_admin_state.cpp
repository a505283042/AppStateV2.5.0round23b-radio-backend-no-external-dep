#include "nfc/nfc_admin_state.h"

#include "app_state.h"
#include "nfc/nfc.h"
#include "nfc/nfc_binding.h"
#include "player_control.h"
#include "player_playlist.h"
#include "player_state.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_groups_v3.h"
#include "audio/audio_service.h"
#include "ui/ui.h"
#include "utils/log.h"

// 外部声明
extern volatile play_mode_t g_play_mode;


namespace {
enum NfcAdminStep {
    ADMIN_IDLE = 0,
    ADMIN_WAIT_CARD,
    ADMIN_CONFIRM_BIND,
    ADMIN_SAVING,
    ADMIN_WAIT_REMOVE,
    ADMIN_DONE,
    ADMIN_ERROR,
};

struct NfcAdminCtx {
    NfcAdminStep step = ADMIN_IDLE;
    NfcAdminTarget target;
    String pending_uid;
    bool save_ok = false;
    uint32_t step_ms = 0;
    uint32_t enter_ms = 0;
};

static NfcAdminCtx s_admin;
static uint32_t s_remove_miss_ms = 0;
static bool s_resume_play_on_exit = false;

// 切换 step 并更新时间戳
static void admin_set_step(NfcAdminStep step)
{
    s_admin.step = step;
    s_admin.step_ms = millis();
}

static bool nfc_admin_is_card_removed(const String& uid)
{
    if (nfc_is_uid_present(uid)) {
        s_remove_miss_ms = 0;
        return false;
    }

    if (s_remove_miss_ms == 0) s_remove_miss_ms = millis();
    return (millis() - s_remove_miss_ms) > 300;
}

static bool build_current_bind_target(NfcAdminTarget& out)
{
    out = NfcAdminTarget{};

    switch (g_play_mode) {
        case PLAY_MODE_ARTIST_SEQ:
        case PLAY_MODE_ARTIST_RND: {
            const auto& groups = player_playlist_artist_groups();
            int group_idx = player_playlist_get_current_group_idx();
            if (group_idx < 0 || group_idx >= (int)groups.size()) return false;
            const MusicCatalogV3& cat = storage_catalog_v3();
            out.type = NFC_ADMIN_TARGET_ARTIST;
            out.key = playlist_group_name_string(cat, groups[group_idx]);
            out.display = out.key;
            return true;
        }

        case PLAY_MODE_ALBUM_SEQ:
        case PLAY_MODE_ALBUM_RND: {
            const auto& groups = player_playlist_album_groups();
            int group_idx = player_playlist_get_current_group_idx();
            if (group_idx < 0 || group_idx >= (int)groups.size()) return false;
            const MusicCatalogV3& cat = storage_catalog_v3();
            out.type = NFC_ADMIN_TARGET_ALBUM;
            // 使用"主要歌手 - 专辑名"作为key和display
            out.key = playlist_group_display_string(cat, groups[group_idx]);
            out.display = out.key;
            return true;
        }

        default: {
            int idx = player_state_current_index();
            if (idx < 0) return false;
            out.type = NFC_ADMIN_TARGET_TRACK;
            out.track_idx = idx;

            if (storage_catalog_v3_ready()) {
                TrackViewV3 view;
                if (storage_catalog_v3_get_track_view((uint32_t)idx, view)) {
                    out.key = view.audio_path;
                    out.display = view.title + " - " + view.artist;
                }
            }
            return true;
        }
    }
}
}

static NfcUiTargetType to_ui_target_type_from_bind(NfcBindType t)
{
  switch (t) {
    case NFC_BIND_TRACK:  return NFC_UI_TARGET_TRACK;
    case NFC_BIND_ARTIST: return NFC_UI_TARGET_ARTIST;
    case NFC_BIND_ALBUM:  return NFC_UI_TARGET_ALBUM;
    default:              return NFC_UI_TARGET_TRACK;
  }
}

static NfcUiTargetType to_ui_target_type_from_target(NfcAdminTargetType t)
{
  switch (t) {
    case NFC_ADMIN_TARGET_TRACK:  return NFC_UI_TARGET_TRACK;
    case NFC_ADMIN_TARGET_ARTIST: return NFC_UI_TARGET_ARTIST;
    case NFC_ADMIN_TARGET_ALBUM: return NFC_UI_TARGET_ALBUM;
    default:                      return NFC_UI_TARGET_TRACK;
  }
}

void nfc_admin_state_enter(void)
{
    LOGI("[NFC_ADMIN] enter");

    s_admin = NfcAdminCtx{};
    s_resume_play_on_exit = false;
    s_admin.enter_ms = millis();
    s_admin.step_ms = s_admin.enter_ms;

    if (!build_current_bind_target(s_admin.target)) {
        LOGI("[NFC_ADMIN] invalid bind target");
        admin_set_step(ADMIN_ERROR);
        ui_nfc_admin_show_error("无可绑定目标");
        return;
    }

    ui_enter_nfc_admin();
    ui_nfc_admin_show_wait_card(s_admin.target);
    admin_set_step(ADMIN_WAIT_CARD);
}

void nfc_admin_state_exit(void)
{
    LOGI("[NFC_ADMIN] exit");
    s_admin = NfcAdminCtx{};
}

void nfc_admin_state_run(void)
{
    nfc_poll(); //admin 模式下也要持续轮询 RC522
    
    uint32_t now = millis();
    uint32_t dt_enter = now - s_admin.enter_ms;
    uint32_t dt_step  = now - s_admin.step_ms;


    if (dt_enter > 30000UL) {
        LOGI("[NFC_ADMIN] timeout");
        app_request_exit_nfc_admin();
        return;
    }

    switch (s_admin.step) {
        case ADMIN_WAIT_CARD: {
            String uid;
            if (nfc_take_last_uid(uid)) {
                s_admin.pending_uid = uid;
                LOGI("[NFC_ADMIN] card detected: %s", uid.c_str());

                NfcBindingEntry old_entry;
                NfcUiConfirmState confirm_state;
                NfcUiTargetType old_type = NFC_UI_TARGET_TRACK;
                String old_name;

                if (!nfc_binding_find(uid, old_entry)) {
                    confirm_state = NFC_UI_CONFIRM_NEW;
                } else {
                    old_type = to_ui_target_type_from_bind(old_entry.type);
                    old_name = old_entry.display;

                    bool same = false;
                    switch (s_admin.target.type) {
                        case NFC_ADMIN_TARGET_TRACK:
                            same = (old_entry.type == NFC_BIND_TRACK && old_entry.key == s_admin.target.key);
                            break;
                        case NFC_ADMIN_TARGET_ARTIST:
                            same = (old_entry.type == NFC_BIND_ARTIST && old_entry.key == s_admin.target.key);
                            break;
                        case NFC_ADMIN_TARGET_ALBUM:
                            same = (old_entry.type == NFC_BIND_ALBUM && old_entry.key == s_admin.target.key);
                            break;
                    }

                    confirm_state = same ? NFC_UI_CONFIRM_SAME : NFC_UI_CONFIRM_REPLACE;
                }

                String new_name = s_admin.target.display;

                ui_nfc_admin_show_confirm(
                    uid,
                    confirm_state,
                    old_type,
                    old_name,
                    to_ui_target_type_from_target(s_admin.target.type),
                    new_name);
                admin_set_step(ADMIN_CONFIRM_BIND);
            }
            break;
        }

        case ADMIN_CONFIRM_BIND:
            // 等按键，不在这里主动做事
            break;

        case ADMIN_SAVING: {
            LOGI("[NFC_ADMIN] saving binding...");

            // 安全方案：保存前必须停音频并等待文件关闭
            // 如果进入保存前本来正在播放，则退出 admin 后自动恢复当前曲目。
            if (audio_service_is_playing() && !audio_service_is_paused()) {
                s_resume_play_on_exit = true;
            }
            player_control_mark_manual_stop();
            audio_service_stop(true);

            // 使用新接口保存绑定关系
            bool ok = false;
            switch (s_admin.target.type) {
                case ::NFC_ADMIN_TARGET_TRACK:
                    ok = nfc_binding_set(s_admin.pending_uid,
                                         NFC_BIND_TRACK,
                                         s_admin.target.key,
                                         s_admin.target.display);
                    break;
                case ::NFC_ADMIN_TARGET_ARTIST:
                    ok = nfc_binding_set(s_admin.pending_uid,
                                         NFC_BIND_ARTIST,
                                         s_admin.target.key,
                                         s_admin.target.display);
                    break;
                case ::NFC_ADMIN_TARGET_ALBUM:
                    ok = nfc_binding_set(s_admin.pending_uid,
                                         NFC_BIND_ALBUM,
                                         s_admin.target.key,
                                         s_admin.target.display);
                    break;
                default:
                    LOGI("[NFC_ADMIN] invalid target type");
                    break;
            }

            if (ok) {
                ok = nfc_binding_save("/System/nfc_map.txt");
            }

            s_admin.save_ok = ok;

            if (ok) {
                LOGI("[NFC_ADMIN] save ok");
                s_remove_miss_ms = 0;
                ui_nfc_admin_show_wait_remove(s_admin.pending_uid);
                admin_set_step(ADMIN_WAIT_REMOVE);
            } else {
                LOGI("[NFC_ADMIN] save failed");
                ui_nfc_admin_show_error("保存失败");
                admin_set_step(ADMIN_ERROR);
            }
            break;
        }

        case ADMIN_WAIT_REMOVE: {
            if (nfc_admin_is_card_removed(s_admin.pending_uid)) {
                nfc_ignore_uid_once(s_admin.pending_uid, 1500);
                ui_nfc_admin_show_done();
                admin_set_step(ADMIN_DONE);
            }
            break;
        }

        case ADMIN_DONE:
            if (dt_step > 800UL) {
                app_request_exit_nfc_admin();
            }
            break;

        case ADMIN_ERROR:
            if (dt_step > 1200UL) {
                app_request_exit_nfc_admin();
            }
            break;

        default:
            break;
    }
}

bool nfc_admin_state_consume_resume_request(void)
{
    bool ret = s_resume_play_on_exit;
    s_resume_play_on_exit = false;
    return ret;
}

void nfc_admin_state_on_key(NfcAdminKey key)
{
    LOGI("[NFC_ADMIN] on_key key=%d", (int)key);
    switch (key) {
        case NFC_ADMIN_KEY_MODE_SHORT:
            LOGI("[NFC_ADMIN] cancel by MODE");
            app_request_exit_nfc_admin();
            break;

        case NFC_ADMIN_KEY_PLAY_SHORT:
            if (s_admin.step == ADMIN_CONFIRM_BIND && s_admin.pending_uid.length() > 0) {
                LOGI("[NFC_ADMIN] confirm bind uid=%s type=%d",
                     s_admin.pending_uid.c_str(),
                     (int)s_admin.target.type);
                ui_nfc_admin_show_saving();
                admin_set_step(ADMIN_SAVING);
            }
            break;

        default:
            break;
    }
}
