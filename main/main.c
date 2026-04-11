#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "walkie_audio.h"
#include "walkie_display.h"
#include "walkie_types.h"

#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"

/*
 * Firmware map:
 * - Hardware constants and board_config_init() select the black/grey pinout.
 * - control_task() owns buttons, menus, lights, battery/volume smoothing, and OLED redraws.
 * - capture_task() records the I2S mic, cleans/gains/compresses it, then sends ESP-NOW frames.
 * - handle_radio_item() validates packets, updates link quality, and feeds the jitter buffer.
 * - playback_task() drains the jitter buffer to the I2S speaker and fills short packet gaps.
 */

static const char *TAG = "walkie";

/* Shared fixed wiring. The board-specific swaps live in board_config_init(). */
#define OLED_SCL GPIO_NUM_18
#define OLED_SDA GPIO_NUM_19

#define SPK_BCLK GPIO_NUM_32
#define SPK_WS   GPIO_NUM_33
#define SPK_DIN  GPIO_NUM_25

#define MIC_BCLK GPIO_NUM_16
#define MIC_WS   GPIO_NUM_17
#define MIC_SD   GPIO_NUM_4

#define OK_PIN        GPIO_NUM_0
#define BOT_LEFT_PIN  GPIO_NUM_14
#define BOT_RIGHT_PIN GPIO_NUM_15
#define LASER_PIN     GPIO_NUM_21

#define POT_PIN GPIO_NUM_34
#define BAT_PIN GPIO_NUM_35

#define LOGICAL_CHANNEL_MIN 1
#define LOGICAL_CHANNEL_MAX 20
#define FLAG_KID 0x01
#define KID_CHANNEL 1

#define SAMPLE_RATE_HZ      16000
#define FRAME_MS            20
#define SAMPLES_PER_FRAME   ((SAMPLE_RATE_HZ * FRAME_MS) / 1000)
#define AUDIO_PAYLOAD_BYTES (SAMPLES_PER_FRAME / 2)
#define RADIO_RX_DATA_MAX   250

#define RX_QUEUE_FRAMES   10
#define RX_PREFILL_FRAMES 4

#define POT_UPDATE_MS   90
#define BATT_UPDATE_MS  700
#define RESOURCE_UPDATE_MS 500
#define OLED_UPDATE_MS  180
#define HEARTBEAT_MS    650
#define LINK_TIMEOUT_MS 2200
#define SCAN_WAIT_MS    95

/* The range telemetry cadence. One line per second is frequent enough to show
 * packet loss trends while keeping serial/flash writes small. */
#define DEBUG_JSON_STATS_MS 1000

/* Onboard range-test log storage. The partition is mounted at /fieldlog and the
 * log is JSON Lines so every record can be parsed independently after a test. */
#define FIELD_LOG_MOUNT_PATH "/fieldlog"
#define FIELD_LOG_CURRENT_PATH FIELD_LOG_MOUNT_PATH "/range.jsonl"
#define FIELD_LOG_PREVIOUS_PATH FIELD_LOG_MOUNT_PATH "/range.prev.jsonl"

/* Keep two files under the 512 KB SPIFFS partition: current plus previous. */
#define FIELD_LOG_MAX_FILE_BYTES (220 * 1024)

/* Flash writes happen from a low-priority task. The control loop only drops a
 * fixed-size line into this short queue so audio timing is not blocked by flash. */
#define FIELD_LOG_QUEUE_DEPTH    8
#define FIELD_LOG_LINE_BYTES     512

/* Range-first ESP-NOW settings. TX power is quarter-dBm; 84 requests the ESP32 max. */
#define WIFI_MAX_TX_POWER_QDBM 84
#define LINK_RSSI_UNKNOWN_DBM (-127)
#define LINK_RSSI_WEAK_DBM    (-92)
#define LINK_RSSI_STRONG_DBM  (-45)

/* Weak-link redundancy threshold. When the RSSI-derived quality is <= 65%, or
 * the peer is not linked yet, each 20 ms audio frame is sent twice. */
#define AUDIO_REDUNDANCY_LINK_QUALITY_PCT 65
#define AUDIO_REDUNDANCY_EXTRA_COPIES     1

#define TX_PREAMBLE_FRAMES 2
#define TX_END_FRAMES      2
#define MIC_WARMUP_FRAMES  2

/* Packet types are intentionally tiny so the 20 ms ADPCM voice frames fit in ESP-NOW. */
#define PKT_AUDIO     0xA1
#define PKT_HEART     0xB1
#define PKT_SCAN_REQ  0xB2
#define PKT_SCAN_RESP 0xB3
#define PROTO_VERSION 0x02

/**
 * Individual counters included in the once-per-second field-test JSON line.
 *
 * These are intentionally event-style counters rather than logs of every packet.
 * A one-second aggregate is compact enough for flash while still showing whether
 * long-range jitter is caused by packet loss, duplicate recovery, wrong-channel
 * traffic, or local ESP-NOW send failures.
 */
typedef enum {
    DBG_TX_AUDIO = 0,
    DBG_TX_AUDIO_DUP,
    DBG_TX_CTRL,
    DBG_TX_NO_MEM,
    DBG_TX_FAIL,
    DBG_RX_AUDIO,
    DBG_RX_AUDIO_DUP,
    DBG_RX_AUDIO_OLD,
    DBG_RX_PLC,
    DBG_RX_CTRL,
    DBG_RX_WRONG_PEER,
    DBG_RX_BAD_PROTO,
    DBG_RX_WRONG_CHANNEL,
} debug_stat_id_t;

/**
 * Small shared control-packet header.
 *
 * Heartbeat and scan packets use only these fields. Audio packets start with the
 * same first fields so handle_radio_item() can validate type/version/channel
 * before looking at the rest of the payload.
 */
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t version;
    uint8_t logical_channel;
    uint8_t flags;
    uint16_t seq;
} walkie_ctrl_packet_t;

/**
 * One compressed voice frame carried by ESP-NOW.
 *
 * The packet contains one 20 ms ADPCM frame. predictor and step_index capture
 * the encoder state at the start of the frame, letting the receiver decode the
 * packet independently enough to recover after short loss.
 */
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t version;
    uint8_t logical_channel;
    uint8_t flags;
    uint16_t seq;
    int16_t predictor;
    uint8_t step_index;
    uint16_t sample_count;
    uint8_t payload[AUDIO_PAYLOAD_BYTES];
} walkie_audio_packet_t;

/**
 * Debounce state for one active-low GPIO button.
 */
typedef struct {
    gpio_num_t pin;
    bool state;
    bool last_raw;
    int debounce_ms;
    int64_t last_change_ms;
} debounced_button_t;

/**
 * Queue item copied out of the ESP-NOW receive callback.
 *
 * Includes packet bytes, source MAC, and RSSI metadata. Keeping this separate
 * from protocol structs lets the callback remain generic and lightweight.
 */
typedef struct {
    size_t len;
    int8_t rssi;
    uint8_t src_mac[6];
    uint8_t data[RADIO_RX_DATA_MAX];
} radio_rx_item_t;

/**
 * Small ring buffer between radio receive and speaker playback.
 *
 * ESP-NOW packets do not arrive at a perfectly steady 20 ms cadence. This buffer
 * absorbs timing jitter, tracks sequence history, and stores the last good frame
 * for short packet-loss concealment.
 */
typedef struct {
    SemaphoreHandle_t mutex;
    int16_t frames[RX_QUEUE_FRAMES][SAMPLES_PER_FRAME];
    int16_t last_good[SAMPLES_PER_FRAME];
    int rd;
    int wr;
    int count;
    bool started;
    bool have_last_good;
    bool have_last_seq;
    uint16_t last_seq;
    TickType_t last_rx_tick;
} jitter_buffer_t;

/**
 * Per-second radio counters printed as JSON for long-range field testing.
 *
 * The counters reset after every debug line, so a saved USB serial log can be
 * graphed as "what happened during this second" instead of needing post-
 * processing on lifetime totals.
 */
typedef struct {
    /* Packets this unit tried to send. Duplicates are tracked separately so we
     * can confirm whether weak-link redundancy actually turned on. */
    uint32_t tx_audio;
    uint32_t tx_audio_dup;
    uint32_t tx_ctrl;

    /* ESP-NOW send-side pressure. NO_MEM means the Wi-Fi/ESP-NOW queue was full;
     * tx_fail is for other immediate esp_now_send() errors. */
    uint32_t tx_no_mem;
    uint32_t tx_fail;

    /* Receive-side audio health. rx_audio is accepted unique audio; duplicate
     * and old packets are counted but intentionally not played. */
    uint32_t rx_audio;
    uint32_t rx_audio_dup;
    uint32_t rx_audio_old;

    /* Packet-loss concealment frames generated locally to cover missing audio. */
    uint32_t rx_plc;

    /* Control and reject counters help separate real range loss from wrong peer,
     * wrong protocol, or wrong logical-channel situations. */
    uint32_t rx_ctrl;
    uint32_t rx_wrong_peer;
    uint32_t rx_bad_proto;
    uint32_t rx_wrong_channel;
} radio_debug_stats_t;

/**
 * Fixed-size queue item for field logging.
 *
 * Dynamic allocation in an audio/radio firmware is a tiny gremlin factory, so
 * log records are copied into fixed buffers and sent to the flash task.
 */
typedef struct {
    char line[FIELD_LOG_LINE_BYTES];
} field_log_line_t;

/**
 * Mutable application state protected by s_state_lock.
 *
 * Tasks share this struct for UI mode, button levels, logical channel, battery,
 * link status, and derived settings. Rendering code never reads it directly; it
 * receives a walkie_ui_snapshot_t copy instead.
 */
typedef struct {
    walkie_ui_mode_t ui_mode;
    int selected_app;
    int current_channel;
    bool laser_on;

    bool tl_down;
    bool tr_down;
    bool ok_down;
    bool bl_down;
    bool br_down;
    bool ptt_down;

    int base_vol_percent;
    float vbat;
    int batt_pct;

    TickType_t last_link_tick;
    int link_rssi_dbm;
    int link_quality_pct;
    TickType_t last_heart_tick;
    TickType_t last_pot_tick;
    TickType_t last_batt_tick;
    TickType_t last_oled_tick;
    TickType_t last_stats_tick;
    int64_t kid_hold_start_ms;
    int64_t rx_led_until_ms;

    walkie_extra_state_t extra;
} walkie_app_state_t;

static walkie_board_config_t s_board;
static walkie_display_t s_display;

static SemaphoreHandle_t s_state_lock;
static walkie_app_state_t s_app;
static jitter_buffer_t s_jitter;

static QueueHandle_t s_radio_rx_queue;

static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static adc_oneshot_unit_handle_t s_adc_unit;
static adc_channel_t s_pot_channel;
static adc_channel_t s_bat_channel;
static adc_cali_handle_t s_bat_cali;
static bool s_bat_cali_ok;
static bool s_adc_ready;
static bool s_radio_ready;
static bool s_i2s_tx_ready;
static bool s_i2s_rx_ready;

/* Debug counters can be touched from several tasks, so they use a tiny spinlock
 * instead of the larger app-state mutex. They are quick increment-only paths. */
static portMUX_TYPE s_debug_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static radio_debug_stats_t s_debug_stats;

/* Flash logging is optional at runtime. If SPIFFS fails to mount, the firmware
 * still runs normally and keeps serial JSON output. */
static QueueHandle_t s_field_log_queue;
static bool s_field_log_ready;

static debounced_button_t s_ok_btn;
static debounced_button_t s_left_btn;
static debounced_button_t s_right_btn;
static debounced_button_t s_bl_btn;
static debounced_button_t s_br_btn;

_Static_assert(sizeof(walkie_audio_packet_t) <= RADIO_RX_DATA_MAX, "radio rx buffer too small");

/**
 * Initialize one debounced, active-low button.
 *
 * The physical buttons are wired to pull the GPIO low when pressed. This stores
 * the current raw level and timestamp so button_pressed() can later reject
 * bounce/noise before reporting a real edge.
 */
