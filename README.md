# ESP32-S3 圆屏音乐播放器

基于 **ESP32-S3 + OPI PSRAM** 的独立音乐播放器固件，面向圆屏桌面播放器与定制 PCB 场景开发。

当前文档对应持续维护基线 **Round 59**。项目集成本地音乐、HTTP 网络电台、NAS/HTTP 网络曲库、圆屏 UI、Web 控制、NFC、蓝牙发射、电池管理、RTC 闹钟、睡眠关机、TF 卡配置中心、霍尔/电磁铁摆臂联动和运行时诊断等功能。

> 本文档按当前维护代码更新。硬件接线、功能状态、配置路径和控制语义均以源码为准，不再沿用旧版 README 中的独立按键、旧 PCB 引脚表或“电磁铁盲目翻转”逻辑。

---

## 1. 功能概览

### 音频播放

- 本地 MP3、FLAC 播放
- HTTP MP3 网络电台
- NAS/HTTP MP3 文件播放
- NAS/HTTP FLAC Range 播放
- 本地与网络歌曲进度显示、拖动和长按快进/快退
- NAS 网络歌曲顺序 / 随机播放
- 网络播放结束自动下一首
- 网络 FLAC 独立预取环形缓冲、断流重连和性能诊断
- MP3、FLAC 内嵌封面解析及外部默认封面兜底
- LRC 歌词显示、当前句和下一句提示

### 播放组织

- 全部顺序 / 全部随机
- 歌手顺序 / 歌手随机
- 专辑顺序 / 专辑随机
- 歌手、专辑、本地歌曲、电台和 NAS 歌曲列表
- V3 本地音乐索引
- 超快速、快速、严格增量和完整重扫模式
- 按 TF 卡身份分别保存本地播放快照
- 按 NAS 根地址分别保存网络播放快照
- NVS 快照容量治理：最多保留最近 4 张 TF 卡和 2 个 NAS 上下文

### 输出与硬件控制

- 3.5 mm 耳机 / Line out 常通输出
- 耳机模式
- 功放 / 扬声器模式
- EWM104-BT62SP 蓝牙发射模式
- 蓝牙发射音量查询、保存和逻辑音量映射
- 蓝牙配对、模块重启及已连接设备查询
- 功放静音和关断保护
- 背光、WS2812 状态灯、电磁铁和霍尔控制
- 电磁铁 A/B 方向可在快捷菜单和 Web 中反转并持久化
- 摆臂停止位 / 播放位与播放状态联动

### 电源与时间

- BQ27441 电量计
- 电压、SOC、电流、容量、SOH 和充电状态显示
- 续航时间估算
- 低电量安全关机判定
- PCF85063A RTC
- 单次、每天、工作日、周末和指定星期闹钟
- 闹钟开机后仅唤醒或恢复上次播放；自动播放时音量从低档缓慢增加到目标值
- 15 / 30 / 60 / 90 分钟睡眠关机
- 保存状态后安全断电

### Web 与 TF 卡配置

- 240 × 240 圆形 TFT UI
- EC06 旋转编码器与组合按键
- NFC 绑定歌曲、歌手和专辑
- Web 主控、曲库、电台、NAS、NFC、设置和诊断页面
- “TF 卡配置中心”统一管理 Wi-Fi、电台、NAS 和系统图片
- Wi-Fi、电台和 NAS 条目逐项折叠编辑
- 电台台标、默认封面和 NAS 加载图上传
- 配置内容先暂存 PSRAM；本地音乐播放时在切歌安全窗口写入 TF 卡
- `.tmp + .bak` 原子替换和掉电恢复
- Web 普通提示使用顶部非阻塞通知，删除、重连和重启等危险操作保留确认框
- 浏览器首次连接 Web 后，每次设备开机可自动用浏览器本地时间校准 RTC 一次
- Wi-Fi STA 优先、AP 回退和多网络切换
- 无 Card Detect 引脚的 TF 卡热插拔检测
- 崩溃摘要和 coredump 转存

---

## 2. 当前边界

当前代码未提供或不应视为稳定能力的功能：

- HLS / M3U8 解码
- SMB / NFS 协议直连
- ESP32 直接扫描 NAS 文件系统
- HTTPS 网络 FLAC；当前 HTTP Range 音源仅支持 `http://`
- OTA 固件升级
- 触摸屏交互
- 蓝牙接收音频播放链路；当前播放器输出路由以蓝牙发射为主

NAS 音乐需要由 Web Station、nginx、Apache 或其他静态 HTTP 服务暴露。远程 FLAC 服务器必须正确支持 `Range` 请求和文件长度响应。

---

## 3. 硬件平台

### 主控与存储

- ESP32-S3
- 16 MB Flash
- OPI PSRAM
- TF / SD 卡，FAT32
- 自定义 16 MB 分区表，包含 512 KB coredump 分区

### 外设

- 240 × 240 GC9A01 圆形 TFT
- I2S DAC / 模拟音频输出级
- RC522 NFC 模块
- MCP23017 GPIO 扩展器，地址 `0x20`
- BQ27441 电量计
- PCF85063A RTC
- EWM104-BT62SP 蓝牙模块
- EC06 旋转编码器
- WS2812 状态灯
- 霍尔传感器
- 功放、电磁铁和自锁电源控制电路

---

## 4. PCB1 引脚定义

实际定义文件：

- `include/board/board_pins_pcb1_mcp23017.h`
- `include/board/board_pins.h`
- `include/keys/keys_pins.h`

### ESP32-S3 GPIO

