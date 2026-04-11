"""
Wokwi MicroPython OLED UI demo for the ESP32 walkie-talkie project.

Paste this whole file into Wokwi as `main.py`.

Hardware expected in Wokwi:
- ESP32
- SSD1306 OLED, 128x64, I2C address 0x3C
- OLED SCL -> GPIO22
- OLED SDA -> GPIO21

This is not the real firmware. It is a screenshot/demo loop that recreates the
main PTT screen, apps carousel, settings rows, and range-log/version settings
using MicroPython so the GUI can be captured in a simulator.
"""

from machine import I2C, Pin
import framebuf
import time


WIDTH = 128
HEIGHT = 64
OLED_ADDR = 0x3C


class SSD1306:
    """Minimal SSD1306 I2C driver bundled so the Wokwi paste is self-contained."""

    def __init__(self, width, height, i2c, addr=OLED_ADDR):
        self.width = width
        self.height = height
        self.i2c = i2c
        self.addr = addr
        self.pages = height // 8
        self.buffer = bytearray(self.pages * width)
        self.fb = framebuf.FrameBuffer(self.buffer, width, height, framebuf.MONO_VLSB)
        self.init_display()

    def write_cmd(self, cmd):
        self.i2c.writeto(self.addr, b"\x80" + bytes([cmd]))

    def write_data(self, data):
        self.i2c.writeto(self.addr, b"\x40" + data)

    def init_display(self):
        """Initialize the panel using the standard 128x64 SSD1306 sequence."""
        for cmd in (
            0xAE,
            0x20,
            0x00,
            0x40,
            0xA1,
            0xC8,
            0x81,
            0x7F,
            0xA4,
            0xA6,
            0xA8,
            0x3F,
            0xD3,
            0x00,
            0xD5,
            0x80,
            0xD9,
            0xF1,
            0xDA,
            0x12,
            0xDB,
            0x40,
            0x8D,
            0x14,
            0xAF,
        ):
            self.write_cmd(cmd)
        self.fill(0)
        self.show()

    def fill(self, color):
        self.fb.fill(color)

    def pixel(self, x, y, color):
        self.fb.pixel(x, y, color)

    def text(self, text, x, y, color=1):
        self.fb.text(text, x, y, color)

    def rect(self, x, y, w, h, color=1):
        self.fb.rect(x, y, w, h, color)

    def fill_rect(self, x, y, w, h, color=1):
        self.fb.fill_rect(x, y, w, h, color)

    def line(self, x1, y1, x2, y2, color=1):
        self.fb.line(x1, y1, x2, y2, color)

    def show(self):
        for page in range(self.pages):
            self.write_cmd(0xB0 | page)
            self.write_cmd(0x00)
            self.write_cmd(0x10)
            start = page * self.width
            self.write_data(self.buffer[start:start + self.width])


i2c = I2C(0, scl=Pin(22), sda=Pin(21), freq=400_000)
oled = SSD1306(WIDTH, HEIGHT, i2c)


def text_width(text):
    """MicroPython's built-in font is 8 pixels wide per character."""
    return len(text) * 8