static void button_init(debounced_button_t *button, gpio_num_t pin, int debounce_ms)
{
    button->pin = pin;
    button->debounce_ms = debounce_ms;
    button->state = gpio_get_level(pin);
    button->last_raw = button->state;
    button->last_change_ms = esp_timer_get_time() / 1000;
}

/**
 * Return true exactly once when a debounced press edge is observed.
 *
 * This is edge-based, not level-based: holding the button down does not keep
 * returning true. Long-hold behavior is implemented separately by checking
 * button_down() over time.
 */
static bool button_pressed(debounced_button_t *button, int64_t now_ms)
{
    bool raw = gpio_get_level(button->pin);
    if (raw != button->last_raw) {
        button->last_raw = raw;
        button->last_change_ms = now_ms;
        return false;
    }

    if (raw != button->state && (now_ms - button->last_change_ms) >= button->debounce_ms) {
        button->state = raw;
        return button->state == 0;
    }

    return false;
}

/**
 * Return the current debounced button level as a pressed/not-pressed boolean.
 *
 * Used for UI indicators and hold detection, where we need to know that a
 * button is currently down rather than just seeing the first press edge.
 */
static bool button_down(const debounced_button_t *button)
{
    return button->state == 0;
}

/**
 * Parse a colon-separated MAC address from Kconfig.
 *
 * If the string is malformed the caller keeps its compiled-in default MAC.
 * This lets the firmware be configured per pair without editing C source.
 */
static bool parse_mac_string(const char *text, uint8_t mac[6])
{
    unsigned int bytes[6];
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
               &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = (uint8_t)bytes[i];
    }
    return true;
}

/**
 * Format a MAC address into a caller-provided text buffer.
 *
 * Used only for boot logs so the flashed image can confirm whether it is the
 * black or grey unit and which peer it expects to talk to.
 */
static const char *mac_to_str(const uint8_t mac[6], char *buffer, size_t buffer_len)
{
    snprintf(buffer, buffer_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buffer;
}

/**
 * Fill the board profile selected by sdkconfig.
 *
 * This is the central place for physical differences between the black and grey
 * walkies: GPIO swaps, battery divider smoothing, peer MAC, and per-unit mic
 * gain calibration. The rest of the code reads s_board and stays board-agnostic.
 */
static void board_config_init(walkie_board_config_t *board)
{
    uint8_t black_mac[6] = {0xA4, 0xF0, 0x0F, 0x66, 0xD2, 0xD0};
    uint8_t grey_mac[6]  = {0xA4, 0xF0, 0x0F, 0x67, 0xBA, 0x1C};

    parse_mac_string(CONFIG_WALKIE_BLACK_MAC, black_mac);
    parse_mac_string(CONFIG_WALKIE_GREY_MAC, grey_mac);

#if CONFIG_WALKIE_BOARD_BLACK
    board->label = "BLACK";
    memcpy(board->self_mac, black_mac, sizeof(board->self_mac));
    memcpy(board->peer_mac, grey_mac, sizeof(board->peer_mac));
    board->ptt_pin = GPIO_NUM_22;
    board->led_pin = GPIO_NUM_23;
    board->top_left_pin = GPIO_NUM_26;
    board->top_right_pin = GPIO_NUM_2;
    board->batt_r_top = 100000.0f;
    board->batt_r_bottom = 100000.0f;
    board->batt_avg_samples = 8;
    board->batt_smooth_alpha = 0.12f;
    board->mic_base_gain_q10 = 4096;
    board->mic_boost_gain_q10 = 8192;
    board->mic_cut_gain_q10 = 2048;
#else
    board->label = "GREY";
    memcpy(board->self_mac, grey_mac, sizeof(board->self_mac));
    memcpy(board->peer_mac, black_mac, sizeof(board->peer_mac));
    board->ptt_pin = GPIO_NUM_23;
    board->led_pin = GPIO_NUM_22;
    board->top_left_pin = GPIO_NUM_2;
    board->top_right_pin = GPIO_NUM_26;
    board->batt_r_top = 220000.0f;
    board->batt_r_bottom = 220000.0f;
    board->batt_avg_samples = 16;
    board->batt_smooth_alpha = 0.08f;
    board->mic_base_gain_q10 = 1024;
    board->mic_boost_gain_q10 = 3072;
    board->mic_cut_gain_q10 = 512;
#endif
}

/**
 * Return the currently active logical communication channel.
 *
 * PTT mode uses the user-selected channel. Kid mode is locked to channel 1 and
 * sets a flag so normal PTT traffic and kid-mode traffic do not mix.
 * Callers must hold s_state_lock because this reads UI state.
 */
static bool active_comm_locked(uint8_t *logical_channel, uint8_t *flags)
{
    if (s_app.ui_mode == MODE_KID) {
        if (logical_channel) {
            *logical_channel = KID_CHANNEL;
        }
        if (flags) {
            *flags = FLAG_KID;
        }
        return true;
    }

    if (s_app.ui_mode == MODE_PTT) {
        if (logical_channel) {
            *logical_channel = (uint8_t)s_app.current_channel;
        }
        if (flags) {
            *flags = 0;
        }
        return true;
    }

    return false;
}

/**
 * Decide whether the peer link should currently be shown as connected.
 *
 * Link state is heartbeat/audio based. A packet from the peer updates
 * last_link_tick; if nothing has arrived within LINK_TIMEOUT_MS, the UI shows
 * LINK OFF and the signal meter goes empty.
 */
static bool link_is_connected_locked(TickType_t now_tick)
{
    if (s_app.last_link_tick == 0) {
        return false;
    }
    return (now_tick - s_app.last_link_tick) <= pdMS_TO_TICKS(LINK_TIMEOUT_MS);
}

/**
 * Clear all transient link-strength state.
 *
 * Called when changing channels or leaving a communication mode so stale RSSI
 * bars are not displayed for the new channel.
 */
static void clear_link_locked(void)
{
    s_app.last_link_tick = 0;
    s_app.link_rssi_dbm = LINK_RSSI_UNKNOWN_DBM;
    s_app.link_quality_pct = 0;
}

/**
 * Convert RSSI in dBm to a UI-friendly 0-100 quality value.
 *
 * The chosen endpoints are deliberately practical for ESP-NOW walkie behavior:
 * around -45 dBm is excellent nearby signal, while around -92 dBm is barely
 * usable long-range signal.
 */
static int link_quality_from_rssi(int rssi_dbm)
{
    if (rssi_dbm <= LINK_RSSI_WEAK_DBM) {
        return 0;
    }
    if (rssi_dbm >= LINK_RSSI_STRONG_DBM) {
        return 100;
    }

    return ((rssi_dbm - LINK_RSSI_WEAK_DBM) * 100) /
           (LINK_RSSI_STRONG_DBM - LINK_RSSI_WEAK_DBM);
}

/**
 * Increment one JSON debug counter from any task.
 *
 * ESP-NOW callbacks, radio dispatch, capture, and playback all contribute to
 * the long-range debug line. A tiny critical section keeps the counters coherent
 * without taking the larger application mutex.
 */
static void debug_stats_inc(debug_stat_id_t id)
{
    portENTER_CRITICAL(&s_debug_stats_mux);
    switch (id) {
    case DBG_TX_AUDIO:
        s_debug_stats.tx_audio++;
        break;
    case DBG_TX_AUDIO_DUP:
        s_debug_stats.tx_audio_dup++;
        break;
    case DBG_TX_CTRL:
        s_debug_stats.tx_ctrl++;
        break;
    case DBG_TX_NO_MEM:
        s_debug_stats.tx_no_mem++;
        break;
    case DBG_TX_FAIL:
        s_debug_stats.tx_fail++;
        break;
    case DBG_RX_AUDIO:
        s_debug_stats.rx_audio++;
        break;
    case DBG_RX_AUDIO_DUP:
        s_debug_stats.rx_audio_dup++;
        break;
    case DBG_RX_AUDIO_OLD:
        s_debug_stats.rx_audio_old++;
        break;
    case DBG_RX_PLC:
        s_debug_stats.rx_plc++;
        break;
    case DBG_RX_CTRL:
        s_debug_stats.rx_ctrl++;
        break;
    case DBG_RX_WRONG_PEER:
        s_debug_stats.rx_wrong_peer++;
        break;
    case DBG_RX_BAD_PROTO:
        s_debug_stats.rx_bad_proto++;
        break;
    case DBG_RX_WRONG_CHANNEL:
        s_debug_stats.rx_wrong_channel++;
        break;
    }
    portEXIT_CRITICAL(&s_debug_stats_mux);
}

/**
 * Snapshot and clear the current JSON debug counters.
 */
static radio_debug_stats_t debug_stats_take_snapshot(void)
{
    radio_debug_stats_t snapshot;
    portENTER_CRITICAL(&s_debug_stats_mux);
    snapshot = s_debug_stats;
    memset(&s_debug_stats, 0, sizeof(s_debug_stats));
    portEXIT_CRITICAL(&s_debug_stats_mux);
    return snapshot;
}

/**
 * Compare two 16-bit audio sequence numbers with wraparound.
 *
 * Returns positive when seq is newer than reference, zero for a duplicate, and
 * negative for an old late packet. This is what lets us transmit redundant
 * copies without the receiver playing the same 20 ms voice frame twice.
 */
static int audio_seq_delta(uint16_t seq, uint16_t reference)
{
    uint16_t forward = (uint16_t)(seq - reference);
    if (forward == 0) {
        return 0;
    }
    if (forward < 0x8000U) {
        return (int)forward;
    }
    return -((int)((uint16_t)(reference - seq)));
}

/**
 * Decide whether weak-link redundancy should be enabled for the next TX frame.
 *
 * Close-range links keep one packet per frame to preserve airtime. When the
 * peer is unknown or the smoothed RSSI-derived quality drops, each audio frame
 * gets one immediate duplicate so the receiver has a second chance before the
 * 20 ms playback deadline.
 *
 * Callers must hold s_state_lock.
 */
static int audio_redundancy_extra_copies_locked(TickType_t now_tick)
{
    if (!link_is_connected_locked(now_tick) ||
        s_app.link_quality_pct <= AUDIO_REDUNDANCY_LINK_QUALITY_PCT) {
        return AUDIO_REDUNDANCY_EXTRA_COPIES;
    }
    return 0;
}

/**
 * Convert battery voltage to an approximate lithium-cell percentage.
 *
 * This is intentionally a display curve, not a fuel-gauge model. It makes the
 * OLED battery bar more useful around the steep middle of a Li-ion discharge.
 */
static int pct_curve_from_vbat(float vbat)
{
    if (vbat <= 3.30f) {
        return 0;
    }
    if (vbat <= 3.55f) {
        return (int)((vbat - 3.30f) * 80.0f);
    }
    if (vbat <= 3.70f) {
        return 20 + (int)((vbat - 3.55f) * 266.6667f);
    }
    if (vbat <= 3.90f) {
        return 60 + (int)((vbat - 3.70f) * 150.0f);
    }
    if (vbat <= 4.20f) {
        return 90 + (int)((vbat - 3.90f) * 33.3333f);
    }
    return 100;
}

/**
 * Recompute effective speaker and microphone settings from UI toggles.
 *
 * Volume starts from the analog pot, then settings can cap or boost it. Mic
 * boost/cut select board-calibrated gain values so the black and grey units can
 * behave similarly despite different microphone sensitivity.
 *
 * Callers must hold s_state_lock.
 */
static void apply_audio_settings_locked(void)
{
    int effective = s_app.base_vol_percent;

    if (s_app.extra.set_spk_boost) {
        effective = walkie_clamp_int(effective + 20, 0, 100);
    }
    if (s_app.extra.set_limit60) {
        effective = walkie_clamp_int(effective, 0, 60);
    }
    if (s_app.extra.set_limit60_lowbat && s_app.vbat < 3.60f) {
        effective = walkie_clamp_int(effective, 0, 60);
    }

    s_app.extra.eff_vol_percent = effective;

    if (effective <= 0) {
        s_app.extra.speaker_gain_q12 = 0;
    } else {
        int gain_q12 = 256 + (effective * 67);
        if (s_app.extra.set_spk_boost) {
            gain_q12 = (gain_q12 * 6) / 5;
        }
        s_app.extra.speaker_gain_q12 = walkie_clamp_int(gain_q12, 0, 8192);
    }

    if (s_app.extra.set_mic_boost) {
        s_app.extra.mic_manual_mode = 1;
        s_app.extra.mic_gain_q10 = s_board.mic_boost_gain_q10;
    } else if (s_app.extra.set_mic_cut) {
        s_app.extra.mic_manual_mode = -1;
        s_app.extra.mic_gain_q10 = s_board.mic_cut_gain_q10;
    } else {
        s_app.extra.mic_manual_mode = 0;
        s_app.extra.mic_gain_q10 = s_board.mic_base_gain_q10;
    }
}

/**
 * Refresh flash, heap, and CPU usage values shown on the settings screen.
 *
 * Flash usage comes from the running OTA partition metadata. Memory is based on
 * internal heap totals. CPU usage uses FreeRTOS run-time stats over a periodic
 * sampling window when that feature is enabled in sdkconfig.defaults.
 *
 * Callers must hold s_state_lock.
 */
static void update_resource_stats_locked(TickType_t now_tick)
{
    if (s_app.extra.flash_total_bytes == 0 || s_app.extra.flash_used_bytes == 0) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running != NULL) {
            esp_partition_pos_t pos = {
                .offset = running->address,
                .size = running->size,
            };
            esp_image_metadata_t metadata = {0};

            s_app.extra.flash_total_bytes = (uint32_t)running->size;
            if (esp_image_get_metadata(&pos, &metadata) == ESP_OK &&
                metadata.image_len > 0 &&
                metadata.image_len <= running->size) {
                s_app.extra.flash_used_bytes = metadata.image_len;
            }
        }
    }

    if (s_app.extra.memory_total_bytes != 0 &&
        (now_tick - s_app.last_stats_tick) < pdMS_TO_TICKS(RESOURCE_UPDATE_MS)) {
        return;
    }

    s_app.last_stats_tick = now_tick;
    s_app.extra.memory_total_bytes =
        (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_app.extra.memory_free_bytes =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_app.extra.memory_total_bytes >= s_app.extra.memory_free_bytes) {
        s_app.extra.memory_used_bytes = s_app.extra.memory_total_bytes - s_app.extra.memory_free_bytes;
    } else {
        s_app.extra.memory_used_bytes = 0;
    }

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && defined(INCLUDE_xTaskGetIdleTaskHandle) && INCLUDE_xTaskGetIdleTaskHandle
    {
        int idle0 = (int)ulTaskGetIdleRunTimePercentForCore(0);
#if portNUM_PROCESSORS > 1
        int idle1 = (int)ulTaskGetIdleRunTimePercentForCore(1);
        s_app.extra.cpu_usage_pct = walkie_clamp_int(100 - ((idle0 + idle1) / 2), 0, 100);
#else
        s_app.extra.cpu_usage_pct = walkie_clamp_int(100 - idle0, 0, 100);
#endif
    }
