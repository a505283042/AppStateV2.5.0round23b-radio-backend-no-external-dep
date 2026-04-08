# ESP32-S3 项目“只保留最新方式”收口修改指南

目标：不保留兼容层，不做过渡接口，直接让工程只认当前主线。

## 修改顺序

建议按下面顺序改，风险最低：

1. **先改接口命名**：把 `legacy` / `compat` 相关 API 全部改成正式主线名。
2. **再删旧数据导入**：删掉旧 NVS key、旧 SD 配置导入、旧 NFC 文本格式。
3. **最后删旧辅助接口和文案**：删字符串歌词旧接口、重复日志头、README 旧说明。

---

## 一、V3 catalog 运行时展开接口：去掉 legacy 命名

### 1）`include/storage/storage_view_v3.h`

把：

```cpp
bool storage_fill_legacy_trackinfo_from_v3(...)
```

改成：

```cpp
bool storage_fill_trackinfo_from_v3(...)
```

同时把注释从“兼容层 TrackInfo”改成“运行时 TrackInfo”。

### 2）`src/storage/storage_view_v3.cpp`

把函数定义名同步改掉：

```cpp
bool storage_fill_trackinfo_from_v3(...)
```

函数体逻辑不用动。

### 3）`include/storage/storage_catalog_v3.h`

把：

```cpp
bool storage_catalog_v3_get_legacy_trackinfo(...)
```

改成：

```cpp
bool storage_catalog_v3_get_trackinfo(...)
```

### 4）`src/storage/storage_catalog_v3.cpp`

同步改定义与调用：

```cpp
bool storage_catalog_v3_get_trackinfo(...)
{
    ...
    return storage_fill_trackinfo_from_v3(...);
}
```

### 5）调用点一起改

#### `src/player_state.cpp`

把：

```cpp
if (!storage_catalog_v3_get_legacy_trackinfo(idx, t, "/Music")) {
    LOGE("[PLAYER] expand legacy trackinfo failed, idx=%u", (unsigned)idx);
```

改成：

```cpp
if (!storage_catalog_v3_get_trackinfo(idx, t, "/Music")) {
    LOGE("[PLAYER] expand trackinfo failed, idx=%u", (unsigned)idx);
```

#### `src/player_playlist.cpp`

把：

```cpp
storage_catalog_v3_get_legacy_trackinfo(...)
```

全部替换为：

```cpp
storage_catalog_v3_get_trackinfo(...)
```

---

## 二、播放器快照：只保留 blob，不再读旧 NVS key

文件：`src/player_snapshot.cpp`

### 删掉这几个东西

1. 删：

```cpp
static const char* kPrefsInit = "init";
```

2. 整个删掉函数：

```cpp
static bool snapshot_read_legacy_keys(...)
```

3. 在 `player_snapshot_load_pending_from_nvs()` 里，删掉这段 fallback：

```cpp
} else if (snapshot_read_legacy_keys(pref, s_pending)) {
    ok = true;
    snapshot_log_loaded("pending loaded from legacy NVS keys", s_pending);
}
```

### 保留后的逻辑

只认：

```cpp
snapshot_read_blob(pref, s_pending)
```

也就是：
- 读到 blob 就恢复
- 读不到就当没有快照
- 不再兜底旧版 key-value

### 这样改的意义

- 播放恢复逻辑会更单纯
- NVS 结构只有一套
- 后面不会再纠结“到底当前生效的是 blob 还是旧 key”

---

## 三、网页设置：只保留 NVS，不再导入旧 SD 配置

### 1）`include/web/web_settings.h`

把注释从：

```cpp
启动时优先从 NVS 加载网页运行设置；若不存在则兼容导入旧版 SD 配置；都没有则使用默认值。
```

改成：

```cpp
启动时从 NVS 加载网页运行设置；没有则使用默认值。
```

### 2）`src/web/web_settings.cpp`

#### 先删这些

