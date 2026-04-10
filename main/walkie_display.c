#include "walkie_display.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Minimal SSD1306 renderer.
 *
 * The rest of the firmware builds a walkie_ui_snapshot_t, and this file turns
 * that snapshot into pixels. Keeping drawing code here means the control/audio
 * tasks never need to know about font glyphs, framebuffer pages, or I2C flushes.
 */

static const char *TAG = "walkie_display";

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)

typedef enum {
    FONT_SMALL = 0,
    FONT_BODY,
    FONT_LARGE,
} font_kind_t;

typedef struct {
    char ch;
    uint8_t cols[5];
} glyph5x7_t;

static const glyph5x7_t s_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'<', {0x08, 0x14, 0x22, 0x41, 0x00}},
    {'>', {0x41, 0x22, 0x14, 0x08, 0x00}},
    {'+', {0x08, 0x08, 0x3E, 0x08, 0x08}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'%', {0x63, 0x13, 0x08, 0x64, 0x63}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x7F, 0x20, 0x18, 0x20, 0x7F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x03, 0x04, 0x78, 0x04, 0x03}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

/**
 * Look up a 5x7 bitmap glyph for a printable character.
 *
 * The font table is uppercase-only, so lowercase input is normalized. Unknown
 * characters render as spaces rather than drawing garbage.
 */
static const uint8_t *glyph_for_char(char ch)
{
    char upper = (char)toupper((unsigned char)ch);

    for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); ++i) {
        if (s_font[i].ch == upper) {
            return s_font[i].cols;
        }
    }

    return s_font[0].cols;
}

/**
 * Return pixel scale for a font size.
 *
 * FONT_SMALL and FONT_BODY use native 5x7 pixels. FONT_LARGE doubles both axes
 * for splash/scanning screens where bigger text is useful.
 */
static int font_scale(font_kind_t font)
{
    if (font == FONT_LARGE) {
        return 2;
    }
    return 1;
}

/**
 * Return horizontal cursor advance for one character.
 *
 * The glyph itself is 5 columns wide; the sixth column is spacing. Scaling is
 * included so layout helpers can measure text consistently.
 */
static int font_advance(font_kind_t font)
{
    return 6 * font_scale(font);
}

/**
 * Send one or more command/data bytes to the SSD1306 over I2C.
 *
 * The small tx buffer keeps stack usage low. Large framebuffer writes are split
 * into 16-byte chunks because each I2C transfer includes a control byte.
 */
static esp_err_t oled_send_block(walkie_display_t *display, uint8_t control, const uint8_t *data, size_t len)
{
    uint8_t tx[17];

    if (!display || !display->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    tx[0] = control;
    while (len > 0) {
        size_t chunk = len > 16 ? 16 : len;
        memcpy(&tx[1], data, chunk);
        esp_err_t err = i2c_master_transmit(display->dev, tx, chunk + 1, 1000);
        if (err != ESP_OK) {
            return err;
        }
        data += chunk;
        len -= chunk;
    }

    return ESP_OK;
}

/**
 * Send SSD1306 command bytes.
 *
 * Control byte 0x00 tells the display controller that the following bytes are
 * commands, not framebuffer data.
 */
static esp_err_t oled_send_cmds(walkie_display_t *display, const uint8_t *cmds, size_t len)
{
    return oled_send_block(display, 0x00, cmds, len);
}

/**
 * Send SSD1306 framebuffer data bytes.
 *
 * Control byte 0x40 selects data mode. oled_flush() calls this one page at a
 * time to update the visible display.
 */
static esp_err_t oled_send_data(walkie_display_t *display, const uint8_t *data, size_t len)
{
    return oled_send_block(display, 0x40, data, len);
}

/**
 * Clear the local 1-bit framebuffer.
 *
 * Nothing changes on the OLED until oled_flush() sends the framebuffer pages.
 */
static void fb_clear(walkie_display_t *display)
{
    memset(display->framebuffer, 0, sizeof(display->framebuffer));
}

/**
 * Set or clear one framebuffer pixel with bounds checking.
 *
 * The SSD1306 framebuffer is arranged as vertical 8-pixel pages, so y selects a
 * bit inside one byte and x selects the byte column.
 */
static void fb_set_pixel(walkie_display_t *display, int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }

    uint8_t *cell = &display->framebuffer[x + ((y / 8) * OLED_WIDTH)];
    uint8_t mask = (uint8_t)(1U << (y & 7));

    if (on) {
        *cell |= mask;
    } else {
        *cell &= (uint8_t)~mask;
    }
}

