#pragma once

/**
 * @brief 轻量网页控制服务器（round43 MVP）。
 *
 * 对外只暴露三件事：
 * - start: 启动 Wi-Fi + HTTP server
 * - poll : 在主循环里处理 HTTP 请求
 * - started/ready: 供日志或状态判断
 */

void web_server_start();
void web_server_start_async();
void web_server_poll();
bool web_server_started();
bool web_server_ready();

/**
 * 重新从 TF 卡 /System/config/wifi.conf 读取 WiFi 配置。
 * 用于开机无卡进入 AP 后，插卡时切换到 STA。
 */
bool web_server_retry_sta_from_config();

/**
 * @brief 按 /System/config/wifi.conf 里的网络顺序切换到下一个 WiFi。
 *
 * 当前已连接时，会优先尝试当前 SSID 后面的下一个配置；
 * 只有一个配置时，相当于重新连接当前 WiFi。
 */
bool web_server_switch_wifi_from_config();

bool web_wifi_is_enabled();
void web_wifi_set_enabled(bool enabled);
void web_wifi_toggle();