| 功能 | GPIO |
|---|---:|
| I2C SDA | 18 |
| I2C SCL | 8 |
| MCP23017 INTA | 2 |
| WS2812 | 3 |
| POWER_CTRL | 47 |
| POWER_PLAY | 48 |
| BQ27441 GPOUT | 1 |
| RTC INT | 未直接连接，代码值为 `-1` |
| HALL_OUT | 9 |
| NFC IRQ | 4 |
| UI SPI MISO | 5 |
| UI SPI MOSI | 6 |
| UI SPI CLK | 7 |
| NFC CS | 15 |
| TFT DC | 16 |
| TFT CS | 17 |
| SD MISO | 10 |
| SD SCK | 11 |
| SD MOSI | 12 |
| SD CS | 13 |
| UART1 RX | 14 |
| UART1 TX | 21 |
| EC06 B | 38 |
| EC06 A | 39 |
| BT MODE CTRL | 45 |
| I2S LRCK | 40 |
| I2S DOUT | 41 |
| I2S BCLK | 42 |

> GPIO45 是 ESP32-S3 启动绑带脚。外部电路不得在启动阶段强制到错误电平。

### MCP23017 U3，地址 0x20

#### Port B

| Bit | 功能 |
|---:|---|
| B0 | SOL_CTRL_A |
| B1 | SOL_CTRL_B |
| B2 | NFC RESET |
| B3 | TFT RESET |
| B4 | 背光控制 BLK |
| B5 | 充电电源良好 PG |
| B6 | 充电状态 CHG_STAT |
| B7 | 蓝牙电源 BT_PWR_EN |

#### Port A

| Bit | 功能 |
|---:|---|
| A0 | 功放静音 MUTE_EN |
| A1 | 功放关断 SHDN_EN |
| A2 | BACK / MODE 键 |
| A3 | EC06 按压键 |
| A4 | BT_LINK |
| A5 | BT_SW_CTRL |
| A6 | PREV / NFC 键 |
| A7 | NEXT / LIST 键 |

TFT 与 NFC 共用 UI SPI，总线访问由互斥锁保护；SD 使用独立 SPI 实例。

---

## 5. 软件架构

```text
Arduino setup / loop
└─ app_state
   ├─ boot_state
   ├─ player
   └─ NFC admin

Player
├─ player_state / player_control
├─ player_playlist / player_list_select
├─ player_assets
├─ player_source
├─ player_snapshot
├─ player_binding
└─ app_alarm / app_power

AudioTask
├─ audio_service：命令队列与任务边界
├─ audio_mp3：MP3 解码主线
├─ audio_flac：本地与网络 FLAC
├─ audio_radio_backend：网络电台
├─ audio_http_range_source：网络 FLAC 预取与 Range 读取
├─ audio_i2s：I2S 输出
└─ audio_output_route：耳机、功放、蓝牙发射路由

Storage
├─ storage_catalog_v3
├─ storage_scan_v3 / storage_builder_v3
├─ storage_manifest_v1
├─ storage_hotplug
├─ storage_config_writer
└─ system_paths

Hardware / HAL
├─ board_hw_control
├─ hall_control
├─ bq27441
├─ pcf85063
├─ mcp23017_u3
├─ i2c_bus_lock
├─ bt62sp_uart_debug
├─ bluetooth_restart_controller
└─ ws2812_status

Presentation
├─ ui
├─ menu / quick_menu
├─ web_server / web_snapshot / web_settings / web_notice
└─ nfc
```

### 并发模型

项目不是单线程播放器。常驻或按需创建的主要并发单元包括：

- Arduino `loopTask`
- `AudioTask`
- `UiTask`
- `PlayerAssetTask`
- 网络封面任务 `NetCoverTask`
- 网络 FLAC 预取任务 `FlacNetTask`
- 曲库重扫任务 `rescan_v3`
- 蓝牙重启控制任务
- Web 异步启动任务

`RuntimeMon` 不再创建独立任务。它通过 `runtime_monitor_update()` 并入 `loopTask`，首次延迟约 6 秒，之后约每 15 秒采样一次。

关键设计原则：

1. 音频硬件和输出路由的底层变更由 `AudioTask` 串行执行。
2. 网络 FLAC 的 `WiFiClient` 只由 `FlacNetTask` 持有。
3. UI、Web 和其他任务读取纯数值快照，不直接访问音频传输对象。
4. SD、I2C、UI SPI 等共享资源必须经过项目封装的锁。
5. 大块封面、配置上传、网络列表和 MP3 输入缓冲优先使用 PSRAM。
6. 配置写入只有在旧本地音频文件确认关闭后才进入 TF 卡事务。
7. 跨任务通知使用固定快照、序号或受锁状态，不依赖悬空对象。

### 内部 RAM 优化

当前维护版本已经实施以下常驻内部 RAM 回收：

- 圆形文字、唱片跨度和三角函数 LUT 迁移到 Flash 只读表
- MP3 与 FLAC 共用 16,512 字节 PCM 解码工作区
- 本地 MP3 8 KB 输入缓冲按需分配到 PSRAM
- 崩溃文件迁移去除 32 × 96 字节常驻名称表
- SD 探测 512 字节缓冲改为临时栈缓冲
- `RuntimeMon` 并入 `loopTask`

累计理论回收约 **31.8～32.1 KiB 内部 RAM**。一次网络 FLAC 播放实测中，内部空闲约 **94 KiB**，最大连续块约 **80 KiB**；实际值会随功能、任务和日志配置变化。

---

## 6. 启动流程

启动主线位于 `src/main.cpp`、`src/app_state.cpp` 和 `src/boot_state.cpp`。

