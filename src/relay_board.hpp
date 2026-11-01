// Waveshare Modbus RTU Relay (D), 8 channels, on the display's RS485 port.
//
// Runs in its own FreeRTOS task: sends relay commands from a queue and reads back the
// relay states a few times per second, so the UI always shows what the board really did.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace relay
{
	class RelayBoard
	{
	public:
		static constexpr std::uint8_t DEVICE_ADDRESS = 1; // factory default
		static constexpr int BAUD_RATE = 9600;            // factory default (9600 8N1)

		// Starts the UART and the polling task
		bool start();

		// Ask for a relay to be switched. relayNumber is 1..8 as printed on the board.
		// Safe to call from any task.
		void request(std::uint8_t relayNumber, bool on);

		// Bit 0 = relay 1 ... bit 7 = relay 8, as last read from the board
		std::uint8_t states() const { return relayStates.load(); }

		// False until the board has answered, and again after several missed replies
		bool online() const { return isOnline.load(); }

	private:
		static void task_entry(void *self);
		void run();
		bool transaction(const std::uint8_t *request, std::size_t requestLength, std::uint8_t *reply, std::size_t &replyLength, std::size_t wantedLength);
		bool poll_states();
		void apply_pins();

		void *commandQueue = nullptr;
		std::atomic<std::uint8_t> relayStates{ 0 };
		std::atomic<bool> isOnline{ false };
		bool pinsSwapped = false;
		bool everOnline = false;
	};
}
