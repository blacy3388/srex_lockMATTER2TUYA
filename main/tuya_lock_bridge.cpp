#include "tuya_lock_bridge.h"

#include <atomic>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tuya_lock";

// ESP32-C6 Super Mini default for this project:
// LOCK TX -> C6 GPIO6 (RX)
// LOCK RX <- C6 GPIO7 (TX)
// Common GND required.
// GPIO16/17 are intentionally left free for UART0/debug compatibility.
static constexpr uart_port_t LOCK_UART = UART_NUM_1;
static constexpr gpio_num_t LOCK_RX_GPIO = GPIO_NUM_6;
static constexpr gpio_num_t LOCK_TX_GPIO = GPIO_NUM_7;
static constexpr int LOCK_BAUD = 115200;

static constexpr uint16_t WAKE_SEQUENCE = 0x55AA;
static constexpr int MAX_WAKE_ATTEMPTS = 3;
static constexpr int64_t WAKE_TIMEOUT_US = 70000;       // 70 ms
static constexpr int64_t CMD_ACK_TIMEOUT_US = 1200000; // 1.2 s
static constexpr int64_t DP_RESULT_TIMEOUT_US = 7000000;
static constexpr int64_t MOTOR_TIMEOUT_US = 12000000;

static const uint8_t REMOTE_KEY[8] = {0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56};
static constexpr uint16_t REMOTE_KEY_ID = 0x0001;

enum class Stage : uint8_t {
    Idle,
    WaitWakeProvision,
    WaitDp48CmdAck,
    WaitDp48Result,
    WaitWakeUnlock,
    WaitDp49CmdAck,
    WaitUnlockResult,
    WaitWakeLock,
    WaitLockCmdAck,
    WaitLockResult,
};

static std::atomic<Stage> s_stage{Stage::Idle};
static uint16_t s_tx_seq = 1;
static uint16_t s_active_seq = 0;
static std::atomic<int> s_wake_attempt{0};
static std::atomic<int64_t> s_stage_started{0};
static std::atomic<bool> s_key_provisioned{false};
static std::atomic<bool> s_pending_unlock{false};

static tuya_lock_state_cb_t s_state_cb = nullptr;
static void *s_state_ctx = nullptr;

static uint8_t s_rx[512];
static size_t s_rx_len = 0;

static uint8_t checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += data[i];
    return static_cast<uint8_t>(sum & 0xFF);
}

static uint16_t next_seq()
{
    uint16_t out = s_tx_seq++;
    if (s_tx_seq == 0 || s_tx_seq == WAKE_SEQUENCE) ++s_tx_seq;
    return out;
}

static void print_hex(const char *prefix, const uint8_t *data, size_t len)
{
    printf("%s", prefix);
    for (size_t i = 0; i < len; ++i) printf("%02X ", data[i]);
    printf("\n");
}

static void send_bytes(const uint8_t *data, size_t len)
{
    print_hex("TX -> LOCK: ", data, len);
    uart_write_bytes(LOCK_UART, data, len);
    uart_wait_tx_done(LOCK_UART, pdMS_TO_TICKS(100));
}

static uint16_t send_frame(uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[300];
    const size_t total = 8 + payload_len + 1;
    if (total > sizeof(frame)) return 0;

    uint16_t seq = next_seq();
    frame[0] = 0x55;
    frame[1] = 0xAA;
    frame[2] = 0x03;
    frame[3] = static_cast<uint8_t>(seq >> 8);
    frame[4] = static_cast<uint8_t>(seq);
    frame[5] = cmd;
    frame[6] = static_cast<uint8_t>(payload_len >> 8);
    frame[7] = static_cast<uint8_t>(payload_len);
    if (payload_len && payload) memcpy(&frame[8], payload, payload_len);
    frame[total - 1] = checksum(frame, total - 1);
    send_bytes(frame, total);
    return seq;
}

static uint16_t send_dp(uint8_t dp, uint8_t type, const uint8_t *value, uint16_t value_len)
{
    uint8_t payload[260];
    if (value_len + 4 > sizeof(payload)) return 0;
    payload[0] = dp;
    payload[1] = type;
    payload[2] = static_cast<uint8_t>(value_len >> 8);
    payload[3] = static_cast<uint8_t>(value_len);
    if (value_len) memcpy(&payload[4], value, value_len);
    return send_frame(0x04, payload, value_len + 4);
}

static void send_wake()
{
    const uint8_t wake[] = {
        0,0,0,0,0,0,0,
        0x55,0xAA,0x03,0x55,0xAA,0x00,0x00,0x00,0x01
    };
    const int attempt = ++s_wake_attempt;
    ESP_LOGI(TAG, "Wake attempt %d/%d", attempt, MAX_WAKE_ATTEMPTS);
    print_hex("TX RAW -> LOCK: ", wake, sizeof(wake));
    uart_write_bytes(LOCK_UART, wake, sizeof(wake));
    uart_wait_tx_done(LOCK_UART, pdMS_TO_TICKS(100));
    s_stage_started = esp_timer_get_time();
}