/**
 * Fill a rectangle in the local framebuffer.
 *
 * This simple pixel-loop renderer is fast enough for a 128x64 monochrome OLED
 * and keeps all higher-level drawing primitives easy to reason about.
 */
static void fb_fill_rect(walkie_display_t *display, int x, int y, int w, int h, bool on)
{
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            fb_set_pixel(display, x + xx, y + yy, on);
        }
    }
}

/**
 * Draw a one-pixel rectangular outline.
 *
 * Used for cards, menu boxes, signal meters, battery outline, and status boxes.
 */
static void fb_draw_frame(walkie_display_t *display, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    fb_fill_rect(display, x, y, w, 1, true);
    fb_fill_rect(display, x, y + h - 1, w, 1, true);
    fb_fill_rect(display, x, y, 1, h, true);
    fb_fill_rect(display, x + w - 1, y, 1, h, true);
}

/**
 * Draw one glyph in foreground color.
 *
 * baseline_y is the text baseline, not the top-left corner. That matches the
 * rest of the display helpers and makes mixed font sizes easier to align.
 */
static void fb_draw_glyph(walkie_display_t *display, int x, int y, char ch, font_kind_t font)
{
    const uint8_t *cols = glyph_for_char(ch);
    int scale = font_scale(font);
    int draw_y = y - (7 * scale);

    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if ((cols[col] >> row) & 0x01) {
                fb_fill_rect(display, x + (col * scale), draw_y + (row * scale), scale, scale, true);
            }
        }
    }
}

/**
 * Draw one glyph in a caller-selected color.
 *
 * Used by inverted status boxes: the box background is filled white, then text
 * is drawn with pixels cleared to black.
 */
static void fb_draw_glyph_color(walkie_display_t *display, int x, int y, char ch, font_kind_t font, bool on)
{
    const uint8_t *cols = glyph_for_char(ch);
    int scale = font_scale(font);
    int draw_y = y - (7 * scale);

    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if ((cols[col] >> row) & 0x01) {
                fb_fill_rect(display, x + (col * scale), draw_y + (row * scale), scale, scale, on);
            }
        }
    }
}

/**
 * Draw a string in foreground color.
 *
 * This is the normal text primitive used by almost every screen.
 */
static void fb_draw_text(walkie_display_t *display, int x, int y, const char *text, font_kind_t font)
{
    int advance = font_advance(font);

    for (size_t i = 0; text && text[i] != '\0'; ++i) {
        fb_draw_glyph(display, x + ((int)i * advance), y, text[i], font);
    }
}

/**
 * Draw a string in a caller-selected color.
 *
 * This mirrors fb_draw_text() but is useful when a UI element needs inverted
 * text on a filled background.
 */
static void fb_draw_text_color(walkie_display_t *display, int x, int y, const char *text, font_kind_t font, bool on)
{
    int advance = font_advance(font);

    for (size_t i = 0; text && text[i] != '\0'; ++i) {
        fb_draw_glyph_color(display, x + ((int)i * advance), y, text[i], font, on);
    }
}

/**
 * Measure text width in pixels for the chosen font.
 *
 * Layout helpers use this to center text and right-align bottom/header labels.
 */
static int text_width(font_kind_t font, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    return ((int)strlen(text) * font_advance(font)) - font_scale(font);
}

/**
 * Return the x coordinate that centers text on the full OLED width.
 */
static int center_x(font_kind_t font, const char *text)
{
    return walkie_clamp_int((OLED_WIDTH - text_width(font, text)) / 2, 0, OLED_WIDTH - 1);
}

/**
 * Push the entire local framebuffer to the SSD1306.
 *
 * SSD1306 memory is organized into 8-pixel-high pages. Each flush selects a page
 * and streams 128 bytes for that row band.
 */
