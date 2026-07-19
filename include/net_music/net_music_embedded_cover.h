#pragma once

#include <Arduino.h>

#include "storage/storage_types_v3.h"

/**
 * @brief 启动 NAS/HTTP MP3 或 FLAC 的后台歌词与内嵌封面加载任务。
 *
 * 常驻任务先尝试下载同目录同名 .lrc/.LRC；MP3 通过 HTTP Range 解析
 * ID3 APIC，FLAC 通过 HTTP Range 解析 PICTURE metadata block。
 * 请求队列长度固定为1，只保留最新曲目；快速切歌时旧任务会在检查点
 * 取消，不会重复创建任务栈。
 */
void net_music_embedded_cover_start(int net_track_idx,
                                    const String& media_url,
                                    const String& media_format);

/** @brief 使当前和待处理请求失效；常驻任务本身保留并等待下一次请求。 */
void net_music_embedded_cover_cancel();

/**
 * @brief 查询当前 NAS 曲目的内嵌封面运行时状态。
 *
 * 成功返回 true 表示当前曲目已经解析并生成网页封面缓存。
 * cover_source 用于区分 MP3 APIC 与 FLAC PICTURE。
 */
bool net_music_embedded_cover_get_current(int net_track_idx,
                                          const String& media_url,
                                          CoverSource* out_cover_source,
                                          uint32_t* out_offset,
                                          uint32_t* out_size,
                                          String* out_rev);
