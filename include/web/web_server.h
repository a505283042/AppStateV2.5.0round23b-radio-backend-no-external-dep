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
void web_server_poll();
bool web_server_started();
bool web_server_ready();
