#include <Arduino.h>
#include <SPI.h>

#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>

#include "board/board_pins.h"
#include "board/board_spi.h"
#include "nfc/nfc.h"
#include "ui/ui.h"

// RC522 片选脚
static MFRC522DriverPinSimple g_rc522_ss(PIN_RC522_CS);

// 显式绑定到 Arduino 全局 SPI，并指定 4MHz / MODE0
static MFRC522DriverSPI g_rc522_driver(
    g_rc522_ss,
    SPI,
    SPISettings(4000000u, MSBFIRST, SPI_MODE0)
);

// MFRC522 实例
static MFRC522 g_mfrc522(g_rc522_driver);

// 轮询/去重状态
static uint32_t s_last_poll_ms = 0;
static uint32_t s_last_uid_ms = 0;
static String   s_last_uid;
static String   s_pending_uid;
static String   s_pending_card_type;
static bool     s_has_pending_uid = false;

static bool     s_uid_active = false;
static String   s_active_uid;
static uint32_t s_last_seen_ms = 0;

// 忽略指定 UID 的冷却期
static String   s_ignore_uid;
static uint32_t s_ignore_uid_until = 0;

static String format_uid_hex(const MFRC522::Uid& uid)
{
    String out;
    for (byte i = 0; i < uid.size; ++i) {
        if (i) out += ":";
        if (uid.uidByte[i] < 0x10) out += "0";
        out += String(uid.uidByte[i], HEX);
    }
    out.toUpperCase();
    return out;
}

static String format_card_type(uint8_t sak, byte uid_size)
{
    // RC522 读到的 SAK 能粗略判断常见 13.56MHz 卡类型。
    // 不同卡厂可能有差异，所以未知类型保留 SAK/UID 长度，方便排查。
    switch (sak) {
        case 0x00:
            return (uid_size >= 7) ? String("NTAG/Ultralight") : String("MIFARE? SAK00");
        case 0x08:
            return String("MIFARE 1K");
        case 0x09:
            return String("MIFARE Mini");
        case 0x18:
            return String("MIFARE 4K");
        case 0x10:
            return String("MIFARE Plus 2K");
        case 0x11:
            return String("MIFARE Plus 4K");
        case 0x20:
            return String("ISO14443-4");
        case 0x28:
            return String("DESFire");
        default: {
            char buf[24];
            snprintf(buf, sizeof(buf), "SAK%02X UID%u", sak, (unsigned)uid_size);
            return String(buf);
        }
    }
}

void nfc_init(void)
{
    Serial.println("[NFC] 初始化...");

    // board_spi_init() 已经把全局 SPI 绑到 UI SPI 那组引脚
    // 这里只做 RC522 初始化
    board_spi_ui_lock();

    bool ok = g_mfrc522.PCD_Init();
    uint8_t ver = g_rc522_driver.PCD_ReadRegister(MFRC522::PCD_Register::VersionReg);

    if (ok) {
        g_mfrc522.PCD_AntennaOn();
        g_mfrc522.PCD_SetAntennaGain(MFRC522::PCD_RxGain::RxGain_max);
    }

    board_spi_ui_unlock();

    Serial.printf("[NFC] RC522 初始化=%s\n", ok ? "成功" : "失败");
    Serial.printf("[NFC] MFRC522 版本寄存器：0x%02X\n", ver);

    switch (ver) {
        case 0x88:
            Serial.println("[NFC] 版本识别：FM17522?");
            break;
        case 0x90:
            Serial.println("[NFC] 版本识别：v0.0");
            break;
        case 0x91:
            Serial.println("[NFC] 版本识别：v1.0");
            break;
        case 0x92:
            Serial.println("[NFC] 版本识别：v2.0");
            break;
        case 0x00:
            Serial.println("[NFC] 无响应");
            break;
        case 0xFF:
            Serial.println("[NFC] 通信失败");
            break;
        default:
            Serial.println("[NFC] 未知版本");
            break;
    }

    Serial.println("[NFC] 就绪");
}

void nfc_poll(void)
{
    const uint32_t now = millis();
    if (now - s_last_poll_ms < 60) return;
    s_last_poll_ms = now;

    bool got_card = false;
    String uid_str;
    String card_type;

    board_spi_ui_lock();

    if (g_mfrc522.PICC_IsNewCardPresent() && g_mfrc522.PICC_ReadCardSerial()) {
        uid_str = format_uid_hex(g_mfrc522.uid);
        card_type = format_card_type(g_mfrc522.uid.sak, g_mfrc522.uid.size);
        got_card = true;

        g_mfrc522.PICC_HaltA();
        g_mfrc522.PCD_StopCrypto1();
    }

    board_spi_ui_unlock();

    // 卡片离开一段时间后，才允许再次触发
    if (!got_card) {
        if (s_uid_active && (now - s_last_seen_ms > 500)) {
            s_uid_active = false;
            s_active_uid = "";
        }
        return;
    }

    s_last_seen_ms = now;

    if (s_ignore_uid_until != 0 && now > s_ignore_uid_until) {
        s_ignore_uid = "";
        s_ignore_uid_until = 0;
    }

    if (!s_ignore_uid.isEmpty() && now <= s_ignore_uid_until && uid_str == s_ignore_uid) {
        s_uid_active = true;
        s_active_uid = uid_str;
        return;
    }

    // 同一张卡仍在场，不重复上报
    if (s_uid_active && uid_str == s_active_uid) {
        return;
    }

    // 激活当前卡
    s_uid_active = true;
    s_active_uid = uid_str;

    // 再保留一层短时间去重，防毛刺
    if (uid_str == s_last_uid && (now - s_last_uid_ms) < 300) {
        return;
    }

    s_last_uid = uid_str;
    s_last_uid_ms = now;
    s_pending_uid = uid_str;
    s_pending_card_type = card_type;
    s_has_pending_uid = true;

    Serial.printf("[NFC] 识别到 UID=%s 类型=%s\n", uid_str.c_str(), card_type.c_str());
}

bool nfc_is_uid_present(const String& uid)
{
    bool present = false;
    String cur;

    board_spi_ui_lock();

    byte atqa[2];
    byte atqa_len = sizeof(atqa);

    // 用 WUPA 唤醒场上的卡，而不是用 IsNewCardPresent 判断"新卡"
    MFRC522::StatusCode st = g_mfrc522.PICC_WakeupA(atqa, &atqa_len);
    if (st == MFRC522Constants::StatusCode::STATUS_OK) {
        if (g_mfrc522.PICC_ReadCardSerial()) {
            cur = format_uid_hex(g_mfrc522.uid);
            present = (cur == uid);
        }
        g_mfrc522.PICC_HaltA();
        g_mfrc522.PCD_StopCrypto1();
    }

    board_spi_ui_unlock();
    return present;
}

bool nfc_take_last_card_info(String& out_uid, String& out_card_type)
{
    if (!s_has_pending_uid) return false;
    out_uid = s_pending_uid;
    out_card_type = s_pending_card_type;
    s_pending_uid = "";
    s_pending_card_type = "";
    s_has_pending_uid = false;
    return true;
}

bool nfc_take_last_uid(String& out_uid)
{
    String ignored_type;
    return nfc_take_last_card_info(out_uid, ignored_type);
}


void nfc_ignore_uid_once(const String& uid, uint32_t ms)
{
    s_ignore_uid = uid;
    s_ignore_uid.trim();
    s_ignore_uid.toUpperCase();
    s_ignore_uid_until = millis() + ms;
}
