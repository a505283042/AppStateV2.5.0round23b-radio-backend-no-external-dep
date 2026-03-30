# ESP32-S3 圆屏音乐播放器

一个基于 **ESP32-S3 + PSRAM** 的本地音乐 / 网络电台播放器，面向“圆屏桌面播放器”场景设计，支持：

- 本地 **MP3 / FLAC** 播放
- **HTTP MP3 网络电台** 播放
- **圆形 TFT 双视图 UI**（旋转封面 / 信息视图）
- **歌词显示** 与下一句提示
- **NFC 绑定播放**（歌曲 / 歌手 / 专辑）
- **Web 控制页**（状态查看、切歌、音量、模式、电台、封面、设置）
- 基于 **V3 音乐索引** 的快速启动与重扫
- 基于 **NVS** 的播放状态与网页设置持久化

> 本 README 按当前主线整理：
> - 网络电台主线已统一为 **Audio Tools URLStream -> unified MP3 core (`audio_mp3.cpp`)**
> - 旧的 `audio_mp3_stream*` 兼容播放链已下线或应视为历史残留
> - 历史数据兼容层（如旧 NVS key / 旧 NFC 绑定格式）仍保留，用于平滑升级

---

## 1. 当前能力概览

### 已完成

- 本地文件播放：`MP3`、`FLAC`
- 电台播放：`HTTP MP3 stream`
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
- 列表选择模式（歌手 / 专辑）
- NFC 绑定：
  - `track`
  - `artist`
  - `album`
- Web 控制：
  - 当前状态
  - 切歌 / 播放暂停 / 模式切换 / 音量
  - 歌手 / 专辑 / 电台页面
  - 当前封面获取
  - 设置页
  - 扫描 / 保存状态
- 启动恢复：
  - NVS blob 快照恢复播放状态
  - 兼容旧版 NVS key
- Wi‑Fi：
  - 优先 STA 连 `wifi.conf`
  - 失败自动回退 AP 热点
- 运行时监控：
  - heap / internal / dma / psram
  - 任务栈高水位

### 当前主线不包含

- `m3u8 / HLS` 播放
- SMB / NFS / NAS 目录浏览
- 网络 FLAC 文件播放
- OTA / 蓝牙 / 触摸交互

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
└─ player_source（本地 / 网络电台来源摘要）

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
└─ storage_groups_v3

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

这意味着当前项目已经实现了：

- **文件 MP3** 与 **网络 MP3** 共用统一 MP3 解码主线
- “来源”和“解码器”已经开始分离，后续扩展网络文件 / NAS / WebDAV 会更顺

---

## 5. 启动流程

系统启动大致顺序如下：

1. 初始化串口与 SPI 总线
2. 初始化 SD 卡
3. 读取 NFC 绑定表 `/System/nfc_map.txt`
4. 初始化固定封面缓冲区（优先 PSRAM）
5. 初始化 UI
6. 启动 `audio_service` 音频任务
7. 启动运行时监控任务
8. 初始化 NFC
9. 加载或重建 `V3` 音乐索引 `/System/music_index_v3.bin`
10. 预加载电台列表 `/System/radio_list.txt`
11. 从 NVS 读取待恢复快照
12. 启动 Web 服务器
13. 进入 `STATE_PLAYER`

---

## 6. SD 卡目录约定

推荐最小目录：

```text
/ Music/
    xxx.mp3
    xxx.flac
/ System/
    music_index_v3.bin
    radio_list.txt
    nfc_map.txt
    default_cover.jpg
    /config/
        wifi.conf
        web_settings.conf   # 旧版导入源，可选
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

兼容旧格式：

```text
UID=PATH
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

---

## 7. Web 控制

### 入口页面

- `/`：主控页
- `/artists`
- `/albums`
- `/radios`
- `/settings`

### 主要 API

- `GET /api/status`
- `GET /api/artists`
- `GET /api/albums`
- `GET /api/radios`
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

### 网页设置

当前持久化在 **NVS** 中：

- 刷新档位：省流量 / 平衡 / 流畅
- 歌词同步策略：精准优先 / 平衡 / 等轮询优先
- 是否显示下一句歌词
- 是否显示封面
- 旋转视图时网页封面是否旋转

如果没有 NVS 设置，会尝试导入旧版 SD 文件：

- `/System/config/web_settings.conf`

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
| NEXT | 长按 | 歌手/专辑模式下进入列表选择；全部模式下大步前进 |
| VOL- | 按住连发 | 音量减 |
| VOL+ | 按住连发 | 音量加 |

### 扫描中

- 仅允许 `MODE` 触发取消扫描
- 其他按键逻辑屏蔽

### NFC 管理模式

- `MODE` / `PLAY` 交给 `nfc_admin_state` 处理

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

恢复策略：

- 启动时先读取“待恢复快照”
- 首次进入 player 后先恢复轻量状态
- 再延后恢复曲目，避免阻塞进入主界面

兼容保留：

- 旧 NVS key 读取逻辑仍在
- 目的是让旧版本升级后仍能恢复已有状态

---

## 12. 内存设计（当前版本重点）

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

启动时建议关注这些日志：

- `psramFound / FreePsram / FreeHeap`
- `[SDIO] recursive SD mutex created`
- `[BOOT] NFC bindings loaded`
- `[CATALOG_V3] load ok` 或 `native rebuild ok`
- `[RADIO] catalog loaded`
- `[SNAPSHOT] pending loaded`
- `[WEB] STA connected` 或 `[WEB] AP ready`
- `[WEB] server started`
- `[MON][MEM] ...`
- `[MON][STACK] ...`

如果电台播放异常，建议重点看：

- `[RADIO] http code=...`
- `content-type=...`
- `icy-metaint=...`
- `backend=...`

---

## 16. 已知边界与注意事项

1. 当前稳定网络音频能力是 **HTTP MP3 电台**。  
   `m3u8/HLS` 不是当前稳定主线。

2. 当前项目仍保留部分**兼容迁移层**，例如：
   - 旧 NVS key 恢复
   - 旧 NFC `UID=PATH` 格式导入
   - V3 catalog 到旧 `TrackInfo` 的桥接

3. 如果你的本地分支里还看到这些文件：
   - `audio_mp3_stream.cpp`
   - `audio_mp3_stream_audiotools.cpp`
   - `audio_mp3_stream.h`

   它们应视为历史兼容残留，而不是当前推荐主线。

4. Web 页当前是轻量内嵌页面，不是独立前端工程。

5. 目前重点优化方向仍然是：
   - 内部 RAM 压力
   - String / 队列对象瘦身
   - 歌词 / playlist 索引进一步外移到 PSRAM

---

## 17. 后续演进建议

### 较近目标

- 继续瘦身内部 RAM
- 统一更多播放源抽象
- 优化 Web JSON 构造与长时间运行稳定性
- 收口兼容桥接层

### 中期方向

- 远程 MP3 文件播放
- WebDAV / NAS 文件源
- 更完整的列表页与筛选页
- 更细的播放状态 / 电台元数据展示

### 更远方向

- HLS / m3u8
- 网络 FLAC
- 更通用的 media source 抽象

---

## 18. 项目结构（按职责）

```text
include/
├─ audio/
├─ board/
├─ keys/
├─ lyrics/
├─ nfc/
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