1. 提前拉高 `POWER_CTRL`，保持整机供电。
2. 初始化 USB CDC 主串口和 BT62SP UART1 调试桥。
3. 初始化应用状态、板级硬件、按键和 I2C 外设。
4. 初始化 UI SPI、NFC 共享锁和 SD SPI。
5. 初始化 RTC，并加载和重新安排闹钟。
6. 挂载 TF 卡。
7. 创建 `/System` 分类目录，并迁移旧版根目录文件。
8. 将 flash coredump 转存到 TF 卡。
9. 加载 NFC 绑定、本地音乐索引、电台配置和 NAS 基础配置。
10. 初始化固定封面缓冲区与列表快照。
11. 启动 UI、`AudioTask`，并初始化已并入 `loopTask` 的运行时监控。
12. 初始化 NFC 和霍尔输入状态。
13. 按 TF 卡身份加载待恢复的 NVS 播放快照。
14. 进入播放器状态。
15. 延迟异步启动 Wi-Fi 和 Web 服务，避免联网过程干扰开机起播。

开机无 TF 卡不是致命错误。系统仍可进入 UI，并在后续检测到插卡时重新挂载和加载本地资源。

换卡后，如果新卡没有有效快照，固件会从当前播放列表预选第一首并直接显示播放器 UI，但不会自动播放。只有无歌曲、曲库加载失败或歌曲信息失败时才显示明确错误。

---

## 7. 音频来源

### 本地文件

默认扫描根目录：

```text
/Music
```

支持：

- `.mp3`
- `.flac`

音频、歌词和封面读取均应通过项目存储封装，避免绕过 SD 互斥锁。

### HTTP MP3 电台

电台列表来自：

```text
/System/config/radio_list.txt
```

建议格式：

```text
name|url
name|url|format
name|url|format|region
name|url|format|region|logo
```

示例：

```text
Music Radio|http://192.168.1.10:8000/live.mp3|mp3|Local|/System/assets/radio/radio_00.jpg
```

`format` 字段可以保留扩展信息，但当前稳定播放主线是 HTTP MP3，不支持 HLS/M3U8。

电台列表可以从 Web 配置中心新建、编辑、排序和删除。读取旧文件时支持 UTF-8 BOM、注释行和旧版字段数量；个别不兼容行会被跳过并显示行号，不会让整个列表加载失败。台标可以使用远程 HTTP/HTTPS URL，或使用 `/System/assets/radio/` 下的本地文件。

### NAS / HTTP 网络歌曲

网络曲库由两级配置组成：

```text
/System/config/net_music_base.txt
/System/config/net_music_sources.txt
```

开机只读取小型基础配置；进入 NAS 列表或 Web NAS 页面时，才下载当前源的 `net_music.txt`。

`net_music.txt` 不写入 TF 卡，而是存入内存并建立行偏移索引，避免与本地音频抢占 SD。

网络歌曲记录格式：

```text
title|path|format|artist|album|duration_ms
```

后面的字段可按实际生成工具兼容处理。第二列可以是：

- UTF-8 原始相对路径；固件在构造 URL 时编码
- 旧版 `%XX` URL 编码路径；固件不会重复编码

示例：

```text
Track One|Artist/Track One.mp3|mp3|Artist|Album|215000
Track Two|HiRes/Track Two.flac|flac|Artist|Album|0
```

网络 FLAC 使用 HTTP Range 可寻址音源：

- 当前只接受 `http://`
- 服务端必须支持 Range
- 后台任务持续预取到环形缓冲
- 解码任务只从缓存读取
- 支持断流重连、缓存水位和等待时间诊断
- 高码率 / 高采样率文件会提高预取水位，但不会故意延后歌词和封面任务

### 多 NAS 曲库源

`net_music_sources.txt` 用于声明多个逻辑曲库。设备只加载当前选中的一个源，切换源时释放旧列表和偏移表，再按需加载新源。NAS Web 页面可通过“重新读取列表文件”强制下载当前源最新的 `net_music.txt`；新列表完成下载和索引后才替换旧列表，失败时继续保留旧列表。

根地址和曲库源可以从 Web 配置中心新建、编辑、排序和保存。“保存配置”只更新 TF 卡；“保存并应用”在两个文件都成功落盘后重新加载运行配置，不会在只写入一半时应用。

NAS 侧列表可使用仓库中的工具生成：

```text
tools/music_library_scanner
tools/nas
```

---

## 8. 输出路由与蓝牙

输出路由：

| 路由 | 行为 |
|---|---|
| `HeadphoneOnly` | 仅保留 3.5 mm 耳机 / Line out，功放保持静音和关断 |
| `Speaker` | 耳机 / Line out 常通，同时启用功放输出 |
| `BluetoothTx` | 耳机 / Line out 常通，关闭功放，启用 BT62SP 发射 |

蓝牙发射音量使用面向用户的 `0..100` 逻辑范围：

- 逻辑 `0` 映射为 BT62SP 硬件静音 `0`
- 逻辑 `1..100` 映射到模块可听区间 `4..100`
- 第一次使用默认值为 50
- 普通输出音量与蓝牙发射音量分别保存

BT62SP UART1 默认波特率：

```text
1000000
```

USB 串口可作为 AT 命令桥：电脑输入 `AT...` 后转发到 BT62SP，模块返回内容同步输出到 USB 串口。

---

## 9. 电池、充电和关机

BQ27441 提供：

- 电池电压
- SOC 百分比
- 平均电流
- 剩余容量
- 满充容量
- 设计容量
- Flags
- SOH
- GPOUT 状态

充电管理输入来自 MCP23017：

- `PG`：外部电源良好
- `CHG_STAT`：充电状态

续航估算会区分：

- 数据不可用
- 正在充电
- 外部供电
- 电流过低
- 负载稳定中
- 蓝牙估算
- 估算可用

低电量关机原因包括：

- 最终 SOC 阈值
- 临界 SOC
- 临界电压
- GPOUT 拉低

所有主动关机应走统一的 `app_power_save_and_shutdown()` 流程，先保存必要状态、停止音频和外设，再释放自锁电源。

### 睡眠关机

档位：

