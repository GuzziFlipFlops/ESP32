#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared types between main.c and the display/audio modules.
 * The UI is rendered from a snapshot so the OLED code can draw without holding
 * the main state lock or touching hardware state directly.
 */

#define WALKIE_FIRMWARE_VERSION "0.5.4"

typedef enum {
    MODE_PTT = 0,
    MODE_APPS,
    MODE_APP_VIEW,
    MODE_KID,
    MODE_SCANNING,
    MODE_SETTINGS,
} walkie_ui_mode_t;

typedef enum {
    APP_TX_WIFI = 0,
    APP_RX_WIFI,
    APP_TEXT,
    APP_BUTTONS,
    APP_LIGHTS,
    APP_KID,
    APP_COUNT,
} walkie_app_id_t;

typedef enum {
    SET_LIMIT60 = 0,
    SET_LIMIT60_LOWBAT,
    SET_SPK_BOOST,
    SET_MIC_BOOST,
    SET_MIC_CUT,
    SET_FLASH_USAGE,
    SET_MEMORY_USAGE,
    SET_CPU_OVERLAY,
    SET_FIRMWARE_VERSION,
    SET_LOG_DUMP,
    SETTING_COUNT,
} walkie_setting_id_t;

typedef enum {
    LIGHT_STROBE = 0,
    LIGHT_TARGET,
    LIGHT_RATE,
    LIGHT_LED_CONST,
    LIGHT_LASER_CONST,
    LIGHT_PRE1,
    LIGHT_PRE2,
    LIGHT_PRE3,
    LIGHT_COUNT,
} walkie_light_id_t;

typedef enum {
    LIGHT_TARGET_LED = 0,
    LIGHT_TARGET_LASER,
    LIGHT_TARGET_BOTH,
} walkie_light_target_t;

typedef struct {
    bool set_limit60;
    bool set_limit60_lowbat;
    bool set_spk_boost;
    bool set_mic_boost;
    bool set_mic_cut;
    bool show_cpu_usage;

    int eff_vol_percent;
    int speaker_gain_q12;
    int mic_manual_mode;
    int mic_gain_q10;
    int cpu_usage_pct;

    uint32_t flash_used_bytes;
    uint32_t flash_total_bytes;
    uint32_t memory_used_bytes;
    uint32_t memory_free_bytes;
    uint32_t memory_total_bytes;

    int settings_index;
    int lights_index;
    int lights_mode;
    int lights_target;
    int lights_strobe_hz;
    bool lights_led_const;
    bool lights_laser_const;

    int64_t big_knob_start_ms;
    int64_t lights_rate_repeat_next_ms;
} walkie_extra_state_t;

typedef struct {
    const char *label;
    uint8_t self_mac[6];
    uint8_t peer_mac[6];

    gpio_num_t ptt_pin;
    gpio_num_t led_pin;
    gpio_num_t top_left_pin;
    gpio_num_t top_right_pin;

    float batt_r_top;
    float batt_r_bottom;
    int batt_avg_samples;
    float batt_smooth_alpha;
    int mic_base_gain_q10;
    int mic_boost_gain_q10;
    int mic_cut_gain_q10;
} walkie_board_config_t;

typedef struct {
    const char *device_label;
    walkie_ui_mode_t ui_mode;
    int selected_app;
    int current_channel;
    bool laser_on;
    bool link_on;
    int link_quality_pct;
    int link_rssi_dbm;

    int batt_pct;
    float vbat;
    int eff_vol_percent;

    bool tl_pressed;
    bool tr_pressed;
    bool ok_pressed;
    bool bl_pressed;
    bool br_pressed;
    bool ptt_pressed;
    bool rx_audio_active;

    int kid_hold_ms;
    walkie_extra_state_t extra;
} walkie_ui_snapshot_t;

typedef struct {
    bool led_on;
    bool laser_on;
} walkie_light_outputs_t;

/**
 * Clamp an integer to a closed range.
 *
 * Used throughout UI/math code to keep percentages, drawing coordinates, and
 * fixed-point gains inside safe bounds.
 */
static inline int walkie_clamp_int(int value, int lo, int hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

/**
 * Wrap an index into [0, count).
 *
 * The small UI uses wraparound menus so users can keep pressing one direction
 * instead of needing separate end-stop behavior.
 */
static inline int walkie_wrap_index(int index, int count)
{
    if (index < 0) {
        return count - 1;
    }
    if (index >= count) {
        return 0;
    }
    return index;
}

#ifdef __cplusplus
}
#endif
