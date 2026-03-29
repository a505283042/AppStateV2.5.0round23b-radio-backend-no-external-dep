#pragma once

#include <SdFat.h>

#include "audio/audio_mp3_source.h"

bool audio_mp3_file_source_open(SdFat& sd, const char* path, AudioMp3Source& out_source);
void audio_mp3_file_source_close();
bool audio_mp3_file_source_is_open();
const char* audio_mp3_file_source_path();