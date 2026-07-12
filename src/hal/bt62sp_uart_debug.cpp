#include "hal/bt62sp_uart_debug.h"

#include <Arduino.h>

#include "board/board_pins_pcb1_mcp23017.h"
#include "hal/board_hw_control.h"

namespace {

constexpr size_t USB_LINE_MAX = 160;
constexpr size_t BT_RX_LINE_MAX = 256;
constexpr uint32_t BT_RX_PARTIAL_FLUSH_MS = 80;

// 模块上电查询可能撞上启动输出，因此使用非阻塞多次重试并结合 UART 启动信息重排查询。
constexpr uint32_t BT_VOLUME_QUERY_RESPONSE_TIMEOUT_MS = 1500;
constexpr uint32_t BT_VOLUME_QUERY_RETRY_DELAY_MS = 500;
constexpr uint32_t BT_VOLUME_QUERY_AFTER_RX_DELAY_MS = 300;
constexpr uint8_t BT_VOLUME_QUERY_MAX_ATTEMPTS = 4;

enum class VolumeQueryPhase : uint8_t {
    Idle = 0,
    WaitBeforeSend,
    WaitResponse,
};

enum class PendingVolumeTx : uint8_t {
    None = 0,
    Query,
    Set,
};

uint32_t s_bt_baud = 1000000;
bool s_ready = false;

char s_usb_line[USB_LINE_MAX];
size_t s_usb_len = 0;

char s_bt_rx_line[BT_RX_LINE_MAX];
size_t s_bt_rx_len = 0;
uint32_t s_bt_rx_last_ms = 0;

// AudioTask 只提交请求、读取结果；Serial1 的实际收发仍统一在 loopTask 的 update() 中执行。
portMUX_TYPE s_volume_mux = portMUX_INITIALIZER_UNLOCKED;
VolumeQueryPhase s_volume_query_phase = VolumeQueryPhase::Idle;
uint32_t s_volume_query_request_id = 0;
uint32_t s_next_volume_query_request_id = 1;
uint32_t s_volume_query_due_ms = 0;
uint32_t s_volume_query_sent_ms = 0;
uint8_t s_volume_query_attempts = 0;

bool s_volume_query_event_pending = false;
Bt62spVolumeQueryEvent s_volume_query_event{};

bool s_pending_set_volume_valid = false;
uint8_t s_pending_set_volume = 0;

// BT62SP 在音频运行态初始化完成后会输出 CLEAR OK。
// 使用递增序号而不是简单布尔值，便于识别每一次新的启动完成事件。
uint32_t s_module_ready_generation = 0;

bool time_reached(uint32_t now, uint32_t target)
{
    return static_cast<int32_t>(now - target) >= 0;
}

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

void clear_volume_query_locked()
{
    s_volume_query_phase = VolumeQueryPhase::Idle;
    s_volume_query_request_id = 0;
    s_volume_query_due_ms = 0;
    s_volume_query_sent_ms = 0;
    s_volume_query_attempts = 0;
}

void publish_volume_query_event_locked(Bt62spVolumeQueryResult result,
                                       uint32_t request_id,
                                       uint8_t volume)
{
    s_volume_query_event.result = result;
    s_volume_query_event.request_id = request_id;
    s_volume_query_event.volume = volume;
    s_volume_query_event_pending = true;
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
    Serial.println("  AT+VER? / AT+MODE? / AT+STATUS? / AT+LIST? / AT+VOL?");
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

bool is_module_ready_line(const char* text)
{
    if (!text || !*text) {
        return false;
    }

    String upper = trim_copy(text);
    upper.toUpperCase();

    // 实机启动完成日志为 CLEAR OK；同时兼容后续固件可能使用的 READY 文本。
    return upper.indexOf("CLEAR OK") >= 0 ||
           upper == "READY" ||
           upper.indexOf("BT READY") >= 0;
}

bool parse_volume_response(const char* text, uint8_t* out_volume)
{
    if (!text || !out_volume) {
        return false;
    }

    String upper = trim_copy(text);
    upper.toUpperCase();

    const int marker = upper.indexOf("VOL:");
    if (marker < 0) {
        return false;
    }

    int pos = marker + 4;
    while (pos < static_cast<int>(upper.length()) && upper[pos] == ' ') {
        ++pos;
    }

    int value = 0;
    bool has_digit = false;
    while (pos < static_cast<int>(upper.length())) {
        const char c = upper[pos];
        if (c < '0' || c > '9') {
            break;
        }
        has_digit = true;
        value = value * 10 + (c - '0');
        if (value > 100) {
            return false;
        }
        ++pos;
    }

    if (!has_digit || value < 0 || value > 100) {
        return false;
    }

    *out_volume = static_cast<uint8_t>(value);
    return true;
}

void note_module_rx_activity_for_volume_query(const char* text)
{
    if (!text || !*text) return;

    const bool module_ready = is_module_ready_line(text);
    if (!module_ready) {
        return;
    }

    const uint32_t now = millis();
    uint32_t generation = 0;

    portENTER_CRITICAL(&s_volume_mux);

    ++s_module_ready_generation;
    if (s_module_ready_generation == 0) {
        s_module_ready_generation = 1;
    }
    generation = s_module_ready_generation;

    // 只有明确收到启动完成标志后才重排查询。
    // 普通 VOL/STATUS 等响应不能被当作模块已经完成音频运行态初始化。
    if (s_volume_query_request_id != 0) {
        if (s_volume_query_phase == VolumeQueryPhase::WaitBeforeSend) {
            s_volume_query_due_ms = now + BT_VOLUME_QUERY_AFTER_RX_DELAY_MS;
        } else if (s_volume_query_phase == VolumeQueryPhase::WaitResponse &&
                   s_volume_query_attempts < BT_VOLUME_QUERY_MAX_ATTEMPTS) {
            s_volume_query_phase = VolumeQueryPhase::WaitBeforeSend;
            s_volume_query_due_ms = now + BT_VOLUME_QUERY_AFTER_RX_DELAY_MS;
        }
    }

    portEXIT_CRITICAL(&s_volume_mux);

    Serial.printf("[BT62SP] 模块启动完成：代次=%lu\n",
                  static_cast<unsigned long>(generation));
}

void handle_volume_response_line(const char* text)
{
    uint8_t volume = 0;
    if (!parse_volume_response(text, &volume)) {
        return;
    }

    uint32_t request_id = 0;
    bool accepted = false;

    portENTER_CRITICAL(&s_volume_mux);
    // 第一次查询超时后、准备重试期间也接受迟到响应；尚未发送过查询时不接收旧串口残留。
    const bool waiting_response = s_volume_query_phase == VolumeQueryPhase::WaitResponse;
    const bool waiting_retry = s_volume_query_phase == VolumeQueryPhase::WaitBeforeSend &&
                               s_volume_query_attempts > 0;
    if ((waiting_response || waiting_retry) && s_volume_query_request_id != 0) {
        request_id = s_volume_query_request_id;
        publish_volume_query_event_locked(Bt62spVolumeQueryResult::Success,
                                          request_id,
                                          volume);
        clear_volume_query_locked();
        accepted = true;
    }
    portEXIT_CRITICAL(&s_volume_mux);

    if (accepted) {
        Serial.printf("[BT62SP] 音量查询完成：请求=%lu 音量=%u%%\n",
                      static_cast<unsigned long>(request_id),
                      static_cast<unsigned>(volume));
    } else {
        Serial.printf("[BT62SP] 收到非事务音量响应：%u%%\n",
                      static_cast<unsigned>(volume));
    }
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
        send_at_line("AT+VOL?");
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
    handle_volume_response_line(s_bt_rx_line);
    note_module_rx_activity_for_volume_query(s_bt_rx_line);
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

void service_volume_command_state()
{
    const uint32_t now = millis();
    PendingVolumeTx action = PendingVolumeTx::None;
    uint8_t set_volume = 0;
    uint32_t query_request_id = 0;
    uint8_t query_attempt = 0;
    bool query_timed_out = false;

    portENTER_CRITICAL(&s_volume_mux);

    // 用户设置优先级高于查询。连续旋钮操作只保留最新音量，避免 UART 队列堆积。
    if (s_pending_set_volume_valid) {
        set_volume = s_pending_set_volume;
        s_pending_set_volume_valid = false;
        action = PendingVolumeTx::Set;
    } else if (s_volume_query_phase == VolumeQueryPhase::WaitBeforeSend &&
               time_reached(now, s_volume_query_due_ms)) {
        ++s_volume_query_attempts;
        s_volume_query_sent_ms = now;
        s_volume_query_phase = VolumeQueryPhase::WaitResponse;
        query_request_id = s_volume_query_request_id;
        query_attempt = s_volume_query_attempts;
        action = PendingVolumeTx::Query;
    } else if (s_volume_query_phase == VolumeQueryPhase::WaitResponse &&
               (now - s_volume_query_sent_ms) >= BT_VOLUME_QUERY_RESPONSE_TIMEOUT_MS) {
        if (s_volume_query_attempts < BT_VOLUME_QUERY_MAX_ATTEMPTS) {
            s_volume_query_phase = VolumeQueryPhase::WaitBeforeSend;
            s_volume_query_due_ms = now + BT_VOLUME_QUERY_RETRY_DELAY_MS;
        } else {
            query_request_id = s_volume_query_request_id;
            publish_volume_query_event_locked(Bt62spVolumeQueryResult::Timeout,
                                              query_request_id,
                                              0);
            clear_volume_query_locked();
            query_timed_out = true;
        }
    }

    portEXIT_CRITICAL(&s_volume_mux);

    if (action == PendingVolumeTx::Set) {
        char cmd[20];
        snprintf(cmd, sizeof(cmd), "AT+VOL=%u", static_cast<unsigned>(set_volume));
        send_at_line(cmd);
    } else if (action == PendingVolumeTx::Query) {
        send_at_line("AT+VOL?");
        Serial.printf("[BT62SP] 音量查询已发送：请求=%lu 尝试=%u/%u\n",
                      static_cast<unsigned long>(query_request_id),
                      static_cast<unsigned>(query_attempt),
                      static_cast<unsigned>(BT_VOLUME_QUERY_MAX_ATTEMPTS));
    }

    if (query_timed_out) {
        Serial.printf("[BT62SP] 音量查询超时：请求=%lu，保持模块原音量，不写入默认值\n",
                      static_cast<unsigned long>(query_request_id));
    }
}

}  // namespace

void bt62sp_uart_debug_begin(uint32_t baud)
{
    bt_uart_begin(baud);

    portENTER_CRITICAL(&s_volume_mux);
    s_ready = true;
    clear_volume_query_locked();
    s_volume_query_event_pending = false;
    s_pending_set_volume_valid = false;
    s_module_ready_generation = 0;
    portEXIT_CRITICAL(&s_volume_mux);

    Serial.println("[BT62SP] UART调试桥已启用：电脑串口输入 AT... 直接转发，输入 BT62 HELP 查看控制命令");
}

uint32_t bt62sp_uart_debug_ready_generation()
{
    uint32_t generation = 0;
    portENTER_CRITICAL(&s_volume_mux);
    generation = s_module_ready_generation;
    portEXIT_CRITICAL(&s_volume_mux);
    return generation;
}

bool bt62sp_uart_debug_request_volume_query(uint32_t settle_ms,
                                            uint32_t* out_request_id)
{
    if (out_request_id) {
        *out_request_id = 0;
    }

    const uint32_t now = millis();
    uint32_t request_id = 0;

    portENTER_CRITICAL(&s_volume_mux);
    if (!s_ready || s_pending_set_volume_valid) {
        // 用户设置命令优先，不能为了重新查询而丢弃尚未发送的设置值。
        portEXIT_CRITICAL(&s_volume_mux);
        return false;
    }

    request_id = s_next_volume_query_request_id++;
    if (s_next_volume_query_request_id == 0) {
        s_next_volume_query_request_id = 1;
    }

    s_volume_query_request_id = request_id;
    s_volume_query_phase = VolumeQueryPhase::WaitBeforeSend;
    s_volume_query_due_ms = now + settle_ms;
    s_volume_query_sent_ms = 0;
    s_volume_query_attempts = 0;
    s_volume_query_event_pending = false;
    s_pending_set_volume_valid = false;
    portEXIT_CRITICAL(&s_volume_mux);

    if (out_request_id) {
        *out_request_id = request_id;
    }

    Serial.printf("[BT62SP] 音量查询已排队：请求=%lu 上电稳定等待=%lums\n",
                  static_cast<unsigned long>(request_id),
                  static_cast<unsigned long>(settle_ms));
    return true;
}

void bt62sp_uart_debug_cancel_volume_query()
{
    portENTER_CRITICAL(&s_volume_mux);
    clear_volume_query_locked();
    s_volume_query_event_pending = false;
    portEXIT_CRITICAL(&s_volume_mux);
}

bool bt62sp_uart_debug_take_volume_query_event(Bt62spVolumeQueryEvent* out_event)
{
    if (!out_event) {
        return false;
    }

    bool available = false;
    portENTER_CRITICAL(&s_volume_mux);
    if (s_volume_query_event_pending) {
        *out_event = s_volume_query_event;
        s_volume_query_event_pending = false;
        available = true;
    }
    portEXIT_CRITICAL(&s_volume_mux);
    return available;
}

bool bt62sp_uart_debug_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }

    portENTER_CRITICAL(&s_volume_mux);
    if (!s_ready) {
        portEXIT_CRITICAL(&s_volume_mux);
        return false;
    }

    // 用户主动设置后，迟到的查询响应不能再覆盖用户选择。
    clear_volume_query_locked();
    s_volume_query_event_pending = false;
    s_pending_set_volume = volume;
    s_pending_set_volume_valid = true;
    portEXIT_CRITICAL(&s_volume_mux);
    return true;
}

void bt62sp_uart_debug_update()
{
    portENTER_CRITICAL(&s_volume_mux);
    const bool ready = s_ready;
    portEXIT_CRITICAL(&s_volume_mux);
    if (!ready) {
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

    service_volume_command_state();
}
