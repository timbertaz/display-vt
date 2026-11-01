#include "board.hpp"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <cstring>

namespace
{
	const char *TAG = "board";

	// ---------------- Pins (Waveshare ESP32-S3-Touch-LCD-7) ----------------
	constexpr i2c_port_t I2C_PORT = I2C_NUM_0;
	constexpr gpio_num_t I2C_SDA = GPIO_NUM_8;
	constexpr gpio_num_t I2C_SCL = GPIO_NUM_9;
	constexpr gpio_num_t TOUCH_INT = GPIO_NUM_4; // driven low during reset to select GT911 address 0x5D

	constexpr int H_RES = 800;
	constexpr int V_RES = 480;

	// ---------------- CH422G IO expander ----------------
	// Commands are sent as I2C addresses: 0x24 = mode register, 0x38 = output register (EXIO0..7)
	constexpr std::uint8_t CH422G_MODE = 0x24;
	constexpr std::uint8_t CH422G_OUTPUT = 0x38;
	constexpr std::uint8_t EXIO_TP_RST = 1 << 1;  // touch reset (active low)
	constexpr std::uint8_t EXIO_LCD_BL = 1 << 2;  // backlight
	constexpr std::uint8_t EXIO_LCD_RST = 1 << 3; // panel reset (active low)
	constexpr std::uint8_t EXIO_SD_CS = 1 << 4;   // SD card chip select (high = not selected)
	constexpr std::uint8_t EXIO_CAN_SEL = 1 << 5; // HIGH = CAN on GPIO19/20, LOW = native USB
	// Waveshare's own example ends with 0x1E, which clears EXIO5 and would switch CAN off.
	// We always keep EXIO5 high.

	// ---------------- GT911 touch ----------------
	std::uint8_t gt911Address = 0;
	lv_point_t lastPoint = { 0, 0 };
	bool lastPressed = false;

	esp_lcd_panel_handle_t panel = nullptr;
	SemaphoreHandle_t lvglMutex = nullptr;

	bool ch422g_write(std::uint8_t command, std::uint8_t value)
	{
		return ESP_OK == i2c_master_write_to_device(I2C_PORT, command, &value, 1, pdMS_TO_TICKS(100));
	}

	bool i2c_init()
	{
		i2c_config_t conf = {};
		conf.mode = I2C_MODE_MASTER;
		conf.sda_io_num = I2C_SDA;
		conf.scl_io_num = I2C_SCL;
		conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
		conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
		conf.master.clk_speed = 400000;
		if (ESP_OK != i2c_param_config(I2C_PORT, &conf))
		{
			return false;
		}
		esp_err_t err = i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
		return (ESP_OK == err) || (ESP_ERR_INVALID_STATE == err);
	}

	// Same sequence as Waveshare's example: hold INT low while releasing reset -> address 0x5D.
	// CAN mode (EXIO5) stays on the whole time.
	void reset_touch_and_select_can()
	{
		ch422g_write(CH422G_MODE, 0x01); // enable outputs

		gpio_config_t io = {};
		io.pin_bit_mask = 1ULL << TOUCH_INT;
		io.mode = GPIO_MODE_OUTPUT;
		gpio_config(&io);

		ch422g_write(CH422G_OUTPUT, EXIO_LCD_BL | EXIO_LCD_RST | EXIO_CAN_SEL); // touch in reset
		vTaskDelay(pdMS_TO_TICKS(100));
		gpio_set_level(TOUCH_INT, 0);
		vTaskDelay(pdMS_TO_TICKS(100));
		ch422g_write(CH422G_OUTPUT, EXIO_TP_RST | EXIO_LCD_BL | EXIO_LCD_RST | EXIO_CAN_SEL); // release reset
		vTaskDelay(pdMS_TO_TICKS(200));
		gpio_set_direction(TOUCH_INT, GPIO_MODE_INPUT); // address is latched; let the GT911 drive INT again (we poll)

		bool ok = ch422g_write(CH422G_OUTPUT, EXIO_TP_RST | EXIO_LCD_BL | EXIO_LCD_RST | EXIO_SD_CS | EXIO_CAN_SEL);
		ESP_LOGI(TAG, "CH422G %s: backlight on, CAN mode selected", ok ? "OK" : "FAILED");
	}

