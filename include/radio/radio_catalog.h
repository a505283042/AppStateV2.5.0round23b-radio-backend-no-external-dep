#pragma once

#include <Arduino.h>
#include <vector>

struct RadioItem {
  String name;
  String url;
  String format;
  String region;
  String logo;
  bool valid = false;
};

bool radio_catalog_load();
bool radio_catalog_is_loaded();
size_t radio_catalog_count();
const RadioItem* radio_catalog_get(size_t idx);
const std::vector<RadioItem>& radio_catalog_items();
String radio_catalog_error();
const char* radio_catalog_path();
