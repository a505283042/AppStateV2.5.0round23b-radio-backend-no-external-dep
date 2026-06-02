#pragma once
#include <stdbool.h>
#include <SdFat.h>

bool audio_flac_start(SdFat& sd, const char* path);
void audio_flac_stop();
bool audio_flac_loop();
