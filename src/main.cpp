// Waveshare ESP32-S3-Touch-LCD-7 — ISOBUS Virtual Terminal
//
// The display claims an address as a Virtual Terminal (preferred 0x26), answers the
// implement's questions, receives its object pool, and draws the active mask and soft
// keys with LVGL (vt_renderer.cpp). Touching a button or soft key sends the matching
// ISOBUS activation messages back to the implement.
//
// The screen has three tabs (ui.cpp): Lights, ISOBUS (the implement's screens) and Camera.
// The lights are switched by a Modbus relay board on the RS485 port (relay_board.cpp):
//   relay 1 = front work lights, relay 2 = rear work lights, relay 3 = beacon
//
// Threading: the CAN stack, the VT server and all drawing decisions run in this main
// task, so the object pool is never changed while it is being drawn. LVGL itself runs
// in its own task (board.cpp); we take its lock whenever we touch LVGL objects.
//
// Modelled on AgIsoVirtualTerminal (Open-Agriculture), which uses the same
// isobus::VirtualTerminalServer class from AgIsoStack++.

#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/twai_plugin.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server.hpp"

#include "board.hpp"
#include "relay_board.hpp"
#include "console_logger.cpp"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.hpp"
#include "vt_renderer.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

static const char *TAG = "vt";

// ---------------- Helpers for readable logs ----------------
static const char *object_type_name(std::uint8_t t)
{
	static const char *names[] = {
		"WorkingSet", "DataMask", "AlarmMask", "Container", "SoftKeyMask", "Key", "Button",
		"InputBoolean", "InputString", "InputNumber", "InputList", "OutputString", "OutputNumber",
		"OutputLine", "OutputRectangle", "OutputEllipse", "OutputPolygon", "OutputMeter",
		"OutputLinearBarGraph", "OutputArchedBarGraph", "PictureGraphic", "NumberVariable",
		"StringVariable", "FontAttributes", "LineAttributes", "FillAttributes", "InputAttributes",
		"ObjectPointer", "Macro", "AuxFunction", "AuxInput", "AuxFunction2", "AuxInput2",
		"AuxControlDesignator", "WindowMask", "KeyGroup", "GraphicsContext", "OutputList",
		"ExtendedInputAttributes", "ColourMap", "ObjectLabelReferenceList"
	};
	return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "Other";
}

static std::string label_to_string(const std::vector<std::uint8_t> &label)
{
	std::string s;
	for (auto c : label)
	{
		s += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
	}
	return s;
}

// ---------------- The Virtual Terminal ----------------
class BenchVT : public isobus::VirtualTerminalServer
{
public:
	explicit BenchVT(std::shared_ptr<isobus::InternalControlFunction> cf) :
	  VirtualTerminalServer(cf) {}

	// update() is protected in the base class; call this cyclically
	void tick() { update(); }

	std::uint16_t active_mask_id() const { return activeWorkingSetDataMaskObjectID; }

	// True for a moment after a new pool was parsed, so the screen can be rebuilt
	std::atomic<bool> poolChanged{ false };

	// Text for the status panel on the right of the screen
	std::string status_text() const
	{
		char text[200];
		auto cf = get_internal_control_function();
		auto active = get_active_working_set();
		int len = std::snprintf(text, sizeof(text), "ISOBUS VT\n\nAddress: %s\n",
		                        cf->get_address_valid() ? "" : "claiming...");
		if (cf->get_address_valid())
		{
			len = std::snprintf(text, sizeof(text), "ISOBUS VT\n\nAddress: 0x%02X\n", cf->get_address());
		}

		if (nullptr != active)
		{
			std::snprintf(text + len, sizeof(text) - len, "Implement: 0x%02X\nMask: %u",
			              active->get_control_function()->get_address(), static_cast<unsigned>(activeWorkingSetDataMaskObjectID));
		}
		else if (!managedWorkingSetList.empty())
		{
			std::snprintf(text + len, sizeof(text) - len, "Implement found,\nloading screens...");
		}
		else
		{
			std::snprintf(text + len, sizeof(text) - len, "No implement");
		}
		return text;
	}

