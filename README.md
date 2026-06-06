# ESP32-S3 圆屏音乐播放器

一个基于 **ESP32-S3 + PSRAM** 的本地音乐 / 网络电台播放器，面向“圆屏桌面播放器”场景设计，支持：

- 本地 **MP3 / FLAC** 播放
- **HTTP MP3 网络电台** 播放
- **NAS / HTTP MP3 网络歌曲** 播放
- **圆形 TFT 双视图 UI**（旋转封面 / 信息视图）
- **歌词显示** 与下一句提示
- **NFC 绑定播放**（歌曲 / 歌手 / 专辑）
- **Web 控制页**（状态查看、切歌、音量、模式、电台、NAS 音乐、封面、设置）
- 基于 **V3 音乐索引** 的快速启动与重扫
- 基于 **NVS** 的播放状态与网页设置持久化

> 本 README 按当前主线整理：
> - 网络电台与 NAS/HTTP MP3 网络歌曲均复用 **Audio Tools URLStream -> unified MP3 core (`audio_mp3.cpp`)**
> - NAS 歌曲列表使用 offset 索引按需读取，避免全量加载长 URL 列表

---

## 1. 当前能力概览

### 已完成

- 本地文件播放：`MP3`、`FLAC`
- 电台播放：`HTTP MP3 stream`
- NAS / HTTP 网络歌曲播放：
  - 支持 `/System/net_music_base.txt` + `/System/net_music.txt`
  - 支持 URL 编码后的 HTTP MP3 文件直链
  - 支持 1339+ 首网络歌曲 offset 索引
  - 不全量加载歌曲 URL，按 index seek 读取
  - 支持设备端 NAS 歌曲列表
  - 支持 Web NAS 分页页
  - 支持 NAS 歌曲顺序 / 随机播放
  - 支持 NAS 歌曲播放结束自动下一首
  - 对 HTTP 文件 EOF 不明确的情况增加 EOF watchdog 兜底
- UI 双视图：
  - 旋转封面视图
  - 信息详情视图（标题 / 歌手 / 专辑 / 进度 / 歌词）
- 歌词：LRC 解析、当前句 / 下一句 / 再下一句摘要
- 封面：
  - MP3 内嵌 APIC
  - FLAC picture block
  - 外部封面兜底
  - `/System/default_cover.jpg` 默认封面
- 播放模式：
  - 全部顺序 / 全部随机
  - 歌手顺序 / 歌手随机
  - 专辑顺序 / 专辑随机
- 列表选择模式：
  - 歌手列表
  - 专辑列表
  - 本地歌曲列表
  - 网络电台列表
  - NAS 歌曲分页列表
- NFC 绑定：
  - `track`
  - `artist`
  - `album`
- Web 控制：
  - 当前状态
  - 切歌 / 播放暂停 / 模式切换 / 音量
  - 歌手 / 专辑 / 电台 / NAS 音乐页面
  - 当前封面获取
  - 设置页
  - 扫描 / 保存状态
- 启动恢复：
  - NVS blob 快照恢复播放状态
  - **按 TF 卡区分 snapshot**（不同卡保存独立播放状态）
- Wi‑Fi：
  - 优先 STA 连 `wifi.conf`
  - 失败自动回退 AP 热点
  - **双击 VOL- 手动开关 WiFi**
- **无 Card Detect 引脚 TF 卡热插拔**：
  - 开机无卡可正常进入系统
  - 开机后插卡自动挂载 TF 卡
  - 本地播放中拔卡不连续跳歌、不 WDT
  - 网络电台播放中拔卡不主动停止电台
- 运行时监控：
  - heap / internal / dma / psram
  - 任务栈高水位

### 当前主线不包含

- `m3u8 / HLS` 播放
- SMB / NFS 直连目录浏览
- NAS 目录自动扫描
- 网络 FLAC 文件播放
- OTA / 蓝牙 / 触摸交互
- NAS 歌曲时长自动计算
- NAS 歌曲网络封面 / 网络歌词

---

## 2. 硬件组成

### 主控

- ESP32-S3（带 PSRAM）
- 当前 PlatformIO 配置按 **16MB Flash + OPI PSRAM** 使用

### 显示

- GC9A01 圆形 TFT，240x240
- 图形库：`LovyanGFX`

### 音频

- I2S DAC：如 `PCM5102A`

### 存储

- TF / SD 卡（`SdFat`）

### NFC

- RC522（SPI）

### 按键

- 6 个独立按键：模式 / 播放 / 上一首 / 下一首 / 音量减 / 音量加

---

## 3. 默认引脚定义

### SD SPI

