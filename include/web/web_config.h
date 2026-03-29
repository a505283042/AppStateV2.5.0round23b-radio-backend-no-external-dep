#pragma once

/**
 * @brief 网页控制模块配置。
 *
 * MVP 阶段支持两种联网方式：
 * 1) 先尝试连接固定 STA Wi-Fi
 * 2) 失败后自动回退到设备热点 AP
 *
 * 这样即使还没配置家里 Wi-Fi，也能先用手机直接连设备热点访问网页。
 */

#ifndef WEBCTRL_ENABLED
#define WEBCTRL_ENABLED 0
#endif

// --- Station 模式：优先从 SD 配置文件读取 Wi‑Fi ---
#ifndef WEBCTRL_WIFI_CONFIG_PATH
#define WEBCTRL_WIFI_CONFIG_PATH "/System/config/wifi.conf"
#endif

#ifndef WEBCTRL_SETTINGS_CONFIG_PATH
#define WEBCTRL_SETTINGS_CONFIG_PATH "/System/config/web_settings.conf"
#endif

#ifndef WEBCTRL_HOSTNAME_DEFAULT
#define WEBCTRL_HOSTNAME_DEFAULT "esp32s3-player"
#endif

#ifndef WEBCTRL_STA_CONNECT_TIMEOUT_MS
#define WEBCTRL_STA_CONNECT_TIMEOUT_MS 10000
#endif

// --- AP 回退模式：STA 连接失败时自动启用 ---
#ifndef WEBCTRL_AP_SSID
#define WEBCTRL_AP_SSID "ESP32S3-Player"
#endif

#ifndef WEBCTRL_AP_PASS
#define WEBCTRL_AP_PASS "12345678"
#endif

#ifndef WEBCTRL_STATUS_POLL_MS
#define WEBCTRL_STATUS_POLL_MS 1000
#endif

// 网页轮询建议值：
// - 常规播放状态保持 1 秒轮询，减少请求频率
// - 扫描中适当加快，便于更及时看到扫描/取消状态
#ifndef WEBCTRL_STATUS_POLL_PLAYING_MS
#define WEBCTRL_STATUS_POLL_PLAYING_MS 1000
#endif

#ifndef WEBCTRL_STATUS_POLL_LYRICS_MS
#define WEBCTRL_STATUS_POLL_LYRICS_MS 1000
#endif

#ifndef WEBCTRL_STATUS_POLL_SCAN_MS
#define WEBCTRL_STATUS_POLL_SCAN_MS 350
#endif

// 歌词切换点如果离下一次轮询不足这个阈值，则直接等轮询结果更新，
// 避免网页本地切词后很快又被轮询覆盖造成闪动。
#ifndef WEBCTRL_LYRIC_WAIT_POLL_THRESHOLD_MS
#define WEBCTRL_LYRIC_WAIT_POLL_THRESHOLD_MS 150
#endif
