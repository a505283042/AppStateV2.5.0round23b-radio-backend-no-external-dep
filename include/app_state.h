/* 应用状态管理模块头文件 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include "ui/ui.h"  /* 包含UI模块头文件 */

/* 定义应用程序的各种状态 */
typedef enum {
    STATE_BOOT = 0,      /* 启动状态 */
    STATE_PLAYER,        /* 音乐播放器状态 */
    STATE_NFC_ADMIN,     /* NFC管理状态 */
    STATE_RADIO,         /* 收音机状态 */
} app_state_t;

/* 全局应用状态变量 */
extern app_state_t g_app_state;

/* 应用状态管理函数声明 */
void app_state_init(void);    /* 初始化应用状态 */
void app_state_update(void);  /* 更新应用状态 */

/* NFC 管理状态入口函数 */
bool app_request_enter_nfc_admin();  /* 请求进入 NFC 管理状态 */
void app_request_exit_nfc_admin();   /* 请求退出 NFC 管理状态 */

bool app_request_enter_nfc_admin_with_target(const NfcAdminTarget& target); /* 请求进入 NFC 管理状态并指定目标 */

bool app_request_start_rescan();      /* 请求开始重扫（供按键 / 网页统一调用） */
bool app_request_cancel_rescan();     /* 请求取消重扫（供按键 / 网页统一调用） */

#endif
