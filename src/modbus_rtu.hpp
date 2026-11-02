// Minimal Modbus RTU framing: just what the relay board needs.
// Platform independent (no ESP-IDF), so it can be tested on a PC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace modbus
{
	// Modbus CRC-16 (polynomial 0xA001, start 0xFFFF). Sent low byte first.
	inline std::uint16_t crc16(const std::uint8_t *data, std::size_t length)
	{
		std::uint16_t crc = 0xFFFF;
		for (std::size_t i = 0; i < length; i++)
		{
			crc ^= data[i];
			for (int bit = 0; bit < 8; bit++)
			{
				crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001) : static_cast<std::uint16_t>(crc >> 1);
			}
		}
		return crc;
	}

	inline void append_crc(std::vector<std::uint8_t> &frame)
	{
		std::uint16_t crc = crc16(frame.data(), frame.size());
		frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
		frame.push_back(static_cast<std::uint8_t>(crc >> 8));
	}

	inline bool crc_ok(const std::uint8_t *frame, std::size_t length)
	{
		if (length < 4)
		{
			return false;
		}
		std::uint16_t crc = crc16(frame, length - 2);
		return (frame[length - 2] == (crc & 0xFF)) && (frame[length - 1] == (crc >> 8));
	}

	// Function 05: write single coil (relay). On = 0xFF00, off = 0x0000.
	inline std::vector<std::uint8_t> write_single_coil(std::uint8_t device, std::uint16_t coil, bool on)
	{
		std::vector<std::uint8_t> frame = { device, 0x05,
			                                  static_cast<std::uint8_t>(coil >> 8), static_cast<std::uint8_t>(coil & 0xFF),
			                                  static_cast<std::uint8_t>(on ? 0xFF : 0x00), 0x00 };
		append_crc(frame);
		return frame;
	}

	// Function 01: read coils (relay states)
	inline std::vector<std::uint8_t> read_coils(std::uint8_t device, std::uint16_t firstCoil, std::uint16_t count)
	{
		std::vector<std::uint8_t> frame = { device, 0x01,
			                                  static_cast<std::uint8_t>(firstCoil >> 8), static_cast<std::uint8_t>(firstCoil & 0xFF),
			                                  static_cast<std::uint8_t>(count >> 8), static_cast<std::uint8_t>(count & 0xFF) };
		append_crc(frame);
		return frame;
	}

	// Look for a valid "read coils" reply (up to 8 coils) anywhere in the received bytes.
	// Searching (instead of expecting it at byte 0) also copes with an echo of our own request.
	inline bool find_read_coils_reply(const std::uint8_t *data, std::size_t length, std::uint8_t device, std::uint8_t &coilBits)
	{
		for (std::size_t i = 0; i + 6 <= length; i++)
		{
			if ((data[i] == device) && (data[i + 1] == 0x01) && (data[i + 2] == 0x01) && crc_ok(&data[i], 6))
			{
				coilBits = data[i + 3];
				return true;
			}
		}
		return false;
	}

	// The reply to "write single coil" is an exact copy of the request
	inline bool find_frame(const std::uint8_t *data, std::size_t length, const std::vector<std::uint8_t> &wanted)
	{
		for (std::size_t i = 0; i + wanted.size() <= length; i++)
		{
			bool match = true;
			for (std::size_t j = 0; (j < wanted.size()) && match; j++)
			{
				match = (data[i + j] == wanted[j]);
			}
			if (match)
			{
				return true;
			}
		}
		return false;
	}
}
