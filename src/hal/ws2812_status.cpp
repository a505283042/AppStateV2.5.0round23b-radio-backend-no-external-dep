#include "hal/ws2812_status.h"

#include <Arduino.h>
#include <driver/rmt.h>

#include "app_flags.h"
#include "board/board_pins.h"
#include "player_control.h"
#include "player_source.h"
#include "utils/log.h"
#include "web/web_settings.h"

namespace {

constexpr rmt_channel_t WS2812_RMT_CHANNEL = RMT_CHANNEL_0;
constexpr uint8_t WS2812_RMT_CLK_DIV = 2;  // 80MHz / 2 = 40MHz，单 tick 25ns。
constexpr uint32_t WS2812_UPDATE_MS = 40;  // 25 FPS。
constexpr uint8_t WS2812_BITS = 24;

// WS2812 典型时序，单位为 25ns tick。
constexpr uint16_t T0H = 14; // 350ns
constexpr uint16_t T0L = 36; // 900ns
constexpr uint16_t T1H = 30; // 750ns
constexpr uint16_t T1L = 20; // 500ns

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

enum class LedCategory : uint8_t {
    All = 0,
    Artist,
    Album,
    Nas,
    Radio,
};

bool s_ready = false;
uint32_t s_last_tick_ms = 0;
Rgb s_current{};
Rgb s_last_sent{};
uint32_t s_last_signature = UINT32_MAX;

uint8_t approach_u8(uint8_t current, uint8_t target, uint8_t step)
{
    if (current < target) {
        const uint16_t next = static_cast<uint16_t>(current) + step;
        return static_cast<uint8_t>(next > target ? target : next);
    }
    if (current > target) {
        return static_cast<uint8_t>(current - target > step ? current - step : target);
    }
    return current;
}

bool rgb_equal(const Rgb& a, const Rgb& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

LedCategory current_category(PlayerSourceType source, play_mode_t mode)
{
    if (source == PlayerSourceType::NET_TRACK) {
        return LedCategory::Nas;
    }
    if (source == PlayerSourceType::NET_RADIO) {
        return LedCategory::Radio;
    }

    switch (mode) {
        case PLAY_MODE_ARTIST_SEQ:
        case PLAY_MODE_ARTIST_RND:
            return LedCategory::Artist;
        case PLAY_MODE_ALBUM_SEQ:
        case PLAY_MODE_ALBUM_RND:
            return LedCategory::Album;
        case PLAY_MODE_ALL_SEQ:
        case PLAY_MODE_ALL_RND:
        default:
            return LedCategory::All;
    }
}

const char* category_label(LedCategory category)
{
    switch (category) {
        case LedCategory::Artist: return "歌手";
        case LedCategory::Album:  return "专辑";
        case LedCategory::Nas:    return "NAS";
        case LedCategory::Radio:  return "网络电台";
        case LedCategory::All:
        default:                  return "全部";
    }
}

Rgb category_color(LedCategory category)
{
    Rgb color{};
    switch (category) {
        case LedCategory::Artist:
            color.r = 176; color.g = 68;  color.b = 255;
            break;
        case LedCategory::Album:
            color.r = 255; color.g = 138; color.b = 32;
            break;
        case LedCategory::Nas:
            color.r = 0;   color.g = 200; color.b = 208;
            break;
        case LedCategory::Radio:
            color.r = 255; color.g = 64;  color.b = 48;
            break;
        case LedCategory::All:
        default:
            color.r = 40;  color.g = 120; color.b = 255;
            break;
    }
    return color;
}

uint8_t sequential_factor(uint32_t now, LedCategory category)
{
    if (category == LedCategory::Radio) {
        // 网络电台顺序模式使用规律双心跳，和普通歌曲呼吸区分。
        const uint32_t phase = now % 1400U;
        if (phase < 90U) return static_cast<uint8_t>(90U + phase * 165U / 90U);
        if (phase < 190U) return static_cast<uint8_t>(255U - (phase - 90U) * 170U / 100U);
        if (phase < 270U) return static_cast<uint8_t>(85U + (phase - 190U) * 135U / 80U);
        if (phase < 390U) return static_cast<uint8_t>(220U - (phase - 270U) * 150U / 120U);
        return 70;
    }

    // 本地全部/歌手/专辑和 NAS 顺序模式：2 秒规律呼吸。
    const uint32_t phase = now % 2000U;
    const uint32_t triangle = phase < 1000U ? phase : 2000U - phase;
    return static_cast<uint8_t>(70U + triangle * 185U / 1000U);
}

uint8_t random_factor(uint32_t now)
{
    // 固定但不等间隔的双脉冲序列，视觉上表示随机模式且不依赖动态内存或随机数状态。
    static const uint16_t periods[] = {920, 1270, 760, 1480};
    const uint32_t coarse = now / 4000U;
    const uint16_t period = periods[coarse % 4U];
    const uint32_t phase = now % period;

    if (phase < 70U) return 255;
    if (phase < 150U) return static_cast<uint8_t>(255U - (phase - 70U) * 175U / 80U);
    if (phase >= 230U && phase < 300U) return 205;
    if (phase >= 300U && phase < 380U) return static_cast<uint8_t>(205U - (phase - 300U) * 135U / 80U);
    return 55;
}

Rgb scaled_color(const Rgb& base, uint8_t brightness, uint8_t factor)
{
    Rgb out{};
    const uint32_t denominator = 255U * 255U;
    out.r = static_cast<uint8_t>(static_cast<uint32_t>(base.r) * brightness * factor / denominator);
    out.g = static_cast<uint8_t>(static_cast<uint32_t>(base.g) * brightness * factor / denominator);
    out.b = static_cast<uint8_t>(static_cast<uint32_t>(base.b) * brightness * factor / denominator);
    return out;
}

bool send_rgb(const Rgb& color)
{
    if (!s_ready) {
        return false;
    }

    const uint8_t bytes[3] = {color.g, color.r, color.b}; // WS2812 使用 GRB 顺序。
    rmt_item32_t items[WS2812_BITS]{};
    uint8_t item_index = 0;

    for (uint8_t byte_index = 0; byte_index < 3; ++byte_index) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool one = (bytes[byte_index] & (1U << bit)) != 0;
            rmt_item32_t& item = items[item_index++];
            item.level0 = 1;
            item.duration0 = one ? T1H : T0H;
            item.level1 = 0;
            item.duration1 = one ? T1L : T0L;
        }
    }

