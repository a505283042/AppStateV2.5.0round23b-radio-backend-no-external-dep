#pragma once

#include <stddef.h>
#include <stdint.h>

namespace AudioDecodeWorkspace {

// MP3 与 FLAC 由同一个 AudioTask 串行驱动，不会同时解码。
// 共用一块按 FLAC 最大单次解码量配置的内部 PCM 工作区，
// 避免两个解码器分别常驻一份缓冲。
static constexpr size_t kPcmSamples = 8256;

extern int16_t pcm[kPcmSamples];

}  // namespace AudioDecodeWorkspace
