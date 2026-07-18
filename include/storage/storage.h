/* 存储系统(SD/文件系统)模块头文件 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 存储系统初始化 */
bool storage_init(void);

/* 挂载 / 卸载 TF 卡。storage_init() 保持兼容，内部等价于 storage_mount()。 */
bool storage_mount(void);
bool storage_unmount(void);

/* 存储运行态快照。所有字段来自同一个锁内时间点。 */
struct StorageRuntimeSnapshot {
    bool ready = false;
    bool recent_io_error = false;
    uint32_t io_error_generation = 0;
    uint32_t card_hash = 0;
    char card_snapshot_key[16] = {0};
    uint32_t revision = 0;
};

StorageRuntimeSnapshot storage_runtime_snapshot_get(void);

/* 存储状态 */
bool storage_is_ready(void);

/* 立即标记 TF 不可用，用于拔卡确认后阻止新的文件访问。 */
void storage_mark_not_ready(void);

/* 没有 CD 脚时的软件探测：轻量打开根目录，判断当前挂载是否还可用。 */
bool storage_probe_alive(void);

/* SD 访问失败上报。热插拔状态机会据此缩短确认拔卡的探测间隔。 */
void storage_report_io_error(const char* where);
bool storage_has_recent_io_error(void);

/* 仅当错误代次仍等于 observed_generation 时清除，避免覆盖刚发生的新 IO 错误。 */
bool storage_clear_io_error_if_generation(uint32_t observed_generation);

/* TF 卡唯一标识 */
uint32_t storage_card_hash(void);
bool storage_copy_card_snapshot_key(char* out, size_t out_size);

/* 调试用：列出根目录 */
void storage_list_root(void);

#endif // STORAGE_H