| 功能 | GPIO |
|---|---:|
| MOSI | 11 |
| MISO | 13 |
| SCK | 12 |
| CS | 10 |

### UI SPI（TFT + RC522）

| 功能 | GPIO |
|---|---:|
| MOSI | 14 |
| MISO | 47 |
| SCK | 21 |
| TFT CS | 42 |
| TFT DC | 41 |
| TFT RST | 40 |
| RC522 CS | 38 |
| RC522 RST | 39 |
| RC522 IRQ | 45 |

### I2S

| 功能 | GPIO |
|---|---:|
| BCLK | 4 |
| LRCK | 5 |
| DOUT | 6 |

### 按键

| 功能 | GPIO |
|---|---:|
| MODE | 15 |
| PLAY | 16 |
| PREV | 17 |
| NEXT | 18 |
| VOL- | 8 |
| VOL+ | 3 |

> 实际板级定义在：`include/board/board_pins.h` 与 `include/keys/keys_pins.h`

---

## 4. 软件架构

## 总体分层

```text
App State
├─ Boot
├─ Player
└─ NFC Admin

Player Core
├─ player_state / player_control / player_playlist
├─ player_assets（歌词/封面/总时长补齐与预取）
├─ player_snapshot（NVS 快照）
├─ player_source（本地 / 网络电台 / NAS 网络歌曲来源摘要）
└─ net_music_catalog（NAS 歌曲 base url、列表、offset 索引）

Audio
├─ audio_service（独立任务，命令队列）
├─ audio.cpp（本地文件播放入口）
├─ audio_flac.cpp
├─ audio_mp3.cpp（统一 MP3 核心）
├─ audio_mp3_source_file.cpp
├─ audio_mp3_source_audiotools.cpp
└─ audio_radio_backend.cpp

Storage
├─ storage_catalog_v3
├─ storage_index_v3
├─ storage_scan_v3
├─ storage_builder_v3
├─ storage_groups_v3
└─ net_music_catalog（读取 /System/net_music*.txt，建立行偏移 offset 索引）

UI / Web / NFC
├─ ui_*（圆屏渲染、封面缓存、列表页）
├─ web_server / web_snapshot / web_settings
└─ nfc / nfc_binding / nfc_admin_state
```

## 当前音频主线

### 本地文件

```text
player -> audio_service_play(...) -> audio.cpp
     -> MP3 / FLAC 解码 -> I2S
```

### 网络电台

```text
player -> audio_radio_backend.cpp
       -> audio_service_play_stream_mp3(...)
       -> audio_mp3_start_url(...)
       -> Audio Tools URLStream
       -> audio_mp3.cpp unified MP3 core
       -> I2S
```

### NAS / HTTP 网络歌曲

```text
player / Web / list_select
       -> player_play_net_track_index(idx)
       -> net_music_catalog_get(idx)
       -> base_url + encoded_path
       -> audio_service_play_stream_mp3(...)
       -> audio_mp3_start_url(...)
       -> Audio Tools URLStream
       -> audio_mp3.cpp unified MP3 core
       -> I2S
```

NAS 网络歌曲与网络电台一样复用 URLStream 和统一 MP3 核心，但状态上使用独立的 `PlayerSourceType::NET_TRACK`。
网络歌曲是有限长度 HTTP MP3 文件，播放结束后由 `player_control_try_auto_next()` 触发下一首。由于部分 HTTP 文件结束时底层流不一定明确返回 EOF，当前增加了 NET_TRACK 专用 EOF watchdog：当播放时间长时间不增长时，主动停止当前流并切换下一首。

这意味着当前项目已经实现了：

- **文件 MP3**、**网络电台 MP3** 与 **NAS HTTP MP3 文件** 共用统一 MP3 解码主线
- "来源"和"解码器"已经分层：本地文件、网络电台、NAS 网络歌曲使用不同 source 状态，但最终收口到统一 MP3 core
- NAS 歌曲播放已验证可通过 HTTP Web API / NAS 静态文件服务实现，不依赖 SMB / NFS

---

## 5. 启动流程

系统启动大致顺序如下：

1. 初始化串口与 SPI 总线
2. 初始化 SD / TF 卡
3. 如果 TF 卡挂载成功：
   - 读取 TF 卡 CID
   - 生成当前卡的 snapshot key，例如 `snap_BE61B111`
   - 读取 NFC 绑定表 `/System/nfc_map.txt`
   - 加载或重建 V3 音乐索引 `/System/music_index_v3.bin`
   - 加载电台列表 `/System/radio_list.txt`
   - 加载 NAS 歌曲 base url `/System/net_music_base.txt`
   - 扫描 NAS 歌曲列表 `/System/net_music.txt`，建立行偏移 offset 索引
   - 读取 WiFi 配置 `/System/config/wifi.conf`
