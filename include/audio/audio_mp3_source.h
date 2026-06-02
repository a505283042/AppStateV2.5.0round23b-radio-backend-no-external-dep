#pragma once

#include <stddef.h>
#include <stdint.h>

// 通用 MP3 字节源抽象：
// 1) 本地文件：read() 返回实际字节数；读到文件尾返回 AUDIO_MP3_SOURCE_EOF
// 2) 网络流：read() 返回实际字节数；暂时无数据返回 AUDIO_MP3_SOURCE_WOULD_BLOCK；
//    真正断流/结束返回 AUDIO_MP3_SOURCE_EOF；错误返回 AUDIO_MP3_SOURCE_ERROR

constexpr int AUDIO_MP3_SOURCE_WOULD_BLOCK = 0;
constexpr int AUDIO_MP3_SOURCE_EOF = -1;
constexpr int AUDIO_MP3_SOURCE_ERROR = -2;

struct AudioMp3Source {
  void* ctx = nullptr;

  // 返回值语义：
  // >0  : 实际读到的字节数
  //  0  : 暂时没有数据（流式 source 可用）
  // -1  : 错误
  // -2  : 真正结束 / EOF
  int (*read)(void* ctx, uint8_t* dst, size_t bytes) = nullptr;

  void (*close)(void* ctx) = nullptr;

  const char* debug_name = nullptr;
  bool is_stream = false;
};