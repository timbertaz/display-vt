#include "gps.hpp"

#include "nmea.hpp"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace gps
{
	namespace
	{
		const char *TAG = "gps";

		// UART0 = logs/flashing, UART2 = RS485 relay board, so the GPS gets UART1.
		constexpr uart_port_t PORT = UART_NUM_1;
		constexpr int PIN_RX = 6; // sensor header signal pin
		constexpr int BAUD_RATE = 9600;
		constexpr std::uint32_t TIMEOUT_MS = 3000;

		std::uint32_t now_ms()
		{
			return xTaskGetTickCount() * portTICK_PERIOD_MS;
		}
	}

	bool Gps::start()
	{
		// UART installed inside the task on core 1 (core 0 has no interrupts to spare)
		return pdPASS == xTaskCreatePinnedToCore(task_entry, "gps", 4096, this, 2, nullptr, 1);
	}

	bool Gps::init_uart()
	{
		uart_config_t config = {};
		config.baud_rate = BAUD_RATE;
		config.data_bits = UART_DATA_8_BITS;
		config.parity = UART_PARITY_DISABLE;
		config.stop_bits = UART_STOP_BITS_1;
		config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
		config.source_clk = UART_SCLK_DEFAULT;

		esp_err_t error = uart_driver_install(PORT, 1024, 0, 0, nullptr, 0);
		if (ESP_OK == error)
		{
			error = uart_param_config(PORT, &config);
		}
		if (ESP_OK == error)
		{
			error = uart_set_pin(PORT, UART_PIN_NO_CHANGE, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
		}
		if (ESP_OK != error)
		{
			ESP_LOGE(TAG, "GPS UART init FAILED: %s", esp_err_to_name(error));
			return false;
		}
		ESP_LOGI(TAG, "Listening for GPS on GPIO%d at %d baud", PIN_RX, BAUD_RATE);
		return true;
	}

	Reading Gps::read() const
	{
		Reading r;
		r.connected = everHeard.load() && ((now_ms() - lastSentenceMs.load()) < TIMEOUT_MS);
		r.fix = r.connected && fix.load();
		r.speedKmh = r.fix ? (speedTenths.load() / 10.0f) : 0.0f;
		r.satellites = r.connected ? satellites.load() : 0;
		return r;
	}

	void Gps::task_entry(void *self)
	{
		static_cast<Gps *>(self)->run();
	}

	void Gps::run()
	{
		if (!init_uart())
		{
			vTaskDelete(nullptr);
			return;
		}
		nmea::Parser parser;
		bool hadFix = false;
		std::uint8_t buffer[128];

		// Until the first valid sentence: report what arrives every 5 s, and if bytes arrive but
		// never form valid NMEA, try the other common baud rates (some modules ship at 38400).
		static const int BAUD_RATES[] = { 9600, 38400, 4800, 115200, 57600, 19200 };
		std::size_t baudIndex = 0;
		std::uint32_t bytesSinceReport = 0;
		std::uint32_t lastReport = now_ms();

		while (true)
		{
			int got = uart_read_bytes(PORT, buffer, sizeof(buffer), pdMS_TO_TICKS(100));
			if (got > 0)
			{
				bytesSinceReport += static_cast<std::uint32_t>(got);
			}

			if (!everHeard.load() && ((now_ms() - lastReport) >= 5000))
			{
				if (0 == bytesSinceReport)
				{
					ESP_LOGW(TAG, "No data from the GPS on GPIO%d - check power and the GPS TX wire", PIN_RX);
				}
				else
				{
					ESP_LOGW(TAG, "%lu bytes at %d baud but no valid NMEA - trying another baud rate",
					         static_cast<unsigned long>(bytesSinceReport), BAUD_RATES[baudIndex]);
					baudIndex = (baudIndex + 1) % (sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]));
					uart_set_baudrate(PORT, BAUD_RATES[baudIndex]);
					uart_flush_input(PORT);
					ESP_LOGI(TAG, "Now listening at %d baud", BAUD_RATES[baudIndex]);
				}
				bytesSinceReport = 0;
				lastReport = now_ms();
			}

			for (int i = 0; i < got; i++)
			{
				if (parser.feed(static_cast<char>(buffer[i])))
				{
					const nmea::Fix &f = parser.fix();
					speedTenths = static_cast<std::int32_t>(f.speedKmh * 10.0f + 0.5f);
					satellites = f.satellites;
					fix = f.valid;
					lastSentenceMs = now_ms();
					if (!everHeard.load())
					{
						ESP_LOGI(TAG, "GPS connected (NMEA data arriving at %d baud)", BAUD_RATES[baudIndex]);
						everHeard = true;
					}
					if (f.valid != hadFix)
					{
						ESP_LOGI(TAG, "%s (%d satellites)", f.valid ? "GPS fix" : "GPS lost fix", f.satellites);
						hadFix = f.valid;
					}
				}
			}
		}
	}
}