	bool gt911_read(std::uint16_t reg, std::uint8_t *data, std::size_t length)
	{
		std::uint8_t regBytes[2] = { static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg & 0xFF) };
		return ESP_OK == i2c_master_write_read_device(I2C_PORT, gt911Address, regBytes, 2, data, length, pdMS_TO_TICKS(20));
	}

	bool gt911_write8(std::uint16_t reg, std::uint8_t value)
	{
		std::uint8_t bytes[3] = { static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg & 0xFF), value };
		return ESP_OK == i2c_master_write_to_device(I2C_PORT, gt911Address, bytes, 3, pdMS_TO_TICKS(20));
	}

	void gt911_probe()
	{
		const std::uint8_t candidates[] = { 0x5D, 0x14 };
		for (std::uint8_t address : candidates)
		{
			gt911Address = address;
			std::uint8_t productId[4] = {};
			if (gt911_read(0x8140, productId, sizeof(productId)))
			{
				ESP_LOGI(TAG, "GT911 touch found at 0x%02X (product id %c%c%c%c)", address, productId[0], productId[1], productId[2], productId[3]);
				return;
			}
		}
		gt911Address = 0;
		ESP_LOGW(TAG, "GT911 touch controller not found - screen will work without touch");
	}

	// LVGL asks for the touch state every ~30 ms
	void touch_read_cb(lv_indev_drv_t *, lv_indev_data_t *data)
	{
		std::uint8_t status = 0;
		if ((0 != gt911Address) && gt911_read(0x814E, &status, 1) && (status & 0x80)) // 0x80 = new data ready
		{
			std::uint8_t touches = status & 0x0F;
			if ((touches > 0) && (touches <= 5))
			{
				std::uint8_t point[4] = {};
				if (gt911_read(0x8150, point, sizeof(point)))
				{
					lv_coord_t x = static_cast<lv_coord_t>(point[0] | (point[1] << 8));
					lv_coord_t y = static_cast<lv_coord_t>(point[2] | (point[3] << 8));
					lastPoint.x = (x < H_RES) ? x : (H_RES - 1);
					lastPoint.y = (y < V_RES) ? y : (V_RES - 1);
					lastPressed = true;
				}
			}
			else
			{
				lastPressed = false;
			}
			gt911_write8(0x814E, 0); // tell the GT911 we read it
		}
		data->point = lastPoint;
		data->state = lastPressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	}

	bool panel_init()
	{
		esp_lcd_rgb_panel_config_t config;
		std::memset(&config, 0, sizeof(config));
		config.clk_src = LCD_CLK_SRC_DEFAULT;
		config.timings.pclk_hz = 16 * 1000 * 1000;
		config.timings.h_res = H_RES;
		config.timings.v_res = V_RES;
		config.timings.hsync_pulse_width = 4;
		config.timings.hsync_back_porch = 8;
		config.timings.hsync_front_porch = 8;
		config.timings.vsync_pulse_width = 4;
		config.timings.vsync_back_porch = 8;
		config.timings.vsync_front_porch = 8;
		config.timings.flags.pclk_active_neg = 1;
		config.data_width = 16;
		config.bits_per_pixel = 16;
		config.num_fbs = 1;
		config.bounce_buffer_size_px = H_RES * 10;
		config.hsync_gpio_num = GPIO_NUM_46;
		config.vsync_gpio_num = GPIO_NUM_3;
		config.de_gpio_num = GPIO_NUM_5;
		config.pclk_gpio_num = GPIO_NUM_7;
		config.disp_gpio_num = -1;

		const int dataPins[16] = { 14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40 };
		for (int i = 0; i < 16; i++)
		{
			config.data_gpio_nums[i] = dataPins[i];
		}
		config.flags.fb_in_psram = 1;

		if (ESP_OK != esp_lcd_new_rgb_panel(&config, &panel))
		{
			ESP_LOGE(TAG, "RGB panel driver install FAILED");
			return false;
		}
		if (ESP_OK != esp_lcd_panel_init(panel))
		{
			ESP_LOGE(TAG, "RGB panel init FAILED");
			return false;
		}
		ESP_LOGI(TAG, "RGB panel running %dx%d", H_RES, V_RES);
		return true;
	}

	void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels)
	{
		// For RGB panels this copies into the frame buffer right away
		esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
		lv_disp_flush_ready(drv);
	}

	void tick_cb(void *)
	{
		lv_tick_inc(2);
	}

	void lvgl_task(void *)
	{
		while (true)
		{
			std::uint32_t waitMs = 10;
			if (board::lvgl_lock())
			{
				waitMs = lv_timer_handler();
				board::lvgl_unlock();
			}
			if (waitMs > 20)
			{
				waitMs = 20;
			}
			TickType_t ticks = pdMS_TO_TICKS(waitMs);
			vTaskDelay((ticks > 0) ? ticks : 1);
		}
	}
}