#else
    s_app.extra.cpu_usage_pct = 0;
#endif
}

/**
 * Average multiple ADC reads from one channel.
 *
 * Used for both the volume pot and battery divider. Averaging removes a little
 * electrical jitter before higher-level smoothing decides how quickly the UI
 * and audio gain should react.
 */
static int read_adc_raw_average(adc_channel_t channel, int samples)
{
    if (!s_adc_ready || samples <= 0) {
        return 0;
    }

    int total = 0;
    int raw = 0;
    for (int i = 0; i < samples; ++i) {
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc_unit, channel, &raw));
        total += raw;
    }
    return total / samples;
}

/**
 * Read the analog volume knob and map it to 0-100%.
 *
 * If ADC setup failed, this returns a safe mid-volume default so the firmware
 * remains usable even without analog input.
 */
static int read_pot_percent(void)
{
    if (!s_adc_ready) {
        return 50;
    }

    int raw = read_adc_raw_average(s_pot_channel, 8);
    return walkie_clamp_int((raw * 100) / 4095, 0, 100);
}

/**
 * Read and calibrate the battery voltage divider.
 *
 * The divider ratio is board-specific because the black and grey walkies use
 * different resistor values. ADC calibration is used when available; otherwise
 * a conservative raw-to-millivolt fallback is used.
 */
static float read_battery_voltage(void)
{
    if (!s_adc_ready) {
        return 3.95f;
    }

    float divider = (s_board.batt_r_top + s_board.batt_r_bottom) / s_board.batt_r_bottom;
    int raw = 0;
    int total_mv = 0;

    for (int i = 0; i < s_board.batt_avg_samples; ++i) {
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc_unit, s_bat_channel, &raw));
        if (s_bat_cali_ok) {
            int mv = 0;
            adc_cali_raw_to_voltage(s_bat_cali, raw, &mv);
            total_mv += mv;
        } else {
            total_mv += (raw * 2450) / 4095;
        }
    }

    return ((float)total_mv / (float)s_board.batt_avg_samples / 1000.0f) * divider;
}

/**
 * Smooth slow analog inputs and update derived settings.
 *
 * The volume knob uses a hybrid smoother: small noise is filtered, but a real
 * user twist catches up quickly. Battery voltage is smoothed more slowly so the
 * header bar does not flicker.
 *
 * Callers must hold s_state_lock.
 */
static void update_smoothed_inputs_locked(int64_t now_ms, TickType_t now_tick)
{
    if ((now_tick - s_app.last_pot_tick) >= pdMS_TO_TICKS(POT_UPDATE_MS)) {
        int percent = read_pot_percent();
        int delta = percent - s_app.base_vol_percent;

        if (abs(delta) >= 6) {
            if (s_app.extra.big_knob_start_ms == 0) {
                s_app.extra.big_knob_start_ms = now_ms;
            } else if ((now_ms - s_app.extra.big_knob_start_ms) >= 200) {
                s_app.base_vol_percent = percent;
            }
        } else {
            s_app.extra.big_knob_start_ms = 0;
            s_app.base_vol_percent = (s_app.base_vol_percent * 85 + percent * 15 + 50) / 100;
        }

        apply_audio_settings_locked();
        s_app.last_pot_tick = now_tick;
    }

    if ((now_tick - s_app.last_batt_tick) >= pdMS_TO_TICKS(BATT_UPDATE_MS)) {
        float new_vbat = read_battery_voltage();
        s_app.vbat = (s_app.vbat * (1.0f - s_board.batt_smooth_alpha)) + (new_vbat * s_board.batt_smooth_alpha);
        s_app.batt_pct = walkie_clamp_int(pct_curve_from_vbat(s_app.vbat), 0, 100);
        apply_audio_settings_locked();
        s_app.last_batt_tick = now_tick;
    }

    update_resource_stats_locked(now_tick);
}

/**
 * Copy the live app state into a rendering snapshot.
 *
 * The display code reads only this snapshot, so it can render without holding
 * the state mutex or poking hardware. This keeps UI drawing separated from
 * button/radio/audio state ownership.
 *
 * Callers must hold s_state_lock.
 */
static void state_copy_snapshot_locked(walkie_ui_snapshot_t *snapshot, TickType_t now_tick, int64_t now_ms)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->device_label = s_board.label;
    snapshot->ui_mode = s_app.ui_mode;
    snapshot->selected_app = s_app.selected_app;
    snapshot->current_channel = s_app.current_channel;
    snapshot->laser_on = s_app.laser_on;
    snapshot->link_on = link_is_connected_locked(now_tick);
    snapshot->link_quality_pct = snapshot->link_on ? s_app.link_quality_pct : 0;
    snapshot->link_rssi_dbm = snapshot->link_on ? s_app.link_rssi_dbm : LINK_RSSI_UNKNOWN_DBM;
    snapshot->batt_pct = s_app.batt_pct;
    snapshot->vbat = s_app.vbat;
    snapshot->eff_vol_percent = s_app.extra.eff_vol_percent;
    snapshot->tl_pressed = s_app.tl_down;
    snapshot->tr_pressed = s_app.tr_down;
    snapshot->ok_pressed = s_app.ok_down;
    snapshot->bl_pressed = s_app.bl_down;
    snapshot->br_pressed = s_app.br_down;
    snapshot->ptt_pressed = s_app.ptt_down;
    snapshot->rx_audio_active = (now_ms < s_app.rx_led_until_ms);
    snapshot->extra = s_app.extra;

    if (s_app.ui_mode == MODE_KID && s_app.kid_hold_start_ms > 0 && s_app.ok_down) {
        snapshot->kid_hold_ms = (int)(now_ms - s_app.kid_hold_start_ms);
    }
}

/**
 * Configure GPIO directions for all buttons and outputs.
 *
 * Inputs use internal pull-ups because buttons are active-low. Board-specific
 * PTT, LED, and top-button pins have already been selected in s_board.
 */
static void gpio_init_inputs_outputs(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << s_board.led_pin) | (1ULL << LASER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));

    gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << s_board.ptt_pin) |
                        (1ULL << OK_PIN) |
                        (1ULL << s_board.top_left_pin) |
                        (1ULL << s_board.top_right_pin) |
                        (1ULL << BOT_LEFT_PIN) |
                        (1ULL << BOT_RIGHT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&inputs));

    gpio_set_level(s_board.led_pin, 0);
    gpio_set_level(LASER_PIN, 0);
}

/**
 * Create an ADC calibration handle when the ESP-IDF target supports it.
 *
 * The firmware tries curve fitting first, then line fitting. If neither scheme
 * is available, battery reads still work through a raw conversion fallback.
 */
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_cali_handle_t *out_handle)
{
    esp_err_t err = ESP_FAIL;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, out_handle);
    if (err == ESP_OK) {
        return true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_line_fitting(&cali_cfg, out_handle);
    if (err == ESP_OK) {
        return true;
    }
#endif

    *out_handle = NULL;
    return false;
}

/**
 * Initialize ADC1 channels used by the volume pot and battery divider.
 *
 * GPIO34 and GPIO35 are input-only pins, which makes them good ADC choices for
 * this board. The function sets s_adc_ready only after both channels are usable.
 */
static esp_err_t adc_init_all(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_adc_unit);
    if (err != ESP_OK) {
        return err;
    }

    adc_unit_t unit = ADC_UNIT_1;
    err = adc_oneshot_io_to_channel(POT_PIN, &unit, &s_pot_channel);
    if (err != ESP_OK) {
        return err;
    }
    err = adc_oneshot_io_to_channel(BAT_PIN, &unit, &s_bat_channel);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_unit, s_pot_channel, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = adc_oneshot_config_channel(s_adc_unit, s_bat_channel, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_bat_cali_ok = adc_calibration_init(ADC_UNIT_1, s_bat_channel, &s_bat_cali);
    s_adc_ready = true;
    return ESP_OK;
}

/**
 * Create the jitter-buffer mutex and zero all buffer state.
 *
 * The jitter buffer sits between radio packet arrival and speaker playback so
 * small ESP-NOW timing bursts do not directly become speaker underruns.
 */
static void jitter_init(jitter_buffer_t *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->mutex = xSemaphoreCreateMutex();
}

/**
 * Drop all queued receive audio and reset packet-history tracking.
 *
 * Called when changing channels, beginning local transmit, or leaving receive
 * mode. Without this, old frames from a previous channel could leak into new
 * playback.
 */
static void jitter_reset(jitter_buffer_t *buffer)
{
    xSemaphoreTake(buffer->mutex, portMAX_DELAY);
    buffer->rd = 0;
    buffer->wr = 0;
    buffer->count = 0;
    buffer->started = false;
    buffer->have_last_good = false;
    buffer->have_last_seq = false;
    buffer->last_seq = 0;
    buffer->last_rx_tick = 0;
    memset(buffer->last_good, 0, sizeof(buffer->last_good));
    xSemaphoreGive(buffer->mutex);
}

/**
 * Push one decoded PCM frame into the receive jitter buffer.
 *
 * If the buffer is full, the oldest frame is dropped so the speaker stays close
 * to real time instead of building latency. The newest good frame and sequence
 * are retained for packet-loss concealment.
 */