4. 如果开机无卡：
   - 系统仍继续启动
   - 本地曲库为空
   - Web 进入 AP fallback
   - 后续插卡时自动重新加载 TF 卡资源
5. 初始化固定封面缓冲区
6. 初始化 UI
7. 启动 `audio_service` 音频任务
8. 启动运行时监控任务
9. 初始化 NFC
10. 从当前卡对应的 NVS key 读取待恢复快照
11. 启动 Web 服务器
12. 进入 `STATE_PLAYER`

无卡启动不是异常状态。此时出现类似日志属于预期行为：

```text
[STORAGE] SdFat mount FAILED
[BOOT] no TF card, start without local library
[BOOT] skip local catalog: storage not ready
```

NAS 歌曲索引加载成功时会出现：

```text
[NETMUSIC] catalog loaded tracks=1339 offsets=1339 base=http://192.168.1.105:8080/music/ path=/System/net_music.txt
[BOOT] Net music catalog loaded: 1339 tracks
```

这里的 offset 索引只保存每一首歌曲所在行的文件偏移，不保存完整标题和 URL。
播放或分页显示时，再按 index seek 到对应行读取。

---

## 6. SD 卡目录约定

推荐最小目录：

```text
/Music/
    xxx.mp3
    xxx.flac

/System/
    music_index_v3.bin
    radio_list.txt
    net_music_base.txt
    net_music.txt
    nfc_map.txt
    default_cover.jpg
    /config/
        wifi.conf
```

### 音乐目录

- 默认扫描根目录：`/Music`
- 启动时优先加载 `/System/music_index_v3.bin`
- 若索引不存在或加载失败，会自动重扫 `/Music` 并重建索引

### 电台列表

文件：`/System/radio_list.txt`

支持格式：

```text
name|url
name|url|format
name|url|format|region
name|url|format|region|logo
```

示例：

```text
怀集音乐之声|http://lhttp.qingting.fm/live/4804/64k.mp3|mp3|广东
央广音乐之声|http://ngcdn003.cnr.cn/live/yyzs/index.m3u8|hls|全国
```

> 说明：当前项目的**电台实际主线是 HTTP MP3**。即使列表文件可以记 `format` 字段，`m3u8/hls` 仍属于后续扩展方向，不是当前稳定能力。

### NAS / HTTP 网络歌曲列表

Base URL 文件：

```text
/System/net_music_base.txt
```

内容只写一行，例如：

```text
http://192.168.1.105:8080/music/
```

歌曲列表文件：

```text
/System/net_music.txt
```

格式：

```text
title|encoded_path|format|artist|album
```

示例：

```text
不修|%E6%88%BF%E7%94%B0%E7%AB%8B%20-%20%E4%B8%8D%E4%BF%AE.mp3|mp3|房田立|NAS
Kageokuri|5u5h1%20-%20Kageokuri.mp3|mp3|5u5h1|NAS
```

说明：

* `encoded_path` 必须是 URL 编码后的相对路径
* 中文、空格、特殊符号不能直接写进 URL
* 播放时实际 URL 为：`base_url + encoded_path`
* 当前稳定支持 `mp3`
* 当前不支持网络 FLAC
* 当前不从 ESP32 直接扫描 NAS 目录
* NAS 侧建议通过 Web Station、nginx、HTTP 静态文件服务暴露音乐目录

Windows PowerShell 生成 `net_music.txt` 示例：

```powershell
$root = "\\192.168.1.105\麦田广告\Music\音乐"
$out = ".\net_music.txt"

Get-ChildItem $root -File -Recurse -Include *.mp3 | ForEach-Object {
    $relative = $_.FullName.Substring($root.Length).TrimStart('\')
    $relativeUrl = $relative -replace "\\", "/"

    $encodedParts = $relativeUrl.Split("/") | ForEach-Object {
        [System.Uri]::EscapeDataString($_)
    }
    $encodedPath = $encodedParts -join "/"

    $title = [System.IO.Path]::GetFileNameWithoutExtension($_.Name)

    "$title|$encodedPath|mp3|NAS|NAS"
} | Set-Content -Encoding UTF8 $out
```

### NFC 绑定表

文件：`/System/nfc_map.txt`

当前新格式：

```text
UID|TYPE|KEY|DISPLAY
```

示例：

```text
09:76:10:05|track|/Music/周杰伦 - 忍者.flac|忍者 - 周杰伦
F7:8C:64:06|album|王菲菲 - 那些年|王菲菲 - 那些年
```

系统会自动按 `track` 处理旧格式。