```text
关闭 -> 15 -> 30 -> 60 -> 90 -> 关闭
```

启用睡眠定时后，屏幕在最后一次实体按键或旋钮操作 15 秒后关闭；操作过程中会持续刷新熄屏期限，计时到点后进入统一安全关机流程。

---

## 10. RTC 与闹钟

RTC：PCF85063A。

闹钟配置支持：

- 单次
- 每天
- 工作日
- 周末
- 每周指定一个或多个星期

到点动作：

- `WAKE_ONLY`：只开机，不自动播放
- `RESUME_LAST`：恢复上次播放

默认配置值：

```text
时间：07:30
重复：每天
动作：恢复上次播放
音量：30
```

闹钟可通过圆屏快捷菜单和 Web API 保存、停用或删除。固件会计算下次触发时间并写入 RTC；浏览器校时后会重新安排当前已启用闹钟。自动恢复播放时，音量从 5 开始，每 500ms 增加 2，直到目标音量；用户手动改音量会终止渐增。

> 当前板级定义中 `PIN_RTC_INT = -1`。RTC 唤醒是否能在整机断电状态工作，取决于 PCB 的 RTC 中断与自锁电源电路连接，而不是 MCU GPIO 轮询。

---

## 11. 按键与旋钮

当前 PCB 不再使用旧 README 中的 6 个独立 GPIO 按键。MODE、编码器按压、PREV 和 NEXT 位于 MCP23017，PLAY / POWER 位于 GPIO48。

### 播放器主界面

| 操作 | 行为 |
|---|---|
| 旋转编码器 | 调整音量 |
| MODE 短按 | 切换音量步进 `×1 / ×5` |
| MODE 长按 | 切换播放器视图 |
| PLAY 短按 | 按霍尔 / 电磁铁模式执行播放、暂停或摆臂动作 |
| PREV 短按 | 用户主动上一首；必要时先等待播放位 |
| NEXT 短按 | 用户主动下一首；必要时先等待播放位 |
| PREV 长按 | 快退预览，松开后提交 |
| NEXT 长按 | 快进预览，松开后提交 |
| 编码器短按 | 进入快捷菜单 |
| 编码器 + 长按 PREV | 打开 NFC 绑定 |
| 编码器 + 长按 NEXT | 进入 / 切换播放列表或分组 |

长按判定与组合键有防误触处理；必须先按住编码器，再按 PREV / NEXT，才会触发组合动作。

### 快捷菜单

根菜单包括：

- 播放控制
- 播放来源
- 显示设置
- 时间与闹钟
- NFC 管理
- 网络设置
- 音频输出
- 系统信息

“播放控制”中包含：

- 霍尔控制
- 电磁铁
- 电磁铁方向：正常 / 反转
- 其他播放相关开关

电磁铁开启时霍尔会被强制联动，菜单显示为联动状态；必须先关闭电磁铁，才能单独关闭霍尔。

### 霍尔与电磁铁摆臂

当前机械语义：

```text
靠近霍尔 = 停止位 / 暂停位
离开霍尔 = 播放位
```

霍尔低电平表示摆臂磁铁靠近。

#### 电磁铁开启

- PLAY 和 Web 播放 / 暂停使用同一逻辑。
- 停止或暂停状态请求播放：目标是播放位。
- 正在播放时请求暂停：目标是停止位。
- 摆臂已经位于目标位置时，不重复输出电磁铁脉冲。
- 开机时若摆臂已经在播放位，第一次按 PLAY 会直接开始播放，不会先推回停止位。
- 电磁铁 A/B 映射可在菜单或 Web 反转，设置保存到 NVS。

电磁铁时序：

```text
0～220ms：完整全功率脉冲，不做最终到位判定
220ms：强制断电
220～240ms：等待机械稳定
之后：目标霍尔电平连续稳定 30ms
最晚 360ms：仍未确认则判定失败
```

动作失败时不会改变播放状态，也不会执行暂存的选歌请求。

#### 仅霍尔开启

- 播放中的暂停请求可以直接暂停。
- 停止或暂停状态下，如果摆臂不在播放位，PLAY、Web 播放、选歌和手动上一首 / 下一首都不会立即执行。
- 页面和圆屏提示“摆臂未在播放位，请手动拨到播放位”。
- 用户手动拨到播放位并通过 30ms 消抖后，才执行之前暂存的请求。
- 摆臂从播放位手动拨回停止位时会暂停。
- 等待期间重复请求不会叠加，也不会覆盖第一个请求。

#### 霍尔和电磁铁都关闭

PLAY、Web 播放、列表选歌和手动上一首 / 下一首直接执行，摆臂位置不参与控制。

### 用户主动选歌的播放位门禁

以下入口统一经过 `hall_control_request_play_position()`：

- 本地 UI 歌曲、歌手和专辑列表
- 本地 UI NAS 和电台列表
- Web 歌曲、歌手、专辑、NAS 和电台列表
- 实体与 Web 上一首 / 下一首
- NFC 绑定起播

如果电磁铁开启，会自动驱动到播放位；如果仅霍尔开启，会等待用户手动拨动。歌曲自然结束后的自动下一首不驱动摆臂。

---

## 12. NFC

绑定文件：

```text
/System/config/nfc_map.txt
```

格式：

```text
UID|TYPE|KEY|DISPLAY
```

类型：

- `track`
- `artist`
- `album`

示例：

```text
09:76:10:05|track|/Music/Artist/Track.flac|Track - Artist
F7:8C:64:06|album|Album Key|Album Name
```

旧版无类型记录会按单曲绑定兼容处理。

Web 端支持：

- 查询绑定列表
- 删除绑定
- 测试播放
- 对歌曲、歌手和专辑发起绑定

NAS 网络歌曲当前不作为 NFC 单曲绑定目标；NFC 绑定以本地 V3 曲库为主。

