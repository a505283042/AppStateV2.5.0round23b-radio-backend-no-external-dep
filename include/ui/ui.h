/* 用户界面系统接口模块头文件 */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <freertos/task.h>
#include "storage/storage_types_v3.h"

/**
 * @brief UI 对外接口总头。
 *
 * 当前 UI 已经拆分为：
 * - ui.cpp：UiTask / 屏幕与刷新编排
 * - ui_player_render.cpp：播放器两种主视图绘制
 * - ui_cover_pipeline.cpp：封面加载 / 缩放 / 缓存
 * - ui_pages.cpp：boot / 扫描 / NFC admin 等页面
 * - ui_list_select.cpp：列表选择页面
 *
 * 这里保留的是“其他模块真正会调用的公开接口”。
 */

/* UI模式/屏幕类型定义 */
typedef enum {
    UI_SCREEN_BOOT = 0,      /* 启动界面 */
    UI_SCREEN_PLAYER,        /* 播放器界面 */
    UI_SCREEN_NFC_ADMIN,     /* NFC管理界面 */
} ui_screen_t;

/* 播放器视图类型 */
enum ui_player_view_t : uint8_t { UI_VIEW_ROTATE = 0, UI_VIEW_INFO = 1 };

/* 播放模式类型（大类 + 小类组合后的 6 个实际状态） */
typedef enum {
    PLAY_MODE_ALL_SEQ = 0,
    PLAY_MODE_ALL_RND,
    PLAY_MODE_ARTIST_SEQ,
    PLAY_MODE_ARTIST_RND,
    PLAY_MODE_ALBUM_SEQ,
    PLAY_MODE_ALBUM_RND,
} play_mode_t;

/* -------- UI生命周期函数 -------- */
void ui_init(void);                        /* 初始化 UI、创建 UiTask、准备基础绘图资源 */
void ui_set_screen(ui_screen_t screen);    /* 切换当前显示页面 */

/* -------- 通用提示 -------- */

/* -------- 页面切换 -------- */
void ui_enter_player(void);                /* 进入播放器页 */
void ui_return_to_player(void);            /* 从 NFC admin / 扫描页轻量返回播放器 */
void ui_enter_nfc_admin(void);             /* 进入 NFC 管理页 */

/* -------- 封面主图 / 缓存 -------- */
bool ui_draw_cover_for_track(const TrackInfo& t, bool force_redraw = false);

// 封面解码拆分（先读入内存，再在安全时机缩放到精灵）
bool ui_cover_load_to_memory(const TrackInfo& t);  // 从 SD 读取封面到共享内存
bool ui_cover_scale_from_memory();                 // 从共享内存缩放到当前封面精灵（不访问 SD）

// 从调用方提供的原始图片缓冲解码缩放到精灵（不访问 SD）
bool ui_cover_scale_from_buffer(const uint8_t* ptr, size_t len, bool is_png);
// 预读并缩放到“下一首封面缓存”，不立即显示；命中后可瞬时切换
bool ui_cover_scale_to_cache_from_buffer(const uint8_t* ptr, size_t len, bool is_png, int track_idx);
bool ui_cover_apply_cached(int track_idx);
bool ui_cover_cache_is_ready(int track_idx);
void ui_cover_cache_invalidate();

// 读取一份独立分配的原始封面缓冲；调用方后续用 ui_cover_scale_from_buffer / ui_cover_free_allocated
bool ui_cover_load_allocated(const TrackInfo& t, uint8_t*& out_buf, size_t& out_len, bool& out_is_png);
void ui_cover_free_allocated(uint8_t* p);

/* -------- 扫描页 -------- */
void ui_scan_begin();
void ui_scan_tick(int tracks_count);
void ui_scan_end();
void ui_scan_abort();

/* -------- 播放器视图 / 状态 -------- */
void ui_toggle_view();
enum ui_player_view_t ui_get_view();
void ui_set_now_playing(const char* title, const char* artist);
void ui_set_album(const String& album);
void ui_set_volume(uint8_t vol);
void ui_volume_key_pressed();
void ui_set_play_mode(play_mode_t mode);
void ui_mode_switch_highlight();
void ui_set_track_pos(int idx, int total);

/* -------- UiTask 协调 -------- */
void ui_hold_render(bool hold);
void ui_request_refresh_now();
void ui_set_rotate_wait_prefetch(bool wait);
TaskHandle_t ui_get_task_handle(void);

/* -------- NFC 管理页面 -------- */
enum NfcUiConfirmState {
    NFC_UI_CONFIRM_NEW = 0,
    NFC_UI_CONFIRM_REPLACE,
    NFC_UI_CONFIRM_SAME,
};

enum NfcUiTargetType {
    NFC_UI_TARGET_TRACK = 0,
    NFC_UI_TARGET_ARTIST,
    NFC_UI_TARGET_ALBUM,
};

enum NfcAdminTargetType {
    NFC_ADMIN_TARGET_TRACK = 0,
    NFC_ADMIN_TARGET_ARTIST,
    NFC_ADMIN_TARGET_ALBUM,
};

/** NFC 管理页里“当前准备写入什么目标”的描述。 */
struct NfcAdminTarget {
    NfcAdminTargetType type = NFC_ADMIN_TARGET_TRACK;
    int track_idx = -1;
    String key;
    String display;
};

void ui_nfc_admin_show_wait_card(const NfcAdminTarget& target);
void ui_nfc_admin_show_confirm(const String& uid, NfcUiConfirmState state, NfcUiTargetType old_type, const String& old_name, NfcUiTargetType new_type, const String& new_name);
void ui_nfc_admin_show_saving();
void ui_nfc_admin_show_wait_remove(const String& uid);
void ui_nfc_admin_show_error(const char* msg);
void ui_nfc_admin_show_done();

#endif // UI_H
