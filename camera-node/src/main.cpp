// Reversing camera node: ESP32-S3-DevKitC-1 N16R8 + Waveshare OV5640 Camera Board (C)
//
// - Makes its own WiFi network (HESAB-CAM). The cab display joins it.
// - http://192.168.4.1/        test page for a phone (picture + buttons)
// - http://192.168.4.1:81/stream  MJPEG stream (what the display shows)
// - http://192.168.4.1/led?on=1   camera LEDs on (on=0: off)
// - http://192.168.4.1/set?mirror=1 / ?flip=1   picture orientation, remembered after a restart
// - http://192.168.4.1/status     JSON with the current state
//
// The OV5640 makes the JPEG itself, so the ESP32 only moves bytes from the camera to WiFi.
//
// Wiring (camera board pin -> DevKitC GPIO), see README.md:
//   3.3V -> 3V3, GND -> GND, SIOD -> 4, SIOC -> 5, VSYNC -> 6, HREF -> 7, PCLK -> 13, XCLK -> 15,
//   D7 -> 16, D6 -> 17, D5 -> 18, D4 -> 12, D3 -> 10, D2 -> 8, D1 -> 9, D0 -> 11,
//   PWDN -> 1, RST -> 2

#include "esp_camera.h"
#if __has_include("esp_camera_af.h")
#include "esp_camera_af.h"
#define HAVE_AUTOFOCUS 1
#else
#define HAVE_AUTOFOCUS 0
#endif

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
	const char *TAG = "camera";

	// ---- WiFi: must match display-vt/src/camera_feed.cpp ----
	constexpr char WIFI_SSID[] = "HESAB-CAM";
	constexpr char WIFI_PASSWORD[] = "hesabcam1"; // WPA2 needs at least 8 characters
	constexpr int WIFI_CHANNEL = 6;

	// ---- Camera board pins on the DevKitC (same layout as Espressif's ESP32-S3-EYE) ----
	constexpr int PIN_PWDN = 1;
	constexpr int PIN_RESET = 2;
	constexpr int PIN_XCLK = 15;
	constexpr int PIN_SIOD = 4;
	constexpr int PIN_SIOC = 5;
	constexpr int PIN_VSYNC = 6;
	constexpr int PIN_HREF = 7;
	constexpr int PIN_PCLK = 13;
	constexpr int PIN_D0 = 11;
	constexpr int PIN_D1 = 9;
	constexpr int PIN_D2 = 8;
	constexpr int PIN_D3 = 10;
	constexpr int PIN_D4 = 12;
	constexpr int PIN_D5 = 18;
	constexpr int PIN_D6 = 17;
	constexpr int PIN_D7 = 16;

	// 640x480 fits the display's camera page. Quality: 0-63, lower = better picture, bigger frames.
	constexpr framesize_t FRAME_SIZE = FRAMESIZE_VGA;
	constexpr int JPEG_QUALITY = 12;
	// 20 MHz is what Espressif uses. With long jumper wires a lower value (10 MHz) can help.
	constexpr int XCLK_HZ = 20000000;

	constexpr int MAX_STREAM_CLIENTS = 2;

	std::atomic<bool> cameraReady{ false };
	std::atomic<bool> ledOn{ false };
	std::atomic<bool> mirror{ true }; // reversing camera: mirrored like a rear-view mirror
	std::atomic<bool> flip{ false };
	std::atomic<int> streamClients{ 0 };
	std::atomic<std::uint32_t> framesSent{ 0 };
	std::atomic<std::uint32_t> fpsTimes10{ 0 };

	// ------------------------------------------------------------------ settings in flash

	void load_settings()
	{
		nvs_handle_t nvs;
		if (ESP_OK == nvs_open("camera", NVS_READONLY, &nvs))
		{
			std::uint8_t value;
			if (ESP_OK == nvs_get_u8(nvs, "mirror", &value))
			{
				mirror = (0 != value);
			}
			if (ESP_OK == nvs_get_u8(nvs, "flip", &value))
			{
				flip = (0 != value);
			}
			nvs_close(nvs);
		}
	}

	void save_settings()
	{
		nvs_handle_t nvs;
		if (ESP_OK == nvs_open("camera", NVS_READWRITE, &nvs))
		{
			nvs_set_u8(nvs, "mirror", mirror ? 1 : 0);
			nvs_set_u8(nvs, "flip", flip ? 1 : 0);
			nvs_commit(nvs);
			nvs_close(nvs);
		}
	}

	// ------------------------------------------------------------------ camera

	// The LEDs on the Waveshare board hang on the OV5640's STROBE pin: we drive it as a plain output
	void apply_led()
	{
		sensor_t *sensor = esp_camera_sensor_get();
		if (nullptr == sensor)
		{
			return;
		}
		sensor->set_reg(sensor, 0x3016, 0x02, 0x02);                  // STROBE pad: output enable
		sensor->set_reg(sensor, 0x301C, 0x02, 0x02);                  // STROBE pad: controlled by register
		sensor->set_reg(sensor, 0x3019, 0x02, ledOn ? 0x02 : 0x00);   // STROBE level
	}

	void apply_orientation()
	{
		sensor_t *sensor = esp_camera_sensor_get();
		if (nullptr != sensor)
		{
			sensor->set_hmirror(sensor, mirror ? 1 : 0);
			sensor->set_vflip(sensor, flip ? 1 : 0);
		}
	}

	camera_config_t make_config(pixformat_t format, framesize_t size, int buffers)
	{
		camera_config_t config = {};
		config.pin_pwdn = PIN_PWDN;
		config.pin_reset = PIN_RESET;
		config.pin_xclk = PIN_XCLK;
		config.pin_sccb_sda = PIN_SIOD;
		config.pin_sccb_scl = PIN_SIOC;
		config.pin_d7 = PIN_D7;
		config.pin_d6 = PIN_D6;
		config.pin_d5 = PIN_D5;
		config.pin_d4 = PIN_D4;
		config.pin_d3 = PIN_D3;
		config.pin_d2 = PIN_D2;
		config.pin_d1 = PIN_D1;
		config.pin_d0 = PIN_D0;
		config.pin_vsync = PIN_VSYNC;
		config.pin_href = PIN_HREF;
		config.pin_pclk = PIN_PCLK;
		config.xclk_freq_hz = XCLK_HZ;
		config.ledc_timer = LEDC_TIMER_0;
		config.ledc_channel = LEDC_CHANNEL_0;
		config.pixel_format = format;
		config.frame_size = size;
		config.jpeg_quality = JPEG_QUALITY;
		config.fb_count = buffers;
		config.fb_location = CAMERA_FB_IN_PSRAM;
		config.grab_mode = (buffers > 1) ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
		return config;
	}

	// Data wire test: the camera sends its built-in colour bars as plain grey values (no JPEG),
	// and we look at each data wire. A wire that never changes isn't connected; two wires that
	// always carry the same value are touching or go to the same pin. The bar values show swaps.
	void data_wire_test()
	{
		ESP_LOGW(TAG, "---- Data wire test (colour bars) ----");
		camera_config_t config = make_config(PIXFORMAT_GRAYSCALE, FRAMESIZE_QQVGA, 1);
		if (ESP_OK != esp_camera_init(&config))
		{
			ESP_LOGE(TAG, "Wire test: camera did not start");
			return;
		}
		sensor_t *sensor = esp_camera_sensor_get();
		sensor->set_hmirror(sensor, 0);
		sensor->set_colorbar(sensor, 1);

		camera_fb_t *frame = nullptr;
		for (int i = 0; i < 4; i++) // skip the first pictures while the sensor settles
		{
			if (nullptr != frame)
			{
				esp_camera_fb_return(frame);
			}
			frame = esp_camera_fb_get();
		}
		if (nullptr == frame)
		{
			ESP_LOGE(TAG, "Wire test: no picture at all -> check VSYNC -> GPIO%d, HREF -> GPIO%d, PCLK -> GPIO%d", PIN_VSYNC, PIN_HREF, PIN_PCLK);
			esp_camera_deinit();
			return;
		}

		const int pins[8] = { PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7 };
		std::uint32_t ones[8] = {};
		std::uint32_t same[8][8] = {};
		for (std::size_t n = 0; n < frame->len; n++)
		{
			std::uint8_t v = frame->buf[n];
			for (int a = 0; a < 8; a++)
			{
				ones[a] += (v >> a) & 1;
				for (int b = a + 1; b < 8; b++)
				{
					same[a][b] += (((v >> a) ^ (v >> b)) & 1) ? 0 : 1;
				}
			}
		}
		ESP_LOGI(TAG, "Picture %ux%u, %u bytes", static_cast<unsigned>(frame->width), static_cast<unsigned>(frame->height), static_cast<unsigned>(frame->len));
		for (int a = 0; a < 8; a++)
		{
			unsigned percent = static_cast<unsigned>(ones[a] * 100ULL / frame->len);
			const char *verdict = "OK";
			char buddy[48] = "";
			if (0 == ones[a])
			{
				verdict = "ALWAYS 0 -> not connected / broken";
			}
			else if (ones[a] == frame->len)
			{
				verdict = "ALWAYS 1 -> not connected / touching 3.3V";
			}
			else
			{
				for (int b = 0; b < 8; b++)
				{
					if (b == a)
					{
						continue;
					}
					std::uint32_t match = (a < b) ? same[a][b] : same[b][a];
					if (match * 1000ULL >= frame->len * 995ULL)
					{
						std::snprintf(buddy, sizeof(buddy), "always same as D%d (touching?)", b);
						verdict = buddy;
						break;
					}
				}
			}
			ESP_LOGI(TAG, "  D%d (GPIO%-2d): %3u%% ones  %s", a, pins[a], percent, verdict);
		}

		// Middle row, the centre of each of the 8 bars
		char bars[8 * 14 + 1] = "";
		std::size_t width = frame->width;
		std::size_t row = (frame->height / 2) * width;
		for (int bar = 0; bar < 8; bar++)
		{
			std::uint8_t v = frame->buf[row + (width / 8) * bar + width / 16];
			char bits[14];
			std::snprintf(bits, sizeof(bits), "%3u=%c%c%c%c%c%c%c%c ", v, (v & 0x80) ? '1' : '0', (v & 0x40) ? '1' : '0', (v & 0x20) ? '1' : '0',
			              (v & 0x10) ? '1' : '0', (v & 0x08) ? '1' : '0', (v & 0x04) ? '1' : '0', (v & 0x02) ? '1' : '0', (v & 0x01) ? '1' : '0');
			std::strcat(bars, bits);
		}
		ESP_LOGI(TAG, "  Bars: %s", bars);
		ESP_LOGW(TAG, "---- End of wire test ----");
		esp_camera_fb_return(frame);
		esp_camera_deinit();
	}

	bool start_camera()
	{
		camera_config_t config = make_config(PIXFORMAT_JPEG, FRAME_SIZE, 3); // one buffer per stream client + one being filled
		esp_err_t error = esp_camera_init(&config);
		if (ESP_OK != error)
		{
			ESP_LOGE(TAG, "Camera not found (%s). Check 3.3V/GND, SIOD -> GPIO%d, SIOC -> GPIO%d, XCLK -> GPIO%d, PWDN -> GPIO%d",
			         esp_err_to_name(error), PIN_SIOD, PIN_SIOC, PIN_XCLK, PIN_PWDN);
			return false;
		}

		sensor_t *sensor = esp_camera_sensor_get();
		ESP_LOGI(TAG, "Camera found: sensor PID 0x%04X (OV5640 = 0x5640)", (nullptr != sensor) ? sensor->id.PID : 0);
		apply_orientation();
		apply_led();

#if HAVE_AUTOFOCUS
		if ((nullptr != sensor) && esp_camera_af_is_supported(sensor))
		{
			esp_camera_af_config_t af = {};
			af.mode = ESP_CAMERA_AF_MODE_AUTO; // continuous
			af.timeout_ms = 3000;
			esp_err_t afError = esp_camera_af_init(sensor, &af);
			ESP_LOGI(TAG, "Autofocus: %s", (ESP_OK == afError) ? "on (continuous)" : esp_err_to_name(afError));
		}
#else
		ESP_LOGI(TAG, "Autofocus: not in this camera driver version, lens stays at its rest position");
#endif

		// Check that pictures really arrive (data/PCLK/VSYNC wiring)
		camera_fb_t *frame = esp_camera_fb_get();
		if (nullptr == frame)
		{
			ESP_LOGE(TAG, "Camera answers but sends no usable pictures. Check VSYNC -> GPIO%d, HREF -> GPIO%d, PCLK -> GPIO%d and D0-D7",
			         PIN_VSYNC, PIN_HREF, PIN_PCLK);
			esp_camera_deinit();
			data_wire_test();
			return false;
		}
		ESP_LOGI(TAG, "First picture: %ux%u, %u bytes", static_cast<unsigned>(frame->width), static_cast<unsigned>(frame->height), static_cast<unsigned>(frame->len));
		esp_camera_fb_return(frame);
		return true;
	}

	// ------------------------------------------------------------------ MJPEG stream (port 81)

	void stream_task(void *arg)
	{
		auto *req = static_cast<httpd_req_t *>(arg);
		ESP_LOGI(TAG, "Stream client connected (%d watching)", streamClients.load());

		httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
		httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
		httpd_resp_set_hdr(req, "Cache-Control", "no-store");

		while (true)
		{
			camera_fb_t *frame = esp_camera_fb_get();
			if (nullptr == frame)
			{
				vTaskDelay(pdMS_TO_TICKS(20));
				continue;
			}
			char head[96];
			int headLength = std::snprintf(head, sizeof(head), "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
			                               static_cast<unsigned>(frame->len));
			esp_err_t error = httpd_resp_send_chunk(req, head, headLength);
			if (ESP_OK == error)
			{
				error = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(frame->buf), frame->len);
			}
			esp_camera_fb_return(frame);
			if (ESP_OK != error)
			{
				break; // client gone
			}
			framesSent++;
		}

		httpd_req_async_handler_complete(req);
		streamClients--;
		ESP_LOGI(TAG, "Stream client left (%d watching)", streamClients.load());
		vTaskDelete(nullptr);
	}

	esp_err_t stream_handler(httpd_req_t *req)
	{
		if (!cameraReady)
		{
			httpd_resp_set_status(req, "503 Service Unavailable");
			return httpd_resp_sendstr(req, "camera not started");
		}
		if (streamClients >= MAX_STREAM_CLIENTS)
		{
			httpd_resp_set_status(req, "503 Service Unavailable");
			return httpd_resp_sendstr(req, "too many viewers");
		}
		// Each viewer gets its own task, so the server stays free for the next request
		httpd_req_t *copy = nullptr;
		if (ESP_OK != httpd_req_async_handler_begin(req, &copy))
		{
			return ESP_FAIL;
		}
		streamClients++;
		if (pdPASS != xTaskCreatePinnedToCore(stream_task, "stream", 4096, copy, 5, nullptr, 0))
		{
			streamClients--;
			httpd_req_async_handler_complete(copy);
			return ESP_FAIL;
		}
		return ESP_OK;
	}

	// ------------------------------------------------------------------ control (port 80)

	bool query_flag(httpd_req_t *req, const char *key, bool &value)
	{
		char query[64];
		char text[8];
		if ((ESP_OK == httpd_req_get_url_query_str(req, query, sizeof(query))) &&
		    (ESP_OK == httpd_query_key_value(query, key, text, sizeof(text))))
		{
			value = ('1' == text[0]);
			return true;
		}
		return false;
	}

	esp_err_t send_status(httpd_req_t *req)
	{
		char json[160];
		std::snprintf(json, sizeof(json), "{\"camera\":%d,\"led\":%d,\"mirror\":%d,\"flip\":%d,\"viewers\":%d,\"fps\":%.1f}",
		              cameraReady ? 1 : 0, ledOn ? 1 : 0, mirror ? 1 : 0, flip ? 1 : 0, streamClients.load(), fpsTimes10.load() / 10.0);
		httpd_resp_set_type(req, "application/json");
		httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
		return httpd_resp_sendstr(req, json);
	}

	esp_err_t status_handler(httpd_req_t *req)
	{
		return send_status(req);
	}

	esp_err_t led_handler(httpd_req_t *req)
	{
		bool on = false;
		if (query_flag(req, "on", on))
		{
			ledOn = on;
			apply_led();
			ESP_LOGI(TAG, "LEDs %s", on ? "ON" : "OFF");
		}
		return send_status(req);
	}

	esp_err_t set_handler(httpd_req_t *req)
	{
		bool value = false;
		bool changed = false;
		if (query_flag(req, "mirror", value))
		{
			mirror = value;
			changed = true;
		}
		if (query_flag(req, "flip", value))
		{
			flip = value;
			changed = true;
		}
		if (changed)
		{
			apply_orientation();
			save_settings();
			ESP_LOGI(TAG, "Orientation: mirror %d, flip %d (saved)", mirror ? 1 : 0, flip ? 1 : 0);
		}
		return send_status(req);
	}

	esp_err_t capture_handler(httpd_req_t *req)
	{
		camera_fb_t *frame = cameraReady ? esp_camera_fb_get() : nullptr;
		if (nullptr == frame)
		{
			httpd_resp_set_status(req, "503 Service Unavailable");
			return httpd_resp_sendstr(req, "no picture");
		}
		httpd_resp_set_type(req, "image/jpeg");
		httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=camera.jpg");
		esp_err_t error = httpd_resp_send(req, reinterpret_cast<const char *>(frame->buf), frame->len);
		esp_camera_fb_return(frame);
		return error;
	}

	const char PAGE[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HESAB camera</title>
<style>
body{margin:0;background:#1b1b1b;color:#f2f2f2;font-family:system-ui,sans-serif;text-align:center}
h1{font-size:18px;letter-spacing:3px;margin:14px 0;color:#ffb800}
img{width:100%;max-width:640px;background:#000;display:block;margin:0 auto}
.row{display:flex;gap:10px;justify-content:center;flex-wrap:wrap;margin:14px}
button{background:#262626;color:#f2f2f2;border:2px solid #3a3a3a;border-radius:12px;padding:12px 18px;font-size:16px}
button.on{background:#ffb800;color:#151515;border-color:#ffb800}
#s{color:#9a9a9a;font-size:13px}
</style></head><body>
<h1>HESAB CAMERA</h1>
<img id="v" alt="camera">
<div class="row">
<button id="led" onclick="go('/led?on='+(st.led?0:1))">LEDs</button>
<button id="mirror" onclick="go('/set?mirror='+(st.mirror?0:1))">Mirror</button>
<button id="flip" onclick="go('/set?flip='+(st.flip?0:1))">Flip</button>
</div>
<div id="s"></div>
<script>
var st={};
document.getElementById('v').src='http://'+location.hostname+':81/stream';
function show(j){st=j;['led','mirror','flip'].forEach(function(k){document.getElementById(k).className=j[k]?'on':''});
document.getElementById('s').textContent=(j.camera?'camera OK':'camera NOT found')+' - '+j.fps+' fps - '+j.viewers+' watching';}
function go(u){fetch(u).then(function(r){return r.json()}).then(show)}
go('/status');setInterval(function(){go('/status')},2000);
</script></body></html>)HTML";

	esp_err_t page_handler(httpd_req_t *req)
	{
		httpd_resp_set_type(req, "text/html");
		return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
	}

	void start_web_servers()
	{
		httpd_handle_t control = nullptr;
		httpd_config_t config = HTTPD_DEFAULT_CONFIG();
		config.server_port = 80;
		config.ctrl_port = 32768;
		config.max_open_sockets = 4;
		config.lru_purge_enable = true;
		if (ESP_OK == httpd_start(&control, &config))
		{
			const httpd_uri_t uris[] = {
				{ "/", HTTP_GET, page_handler, nullptr },
				{ "/status", HTTP_GET, status_handler, nullptr },
				{ "/led", HTTP_GET, led_handler, nullptr },
				{ "/set", HTTP_GET, set_handler, nullptr },
				{ "/capture", HTTP_GET, capture_handler, nullptr },
			};
			for (const auto &uri : uris)
			{
				httpd_register_uri_handler(control, &uri);
			}
		}

		httpd_handle_t stream = nullptr;
		httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
		streamConfig.server_port = 81;
		streamConfig.ctrl_port = 32769;
		streamConfig.max_open_sockets = MAX_STREAM_CLIENTS + 2;
		streamConfig.lru_purge_enable = true;
		if (ESP_OK == httpd_start(&stream, &streamConfig))
		{
			httpd_uri_t uri = { "/stream", HTTP_GET, stream_handler, nullptr };
			httpd_register_uri_handler(stream, &uri);
		}
		ESP_LOGI(TAG, "Web: http://192.168.4.1/ (test page), stream on http://192.168.4.1:81/stream");
	}

	// ------------------------------------------------------------------ WiFi access point

	void wifi_event(void *, esp_event_base_t, std::int32_t id, void *data)
	{
		if (WIFI_EVENT_AP_STACONNECTED == id)
		{
			auto *event = static_cast<wifi_event_ap_staconnected_t *>(data);
			ESP_LOGI(TAG, "WiFi: " MACSTR " joined", MAC2STR(event->mac));
		}
		else if (WIFI_EVENT_AP_STADISCONNECTED == id)
		{
			auto *event = static_cast<wifi_event_ap_stadisconnected_t *>(data);
			ESP_LOGI(TAG, "WiFi: " MACSTR " left", MAC2STR(event->mac));
		}
	}

	void start_wifi()
	{
		ESP_ERROR_CHECK(esp_netif_init());
		ESP_ERROR_CHECK(esp_event_loop_create_default());
		esp_netif_create_default_wifi_ap(); // 192.168.4.1, hands out addresses (DHCP)

		wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
		ESP_ERROR_CHECK(esp_wifi_init(&init));
		esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr);

		wifi_config_t config = {};
		std::memcpy(config.ap.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
		config.ap.ssid_len = sizeof(WIFI_SSID) - 1;
		std::memcpy(config.ap.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD) - 1);
		config.ap.channel = WIFI_CHANNEL;
		config.ap.max_connection = 4;
		config.ap.authmode = WIFI_AUTH_WPA2_PSK;

		ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
		ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
		ESP_ERROR_CHECK(esp_wifi_start());
		esp_wifi_set_ps(WIFI_PS_NONE); // no power saving: steadier picture
		ESP_LOGI(TAG, "WiFi network \"%s\" started (password %s, channel %d)", WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
	}
}

extern "C" void app_main()
{
	ESP_LOGI(TAG, "=== Reversing camera node ===");
	esp_err_t nvsError = nvs_flash_init();
	if ((ESP_ERR_NVS_NO_FREE_PAGES == nvsError) || (ESP_ERR_NVS_NEW_VERSION_FOUND == nvsError))
	{
		nvs_flash_erase();
		nvs_flash_init();
	}
	load_settings();

	start_wifi();
	start_web_servers();

	// Start the camera; if it isn't found, keep trying (WiFi and the test page stay up)
	while (!start_camera())
	{
		vTaskDelay(pdMS_TO_TICKS(3000));
		ESP_LOGI(TAG, "Trying the camera again...");
	}
	cameraReady = true;

	std::uint32_t lastFrames = 0;
	std::int64_t lastTime = esp_timer_get_time();
	while (true)
	{
		vTaskDelay(pdMS_TO_TICKS(5000));
		std::int64_t now = esp_timer_get_time();
		std::uint32_t frames = framesSent.load();
		std::uint32_t fps10 = static_cast<std::uint32_t>((frames - lastFrames) * 10000000LL / (now - lastTime));
		fpsTimes10 = fps10;
		lastFrames = frames;
		lastTime = now;
		if (streamClients > 0)
		{
			ESP_LOGI(TAG, "Streaming: %.1f frames/s to %d viewer(s)", fps10 / 10.0, streamClients.load());
		}
	}
}