### NFC 与摆臂联动

刷卡后先解析并确认绑定目标有效，但不会提前改变播放模式或打开歌曲。

- 摆臂已经在播放位：立即执行绑定。
- 电磁铁开启且摆臂在停止位：驱动到播放位，霍尔确认离开后才播放。
- 仅霍尔开启且摆臂在停止位：暂存绑定目标，提示用户手动拨到播放位。
- 动作失败、超时、关闭霍尔或退出允许播放的状态：取消绑定起播，不播放目标。
- 摆臂动作期间重复刷卡不会覆盖当前待执行目标。

单曲、歌手和专辑绑定共用同一套延迟起播状态机。

---

## 13. TF 卡目录

推荐目录：

```text
/Music/
    Artist/
        Album/
            track.mp3
            track.flac
            track.lrc

/System/
    config/
        wifi.conf
        wifi.conf.bak
        radio_list.txt
        radio_list.txt.bak
        net_music_base.txt
        net_music_sources.txt
        nfc_map.txt
    assets/
        default_cover.jpg
        net_cover_loading.jpg
        radio/
            radio_00.jpg
            radio_01.png
    library/
        music_index_v3.bin
        music_manifest_v1.bin
    reports/
        music_scan_report.json
        music_scan_tracks.csv
    crash/
        panic_summary.txt
        panic_xxxxxxxx.txt
        coredump_xxxxxxxx.bin
```

固件启动后会创建缺失的分类目录，并尝试把旧版直接位于 `/System` 根目录下的配置、资源、索引和诊断文件迁移到新目录。

### TF 卡配置中心

Web `/settings` 中的“TF 卡配置中心”统一管理：

- Wi-Fi 网络
- 网络电台
- NAS 根地址和曲库源
- 默认封面
- NAS 加载图

顶层配置中心、四个分类以及 Wi-Fi / 电台 / NAS 的每条记录都可独立折叠。已有条目默认收起，新建条目自动展开。

即使对应文件不存在，也可以直接从 Web 新建：

```text
/System/config/wifi.conf
/System/config/radio_list.txt
/System/config/net_music_base.txt
/System/config/net_music_sources.txt
/System/assets/default_cover.jpg
/System/assets/net_cover_loading.jpg
```

### PSRAM 延迟写入

所有受管配置和上传资源先复制到 PSRAM：

```text
Web 请求
→ 参数与文件格式校验
→ PSRAM 待写队列
→ HTTP 立即返回
```

如果本地歌曲文件正在播放或暂停，则等待安全窗口：

```text
停止旧音频并确认文件关闭
→ 提交所有待写项
→ 写入 .tmp
→ sync 与重新校验
→ 原文件改名为 .bak
→ .tmp 提升为正式文件
→ 再打开下一音源
```

如果没有本地音频文件占用 TF 卡，则仍先进入 PSRAM，再立即走相同事务写入路径。

重要边界：

- 同一路径连续保存只保留最新内容。
- 写入失败的项目保留在 PSRAM，下一安全窗口重试。
- 拔卡时丢弃全部待写项，避免旧卡配置写到新卡。
- PSRAM 是易失存储；尚未落盘时断电或复位会丢失待写配置。
- 多文件 NAS 配置使用事务世代和回滚保护，两个文件完整落盘后才应用。

### 图片规则

默认封面：

```text
/System/assets/default_cover.jpg
```

NAS 封面加载占位图：

```text
/System/assets/net_cover_loading.jpg
```

两者通过 Web 上传时：

- 只接受完整 JPEG
- 最大 400KB
- 建议 240 × 240

电台台标保存到：

```text
/System/assets/radio/
```

电台台标支持完整 JPEG 或 PNG，最大 1MB。

---

## 14. Wi-Fi 配置

文件：

```text
/System/config/wifi.conf
```

当前格式使用 `[network]` 分组：

```ini
hostname=esp32s3-player

[network]
ssid=HomeWiFi
password=your_password

[network]
ssid=OfficeHidden
password=your_password
hidden=1
channel=6
bssid=AA:BB:CC:DD:EE:FF
```

行为：

1. 按文件顺序尝试 STA 网络。
2. 连接失败后启动 AP 回退。
3. Wi-Fi 总开关保存在 NVS；关闭后下次开机不会启动 STA 或 AP。
4. 切换 Wi-Fi 前会停止网络音频，避免正在读取的 socket 被强制断开。
5. 无卡启动进入 AP 后，插卡可重新读取配置并尝试切换到 STA。
6. `wifi.conf` 不存在时，可从 Web 添加网络并首次创建。
7. Web 不返回明文密码，只返回是否已有密码；密码输入留空表示保留原值。
8. 支持添加、删除、排序、隐藏网络、信道、BSSID 和主机名。
9. “保存配置”不切换当前连接；“保存并重连”在响应返回后延迟应用。

本地音乐播放期间保存时，Wi-Fi 配置先暂存 PSRAM，在切歌安全窗口写入 TF 卡。只有实际落盘成功后，“保存并重连”才执行重连动作。

默认 AP：

```text
SSID: ESP32S3-Player
Password: 12345678
```

量产或公开部署前必须修改默认 AP 密码。

---

## 15. Web 控制

Web 功能由编译参数启用：

```ini
-DWEBCTRL_ENABLED=1
```

### 页面

| 路径 | 用途 |
|---|---|
| `/` | 主控制页 |
| `/artists` | 歌手 |
| `/albums` | 专辑 |
| `/nfc` | NFC 管理 |
| `/radios` | 网络电台 |
| `/netmusic` | NAS 网络曲库 |
| `/settings` | 设置、TF 卡配置与硬件控制 |

设置页针对手机端优化：

