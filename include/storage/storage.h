/* 存储系统(SD/文件系统)模块头文件 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>

/* 存储系统初始化 */
bool storage_init(void);

/* 存储状态 */
bool storage_is_ready(void);

/* 调试用：列出根目录 */
void storage_list_root(void);

#endif // STORAGE_H