static esp_err_t oled_flush(walkie_display_t *display)
{
    for (uint8_t page = 0; page < OLED_PAGES; ++page) {
        uint8_t setup[] = {
            (uint8_t)(0xB0 + page),
            0x00,
            0x10,
        };

        esp_err_t err = oled_send_cmds(display, setup, sizeof(setup));
        if (err != ESP_OK) {
            return err;
        }

        err = oled_send_data(display, &display->framebuffer[page * OLED_WIDTH], OLED_WIDTH);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

/**
 * Draw text centered inside a horizontal region.
 *
 * The x/width pair defines the region, while baseline_y controls vertical
 * placement. Text is clipped only by the framebuffer bounds.
 */
static void draw_centered_text(walkie_display_t *display,
                               int x,
                               int width,
                               int baseline_y,
                               const char *text,
                               font_kind_t font)
{
    int draw_x = x + ((width - text_width(font, text)) / 2);
    fb_draw_text(display, walkie_clamp_int(draw_x, 0, OLED_WIDTH - 1), baseline_y, text, font);
}

/**
 * Draw colored text centered inside a horizontal region.
 *
 * Used by inverted RX/PTT status boxes where the text should be cleared from a
 * filled rectangle.
 */
static void draw_centered_text_color(walkie_display_t *display,
                                     int x,
                                     int width,
                                     int baseline_y,
                                     const char *text,
                                     font_kind_t font,
                                     bool on)
{
    int draw_x = x + ((width - text_width(font, text)) / 2);
    fb_draw_text_color(display, walkie_clamp_int(draw_x, 0, OLED_WIDTH - 1), baseline_y, text, font, on);
}

/**
 * Draw the shared top header used by all screens.
 *
 * The left side shows BLACK/GREY, the middle can show CPU usage when enabled,
 * and the right side shows a battery outline with voltage text.
 */
static void draw_top_bar(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot)
{
    char voltage[8];
    int fill_w = ((39 - 2) * walkie_clamp_int(snapshot->batt_pct, 0, 100)) / 100;

    fb_draw_text(display, 0, 9, snapshot->device_label, FONT_BODY);
    if (snapshot->extra.show_cpu_usage) {
        char cpu[10];
        snprintf(cpu, sizeof(cpu), "CPU%d%%", walkie_clamp_int(snapshot->extra.cpu_usage_pct, 0, 100));
        fb_draw_text(display, 80 - text_width(FONT_SMALL, cpu), 10, cpu, FONT_SMALL);
    }
    fb_draw_frame(display, 82, 1, 39, 12);
    fb_fill_rect(display, 121, 4, 3, 6, true);
    if (fill_w > 0) {
        fb_fill_rect(display, 83, 2, fill_w, 10, true);
    }

    snprintf(voltage, sizeof(voltage), "%.1f", snapshot->vbat);
    fb_fill_rect(display, 86, 3, 28, 8, false);
    fb_draw_text(display, 87, 10, voltage, FONT_SMALL);
}

/**
 * Draw the three-label bottom navigation hint row.
 *
 * Labels are optional. Each screen passes only the hints that make sense for the
 * current mode.
 */
static void draw_bottom_nav3(walkie_display_t *display, const char *left, const char *center, const char *right)
{
    fb_fill_rect(display, 0, 54, OLED_WIDTH, 10, false);

    if (left && left[0]) {
        fb_draw_text(display, 0, 62, left, FONT_SMALL);
    }
    if (center && center[0]) {
        fb_draw_text(display, center_x(FONT_SMALL, center), 62, center, FONT_SMALL);
    }
    if (right && right[0]) {
        fb_draw_text(display, OLED_WIDTH - text_width(FONT_SMALL, right), 62, right, FONT_SMALL);
    }
}

/**
 * Draw a framed menu/card box with one or two centered text lines.
 *
 * The selected flag adds a second inner frame, giving the apps carousel a focus
 * state without needing color.
 */
static void draw_box_label(walkie_display_t *display,
                           int x,
                           int y,
                           int w,
                           int h,
                           const char *line1,
                           const char *line2,
                           bool selected)
{
    fb_draw_frame(display, x, y, w, h);
    if (selected) {
        fb_draw_frame(display, x + 1, y + 1, w - 2, h - 2);
    }

    if (line2) {
        draw_centered_text(display, x, w, y + 11, line1, FONT_BODY);
        draw_centered_text(display, x, w, y + 22, line2, FONT_BODY);
    } else {
        draw_centered_text(display, x, w, y + 17, line1, FONT_BODY);
    }
}

/**
 * Convert an app index into compact text labels for the apps carousel.
 *
 * Side cards have less width than the selected center card, so this helper can
 * return abbreviated labels when side is true.
 */
static void app_lines(int idx, bool side, const char **line1, const char **line2)
{
    switch (idx) {
    case APP_TX_WIFI:
        *line1 = "TX";
        *line2 = side ? NULL : "WIFI";
        break;
    case APP_RX_WIFI:
        *line1 = "RX";
        *line2 = side ? NULL : "WIFI";
        break;
    case APP_TEXT:
        *line1 = side ? "TXT" : "TEXT";
        *line2 = side ? NULL : "ONLY";
        break;
    case APP_BUTTONS:
        *line1 = "BTN";
        *line2 = side ? NULL : "CTRL";
        break;
    case APP_LIGHTS:
        *line1 = side ? "LGT" : "LIGHTS";
        *line2 = NULL;
        break;
    default:
        *line1 = "KID";
        *line2 = side ? NULL : "MODE";
        break;
    }
}

/**
 * Draw the horizontally scrollable apps menu.
 *
 * The selected app is drawn in the center. The old/new app animation reuses this
 * function with a temporary x offset for a simple slide effect.
 */
static void draw_apps_carousel(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot, int selected, int anim_offset)
{
    const char *line1;
    const char *line2;
    int left_idx = walkie_wrap_index(selected - 1, APP_COUNT);
    int right_idx = walkie_wrap_index(selected + 1, APP_COUNT);

    fb_clear(display);
    draw_top_bar(display, snapshot);

    app_lines(left_idx, true, &line1, &line2);
    draw_box_label(display, 6 + anim_offset, 21, 26, 22, line1, line2, false);

    app_lines(selected, false, &line1, &line2);
    draw_box_label(display, 36 + anim_offset, 18, 56, 28, line1, line2, true);

    app_lines(right_idx, true, &line1, &line2);
    draw_box_label(display, 96 + anim_offset, 21, 26, 22, line1, line2, false);

    draw_bottom_nav3(display, "BACK", "SETTING", "OK");
    oled_flush(display);
}

/**
 * Draw the RSSI-based signal-strength meter on the PTT screen.
 *
 * link_quality_pct is derived from smoothed ESP-NOW RSSI. Empty bars mean either
 * LINK OFF or RSSI below the weak threshold.
 */
static void draw_signal_meter(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot)
{
    const int quality = snapshot->link_on ? walkie_clamp_int(snapshot->link_quality_pct, 0, 100) : 0;
    const int active_bars = snapshot->link_on ? walkie_clamp_int((quality + 19) / 20, 0, 5) : 0;
    const int base_y = 47;

    fb_draw_frame(display, 4, 19, 20, 30);
    for (int i = 0; i < 5; ++i) {
        const int bar_h = 4 + (i * 4);
        const int bar_x = 7 + (i * 3);
        const int bar_y = base_y - bar_h;

        if (i < active_bars) {
            fb_fill_rect(display, bar_x, bar_y, 2, bar_h, true);
        } else {
            fb_fill_rect(display, bar_x, base_y - 1, 2, 1, true);
        }
    }
}

/**
 * Draw one tiny RX/PTT status indicator box.
 *
 * Inactive state is an outline with white text. Active state fills the box and
 * draws black text by clearing text pixels, producing the requested inversion.
 */
static void draw_status_box(walkie_display_t *display, int x, int y, int w, int h, const char *label, bool active)
{
    if (active) {
        fb_fill_rect(display, x, y, w, h, true);
        draw_centered_text_color(display, x, w, y + 10, label, FONT_SMALL, false);
    } else {
        fb_draw_frame(display, x, y, w, h);
        draw_centered_text(display, x, w, y + 10, label, FONT_SMALL);
    }
}

/**
 * Draw the main push-to-talk screen.
 *
 * This is the highest-density UI: signal meter, channel/link card, RX/PTT
 * indicators, optional CPU in the header, volume, laser state, and apps hint.
 */
static void draw_ptt_screen(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot)
{
    char ch_text[16];
    const char *link_text = snapshot->link_on ? "LINK ON" : "LINK OFF";
    char vol_text[16];
    const char *laser_text = snapshot->laser_on ? "ON" : "OFF";

    fb_clear(display);
    draw_top_bar(display, snapshot);
    draw_signal_meter(display, snapshot);
    fb_draw_frame(display, 28, 17, 72, 32);
    snprintf(ch_text, sizeof(ch_text), "< CH %02d >", snapshot->current_channel);
    draw_centered_text(display, 28, 72, 30, ch_text, FONT_BODY);
    draw_centered_text(display, 28, 72, 42, link_text, FONT_BODY);
    draw_status_box(display, 103, 18, 22, 14, "RX", snapshot->rx_audio_active);
    draw_status_box(display, 103, 35, 22, 14, "PTT", snapshot->ptt_pressed);
    snprintf(vol_text, sizeof(vol_text), "VOL %d%%", snapshot->eff_vol_percent);
    draw_bottom_nav3(display, vol_text, laser_text, "APPS");
    oled_flush(display);
}

/**
 * Draw the temporary scan screen shown during channel search.
 *
 * auto_scan_channels() updates current_channel before each redraw so the user
 * can see which logical channel is being probed.
 */
static void draw_scanning_screen(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot)
{
    char channel[12];

    fb_clear(display);
    draw_top_bar(display, snapshot);
    fb_draw_frame(display, 24, 18, 80, 28);
    fb_draw_text(display, center_x(FONT_LARGE, "SCAN"), 31, "SCAN", FONT_LARGE);
    snprintf(channel, sizeof(channel), "CH %02d", snapshot->current_channel);
    fb_draw_text(display, center_x(FONT_BODY, channel), 43, channel, FONT_BODY);
    oled_flush(display);
}

/**
 * Draw a simple placeholder/info panel for non-custom app screens.
 *
 * TX WIFI, RX WIFI, and TEXT currently use this lightweight panel until those
 * app modes get deeper behavior.
 */
static void draw_panel_app(walkie_display_t *display,
                           const walkie_ui_snapshot_t *snapshot,
                           const char *line1,
                           const char *line2)
{
    fb_clear(display);
    draw_top_bar(display, snapshot);
    fb_draw_frame(display, 4, 18, 120, 33);
    fb_draw_text(display, 12, 31, line1, FONT_BODY);
    fb_draw_text(display, 12, 44, line2, FONT_BODY);
    draw_bottom_nav3(display, "BACK", "", "");
    oled_flush(display);
}

/**
 * Draw one button state tile for the BUTTON CTRL app.
 *
 * Pressed buttons are shown with an inverted-looking fill so pin mapping and
 * debounce behavior can be tested without a serial monitor.
 */
static void draw_button_box(walkie_display_t *display, int x, int y, const char *label, bool pressed)
{
    fb_draw_frame(display, x, y, 24, 12);
    if (pressed) {
        fb_fill_rect(display, x + 1, y + 1, 22, 10, true);
        fb_fill_rect(display, x + 4, y + 2, 16, 8, false);
    }
    fb_draw_text(display, x + 5, y + 9, label, FONT_SMALL);
}

/**
 * Draw the BUTTON CTRL diagnostics app.
 *
 * This screen visualizes top-left, top-right, OK, bottom-left, bottom-right, and
 * PTT button levels. It is useful when checking black/grey pinout differences.
 */
static void draw_buttons_app(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot)
{
    fb_clear(display);
    draw_top_bar(display, snapshot);
    fb_draw_text(display, 16, 17, "BUTTON CTRL", FONT_BODY);
    draw_button_box(display, 20, 28, "TL", snapshot->tl_pressed);
    draw_button_box(display, 84, 28, "TR", snapshot->tr_pressed);
    draw_button_box(display, 52, 28, "OK", snapshot->ok_pressed);
    draw_button_box(display, 20, 42, "BL", snapshot->bl_pressed);
    draw_button_box(display, 84, 42, "BR", snapshot->br_pressed);
    draw_button_box(display, 52, 42, "PT", snapshot->ptt_pressed);
    draw_bottom_nav3(display, "BACK", "", "");
    oled_flush(display);
}

/**
 * Draw kid mode and its exit-progress bar.
 *
 * Kid mode locks the walkie to a fixed channel. Holding OK fills the progress
 * bar; once it reaches 2 seconds the firmware returns to normal PTT mode.
 */
static void draw_kid_mode_screen(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot)
{
    int progress = walkie_clamp_int((snapshot->kid_hold_ms * 100) / 2000, 0, 100);
    int fill_w = (98 * progress) / 100;

    fb_clear(display);
    draw_top_bar(display, snapshot);
    fb_draw_frame(display, 18, 18, 92, 26);
    fb_draw_text(display, 28, 29, "KID MODE", FONT_BODY);
    fb_draw_text(display, 26, 41, snapshot->link_on ? "LINK ON" : "LINK OFF", FONT_BODY);
    fb_draw_frame(display, 14, 47, 100, 5);
    if (fill_w > 0) {
        fb_fill_rect(display, 15, 48, fill_w, 3, true);
    }
    draw_bottom_nav3(display, "HOLD OK", "", "");
    oled_flush(display);
}

/**
 * Return the display name for one settings row.
 *
 * Names are intentionally short enough to split across the small center panel.
 */
static const char *setting_name(int index)
{
    switch (index) {
    case SET_LIMIT60:
        return "LIMIT MAX AUDIO 60%";
    case SET_LIMIT60_LOWBAT:
        return "LIMIT 60% UNDER 3.6V";
    case SET_SPK_BOOST:
        return "INCREASE SPEAKER VOL";
    case SET_MIC_BOOST:
        return "INCREASE MIC SENSE";
    case SET_MIC_CUT:
        return "DECREASE MIC SENSE";
    case SET_FLASH_USAGE:
        return "FLASH";
    case SET_MEMORY_USAGE:
        return "MEMORY";
    default:
        return "SHOW CPU IN PTT";
    }
}

/**
 * Return whether a settings row is an on/off toggle.
 *
 * Flash and memory rows are read-only resource displays, so they return false.
 */
static bool setting_is_toggle(int index)
{
    return index != SET_FLASH_USAGE && index != SET_MEMORY_USAGE;
}

/**
 * Read the current on/off state for a toggle settings row.
 *
 * Read-only rows return false because their value is rendered by
 * setting_detail_text() instead of ON/OFF.
 */
static bool setting_state(const walkie_extra_state_t *extra, int index)
{
    switch (index) {
    case SET_LIMIT60:
        return extra->set_limit60;
    case SET_LIMIT60_LOWBAT:
        return extra->set_limit60_lowbat;
    case SET_SPK_BOOST:
        return extra->set_spk_boost;
    case SET_MIC_BOOST:
        return extra->set_mic_boost;
    case SET_MIC_CUT:
        return extra->set_mic_cut;
    case SET_CPU_OVERLAY:
        return extra->show_cpu_usage;
    default:
        return false;
    }
}

/**
 * Format flash or RAM usage as a compact kilobyte string.
 *
 * The settings panel has limited width, so this favors readability over exact
 * byte precision.
 */
static void format_usage_text(uint32_t used_bytes, uint32_t total_bytes, char *buffer, size_t buffer_len)
{
    unsigned int used_kb = (used_bytes + 512U) / 1024U;
    unsigned int total_kb = (total_bytes + 512U) / 1024U;

    if (total_kb == 0U) {
        snprintf(buffer, buffer_len, "WAIT");
        return;
    }

    snprintf(buffer, buffer_len, "USED %uK/%uK", used_kb, total_kb);
}

/**
 * Format the detail text for settings rows that need live values.
 *
 * Resource rows show actual usage. Ordinary toggles leave detail empty so the
 * second line can show the split setting name instead.
 */
static void setting_detail_text(const walkie_extra_state_t *extra, int index, char *buffer, size_t buffer_len)
{
    switch (index) {
    case SET_FLASH_USAGE:
        format_usage_text(extra->flash_used_bytes, extra->flash_total_bytes, buffer, buffer_len);
        break;
    case SET_MEMORY_USAGE:
        format_usage_text(extra->memory_used_bytes, extra->memory_total_bytes, buffer, buffer_len);
        break;
    default:
        buffer[0] = '\0';
        break;
    }
}

/**
 * Split a long label into two rough display lines.
 *
 * The function prefers splitting at a space near max_first so words are not
 * chopped unless no suitable space exists.
 */
static void split_text_rough(const char *src, char *line1, size_t len1, char *line2, size_t len2, int max_first)
{
    size_t src_len = strlen(src);
    if ((int)src_len <= max_first) {
        snprintf(line1, len1, "%s", src);
        line2[0] = '\0';
        return;
    }

    int split = max_first;
    while (split > 0 && src[split] != ' ') {
        split--;
    }
    if (split <= 0) {
        split = max_first;
    }

    snprintf(line1, len1, "%.*s", split, src);
    while (src[split] == ' ') {
        split++;
    }
    snprintf(line2, len2, "%s", src + split);
}

/**
 * Draw the settings screen.
 *
 * The center panel shows the selected setting or resource value. The bottom row
 * shows BACK plus ON/OFF and OK hints only when the row is toggleable.
 */
static void draw_settings_screen(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot)
{
    const char *name = setting_name(snapshot->extra.settings_index);
    char line1[18];
    char line2[18];
    char detail[18];
    const bool is_toggle = setting_is_toggle(snapshot->extra.settings_index);
    const char *state_text = is_toggle &&
                                     setting_state(&snapshot->extra, snapshot->extra.settings_index)
                                 ? "ON"
                                 : "OFF";
    const char *right_hint = is_toggle ? "OK" : "";

    memset(line1, 0, sizeof(line1));
    memset(line2, 0, sizeof(line2));
    memset(detail, 0, sizeof(detail));
    split_text_rough(name, line1, sizeof(line1), line2, sizeof(line2), 13);
    setting_detail_text(&snapshot->extra, snapshot->extra.settings_index, detail, sizeof(detail));

    fb_clear(display);
    draw_top_bar(display, snapshot);
    fb_draw_frame(display, 10, 18, 108, 30);
    draw_centered_text(display, 10, 108, 31, line1, FONT_BODY);
    if (detail[0] != '\0') {
        draw_centered_text(display, 10, 108, 43, detail, FONT_BODY);
    } else if (line2[0] != '\0') {
        draw_centered_text(display, 10, 108, 43, line2, FONT_BODY);
    }
    draw_bottom_nav3(display, "BACK", is_toggle ? state_text : "", right_hint);
    oled_flush(display);
}

/**
 * Format the selected LIGHTS row into two display lines.
 *
 * This keeps draw_lights_screen() focused on layout while this helper handles
 * the text for strobe, target, rate, constants, and presets.
 */
static void light_name(const walkie_extra_state_t *extra, char *line1, size_t len1, char *line2, size_t len2)
{
    switch (extra->lights_index) {
    case LIGHT_STROBE:
        snprintf(line1, len1, "STROBE");
        snprintf(line2, len2, "%s", extra->lights_mode == 1 ? "ON" : "OFF");
        break;
    case LIGHT_TARGET:
        snprintf(line1, len1, "TARGET");
        if (extra->lights_target == LIGHT_TARGET_LED) {
            snprintf(line2, len2, "LED");
        } else if (extra->lights_target == LIGHT_TARGET_LASER) {
            snprintf(line2, len2, "LASER");
        } else {
            snprintf(line2, len2, "BOTH");
        }
        break;
    case LIGHT_RATE:
        snprintf(line1, len1, "RATE");
        snprintf(line2, len2, "%dHZ", extra->lights_strobe_hz);
        break;
    case LIGHT_LED_CONST:
        snprintf(line1, len1, "LED CONST");
        snprintf(line2, len2, "%s", extra->lights_led_const ? "ON" : "OFF");
        break;
    case LIGHT_LASER_CONST:
        snprintf(line1, len1, "LASER ON");
        snprintf(line2, len2, "%s", extra->lights_laser_const ? "ON" : "OFF");
        break;
    case LIGHT_PRE1:
        snprintf(line1, len1, "PRESET 1");
        snprintf(line2, len2, "%s", extra->lights_mode == 2 ? "ON" : "OFF");
        break;
    case LIGHT_PRE2:
        snprintf(line1, len1, "PRESET 2");
        snprintf(line2, len2, "%s", extra->lights_mode == 3 ? "ON" : "OFF");
        break;
    default:
        snprintf(line1, len1, "PRESET 3");
        snprintf(line2, len2, "%s", extra->lights_mode == 4 ? "ON" : "OFF");
        break;
    }
}

/**
 * Draw the LIGHTS app control screen.
 *
 * Top-left/top-right move through rows, OK toggles/cycles the selected row, and
 * TL+ appears when holding top-left will rapidly increase strobe rate.
 */
static void draw_lights_screen(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot)
{
    char line1[16];
    char line2[16];
    const char *mid = snapshot->extra.lights_index == LIGHT_RATE ? "TL+" : "";

    light_name(&snapshot->extra, line1, sizeof(line1), line2, sizeof(line2));

    fb_clear(display);
    draw_top_bar(display, snapshot);
    fb_draw_frame(display, 10, 18, 108, 30);
    draw_centered_text(display, 10, 108, 31, line1, FONT_BODY);
    draw_centered_text(display, 10, 108, 43, line2, FONT_BODY);
    draw_bottom_nav3(display, "BACK", mid, "OK");
    oled_flush(display);
}

/**
 * Initialize the OLED display and SSD1306 controller.
 *
 * Creates the I2C bus/device, sends the SSD1306 init sequence, clears the
 * framebuffer, and marks the display ready only after the first flush succeeds.
 */
esp_err_t walkie_display_init(walkie_display_t *display, gpio_num_t sda, gpio_num_t scl)
{
    static const uint8_t init_seq[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0x2E, 0xAF,
    };

    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &display->bus), TAG, "i2c bus init failed");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x3C,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };

    esp_err_t err = i2c_master_bus_add_device(display->bus, &dev_config, &display->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "oled add device failed: %s", esp_err_to_name(err));
        return err;
    }

    fb_clear(display);
    ESP_RETURN_ON_ERROR(oled_send_cmds(display, init_seq, sizeof(init_seq)), TAG, "oled init sequence failed");
    ESP_RETURN_ON_ERROR(oled_flush(display), TAG, "oled flush failed");
    display->ready = true;
    return ESP_OK;
}

