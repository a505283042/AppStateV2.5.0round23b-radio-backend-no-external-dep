# ESP32-S3 音乐播放器项目当前状态总结

当前项目：ESP32-S3 音乐播放器  
当前分支：`feat/quick-menu`  
本地状态：用户本地编译已通过  
云端工作副本路径：`/mnt/data/esp32s3_music_player_current`

> 说明：本总结基于当前上传的最新项目 zip 解压后的云端副本分析。后续修改将继续基于该云端副本推进，不回退到初始上传包。

---

## 1. 当前项目整体结构

这是一个 PlatformIO + Arduino 框架的 ESP32-S3 项目。

核心入口：

```txt
platformio.ini
src/main.cpp
src/app_state.cpp
src/boot_state.cpp
src/player_state.cpp
```

主要目录结构：

```txt
include/
  audio/        音频接口、I2S、MP3/FLAC、网络流、输出路由
  board/        板级 GPIO / MCP23017 引脚定义
  hal/          MCP23017 与硬件控制层
  keys/         按键与旋钮定义
  menu/         快捷菜单框架与各页面
  nfc/          NFC 绑定与管理
  radio/        网络电台列表
  net_music/    NAS/HTTP 网络歌曲索引
  storage/      TF 卡、曲库扫描、V3 索引
  ui/           GC9A01 显示、播放器 UI、菜单 UI、列表 UI
  web/          Web 控制、WiFi、网页设置
  utils/        日志、运行监控

src/
  audio/
  board/
  hal/
  keys/
  menu/
  meta/
  net_music/
  nfc/
  radio/
  storage/
  ui/
  web/
```

当前代码规模：

```txt
源码/头文件数量：约 163 个
include + src 总行数：约 44901 行
```

---

## 2. 启动与主循环链路

启动入口在：

```txt
src/main.cpp
```

流程：

```cpp
setup()
  -> 先拉高 PIN_POWER_CTRL 保持整机供电
  -> WS2812 拉低，避免上电乱闪
  -> Serial 初始化
  -> app_state_init()

loop()
  -> app_state_update()
```

应用状态机在：

```txt
src/app_state.cpp
include/app_state.h
```

目前状态：

```cpp
STATE_BOOT
STATE_PLAYER
STATE_NFC_ADMIN
```

启动阶段由：

```txt
src/boot_state.cpp
```

完成这些动作：

```txt
初始化 SPI
初始化 TF / storage
加载 NFC 绑定
初始化封面缓冲
初始化 UI
启动 audio_service 音频任务
启动 runtime_monitor
初始化 NFC
加载本地曲库 V3 索引
加载 radio_list.txt 电台列表
读取播放器快照
切到 STATE_PLAYER
异步启动 Web/WiFi
```

---

## 3. 快捷菜单结构

快捷菜单核心文件：

```txt
include/menu/quick_menu.h
include/menu/quick_menu_types.h
src/menu/quick_menu.cpp
src/menu/quick_menu_pages.cpp
src/ui/ui_quick_menu_view.cpp
```

根菜单当前项目：

```cpp
播放控制
播放源
显示设置
网络设置
音频输出
NFC
系统信息
返回
```

页面拆分已经比较清楚：

```txt
src/menu/quick_menu_page_playback.cpp       播放控制
src/menu/quick_menu_page_source.cpp         播放源
src/menu/quick_menu_page_display.cpp        显示设置
src/menu/quick_menu_page_network.cpp        网络设置
src/menu/quick_menu_page_audio_output.cpp   音频输出
src/menu/quick_menu_page_nfc.cpp            NFC
src/menu/quick_menu_page_system.cpp         系统信息 / 内存 / 栈 / 电池
src/menu/quick_menu_page_bluetooth.cpp      蓝牙页面，当前未挂到根菜单
```

注意点：

```txt
QuickMenuPage::Bluetooth 已经存在
src/menu/quick_menu_page_bluetooth.cpp 也存在
但 quick_menu_pages.cpp 里没有 Bluetooth case
根菜单 ROOT_ITEMS 里也没有“蓝牙设置”
```