static void jitter_push_frame(jitter_buffer_t *buffer, const int16_t *frame, uint16_t seq, TickType_t now_tick)
{
    xSemaphoreTake(buffer->mutex, portMAX_DELAY);
    if (buffer->count >= RX_QUEUE_FRAMES) {
        buffer->rd = (buffer->rd + 1) % RX_QUEUE_FRAMES;
        buffer->count--;
    }

    memcpy(buffer->frames[buffer->wr], frame, sizeof(buffer->frames[buffer->wr]));
    buffer->wr = (buffer->wr + 1) % RX_QUEUE_FRAMES;
    buffer->count++;
    memcpy(buffer->last_good, frame, sizeof(buffer->last_good));
    buffer->have_last_good = true;
    buffer->have_last_seq = true;
    buffer->last_seq = seq;
    buffer->last_rx_tick = now_tick;
    xSemaphoreGive(buffer->mutex);
}

/**
 * Pop the next PCM frame for speaker playback.
 *
 * Returns false when the buffer is empty. playback_task() then decides whether
 * to synthesize a short concealment frame or output silence.
 */
static bool jitter_pop_frame(jitter_buffer_t *buffer, int16_t *out_frame)
{
    bool ok = false;
    xSemaphoreTake(buffer->mutex, portMAX_DELAY);
    if (buffer->count > 0) {
        memcpy(out_frame, buffer->frames[buffer->rd], sizeof(buffer->frames[buffer->rd]));
        buffer->rd = (buffer->rd + 1) % RX_QUEUE_FRAMES;
        buffer->count--;
        ok = true;
    }
    xSemaphoreGive(buffer->mutex);
    return ok;
}

/**
 * Return the number of queued PCM frames.
 *
 * playback_task() waits for RX_PREFILL_FRAMES before starting audio so packets
 * that arrive slightly unevenly still play at a steady 20 ms cadence.
 */
static int jitter_count(jitter_buffer_t *buffer)
{
    int count;
    xSemaphoreTake(buffer->mutex, portMAX_DELAY);
    count = buffer->count;
    xSemaphoreGive(buffer->mutex);
    return count;
}

/**
 * Copy selected jitter-buffer status fields under the buffer mutex.
 *
 * The many nullable output pointers let callers ask only for the pieces they
 * need, avoiding several separate lock/unlock cycles in timing-sensitive audio
 * paths.
 */
static void jitter_get_status(jitter_buffer_t *buffer,
                              bool *started,
                              bool *have_last_good,
                              uint16_t *last_seq,
                              bool *have_last_seq,
                              TickType_t *last_rx_tick,
                              int16_t *last_good)
{
    xSemaphoreTake(buffer->mutex, portMAX_DELAY);
    if (started) {
        *started = buffer->started;
    }
    if (have_last_good) {
        *have_last_good = buffer->have_last_good;
    }
    if (last_seq) {
        *last_seq = buffer->last_seq;
    }
    if (have_last_seq) {
        *have_last_seq = buffer->have_last_seq;
    }
    if (last_rx_tick) {
        *last_rx_tick = buffer->last_rx_tick;
    }
    if (last_good && buffer->have_last_good) {
        memcpy(last_good, buffer->last_good, sizeof(buffer->last_good));
    }
    xSemaphoreGive(buffer->mutex);
}

/**
 * Mark whether the jitter buffer has enough prefill to begin playback.
 *
 * This is separate from count so playback_task() can stop cleanly after an
 * underrun and then wait for a fresh prefill before restarting audio output.
 */
static void jitter_set_started(jitter_buffer_t *buffer, bool started)
{
    xSemaphoreTake(buffer->mutex, portMAX_DELAY);
    buffer->started = started;
    xSemaphoreGive(buffer->mutex);
}

/**
 * Return the size of a file, or 0 when it does not exist.
 */
static size_t field_log_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) {
        return (size_t)st.st_size;
    }
    return 0;
}

/**
 * Keep the onboard JSONL log bounded.
 *
 * SPIFFS handles wear leveling, while this simple two-file rotation keeps field
 * logs from filling the partition. The dump command prints the previous file
 * first, then the current file, preserving useful time order.
 */
static void field_log_rotate_if_needed(void)
{
    /* Rotation is checked before every append. At 1 line/second this is cheap,
     * and it prevents an all-at-once cleanup pause during a field test. */
    if (field_log_file_size(FIELD_LOG_CURRENT_PATH) < FIELD_LOG_MAX_FILE_BYTES) {
        return;
    }

    /* Keep only one previous file. If rename fails for any reason, deleting the
     * current file is safer than letting SPIFFS fill and block future logs. */
    remove(FIELD_LOG_PREVIOUS_PATH);
    if (rename(FIELD_LOG_CURRENT_PATH, FIELD_LOG_PREVIOUS_PATH) != 0) {
        remove(FIELD_LOG_CURRENT_PATH);
    }
}

/**
 * Append one JSON line to the SPIFFS-backed field log.
 */
static void field_log_write_line(const char *line)
{
    if (!s_field_log_ready) {
        return;
    }

    field_log_rotate_if_needed();

    /* Open/append/close once per second. That is slower than keeping the file
     * open, but safer if power is removed during an outdoor range test. */
    FILE *file = fopen(FIELD_LOG_CURRENT_PATH, "a");
    if (file == NULL) {
        return;
    }

    fputs(line, file);
    fclose(file);
}

/**
 * Queue one JSON line for asynchronous flash logging.
 *
 * The control task never writes flash directly. If flash is busy and this queue
 * fills, the newest line is dropped instead of risking audio timing.
 */
static void field_log_enqueue_line(const char *line)
{
    if (!s_field_log_ready || s_field_log_queue == NULL) {
        return;
    }

    field_log_line_t item = {0};
    strlcpy(item.line, line, sizeof(item.line));

    /* Non-blocking by design: if the flash writer falls behind, audio and radio
     * continue and this one telemetry sample is simply not persisted. */
    (void)xQueueSend(s_field_log_queue, &item, 0);
}

/**
 * Low-priority task that performs actual SPIFFS appends.
 */
static void field_log_task(void *arg)
{
    (void)arg;
    field_log_line_t item;

    while (1) {
        if (xQueueReceive(s_field_log_queue, &item, portMAX_DELAY) == pdTRUE) {
            field_log_write_line(item.line);
        }
    }
}

/**
 * Mount the onboard SPIFFS partition used for range-test logs.
 */
static esp_err_t field_log_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = FIELD_LOG_MOUNT_PATH,
        .partition_label = "fieldlog",
        .max_files = 3,

        /* First boot after flashing a new partition table needs formatting, and
         * a corrupted log partition should not stop the walkie from booting. */
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "field log mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info("fieldlog", &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "field log SPIFFS: %u/%u bytes used", (unsigned)used, (unsigned)total);
    }

    s_field_log_queue = xQueueCreate(FIELD_LOG_QUEUE_DEPTH, sizeof(field_log_line_t));
    if (s_field_log_queue == NULL) {
        ESP_LOGW(TAG, "field log queue alloc failed");
        esp_vfs_spiffs_unregister("fieldlog");
        return ESP_ERR_NO_MEM;
    }

    s_field_log_ready = true;
    return ESP_OK;
}

/**
 * Print one stored JSONL file to USB serial.
 */
static void field_log_dump_file(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }

    char line[FIELD_LOG_LINE_BYTES];
    while (fgets(line, sizeof(line), file) != NULL) {
        fputs(line, stdout);
    }
    fclose(file);
}

/**
 * Dump onboard range logs over USB serial for later extraction.
 */
static void field_log_dump_over_serial(void)
{
    if (!s_field_log_ready) {
        printf("{\"event\":\"field_log_unavailable\"}\n");
        fflush(stdout);
        return;
    }

    /* Markers let a script extract exactly the stored field-test records from a
     * normal ESP-IDF monitor stream that also contains boot logs. */
    printf("{\"event\":\"field_log_dump_begin\",\"current_bytes\":%u,\"previous_bytes\":%u}\n",
           (unsigned)field_log_file_size(FIELD_LOG_CURRENT_PATH),
           (unsigned)field_log_file_size(FIELD_LOG_PREVIOUS_PATH));
    field_log_dump_file(FIELD_LOG_PREVIOUS_PATH);
    field_log_dump_file(FIELD_LOG_CURRENT_PATH);
    printf("{\"event\":\"field_log_dump_end\"}\n");
    fflush(stdout);
}

/**
 * Dump stored field logs when the user intentionally holds PTT + bottom-left.
 *
 * GPIO0 is the OK button and also an ESP32 boot strap, so the dump gesture avoids
 * OK. Hold PTT plus bottom-left while the firmware starts, then capture the USB
 * serial output.
 */
static void field_log_dump_if_requested(void)
{
    if (gpio_get_level(s_board.ptt_pin) == 0 && gpio_get_level(BOT_LEFT_PIN) == 0) {
        ESP_LOGI(TAG, "PTT + bottom-left held: dumping onboard field log");
        field_log_dump_over_serial();
    }
}

/**
 * Emit one JSON-line telemetry record over USB serial.
 *
 * Use `idf.py monitor | tee range-test.jsonl` while walking away from the other
 * radio. Each line is standalone JSON, making it easy to filter or plot packet
 * loss, duplicates, jitter depth, RSSI, and send errors after a range test.
 */
static void debug_emit_json_line(TickType_t now_tick)
{
    radio_debug_stats_t stats = debug_stats_take_snapshot();
    char line[FIELD_LOG_LINE_BYTES];
    int jitter_depth = jitter_count(&s_jitter);
    const char *board_label;
    bool ptt_down;
    bool link_on;
    int channel;
    int quality;
    int rssi;
    int volume;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    board_label = s_board.label;
    ptt_down = s_app.ptt_down;
    link_on = link_is_connected_locked(now_tick);
    channel = s_app.current_channel;
    quality = s_app.link_quality_pct;
    rssi = link_on ? s_app.link_rssi_dbm : LINK_RSSI_UNKNOWN_DBM;
    volume = s_app.extra.eff_vol_percent;
    xSemaphoreGive(s_state_lock);

    /* One line contains the full one-second aggregate. Keeping it as a single
     * JSON object makes the onboard file robust if extraction starts mid-log. */
    snprintf(line, sizeof(line),
             "{\"event\":\"radio_stats\",\"t_ms\":%" PRId64
             ",\"board\":\"%s\",\"ch\":%d,\"ptt\":%s,\"link\":%s"
             ",\"rssi_dbm\":%d,\"quality_pct\":%d,\"jitter_frames\":%d"
             ",\"vol_pct\":%d,\"tx_audio\":%" PRIu32 ",\"tx_audio_dup\":%" PRIu32
             ",\"tx_ctrl\":%" PRIu32 ",\"tx_no_mem\":%" PRIu32 ",\"tx_fail\":%" PRIu32
             ",\"rx_audio\":%" PRIu32 ",\"rx_audio_dup\":%" PRIu32
             ",\"rx_audio_old\":%" PRIu32 ",\"rx_plc\":%" PRIu32
             ",\"rx_ctrl\":%" PRIu32 ",\"rx_wrong_peer\":%" PRIu32
             ",\"rx_bad_proto\":%" PRIu32 ",\"rx_wrong_channel\":%" PRIu32 "}\n",
             esp_timer_get_time() / 1000,
             board_label ? board_label : "?",
             channel,
             ptt_down ? "true" : "false",
             link_on ? "true" : "false",
             rssi,
             quality,
             jitter_depth,
             volume,
             stats.tx_audio,
             stats.tx_audio_dup,
             stats.tx_ctrl,
             stats.tx_no_mem,
             stats.tx_fail,
             stats.rx_audio,
             stats.rx_audio_dup,
             stats.rx_audio_old,
             stats.rx_plc,
             stats.rx_ctrl,
             stats.rx_wrong_peer,
             stats.rx_bad_proto,
             stats.rx_wrong_channel);

    /* Serial output is still useful at the bench; flash persistence is what lets
     * the same record survive real long-distance testing without a computer. */
    fputs(line, stdout);
    fflush(stdout);
    field_log_enqueue_line(line);
}

