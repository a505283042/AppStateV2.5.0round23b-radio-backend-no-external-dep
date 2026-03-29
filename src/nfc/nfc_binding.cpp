#include "nfc/nfc_binding.h"
#include "utils/log.h"
#include "storage/storage_io.h"

#include <SdFat.h>

// 外部声明全局 SD 对象（定义在 storage.cpp）
extern SdFat sd;

#include <vector>

static std::vector<NfcBindingEntry> s_bindings;

static void ensure_capacity_once()
{
    static bool inited = false;
    if (!inited) {
        s_bindings.reserve(128);   // 建议预留一个常用容量，减少反复扩容
        inited = true;
    }
}

static String trim_copy(const String& s)
{
    String t = s;
    t.trim();
    return t;
}

static String sanitize_field(const String& s)
{
    String out = s;
    out.replace("\r", " ");
    out.replace("\n", " ");
    out.replace("|", "/");   // 简单避免破坏分隔符
    return out;
}

static bool split4(const String& line, String& a, String& b, String& c, String& d)
{
    int p1 = line.indexOf('|');
    if (p1 < 0) return false;
    int p2 = line.indexOf('|', p1 + 1);
    if (p2 < 0) return false;
    int p3 = line.indexOf('|', p2 + 1);
    if (p3 < 0) return false;

    a = line.substring(0, p1);
    b = line.substring(p1 + 1, p2);
    c = line.substring(p2 + 1, p3);
    d = line.substring(p3 + 1);

    a.trim();
    b.trim();
    c.trim();
    d.trim();
    return true;
}

static bool split_old_uid_path(const String& line, String& uid, String& path)
{
    int eq = line.indexOf('=');
    if (eq < 0) return false;

    uid = line.substring(0, eq);
    path = line.substring(eq + 1);
    uid.trim();
    path.trim();
    return !(uid.isEmpty() || path.isEmpty());
}

static String basename_no_ext(const String& path)
{
    int slash = path.lastIndexOf('/');
    String name = (slash >= 0) ? path.substring(slash + 1) : path;

    int dot = name.lastIndexOf('.');
    if (dot > 0) {
        name = name.substring(0, dot);
    }
    return name;
}

const char* nfc_binding_type_to_cstr(NfcBindType type)
{
    switch (type) {
        case NFC_BIND_TRACK:  return "track";
        case NFC_BIND_ARTIST: return "artist";
        case NFC_BIND_ALBUM:  return "album";
        default:              return "unknown";
    }
}

NfcBindType nfc_binding_type_from_str(const String& s)
{
    String t = s;
    t.trim();
    t.toLowerCase();

    if (t == "track")  return NFC_BIND_TRACK;
    if (t == "artist") return NFC_BIND_ARTIST;
    if (t == "album")  return NFC_BIND_ALBUM;
    return NFC_BIND_UNKNOWN;
}

void nfc_binding_clear()
{
    ensure_capacity_once();
    s_bindings.clear();
}

int nfc_binding_count()
{
    return (int)s_bindings.size();
}

int nfc_binding_find_index(const String& uid)
{
    for (int i = 0; i < (int)s_bindings.size(); ++i) {
        if (s_bindings[i].uid == uid) return i;
    }
    return -1;
}

bool nfc_binding_find(const String& uid, NfcBindingEntry& out)
{
    int idx = nfc_binding_find_index(uid);
    if (idx < 0) return false;
    out = s_bindings[idx];
    return true;
}

bool nfc_binding_get(int index, NfcBindingEntry& out)
{
    if (index < 0 || index >= (int)s_bindings.size()) return false;
    out = s_bindings[index];
    return true;
}

bool nfc_binding_set(const String& uid,
                     NfcBindType type,
                     const String& key,
                     const String& display)
{
    ensure_capacity_once();

    if (uid.isEmpty() || key.isEmpty()) {
        LOGI("[NFC_BIND] set failed: empty uid/key");
        return false;
    }

    if (type == NFC_BIND_UNKNOWN) {
        LOGI("[NFC_BIND] set failed: unknown type");
        return false;
    }

    int idx = nfc_binding_find_index(uid);

    NfcBindingEntry entry;
    entry.uid = sanitize_field(uid);
    entry.type = type;
    entry.key = sanitize_field(key);
    entry.display = sanitize_field(display);

    if (idx >= 0) {
        s_bindings[idx] = entry;
    } else {
        s_bindings.push_back(entry);
    }

    LOGI("[NFC_BIND] update uid=%s type=%s key=%s display=%s", 
         entry.uid.c_str(), 
         nfc_binding_type_to_cstr(entry.type), 
         entry.key.c_str(), 
         entry.display.c_str());

    return true;
}

bool nfc_binding_remove(const String& uid)
{
    int idx = nfc_binding_find_index(uid);
    if (idx < 0) return false;

    s_bindings.erase(s_bindings.begin() + idx);
    return true;
}

bool nfc_binding_load(const char* path)
{
    nfc_binding_clear();

    StorageSdLockGuard sd_lock(1000);
    if (!sd_lock) {
        LOGI("[NFC_BIND] load lock timeout: %s", path);
        return false;
    }

    File32 f = sd.open(path, FILE_READ);
    if (!f) {
        LOGI("[NFC_BIND] no map file: %s", path);
        return false;
    }

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();

        if (line.isEmpty()) continue;
        if (line.startsWith("#")) continue;

        String uid, type_s, key, display;

        // 新格式：UID|TYPE|KEY|DISPLAY
        if (line.indexOf('|') >= 0) {
            if (!split4(line, uid, type_s, key, display)) {
                LOGI("[NFC_BIND] skip bad line: %s", line.c_str());
                continue;
            }

            NfcBindType type = nfc_binding_type_from_str(type_s);
            if (type == NFC_BIND_UNKNOWN) {
                LOGI("[NFC_BIND] skip unknown type: %s", line.c_str());
                continue;
            }

            if (!nfc_binding_set(uid, type, key, display)) {
                LOGI("[NFC_BIND] skip set failed: %s", line.c_str());
                continue;
            }
        }
        // 旧格式兼容：UID=PATH
        else {
            String old_uid, old_path;
            if (!split_old_uid_path(line, old_uid, old_path)) {
                LOGI("[NFC_BIND] skip legacy bad line: %s", line.c_str());
                continue;
            }

            String legacy_display = basename_no_ext(old_path);
            if (!nfc_binding_set(old_uid, NFC_BIND_TRACK, old_path, legacy_display)) {
                LOGI("[NFC_BIND] skip legacy set failed: %s", line.c_str());
                continue;
            }
        }
    }

    f.close();

    LOGI("[NFC_BIND] loaded %d entries from %s", (int)s_bindings.size(), path);
    return true;
}

bool nfc_binding_save(const char* path)
{
    ensure_capacity_once();

    StorageSdLockGuard sd_lock(1000);
    if (!sd_lock) {
        LOGI("[NFC_BIND] save lock timeout: %s", path);
        return false;
    }

    File32 f = sd.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        LOGI("[NFC_BIND] save open failed: %s", path);
        return false;
    }

    f.seek(0);
    f.truncate(0);

    f.println("# NFC map v2");
    f.println("# UID|TYPE|KEY|DISPLAY");

    for (const auto& e : s_bindings) {
        f.print(e.uid);
        f.print("|");
        f.print(nfc_binding_type_to_cstr(e.type));
        f.print("|");
        f.print(e.key);
        f.print("|");
        f.println(e.display);
    }

    f.flush();
    f.close();

    LOGI("[NFC_BIND] saved %d entries to %s", (int)s_bindings.size(), path);
    return true;
}