#include "relay_board.hpp"

#include "modbus_rtu.hpp"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace relay
{
	namespace
	{
		const char *TAG = "relays";

		// The display's RS485 port (automatic direction control on the board).
		// Waveshare's RS485 example uses TX = GPIO16, RX = GPIO15, while their pin table can be
		// read the other way round. If the relay board doesn't answer, we simply try the other way.
		constexpr uart_port_t PORT = UART_NUM_2;
		constexpr int PIN_A = 16;
		constexpr int PIN_B = 15;

		constexpr std::uint32_t POLL_INTERVAL_MS = 300;
		constexpr int FAILURES_BEFORE_OFFLINE = 3;

		struct Command
		{
			std::uint8_t relayNumber;
			bool on;
		};
	}

	bool RelayBoard::start()
	{
		uart_config_t config = {};
		config.baud_rate = BAUD_RATE;
		config.data_bits = UART_DATA_8_BITS;
		config.parity = UART_PARITY_DISABLE;
		config.stop_bits = UART_STOP_BITS_1;
		config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
		config.source_clk = UART_SCLK_DEFAULT;

		if ((ESP_OK != uart_driver_install(PORT, 256, 0, 0, nullptr, 0)) ||
		    (ESP_OK != uart_param_config(PORT, &config)))
		{
			ESP_LOGE(TAG, "RS485 UART init FAILED");
			return false;
		}
		apply_pins();

		commandQueue = xQueueCreate(16, sizeof(Command));
		xTaskCreatePinnedToCore(task_entry, "relays", 4096, this, 3, nullptr, 0);
		ESP_LOGI(TAG, "RS485 started, looking for relay board at address %u, %d baud", DEVICE_ADDRESS, BAUD_RATE);
		return true;
	}

	void RelayBoard::apply_pins()
	{
		int tx = pinsSwapped ? PIN_B : PIN_A;
		int rx = pinsSwapped ? PIN_A : PIN_B;
		uart_set_pin(PORT, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
		ESP_LOGI(TAG, "RS485 pins: TX = GPIO%d, RX = GPIO%d", tx, rx);
	}

	void RelayBoard::request(std::uint8_t relayNumber, bool on)
	{
		if ((nullptr == commandQueue) || (relayNumber < 1) || (relayNumber > 8))
		{
			return;
		}
		Command command = { relayNumber, on };
		xQueueSend(static_cast<QueueHandle_t>(commandQueue), &command, 0);
	}

	void RelayBoard::task_entry(void *self)
	{
		static_cast<RelayBoard *>(self)->run();
	}

	bool RelayBoard::transaction(const std::uint8_t *request, std::size_t requestLength, std::uint8_t *reply, std::size_t &replyLength, std::size_t capacity)
	{
		uart_flush_input(PORT);
		uart_write_bytes(PORT, reinterpret_cast<const char *>(request), requestLength);
		uart_wait_tx_done(PORT, pdMS_TO_TICKS(100));

		// Collect bytes until the line has been quiet for a bit (end of the reply) or 200 ms passed
		replyLength = 0;
		TickType_t start = xTaskGetTickCount();
		while ((replyLength < capacity) && ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(200)))
		{
			int got = uart_read_bytes(PORT, reply + replyLength, capacity - replyLength, pdMS_TO_TICKS(20));
			if (got > 0)
			{
				replyLength += static_cast<std::size_t>(got);
			}
			else if (replyLength > 0)
			{
				break; // quiet after receiving something: reply complete
			}
		}
		return replyLength > 0;
	}

	bool RelayBoard::poll_states()
	{
		auto request = modbus::read_coils(DEVICE_ADDRESS, 0, 8);
		std::uint8_t reply[64];
		std::size_t length = 0;
		std::uint8_t bits = 0;

		if (transaction(request.data(), request.size(), reply, length, sizeof(reply)) &&
		    modbus::find_read_coils_reply(reply, length, DEVICE_ADDRESS, bits))
		{
			if (bits != relayStates.load())
			{
				ESP_LOGI(TAG, "Relay states: 0x%02X", bits);
			}
			relayStates = bits;
			return true;
		}
		return false;
	}

	void RelayBoard::run()
	{
		int failures = 0;
		TickType_t lastPoll = 0;

		while (true)
		{
			// 1. Commands from the UI
			Command command;
			bool switched = false;
			while (xQueueReceive(static_cast<QueueHandle_t>(commandQueue), &command, 0) == pdTRUE)
			{
				auto frame = modbus::write_single_coil(DEVICE_ADDRESS, command.relayNumber - 1, command.on);
				std::uint8_t reply[64];
				std::size_t length = 0;
				bool ok = transaction(frame.data(), frame.size(), reply, length, sizeof(reply)) &&
				  modbus::find_frame(reply, length, frame);
				ESP_LOGI(TAG, "Relay %u -> %s%s", command.relayNumber, command.on ? "ON" : "OFF", ok ? "" : " (no confirmation)");
				switched = true;
			}

			// 2. Read back the real states: right after a command, otherwise every 300 ms
			if (switched || ((xTaskGetTickCount() - lastPoll) >= pdMS_TO_TICKS(POLL_INTERVAL_MS)))
			{
				lastPoll = xTaskGetTickCount();
				if (poll_states())
				{
					if (!isOnline.load())
					{
						ESP_LOGI(TAG, "Relay board online");
					}
					failures = 0;
					everOnline = true;
					isOnline = true;
				}
				else if (++failures >= FAILURES_BEFORE_OFFLINE)
				{
					if (isOnline.load())
					{
						ESP_LOGW(TAG, "Relay board not answering");
					}
					isOnline = false;
					failures = 0;

					// Never heard from it: maybe TX/RX are the other way round on this board
					if (!everOnline)
					{
						pinsSwapped = !pinsSwapped;
						apply_pins();
					}
				}
			}

			vTaskDelay(pdMS_TO_TICKS(20));
		}
	}
}
