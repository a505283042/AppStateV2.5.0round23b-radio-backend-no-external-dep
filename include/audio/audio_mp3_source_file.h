#pragma once

#include <SdFat.h>

#include "audio/audio_mp3_source.h"

bool audio_mp3_file_source_open(SdFat& sd, const char* path, AudioMp3Source& out_source);
void audio_mp3_file_source_close();