### Wi‑Fi 配置

文件：`/System/config/wifi.conf`

示例：

```ini
hostname=esp32s3-player

ssid=MyWiFi
password=12345678

ssid=BackupWiFi
password=87654321
```

行为：

- 启动时按顺序尝试连接配置文件里的 Wi‑Fi
- 连接失败后自动开启 AP：
  - SSID: `ESP32S3-Player`
  - Password: `12345678`

### 默认封面

文件：

```text
/System/default_cover.jpg
```

用途：

- 歌曲没有内嵌封面时使用
- 封面读取失败时使用
- 避免播放器因封面资源失败一直停留在“加载中”占位页

建议：

- JPG 格式
- 240×240
- 文件大小小于 100KB

---

## 7. Web 控制

### 入口页面

- `/`：主控页
- `/artists`
- `/albums`
- `/radios`
- `/netmusic`
- `/settings`

### 主要 API

- `GET /api/status`
- `GET /api/artists`
- `GET /api/albums`
- `GET /api/radios`
- `GET /api/netmusic?offset=0&limit=20`
- `GET /api/netmusic?offset=0&limit=20&detail=1`
- `GET /api/netmusic/play?idx=0`
- `POST /api/netmusic/play?idx=0`
- `GET /api/artist/detail`
- `GET /api/album/detail`
- `GET /api/settings`
- `POST /api/settings`
- `GET /api/cover/current`
- `POST /api/track/play`
- `POST /api/artist/play`
- `POST /api/album/play`
- `POST /api/radio/play`
- `POST /api/radio/stop`
- `POST /api/playpause`
- `POST /api/next`
- `POST /api/prev`
- `POST /api/mode/toggle`
- `POST /api/mode/category`
- `POST /api/view/toggle`
- `POST /api/volume`
- `POST /api/state/save`
- `POST /api/scan`

### NAS 音乐页

页面：

```text
/netmusic
```

特点：

* Web 端分页读取 NAS 歌曲列表
* 默认每页 20 首，最大 50 首
* 默认不返回 encoded path，减少 JSON 大小
* 需要调试路径时可加 `detail=1`
* 点击播放后调用 `/api/netmusic/play?idx=...`
* 当前播放的 NAS 歌曲会在状态中显示为 `source_type=net_track`

状态接口中 NAS 播放相关字段：

```json
{
  "source_type": "net_track",
  "net_track_active": true,
  "net_track_idx": 3,
  "net_track_title": "...",
  "net_track_url": "...",
  "net_track_format": "mp3",
  "net_track_artist": "NAS",
  "net_track_album": "NAS",
  "net_track_state": "playing",
  "net_track_error": ""
}
```

### 网页设置

当前持久化在 **NVS** 中：

- 刷新档位：省流量 / 平衡 / 流畅
- 歌词同步策略：精准优先 / 平衡 / 等轮询优先
- 是否显示下一句歌词
- 是否显示封面
- 旋转视图时网页封面是否旋转

---

## 8. 按键语义

### 正常播放状态

| 按键 | 操作 | 行为 |
|---|---|---|
| MODE | 单击 | 切换小类：顺序 / 随机 |
| MODE | 双击 | 切换大类：全部 / 歌手 / 专辑 |
| MODE | 长按 | 开始重扫音乐库 |
| PLAY | 短按 | 播放 / 暂停 / 恢复 |
| PLAY | 长按 | 切换 UI 视图 |
| PREV | 短按 | 上一首 |
| PREV | 长按 | 进入 NFC 管理模式 |
| NEXT | 短按 | 下一首 |
| NEXT | 长按 | 本地歌手/专辑模式下进入对应列表；网络电台播放中进入电台列表；NAS 歌曲播放中进入 NAS 歌曲列表；其他本地全部模式下大步前进 |
| VOL- | 按住连发 | 音量减 |
| VOL- | 双击 | WiFi 开关 |
| VOL+ | 按住连发 | 音量加 |

### WiFi 开关说明

双击 `VOL-` 可临时关闭或开启 WiFi。

WiFi 关闭时：

- 停止 Web Server
- 断开 STA
- 关闭 AP 热点
- 执行 `WiFi.mode(WIFI_OFF)`
- 插拔 TF 卡不会自动重新打开 WiFi

WiFi 重新开启时：

- 重新读取 `/System/config/wifi.conf`
- 优先连接 STA
- STA 失败时进入 AP fallback

当前 WiFi 开关默认不写入 NVS，重启后默认 WiFi 开启。

### 扫描中

- 仅允许 `MODE` 触发取消扫描
- 其他按键逻辑屏蔽

### NFC 管理模式