- `#include <SdFat.h>`
- `#include "storage/storage_io.h"`
- `#include "web/web_config.h"`
- `extern SdFat sd;`
- `static const char* kPrefsInit = "init";`
- `trim_copy(...)`
- `parse_bool(...)`
- `parse_refresh_preset(...)`
- `parse_lyric_sync_mode(...)`
- `load_legacy_sd_settings(...)`

#### 再改 `web_settings_load()`

不要再判断 `init` 标记，也不要再导入 SD 文件。

建议直接改成：

```cpp
bool web_settings_load() {
  s_cfg = WebRuntimeSettings{};

  Preferences pref;
  if (!pref.begin(kPrefsNs, true)) {
    LOGW("[WEB] settings load failed: open NVS namespace");
    LOGI("[WEB] settings use defaults");
    return false;
  }

  s_cfg.refresh_preset = (WebRefreshPreset)pref.getUChar("refresh", (uint8_t)s_cfg.refresh_preset);
  s_cfg.lyric_sync_mode = (WebLyricSyncMode)pref.getUChar("lyric", (uint8_t)s_cfg.lyric_sync_mode);
  s_cfg.show_next_lyric = pref.getBool("show_next", s_cfg.show_next_lyric);
  s_cfg.show_cover = pref.getBool("show_cover", s_cfg.show_cover);
  s_cfg.web_cover_spin = pref.getBool("cover_spin", s_cfg.web_cover_spin);
  pref.end();

  LOGI("[WEB] settings loaded from NVS/defaults: refresh=%s lyric=%s show_next=%d show_cover=%d cover_spin=%d",
       web_refresh_preset_key(s_cfg.refresh_preset),
       web_lyric_sync_mode_key(s_cfg.lyric_sync_mode),
       (int)s_cfg.show_next_lyric,
       (int)s_cfg.show_cover,
       (int)s_cfg.web_cover_spin);
  return true;
}
```

#### 再改 `web_settings_save()`

删掉：

```cpp
pref.putBool(kPrefsInit, true)
```

只写实际配置字段。

### 3）网页说明文案也要同步

文件：`src/web/web_page.h`

把：

```html
设置会保存到设备内部配置区（更稳定），旧版 SD 配置会自动兼容导入
```

改成：

```html
设置会保存到设备内部配置区（更稳定）
```

---

## 四、NFC 绑定：只认新格式 `UID|TYPE|KEY|DISPLAY`

文件：`src/nfc/nfc_binding.cpp`

### 删除两个旧辅助函数

删：

```cpp
static bool split_old_uid_path(...)
static String basename_no_ext(...)
```

### 修改加载逻辑

当前逻辑是：
- 有 `|` 就按新格式
- 没有 `|` 就按旧 `UID=PATH`

你要改成：**全部只按新格式**。

也就是把这段：

```cpp
if (line.indexOf('|') >= 0) {
   ...
} else {
   ... old format ...
}
```

改成：

```cpp
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
```

### 这样改以后要注意

旧的 `/System/nfc_map.txt` 如果还是 `UID=PATH`，启动就会被跳过。

所以你要么：
- 手动把文件升级成新格式
- 要么在这次版本发布说明里明确写“需要重新生成 NFC 映射文件”

---

## 五、歌词：删掉 String 版本旧接口，只保留 owned buffer 方式

### 1）`include/lyrics/lyrics.h`

删掉：

```cpp
bool parse(const String& content);
bool loadFromText(const String& content);
```

只保留：

```cpp
bool parseOwnedBuffer(char* content, size_t len);
bool loadFromOwnedTextBuffer(char* content, size_t len);
```

### 2）`src/lyrics/lyrics.cpp`

删掉两个定义：

```cpp
bool LyricsParser::parse(const String& content)
bool LyricsDisplay::loadFromText(const String& content)
```

### 为什么这一步值得做

当前工程里实际调用已经走：

```cpp
g_lyricsDisplay.loadFromOwnedTextBuffer(...)
```

所以保留 `String -> 再复制一份 buffer` 这条路已经没意义了，只会让接口层看起来更乱。

---

## 六、日志体系：删掉重复日志头

文件：`src/ui/ui_cover_mem.cpp`

把：

