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

		// Called from the feed task WITH the LVGL lock held. pixels stay valid until the next call.
		using FrameHandler = std::function<void(const std::uint16_t *pixels, int width, int height)>;

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
		void start_wifi();
		void run();
		void stream_once();
		bool http_get(const char *path);
		void handle_jpeg(const std::uint8_t *jpeg, std::size_t length);

		FrameHandler handler;
		std::uint8_t *jpegBuffer = nullptr;
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
		bool wifiLostLogged = false;

		static void on_wifi_event(void *self, const char *base, std::int32_t id, void *data);
	};
}