- 勾选项保持标题左、复选框右的单行布局
- 系统诊断优先显示标题左、数值右
- 极窄屏幕下诊断值换到下一行，标题保持颜色标记
- 展开的设置项和当前选中项有颜色高亮

### API 分组

#### 状态与播放

- `GET /api/status`
- `GET /api/status/check`
- `POST /api/playpause`
- `POST /api/seek`
- `POST /api/next`
- `POST /api/prev`
- `POST /api/volume`
- `POST /api/mode/toggle`
- `POST /api/mode/category`
- `POST /api/view/toggle`
- `POST /api/state/save`

Web PLAY、上一首、下一首和列表选歌与实体按键使用同一套霍尔 / 电磁铁播放位逻辑。

#### 本地曲库

- `GET /api/artists`
- `GET /api/albums`
- `GET /api/artist/detail`
- `GET /api/album/detail`
- `GET /api/artist/search_song`
- `GET /api/album/search_song`
- `POST /api/artist/play`
- `POST /api/album/play`
- `POST /api/track/play`
- `POST /api/scan`

重扫模式参数：

```text
ultra | fast | strict | full
```

#### NAS 网络音乐

- `GET /api/netmusic`
- `GET /api/netmusic/search`
- `POST /api/netmusic/source`
- `POST /api/netmusic/reload`
- `GET|POST /api/netmusic/play`
- `POST /api/netmusic/prev`
- `POST /api/netmusic/next`
- `POST /api/netmusic/toggle`
- `POST /api/netmusic/mode`
- `POST /api/netmusic/return-local`

#### 电台与封面

- `GET /api/radios`
- `POST /api/radio/play`
- `POST /api/radio/stop`
- `GET /api/cover/current`
- `GET /api/radio/logo/current`

#### TF 卡配置中心

Wi-Fi：

- `GET /api/config/wifi`
- `POST /api/config/wifi/save`
- `POST /api/config/wifi/apply`

网络电台：

- `GET /api/config/radios`
- `POST /api/config/radios/save`
- `POST /api/config/radios/logo?slot=N`

NAS：

- `GET /api/config/nas`
- `POST /api/config/nas/save`
- `POST /api/config/nas/apply`

系统图片：

- `GET /api/config/assets`
- `POST /api/config/assets/default-cover`
- `POST /api/config/assets/net-cover-loading`

#### NFC

- `GET /api/nfc/bindings`
- `POST /api/nfc/binding/delete`
- `POST /api/nfc/binding/test_play`
- `POST /api/artist/bind_nfc`
- `POST /api/album/bind_nfc`
- `POST /api/track/bind_nfc`

#### 音频输出与蓝牙

- `GET /api/audio-output/status`
- `POST /api/audio-output/route`
- `POST /api/audio-output/amp-mute`
- `POST /api/audio-output/bluetooth/query`
- `POST /api/audio-output/bluetooth/volume`
- `POST /api/audio-output/bluetooth/pair`
- `POST /api/audio-output/bluetooth/restart`

#### RTC、闹钟、设置与诊断

- `GET /api/rtc/status`
- `POST /api/rtc/time`
- `GET /api/alarm/status`
- `POST /api/alarm/save`
- `POST /api/alarm/disable`
- `POST /api/alarm/delete`
- `GET /api/system/diagnostics`
- `GET|POST /api/settings`

#### Web 通知

- `GET /api/notice?after=<sequence>`
- `GET /web-feedback.js`

机械动作、霍尔门禁和电磁铁超时会异步发布到所有 Web 页面。

### Web 通知规则

普通 `alert()` 已被公共兼容层改为顶部非阻塞通知：

- 成功：绿色
- 普通信息：蓝色
- 需要用户操作：黄色
- 错误：红色

通知自动关闭，也可点击 `×`。删除、重连、重启、重扫和其他破坏性操作继续使用原生 `confirm()`，必须选择确认或取消。

### Web 状态设计

`/api/status` 使用状态修订号和分块 JSON 输出，以减少大 `String` 拼接造成的内部堆碎片。浏览器端在两次轮询之间本地推进播放时间，降低请求频率并保持进度显示连续。

Web 运行设置保存在 NVS，包括：

- 状态刷新档位
- 歌词同步策略
- 下一句歌词显示
- 封面显示与旋转
- Wi-Fi 总开关
- 霍尔控制
- 电磁铁控制
- 电磁铁方向反转
- WS2812 状态灯和亮度

Wi-Fi、电台、NAS和系统图片内容保存在 TF 卡，不存入 NVS。

---

## 16. 本地曲库索引与扫描

V3 索引：

```text
/System/library/music_index_v3.bin
```

增量扫描清单：

```text
/System/library/music_manifest_v1.bin
```

报告：

```text
/System/reports/music_scan_report.json
/System/reports/music_scan_tracks.csv
```

扫描模式：

| 模式 | 目标 |
|---|---|
| `ultra` | 优先通过目录和清单信息快速判断变化 |
| `fast` | 常规增量校验 |
| `strict` | 更严格地核验文件变化 |
| `full` | 完整遍历并重建 |

索引失败或不存在时，固件会扫描 `/Music` 并重建。扫描期间可以取消，其他会破坏状态的播放操作会被限制。

### 播放快照与 NVS 容量治理

当前分区表中的 NVS 大小为 `0x5000`，即 20KB。播放器快照位于 NVS 的 `playerst` 命名空间：

- 本地卡：`snap_XXXXXXXX`
- NAS：`nas_XXXXXXXX`
- 元数据：`meta_v1`
- 最近使用索引：`index_v1`

常态保留：

```text
最近 TF 卡快照：最多 4 个
最近 NAS 快照：最多 2 个
```

升级到治理版本时会重建索引并删除超限历史键。若保存仍返回 `NOT_ENOUGH_SPACE`，系统会只保留当前 TF 和当前 NAS 后重试一次，不清理其他模块的 NVS 命名空间。