- `MODE` / `PLAY` 交给 `nfc_admin_state` 处理

### NAS 歌曲播放状态

NAS 歌曲播放中：

| 按键 | 操作 | 行为 |
|---|---|---|
| NEXT | 短按 | NAS 下一首 |
| PREV | 短按 | NAS 上一首 |
| NEXT | 长按 | 进入 NAS 歌曲分页列表 |
| PREV | 长按 | 保持原 NFC 管理入口；当前 NAS 歌曲暂不支持 NFC 绑定，显示"无可绑定目标" |
| PLAY | 短按 | 暂停 / 恢复 |
| MODE | 短按 | 顺序 / 随机切换 |
| MODE | 双击 | 大类切换，当前 NAS 歌曲暂不支持 |

NAS 歌曲列表中：

| 按键 | 行为 |
|---|---|
| NEXT / PREV | 上下选择 |
| VOL+ / VOL- | 每次跳 5 首 |
| PLAY | 播放选中 NAS 歌曲 |
| MODE | 退出列表 |

---

## 9. 播放模式说明

项目内部的 6 个播放模式：

- `PLAY_MODE_ALL_SEQ`
- `PLAY_MODE_ALL_RND`
- `PLAY_MODE_ARTIST_SEQ`
- `PLAY_MODE_ARTIST_RND`
- `PLAY_MODE_ALBUM_SEQ`
- `PLAY_MODE_ALBUM_RND`

切换逻辑分成两层：

- **小类切换**：顺序 ↔ 随机
- **大类切换**：全部 → 歌手 → 专辑

这种拆法让模式控制更清楚，也更适合映射到 Web 与硬件按键。

### NAS 歌曲播放模式

NAS 歌曲复用当前播放模式中的"顺序 / 随机"属性：

- `*_SEQ`：按 `net_music.txt` 中的 index 顺序播放
- `*_RND`：使用 NAS 专用随机队列播放

NAS 随机播放使用"互质步长洗牌序列"：

```text
index = (shuffle_start + shuffle_pos * shuffle_step) % track_count
```

其中 `shuffle_step` 与 `track_count` 保持互质。这样一轮随机中尽量不重复，同时不需要在内存中保存完整随机列表。

行为：

* 手动选择 NAS 歌曲时，该歌曲作为新一轮随机起点
* 短按 NEXT：随机序列下一首
* 短按 PREV：随机序列上一首
* 播放结束：随机序列下一首
* 一轮播放完后重新生成随机起点和步长

---

## 10. V3 音乐索引

当前主线使用 **V3 catalog**：

- 启动优先加载 `/System/music_index_v3.bin`
- 失败则重扫 `/Music`
- 扫描结果重建并保存回 V3 索引

V3 的设计目标：

- 降低启动全盘扫描成本
- 支持更大的音乐库
- 将 `tracks / albums / artists / string_pool` 结构化存储
- 为歌手 / 专辑分组和列表页提供更稳定的基础

日志中会输出大致内存统计，例如：

- `tracks`
- `albums`
- `artists`
- `string_pool`
- `artist_groups`
- `album_groups`

---

## 11. 状态恢复

播放器状态使用 **NVS blob** 保存，主要包括：

- 音量
- 播放模式
- 当前 group
- 当前 track index
- 当前 track path
- 当前 UI view
- 用户是否处于暂停态

### 按 TF 卡区分 snapshot

当前版本会在 TF 卡 mount 成功后读取 TF 卡 CID，并生成卡专属 snapshot key，例如：

```text
snap_BE61B111
```

这样可以实现：

```text
TF卡A -> 恢复 TF卡A 的上次播放记录
TF卡B -> 恢复 TF卡B 的上次播放记录
```

保存和恢复逻辑：

- 开机有卡：挂载成功后读取当前卡对应的 snapshot
- 开机无卡：不提前恢复本地歌曲 snapshot
- 后续插卡：挂载成功后读取当前卡对应的 snapshot
- 本地播放中拔卡：拔卡处理前保存当前卡对应的 snapshot

### 网络播放源与本地 snapshot 隔离

`player_snapshot_save_to_nvs()` 只保存本地歌曲状态。

网络电台或 NAS 网络歌曲播放时：

- 网页点击“保存当前状态”不会写入本地歌曲 snapshot
- 拔卡时不会保存本地歌曲 snapshot
- 插卡后不会恢复本地歌曲 snapshot
- NAS 网络歌曲播放状态不会写入本地歌曲 snapshot
- NAS 网络歌曲当前通过 `PlayerSourceType::NET_TRACK` 维护运行时状态
- 网络电台播放状态不会被本地歌曲 UI 覆盖