	void print_status()
	{
		auto cf = get_internal_control_function();
		auto active = get_active_working_set();
		ESP_LOGI(TAG, "address %s 0x%02X | implements: %u | active: %s | data mask: %u | free heap: %u KB",
		         cf->get_address_valid() ? "claimed" : "not yet",
		         cf->get_address(),
		         static_cast<unsigned>(managedWorkingSetList.size()),
		         active ? "yes" : "none",
		         static_cast<unsigned>(activeWorkingSetDataMaskObjectID),
		         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
	}

	// ---- What this terminal is (reported to implements) ----
	VTVersion get_version() const override { return VTVersion::Version5; } // same as AgIsoVirtualTerminal's default

	// The data mask area is square; the 800x480 panel leaves 320 px on the right for soft keys
	std::uint16_t get_data_mask_area_size_x_pixels() const override { return 480; }
	std::uint16_t get_data_mask_area_size_y_pixels() const override { return 480; }

	std::uint8_t get_number_of_navigation_soft_keys() const override { return 0; }
	std::uint8_t get_soft_key_descriptor_x_pixel_width() const override { return 60; }
	std::uint8_t get_soft_key_descriptor_y_pixel_height() const override { return 60; }
	std::uint8_t get_number_of_possible_virtual_soft_keys_in_soft_key_mask() const override { return 64; }
	std::uint8_t get_number_of_physical_soft_keys() const override { return 6; } // VT v4+ requires at least 6

	bool get_is_enough_memory(std::uint32_t requestedMemory) const override
	{
		// Raw pool + parsed objects: leave a generous margin
		std::size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
		bool enough = freeBytes > (static_cast<std::size_t>(requestedMemory) * 3);
		ESP_LOGI(TAG, "Implement asks for %lu bytes, free %u -> %s",
		         static_cast<unsigned long>(requestedMemory), static_cast<unsigned>(freeBytes), enough ? "OK" : "NOT ENOUGH");
		return enough;
	}

	SupportedWideCharsErrorCode get_supported_wide_chars(std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t &, std::vector<std::uint8_t> &) override
	{
		return SupportedWideCharsErrorCode::AnyOtherError;
	}

	std::vector<std::uint8_t> get_supported_objects() const override
	{
		// Same list as AgIsoVirtualTerminal (ISO 11783-6 Table A.1)
		return { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 37, 39 };
	}

	// ---- Stored versions: kept in RAM only (lost at power-off; real flash storage comes later) ----
	std::vector<std::array<std::uint8_t, 7>> get_versions(isobus::NAME clientNAME) override
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		std::vector<std::array<std::uint8_t, 7>> out;
		for (const auto &entry : store[clientNAME.get_full_name()])
		{
			if (entry.first.size() == 7)
			{
				std::array<std::uint8_t, 7> a;
				std::copy(entry.first.begin(), entry.first.end(), a.begin());
				out.push_back(a);
			}
		}
		ESP_LOGI(TAG, "Implement asked for stored versions: %u found", static_cast<unsigned>(out.size()));
		return out;
	}

