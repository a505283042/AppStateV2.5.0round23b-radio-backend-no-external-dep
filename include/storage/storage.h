/* 存储系统(SD/文件系统)模块头文件 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* 存储系统初始化 */
bool storage_init(void);

/* 挂载 / 卸载 TF 卡。storage_init() 保持兼容，内部等价于 storage_mount()。 */
bool storage_mount(void);
bool storage_unmount(void);

/* 存储状态 */
bool storage_is_ready(void);

/* 立即标记 TF 不可用，用于拔卡确认后阻止新的文件访问。 */
void storage_mark_not_ready(void);

/* 没有 CD 脚时的软件探测：轻量打开根目录，判断当前挂载是否还可用。 */
bool storage_probe_alive(void);

/* SD 访问失败上报。热插拔状态机会据此缩短确认拔卡的探测间隔。 */
void storage_report_io_error(const char* where);
bool storage_has_recent_io_error(void);
void storage_clear_io_error(void);

/* TF 卡唯一标识 */
uint32_t storage_card_hash(void);
const char* storage_card_snapshot_key(void);
void storage_clear_card_identity(void);

/* 调试用：列出根目录 */
void storage_list_root(void);

#endif // STORAGE_H