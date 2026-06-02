#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/storage_types_v3.h"
#include "ui/gc9a01_lgfx.h"

bool web_cover_cache_has(int track_idx,
                         CoverSource cover_source,
                         const char* audio_path,
                         const char* cover_path,
                         uint32_t cover_offset,
                         uint32_t cover_size);

bool web_cover_cache_copy_bmp(int track_idx,
                              CoverSource cover_source,
                              const char* audio_path,
                              const char* cover_path,
                              uint32_t cover_offset,
                              uint32_t cover_size,
                              uint8_t** out_buf,
                              size_t* out_len);

bool web_cover_cache_store_from_sprite(int track_idx,
                                       CoverSource cover_source,
                                       const char* audio_path,
                                       const char* cover_path,
                                       uint32_t cover_offset,
                                       uint32_t cover_size,
                                       LGFX_Sprite& spr);

void web_cover_cache_clear();