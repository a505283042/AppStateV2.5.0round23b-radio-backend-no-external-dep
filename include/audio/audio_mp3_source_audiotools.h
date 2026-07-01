#pragma once

#include "audio/audio_mp3_source.h"

bool audio_mp3_audiotools_source_open(const char* url, AudioMp3Source& out_source);
// HTTP MP3 文件从指定字节偏移起播。offset>0 时会发送 Range: bytes=offset-，
// 仅在服务器返回 206 Partial Content 时成功；失败时由上层回退普通 URL 起播。
bool audio_mp3_audiotools_source_open_from_offset(const char* url, uint32_t start_offset, AudioMp3Source& out_source);
void audio_mp3_audiotools_source_close();

int audio_mp3_audiotools_source_available();
bool audio_mp3_audiotools_source_connected();