#include "player_binding.h"

#include "app_flags.h"
#include "hal/hall_control.h"
#include "nfc/nfc_binding.h"
#include "player_playlist.h"
#include "player_recover.h"
#include "player_state.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_groups_v3.h"
#include "ui/ui.h"
#include "utils/log.h"

// 只给本文件用的内部变量和内部函数
namespace {

PlayerBindingHooks s_hooks{};

static int s_nfc_last_track_idx = -1;
static uint32_t s_nfc_last_track_ms = 0;

struct ResolvedNfcAction {
    bool valid = false;
    NfcBindType type = NFC_BIND_UNKNOWN;
    int track_idx = -1;
    int group_idx = -1;
    String key;
    String display;
};

// 电磁铁动作期间只保留一条已经解析成功的 NFC 目标。
// 目标数据很小，且只存在约 260ms，不提前修改播放列表或打开歌曲文件。
static ResolvedNfcAction s_pending_nfc_action;

static void clear_pending_nfc_action()
{
    s_pending_nfc_action = ResolvedNfcAction{};
}

static const char* nfc_action_type_label(NfcBindType type)
{
    switch (type) {
        case NFC_BIND_TRACK:  return "单曲";
        case NFC_BIND_ARTIST: return "歌手";
        case NFC_BIND_ALBUM:  return "专辑";
        default:              return "未知";
    }
}

// NFC 防重入 helper 函数
static bool nfc_binding_should_suppress_duplicate(int track_idx)
{
    if (track_idx < 0) return false;

    const uint32_t now = millis();
    if (track_idx == s_nfc_last_track_idx &&
        static_cast<uint32_t>(now - s_nfc_last_track_ms) < 1000) {
        LOGW("[NFC] 已抑制重复歌曲触发：索引=%d 间隔=%ums",
             track_idx,
             static_cast<unsigned>(now - s_nfc_last_track_ms));
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
    const play_mode_t mode = app_play_mode_get();
    return mode == PLAY_MODE_ALL_RND ||
           mode == PLAY_MODE_ARTIST_RND ||
           mode == PLAY_MODE_ALBUM_RND;
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
    const play_mode_t mode = nfc_binding_mode_for_type(type);
    (void)app_play_mode_set(mode, AppPlayModeChangeReason::NfcBinding);
    const bool is_random = mode == PLAY_MODE_ALL_RND ||
                           mode == PLAY_MODE_ARTIST_RND ||
                           mode == PLAY_MODE_ALBUM_RND;

    LOGD("[NFC] 应用播放模式：类型=%d -> 模式=%d 随机=%d",
         static_cast<int>(type),
         static_cast<int>(mode),
         is_random ? 1 : 0);
}

static bool resolve_nfc_action(const NfcBindingEntry& entry,
                               ResolvedNfcAction& out)
{
    out = ResolvedNfcAction{};
    out.type = entry.type;
    out.key = entry.key;
    out.display = entry.display;

    switch (entry.type) {
        case NFC_BIND_TRACK: {
            out.track_idx = player_recover_find_track_idx_by_path(entry.key);
            out.group_idx = -1;
            break;
        }

        case NFC_BIND_ARTIST: {
            const MusicCatalogV3& cat = storage_catalog_v3();
            const auto& groups = binding_artist_groups();
            for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
                if (playlist_group_name_string(cat, groups[i]) == entry.key &&
                    !groups[i].track_indices.empty()) {
                    out.group_idx = i;
                    out.track_idx = static_cast<int>(groups[i].track_indices[0]);
                    break;
                }
            }
            break;
        }

        case NFC_BIND_ALBUM: {
            const MusicCatalogV3& cat = storage_catalog_v3();
            const auto& groups = binding_album_groups();
            for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
                if (playlist_group_display_string(cat, groups[i]) == entry.key &&
                    !groups[i].track_indices.empty()) {
                    out.group_idx = i;
                    out.track_idx = static_cast<int>(groups[i].track_indices[0]);
                    break;
                }
            }
            break;
        }

        case NFC_BIND_UNKNOWN:
        default:
            break;
    }

    out.valid = out.track_idx >= 0 &&
                (entry.type == NFC_BIND_TRACK || out.group_idx >= 0);
    if (!out.valid) {
        LOGW("[NFC] 绑定目标无法解析：类型=%s key=%s",
             nfc_action_type_label(entry.type),
             entry.key.c_str());
    }
    return out.valid;
}

