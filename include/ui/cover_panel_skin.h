#pragma once

#include <Arduino.h>
#include <pgmspace.h>

static constexpr int COVER_PANEL_SKIN_W = 240;
static constexpr int COVER_PANEL_SKIN_H = 140;
static constexpr uint16_t COVER_PANEL_SKIN_KEY = 0xF81F;  // RGB565 for #FF00FF

extern const uint16_t g_cover_panel_skin_240x140[COVER_PANEL_SKIN_W * COVER_PANEL_SKIN_H] PROGMEM;
