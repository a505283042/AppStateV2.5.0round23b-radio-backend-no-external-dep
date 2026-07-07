#include "hal/bt62sp_uart_debug.h"

#include <Arduino.h>

#include "board/board_pins_pcb1_mcp23017.h"
#include "hal/board_hw_control.h"

namespace {

constexpr size_t USB_LINE_MAX = 160;
constexpr size_t BT_RX_LINE_MAX = 256;
constexpr uint32_t BT_RX_PARTIAL_FLUSH_MS = 80;

uint32_t s_bt_baud = 1000000;
bool s_ready = false;

char s_usb_line[USB_LINE_MAX];
size_t s_usb_len = 0;

char s_bt_rx_line[BT_RX_LINE_MAX];
size_t s_bt_rx_len = 0;
uint32_t s_bt_rx_last_ms = 0;

String trim_copy(const char* text)
{
    String s(text ? text : "");
    s.trim();
    return s;
}

String upper_copy(const String& src)
{
    String s(src);
    s.toUpperCase();
    return s;
}

void bt_uart_begin(uint32_t baud)
{
    s_bt_baud = baud;
    Serial1.end();
    delay(10);
    // BT62SP 默认 1000000bps，8N1；RX/TX 为 ESP32 视角。
    Serial1.setRxBufferSize(1024);
    Serial1.begin(s_bt_baud, SERIAL_8N1, board::PIN_UART1_RX, board::PIN_UART1_TX);
    Serial.printf("[BT62SP] UART1 started baud=%lu RX=GPIO%d TX=GPIO%d\n",
                  static_cast<unsigned long>(s_bt_baud),
                  board::PIN_UART1_RX,
                  board::PIN_UART1_TX);
}

void print_help()
{
    Serial.println("[BT62SP] USB串口调试命令：");
    Serial.println("  AT                         -> 直接转发 AT 到 BT62SP");
    Serial.println("  AT+VER? / AT+MODE? / AT+STATUS? / AT+LIST?");
    Serial.println("  BT62 HELP                  -> 显示帮助");
    Serial.println("  BT62 PWR ON|OFF            -> 控制 BT_PWR_EN");
    Serial.println("  BT62 MODE TX|RX            -> TX=发射，RX=接收");
    Serial.println("  BT62 PAIR                  -> 脉冲 PAIR/SW 脚");
    Serial.println("  BT62 LINK?                 -> 读取 BT_LINK 状态");
    Serial.println("  BT62 BAUD 1000000          -> 临时切换 UART1 波特率");
    Serial.println("  BT62 PROBE                 -> 发送常用查询 AT 指令");
}

void send_at_line(const String& cmd)
{
    if (cmd.length() == 0) {
        return;
    }
    Serial1.print(cmd);
    Serial1.print("\r\n");
    Serial.printf("[BT62SP TX] %s\n", cmd.c_str());
}

bool parse_on_off(const String& upper, bool* out)
{
    if (!out) {
        return false;
    }
    if (upper.endsWith(" ON") || upper.endsWith("=ON")) {
        *out = true;
        return true;
    }
    if (upper.endsWith(" OFF") || upper.endsWith("=OFF")) {
        *out = false;
        return true;
    }
    return false;
}

void handle_bt62_command(const String& line)
{
    const String upper = upper_copy(line);

    if (upper == "BT62 HELP" || upper == "BTHELP" || upper == "HELP") {
        print_help();
        return;
    }

    if (upper == "BT62 PROBE" || upper == "BTPROBE") {
        send_at_line("AT");
        send_at_line("AT+VER?");
        send_at_line("AT+MODE?");
        send_at_line("AT+STATUS?");
        send_at_line("AT+LIST?");
        return;
    }

    if (upper == "BT62 PAIR" || upper == "BTPAIR") {
        const bool ok = board_hw_pulse_bt_switch(200);
        Serial.printf("[BT62SP] PAIR/SW 脉冲 %s\n", ok ? "OK" : "FAIL");
        return;
    }

    if (upper == "BT62 LINK?" || upper == "BTLINK?") {
        bool linked = false;
        const bool ok = board_hw_read_bt_link(&linked);
        Serial.printf("[BT62SP] BT_LINK %s%s\n", ok ? "" : "读取失败 ", linked ? "已连接" : "未连接");
        return;
    }

    if (upper.startsWith("BT62 PWR") || upper.startsWith("BTPWR")) {
        bool enabled = false;
        if (!parse_on_off(upper, &enabled)) {
            Serial.println("[BT62SP] 用法：BT62 PWR ON 或 BT62 PWR OFF");
            return;
        }
        const bool ok = board_hw_set_bt_power(enabled);
        Serial.printf("[BT62SP] BT_PWR_EN=%s %s\n", enabled ? "ON" : "OFF", ok ? "OK" : "FAIL");
        return;
    }

    if (upper.startsWith("BT62 MODE") || upper.startsWith("BTMODE")) {
        bool tx = false;
        if (upper.endsWith(" TX") || upper.endsWith("=TX") || upper.endsWith(" TRANSMIT")) {
            tx = true;
        } else if (upper.endsWith(" RX") || upper.endsWith("=RX") || upper.endsWith(" RECEIVE")) {
            tx = false;
        } else {
            Serial.println("[BT62SP] 用法：BT62 MODE TX 或 BT62 MODE RX");
            return;
        }
        const bool ok = board_hw_set_bt_mode(tx);
        Serial.printf("[BT62SP] MODE=%s %s\n", tx ? "TX/发射" : "RX/接收", ok ? "OK" : "FAIL");
        return;
    }

    if (upper.startsWith("BT62 BAUD") || upper.startsWith("BTBAUD")) {
        int sep = line.lastIndexOf(' ');
        if (sep < 0) {
            sep = line.lastIndexOf('=');
        }
        const uint32_t baud = (sep >= 0) ? static_cast<uint32_t>(line.substring(sep + 1).toInt()) : 0;
        if (baud < 9600) {
            Serial.println("[BT62SP] 用法：BT62 BAUD 1000000");
            return;
        }
        bt_uart_begin(baud);
        return;
    }

    if (upper.startsWith("BT62 SEND ")) {
        send_at_line(line.substring(10));
        return;
    }

    Serial.println("[BT62SP] 未识别命令。输入 BT62 HELP 查看用法；AT... 会直接转发。 ");
}

void handle_usb_line(const char* text)
{
    String line = trim_copy(text);
    if (line.length() == 0) {
        return;
    }

    const String upper = upper_copy(line);
    if (upper.startsWith("AT")) {
        send_at_line(line);
        return;
    }

    if (upper.startsWith("BT62") || upper.startsWith("BT")) {
        handle_bt62_command(line);
        return;
    }

    Serial.println("[BT62SP] 只转发 AT...；控制命令请用 BT62 HELP 查看。 ");
}

void flush_bt_rx_line(bool partial)
{
    if (s_bt_rx_len == 0) {
        return;
    }
    s_bt_rx_line[s_bt_rx_len] = '\0';
    Serial.printf(partial ? "[BT62SP RX*] %s\n" : "[BT62SP RX] %s\n", s_bt_rx_line);
    s_bt_rx_len = 0;
}

void append_bt_rx_char(char c)
{
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        flush_bt_rx_line(false);
        return;
    }
    if (s_bt_rx_len + 1 >= BT_RX_LINE_MAX) {
        flush_bt_rx_line(true);
    }
    s_bt_rx_line[s_bt_rx_len++] = c;
    s_bt_rx_last_ms = millis();
}