日志会输出整个 NVS 分区和 `playerst` 的条目统计，便于判断已用、空闲和快照键数量。

---

## 17. TF 卡热插拔

当前 PCB 没有 Card Detect 引脚，因此使用软件探测：

- 无卡时周期性尝试挂载
- 有卡且非本地音频热路径时低频探测存活状态
- 本地文件 `open/read/seek` 失败时上报 I/O 异常
- 通过底层扇区读取确认卡是否移除

拔卡后会根据当前来源采取不同策略：

- 本地播放：阻止自动下一首，停止音频并清理 TF 相关资源
- 网络电台：尽量不主动停止当前网络流，仅清理本地资源
- NAS 网络歌曲：清理 NAS 曲库索引；当前流可能短暂继续，但后续选歌依赖重新插卡和重新加载配置
- PSRAM 待写配置：立即丢弃，避免旧卡内容写到新卡

插入新卡后：

- 自动创建系统目录
- 加载索引、Wi-Fi、电台、NAS和NFC配置
- 优先恢复该卡快照
- 无有效快照但存在歌曲时，预选当前列表第一首并直接进入播放器 UI
- 不再用“TF卡已就绪，按播放键开始”全屏提示阻塞正常状态
- 只有无歌曲、曲库失败或歌曲信息失败时显示错误

SD 访问使用共享 SPI 模式和项目级递归互斥锁。不要在业务模块中直接并发操作全局 `sd` 对象。

---

## 18. 构建与烧录

### 环境

- PlatformIO
- `espressif32 @ 6.10.0`
- Arduino framework，兼容 Arduino-ESP32 2.0.17 API
- `board = esp32-s3-devkitc-1`

### 关键配置

```ini
board_upload.flash_size = 16MB
board_build.partitions = partitions_16mb_coredump.csv
board_build.psram = enabled
board_build.arduino.memory_type = qio_opi
board_build.flash_mode = qio
board_build.f_flash = 80000000L
build_type = release
monitor_speed = 115200
upload_protocol = esptool
```

USB CDC：

```ini
-DARDUINO_USB_MODE=1
-DARDUINO_USB_CDC_ON_BOOT=1
```

### 依赖

由 PlatformIO 拉取：

- LovyanGFX `1.2.0`
- SdFat `2.2.3`
- Arduino_MFRC522v2 `2.0.6`

仓库内还包含项目使用的解码 / 音频相关库目录，例如：

- `lib/dr_libs`
- `lib/minimp3`

### 命令

```bash
# 克隆仓库
git clone \
  https://github.com/a505283042/AppStateV2.5.0round23b-radio-backend-no-external-dep.git

cd AppStateV2.5.0round23b-radio-backend-no-external-dep

# 如维护代码位于独立分支，再切换到对应分支
# git checkout <branch-name>

# 编译
pio run

# 烧录
pio run -t upload

# 串口监视器
pio device monitor
```

关闭 / 打开串口监视器时不应使用 DTR/RTS 复位，以免自锁电源被意外断开；`platformio.ini` 已设置：

```ini
monitor_dtr = 0
monitor_rts = 0
```

---

## 19. 诊断开关

`platformio.ini` 中默认关闭的详细诊断：

```ini
-DAPP_DIAG_RAM_ATTRIBUTION=0
-DAPP_DIAG_AUDIO_RUNTIME=0
-DAPP_DIAG_NAS_FLAC_PERFORMANCE=0
-DAPP_DIAG_UI_RUNTIME=0
-DUI_RENDER_PROFILING=0
```

定位问题时只开启所需开关，不建议全部同时开启，否则串口输出和格式化开销可能干扰实时音频。

运行时监控已并入 `loopTask`，覆盖：

- 内部 heap
- 最大连续内部内存
- DMA 可用内存
- PSRAM
- `AudioTask`
- `UiTask`
- `loopTask`
- `PlayerAssetTask`
- `NetCoverTask`
- `FlacNetTask`
- `rescan_v3`
- 网络 FLAC 缓存水位、等待次数和重连次数
- 音频解码实时预算
- I2C 错误与恢复代次
- 电池和充电状态
- NVS 已用、空闲、总条目和播放器快照键数量
- 霍尔、电磁铁目标、脉冲阶段、确认阶段和超时

Web `/settings` 的系统诊断页支持手机端标题左、数值右显示；极窄屏幕下数值换行并保留标题颜色。

崩溃文件目录：

```text
/System/crash
```

设备启动并成功挂载 TF 卡后，会把 flash 中的 coredump 转存到该目录。

---

## 20. 项目目录

```text
.
├─ include/
│  ├─ audio/
│  ├─ board/
│  ├─ hal/
│  ├─ keys/
│  ├─ lyrics/
│  ├─ menu/
│  ├─ net_music/
│  ├─ nfc/
│  ├─ radio/
│  ├─ storage/
│  ├─ ui/
│  ├─ utils/
│  └─ web/
├─ lib/
│  ├─ dr_libs/
│  └─ minimp3/
├─ src/
│  ├─ audio/
│  ├─ board/
│  ├─ fonts/
│  ├─ hal/
│  ├─ keys/
│  ├─ lyrics/
│  ├─ menu/
│  ├─ meta/
│  ├─ net_music/
│  ├─ nfc/
│  ├─ radio/
│  ├─ storage/
│  ├─ ui/
│  ├─ utils/
│  ├─ web/
│  ├─ app_alarm.cpp
│  ├─ app_power.cpp
│  ├─ app_state.cpp
│  ├─ boot_state.cpp
│  ├─ main.cpp
│  └─ player_*.cpp
├─ tools/
│  ├─ music_library_scanner/
│  ├─ nas/
│  └─ find_mp3_seek_samples.py
├─ TF卡结构/System/
├─ partitions_16mb_coredump.csv
├─ platformio.ini
└─ README.md
```