namespace board
{
	bool init()
	{
		if (!i2c_init())
		{
			ESP_LOGE(TAG, "I2C init FAILED");
		}
		reset_touch_and_select_can();
		gt911_probe();

		if (!panel_init())
		{
			return false;
		}

		lvglMutex = xSemaphoreCreateRecursiveMutex();
		lv_init();

		// Two partial draw buffers (60 lines each) in PSRAM
		constexpr std::size_t BUFFER_PIXELS = H_RES * 60;
		static lv_disp_draw_buf_t drawBuffer;
		auto *buf1 = static_cast<lv_color_t *>(heap_caps_malloc(BUFFER_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM));
		auto *buf2 = static_cast<lv_color_t *>(heap_caps_malloc(BUFFER_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM));
		if ((nullptr == buf1) || (nullptr == buf2))
		{
			ESP_LOGE(TAG, "Could not allocate LVGL draw buffers");
			return false;
		}
		lv_disp_draw_buf_init(&drawBuffer, buf1, buf2, BUFFER_PIXELS);

		static lv_disp_drv_t displayDriver;
		lv_disp_drv_init(&displayDriver);
		displayDriver.hor_res = H_RES;
		displayDriver.ver_res = V_RES;
		displayDriver.flush_cb = flush_cb;
		displayDriver.draw_buf = &drawBuffer;
		lv_disp_drv_register(&displayDriver);

		static lv_indev_drv_t touchDriver;
		lv_indev_drv_init(&touchDriver);
		touchDriver.type = LV_INDEV_TYPE_POINTER;
		touchDriver.read_cb = touch_read_cb;
		touchDriver.long_press_time = 400;        // first "still held" after 0.4 s
		touchDriver.long_press_repeat_time = 200; // then every 0.2 s, like AgIsoVirtualTerminal
		lv_indev_drv_register(&touchDriver);

		esp_timer_create_args_t timerArgs = {};
		timerArgs.callback = tick_cb;
		timerArgs.name = "lv_tick";
		esp_timer_handle_t tickTimer = nullptr;
		esp_timer_create(&timerArgs, &tickTimer);
		esp_timer_start_periodic(tickTimer, 2000);

		// LVGL on core 1; the CAN stack and the VT server run on core 0 in app_main
		xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, nullptr, 2, nullptr, 1);
		ESP_LOGI(TAG, "LVGL started");
		return true;
	}

	bool lvgl_lock(std::uint32_t timeoutMs)
	{
		TickType_t ticks = (0xFFFFFFFF == timeoutMs) ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
		return pdTRUE == xSemaphoreTakeRecursive(lvglMutex, ticks);
	}

	void lvgl_unlock()
	{
		xSemaphoreGiveRecursive(lvglMutex);
	}

	bool touch_available()
	{
		return 0 != gt911Address;
	}
}
