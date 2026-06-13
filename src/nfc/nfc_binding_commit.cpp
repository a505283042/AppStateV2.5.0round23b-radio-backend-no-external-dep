#include "nfc/nfc_binding_commit.h"

#include "audio/audio_service.h"
#include "player_control.h"
#include "player_source.h"
#include "player_state.h"
#include "utils/log.h"

namespace {

struct NfcBindingResumeCtx {
    bool valid = false;
    int track_idx = -1;
};

static NfcBindingResumeCtx nfc_binding_capture_resume_ctx(bool allow_resume)
{
    NfcBindingResumeCtx ctx{};
    if (!allow_resume) return ctx;

    if (!audio_service_is_playing() || audio_service_is_paused()) {
        return ctx;
    }

    const PlayerSourceState source = player_source_get();
    if (source.type != PlayerSourceType::LOCAL_TRACK) {
        LOGD("[NFC绑定] 恢复 已跳过: 来源 is not 本地 歌曲");
        return ctx;
    }

    const int idx = player_state_current_index();
    if (idx < 0) {
        LOGD("[NFC绑定] 恢复 已跳过: no 当前 歌曲");
        return ctx;
    }

    ctx.valid = true;
    ctx.track_idx = idx;
    return ctx;
}

static void nfc_binding_try_resume_after_commit(const NfcBindingResumeCtx& ctx)
{
    if (!ctx.valid) return;

    if (!player_play_idx_v3((uint32_t)ctx.track_idx, true, true)) {
        LOGW("[NFC绑定] 提交后恢复 失败: 歌曲=%d", ctx.track_idx);
        return;
    }

    LOGI("[NFC绑定] 提交后恢复 成功: 歌曲=%d", ctx.track_idx);
}

static bool nfc_binding_prepare_safe_commit(bool* was_playing_before)
{
    const bool was_playing = audio_service_is_playing() && !audio_service_is_paused();
    if (was_playing_before) {
        *was_playing_before = was_playing;
    }

    // 与 NFC admin 保存保持一致：保存映射前先停音频，避免本地播放与 nfc_map.txt 同时占用 SD。
    player_control_mark_manual_stop();
    audio_service_stop(true);
    return true;
}

static bool nfc_binding_save_map_with_rollback()
{
    if (nfc_binding_save("/System/nfc_map.txt")) {
        return true;
    }

    LOGW("[NFC绑定] 保存 失败, re加载 文件 rollback");
    (void)nfc_binding_load("/System/nfc_map.txt");
    return false;
}

} // 匿名命名空间

bool nfc_binding_set_and_save_safely(const String& uid,
                                     NfcBindType type,
                                     const String& key,
                                     const String& display,
                                     bool* was_playing_before)
{
    nfc_binding_prepare_safe_commit(was_playing_before);

    if (!nfc_binding_set(uid, type, key, display)) {
        LOGW("[NFC绑定] 安全设置失败：UID=%s", uid.c_str());
        return false;
    }

    return nfc_binding_save_map_with_rollback();
}

bool nfc_binding_remove_and_save_safely(const String& uid,
                                        bool* was_playing_before,
                                        bool resume_playback_after_commit)
{
    const NfcBindingResumeCtx resume_ctx = 
        nfc_binding_capture_resume_ctx(resume_playback_after_commit);

    nfc_binding_prepare_safe_commit(was_playing_before);

    if (!nfc_binding_remove(uid)) {
        LOGW("[NFC绑定] 安全删除失败：UID=%s", uid.c_str());
        return false;
    }

    if (!nfc_binding_save_map_with_rollback()) {
        return false;
    }

    nfc_binding_try_resume_after_commit(resume_ctx);
    return true;
}