def center_x(text, x=0, w=WIDTH):
    return x + max(0, (w - text_width(text)) // 2)


def centered(text, y, x=0, w=WIDTH, color=1):
    oled.text(text, center_x(text, x, w), y, color)


def clear():
    oled.fill(0)


def draw_top_bar(label="GREY", battery_pct=78, voltage="3.9", cpu=None):
    """Shared header: device name, optional CPU text, battery outline/voltage."""
    oled.text(label, 0, 0)

    if cpu is not None:
        cpu_text = "CPU{}%".format(cpu)
        oled.text(cpu_text, 50, 0)

    oled.rect(84, 0, 38, 12, 1)
    oled.fill_rect(122, 4, 3, 5, 1)
    fill_w = int(36 * max(0, min(100, battery_pct)) / 100)
    if fill_w:
        oled.fill_rect(85, 1, fill_w, 10, 1)

    # Clear a window over the battery fill so voltage remains readable.
    oled.fill_rect(88, 2, 25, 8, 0)
    oled.text(voltage, 89, 2)


def draw_bottom(left="", center="", right=""):
    """Bottom navigation hints used by the firmware UI."""
    oled.fill_rect(0, 54, WIDTH, 10, 0)
    if left:
        oled.text(left, 0, 56)
    if center:
        oled.text(center, center_x(center), 56)
    if right:
        oled.text(right, WIDTH - text_width(right), 56)


def draw_signal_meter(x=4, y=18, quality=80):
    """Five-bar signal meter based on the ESP-NOW RSSI quality percentage."""
    oled.rect(x, y, 20, 31, 1)
    bars = max(0, min(5, (quality + 19) // 20))
    base_y = y + 28
    for i in range(5):
        bar_h = 4 + i * 4
        bar_x = x + 3 + i * 3
        if i < bars:
            oled.fill_rect(bar_x, base_y - bar_h, 2, bar_h, 1)
        else:
            oled.fill_rect(bar_x, base_y - 1, 2, 1, 1)


def status_box(x, y, label, active=False):
    """Tiny RX/PTT box. Active boxes are inverted like the ESP-IDF UI."""
    if active:
        oled.fill_rect(x, y, 22, 14, 1)
        oled.text(label, x + 2, y + 3, 0)
    else:
        oled.rect(x, y, 22, 14, 1)
        oled.text(label, x + 2, y + 3, 1)


def card(x, y, w, h, line1, line2=None, selected=False):
    """Draw an apps/settings style card with optional selected double border."""
    oled.rect(x, y, w, h, 1)
    if selected:
        oled.rect(x + 1, y + 1, w - 2, h - 2, 1)

    if line2:
        centered(line1, y + 6, x, w)
        centered(line2, y + 17, x, w)
    else:
        centered(line1, y + 12, x, w)


def draw_ptt_home():
    """Screenshot screen: main PTT mode with channel, link, RX/PTT, and volume."""
    clear()
    draw_top_bar("GREY", 78, "3.9", cpu=42)
    draw_signal_meter(4, 18, 83)

    oled.rect(28, 17, 72, 32, 1)
    centered("< CH 06 >", 26, 28, 72)
    centered("LINK ON", 38, 28, 72)

    status_box(103, 18, "RX", True)
    status_box(103, 35, "PTT", False)
    draw_bottom("VOL 54%", "OFF", "APPS")
    oled.show()


def app_lines(index, side=False):
    apps = [
        ("TX", "WIFI"),
        ("RX", "WIFI"),
        ("TEXT", "ONLY"),
        ("BTN", "CTRL"),
        ("LIGHTS", None),
        ("KID", "MODE"),
    ]
    line1, line2 = apps[index % len(apps)]
    if side:
        return (line1[:3], None)
    return (line1, line2)


def draw_apps_menu(selected=4):
    """Screenshot screen: horizontally scrolling apps carousel."""
    clear()
    draw_top_bar("GREY", 78, "3.9")

    left = (selected - 1) % 6
    right = (selected + 1) % 6
    l1, l2 = app_lines(left, True)
    c1, c2 = app_lines(selected, False)
    r1, r2 = app_lines(right, True)

    card(6, 21, 26, 22, l1, l2, False)
    card(36, 18, 56, 28, c1, c2, True)
    card(96, 21, 26, 22, r1, r2, False)
    draw_bottom("BACK", "SETTING", "OK")
    oled.show()


def draw_settings(row_name, detail="", state=None, action=""):
    """Screenshot screen: one selected Settings row."""
    clear()
    draw_top_bar("GREY", 78, "3.9")
    oled.rect(10, 18, 108, 30, 1)

    if len(row_name) > 13 and " " in row_name:
        split = row_name.rfind(" ", 0, 14)
        line1 = row_name[:split]
        line2 = row_name[split + 1:]
    else:
        line1 = row_name
        line2 = ""

    centered(line1, 26, 10, 108)
    if detail:
        centered(detail, 38, 10, 108)
    elif line2:
        centered(line2, 38, 10, 108)

    middle = ""
    right = ""
    if state is not None:
        middle = "ON" if state else "OFF"
        right = "OK"
    elif action:
        right = action
    draw_bottom("BACK", middle, right)
    oled.show()


def draw_settings_menu():
    """Representative Settings screenshot: firmware version row."""
    draw_settings("FW VERSION", "V0.5.4")


def draw_log_dump_setting():
    """Screenshot screen: onboard log dump row and boot combo hint."""
    draw_settings("DUMP LOGS", "PTT+BL BOOT", action="DUMP")


def draw_audio_setting():
    """Screenshot screen: normal toggle row for comparison."""
    draw_settings("INCREASE MIC SENSE", state=True)


SCREENS = [
    (draw_ptt_home, 3.0),
    (lambda: draw_apps_menu(4), 3.0),
    (draw_audio_setting, 2.5),
    (draw_settings_menu, 2.5),
    (draw_log_dump_setting, 2.5),
]


while True:
    for draw, seconds in SCREENS:
        draw()
        time.sleep(seconds)

