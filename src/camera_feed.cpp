#include "camera_feed.hpp"

#include "board.hpp"
#include "mjpeg.hpp"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace camera
{
	namespace
	{
		const char *TAG = "camera";

		// ---- Must match camera-node/src/main.cpp ----
		constexpr char WIFI_SSID[] = "HESAB-CAM";
		constexpr char WIFI_PASSWORD[] = "hesabcam1";
		constexpr int WIFI_CHANNEL = 6;
		constexpr char CAMERA_IP[] = "192.168.4.1";
		constexpr int CONTROL_PORT = 80;
		constexpr int STREAM_PORT = 81;

		constexpr std::size_t JPEG_CAPACITY = 200 * 1024; // VGA at quality 12 is usually 20-60 KB
		constexpr std::uint32_t NO_PICTURE_TIMEOUT_MS = 3000;

		std::uint32_t now_ms()
		{
			return xTaskGetTickCount() * portTICK_PERIOD_MS;
		}

		int connect_to(int port, int timeoutMs)
		{
			int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (sock < 0)
			{
				return -1;
			}
			timeval timeout = {};
			timeout.tv_sec = timeoutMs / 1000;
			timeout.tv_usec = (timeoutMs % 1000) * 1000;
			setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
			setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

			sockaddr_in address = {};
			address.sin_family = AF_INET;
			address.sin_port = htons(port);
			inet_pton(AF_INET, CAMERA_IP, &address.sin_addr);
			if (0 != connect(sock, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
			{
				close(sock);
				return -1;
			}
			return sock;
		}

		bool send_all(int sock, const char *text)
		{
			std::size_t length = std::strlen(text);
			while (length > 0)
			{
				int sent = send(sock, text, length, 0);
				if (sent <= 0)
				{
					return false;
				}
				text += sent;
				length -= static_cast<std::size_t>(sent);
			}
			return true;
		}
	}

	bool Feed::start(FrameHandler onFrame)
	{
		handler = std::move(onFrame);

		// Big buffers in PSRAM: the incoming JPEG and two pictures (one shown, one being decoded)
		jpegBuffer = static_cast<std::uint8_t *>(heap_caps_malloc(JPEG_CAPACITY, MALLOC_CAP_SPIRAM));
		const std::size_t pictureBytes = MAX_WIDTH * MAX_HEIGHT * sizeof(std::uint16_t);
		for (auto &picture : pixels)
		{
			picture = static_cast<std::uint16_t *>(heap_caps_aligned_alloc(16, pictureBytes, MALLOC_CAP_SPIRAM)); // decoder wants 16-byte alignment
		}
		if ((nullptr == jpegBuffer) || (nullptr == pixels[0]) || (nullptr == pixels[1]))
		{
			ESP_LOGE(TAG, "Not enough PSRAM for the camera picture");
			return false;
		}

		// WiFi is started from core 1: drivers take their interrupt from the core that starts
		// them, and core 0 has none to spare. The stream runs on core 0 next to the CAN loop
		// (core 1 already draws the screen).
		return (pdPASS == xTaskCreatePinnedToCore(wifi_task_entry, "cam_wifi", 4096, this, 3, nullptr, 1)) &&
		  (pdPASS == xTaskCreatePinnedToCore(feed_task_entry, "camera", 6144, this, 1, nullptr, 0));
	}

	void Feed::request_led(bool on)
	{
		ledWanted = on ? 1 : 0;
	}

	// ------------------------------------------------------------------ WiFi

	void Feed::wifi_task_entry(void *self)
	{
		static_cast<Feed *>(self)->start_wifi();
		vTaskDelete(nullptr);
	}

	void Feed::start_wifi()
	{
		esp_err_t error = nvs_flash_init(); // WiFi keeps its calibration in NVS
		if ((ESP_ERR_NVS_NO_FREE_PAGES == error) || (ESP_ERR_NVS_NEW_VERSION_FOUND == error))
		{
			nvs_flash_erase();
			error = nvs_flash_init();
		}
		if (ESP_OK == error)
		{
			error = esp_netif_init();
		}
		if (ESP_OK == error)
		{
			error = esp_event_loop_create_default();
		}
		if (ESP_OK == error)
		{
			esp_netif_create_default_wifi_sta();
			wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
			error = esp_wifi_init(&init);
		}
		if (ESP_OK != error)
		{
			ESP_LOGE(TAG, "WiFi start FAILED: %s (the camera won't work, everything else does)", esp_err_to_name(error));
			return;
		}
		esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, this);
		esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, this);

		wifi_config_t config = {};
		std::memcpy(config.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
		std::memcpy(config.sta.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD) - 1);
		config.sta.channel = WIFI_CHANNEL; // look there first: joins faster
		config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

		esp_wifi_set_mode(WIFI_MODE_STA);
		esp_wifi_set_config(WIFI_IF_STA, &config);
		error = esp_wifi_start();
		esp_wifi_set_ps(WIFI_PS_NONE); // no power saving: smoother picture
		ESP_LOGI(TAG, "WiFi: looking for the camera network \"%s\" (%s)", WIFI_SSID, esp_err_to_name(error));
	}

	void Feed::on_wifi_event(void *arg, const char *base, std::int32_t id, void *data)
	{
		auto *self = static_cast<Feed *>(arg);
		if (WIFI_EVENT == base)
		{
			if (WIFI_EVENT_STA_START == id)
			{
				esp_wifi_connect();
			}
			else if (WIFI_EVENT_STA_DISCONNECTED == id)
			{
				if (self->wifiUp.load() || !self->wifiLostLogged)
				{
					ESP_LOGW(TAG, "WiFi: camera network not there (is the camera powered?) - retrying");
					self->wifiLostLogged = true;
				}
				self->wifiUp = false;
				esp_wifi_connect(); // keeps trying until the camera is back
			}
		}
		else if ((IP_EVENT == base) && (IP_EVENT_STA_GOT_IP == id))
		{
			auto *event = static_cast<ip_event_got_ip_t *>(data);
			ESP_LOGI(TAG, "WiFi: joined the camera network, display IP " IPSTR, IP2STR(&event->ip_info.ip));
			self->wifiUp = true;
			self->wifiLostLogged = false;
		}
	}

	// ------------------------------------------------------------------ stream

	void Feed::feed_task_entry(void *self)
	{
		static_cast<Feed *>(self)->run();
	}

	void Feed::run()
	{
		bool synced = false;
		while (true)
		{
			if (!wifiUp.load())
			{
				currentState = State::NoWifi;
				fpsTimes10 = 0;
				synced = false;
				vTaskDelay(pdMS_TO_TICKS(200));
				continue;
			}
			currentState = State::Connecting;

			// Learn the LED state once after joining, then handle button presses
			if (!synced)
			{
				synced = http_get("/status");
			}
			int led = ledWanted.exchange(-1);
			if (led >= 0)
			{
				char path[16];
				std::snprintf(path, sizeof(path), "/led?on=%d", led);
				if (!http_get(path))
				{
					int none = -1;
					ledWanted.compare_exchange_strong(none, led); // try again, unless pressed again meanwhile
				}
			}

			if (wanted.load())
			{
				stream_once();
			}
			vTaskDelay(pdMS_TO_TICKS(wanted.load() ? 500 : 100));
		}
	}

	void Feed::stream_once()
	{
		int sock = connect_to(STREAM_PORT, 500);
		if (sock < 0)
		{
			return;
		}
		if (!send_all(sock, "GET /stream HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n"))
		{
			close(sock);
			return;
		}
		ESP_LOGI(TAG, "Stream started");

		mjpeg::StreamParser parser(jpegBuffer, JPEG_CAPACITY, [this](const std::uint8_t *jpeg, std::size_t length) {
			handle_jpeg(jpeg, length);
		});
		lastFrameMs = now_ms();
		fpsWindowStartMs = lastFrameMs;
		fpsWindowFrames = 0;
		static std::uint8_t buffer[4096];

		while (wanted.load() && wifiUp.load())
		{
			int got = recv(sock, buffer, sizeof(buffer), 0);
			if (got > 0)
			{
				if (!parser.feed(buffer, static_cast<std::size_t>(got)))
				{
					ESP_LOGW(TAG, "Camera refused the stream (busy or camera not started)");
					break;
				}
			}
			else if ((got < 0) && ((EAGAIN == errno) || (EWOULDBLOCK == errno)))
			{
				// receive timeout: just check the loop conditions again
			}
			else
			{
				break; // closed
			}

			if (now_ms() - lastFrameMs > NO_PICTURE_TIMEOUT_MS)
			{
				ESP_LOGW(TAG, "No pictures for %lu s - reconnecting", static_cast<unsigned long>(NO_PICTURE_TIMEOUT_MS / 1000));
				break;
			}

			// LED button pressed while streaming: send it on a second connection
			int led = ledWanted.exchange(-1);
			if (led >= 0)
			{
				char path[16];
				std::snprintf(path, sizeof(path), "/led?on=%d", led);
				http_get(path);
			}
		}
		close(sock);
		fpsTimes10 = 0;
		ESP_LOGI(TAG, "Stream stopped");
	}

	void Feed::handle_jpeg(const std::uint8_t *jpeg, std::size_t length)
	{
		jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
		config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE; // LVGL's 16-bit colour, no byte swap

		jpeg_dec_handle_t decoder = nullptr;
		if (JPEG_ERR_OK != jpeg_dec_open(&config, &decoder))
		{
			return;
		}
		jpeg_dec_io_t io = {};
		io.inbuf = const_cast<std::uint8_t *>(jpeg);
		io.inbuf_len = static_cast<int>(length);
		jpeg_dec_header_info_t info = {};
		int outLength = 0;
		bool ok = (JPEG_ERR_OK == jpeg_dec_parse_header(decoder, &io, &info)) &&
		  (info.width <= MAX_WIDTH) && (info.height <= MAX_HEIGHT) &&
		  (JPEG_ERR_OK == jpeg_dec_get_outbuf_len(decoder, &outLength)) &&
		  (outLength <= static_cast<int>(MAX_WIDTH * MAX_HEIGHT * sizeof(std::uint16_t)));
		if (ok)
		{
			io.outbuf = reinterpret_cast<std::uint8_t *>(pixels[backBuffer]);
			ok = (JPEG_ERR_OK == jpeg_dec_process(decoder, &io));
		}
		jpeg_dec_close(decoder);

		if (!ok)
		{
			if (0 == (decodeErrors++ % 50))
			{
				ESP_LOGW(TAG, "Could not decode a picture (%u bytes, %ux%u)", static_cast<unsigned>(length), info.width, info.height);
			}
			return;
		}

		// Show it: the UI now points at this buffer; the next picture goes into the other one
		if (board::lvgl_lock(100))
		{
			if (handler)
			{
				handler(pixels[backBuffer], info.width, info.height);
			}
			board::lvgl_unlock();
			backBuffer ^= 1;
		}

		std::uint32_t now = now_ms();
		lastFrameMs = now;
		currentState = State::Streaming;
		fpsWindowFrames++;
		if (now - fpsWindowStartMs >= 1000)
		{
			fpsTimes10 = fpsWindowFrames * 10000 / (now - fpsWindowStartMs);
			fpsWindowStartMs = now;
			fpsWindowFrames = 0;
		}
	}

	// Short request to the camera's control port; reads back the LED state from the JSON reply
	bool Feed::http_get(const char *path)
	{
		int sock = connect_to(CONTROL_PORT, 1000);
		if (sock < 0)
		{
			return false;
		}
		char request[96];
		std::snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: 192.168.4.1\r\nConnection: close\r\n\r\n", path);
		bool ok = send_all(sock, request);

		char reply[512];
		int length = 0;
		while (ok && (length < static_cast<int>(sizeof(reply)) - 1))
		{
			int got = recv(sock, reply + length, sizeof(reply) - 1 - length, 0);
			if (got <= 0)
			{
				break;
			}
			length += got;
		}
		close(sock);
		reply[length] = '\0';

		const char *led = std::strstr(reply, "\"led\":");
		if (nullptr == led)
		{
			return false;
		}
		ledConfirmed = ('1' == led[6]);
		return true;
	}
}