如果未来需要恢复网络电台或 NAS 网络歌曲状态，应新增独立的 network source snapshot，例如保存 radio index 或 net track index、base url、volume、paused、UI view 等信息，不要复用本地歌曲 snapshot。

恢复策略：

- 启动时先读取“待恢复快照”
- 首次进入 player 后先恢复轻量状态
- 再延后恢复曲目，避免阻塞进入主界面

---

## 12. TF 卡热插拔

当前硬件没有 TF 卡 Card Detect 引脚，因此热插拔采用软件探测方案。

### 实现方式

- 无卡状态下周期性尝试 `storage_mount()`
- 有卡状态下低频执行 `storage_probe_alive()`
- 本地播放中不主动 probe，避免影响音频读取
- `AudioFile::read/open/seek` 失败时上报 `storage_report_io_error()`
- 疑似拔卡后通过 `readSector(0)` 快速确认卡是否仍存在

### 本地音乐播放中拔卡

流程：

```text
AudioFile read/open/seek 失败
    ↓
上报 storage IO error
    ↓
阻止 player auto next
    ↓
probe 确认 TF removed
    ↓
保存本地 snapshot
    ↓
停止本地音频
    ↓
清理歌词 / 封面 / 资源任务 / 曲库 / playlist / NFC
    ↓
storage_unmount()
```

预期日志：

```text
[STORAGE] IO error reported: AudioFile::read_negative
[PLAYER] auto next blocked: storage not ready or IO error pending
[SD_HOTPLUG] card removed confirmed
[APP] TF removed
[STORAGE] unmounted
```

### 网络电台播放中拔卡

流程：

```text
probe 确认 TF removed
    ↓
APP 处理 TF removed
    ↓
不保存本地歌曲 snapshot
    ↓
不主动停止网络电台
    ↓
清理本地曲库和 TF 相关资源
    ↓
storage_unmount()
```

网络电台播放中重新插卡时，只重新加载 TF 卡资源，不恢复本地歌曲 snapshot。

预期日志：

```text
[APP] TF mounted while radio active: skip local snapshot restore
```

### NAS 歌曲播放中拔卡

NAS 歌曲音频数据来自 HTTP，但播放列表和 base url 来自 TF 卡：

```text
/System/net_music_base.txt
/System/net_music.txt
```

因此 TF 卡拔出后，NAS 歌曲当前流可能还能继续播放一段时间，但后续选歌、自动下一首、列表分页都依赖 TF 卡列表文件。

当前策略：

* TF 拔出时清理 `net_music_catalog`
* 不保存本地歌曲 snapshot
* 不将 NAS 歌曲状态写入本地 snapshot
* 后续若需要更完整体验，可考虑把当前 NAS 歌曲 URL 缓存在运行时，允许当前首播完，但禁止继续下一首

TF 插回后：

* 重新加载 `/System/net_music_base.txt`
* 重新扫描 `/System/net_music.txt`
* 重建 offset 索引

### 正常失败码说明

由于无 CD 脚，系统无法提前知道卡是否真的存在。无卡状态下周期性 mount 失败属于正常现象：

```text
[STORAGE] mount (SdFat)
[STORAGE] 卡错误代码: 1
[STORAGE] SdFat mount FAILED
```

热插拔瞬间也可能出现：

```text
卡错误代码: 12
卡错误代码: 23
```

只要后续可以重新 `SdFat mount OK`，一般不属于致命问题。

### SPI 配置说明

热插拔版本中，SdFat 使用：

```cpp
SdSpiConfig cfg(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(16), &SPI_SD);
```

原因：

- 当前工程存在 AudioTask、PlayerAssetTask、loopTask 等多任务访问 SD 的情况
- `DEDICATED_SPI` 在部分场景下可能导致 SPI transaction 跨任务收尾
- 可能触发 FreeRTOS mutex assert
- `SHARED_SPI` 更适合当前多任务访问模型

每次重新 mount 前必须显式初始化 SD SPI 引脚：

```cpp
SPI_SD.begin(PIN_SPI_SD_SCK, PIN_SPI_SD_MISO, PIN_SPI_SD_MOSI, PIN_SD_CS);
```

否则 ESP32-S3 可能出现：

```text
HSPI Does not have default pins on ESP32S3
```

---

## 13. 内存设计（当前版本重点）

### 已明确放到 PSRAM 的大头

- 固定封面原图缓冲：约 `400 KB`
- 240x240 RGB565 封面相关 sprite / cache：约 `1.1 MB`
- V3 索引核心数据（tracks / albums / artists / string_pool）优先走 PSRAM

### 内部 RAM 主要压力来源