/**
 * Return whether the display handle is initialized and usable.
 */
bool walkie_display_ready(const walkie_display_t *display)
{
    return display != NULL && display->ready;
}

/**
 * Show the boot splash animation.
 *
 * The text is deliberately drawn below the yellow top band of the OLED modules
 * used in these walkies.
 */
void walkie_display_show_splash(walkie_display_t *display)
{
    if (!walkie_display_ready(display)) {
        return;
    }

    for (int reveal = 0; reveal <= 40; reveal += 4) {
        fb_clear(display);
        fb_draw_text(display, center_x(FONT_BODY, "VERMA"), 30, "VERMA", FONT_BODY);
        fb_draw_text(display, center_x(FONT_BODY, "INDUSTRIES"), 42, "INDUSTRIES", FONT_BODY);
        if (20 + reveal < OLED_HEIGHT) {
            fb_fill_rect(display, 0, 20 + reveal, OLED_WIDTH, OLED_HEIGHT - (20 + reveal), false);
        }
        oled_flush(display);
        vTaskDelay(pdMS_TO_TICKS(45));
    }

    vTaskDelay(pdMS_TO_TICKS(450));
}

/**
 * Redraw the entire OLED for the current UI snapshot.
 *
 * This is the public display entry point used by control_task(). It dispatches
 * to the appropriate screen renderer based on snapshot->ui_mode.
 */
