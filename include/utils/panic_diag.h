#pragma once

#include <stdbool.h>

// 在 TF 卡挂载成功后调用。
// 如果上一次发生 panic/WDT 并且 flash 里存在 core dump，
// 会复制到 /System/crash/coredump_xxxxxx.bin，同时写 /System/crash/panic_xxxxxx.txt 和 /System/crash/panic_summary.txt。
bool panic_diag_flush_to_sd(void);

// 仅写入 reset reason 摘要，不要求一定有 core dump。
// 通常不需要直接调用，panic_diag_flush_to_sd() 内部会处理。
const char* panic_diag_reset_reason_name(int reason);