- `AudioTask / UiTask / loopTask / RuntimeMon / PlayerAssetTask` 栈
- Wi‑Fi / WebServer / TCPIP 运行期开销
- 音频热路径缓冲
- `String / vector` 造成的小块分配与碎片

### 当前原则

- **热路径**（I2S / 解码关键缓冲）优先留内部 RAM
- **大块静态资源**（封面 / 索引）优先放 PSRAM
- 未来可继续把歌词缓存、playlist 索引等往 PSRAM 推

### NAS 歌曲列表内存策略

NAS 歌曲列表不全量加载标题和 URL。  
启动时只扫描 `/System/net_music.txt`，记录每一条有效记录的文件偏移：

```text
offsets[0] = 第 1 行起始位置
offsets[1] = 第 2 行起始位置
...
```

内存估算：

```text
1339 首 * 4B ≈ 5.2KB
5000 首 * 4B ≈ 20KB
```

播放或显示某一页时，通过 `seek(offset)` 读取对应行。
这避免了 1000+ 首长 URL 直接常驻内存导致 internal heap 和 String 碎片压力上升。

---

## 13. 依赖库

当前 `platformio.ini` 中的核心依赖：

- `LovyanGFX`
- `SdFat`
- `Arduino_MFRC522v2`
- `arduino-audio-tools`

说明：

- `Audio Tools` 当前主要用于 **网络收流层**
- 本地文件播放仍走项目自己的解码 / 音频服务主线

---

## 14. 构建与烧录

### 环境

- PlatformIO
- Arduino framework
- `board = esp32-s3-devkitc-1`

### 主要配置

- Flash size: `16MB`
- Partitions: `default_16MB.csv`
- PSRAM: `enabled`
- Memory type: `qio_opi`
- Flash mode: `qio`
- Flash freq: `80MHz`
- Monitor speed: `115200`

### 常用命令

```bash
pio run
pio run -t upload
pio device monitor
```

---

## 15. 关键日志参考

### TF 卡识别

```text
[STORAGE] SdFat mount OK
[STORAGE] card hash=BE61B111 snapshot_key=snap_BE61B111
```

### 开机无卡

```text
[STORAGE] SdFat mount FAILED
[BOOT] no TF card, start without local library
```

### 插卡成功

```text
[SD_HOTPLUG] card mounted
[APP] TF mounted
[CATALOG_V3] load ok
```

### 本地播放中拔卡

```text
[STORAGE] IO error reported: AudioFile::read_negative
[PLAYER] auto next blocked: storage not ready or IO error pending
[SD_HOTPLUG] card removed confirmed
[APP] TF removed
[STORAGE] unmounted
```

### 网络电台播放中插卡

```text
[APP] TF mounted while radio active: skip local snapshot restore
```

### WiFi 开关

```text
[WEB] WiFi disabled by user
[APP] WiFi toggled: OFF
```

重新打开时：

```text
[WEB] WiFi enabled by user
[WEB] STA connected ip=...
```

启动时建议关注这些日志：

- `psramFound / FreePsram / FreeHeap`
- `[SDIO] recursive SD mutex created`
- `[BOOT] NFC bindings loaded`
- `[CATALOG_V3] load ok` 或 `native rebuild ok`
- `[NETMUSIC] catalog loaded`
- `[RADIO] catalog loaded`
- `[SNAPSHOT] pending loaded`
- `[WEB] STA connected` 或 `[WEB] AP ready`
- `[WEB] server started`
- `[MON][MEM] ...`
- `[MON][STACK] ...`

### NAS 歌曲索引加载

```text
[NETMUSIC] catalog loaded tracks=1339 offsets=1339 base=http://192.168.1.105:8080/music/ path=/System/net_music.txt
[BOOT] Net music catalog loaded: 1339 tracks
```

### NAS 歌曲播放

```text
[AUDIO] play stream mp3: http://192.168.1.105:8080/music/xxx.mp3
[MP3] start source detail name=http://192.168.1.105:8080/music/xxx.mp3 stream=1 init=0ms prefill=2ms total=2ms
[NETTRACK] PLAY idx=880 title=蔡健雅 - 被驯服的象 url=http://192.168.1.105:8080/music/xxx.mp3
```

### NAS 随机播放

```text
[NETTRACK] shuffle reset start=884 step=288 count=1339
[NETTRACK] shuffle resolve cur=1272 step=1 pos=49 -> 880
```

### NAS 自动下一首

```text
[NETTRACK] EOF watchdog triggered idx=1272 play_ms=256026 stalled=8063ms
[NETTRACK] AUTO NEXT 1272 -> 880
[NETTRACK] PLAY idx=880 title=蔡健雅 - 被驯服的象
```

