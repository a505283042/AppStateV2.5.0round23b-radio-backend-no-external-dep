/* NFC管理状态模块头文件 */
#ifndef NFC_ADMIN_STATE_H
#define NFC_ADMIN_STATE_H

#include <stdint.h>  /* 包含标准整数类型定义 */
#include "ui/ui.h"   /* 包含UI模块头文件 */

/* =========================================================
 * NFC管理状态模块
 * 用于绑定NFC标签到专辑/文件夹
 * ========================================================= */

/* NFC管理状态按键事件枚举 */
enum NfcAdminKey {
    NFC_ADMIN_KEY_MODE_SHORT = 0,
    NFC_ADMIN_KEY_PLAY_SHORT,
};

/* NFC管理状态处理函数声明 */
void nfc_admin_state_enter(void);  /* 进入NFC管理状态 */
void nfc_admin_state_exit(void);   /* 退出NFC管理状态 */
void nfc_admin_state_run(void);    /* 运行NFC管理状态 */
void nfc_admin_state_on_key(NfcAdminKey key);  /* NFC管理状态按键处理 */
bool nfc_admin_state_consume_resume_request(void); /* 退出admin后是否恢复播放 */

void nfc_admin_state_set_override_target(const NfcAdminTarget& target); /* 设置NFC管理状态覆盖目标 */
void nfc_admin_state_clear_override_target(void); /* 清除NFC管理状态覆盖目标 */

#endif // NFC_ADMIN_STATE_H