static void send_cmd00_ack(uint16_t seq)
{
    uint8_t f[9] = {0x55,0xAA,0x03,static_cast<uint8_t>(seq>>8),static_cast<uint8_t>(seq),0x00,0x00,0x00,0x00};
    f[8] = checksum(f, 8);
    send_bytes(f, sizeof(f));
}

static void send_cmd05_ack(uint16_t seq)
{
    uint8_t f[10] = {0x55,0xAA,0x03,static_cast<uint8_t>(seq>>8),static_cast<uint8_t>(seq),0x05,0x00,0x01,0x10,0x00};
    f[9] = checksum(f, 9);
    send_bytes(f, sizeof(f));
}

static void send_cmd23_ack(uint16_t seq)
{
    uint8_t f[10] = {0x55,0xAA,0x03,static_cast<uint8_t>(seq>>8),static_cast<uint8_t>(seq),0x23,0x00,0x01,0x20,0x00};
    f[9] = checksum(f, 9);
    send_bytes(f, sizeof(f));
}

static void send_time_response(uint16_t seq)
{
    time_t now = time(nullptr);
    if (now < 1700000000) now = 1700000000; // safe fallback when wall clock has not been synchronized yet
    uint32_t utc = static_cast<uint32_t>(now);

    // For now use UTC for both fields. The lock accepts this for protocol continuity.
    uint32_t local = utc;

    uint8_t f[17] = {
        0x55,0xAA,0x03,static_cast<uint8_t>(seq>>8),static_cast<uint8_t>(seq),0x24,0x00,0x08,
        static_cast<uint8_t>(utc>>24),static_cast<uint8_t>(utc>>16),static_cast<uint8_t>(utc>>8),static_cast<uint8_t>(utc),
        static_cast<uint8_t>(local>>24),static_cast<uint8_t>(local>>16),static_cast<uint8_t>(local>>8),static_cast<uint8_t>(local),
        0x00
    };
    f[16] = checksum(f, 16);
    send_bytes(f, sizeof(f));
}

static void send_dp48()
{
    uint8_t data[21] = {};
    data[0] = 0x00;
    data[1] = static_cast<uint8_t>(REMOTE_KEY_ID >> 8);
    data[2] = static_cast<uint8_t>(REMOTE_KEY_ID);

    time_t now = time(nullptr);
    if (now < 1700000000) now = 1700000000;
    uint32_t valid_from = static_cast<uint32_t>(now - 60);
    uint32_t valid_to = valid_from + 31536000UL;

    data[3] = valid_from >> 24; data[4] = valid_from >> 16; data[5] = valid_from >> 8; data[6] = valid_from;
    data[7] = valid_to >> 24; data[8] = valid_to >> 16; data[9] = valid_to >> 8; data[10] = valid_to;
    const uint16_t max_uses = 1000;
    data[11] = max_uses >> 8; data[12] = max_uses;
    memcpy(&data[13], REMOTE_KEY, sizeof(REMOTE_KEY));

    ESP_LOGI(TAG, "DP48 provision remote key");
    s_active_seq = send_dp(0x30, 0x00, data, sizeof(data));
    s_stage = Stage::WaitDp48CmdAck;
    s_stage_started = esp_timer_get_time();
}

static void send_dp49_unlock()
{
    uint8_t data[13] = {};
    data[0] = 0x01;
    data[1] = static_cast<uint8_t>(REMOTE_KEY_ID >> 8);
    data[2] = static_cast<uint8_t>(REMOTE_KEY_ID);
    memcpy(&data[3], REMOTE_KEY, sizeof(REMOTE_KEY));
    data[11] = 0x00;
    data[12] = 0x00;

    ESP_LOGI(TAG, "DP49 remote unlock");
    s_active_seq = send_dp(0x31, 0x00, data, sizeof(data));
    s_stage = Stage::WaitDp49CmdAck;
    s_stage_started = esp_timer_get_time();
}

static void send_dp57_lock()
{
    uint8_t v = 0x00;
    ESP_LOGI(TAG, "DP57 lock");
    s_active_seq = send_dp(0x39, 0x01, &v, 1);
    s_stage = Stage::WaitLockCmdAck;
    s_stage_started = esp_timer_get_time();
}