---

## 21. 开发约束

修改项目时应遵守以下边界：

1. 不要从 UI、Web 或主循环直接操作解码器、I2S、功放和蓝牙路由底层。
2. 不要让多个任务直接持有同一个 `WiFiClient`。
3. 不要绕过 SD 互斥锁访问 TF 卡。
4. 不要在本地音频文件仍打开时直接写 TF 卡配置；统一使用 `storage_config_stage_psram()` 和安全提交窗口。
5. 不要向 Web 开放任意 TF 卡路径上传；上传接口只能写入受白名单约束的配置或资源。
6. 不要在音频实时路径中执行大块动态分配、长时间日志格式化或阻塞式联网。
7. 跨任务状态优先使用 snapshot / revision / 固定事件序号，而不是组合读取多个可变全局变量。
8. MCP23017 同一端口的按键应批量读取，避免每个按键各发一次 I2C 事务。
9. I2C 错误恢复后，依赖设备必须按恢复代次重新初始化。
10. 修改 PCB 引脚时只改板级定义和 HAL 映射，不在菜单、UI 或业务层散落硬编码。
11. 电磁铁业务层只表达“靠近霍尔”或“离开霍尔”，不要直接写 A/B 电平。
12. 所有用户主动起播入口应复用播放位门禁，不要绕过 `hall_control_request_play_position()`。
13. 自动下一首与用户主动切歌必须区分，避免连续播放时反复驱动摆臂。
14. 增加大对象前明确其应位于内部 RAM、PSRAM 还是 Flash，并检查最大连续内存。
15. 高规格 FLAC 优化必须以 4096 帧实时预算、缓存低水位和任务栈为依据，不只比较平均解码耗时。
16. 新增 NVS Blob 前评估条目开销和历史键治理，不要只看结构体字节数。

---

## 22. 常见问题

### NAS MP3 能播放，但 FLAC 打不开

检查：

- URL 是否为 `http://`
- HTTP 服务是否返回 `206 Partial Content`
- 是否支持 `Range: bytes=...`
- 是否返回正确 `Content-Length` 或 `Content-Range`
- 文件路径是否被正确 URL 编码

### 高规格 NAS FLAC 卡顿

依次检查：

- `APP_DIAG_NAS_FLAC_PERFORMANCE`
- 解码耗时是否超过每帧实时预算
- 环形缓冲是否触发低水位
- `reader_wait_total_us` 是否持续增加
- Wi-Fi RSSI 和 HTTP 服务吞吐
- 内部 RAM 最大连续块和任务栈余量
- Web / UI 高频刷新是否同时开启

### Wi-Fi 配置不存在或读取失败

可以进入：

```text
/settings
→ TF卡配置中心
→ Wi-Fi 网络
```

直接添加并创建 `wifi.conf`。若配置存在但仍进入 AP，检查：

- 文件路径是否为 `/System/config/wifi.conf`
- 是否使用 `[network]` 分组
- SSID、密码、隐藏网络频道和 BSSID 是否正确
- NVS 中 Wi-Fi 总开关是否被关闭
- 串口中是否出现 STA 超时日志

### 电台、NAS或系统图片文件不存在

均可从“TF卡配置中心”首次创建：

- 电台：添加条目并保存
- NAS：填写根地址和至少一个曲库源
- 默认封面 / NAS加载图：上传 JPEG
- 电台台标：在对应电台条目中上传 JPEG 或 PNG

### 保存配置后 TF 卡没有立即变化

本地歌曲正在播放或暂停时，配置只暂存在 PSRAM。需要：

- 切换歌曲
- 切换到 NAS / 电台
- 停止本地音频

进入安全窗口后才写入 TF 卡。断电前尚未落盘的 PSRAM 内容会丢失。

### 摆臂在停止位，Web或列表选歌不播放

检查当前模式：

- 电磁铁开启：应自动驱动到播放位；失败时查看“摆臂未到位”弹窗和串口日志。
- 仅霍尔开启：必须手动拨到播放位，霍尔确认后才执行暂存请求。
- 霍尔和电磁铁都关闭：应直接播放。

如果机械方向相反，在快捷菜单或 Web 设置中切换“电磁铁方向反转”。

### 电磁铁动作但一直报未到位

检查：

- `SOL` 日志中的物理方向 A/B
- 方向反转设置
- 220ms 脉冲期间线圈电压
- 摆臂是否在 220ms 后稳定到目标位置
- 霍尔低电平是否确实表示靠近
- 机械摩擦、气隙和供电压降

最终确认窗口为360ms；线圈只通电220ms。

### NVS 报 `NOT_ENOUGH_SPACE`

当前版本会自动治理播放器快照，保留最近4张TF卡和2个NAS。查看日志中的：

```text
NVS统计
本地键
NAS键
其他键
```

如果仍不足，不要直接扩大分区或擦除整个 NVS，先确认是否有其他命名空间持续创建历史键。

### 开机后没有本地歌曲

检查：

- `/Music` 是否存在
- 文件扩展名是否为 MP3 / FLAC
- TF 卡是否为 FAT32
- `/System/library/music_index_v3.bin` 是否损坏
- 尝试 `strict` 或 `full` 重扫
- 查看 `/System/reports/music_scan_report.json`

### 关闭串口监视器后设备掉电

确认 PlatformIO 没有通过 DTR/RTS 触发复位，并保留：

```ini
monitor_dtr = 0
monitor_rts = 0
```

---

## 23. 许可证

仓库当前未提供明确的 `LICENSE` 文件。在复制、分发、商用或公开派生版本前，请由项目所有者补充许可证并确认第三方依赖的许可要求。
