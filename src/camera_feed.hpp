// Reversing camera over WiFi.
//
// The camera node (camera-node/ in this repo) makes its own WiFi network. The display joins
// it, and while the Camera tab is open reads the MJPEG stream, decodes each JPEG to RGB565
// (esp_new_jpeg, uses the S3's vector instructions) and hands the picture to the UI.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace camera
{
	enum class State : std::uint8_t
	{
		NoWifi,     // camera's WiFi network not found / not joined
		Connecting, // on the WiFi, waiting for the stream
		Streaming   // pictures arriving
	};

	class Feed
	{
	public:
		static constexpr int MAX_WIDTH = 640;
		static constexpr int MAX_HEIGHT = 480;

		// Called from the feed task WITH the LVGL lock held, for every decoded picture (RGB565).
		using FrameHandler = std::function<void(std::uint16_t *pixels, int width, int height)>;

		bool start(FrameHandler onFrame);

		// Only stream while someone looks at it (saves CPU and memory bandwidth for the screen)
		void set_active(bool active) { wanted = active; }

		// Camera LEDs. led_on() is what the camera confirmed.
		void request_led(bool on);
		bool led_on() const { return ledConfirmed.load(); }

		State state() const { return currentState.load(); }
		float fps() const { return fpsTimes10.load() / 10.0f; }

	private:
		static void wifi_task_entry(void *self);
		static void feed_task_entry(void *self);
		static void decode_task_entry(void *self);
		void decode_loop();
		void start_wifi();
		void run();
		void stream_once();
		bool http_get(const char *path);
		void handle_jpeg(const std::uint8_t *jpeg, std::size_t length);

		FrameHandler handler;
		// Three JPEG buffers: one being received, the newest complete one, one being decoded.
		// Receiving (core 1) and decoding (core 0) run at the same time; old pictures are dropped.
		std::uint8_t *jpegBuffers[3] = { nullptr, nullptr, nullptr };
		std::size_t jpegLengths[3] = { 0, 0, 0 };
		int fillingIndex = 0;  // receive task only
		int readyIndex = 1;    // shared, guarded by bufferLock
		int decodingIndex = 2; // decode task only
		bool readyIsNew = false;
		void *bufferLock = nullptr; // portMUX_TYPE
		void *decodeTask = nullptr;
		std::uint16_t *pixels[2] = { nullptr, nullptr };
		int backBuffer = 0;

		std::atomic<bool> wanted{ false };
		std::atomic<bool> wifiUp{ false };
		std::atomic<int> ledWanted{ -1 }; // -1 = no request pending
		std::atomic<bool> ledConfirmed{ false };
		std::atomic<State> currentState{ State::NoWifi };
		std::atomic<std::uint32_t> fpsTimes10{ 0 };
		std::uint32_t decodeErrors = 0;
		std::uint32_t lastFrameMs = 0;
		std::uint32_t fpsWindowStartMs = 0;
		std::uint32_t fpsWindowFrames = 0;
		// Where the display's time goes (logged every 5 s while streaming)
		std::uint64_t decodeUs = 0;
		std::uint64_t showUs = 0;
		std::uint32_t timedFrames = 0;
		std::uint32_t timingStartMs = 0;
		bool wifiLostLogged = false;

		static void on_wifi_event(void *self, const char *base, std::int32_t id, void *data);
	};
}