void append_usb_char(char c)
{
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        s_usb_line[s_usb_len] = '\0';
        handle_usb_line(s_usb_line);
        s_usb_len = 0;
        return;
    }
    if (s_usb_len + 1 >= USB_LINE_MAX) {
        s_usb_line[s_usb_len] = '\0';
        handle_usb_line(s_usb_line);
        s_usb_len = 0;
    }
    s_usb_line[s_usb_len++] = c;
}

}  // namespace

void bt62sp_uart_debug_begin(uint32_t baud)
{
    bt_uart_begin(baud);
    s_ready = true;
    Serial.println("[BT62SP] UART调试桥已启用：电脑串口输入 AT... 直接转发，输入 BT62 HELP 查看控制命令");
}

void bt62sp_uart_debug_update()
{
    if (!s_ready) {
        return;
    }

    uint16_t usb_budget = 128;
    while (Serial.available() > 0 && usb_budget-- > 0) {
        append_usb_char(static_cast<char>(Serial.read()));
    }

    uint16_t bt_budget = 512;
    while (Serial1.available() > 0 && bt_budget-- > 0) {
        append_bt_rx_char(static_cast<char>(Serial1.read()));
    }

    if (s_bt_rx_len > 0 && millis() - s_bt_rx_last_ms >= BT_RX_PARTIAL_FLUSH_MS) {
        flush_bt_rx_line(true);
    }
}
