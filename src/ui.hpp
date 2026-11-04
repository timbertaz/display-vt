// The cab display's own UI: a tab rail on the left (Lights / ISOBUS / Camera) and
// one page per tab. The ISOBUS page is an empty container that VtRenderer draws into.
//
// Like VtRenderer, every method must be called with the LVGL lock held.
#pragma once

#include "lvgl.h"

#include <array>
#include <cstdint>
#include <functional>

namespace ui
{
	// JCB-style palette: deep yellow on charcoal
	namespace colour
	{
		constexpr std::uint32_t YELLOW = 0xFFB800;
		constexpr std::uint32_t AMBER = 0xFF7A00;
		constexpr std::uint32_t BACKGROUND = 0x1B1B1B;
		constexpr std::uint32_t PANEL = 0x262626;
		constexpr std::uint32_t RAIL = 0x111111;
		constexpr std::uint32_t BORDER = 0x3A3A3A;
		constexpr std::uint32_t TEXT = 0xF2F2F2;
		constexpr std::uint32_t TEXT_DIM = 0x9A9A9A;
		constexpr std::uint32_t TEXT_ON_YELLOW = 0x151515;
		constexpr std::uint32_t GREEN = 0x3DD68C;
		constexpr std::uint32_t RED = 0xFF4D4D;
	}

	enum class Tab : std::uint8_t
	{
		Lights,
		Isobus,
		Camera,
		Count
	};

	enum class Light : std::uint8_t
	{
		Front,
		Rear,
		Beacon,
		Count
	};

	enum class IsobusState : std::uint8_t
	{
		NoImplement, // grey dot
		Connected,   // green dot
		Alarm        // red dot
	};

	enum class CameraState : std::uint8_t
	{
		NoWifi,     // camera network not found
		Connecting, // on the network, no picture yet
		Streaming
	};

	class Ui
	{
	public:
		static constexpr lv_coord_t RAIL_WIDTH = 96;
		static constexpr lv_coord_t PAGE_WIDTH = 800 - RAIL_WIDTH;
		static constexpr lv_coord_t PAGE_HEIGHT = 480;

		// Called when the operator taps a light. The UI does not change the light by itself:
		// call set_light() with the real state (e.g. reported back by the lights module).
		using LightRequestHandler = std::function<void(Light light, bool wantOn)>;

		Ui(lv_obj_t *screen, LightRequestHandler onLightRequest);

		lv_obj_t *isobus_page() const { return pages[static_cast<int>(Tab::Isobus)]; }

		void show_tab(Tab tab);
		Tab current_tab() const { return currentTab; }

		void set_light(Light light, bool on);

		// Shows "Lights module offline" next to the title when the relay board doesn't answer
		void set_lights_online(bool online);
		bool get_light(Light light) const { return lightOn[static_cast<int>(light)]; }

		void set_isobus_state(IsobusState state);
		void set_can_status(bool busOk, const char *text);

		// Speed in the tab rail. gpsConnected = NMEA data arriving, fix = receiver has a position.
		void set_speed(bool gpsConnected, bool fix, float speedKmh, int satellites);

		// Camera page. The picture itself is drawn straight to the screen (board::draw_pixels) into
		// this area; LVGL only draws the black box, the placeholder and the side buttons.
		static constexpr int VIDEO_X = RAIL_WIDTH; // screen coordinates
		static constexpr int VIDEO_Y = 0;
		static constexpr int VIDEO_WIDTH = 640;
		static constexpr int VIDEO_HEIGHT = 480;
		bool camera_visible() const { return Tab::Camera == currentTab; }
		bool guides_shown() const { return guidesShown; }
		void camera_frame_shown(); // first picture: hides the placeholder
		static void draw_guides(std::uint16_t *pixels, int width, int height); // reversing lines into a picture
		void set_camera_state(CameraState state, float fps);
		void set_camera_led(bool on); // what the camera reports
		void set_camera_led_handler(std::function<void(bool wantOn)> handler) { onCameraLed = std::move(handler); }

	private:
		struct Target
		{
			Ui *owner;
			int index;
		};

		void build_rail();
		void build_lights_page(lv_obj_t *page);
		void build_camera_page(lv_obj_t *page);
		lv_obj_t *make_light_card(lv_obj_t *parent, Light light, const lv_img_dsc_t *icon, const char *title, lv_coord_t x, lv_coord_t y);
		void make_hotspot(lv_obj_t *parent, Light light, lv_coord_t centreX, lv_coord_t centreY);
		void style_tab(int index, bool selected);
		void request_toggle(Light light);
		void update_lights_summary();

		static void tab_clicked(lv_event_t *e);
		static void light_clicked(lv_event_t *e);
		static void all_off_clicked(lv_event_t *e);
		static void beacon_anim(void *object, int32_t value);
		static void camera_led_clicked(lv_event_t *e);
		static void guide_lines_clicked(lv_event_t *e);
		lv_obj_t *make_side_button(lv_obj_t *parent, lv_coord_t y, const char *label, lv_event_cb_t callback);
		void style_side_button(lv_obj_t *button, bool on);

		LightRequestHandler onLightRequest;
		Tab currentTab = Tab::Lights;

		lv_obj_t *screen;
		std::array<lv_obj_t *, 3> pages{};
		std::array<lv_obj_t *, 3> tabButtons{};
		std::array<lv_obj_t *, 3> tabIcons{};
		std::array<lv_obj_t *, 3> tabLabels{};
		lv_obj_t *isobusDot = nullptr;
		lv_obj_t *canDot = nullptr;
		lv_obj_t *canLabel = nullptr;
		lv_obj_t *speedValue = nullptr;
		lv_obj_t *speedUnit = nullptr;
		lv_obj_t *gpsDot = nullptr;
		lv_obj_t *gpsLabel = nullptr;

		// Lights page
		std::array<bool, 3> lightOn{};
		std::array<lv_obj_t *, 3> cards{};
		std::array<lv_obj_t *, 3> cardIcons{};
		std::array<lv_obj_t *, 3> cardTitles{};
		std::array<lv_obj_t *, 3> cardStates{};
		lv_obj_t *beamFront = nullptr;
		lv_obj_t *beamRear = nullptr;
		lv_obj_t *beaconGlow = nullptr;
		lv_obj_t *lightsSummary = nullptr;
		bool lightsOnline = true;

		// Camera page
		std::function<void(bool)> onCameraLed;
		lv_obj_t *videoPlaceholder = nullptr;
		lv_obj_t *placeholderTitle = nullptr;
		lv_obj_t *placeholderText = nullptr;
		lv_obj_t *cameraChipDot = nullptr;
		lv_obj_t *cameraChipText = nullptr;
		lv_obj_t *cameraFpsText = nullptr;
		lv_obj_t *ledButton = nullptr;
		lv_obj_t *linesButton = nullptr;
		bool cameraLedOn = false;
		bool guidesShown = true;
		bool haveFrame = false;

		std::array<Target, 3> tabTargets{};
		std::array<Target, 3> lightTargets{};
	};
}