### NAS 歌曲列表

```text
[LIST] 进入 NAS 歌曲列表，共 1339 首，当前 idx=880
[LIST] 选择下一项: 882/1339
[LIST] 确认 NAS 歌曲: pos=885/1339 idx=884
```

如果电台播放异常，建议重点看：

- `[RADIO] http code=...`
- `content-type=...`
- `icy-metaint=...`
- `backend=...`

---

## 16. 已知边界与注意事项

1. 当前稳定网络音频能力是 **HTTP MP3 电台** 与 **NAS/HTTP MP3 网络歌曲**。  
   `m3u8/HLS`、网络 FLAC 仍不是当前稳定主线。

2. 当前硬件没有 TF 卡 Card Detect 引脚。  
   热插拔采用软件探测，因此无卡状态下周期性 mount 失败日志属于正常现象。

3. 网络电台播放中插卡会重新加载 TF 卡资源。  
   当前会同步加载本地 music index，可能造成短暂卡顿。后续可优化为"电台播放中延迟加载本地曲库"。

4. 本地歌曲 snapshot 和网络电台 snapshot 不共用。  
   当前仅本地歌曲支持 snapshot 保存/恢复。网络电台如果需要持久化，应新增独立 radio snapshot。

5. WiFi 关闭后 Web 控制不可用。  
   需要再次双击 `VOL-` 打开 WiFi。

6. 下面这些文件/模块已下线或应视为历史残留：
   - `audio_mp3_stream_audiotools.cpp`
   - `audio_mp3_stream.h`

7. NAS 音乐当前依赖 HTTP 文件直链。  
   DSM File Station 分享链接、Synology Drive 链接、需要登录/cookie/跳转的链接不适合直接给 ESP32 播放。

8. NAS 音乐当前不直接支持 SMB / NFS。  
   推荐 NAS 侧通过 Web Station、nginx 或其他静态 HTTP 服务暴露只读音乐目录。

9. NAS 歌曲路径必须 URL 编码。  
   浏览器可以自动处理中文和空格，但 ESP32 的 URLStream 更适合直接接收已编码 URL。

10. NAS 自动下一首目前依赖 EOF watchdog 兜底。  
    部分 HTTP 文件播放结束时底层流不会明确返回 EOF，系统会在播放时间长时间不增长后切换下一首。当前默认等待约 8 秒，后续可根据稳定性再调整。

---

## 17. 后续演进建议

### 较近目标

- 网络电台播放中插卡时，延迟加载本地 `music_index_v3.bin`，降低卡顿
- 为网络电台增加独立 radio snapshot
- 完善 `/System/default_cover.jpg` 与内置 NO COVER 兜底逻辑
- 降低无卡状态下周期 mount 的日志频率
- 给 WiFi 默认策略增加配置项：开机开启 / 默认关闭 / 自动超时关闭
- 给 NAS 歌曲增加时长字段，建议由 PC / NAS 侧脚本预生成，不在 ESP32 上逐首计算
- 优化 NAS 歌曲元数据生成，将歌手 / 专辑从文件名或标签中预处理进 `net_music.txt`
- 将 NAS EOF watchdog 的等待时间参数化

### 中期方向

- 继续瘦身内部 RAM
- 统一更多播放源抽象
- 优化 Web JSON 构造与长时间运行稳定性
- NAS MP3 播放已完成，后续可扩展为 WebDAV / 更通用 HTTP 文件源
- 网络 FLAC 文件播放，优先考虑 HTTP Range source，而不是直接做纯流式 FLAC

### 硬件建议

下一版硬件建议增加 TF 卡座 Card Detect 引脚。  
有 CD 脚后，可以减少无卡状态下的周期 mount，降低日志噪声和无效 SD 初始化开销。

---

## 18. 项目结构（按职责）

```text
include/
├─ audio/
├─ board/
├─ keys/
├─ lyrics/
├─ nfc/
├─ net_music/
├─ radio/
├─ storage/
├─ ui/
├─ utils/
└─ web/

src/
├─ audio/
├─ board/
├─ keys/
├─ lyrics/
├─ nfc/
├─ net_music/
├─ radio/
├─ storage/
├─ ui/
├─ utils/
├─ web/
├─ player_*.cpp
├─ app_state.cpp
├─ boot_state.cpp
└─ main.cpp
```

---

## 19. 一句话总结

这不是一个“只有 SD 本地播放”的小播放器了。当前主线已经演进成：

**以 ESP32-S3 为核心、以 V3 索引为底座、以统一 MP3 核心承接本地与网络来源、带圆屏 UI / NFC / Web 控制的多来源音乐播放器原型。**
