#include "nfc/nfc_binding.h"
#include "utils/log.h"
#include "storage/storage_io.h"
#include <SdFat.h>

#include <vector>

// 外部声明全局 SD 对象（定义在 storage.cpp）
extern SdFat sd;

static std::vector<NfcBindingEntry> s_bindings;
// dirty=true 表示当前内存绑定表比 TF 卡上的 /System/nfc_map.txt 更新。
// 刷卡绑定时只改这里，避免播放中写 TF；关机时再统一 flush。
static bool s_bindings_dirty = false;
// true 表示正在从 TF 卡加载绑定表。加载期间复用 nfc_binding_set()，
// 但不应该逐条输出“update”，也不应该把内存表标成脏数据。
static bool s_loading_from_file = false;

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
    s_bindings_dirty = false;
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
        LOGW("[NFC绑定] 设置 失败: 为空 uid/key");
        return false;
    }

    if (type == NFC_BIND_UNKNOWN) {
        LOGW("[NFC绑定] 设置 失败: 未知 类型");
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

    if (!s_loading_from_file) {
        // NFC 绑定确认时只更新内存表并标记 dirty，不在播放中立即写 TF。
        // 统一在关机流程停音频后再 flush 到 /System/nfc_map.txt。
        s_bindings_dirty = true;

        LOGI("[NFC绑定] 更新 uid=%s 类型=%s key=%s 显示=%s",
             entry.uid.c_str(),
             nfc_binding_type_to_cstr(entry.type),
             entry.key.c_str(),
             entry.display.c_str());
    } else {
        LOGD("[NFC绑定] 加载 entry uid=%s 类型=%s",
             entry.uid.c_str(),
             nfc_binding_type_to_cstr(entry.type));
    }

    return true;
}

bool nfc_binding_remove(const String& uid)
{
    int idx = nfc_binding_find_index(uid);
    if (idx < 0) return false;

    s_bindings.erase(s_bindings.begin() + idx);
    s_bindings_dirty = true;
    return true;
}

bool nfc_binding_is_dirty()
{
    return s_bindings_dirty;
}

bool nfc_binding_flush_if_dirty(const char* path)
{
    if (!s_bindings_dirty) {
        LOGD("[NFC绑定] flush 已跳过: no 脏数据 binding");
        return true;
    }

    LOGD("[NFC绑定] 正在把脏绑定表写回 TF：%s", path);
    return nfc_binding_save(path);
}

bool nfc_binding_load(const char* path)
{
    nfc_binding_clear();

    StorageSdLockGuard sd_lock(1000);
    if (!sd_lock) {
        LOGW("[NFC绑定] 加载 锁 超时: %s", path);
        return false;
    }

    File32 f = sd.open(path, FILE_READ);
    if (!f) {
        LOGD("[NFC绑定] 没有映射表文件：%s", path);
        return false;
    }

    s_loading_from_file = true;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();

        if (line.isEmpty()) continue;
        if (line.startsWith("#")) continue;

        // 新格式：UID|TYPE|KEY|DISPLAY
        String uid, type_s, key, display;
        if (!split4(line, uid, type_s, key, display)) {
            LOGW("[NFC绑定] 跳过异常行：%s", line.c_str());
            continue;
        }

        NfcBindType type = nfc_binding_type_from_str(type_s);
        if (type == NFC_BIND_UNKNOWN) {
            LOGW("[NFC绑定] 跳过 未知 类型: %s", line.c_str());
            continue;
        }

        if (!nfc_binding_set(uid, type, key, display)) {
            LOGW("[NFC绑定] 跳过 设置 失败: %s", line.c_str());
            continue;
        }
    }

    s_loading_from_file = false;

    f.close();

    // load 过程中会复用 nfc_binding_set()，它会标 dirty；
    // 文件加载完成后，内存表和 TF 文件一致，需要清掉 dirty。
    s_bindings_dirty = false;

    LOGI("[NFC绑定] 已从 %s 加载 %d 条绑定", path, (int)s_bindings.size());
    return true;
}

bool nfc_binding_save(const char* path)
{
    ensure_capacity_once();

    StorageSdLockGuard sd_lock(1000);
    if (!sd_lock) {
        LOGW("[NFC绑定] 保存 锁 超时: %s", path);
        return false;
    }

    File32 f = sd.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        LOGE("[NFC绑定] 保存 打开失败：%s", path);
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

    s_bindings_dirty = false;

    LOGI("[NFC绑定] 已保存 %d 条绑定到 %s", (int)s_bindings.size(), path);
    return true;
}