    const esp_err_t err = rmt_write_items(WS2812_RMT_CHANNEL, items, WS2812_BITS, true);
    if (err != ESP_OK) {
        LOGW("[WS2812] 发送失败 err=%d", static_cast<int>(err));
        return false;
    }

    // 数据结束后保持低电平超过 50us，完成 WS2812 latch。
    delayMicroseconds(80);
    s_last_sent = color;
    return true;
}

uint32_t effect_signature(PlayerPlaybackState state,
                          LedCategory category,
                          bool random,
                          bool enabled,
                          StatusLedBrightness brightness)
{
    return static_cast<uint32_t>(state)
         | (static_cast<uint32_t>(category) << 4U)
         | (static_cast<uint32_t>(random ? 1U : 0U) << 8U)
         | (static_cast<uint32_t>(enabled ? 1U : 0U) << 9U)
         | (static_cast<uint32_t>(brightness) << 10U);
}

void log_effect_change(PlayerPlaybackState state, LedCategory category, bool random)
{
    const char* state_text = "停止";
    if (state == PlayerPlaybackState::Playing) state_text = "播放";
    else if (state == PlayerPlaybackState::Paused) state_text = "暂停";

    LOGI("[WS2812] 灯效=%s/%s/%s",
         category_label(category),
         random ? "随机" : "顺序",
         state_text);
}

} // namespace