	std::vector<std::array<std::uint8_t, 32>> get_extended_versions(isobus::NAME clientNAME) override
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		std::vector<std::array<std::uint8_t, 32>> out;
		for (const auto &entry : store[clientNAME.get_full_name()])
		{
			if (entry.first.size() == 32)
			{
				std::array<std::uint8_t, 32> a;
				std::copy(entry.first.begin(), entry.first.end(), a.begin());
				out.push_back(a);
			}
		}
		return out;
	}

	std::vector<std::uint8_t> load_version(const std::vector<std::uint8_t> &versionLabel, isobus::NAME clientNAME) override
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		auto &versions = store[clientNAME.get_full_name()];
		auto it = versions.find(versionLabel);
		ESP_LOGI(TAG, "Load version '%s': %s", label_to_string(versionLabel).c_str(), (it != versions.end()) ? "found" : "not stored");
		return (it != versions.end()) ? it->second : std::vector<std::uint8_t>();
	}

	bool save_version(const std::vector<std::uint8_t> &objectPool, const std::vector<std::uint8_t> &versionLabel, isobus::NAME clientNAME) override
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		store[clientNAME.get_full_name()][versionLabel] = objectPool;
		ESP_LOGI(TAG, "Stored version '%s' (%u bytes, RAM only)", label_to_string(versionLabel).c_str(), static_cast<unsigned>(objectPool.size()));
		return true;
	}

	bool delete_version(const std::vector<std::uint8_t> &versionLabel, isobus::NAME clientNAME) override
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		return store[clientNAME.get_full_name()].erase(versionLabel) > 0;
	}

	bool delete_all_versions(std::uint8_t versionLabelLength, isobus::NAME clientNAME) override
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		auto &versions = store[clientNAME.get_full_name()];
		for (auto it = versions.begin(); it != versions.end();)
		{
			it = (it->first.size() == versionLabelLength) ? versions.erase(it) : std::next(it);
		}
		return true;
	}

	bool delete_object_pool(isobus::NAME) override { return true; }

	// ---- Events ----
	void suspend_working_set(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>) override
	{
		ESP_LOGW(TAG, "Working set suspended");
	}

	void handle_unexpected_working_set_shutdown(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws, UnexpectedShutdownReason reason) override
	{
		ESP_LOGW(TAG, "Implement at 0x%02X went away (%s)",
		         ws->get_control_function()->get_address(),
		         (reason == UnexpectedShutdownReason::MaintenanceTimeout) ? "no maintenance message for 3 s" : "it restarted");
	}

	// Implement sent ESC: close any open input field. We don't draw input fields yet, so nothing is ever open.
	std::uint16_t abort_input(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>) override
	{
		return isobus::NULL_OBJECT_ID;
	}

	void transferred_object_pool_parse_start(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &ws) const override
	{
		std::size_t total = 0;
		for (std::size_t i = 0; i < ws->get_number_iop_files(); i++)
		{
			total += ws->get_iop_raw_data(i).size();
		}
		ESP_LOGI(TAG, "Object pool fully received from 0x%02X: %u bytes. Parsing...",
		         ws->get_control_function()->get_address(), static_cast<unsigned>(total));
	}

	void object_pool_parse_finished(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws, bool success) override
	{
		if (!success)
		{
			ESP_LOGE(TAG, "Object pool from 0x%02X FAILED to parse", ws->get_control_function()->get_address());
			return;
		}

		const auto &tree = ws->get_object_tree();
		std::map<std::uint8_t, unsigned> counts;
		for (const auto &obj : tree)
		{
			counts[static_cast<std::uint8_t>(obj.second->get_object_type())]++;
		}

		ESP_LOGI(TAG, "Object pool parsed OK: %u objects", static_cast<unsigned>(tree.size()));
		for (const auto &c : counts)
		{
			ESP_LOGI(TAG, "   %2u x %s", c.second, object_type_name(c.first));
		}
		poolChanged = true;
	}

private:
	std::mutex storeMutex;
	// NAME -> (version label -> object pool)
	std::map<std::uint64_t, std::map<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> store;
};

static twai_general_config_t twaiConfig = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_20, GPIO_NUM_19, TWAI_MODE_NORMAL);
static twai_timing_config_t twaiTiming = TWAI_TIMING_CONFIG_250KBITS();
static twai_filter_config_t twaiFilter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

// Touches are queued by the LVGL task and sent from the main task, which owns the CAN stack
static std::mutex keyEventMutex;
static std::deque<VtRenderer::KeyEvent> keyEvents;

// Set by the VT server's events (they run in the main task, from CANHardwareInterface::update)
static std::atomic<bool> repaintNeeded{ false };
static std::atomic<bool> rebuildNeeded{ false };

static void send_key_event(BenchVT &vt, const VtRenderer::KeyEvent &event)
{
	auto ws = vt.get_active_working_set();
	if (nullptr == ws)
	{
		return;
	}
	auto destination = ws->get_control_function();
	bool sent = event.isSoftKey ?
	  vt.send_soft_key_activation_message(event.code, event.objectId, event.parentMaskId, event.keyCode, destination) :
	  vt.send_button_activation_message(event.code, event.objectId, event.parentMaskId, event.keyCode, destination);

	static const char *codeNames[] = { "released", "pressed", "held", "aborted" };
	ESP_LOGI(TAG, "%s %u %s%s", event.isSoftKey ? "Soft key" : "Button", event.objectId,
	         codeNames[static_cast<int>(event.code) & 3], sent ? "" : " (SEND FAILED)");
}

static relay::RelayBoard relays;

// Which relay on the board drives which light
static std::uint8_t relay_for(ui::Light light)
{
	return static_cast<std::uint8_t>(light) + 1; // Front = relay 1, Rear = relay 2, Beacon = relay 3
}

// Bus health for the dot at the bottom of the tab rail
static void update_can_status(ui::Ui &screenUi)
{
	twai_status_info_t status;
	if (ESP_OK != twai_get_status_info(&status))
	{
		screenUi.set_can_status(false, "CAN ?");
	}
	else if (TWAI_STATE_BUS_OFF == status.state)
	{
		screenUi.set_can_status(false, "BUS OFF");
	}
	else if ((status.tx_error_counter > 96) || (status.rx_error_counter > 96))
	{
		screenUi.set_can_status(false, "CAN ERR");
	}
	else
	{
		screenUi.set_can_status(true, "CAN OK");
	}
}