static void report_lock_state(bool locked)
{
    ESP_LOGI(TAG, "Physical lock state => %s", locked ? "LOCKED" : "UNLOCKED");
    if (s_state_cb) s_state_cb(locked, s_state_ctx);

    if (!locked && s_stage == Stage::WaitUnlockResult) {
        s_stage = Stage::Idle;
        s_pending_unlock = false;
        ESP_LOGI(TAG, "Remote unlock confirmed");
    }
    if (locked && s_stage == Stage::WaitLockResult) {
        s_stage = Stage::Idle;
        ESP_LOGI(TAG, "Remote lock confirmed");
    }
}

static uint32_t read_be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static void decode_dps(const uint8_t *payload, uint16_t payload_len)
{
    size_t pos = 0;
    while (pos + 4 <= payload_len) {
        uint8_t dp = payload[pos++];
        uint8_t type = payload[pos++];
        uint16_t len = (static_cast<uint16_t>(payload[pos]) << 8) | payload[pos + 1];
        pos += 2;
        if (pos + len > payload_len) return;
        const uint8_t *value = &payload[pos];

        ESP_LOGI(TAG, "DP%u (0x%02X) type=0x%02X len=%u", dp, dp, type, len);

        if (dp == 10 && type == 0x02 && len == 4) {
            ESP_LOGI(TAG, "Battery=%" PRIu32 "%%", read_be32(value));
        } else if (dp == 47 && len == 1) {
            report_lock_state(value[0] != 0);
        } else if (dp == 57 && type == 0x01 && len == 1) {
            report_lock_state(value[0] == 0 ? true : false);
        } else if (dp == 48 && type == 0x00 && len >= 3) {
            uint8_t result = value[0];
            ESP_LOGI(TAG, "DP48 result=%u", result);
            if (result == 0) {
                s_key_provisioned = true;
                if (s_pending_unlock) {
                    s_wake_attempt = 0;
                    s_stage = Stage::WaitWakeUnlock;
                    send_wake();
                } else {
                    s_stage = Stage::Idle;
                }
            } else {
                s_stage = Stage::Idle;
                s_pending_unlock = false;
            }
        } else if (dp == 49 && type == 0x00 && len >= 3) {
            ESP_LOGI(TAG, "DP49 result=%u", value[0]);
            if (value[0] == 2) s_key_provisioned = false;
        }

        pos += len;
    }
}

static void process_cmd04_ack(uint16_t seq, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len != 1 || seq != s_active_seq) return;
    ESP_LOGI(TAG, "CMD04 ACK result=0x%02X", payload[0]);
    if (payload[0] != 0) {
        s_stage = Stage::Idle;
        s_pending_unlock = false;
        return;
    }

    if (s_stage == Stage::WaitDp48CmdAck) {
        s_stage = Stage::WaitDp48Result;
    } else if (s_stage == Stage::WaitDp49CmdAck) {
        s_stage = Stage::WaitUnlockResult;
    } else if (s_stage == Stage::WaitLockCmdAck) {
        s_stage = Stage::WaitLockResult;
    }
    s_stage_started = esp_timer_get_time();
}

static void process_frame(const uint8_t *frame, size_t total)
{
    if (total < 9) return;
    uint16_t seq = (static_cast<uint16_t>(frame[3]) << 8) | frame[4];
    uint8_t cmd = frame[5];
    uint16_t payload_len = (static_cast<uint16_t>(frame[6]) << 8) | frame[7];
    const uint8_t *payload = &frame[8];

    ESP_LOGI(TAG, "RX seq=0x%04X cmd=0x%02X", seq, cmd);

    if (cmd == 0x00) {
        if (seq == WAKE_SEQUENCE &&
            (s_stage == Stage::WaitWakeProvision || s_stage == Stage::WaitWakeUnlock || s_stage == Stage::WaitWakeLock)) {
            ESP_LOGI(TAG, "Wake ACK received");
            vTaskDelay(pdMS_TO_TICKS(20));
            if (s_stage == Stage::WaitWakeProvision) send_dp48();
            else if (s_stage == Stage::WaitWakeUnlock) send_dp49_unlock();
            else if (s_stage == Stage::WaitWakeLock) send_dp57_lock();
            return;
        }
        send_cmd00_ack(seq);
        return;
    }

    if (cmd == 0x04) {
        process_cmd04_ack(seq, payload, payload_len);
        return;
    }

    if (cmd == 0x05) {
        decode_dps(payload, payload_len);
        send_cmd05_ack(seq);
        return;
    }

    if (cmd == 0x23) {
        if (payload_len > 5) decode_dps(payload + 5, payload_len - 5);
        send_cmd23_ack(seq);
        return;
    }

    if (cmd == 0x24) {
        send_time_response(seq);
        return;
    }
}

