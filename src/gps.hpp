// GY-GPS6MV2 (u-blox NEO-6M) on the display's sensor header.
// Only the GPS TX line is needed: GPS TX -> GPIO6. 9600 baud, NMEA (the module's defaults).
#pragma once

#include <atomic>
#include <cstdint>

namespace gps
{
	struct Reading
	{
		bool connected;  // NMEA sentences arriving in the last 3 s
		bool fix;        // receiver has a valid position/speed
		float speedKmh;
		int satellites;
	};

	class Gps
	{
	public:
		bool start();
		Reading read() const;

	private:
		static void task_entry(void *self);
		void run();

		std::atomic<std::int32_t> speedTenths{ 0 };
		std::atomic<std::int32_t> satellites{ 0 };
		std::atomic<bool> fix{ false };
		std::atomic<std::uint32_t> lastSentenceMs{ 0 };
		std::atomic<bool> everHeard{ false };
	};
}