extern "C" void app_main()
{
	ESP_LOGI(TAG, "=== Display as ISOBUS Virtual Terminal ===");
	ESP_LOGI(TAG, "PSRAM: %u KB free", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

	// Screen, touch, backlight and CAN-mode switch
	if (!board::init())
	{
		ESP_LOGE(TAG, "Display init FAILED - continuing without screen");
	}

	relays.start(); // RS485 relay board for the lights

	std::unique_ptr<ui::Ui> screenUi;
	std::unique_ptr<VtRenderer> renderer;
	if (board::lvgl_lock())
	{
		// Light switches: ask the relay board. The cards change when the board reports the
		// new state back (see the screen section in the loop), not when the button is pressed.
		screenUi.reset(new ui::Ui(lv_scr_act(), [](ui::Light light, bool wantOn) {
			static const char *names[] = { "Front lights", "Rear lights", "Beacon" };
			ESP_LOGI(TAG, "%s -> %s requested", names[static_cast<int>(light)], wantOn ? "ON" : "OFF");
			relays.request(relay_for(light), wantOn);
		}));
		screenUi->set_lights_online(false);

		renderer.reset(new VtRenderer(screenUi->isobus_page(), [](const VtRenderer::KeyEvent &event) {
			std::lock_guard<std::mutex> lock(keyEventMutex);
			keyEvents.push_back(event);
		}));
		renderer->set_status("ISOBUS VT\n\nStarting...");
		renderer->render(nullptr, isobus::NULL_OBJECT_ID);
		board::lvgl_unlock();
	}

	// ---- CAN via the ESP32's built-in controller (TWAI), 250 kbit/s ----
	twaiConfig.rx_queue_len = 64; // object pool transfers arrive in fast bursts
	twaiConfig.tx_queue_len = 16;
	auto canDriver = std::make_shared<isobus::TWAIPlugin>(&twaiConfig, &twaiTiming, &twaiFilter);

	isobus::CANStackLogger::set_can_stack_logger_sink(&logger);
	isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Info);
	// Room for ~400 frames: drawing can pause the CAN update for a few tens of ms during a pool transfer
	isobus::CANHardwareInterface::set_number_of_can_channels(1, 400);
	isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, canDriver);

	// start(false): no separate stack thread. We call CANHardwareInterface::update() ourselves
	// below, so every VT server callback runs in this task, never while we draw.
	if (!isobus::CANHardwareInterface::start(false) || !canDriver->get_is_valid())
	{
		ESP_LOGE(TAG, "CAN start FAILED");
	}

	// ---- Our identity: a Virtual Terminal ----
	isobus::NAME vtName(0);
	vtName.set_arbitrary_address_capable(true);
	vtName.set_industry_group(2);
	vtName.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::VirtualTerminal));
	vtName.set_function_instance(0); // VT number 1
	vtName.set_identity_number(43);  // different from the implement (42)
	vtName.set_manufacturer_code(1407);

	auto vtCF = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(vtName, 0, 0x26); // 0x26 = usual VT address

	auto vt = std::make_shared<BenchVT>(vtCF);
	vt->get_language_command_interface().set_language_code("en");
	vt->get_language_command_interface().set_country_code("SE");
	vt->get_language_command_interface().initialize();
	vt->initialize();

	vt->get_on_repaint_event_dispatcher().add_listener(
	  [](std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>) { repaintNeeded = true; });
	vt->get_on_change_active_mask_event_dispatcher().add_listener(
	  [](std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>, std::uint16_t workingSetId, std::uint16_t maskId) {
		  ESP_LOGI(TAG, "Implement switched mask: working set %u -> mask %u", workingSetId, maskId);
		  rebuildNeeded = true;
	  });
	vt->get_on_change_active_softkey_mask_event_dispatcher().add_listener(
	  [](std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>, std::uint16_t, std::uint16_t) { rebuildNeeded = true; });

	std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> shownWorkingSet;
	std::uint16_t shownMask = isobus::NULL_OBJECT_ID;
	std::uint32_t lastStatus = 0;
	std::uint32_t lastLog = 0;
	std::uint32_t lastServerUpdate = 0;
	std::uint32_t lastRebuild = 0;
	bool alarmWasShown = false;
	int shownRelayStates = -1; // -1 = nothing shown yet
	bool shownRelaysOnline = false;
	ui::IsobusState shownIsobusState = ui::IsobusState::NoImplement;
	bool lazyRebuild = false; // a repaint may also have changed colours/positions; catch up within 1 s

	while (true)
	{
		// 1. CAN: receive, run the stack (VT server callbacks happen here), transmit
		isobus::CANHardwareInterface::update();

		std::uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
		if (now - lastServerUpdate >= 20)
		{
			lastServerUpdate = now;
			vt->tick();
		}

		// 2. Touches -> ISOBUS button / soft key messages
		std::deque<VtRenderer::KeyEvent> pending;
		{
			std::lock_guard<std::mutex> lock(keyEventMutex);
			pending.swap(keyEvents);
		}
		for (const auto &event : pending)
		{
			send_key_event(*vt, event);
		}

		// 3. Screen
		// Only wait briefly for LVGL: if it is busy drawing, skip the screen this round and keep CAN running
		if ((nullptr != renderer) && board::lvgl_lock(5))
		{
			auto active = vt->get_active_working_set();
			std::uint16_t mask = vt->active_mask_id();
			bool poolChanged = vt->poolChanged.exchange(false);

			if (poolChanged)
			{
				renderer->clear_image_cache(); // pictures may have changed
			}

			if (repaintNeeded.exchange(false))
			{
				// The implement changed something. Numbers and strings update right away (cheap);
				// anything else (colours, hidden containers, positions) is caught by a rebuild within 1 s.
				renderer->refresh_values();
				lazyRebuild = true;
			}

			bool structureChanged = (active != shownWorkingSet) || (mask != shownMask) || poolChanged || rebuildNeeded ||
			  (lazyRebuild && (now - lastRebuild >= 1000));

			if (structureChanged && !renderer->is_key_held())
			{
				// Full rebuild; waits while a key is held so the implement always gets the release
				rebuildNeeded = false;
				lazyRebuild = false;
				lastRebuild = now;
				shownWorkingSet = active;
				shownMask = mask;
				renderer->render(active, mask);
				if (nullptr == active)
				{
					renderer->set_waiting_text(vt->status_text().find("loading") != std::string::npos ? "Loading object pool..." : "Waiting for implement...");
				}
			}

			// Lights: show what the relay board reports
			int relayStates = relays.states();
			bool relaysOnline = relays.online();
			if ((relayStates != shownRelayStates) || (relaysOnline != shownRelaysOnline))
			{
				shownRelayStates = relayStates;
				shownRelaysOnline = relaysOnline;
				for (int i = 0; i < static_cast<int>(ui::Light::Count); i++)
				{
					auto light = static_cast<ui::Light>(i);
					screenUi->set_light(light, relaysOnline && (0 != (relayStates & (1 << (relay_for(light) - 1)))));
				}
				screenUi->set_lights_online(relaysOnline);
			}

			// Tab rail: implement status dot. An alarm mask brings the ISOBUS tab to the front.
			bool alarmShown = false;
			if (nullptr != active)
			{
				auto maskObject = active->get_object_by_id(mask);
				alarmShown = (nullptr != maskObject) && (isobus::VirtualTerminalObjectType::AlarmMask == maskObject->get_object_type());
			}
			if (alarmShown && !alarmWasShown)
			{
				screenUi->show_tab(ui::Tab::Isobus);
			}
			alarmWasShown = alarmShown;
			ui::IsobusState isobusState = alarmShown ? ui::IsobusState::Alarm : ((nullptr != active) ? ui::IsobusState::Connected : ui::IsobusState::NoImplement);
			if (isobusState != shownIsobusState) // only on change: every style change makes LVGL redraw
			{
				shownIsobusState = isobusState;
				screenUi->set_isobus_state(isobusState);
			}

			if (now - lastStatus >= 1000)
			{
				lastStatus = now;
				update_can_status(*screenUi);
				renderer->set_status(vt->status_text());
				if (nullptr == active)
				{
					renderer->set_waiting_text(vt->status_text().find("loading") != std::string::npos ? "Loading object pool..." : "Waiting for implement...");
				}
			}
			board::lvgl_unlock();
		}

		if (now - lastLog >= 3000)
		{
			lastLog = now;
			vt->print_status();
		}
		vTaskDelay(1);
	}
}
