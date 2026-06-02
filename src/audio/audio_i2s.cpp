#include <Arduino.h>
#include "audio/audio_i2s.h"
#include "driver/i2s.h"
#include <freertos/FreeRTOS.h>
#include "utils/log.h"
#include "audio/audio_service.h"
#include <string.h>
#include "audio/audio.h"
#include <freertos/semphr.h>

static const i2s_port_t I2S_PORT = I2S_NUM_0;
static bool g_inited = false;

// ===== 播放位置统计（按“已写入 I2S 的帧数”计算）=====
static portMUX_TYPE s_pos_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_frames_played = 0;   // 写入的 stereo frame 数
static int      s_sample_rate   = 44100;

void audio_i2s_reset_play_pos()
{
  portENTER_CRITICAL(&s_pos_mux);
  s_frames_played = 0;
  portEXIT_CRITICAL(&s_pos_mux);
}

uint32_t audio_i2s_get_play_ms()
{
  portENTER_CRITICAL(&s_pos_mux);
  uint64_t f = s_frames_played;
  int sr = s_sample_rate;
  portEXIT_CRITICAL(&s_pos_mux);
  if (sr <= 0) return 0;
  return (uint32_t)((f * 1000ull) / (uint64_t)sr);
}

// ===================== 音量缩放 bounce buffer（内部RAM） =====================
// 注意：这里的单位是 int16 “样本数”（包含左右声道交错），不是 frame
static constexpr size_t VOL_BUF_SAMPLES = 4096;   // 4096 * 2 bytes = 8KB
static int16_t s_vol_buf[VOL_BUF_SAMPLES];

static inline int16_t vol_scale_s16(int16_t x, uint16_t g_q15)
{
  return (int16_t)(((int32_t)x * (int32_t)g_q15 + 16384) >> 15);
}

bool audio_i2s_init(int bck, int ws, int dout, int sample_rate)
{
    if (g_inited) {
        // 只改时钟，不重装驱动
        i2s_set_clk(I2S_PORT, sample_rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
        // 更新采样率变量，确保播放时间计算准确
        portENTER_CRITICAL(&s_pos_mux);
        s_sample_rate = sample_rate;
        portEXIT_CRITICAL(&s_pos_mux);
        return true;
    }

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = sample_rate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S; // ✅ Philips I2S
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 12;       // ✅ 稍大一些
    cfg.dma_buf_len = 512;        // ✅ 稍大一些，减少“咔嗒”
    cfg.use_apll = true;          // ✅ PCM5102A 通常更稳
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = 0;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = bck;
    pins.ws_io_num = ws;
    pins.data_out_num = dout;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;

    // ✅ 关键：清空 DMA，避免开机杂音
    i2s_zero_dma_buffer(I2S_PORT);

    portENTER_CRITICAL(&s_pos_mux);
    s_sample_rate = sample_rate;
    s_frames_played = 0;
    portEXIT_CRITICAL(&s_pos_mux);

    g_inited = true;
    return true;
}

void audio_i2s_deinit()
{
    if (!g_inited) return;
    i2s_driver_uninstall(I2S_PORT);
    g_inited = false;
}

size_t audio_i2s_write_frames(const int16_t* stereo_samples, size_t frames)
{
    if (!g_inited) return SIZE_MAX;
    if (frames == 0) return 0;

    const uint16_t g = audio_get_gain_q15(); // 0..32768
    const size_t total_samples = frames * 2; // L/R 交错的 int16 数
    size_t sample_off = 0;
    size_t bytes_written_total = 0;

    while (sample_off < total_samples) {
        size_t n = total_samples - sample_off;
        if (n > VOL_BUF_SAMPLES) n = VOL_BUF_SAMPLES;

        // 保证按 stereo 对齐（每帧2个样本）
        n &= ~((size_t)1);
        if (n == 0) break;

        const int16_t* src = stereo_samples + sample_off;
        const int16_t* out = src;

        // 100% 直接写，不做任何处理（最稳最快）
        if (g != 32768) {
            if (g == 0) {
                memset(s_vol_buf, 0, n * sizeof(int16_t));
            } else {
                for (size_t i = 0; i < n; ++i) {
                    s_vol_buf[i] = vol_scale_s16(src[i], g);
                }
            }
            out = s_vol_buf;
        }

        const size_t chunk_bytes = n * sizeof(int16_t);
        size_t chunk_written = 0;

        // ✅ 使用 100ms 超时，避免 I2S 硬件异常时死锁
        esp_err_t err = i2s_write(I2S_PORT,
                                  (const char*)out,
                                  chunk_bytes,
                                  &chunk_written,
                                  pdMS_TO_TICKS(100));

        if (err != ESP_OK) {
            LOGE("[I2S] 写入失败: %d", err);
            return SIZE_MAX;
        }

        if (chunk_written == 0) {
            LOGW("[I2S] 写入超时，未写入数据");
            break;
        }

        // 前进实际写入的样本数
        size_t wrote_samples = chunk_written / sizeof(int16_t);
        wrote_samples &= ~((size_t)1); // stereo 对齐
        if (wrote_samples == 0) break;

        sample_off += wrote_samples;
        bytes_written_total += (wrote_samples * sizeof(int16_t));
    }

    // 转回写入的 frame 数（每帧 4 bytes）
    size_t frames_written = bytes_written_total / (2 * sizeof(int16_t));
    portENTER_CRITICAL(&s_pos_mux);
    s_frames_played += frames_written;
    portEXIT_CRITICAL(&s_pos_mux);
    return frames_written;
}

bool audio_i2s_set_sample_rate(int sample_rate)
{
    if (!g_inited) return false;
    esp_err_t err = i2s_set_clk(I2S_PORT, sample_rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_pos_mux);
        s_sample_rate = sample_rate;
        portEXIT_CRITICAL(&s_pos_mux);
    }
    return err == ESP_OK;
}

// 清空 DMA 缓冲区，消除暂停时的底噪
void audio_i2s_zero_dma_buffer()
{
    if (!g_inited) return;
    i2s_zero_dma_buffer(I2S_PORT);
}
