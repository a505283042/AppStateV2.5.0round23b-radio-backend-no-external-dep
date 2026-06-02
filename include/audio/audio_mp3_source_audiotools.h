#pragma once

#include "audio/audio_mp3_source.h"

bool audio_mp3_audiotools_source_open(const char* url, AudioMp3Source& out_source);
void audio_mp3_audiotools_source_close();
bool audio_mp3_audiotools_source_is_open();
const char* audio_mp3_audiotools_source_url();
int audio_mp3_audiotools_source_available();
bool audio_mp3_audiotools_source_connected();