/**
 * Try to allocate and configure one TX/RX I2S port pairing.
 *
 * ESP32 has two I2S peripherals. Speaker output and microphone input need
 * separate channels, so i2s_init_pair() may try both port orders until one works.
 */
static esp_err_t i2s_try_pair(int tx_port, int rx_port)
{
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(tx_port, I2S_ROLE_MASTER);
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(rx_port, I2S_ROLE_MASTER);
    tx_cfg.dma_desc_num = 8;
    tx_cfg.dma_frame_num = 256;
    rx_cfg.dma_desc_num = 8;
    rx_cfg.dma_frame_num = 256;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_cfg, &s_i2s_tx, NULL), TAG, "tx i2s_new_channel failed");
    esp_err_t err = i2s_new_channel(&rx_cfg, NULL, &s_i2s_rx);
    if (err != ESP_OK) {
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        return err;
    }

    i2s_std_config_t tx_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK,
            .ws = SPK_WS,
            .dout = SPK_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    tx_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    i2s_std_config_t rx_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(s_i2s_tx, &tx_std);
    if (err != ESP_OK) {
        i2s_del_channel(s_i2s_tx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_tx = NULL;
        s_i2s_rx = NULL;
        return err;
    }
    err = i2s_channel_init_std_mode(s_i2s_rx, &rx_std);
    if (err != ESP_OK) {
        i2s_del_channel(s_i2s_tx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_tx = NULL;
        s_i2s_rx = NULL;
        return err;
    }

    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_rx));
    return ESP_OK;
}

/**
 * Initialize both I2S devices used by the walkie.
 *
 * The speaker uses 16-bit mono output. The mic uses 32-bit Philips/I2S capture
 * with the left slot because the hardware L/R pin is tied to GND.
 */
static esp_err_t i2s_init_pair(void)
{
    esp_err_t err = i2s_try_pair(I2S_NUM_0, I2S_NUM_1);
    if (err != ESP_OK) {
        err = i2s_try_pair(I2S_NUM_1, I2S_NUM_0);
    }

    if (err == ESP_OK) {
        s_i2s_tx_ready = (s_i2s_tx != NULL);
        s_i2s_rx_ready = (s_i2s_rx != NULL);
    }
    return err;
}

/**
 * Bring up Wi-Fi in station mode for ESP-NOW.
 *
 * Power save is disabled for lower latency, max TX power is requested, and the
 * Espressif LR protocol is enabled so peer packets can use the long-range PHY.
 */
static esp_err_t wifi_init_for_espnow(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_protocol(WIFI_IF_STA,
                                             WIFI_PROTOCOL_11B |
                                             WIFI_PROTOCOL_11G |
                                             WIFI_PROTOCOL_11N |
                                             WIFI_PROTOCOL_LR),
                        TAG, "wifi LR protocol failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_QDBM), TAG, "wifi tx power failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi ps failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(CONFIG_WALKIE_RF_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG, "wifi channel failed");
    return ESP_OK;
}

/**
 * ESP-NOW receive callback called from the Wi-Fi stack.
 *
 * This must stay small and non-blocking. It copies packet bytes, source MAC, and
 * RSSI metadata into a FreeRTOS queue; normal task code later validates the
 * packet and updates audio/UI state.
 */
static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    radio_rx_item_t item;

    if (info == NULL || data == NULL || len <= 0 || len > RADIO_RX_DATA_MAX) {
        return;
    }

    memset(&item, 0, sizeof(item));
    item.len = (size_t)len;
    item.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;
    memcpy(item.src_mac, info->src_addr, sizeof(item.src_mac));
    memcpy(item.data, data, len);

    /* Keep the Wi-Fi callback tiny: copy data/RSSI into a queue and let the
     * main control loop do validation, UI updates, and audio decode work. */
    if (s_radio_rx_queue != NULL) {
        xQueueSend(s_radio_rx_queue, &item, 0);
    }
}

/**
 * Initialize ESP-NOW and register the single paired peer.
 *
 * The walkies are point-to-point, so only s_board.peer_mac is added. After the
 * peer exists, the code requests LR 250 Kbps for maximum range margin.
 */
static esp_err_t espnow_init_peer(void)
{
    s_radio_rx_queue = xQueueCreate(12, sizeof(radio_rx_item_t));
    if (s_radio_rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp-now init failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(espnow_recv_cb), TAG, "esp-now recv cb failed");

    esp_now_peer_info_t peer = {0};
    peer.channel = CONFIG_WALKIE_RF_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, s_board.peer_mac, sizeof(peer.peer_addr));
    ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "esp-now add peer failed");

    esp_now_rate_config_t rate_config = {
        .phymode = WIFI_PHY_MODE_LR,
        .rate = WIFI_PHY_RATE_LORA_250K,
        .ersu = false,
        .dcm = false,
    };
    esp_err_t rate_err = esp_now_set_peer_rate_config(s_board.peer_mac, &rate_config);
    if (rate_err != ESP_OK) {
        ESP_LOGW(TAG, "esp-now LR rate config failed, using default rate: %s", esp_err_to_name(rate_err));
    } else {
        ESP_LOGI(TAG, "ESP-NOW peer rate: LR 250 Kbps, TX power request: %.2f dBm",
                 (double)WIFI_MAX_TX_POWER_QDBM / 4.0);
    }

    s_radio_ready = true;
    return ESP_OK;
}

/**
 * Send a raw ESP-NOW packet to the paired peer.
 *
 * Transient ESP_ERR_ESPNOW_NO_MEM is ignored because voice sends again every
 * 20 ms; other errors are logged so radio problems are visible during debugging.
 */
static bool send_packet_raw(const void *packet, size_t len)
{
    if (!s_radio_ready) {
        return false;
    }

    esp_err_t err = esp_now_send(s_board.peer_mac, packet, len);
    if (err == ESP_ERR_ESPNOW_NO_MEM) {
        debug_stats_inc(DBG_TX_NO_MEM);
        return false;
    }
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_NO_MEM) {
        debug_stats_inc(DBG_TX_FAIL);
        ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/**
 * Send a compact non-audio control packet.
 *
 * Control packets are used for heartbeat/link detection and scan request/response.
 * They include the logical channel so both radios can share one RF channel while
 * still acting like they have 20 software channels.
 */
static void send_ctrl_packet(uint8_t type, uint8_t logical_channel, uint8_t flags, uint16_t seq)
{
    walkie_ctrl_packet_t packet = {
        .type = type,
        .version = PROTO_VERSION,
        .logical_channel = logical_channel,
        .flags = flags,
        .seq = seq,
    };
    debug_stats_inc(DBG_TX_CTRL);
    send_packet_raw(&packet, sizeof(packet));
}

/**
 * Compress and send one 20 ms PCM voice frame.
 *
 * IMA ADPCM packs two 16-bit PCM samples into one byte. The packet also carries
 * the encoder predictor/step state so the receiver can decode each frame even
 * after a short packet loss. extra_copies sends duplicate packets with the same
 * sequence number for weak links; the receiver de-duplicates them.
 */
static void send_audio_pcm_frame(const int16_t *pcm,
                                 uint8_t logical_channel,
                                 uint8_t flags,
                                 uint16_t seq,
                                 walkie_adpcm_state_t *codec_state,
                                 int extra_copies)
{
    walkie_audio_packet_t packet = {0};
    packet.type = PKT_AUDIO;
    packet.version = PROTO_VERSION;
    packet.logical_channel = logical_channel;
    packet.flags = flags;
    packet.seq = seq;
    packet.predictor = codec_state->predictor;
    packet.step_index = codec_state->step_index;
    packet.sample_count = SAMPLES_PER_FRAME;
    walkie_adpcm_encode_frame(pcm, SAMPLES_PER_FRAME, codec_state, packet.payload);

    /* Count the unique frame once, then count every redundant copy separately.
     * The duplicate uses the exact same seq/payload so the receiver can accept
     * whichever copy arrives first and discard the rest. */
    debug_stats_inc(DBG_TX_AUDIO);
    send_packet_raw(&packet, sizeof(packet));
    for (int copy = 0; copy < extra_copies; ++copy) {
        debug_stats_inc(DBG_TX_AUDIO_DUP);
        send_packet_raw(&packet, sizeof(packet));
    }
}

/**
 * Empty pending radio packets from the receive queue.
 *
 * Used when starting local transmit or scanning so stale audio/control packets
 * from the previous mode do not influence the new mode.
 */
static void drain_radio_rx_queue(void)
{
    if (s_radio_rx_queue == NULL) {
        return;
    }

    radio_rx_item_t item;
    while (xQueueReceive(s_radio_rx_queue, &item, 0) == pdTRUE) {
    }
}

/**
 * Mark the peer as recently seen and update smoothed RSSI.
 *
 * Every valid heartbeat, scan response, or audio packet calls this. The smoothed
 * RSSI drives the signal bars in the PTT screen.
 */
static void state_set_link_seen(TickType_t now_tick, int8_t rssi)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_app.last_link_tick = now_tick;

    /* ESP-NOW gives us packet RSSI in the receive callback. Smooth it so the
     * OLED meter shows useful trend instead of jumping every heartbeat. */
    if (rssi > LINK_RSSI_UNKNOWN_DBM && rssi < 0) {
        if (s_app.link_rssi_dbm <= LINK_RSSI_UNKNOWN_DBM) {
            s_app.link_rssi_dbm = rssi;
        } else {
            s_app.link_rssi_dbm = ((s_app.link_rssi_dbm * 7) + rssi) / 8;
        }
        s_app.link_quality_pct = walkie_clamp_int(link_quality_from_rssi(s_app.link_rssi_dbm), 0, 100);
    }

    xSemaphoreGive(s_state_lock);
}

/**
 * Decode an accepted audio packet and push it toward playback.
 *
 * Sequence numbers are checked before decode. If one or two packets are missing,
 * packet-loss concealment frames are inserted so the speaker does not click or
 * abruptly drop to silence.
 */
static void handle_decoded_audio_packet(const walkie_audio_packet_t *packet, TickType_t now_tick, int8_t rssi)
{
    walkie_adpcm_state_t decode_state = {
        .predictor = packet->predictor,
        .step_index = packet->step_index,
    };
    int16_t frame[SAMPLES_PER_FRAME];
    int16_t last_good[SAMPLES_PER_FRAME];
    bool have_last_good = false;
    bool have_last_seq = false;
    uint16_t last_seq = 0;

    jitter_get_status(&s_jitter, NULL, &have_last_good, &last_seq, &have_last_seq, NULL, last_good);

    if (have_last_seq) {
        int gap = audio_seq_delta(packet->seq, last_seq);
        if (gap == 0) {
            /* This is the expected path for redundancy: the first copy already
             * made it into the jitter buffer, so later copies update link RSSI
             * but do not play again. */
            debug_stats_inc(DBG_RX_AUDIO_DUP);
            state_set_link_seen(now_tick, rssi);
            return;
        }
        if (gap < 0) {
            /* A very delayed packet would sound like a time-travel click. Count
             * it for diagnosis and keep playback moving forward. */
            debug_stats_inc(DBG_RX_AUDIO_OLD);
            state_set_link_seen(now_tick, rssi);
            return;
        }
        if (gap > 1 && gap < 4 && have_last_good) {
            /* For short gaps, insert synthetic fade frames before the real one.
             * Longer gaps are left as silence by playback_task() to avoid huge
             * latency jumps after a bad RF fade. */
            for (int loss = 1; loss < gap; ++loss) {
                int16_t plc[SAMPLES_PER_FRAME];
                walkie_generate_plc_frame(last_good, plc, SAMPLES_PER_FRAME, loss);
                jitter_push_frame(&s_jitter, plc, (uint16_t)(last_seq + loss), now_tick);
                debug_stats_inc(DBG_RX_PLC);
            }
        }
    }

    walkie_adpcm_decode_frame(packet->payload, SAMPLES_PER_FRAME, &decode_state, frame);
    jitter_push_frame(&s_jitter, frame, packet->seq, now_tick);
    debug_stats_inc(DBG_RX_AUDIO);
    state_set_link_seen(now_tick, rssi);
}

