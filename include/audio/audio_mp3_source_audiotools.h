#pragma once

#include "audio/audio_mp3_source.h"

bool audio_mp3_audiotools_source_open(const char* url, AudioMp3Source& out_source);
void audio_mp3_audiotools_source_close();

int audio_mp3_audiotools_source_available();
bool audio_mp3_audiotools_source_connected();