也就是说，目前蓝牙独立页面是“代码存在，但还没接入菜单树”。不过音频输出页面里已经有蓝牙发射相关控制。

---

## 4. 按键与菜单交互

按键核心文件：

```txt
include/keys/keys_pins.h
src/keys/keys.cpp
```

当前 PCB1 按键分配：

```cpp
MODE       -> MCP23017 A2
EC06_E     -> MCP23017 A3
PREV/NFC   -> MCP23017 A6
NEXT/LIST  -> MCP23017 A7
PLAY       -> ESP32 GPIO48
EC06_A/B   -> ESP32 GPIO39 / GPIO38
```

当前正常播放页逻辑：

```txt
旋钮旋转       调音量
旋钮按下       进入快捷菜单
MODE 短按      切换 1 / 5 大步音量模式
PLAY 短按      播放 / 暂停
PLAY 长按      保存状态并关机
PREV 短按      上一首
PREV 长按      NFC 管理
NEXT 短按      下一首
NEXT 长按      进入列表选择 / 大步跳转
```

快捷菜单内逻辑：

```txt
旋钮旋转       上下移动
旋钮按下       确认
PLAY 短按      确认
MODE 短按      返回
MODE 长按      退出菜单
PREV 短按      上移
NEXT 短按      下移
```

这个结构后续继续扩展菜单比较方便。

---

## 5. 硬件控制层

硬件控制主要在：

```txt
include/board/board_pins_pcb1_mcp23017.h
include/hal/board_hw_control.h
src/hal/board_hw_control.cpp
include/hal/mcp23017_u3.h
src/hal/mcp23017_u3.cpp
```

当前已经封装的硬件控制：

```txt
BAT_ADC 电池采样
BQ25606 PG / CHG 状态读取
BT_PWR_EN 蓝牙电源
BT_WKP_CTRL 蓝牙唤醒
BT_SW_CTRL 蓝牙按键模拟
MUTE_EN 功放静音
SHDN_EN 功放关断
BLK 屏幕背光
POWER_CTRL 整机电源保持 / 关机释放
```

电池策略已经做过优化：

```txt
上电初期多次采样
稳定后 1 分钟采样一次
PG / CHG 每 1 秒刷新一次
UI 读取缓存，不直接高频读 ADC
```

后续如果要继续改“电池更准”，主要会动：

```txt
src/hal/board_hw_control.cpp
src/menu/quick_menu_page_system.cpp
src/ui/ui_player_render.cpp 或相关电池图标绘制文件
```

---

## 6. 音频输出路由

音频输出路由文件：

```txt
include/audio/audio_output_route.h
src/audio/audio_output_route.cpp
src/menu/quick_menu_page_audio_output.cpp
src/audio/audio_service.cpp
```

当前输出模式：

```cpp
AudioOutputRoute::HeadphoneOnly   仅耳机
AudioOutputRoute::Speaker         耳机 + 功放
AudioOutputRoute::BluetoothTx     耳机 + 蓝牙发射
```

当前逻辑：

```txt
仅耳机：
  关闭蓝牙
  功放静音 + 关断

耳机 + 功放：
  关闭蓝牙
  释放功放关断
  取消功放静音

耳机 + 蓝牙：
  打开蓝牙
  功放静音 + 关断
```

音频任务里也做了功放保护：

```txt
I2S 初始化后先静音
延迟后释放 SHDN
再等模拟链路稳定
真正播放后再取消静音
```

这个结构比较适合继续做：

```txt
输出路由 NVS 记忆
开机恢复上次输出模式
蓝牙模块配对状态查询
蓝牙发射 / 接收模式区分
功放爆音继续优化
```

---

## 7. WiFi / Web / NAS / 电台结构

Web 和 WiFi 核心文件：

```txt
include/web/web_server.h
src/web/web_server.cpp
include/web/web_settings.h
src/web/web_settings.cpp
src/web/web_page.h
```

