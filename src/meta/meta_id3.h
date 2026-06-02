#pragma once
#include <Arduino.h>
#include <SdFat.h>

struct Id3BasicInfo {
  String title;
  String artist;
  String album;
};

bool id3_read_basic(SdFat& sd, const char* path, Id3BasicInfo& out);
