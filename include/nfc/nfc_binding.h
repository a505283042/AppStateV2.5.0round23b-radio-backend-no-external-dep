#pragma once
#include <Arduino.h>

/**
 * @brief NFC UID 与播放目标的绑定表模块。
 *
 * 当前主链路里最常用的是：
 * - nfc_binding_find()：刷到卡后按 UID 查整条绑定记录
 * - nfc_binding_set()：写卡时新增或覆盖绑定
 *
 * 其余 get/remove/exists/get_display 更偏向“管理页 / 调试页 / 遍历场景”辅助接口。
 */

enum NfcBindType {
    NFC_BIND_TRACK = 0,
    NFC_BIND_ARTIST,
    NFC_BIND_ALBUM,
    NFC_BIND_UNKNOWN
};

/** 一条 UID -> 播放目标 的绑定记录。 */
struct NfcBindingEntry {
    String uid;      // 例如 09:76:10:05
    NfcBindType type;
    String key;      // track: 完整路径；artist: 歌手名；album: 专辑名
    String display;  // 给 UI / 日志显示的名称
};

/** 清空当前内存中的绑定表。 */
void nfc_binding_clear();
/** 当前绑定表条目数。 */
int  nfc_binding_count();

/** 从文件载入全部绑定。 */
bool nfc_binding_load(const char* path);
/** 把当前绑定表保存到文件。 */
bool nfc_binding_save(const char* path);

/** 按 UID 查整条绑定记录。当前 NFC admin / 播放分发主流程主要走这个接口。 */
bool nfc_binding_find(const String& uid, NfcBindingEntry& out);
/** 按 UID 查条目下标。 */
int  nfc_binding_find_index(const String& uid);

/** 新增或覆盖一条绑定。 */
bool nfc_binding_set(const String& uid,
                     NfcBindType type,
                     const String& key,
                     const String& display);

/** 删除指定 UID 的绑定。当前主链路未直接使用，更偏管理页能力。 */
bool nfc_binding_remove(const String& uid);

/** 按索引读取一条绑定，主要用于遍历/管理页。 */
bool nfc_binding_get(int index, NfcBindingEntry& out);

/** 类型到字符串。用于文件读写 / 日志显示。 */
const char* nfc_binding_type_to_cstr(NfcBindType type);
/** 字符串转类型。 */
NfcBindType nfc_binding_type_from_str(const String& s);