/**
 * Check whether a packet belongs to this unit's current logical channel/mode.
 *
 * RF channel is fixed by Wi-Fi, while logical channels are carried inside every
 * packet. This lets both walkies ignore packets for the wrong software channel.
 */
static bool packet_matches_current_comm(uint8_t logical_channel, uint8_t flags)
{
    bool matched = false;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    uint8_t my_channel = 0;
    uint8_t my_flags = 0;
    if (active_comm_locked(&my_channel, &my_flags) && logical_channel == my_channel && flags == my_flags) {
        matched = true;
    }
    xSemaphoreGive(s_state_lock);
    return matched;
}

/**
 * Validate and handle one queued ESP-NOW receive item.
 *
 * This is the central radio dispatcher. It rejects wrong peers/protocol versions,
 * updates link state for control packets, responds to scan requests, and feeds
 * valid audio into the jitter buffer when receiving is allowed.
 */
static bool handle_radio_item(const radio_rx_item_t *item, TickType_t now_tick, bool allow_audio)
{
    if (item->len < sizeof(walkie_ctrl_packet_t)) {
        debug_stats_inc(DBG_RX_BAD_PROTO);
        return false;
    }

    if (memcmp(item->src_mac, s_board.peer_mac, sizeof(s_board.peer_mac)) != 0) {
        debug_stats_inc(DBG_RX_WRONG_PEER);
        return false;
    }

    uint8_t type = item->data[0];
    if (item->data[1] != PROTO_VERSION) {
        debug_stats_inc(DBG_RX_BAD_PROTO);
        return false;
    }

    if (type == PKT_HEART) {
        const walkie_ctrl_packet_t *packet = (const walkie_ctrl_packet_t *)item->data;
        if (packet_matches_current_comm(packet->logical_channel, packet->flags)) {
            debug_stats_inc(DBG_RX_CTRL);
            state_set_link_seen(now_tick, item->rssi);
        } else {
            debug_stats_inc(DBG_RX_WRONG_CHANNEL);
        }
        return false;
    }

    if (type == PKT_SCAN_REQ) {
        const walkie_ctrl_packet_t *packet = (const walkie_ctrl_packet_t *)item->data;
        if (packet_matches_current_comm(packet->logical_channel, packet->flags)) {
            debug_stats_inc(DBG_RX_CTRL);
            send_ctrl_packet(PKT_SCAN_RESP, packet->logical_channel, packet->flags, packet->seq);
        } else {
            debug_stats_inc(DBG_RX_WRONG_CHANNEL);
        }
        return false;
    }

    if (type == PKT_SCAN_RESP) {
        const walkie_ctrl_packet_t *packet = (const walkie_ctrl_packet_t *)item->data;
        if (packet_matches_current_comm(packet->logical_channel, packet->flags)) {
            debug_stats_inc(DBG_RX_CTRL);
            state_set_link_seen(now_tick, item->rssi);
        } else {
            debug_stats_inc(DBG_RX_WRONG_CHANNEL);
        }
        return true;
    }

    if (type == PKT_AUDIO && allow_audio && item->len == sizeof(walkie_audio_packet_t)) {
        const walkie_audio_packet_t *packet = (const walkie_audio_packet_t *)item->data;
        if (packet_matches_current_comm(packet->logical_channel, packet->flags)) {
            handle_decoded_audio_packet(packet, now_tick, item->rssi);
        } else {
            debug_stats_inc(DBG_RX_WRONG_CHANNEL);
        }
    }

    return false;
}

/**
 * Move the settings selection one item upward.
 *
 * Callers must hold s_state_lock. The index wraps so the tiny screen can use
 * simple up/down controls without dead ends.
 */
static void settings_move_up_locked(void)
{
    s_app.extra.settings_index = walkie_wrap_index(s_app.extra.settings_index - 1, SETTING_COUNT);
}

/**
 * Move the settings selection one item downward.
 *
 * Callers must hold s_state_lock. This mirrors settings_move_up_locked().
 */
static void settings_move_down_locked(void)
{
    s_app.extra.settings_index = walkie_wrap_index(s_app.extra.settings_index + 1, SETTING_COUNT);
}

/**
 * Toggle the currently selected setting.
 *
 * Mic boost and mic cut are mutually exclusive. Display-only resource/version
 * rows do nothing when OK is pressed. The log dump row is handled by
 * control_task() outside the state lock because it prints a lot of serial data.
 *
 * Callers must hold s_state_lock.
 */
static void toggle_setting_locked(void)
{
    switch (s_app.extra.settings_index) {
    case SET_LIMIT60:
        s_app.extra.set_limit60 = !s_app.extra.set_limit60;
        break;
    case SET_LIMIT60_LOWBAT:
        s_app.extra.set_limit60_lowbat = !s_app.extra.set_limit60_lowbat;
        break;
    case SET_SPK_BOOST:
        s_app.extra.set_spk_boost = !s_app.extra.set_spk_boost;
        break;
    case SET_MIC_BOOST:
        s_app.extra.set_mic_boost = !s_app.extra.set_mic_boost;
        if (s_app.extra.set_mic_boost) {
            s_app.extra.set_mic_cut = false;
        }
        break;
    case SET_MIC_CUT:
        s_app.extra.set_mic_cut = !s_app.extra.set_mic_cut;
        if (s_app.extra.set_mic_cut) {
            s_app.extra.set_mic_boost = false;
        }
        break;
    case SET_CPU_OVERLAY:
        s_app.extra.show_cpu_usage = !s_app.extra.show_cpu_usage;
        break;
    case SET_FLASH_USAGE:
    case SET_MEMORY_USAGE:
    case SET_FIRMWARE_VERSION:
    case SET_LOG_DUMP:
    default:
        break;
    }

    apply_audio_settings_locked();
}

/**
 * Move the LIGHTS app selection upward.
 *
 * Callers must hold s_state_lock.
 */
static void lights_move_up_locked(void)
{
    s_app.extra.lights_index = walkie_wrap_index(s_app.extra.lights_index - 1, LIGHT_COUNT);
}

/**
 * Move the LIGHTS app selection downward.
 *
 * Callers must hold s_state_lock.
 */
static void lights_move_down_locked(void)
{
    s_app.extra.lights_index = walkie_wrap_index(s_app.extra.lights_index + 1, LIGHT_COUNT);
}

/**
 * Return true when the currently selected LIGHTS row edits strobe rate.
 *
 * The control task uses this to make held top-left presses repeat as rate
 * changes instead of only moving the menu cursor.
 */
static bool lights_rate_selected_locked(void)
{
    return s_app.extra.lights_index == LIGHT_RATE;
}

/**
 * Increment strobe rate and wrap at the supported maximum.
 *
 * Callers must hold s_state_lock. The UI presents this as 1-40 Hz.
 */
static void lights_increment_rate_locked(void)
{
    s_app.extra.lights_strobe_hz++;
    if (s_app.extra.lights_strobe_hz > 40) {
        s_app.extra.lights_strobe_hz = 1;
    }
}

/**
 * Toggle one of the pattern-based light modes.
 *
 * Enabling a preset disables constant LED/laser modes so the output behavior
 * remains unambiguous.
 */
static void lights_toggle_mode_locked(int mode_id)
{
    if (s_app.extra.lights_mode == mode_id) {
        s_app.extra.lights_mode = 0;
    } else {
        s_app.extra.lights_mode = mode_id;
        s_app.extra.lights_led_const = false;
        s_app.extra.lights_laser_const = false;
    }
}

/**
 * Apply OK-button behavior for the currently selected LIGHTS row.
 *
 * Some rows cycle values, some toggle booleans, and the RATE row increments.
 * This keeps the control-task state machine compact.
 */
static void lights_ok_action_locked(void)
{
    switch (s_app.extra.lights_index) {
    case LIGHT_STROBE:
        lights_toggle_mode_locked(1);
        break;
    case LIGHT_TARGET:
        s_app.extra.lights_target = (s_app.extra.lights_target + 1) % 3;
        break;
    case LIGHT_RATE:
        lights_increment_rate_locked();
        break;
    case LIGHT_LED_CONST:
        s_app.extra.lights_mode = 0;
        s_app.extra.lights_led_const = !s_app.extra.lights_led_const;
        break;
    case LIGHT_LASER_CONST:
        s_app.extra.lights_mode = 0;
        s_app.extra.lights_laser_const = !s_app.extra.lights_laser_const;
        break;
    case LIGHT_PRE1:
        lights_toggle_mode_locked(2);
        break;
    case LIGHT_PRE2:
        lights_toggle_mode_locked(3);
        break;
    case LIGHT_PRE3:
    default:
        lights_toggle_mode_locked(4);
        break;
    }
}

/**
 * Compute LED and laser output states for the LIGHTS app.
 *
 * This function is pure with respect to hardware: it only reads state and time,
 * then returns what the GPIOs should be doing. update_outputs() applies the
 * result to the physical pins.
 *
 * Callers must hold s_state_lock.
 */
static walkie_light_outputs_t compute_light_outputs_locked(int64_t now_ms)
{
    walkie_light_outputs_t outputs = {0};
    int mode = s_app.extra.lights_mode;

    if (mode == 0) {
        outputs.led_on = s_app.extra.lights_led_const;
        outputs.laser_on = s_app.extra.lights_laser_const;
        return outputs;
    }

    if (mode == 1) {
        int hz = s_app.extra.lights_strobe_hz > 0 ? s_app.extra.lights_strobe_hz : 1;
        bool on = ((((now_ms * hz * 2) / 1000) & 1) == 0);
        if (s_app.extra.lights_target == LIGHT_TARGET_LED) {
            outputs.led_on = on;
        } else if (s_app.extra.lights_target == LIGHT_TARGET_LASER) {
            outputs.laser_on = on;
        } else {
            outputs.led_on = on;
            outputs.laser_on = on;
        }
        return outputs;
    }

    if (mode == 2) {
        int t = now_ms % 1400;
        bool pulse = (t < 50) || (t >= 100 && t < 150) || (t >= 220 && t < 270);
        outputs.led_on = pulse;
        outputs.laser_on = pulse;
        return outputs;
    }

    if (mode == 3) {
        outputs.led_on = (((now_ms / 80) & 1) == 0);
        outputs.laser_on = (((now_ms / 300) & 1) == 0);
        return outputs;
    }

    if (mode == 4) {
        int t = now_ms % 900;
        outputs.led_on = (t < 50) || (t >= 90 && t < 140) || (t >= 180 && t < 230) || (t >= 400 && t < 450);
        outputs.laser_on = false;
    }

    return outputs;
}

/**
 * Drive the physical LED and laser outputs for the current mode.
 *
 * In PTT mode the LED indicates transmit and can blink on quiet receive. In the
 * LIGHTS app the user-selected pattern owns both LED and laser pins.
 */
static void update_outputs(int64_t now_ms)
{
    bool talking;
    bool led_on;
    bool laser_on;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    talking = ((s_app.ui_mode == MODE_PTT || s_app.ui_mode == MODE_KID) && s_app.ptt_down);

    if (s_app.ui_mode == MODE_APP_VIEW && s_app.selected_app == APP_LIGHTS) {
        walkie_light_outputs_t lights = compute_light_outputs_locked(now_ms);
        led_on = lights.led_on;
        laser_on = lights.laser_on;
    } else {
        if (talking) {
            led_on = true;
        } else if (s_app.ui_mode == MODE_PTT &&
                   s_app.extra.eff_vol_percent < 10 &&
                   now_ms < s_app.rx_led_until_ms) {
            led_on = (((now_ms / 90) & 1) == 0);
        } else {
            led_on = false;
        }

        laser_on = (s_app.ui_mode == MODE_PTT) ? s_app.laser_on : false;
    }
    xSemaphoreGive(s_state_lock);

    gpio_set_level(s_board.led_pin, led_on ? 1 : 0);
    gpio_set_level(LASER_PIN, laser_on ? 1 : 0);
}

/**
 * Initialize all application state to safe defaults after hardware init.
 *
 * This reads the current pot/battery, sets PTT mode on channel 1, enables the
 * low-battery volume limiter, and computes derived audio/resource fields.
 */