static void feed_byte(uint8_t b)
{
    if (s_rx_len == 0) {
        if (b == 0x55) s_rx[s_rx_len++] = b;
        return;
    }
    if (s_rx_len == 1) {
        if (b == 0xAA) s_rx[s_rx_len++] = b;
        else if (b == 0x55) s_rx[0] = 0x55;
        else s_rx_len = 0;
        return;
    }
    if (s_rx_len >= sizeof(s_rx)) { s_rx_len = 0; return; }
    s_rx[s_rx_len++] = b;
    if (s_rx_len >= 8) {
        uint16_t payload_len = (static_cast<uint16_t>(s_rx[6]) << 8) | s_rx[7];
        size_t expected = 8 + payload_len + 1;
        if (expected > sizeof(s_rx)) { s_rx_len = 0; return; }
        if (s_rx_len == expected) {
            print_hex("RX <- LOCK: ", s_rx, expected);
            if (checksum(s_rx, expected - 1) == s_rx[expected - 1]) process_frame(s_rx, expected);
            else ESP_LOGW(TAG, "RX checksum error");
            s_rx_len = 0;
        }
    }
}

static void handle_timeouts()
{
    Stage st = s_stage;
    if (st == Stage::Idle) return;
    int64_t elapsed = esp_timer_get_time() - s_stage_started;

    if (st == Stage::WaitWakeProvision || st == Stage::WaitWakeUnlock || st == Stage::WaitWakeLock) {
        if (elapsed > WAKE_TIMEOUT_US) {
            if (s_wake_attempt < MAX_WAKE_ATTEMPTS) send_wake();
            else {
                ESP_LOGE(TAG, "Wake failed");
                s_stage = Stage::Idle;
                s_pending_unlock = false;
            }
        }
        return;
    }

    if (st == Stage::WaitDp48CmdAck || st == Stage::WaitDp49CmdAck || st == Stage::WaitLockCmdAck) {
        if (elapsed > CMD_ACK_TIMEOUT_US) {
            ESP_LOGE(TAG, "CMD04 ACK timeout");
            s_stage = Stage::Idle;
            s_pending_unlock = false;
        }
        return;
    }

    if (st == Stage::WaitDp48Result && elapsed > DP_RESULT_TIMEOUT_US) {
        ESP_LOGE(TAG, "DP48 result timeout");
        s_stage = Stage::Idle;
        s_pending_unlock = false;
        return;
    }

    if ((st == Stage::WaitUnlockResult || st == Stage::WaitLockResult) && elapsed > MOTOR_TIMEOUT_US) {
        ESP_LOGE(TAG, "Motor/state result timeout");
        s_stage = Stage::Idle;
        s_pending_unlock = false;
    }
}

static void uart_task(void *)
{
    uint8_t buf[128];
    while (true) {
        int n = uart_read_bytes(LOCK_UART, buf, sizeof(buf), pdMS_TO_TICKS(10));
        for (int i = 0; i < n; ++i) feed_byte(buf[i]);
        handle_timeouts();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" esp_err_t tuya_lock_bridge_init(tuya_lock_state_cb_t cb, void *ctx)
{
    s_state_cb = cb;
    s_state_ctx = ctx;

    uart_config_t cfg = {};
    cfg.baud_rate = LOCK_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_param_config(LOCK_UART, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(LOCK_UART, LOCK_TX_GPIO, LOCK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(LOCK_UART, 4096, 0, 0, nullptr, 0);
    if (err != ESP_OK) return err;

    if (xTaskCreate(uart_task, "tuya_uart", 6144, nullptr, 5, nullptr) != pdPASS) {
        uart_driver_delete(LOCK_UART);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Tuya UART ready: RX GPIO%d, TX GPIO%d @ %d", LOCK_RX_GPIO, LOCK_TX_GPIO, LOCK_BAUD);
    return ESP_OK;
}

extern "C" bool tuya_lock_request_unlock(void)
{
    if (s_stage != Stage::Idle) {
        ESP_LOGW(TAG, "Unlock rejected: bridge busy");
        return false;
    }
    s_pending_unlock = true;
    s_wake_attempt = 0;
    s_stage = s_key_provisioned ? Stage::WaitWakeUnlock : Stage::WaitWakeProvision;
    ESP_LOGI(TAG, "Matter request => UNLOCK%s", s_key_provisioned ? "" : " (provision DP48 first)");
    send_wake();
    return true;
}

extern "C" bool tuya_lock_request_lock(void)
{
    if (s_stage != Stage::Idle) {
        ESP_LOGW(TAG, "Lock rejected: bridge busy");
        return false;
    }
    s_pending_unlock = false;
    s_wake_attempt = 0;
    s_stage = Stage::WaitWakeLock;
    ESP_LOGI(TAG, "Matter request => LOCK");
    send_wake();
    return true;
}
