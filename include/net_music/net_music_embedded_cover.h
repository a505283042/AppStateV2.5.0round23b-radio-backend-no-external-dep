#pragma once

#include <Arduino.h>

/**
 * @brief 启动 NAS/HTTP MP3 内嵌封面后台加载任务。
 *
 * 任务会通过 HTTP Range 解析 ID3 APIC 帧并下载图片片段，成功后刷新当前封面。
 * 不会阻塞起播；如果切歌或切换播放源，旧任务结果会被丢弃。
 */
void net_music_embedded_cover_start(int net_track_idx, const String& mp3_url);

/** @brief 使当前/待完成的 NAS 封面任务失效。 */
void net_music_embedded_cover_cancel();
