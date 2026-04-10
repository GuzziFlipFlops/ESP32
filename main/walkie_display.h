#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#include "walkie_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct walkie_display {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t framebuffer[128 * 64 / 8];
    bool ready;
} walkie_display_t;

/* Initialize the SSD1306 OLED and its I2C bus/device handle. */
esp_err_t walkie_display_init(walkie_display_t *display, gpio_num_t sda, gpio_num_t scl);

/* Return true after the display has been initialized successfully. */
bool walkie_display_ready(const walkie_display_t *display);

/* Play the startup splash animation. */
void walkie_display_show_splash(walkie_display_t *display);

/* Render one complete UI frame from a snapshot copied out of app state. */
void walkie_display_redraw(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot);

/* Draw the short slide transition used when moving through the apps carousel. */
void walkie_display_animate_apps_change(walkie_display_t *display,
                                        const walkie_ui_snapshot_t *snapshot,
                                        int old_idx,
                                        int new_idx);

#ifdef __cplusplus
}
#endif