static bool execute_resolved_nfc_action(const ResolvedNfcAction& action)
{
    if (!action.valid || action.track_idx < 0) {
        return false;
    }

    int play_idx = action.track_idx;
    nfc_binding_apply_mode(action.type);

    if (action.type == NFC_BIND_TRACK) {
        player_playlist_set_current_group_idx(-1);
        player_playlist_force_rebuild();
    } else {
        player_playlist_set_current_group_idx(action.group_idx);
        player_playlist_force_rebuild();
        player_playlist_ensure_current();
        play_idx = player_playlist_current_track_at(0);
        if (play_idx < 0) {
            LOGE("[NFC] 到位后播放列表为空：类型=%s 分组=%d",
                 nfc_action_type_label(action.type),
                 action.group_idx);
            return false;
        }
    }

    if (nfc_binding_should_suppress_duplicate(play_idx)) {
        return true;
    }

    player_state_mark_next_play_from_nfc();
    const bool ok = binding_play_track_dispatch(play_idx, true, true);
    LOGI("[NFC] 绑定起播%s：类型=%s 索引=%d 目标=%s",
         ok ? "成功" : "失败",
         nfc_action_type_label(action.type),
         play_idx,
         action.key.c_str());
    return ok;
}

static void nfc_play_position_complete(bool success)
{
    if (!s_pending_nfc_action.valid) {
        LOGW("[NFC] 收到摆臂完成回调，但没有待执行绑定");
        return;
    }

    const ResolvedNfcAction action = s_pending_nfc_action;
    clear_pending_nfc_action();

    if (!success) {
        LOGE("[NFC] 摆臂未到播放位，已取消绑定起播：类型=%s 目标=%s",
             nfc_action_type_label(action.type),
             action.key.c_str());
        return;
    }

    LOGI("[NFC] 摆臂已到播放位，开始执行绑定：类型=%s 目标=%s",
         nfc_action_type_label(action.type),
         action.key.c_str());
    if (!execute_resolved_nfc_action(action)) {
        ui_show_notice_popup("NFC播放失败", "绑定目标无法起播");
    }
}

static bool dispatch_nfc_action_with_hall(const ResolvedNfcAction& action)
{
    if (s_pending_nfc_action.valid) {
        LOGW("[NFC] 已有绑定等待摆臂到位，本次刷卡忽略：目标=%s",
             action.key.c_str());
        ui_show_notice_popup("NFC请求处理中", "请等待摆臂到位");
        return true;
    }

    // 先保存已经验证过的目标，再请求机械动作。此时尚未修改播放模式和播放列表。
    s_pending_nfc_action = action;
    const HallPlayPositionRequestResult result =
        hall_control_request_play_position(nfc_play_position_complete);

    switch (result) {
        case HallPlayPositionRequestResult::Ready: {
            const ResolvedNfcAction ready_action = s_pending_nfc_action;
            clear_pending_nfc_action();
            return execute_resolved_nfc_action(ready_action);
        }

        case HallPlayPositionRequestResult::Started:
            LOGI("[NFC] 有效绑定已暂存，等待摆臂离开霍尔：类型=%s 目标=%s",
                 nfc_action_type_label(action.type),
                 action.key.c_str());
            return true;

        case HallPlayPositionRequestResult::Failed:
        default:
            clear_pending_nfc_action();
            LOGE("[NFC] 摆臂动作未启动，绑定目标不会播放：类型=%s 目标=%s",
                 nfc_action_type_label(action.type),
                 action.key.c_str());
            return false;
    }
}

} // namespace

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

    ResolvedNfcAction action;
    if (!resolve_nfc_action(entry, action)) {
        return true;
    }

    LOGI("[NFC] UID 已匹配：类型=%s 索引=%d 目标=%s",
         nfc_action_type_label(action.type),
         action.track_idx,
         action.key.c_str());
    (void)dispatch_nfc_action_with_hall(action);
    return true;
}

// 尝试播放艺术家。该接口用于网页管理测试，保持立即执行语义。
bool player_play_artist_binding(const String& artist)
{
    String key = artist;
    key.trim();
    if (key.isEmpty()) {
        LOGW("[播放器] 歌手 binding 失败: 为空 歌手");
        return false;
    }

    NfcBindingEntry entry;
    entry.type = NFC_BIND_ARTIST;
    entry.key = key;
    entry.display = key;

    ResolvedNfcAction action;
    if (!resolve_nfc_action(entry, action)) {
        LOGD("[播放器] 歌手 binding 未找到: %s", key.c_str());
        return false;
    }

    return execute_resolved_nfc_action(action);
}

// 尝试播放专辑。该接口用于网页管理测试，保持立即执行语义。
bool player_play_album_binding(const String& album)
{
    String key = album;
    key.trim();
    if (key.isEmpty()) {
        LOGW("[播放器] 专辑 binding 失败: 为空 专辑");
        return false;
    }

    NfcBindingEntry entry;
    entry.type = NFC_BIND_ALBUM;
    entry.key = key;
    entry.display = key;

    ResolvedNfcAction action;
    if (!resolve_nfc_action(entry, action)) {
        LOGD("[播放器] 专辑 binding 未找到: %s", key.c_str());
        return false;
    }

    return execute_resolved_nfc_action(action);
}