static void set_state_defaults(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    TickType_t now_tick = xTaskGetTickCount();

    memset(&s_app, 0, sizeof(s_app));
    s_app.ui_mode = MODE_PTT;
    s_app.selected_app = 0;
    s_app.current_channel = 1;
    s_app.extra.set_limit60_lowbat = true;
    s_app.extra.lights_target = LIGHT_TARGET_BOTH;
    s_app.extra.lights_strobe_hz = 8;
    s_app.base_vol_percent = read_pot_percent();
    s_app.vbat = read_battery_voltage();
    s_app.batt_pct = walkie_clamp_int(pct_curve_from_vbat(s_app.vbat), 0, 100);
    s_app.link_rssi_dbm = LINK_RSSI_UNKNOWN_DBM;
    s_app.link_quality_pct = 0;
    s_app.last_pot_tick = now_tick;
    s_app.last_batt_tick = now_tick;
    s_app.last_oled_tick = now_tick;
    s_app.last_stats_tick = 0;
    s_app.extra.big_knob_start_ms = 0;
    s_app.extra.lights_rate_repeat_next_ms = 0;
    apply_audio_settings_locked();
    update_resource_stats_locked(now_tick);

    s_app.rx_led_until_ms = now_ms;
}

/**
 * Send periodic heartbeats when the user is not transmitting.
 *
 * Heartbeats keep LINK ON/OFF and the RSSI meter updated even when nobody is
 * speaking. They use the current logical channel so both units must be on the
 * same channel to show linked.
 */
static void maybe_send_heartbeat(TickType_t now_tick)
{
    uint8_t logical_channel = 0;
    uint8_t flags = 0;
    bool should_send = false;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (!s_app.ptt_down &&
        active_comm_locked(&logical_channel, &flags) &&
        (now_tick - s_app.last_heart_tick) >= pdMS_TO_TICKS(HEARTBEAT_MS)) {
        s_app.last_heart_tick = now_tick;
        should_send = true;
    }
    xSemaphoreGive(s_state_lock);

    if (should_send) {
        send_ctrl_packet(PKT_HEART, logical_channel, flags, 0);
    }
}

/**
 * Scan logical channels for the peer.
 *
 * This temporarily shows the scanning UI, sends a scan request on each software
 * channel, and waits briefly for a matching response. On success the walkie
 * stays on the found channel; otherwise it restores the old channel.
 */
static bool auto_scan_channels(void)
{
    if (!s_radio_ready || s_radio_rx_queue == NULL) {
        return false;
    }

    radio_rx_item_t item;
    int old_mode;
    int old_channel;
    bool found = false;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    old_mode = s_app.ui_mode;
    old_channel = s_app.current_channel;
    s_app.ui_mode = MODE_SCANNING;
    xSemaphoreGive(s_state_lock);

    jitter_reset(&s_jitter);
    drain_radio_rx_queue();

    for (int logical_channel = LOGICAL_CHANNEL_MIN; logical_channel <= LOGICAL_CHANNEL_MAX; ++logical_channel) {
        walkie_ui_snapshot_t snapshot;

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_app.current_channel = logical_channel;
        state_copy_snapshot_locked(&snapshot, xTaskGetTickCount(), esp_timer_get_time() / 1000);
        xSemaphoreGive(s_state_lock);

        walkie_display_redraw(&s_display, &snapshot);
        send_ctrl_packet(PKT_SCAN_REQ, (uint8_t)logical_channel, 0, 0);

        TickType_t start_tick = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(SCAN_WAIT_MS)) {
            if (xQueueReceive(s_radio_rx_queue, &item, pdMS_TO_TICKS(4)) != pdTRUE) {
                continue;
            }

            if (memcmp(item.src_mac, s_board.peer_mac, sizeof(s_board.peer_mac)) != 0 ||
                item.len < sizeof(walkie_ctrl_packet_t)) {
                continue;
            }

            const walkie_ctrl_packet_t *packet = (const walkie_ctrl_packet_t *)item.data;
            if (packet->type == PKT_SCAN_RESP &&
                packet->version == PROTO_VERSION &&
                packet->logical_channel == logical_channel &&
                packet->flags == 0) {
                state_set_link_seen(xTaskGetTickCount(), item.rssi);
                found = true;
                break;
            }
        }

        if (found) {
            break;
        }
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_app.ui_mode = (walkie_ui_mode_t)old_mode;
    if (!found) {
        s_app.current_channel = old_channel;
    }
    xSemaphoreGive(s_state_lock);

    return found;
}

/**
 * Send a short run of silent audio frames around PTT transitions.
 *
 * These frames prime or drain the receiver-side jitter buffer so the first and
 * last audible speech frames do not pop or get truncated.
 */
static void send_transition_silence(uint8_t logical_channel,
                                    uint8_t flags,
                                    uint16_t *seq,
                                    walkie_adpcm_state_t *codec_state,
                                    int frames,
                                    int extra_copies)
{
    int16_t silence[SAMPLES_PER_FRAME] = {0};
    for (int i = 0; i < frames; ++i) {
        send_audio_pcm_frame(silence, logical_channel, flags, *seq, codec_state, extra_copies);
        (*seq)++;
    }
}

/**
 * FreeRTOS task that records, processes, compresses, and transmits voice.
 *
 * While PTT is down, one 20 ms I2S mic frame is read each loop, converted to
 * PCM, filtered/gained by walkie_process_mic_frame(), ADPCM encoded, and sent as
 * one ESP-NOW audio packet.
 */
static void capture_task(void *arg)
{
    (void)arg;
    int32_t raw_i2s[SAMPLES_PER_FRAME];
    int16_t pcm[SAMPLES_PER_FRAME];
    walkie_mic_proc_state_t mic_state;
    walkie_adpcm_state_t codec_state;
    uint16_t seq = 0;
    bool was_talking = false;
    uint8_t last_tx_channel = LOGICAL_CHANNEL_MIN;
    uint8_t last_tx_flags = 0;

    /* The end-of-PTT silence is sent after s_app.ptt_down has gone false, so the
     * task remembers the last active channel/flags/redundancy choice. */
    int last_tx_extra_copies = 0;

    walkie_mic_proc_reset(&mic_state);
    walkie_adpcm_reset(&codec_state);

    /* Transmit side: only runs while PTT is held. Each loop reads one 20 ms mic
     * frame, applies the selected mic calibration, ADPCM-compresses it, and sends
     * it on the current logical channel. */
    while (1) {
        if (!s_radio_ready || !s_i2s_rx_ready || s_i2s_rx == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool talking;
        uint8_t logical_channel = 0;
        uint8_t flags = 0;
        int mic_mode = 0;
        int mic_gain_q10 = 1024;
        int extra_copies = 0;
        TickType_t now_tick = xTaskGetTickCount();

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        talking = ((s_app.ui_mode == MODE_PTT || s_app.ui_mode == MODE_KID) && s_app.ptt_down);
        if (talking) {
            active_comm_locked(&logical_channel, &flags);
            mic_mode = s_app.extra.mic_manual_mode;
            mic_gain_q10 = s_app.extra.mic_gain_q10;

            /* Redundancy is selected per frame from current link quality. At
             * long range this usually becomes 1 extra copy; close range stays
             * lean at one packet per 20 ms frame. */
            extra_copies = audio_redundancy_extra_copies_locked(now_tick);
            last_tx_channel = logical_channel;
            last_tx_flags = flags;
            last_tx_extra_copies = extra_copies;
        }
        xSemaphoreGive(s_state_lock);

        if (!talking) {
            if (was_talking) {
                send_transition_silence(last_tx_channel, last_tx_flags, &seq, &codec_state, TX_END_FRAMES, last_tx_extra_copies);
                walkie_mic_proc_reset(&mic_state);
                walkie_adpcm_reset(&codec_state);
            }
            was_talking = false;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (!was_talking) {
            size_t warmup_bytes = 0;

            jitter_reset(&s_jitter);
            drain_radio_rx_queue();
            walkie_mic_proc_reset(&mic_state);
            walkie_adpcm_reset(&codec_state);

            /* Some I2S microphones produce unstable first samples after capture
             * starts. Discarding a couple of frames avoids sending that glitch. */
            for (int i = 0; i < MIC_WARMUP_FRAMES; ++i) {
                (void)i2s_channel_read(s_i2s_rx, raw_i2s, sizeof(raw_i2s), &warmup_bytes, pdMS_TO_TICKS(FRAME_MS));
            }

            send_transition_silence(logical_channel, flags, &seq, &codec_state, TX_PREAMBLE_FRAMES, extra_copies);
            was_talking = true;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_i2s_rx, raw_i2s, sizeof(raw_i2s), &bytes_read, 1000);
        if (err != ESP_OK || bytes_read != sizeof(raw_i2s)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        walkie_process_mic_frame(raw_i2s, pcm, SAMPLES_PER_FRAME, &mic_state, mic_mode, mic_gain_q10);
        send_audio_pcm_frame(pcm, logical_channel, flags, seq, &codec_state, extra_copies);
        seq++;
    }
}

/**
 * FreeRTOS task that turns received frames into steady speaker output.
 *
 * It runs at the same 20 ms cadence as transmit. The task waits for a small
 * prefill, plays queued frames, and generates short concealment audio if a
 * packet is late or lost.
 */
static void playback_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    int16_t frame[SAMPLES_PER_FRAME];
    int16_t last_good[SAMPLES_PER_FRAME];
    int loss_count = 0;
    bool last_frame_valid = false;

    memset(frame, 0, sizeof(frame));
    memset(last_good, 0, sizeof(last_good));

    /* Receive side: keep audio steady by pre-filling a small jitter buffer. If a
     * couple of packets are late or lost, synthesize short PLC frames instead of
     * letting the speaker click or underrun. */
    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FRAME_MS));

        if (!s_i2s_tx_ready || s_i2s_tx == NULL) {
            continue;
        }

        bool local_talking;
        bool comm_mode;
        int speaker_gain_q12;
        TickType_t last_rx_tick = 0;
        bool started = false;
        bool have_last_good = false;
        bool rx_audio_frame = false;

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        local_talking = ((s_app.ui_mode == MODE_PTT || s_app.ui_mode == MODE_KID) && s_app.ptt_down);
        comm_mode = (s_app.ui_mode == MODE_PTT || s_app.ui_mode == MODE_KID);
        speaker_gain_q12 = s_app.extra.speaker_gain_q12;
        xSemaphoreGive(s_state_lock);

        if (!comm_mode || local_talking) {
            jitter_reset(&s_jitter);
            loss_count = 0;
            last_frame_valid = false;
            continue;
        }

        if (!started && jitter_count(&s_jitter) >= RX_PREFILL_FRAMES) {
            jitter_set_started(&s_jitter, true);
        }

        jitter_get_status(&s_jitter, &started, &have_last_good, NULL, NULL, &last_rx_tick, last_good);
        if (jitter_pop_frame(&s_jitter, frame)) {
            loss_count = 0;
            last_frame_valid = true;
            rx_audio_frame = true;
            memcpy(last_good, frame, sizeof(frame));
        } else if (started && last_frame_valid && (xTaskGetTickCount() - last_rx_tick) <= pdMS_TO_TICKS(160)) {
            loss_count++;
            walkie_generate_plc_frame(last_good, frame, SAMPLES_PER_FRAME, loss_count);
        } else {
            jitter_set_started(&s_jitter, false);
            memset(frame, 0, sizeof(frame));
            last_frame_valid = false;
            loss_count = 0;
        }

        walkie_apply_playback_gain(frame, SAMPLES_PER_FRAME, speaker_gain_q12);

        size_t bytes_written = 0;
        if (i2s_channel_write(s_i2s_tx, frame, sizeof(frame), &bytes_written, FRAME_MS) == ESP_OK) {
            if (rx_audio_frame) {
                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                s_app.rx_led_until_ms = (esp_timer_get_time() / 1000) + 180;
                xSemaphoreGive(s_state_lock);
            }
        }
    }
}

