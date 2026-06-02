#include "radio/radio_catalog.h"

#include <SdFat.h>

#include "storage/storage_io.h"
#include "utils/log.h"

extern SdFat sd;

namespace {
std::vector<RadioItem> s_items;
bool s_loaded = false;
String s_error;
constexpr const char* kRadioListPath = "/System/radio_list.txt";

static String trim_copy(const String& in) {
  String s = in;
  s.trim();
  return s;
}

static bool parse_line(const String& raw, RadioItem& out) {
  String line = trim_copy(raw);
  if (!line.length()) return false;
  if (line.startsWith("#") || line.startsWith(";")) return false;

  int p1 = line.indexOf('|');
  if (p1 <= 0) return false;
  int p2 = line.indexOf('|', p1 + 1);
  int p3 = p2 >= 0 ? line.indexOf('|', p2 + 1) : -1;
  int p4 = p3 >= 0 ? line.indexOf('|', p3 + 1) : -1;

  out = RadioItem{};
  out.name = trim_copy(line.substring(0, p1));
  if (p2 < 0) {
    out.url = trim_copy(line.substring(p1 + 1));
  } else {
    out.url = trim_copy(line.substring(p1 + 1, p2));
    if (p3 < 0) {
      out.format = trim_copy(line.substring(p2 + 1));
    } else {
      out.format = trim_copy(line.substring(p2 + 1, p3));
      if (p4 < 0) {
        out.region = trim_copy(line.substring(p3 + 1));
      } else {
        out.region = trim_copy(line.substring(p3 + 1, p4));
        out.logo = trim_copy(line.substring(p4 + 1));
      }
    }
  }

  out.valid = out.name.length() > 0 && out.url.length() > 0;
  return out.valid;
}
}  // namespace

bool radio_catalog_load() {
  s_items.clear();
  s_loaded = false;
  s_error = "";

  StorageSdLockGuard guard(1200);
  if (!guard) {
    s_error = "sd_lock_failed";
    LOGW("[RADIO] catalog load skipped: SD lock failed");
    return false;
  }

  File32 f = sd.open(kRadioListPath, O_RDONLY);
  if (!f) {
    s_error = "radio_list_missing";
    LOGW("[RADIO] list not found: %s", kRadioListPath);
    return false;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    RadioItem item{};
    if (parse_line(line, item)) s_items.push_back(item);
  }
  f.close();

  s_loaded = true;
  LOGI("[RADIO] catalog loaded items=%d path=%s", (int)s_items.size(), kRadioListPath);
  return true;
}

bool radio_catalog_is_loaded() { return s_loaded; }
size_t radio_catalog_count() { return s_items.size(); }
const RadioItem* radio_catalog_get(size_t idx) {
  if (idx >= s_items.size()) return nullptr;
  return &s_items[idx];
}
const std::vector<RadioItem>& radio_catalog_items() { return s_items; }
String radio_catalog_error() { return s_error; }
const char* radio_catalog_path() { return kRadioListPath; }