```cpp
#include "utils/utils_log.h"
```

改成：

```cpp
#include "utils/log.h"
```

然后直接删除文件：

```text
include/utils/utils_log.h
```

这样整个项目日志头只保留一套。

---

## 七、注释和文案同步收口

这些不是功能改动，但强烈建议同轮做掉，不然后面看代码还是像“半兼容态”。

### 1）`include/storage/storage_types_v3.h`

把：

```cpp
/* 兼容层：运行时展开后的曲目信息 */
```

改成：

```cpp
/* 运行时展开后的曲目信息 */
```

### 2）`include/ui/ui_internal.h`

把：

```cpp
/* 旧页面/兼容壳 */
```

改成：

```cpp
/* UI 内部页面接口 */
```

### 3）README

把下面几类描述都删掉或改掉：
- 旧 NVS key 恢复
- 旧 NFC `UID=PATH` 导入
- V3 到旧 `TrackInfo` 桥接
- 旧版 SD `web_settings.conf` 自动导入

README 不要再写“当前仍保留兼容层”，直接写“当前只保留最新主线实现”。

---

## 八、这轮不要顺手大改的点

这几个地方我建议 **先别一起动**，免得一轮改太大：

### 1）`player_recover_find_track_idx_by_path()` 的路径归一化

它现在还保留“原样路径”和“`/Music/xxx -> xxx` 归一化”双查。

这块虽然也带一点历史味道，但它实际承担的是“路径容错”，不是单纯旧格式迁移。先别跟前面几项绑一起砍。

等你把快照和 NFC 文件格式都统一稳定后，再决定是否只保留一种路径存储格式。

### 2）`TrackInfo` 结构体本身

虽然它最早有“兼容层”味道，但现在它已经是运行时统一数据结构了。

这轮没必要把它拆成 `RuntimeTrackInfo` 再全工程重命名，收益不大，改动太广。

---

## 九、推荐你的实操顺序

### 第一批：今天就能改

1. `storage_*` 的 `legacy` 命名全删
2. `player_state.cpp` / `player_playlist.cpp` 调用点改完
3. `ui_cover_mem.cpp` 切到 `utils/log.h`
4. 删除 `include/utils/utils_log.h`

### 第二批：同一轮一起改

1. `player_snapshot.cpp` 删旧 NVS key fallback
2. `web_settings.cpp` 删 SD 导入 fallback
3. `nfc_binding.cpp` 删旧 `UID=PATH`

### 第三批：收尾

1. `lyrics` 旧 String 接口删掉
2. README / 注释 / 页面文案同步更新

---

## 十、改完后你应该重点自测什么

### 1）启动恢复

看这几个场景：
- 正常关机后重启，是否还能恢复播放状态
- 没有任何旧 NVS key 时，是否正常
- 没有 snapshot blob 时，是否正常从默认状态启动

### 2）网页设置

看：
- 设置修改后是否能保存到 NVS
- 重启后是否还能读出
- 没有任何设置时，是否直接走默认值

### 3）NFC

看：
- 新格式 `UID|TYPE|KEY|DISPLAY` 是否正常加载
- 如果文件里混入旧格式，是否只是被跳过，而不是把整个表读坏

### 4）歌词

看：
- 当前曲目补齐歌词是否正常
- 切歌时 owned buffer 是否释放正确
- 不存在歌词文件时是否稳定

---

## 十一、我对这轮收口的判断

如果你只想做“最值的一轮”，我建议你至少完成下面 6 项：

1. `storage_catalog_v3_get_legacy_trackinfo` → `storage_catalog_v3_get_trackinfo`
2. `storage_fill_legacy_trackinfo_from_v3` → `storage_fill_trackinfo_from_v3`
3. 删 `player_snapshot.cpp` 的旧 key fallback
4. 删 `web_settings.cpp` 的旧 SD 导入 fallback
5. 删 `nfc_binding.cpp` 的旧 `UID=PATH` fallback
6. 删 `utils/utils_log.h`

做完这 6 项，工程就会从“历史过渡态”明显变成“当前主线态”。