WiFi 开关已经保存到 NVS：

```cpp
WebRuntimeSettings::wifi_enabled
```

相关接口：

```cpp
web_wifi_is_enabled()
web_wifi_set_enabled(bool enabled)
web_wifi_toggle()
web_server_retry_sta_from_config()
```

Web/WiFi 是开机后异步启动：

```cpp
web_server_start_async()
```

这点是好的，可以避免开机 WiFi 拖慢进入播放器。

网络电台文件：

```txt
include/radio/radio_catalog.h
src/radio/radio_catalog.cpp
include/audio/audio_radio_backend.h
src/audio/audio_radio_backend.cpp
```

电台列表来自：

```txt
/System/radio_list.txt
```

NAS/HTTP 网络歌曲文件：

```txt
include/net_music/net_music_catalog.h
src/net_music/net_music_catalog.cpp
```

NAS 文件依赖：

```txt
/System/net_music_base.txt
/System/net_music.txt
```

当前 NAS/HTTP 歌曲只支持 MP3：

```cpp
item.format == "mp3"
```

播放走：

```txt
player_control.cpp
  -> player_play_net_track_index()
  -> audio_service_play_stream_mp3()
  -> audio_mp3_source_audiotools.cpp
```

---

## 8. 当前明显已完成的功能

当前已完成度较高的功能：

```txt
快捷菜单框架
菜单 UI 渲染
播放控制页
显示设置页
网络设置页
系统信息页
电池状态页
音频输出页
Web 控制
WiFi NVS 开关
网络电台播放
NAS/HTTP MP3 播放基础链路
本地曲库 V3 索引
TF 热插拔处理
NFC 管理状态入口
```

---

## 9. 当前待继续完善的位置

还处于占位或可继续完善的地方：

```txt
src/menu/quick_menu_page_source.cpp
  播放源页面目前是占位：本地音乐 / 网络电台 / NAS音乐 都是待接入

src/menu/quick_menu_page_nfc.cpp
  NFC 菜单页多数项目还是待接入 / 占位

src/menu/quick_menu_page_system.cpp
  恢复出厂确认页还是待接入

src/menu/quick_menu_page_bluetooth.cpp
  蓝牙独立页面存在，但没挂到根菜单

src/player_control.cpp
  NAS 歌曲第一版使用默认封面，网络封面和歌词暂未接入

src/net_music/net_music_catalog.cpp
  NAS/HTTP 当前主要支持 MP3，不支持 FLAC / AAC 等网络格式

src/audio/audio_mp3_source_audiotools.cpp
  HTTP 流目前重点是 http，https 支持可能还不完整
```

---

## 10. 后续修改约定

后续每一轮修改保持主题集中，例如：

```txt
一轮只做播放源菜单接入
一轮只做蓝牙输出记忆
一轮只做 NAS 播放增强
一轮只做电池显示校准
一轮只做功放/蓝牙硬件控制优化
```

每轮输出格式：

```txt
本轮目标：

本轮修改文件：
1. xxx.cpp
2. xxx.h

你本地手动修改步骤：
文件：xxx.cpp
位置：某个函数 / 某个菜单项附近
操作：替换 / 插入 / 删除
代码：可直接复制粘贴

云端副本同步状态：
已同步到 /mnt/data/esp32s3_music_player_current

本轮建议测试：
platformio run

实机测试菜单路径：
快捷菜单 -> xxx -> xxx
```

每轮修改后需要输出：

```txt
修改了哪些文件
每个文件改了什么
插入/替换位置
可复制粘贴代码
云端副本是否已同步
建议本地编译和实机测试路径
```

---

## 11. 当前云端副本状态

```txt
路径：/mnt/data/esp32s3_music_player_current
分支：feat/quick-menu
基线：已建立
源码改动：无
```

本轮只做了项目结构分析和总结文件生成，没有修改项目源码。

```txt
本轮源码修改文件：
无
```