bool ws2812_status_begin()
{
    if (s_ready) {
        return true;
    }

    rmt_config_t config{};
    config.rmt_mode = RMT_MODE_TX;
    config.channel = WS2812_RMT_CHANNEL;
    config.gpio_num = static_cast<gpio_num_t>(PIN_WS2812);
    config.clk_div = WS2812_RMT_CLK_DIV;
    config.mem_block_num = 1;
    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false;
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

    esp_err_t err = rmt_config(&config);
    if (err == ESP_OK) {
        err = rmt_driver_install(WS2812_RMT_CHANNEL, 0, 0);
    }
    if (err != ESP_OK) {
        pinMode(PIN_WS2812, OUTPUT);
        digitalWrite(PIN_WS2812, LOW);
        LOGE("[WS2812] RMT 初始化失败 GPIO%d err=%d", PIN_WS2812, static_cast<int>(err));
        return false;
    }

    s_ready = true;
    s_last_tick_ms = 0;
    s_current = Rgb{};
    s_last_sent = Rgb{};
    s_last_signature = UINT32_MAX;
    (void)send_rgb(Rgb{});

    LOGI("[WS2812] 初始化成功 GPIO%d RMT通道=%d 刷新=%ums",
         PIN_WS2812,
         static_cast<int>(WS2812_RMT_CHANNEL),
         static_cast<unsigned>(WS2812_UPDATE_MS));
    return true;
}

void ws2812_status_tick()
{
    if (!s_ready || !web_settings_get().status_led_enabled) {
        if (s_ready && !rgb_equal(s_last_sent, Rgb{})) {
            ws2812_status_off();
        }
        return;
    }

    const uint32_t now = millis();
    if (s_last_tick_ms != 0 && static_cast<uint32_t>(now - s_last_tick_ms) < WS2812_UPDATE_MS) {
        return;
    }
    s_last_tick_ms = now;

    const PlayerPlaybackState state = player_playback_state_get();
    const PlayerSourceType source = player_source_type_get();
    const play_mode_t mode = static_cast<play_mode_t>(g_play_mode);
    const bool random = control_mode_is_random(mode);
    const LedCategory category = current_category(source, mode);
    const WebRuntimeSettings settings = web_settings_get();

    uint8_t factor = 0;
    if (state == PlayerPlaybackState::Playing) {
        factor = random ? random_factor(now) : sequential_factor(now, category);
    } else if (state == PlayerPlaybackState::Paused) {
        if (random) {
            // 随机暂停：低亮常驻，每 3 秒短闪一次。
            factor = (now % 3000U) < 90U ? 105 : 32;
        } else {
            // 顺序暂停：低亮常亮。
            factor = 42;
        }
    }

    const uint8_t brightness = status_led_brightness_value(settings.status_led_brightness);
    const Rgb target = state == PlayerPlaybackState::Stopped
        ? Rgb{}
        : scaled_color(category_color(category), brightness, factor);

    // 每 40ms 平滑靠近目标，播放/暂停切换不会突兀闪变。
    s_current.r = approach_u8(s_current.r, target.r, 5);
    s_current.g = approach_u8(s_current.g, target.g, 5);
    s_current.b = approach_u8(s_current.b, target.b, 5);

    const uint32_t signature = effect_signature(state,
                                                category,
                                                random,
                                                settings.status_led_enabled,
                                                settings.status_led_brightness);
    if (signature != s_last_signature) {
        s_last_signature = signature;
        log_effect_change(state, category, random);
    }

    if (!rgb_equal(s_current, s_last_sent)) {
        (void)send_rgb(s_current);
    }
}

void ws2812_status_off()
{
    s_current = Rgb{};
    s_last_signature = UINT32_MAX;
    if (s_ready) {
        (void)send_rgb(Rgb{});
    } else {
        pinMode(PIN_WS2812, OUTPUT);
        digitalWrite(PIN_WS2812, LOW);
    }
}

void ws2812_status_force_refresh()
{
    s_last_signature = UINT32_MAX;
    s_last_tick_ms = 0;
}