/**
 * Main UI/control task for buttons, radio dispatch, outputs, and OLED redraws.
 *
 * This task owns most state-machine transitions. It also drains queued ESP-NOW
 * receive items, sends heartbeats, updates GPIO outputs, and periodically sends
 * a snapshot to the display renderer.
 */
static void control_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_debug_tick = 0;

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        TickType_t now_tick = xTaskGetTickCount();
        bool tl_evt = button_pressed(&s_left_btn, now_ms);
        bool tr_evt = button_pressed(&s_right_btn, now_ms);
        bool ok_evt = button_pressed(&s_ok_btn, now_ms);
        bool bl_evt = button_pressed(&s_bl_btn, now_ms);
        bool br_evt = button_pressed(&s_br_btn, now_ms);
        bool do_scan = false;
        bool do_log_dump = false;
        bool reset_rx = false;
        bool animate_apps = false;
        int anim_old = 0;
        int anim_new = 0;

        xSemaphoreTake(s_state_lock, portMAX_DELAY);

        s_app.tl_down = button_down(&s_left_btn);
        s_app.tr_down = button_down(&s_right_btn);
        s_app.ok_down = button_down(&s_ok_btn);
        s_app.bl_down = button_down(&s_bl_btn);
        s_app.br_down = button_down(&s_br_btn);
        s_app.ptt_down = (gpio_get_level(s_board.ptt_pin) == 0);

        if (s_app.ui_mode == MODE_PTT) {
            if (tl_evt) {
                s_app.current_channel--;
                if (s_app.current_channel < LOGICAL_CHANNEL_MIN) {
                    s_app.current_channel = LOGICAL_CHANNEL_MAX;
                }
                clear_link_locked();
                reset_rx = true;
            }
            if (tr_evt) {
                s_app.current_channel++;
                if (s_app.current_channel > LOGICAL_CHANNEL_MAX) {
                    s_app.current_channel = LOGICAL_CHANNEL_MIN;
                }
                clear_link_locked();
                reset_rx = true;
            }
            if (bl_evt) {
                s_app.laser_on = !s_app.laser_on;
            }
            if (br_evt) {
                s_app.ui_mode = MODE_APPS;
                reset_rx = true;
            }
            if (ok_evt) {
                do_scan = true;
                reset_rx = true;
            }
        } else if (s_app.ui_mode == MODE_APPS) {
            if (tl_evt) {
                anim_old = s_app.selected_app;
                s_app.selected_app = walkie_wrap_index(s_app.selected_app - 1, APP_COUNT);
                anim_new = s_app.selected_app;
                animate_apps = true;
            }
            if (tr_evt) {
                anim_old = s_app.selected_app;
                s_app.selected_app = walkie_wrap_index(s_app.selected_app + 1, APP_COUNT);
                anim_new = s_app.selected_app;
                animate_apps = true;
            }
            if (ok_evt) {
                if (s_app.selected_app == APP_KID) {
                    s_app.ui_mode = MODE_KID;
                    s_app.laser_on = false;
                    clear_link_locked();
                    reset_rx = true;
                } else {
                    s_app.ui_mode = MODE_APP_VIEW;
                }
            }
            if (bl_evt) {
                s_app.ui_mode = MODE_PTT;
            }
            if (br_evt) {
                s_app.ui_mode = MODE_SETTINGS;
            }
        } else if (s_app.ui_mode == MODE_SETTINGS) {
            if (ok_evt) {
                if (s_app.extra.settings_index == SET_LOG_DUMP) {
                    /* Dumping the onboard JSONL log can take a while, so only
                     * set a flag while holding the state lock. The actual serial
                     * dump happens below after the UI state is released. */
                    do_log_dump = true;
                } else {
                    toggle_setting_locked();
                }
            }
            if (tl_evt) {
                settings_move_up_locked();
            }
            if (tr_evt) {
                settings_move_down_locked();
            }
            if (bl_evt) {
                s_app.ui_mode = MODE_APPS;
            }
        } else if (s_app.ui_mode == MODE_APP_VIEW) {
            if (s_app.selected_app == APP_LIGHTS) {
                if (tl_evt) {
                    if (lights_rate_selected_locked()) {
                        lights_increment_rate_locked();
                        s_app.extra.lights_rate_repeat_next_ms = now_ms + 350;
                    } else {
                        lights_move_up_locked();
                    }
                }
                if (s_app.tl_down &&
                    s_app.extra.lights_rate_repeat_next_ms > 0 &&
                    now_ms >= s_app.extra.lights_rate_repeat_next_ms &&
                    lights_rate_selected_locked()) {
                    lights_increment_rate_locked();
                    s_app.extra.lights_rate_repeat_next_ms = now_ms + 80;
                }
                if (!s_app.tl_down) {
                    s_app.extra.lights_rate_repeat_next_ms = 0;
                }
                if (tr_evt) {
                    lights_move_down_locked();
                }
                if (ok_evt) {
                    lights_ok_action_locked();
                }
                if (bl_evt) {
                    s_app.ui_mode = MODE_APPS;
                    s_app.extra.lights_rate_repeat_next_ms = 0;
                }
            } else if (bl_evt) {
                s_app.ui_mode = MODE_APPS;
            }
        } else if (s_app.ui_mode == MODE_KID) {
            if (s_app.ok_down) {
                if (s_app.kid_hold_start_ms == 0) {
                    s_app.kid_hold_start_ms = now_ms;
                } else if ((now_ms - s_app.kid_hold_start_ms) >= 2000) {
                    s_app.ui_mode = MODE_PTT;
                    s_app.kid_hold_start_ms = 0;
                    clear_link_locked();
                    reset_rx = true;
                }
            } else {
                s_app.kid_hold_start_ms = 0;
            }
        }

        update_smoothed_inputs_locked(now_ms, now_tick);
        xSemaphoreGive(s_state_lock);

        if (reset_rx) {
            jitter_reset(&s_jitter);
            drain_radio_rx_queue();
        }

        if (do_scan) {
            auto_scan_channels();
            jitter_reset(&s_jitter);
            drain_radio_rx_queue();
        }

        if (do_log_dump) {
            field_log_dump_over_serial();
        }

        if (animate_apps && walkie_display_ready(&s_display)) {
            walkie_ui_snapshot_t snapshot;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            state_copy_snapshot_locked(&snapshot, now_tick, now_ms);
            xSemaphoreGive(s_state_lock);
            walkie_display_animate_apps_change(&s_display, &snapshot, anim_old, anim_new);
        }

        bool allow_audio = false;
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        allow_audio = ((s_app.ui_mode == MODE_PTT || s_app.ui_mode == MODE_KID) && !s_app.ptt_down);
        xSemaphoreGive(s_state_lock);

        radio_rx_item_t item;
        if (s_radio_rx_queue != NULL) {
            while (xQueueReceive(s_radio_rx_queue, &item, 0) == pdTRUE) {
                handle_radio_item(&item, xTaskGetTickCount(), allow_audio);
            }
        }

        maybe_send_heartbeat(now_tick);
        update_outputs(now_ms);

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        bool should_redraw = (now_tick - s_app.last_oled_tick) >= pdMS_TO_TICKS(OLED_UPDATE_MS);
        walkie_ui_snapshot_t snapshot;
        if (should_redraw) {
            state_copy_snapshot_locked(&snapshot, now_tick, now_ms);
            s_app.last_oled_tick = now_tick;
        }
        xSemaphoreGive(s_state_lock);

        if (should_redraw) {
            walkie_display_redraw(&s_display, &snapshot);
        }

        /* One aggregate telemetry sample per second. This path only formats and
         * queues the log line; the flash write happens in field_log_task(). */
        if ((now_tick - last_debug_tick) >= pdMS_TO_TICKS(DEBUG_JSON_STATS_MS)) {
            last_debug_tick = now_tick;
            debug_emit_json_line(now_tick);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
    }
}

/**
 * ESP-IDF application entry point.
 *
 * Initializes NVS, selects the board profile, configures GPIO/ADC/display/radio/
 * I2S, logs useful hardware identity information, then starts the three runtime
 * tasks that keep the walkie alive.
 */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    board_config_init(&s_board);
    gpio_init_inputs_outputs();
    button_init(&s_ok_btn, OK_PIN, 70);
    button_init(&s_left_btn, s_board.top_left_pin, 70);
    button_init(&s_right_btn, s_board.top_right_pin, 70);
    button_init(&s_bl_btn, BOT_LEFT_PIN, 70);
    button_init(&s_br_btn, BOT_RIGHT_PIN, 70);

    s_state_lock = xSemaphoreCreateMutex();
    if (s_state_lock == NULL) {
        ESP_LOGE(TAG, "state mutex alloc failed");
        return;
    }

    jitter_init(&s_jitter);
    if (s_jitter.mutex == NULL) {
        ESP_LOGE(TAG, "jitter mutex alloc failed");
        return;
    }

    /* Mount field-test storage before the rest of the app starts so the boot
     * gesture can dump stored logs without needing Wi-Fi or I2S to initialize. */
    if (field_log_init() == ESP_OK) {
        field_log_dump_if_requested();
    }

    esp_err_t adc_err = adc_init_all();
    if (adc_err != ESP_OK) {
        ESP_LOGW(TAG, "adc init failed, using safe defaults: %s", esp_err_to_name(adc_err));
    }

    if (walkie_display_init(&s_display, OLED_SDA, OLED_SCL) == ESP_OK) {
        walkie_display_show_splash(&s_display);
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    set_state_defaults();
    xSemaphoreGive(s_state_lock);

    if (walkie_display_ready(&s_display)) {
        walkie_ui_snapshot_t snapshot;
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        state_copy_snapshot_locked(&snapshot, xTaskGetTickCount(), esp_timer_get_time() / 1000);
        xSemaphoreGive(s_state_lock);
        walkie_display_redraw(&s_display, &snapshot);
    }

    esp_err_t wifi_err = wifi_init_for_espnow();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed, radio disabled: %s", esp_err_to_name(wifi_err));
    } else {
        esp_err_t now_err = espnow_init_peer();
        if (now_err != ESP_OK) {
            ESP_LOGW(TAG, "esp-now init failed, radio disabled: %s", esp_err_to_name(now_err));
        }
    }

    esp_err_t i2s_err = i2s_init_pair();
    if (i2s_err != ESP_OK) {
        ESP_LOGW(TAG, "i2s init failed, audio disabled: %s", esp_err_to_name(i2s_err));
    }

    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    char my_mac_text[18];
    char peer_mac_text[18];
    char expect_mac_text[18];
    ESP_LOGI(TAG, "Device role: %s", s_board.label);
    ESP_LOGI(TAG, "STA MAC: %s", mac_to_str(my_mac, my_mac_text, sizeof(my_mac_text)));
    ESP_LOGI(TAG, "Expected MAC: %s", mac_to_str(s_board.self_mac, expect_mac_text, sizeof(expect_mac_text)));
    ESP_LOGI(TAG, "Peer MAC: %s", mac_to_str(s_board.peer_mac, peer_mac_text, sizeof(peer_mac_text)));
    ESP_LOGI(TAG, "RF channel: %d", CONFIG_WALKIE_RF_CHANNEL);
    ESP_LOGI(TAG, "ADC ready: %s", s_adc_ready ? "yes" : "no");
    ESP_LOGI(TAG, "Radio ready: %s", s_radio_ready ? "yes" : "no");
    ESP_LOGI(TAG, "Audio TX ready: %s", s_i2s_tx_ready ? "yes" : "no");
    ESP_LOGI(TAG, "Audio RX ready: %s", s_i2s_rx_ready ? "yes" : "no");

    xTaskCreatePinnedToCore(control_task, "walkie_control", 8192, NULL, 7, NULL, 1);
    if (s_field_log_ready && s_field_log_queue != NULL) {
        /* Low priority: if flash is slow, audio/radio/control work wins. */
        xTaskCreatePinnedToCore(field_log_task, "walkie_fieldlog", 4096, NULL, 2, NULL, 0);
    }
    xTaskCreatePinnedToCore(capture_task, "walkie_capture", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(playback_task, "walkie_playback", 8192, NULL, 5, NULL, 1);
}
