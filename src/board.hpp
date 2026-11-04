// Waveshare ESP32-S3-Touch-LCD-7: board bring-up (I2C, CH422G IO expander, RGB panel,
// GT911 touch) and the LVGL port. Pin numbers and panel timings come from Waveshare's
// ESP-IDF example "08_lvgl_v8_demo".
#pragma once

#include <cstdint>

namespace board
{
	// I2C bus, CH422G (CAN mode, backlight, touch reset), RGB panel, touch, LVGL task.
	// Returns false if the panel could not be started.
	bool init();

	// LVGL is not thread safe: hold this lock around every lv_* call made outside the LVGL task.
	bool lvgl_lock(std::uint32_t timeoutMs = 0xFFFFFFFF);
	void lvgl_unlock();

	// Copies RGB565 pixels straight into the screen, past LVGL (x2/y2 exclusive).
	// Used for the camera picture: much faster than drawing it through LVGL. Hold the LVGL lock.
	void draw_pixels(int x1, int y1, int x2, int y2, const void *pixels);

	// True if the GT911 touch controller answered during init
	bool touch_available();
}