void walkie_display_redraw(walkie_display_t *display, const walkie_ui_snapshot_t *snapshot)
{
    if (!walkie_display_ready(display) || snapshot == NULL) {
        return;
    }

    switch (snapshot->ui_mode) {
    case MODE_PTT:
        draw_ptt_screen(display, snapshot);
        break;
    case MODE_SCANNING:
        draw_scanning_screen(display, snapshot);
        break;
    case MODE_APPS:
        draw_apps_carousel(display, snapshot, snapshot->selected_app, 0);
        break;
    case MODE_SETTINGS:
        draw_settings_screen(display, snapshot);
        break;
    case MODE_KID:
        draw_kid_mode_screen(display, snapshot);
        break;
    case MODE_APP_VIEW:
    default:
        switch (snapshot->selected_app) {
        case APP_TX_WIFI:
            draw_panel_app(display, snapshot, "TX WIFI", "SEND READY");
            break;
        case APP_RX_WIFI:
            draw_panel_app(display, snapshot, "RX WIFI", "SCAN WAIT");
            break;
        case APP_TEXT:
            draw_panel_app(display, snapshot, "TEXT ONLY", "TYPE...");
            break;
        case APP_BUTTONS:
            draw_buttons_app(display, snapshot);
            break;
        case APP_LIGHTS:
            draw_lights_screen(display, snapshot);
            break;
        default:
            draw_kid_mode_screen(display, snapshot);
            break;
        }
        break;
    }
}

/**
 * Animate left/right movement in the apps carousel.
 *
 * The app index is already changed in app state; this just draws a short slide
 * between old and new positions to make navigation feel less abrupt.
 */
void walkie_display_animate_apps_change(walkie_display_t *display,
                                        const walkie_ui_snapshot_t *snapshot,
                                        int old_idx,
                                        int new_idx)
{
    int direction = 1;

    if (!walkie_display_ready(display) || snapshot == NULL || old_idx == new_idx) {
        return;
    }

    if ((old_idx == 0 && new_idx == APP_COUNT - 1) || new_idx < old_idx) {
        direction = -1;
    }
    if (old_idx == APP_COUNT - 1 && new_idx == 0) {
        direction = 1;
    }

    for (int step = 1; step <= 3; ++step) {
        int shift = (step * 12) / 3;
        draw_apps_carousel(display, snapshot, old_idx, -direction * shift);
        vTaskDelay(pdMS_TO_TICKS(18));
    }

    draw_apps_carousel(display, snapshot, new_idx, 0);
}
