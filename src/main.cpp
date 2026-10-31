// Waveshare ESP32-S3-Touch-LCD-7 — ISOBUS Virtual Terminal, step 1: protocol only
//
// The display claims an address as a Virtual Terminal (preferred 0x26), answers the
// implement's questions, receives its object pool over the transport protocol and
// parses it. Nothing is drawn yet: we print what arrived to the serial log.
//
// Modelled on AgIsoVirtualTerminal (Open-Agriculture), which uses the same
// isobus::VirtualTerminalServer class from AgIsoStack++.

#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/twai_plugin.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server.hpp"

#include "console_logger.cpp"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/twai.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <array>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

static const char *TAG = "vt";

// ---------------- Board: CH422G IO expander must switch the CAN pins from USB to CAN ----------------
#define I2C_PORT          I2C_NUM_0
#define I2C_SDA           GPIO_NUM_8
#define I2C_SCL           GPIO_NUM_9
#define CH422G_MODE_ADDR  0x24
#define CH422G_OUT_ADDR   0x38
#define EXIO_TP_RST  (1 << 1)
#define EXIO_LCD_BL  (1 << 2)
#define EXIO_SD_CS   (1 << 4)
#define EXIO_CAN_SEL (1 << 5)   // HIGH = CAN mode

static bool ch422g_write(std::uint8_t addr, std::uint8_t value)
{
	return ESP_OK == i2c_master_write_to_device(I2C_PORT, addr, &value, 1, pdMS_TO_TICKS(100));
}

static void enable_can_mode()
{
	i2c_config_t conf = {};
	conf.mode = I2C_MODE_MASTER;
	conf.sda_io_num = I2C_SDA;
	conf.scl_io_num = I2C_SCL;
	conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
	conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
	conf.master.clk_speed = 100000;
	i2c_param_config(I2C_PORT, &conf);
	i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);

	bool ok1 = ch422g_write(CH422G_MODE_ADDR, 0x01);
	bool ok2 = ch422g_write(CH422G_OUT_ADDR, EXIO_TP_RST | EXIO_LCD_BL | EXIO_SD_CS | EXIO_CAN_SEL);
	ESP_LOGI(TAG, "CH422G: mode %s, outputs %s -> CAN mode selected", ok1 ? "OK" : "FAIL", ok2 ? "OK" : "FAIL");
}

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
	}

private:
	std::mutex storeMutex;
	// NAME -> (version label -> object pool)
	std::map<std::uint64_t, std::map<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> store;
};

static twai_general_config_t twaiConfig = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_20, GPIO_NUM_19, TWAI_MODE_NORMAL);
static twai_timing_config_t twaiTiming = TWAI_TIMING_CONFIG_250KBITS();
static twai_filter_config_t twaiFilter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

extern "C" void app_main()
{
	ESP_LOGI(TAG, "=== Display as ISOBUS Virtual Terminal (step 1: protocol only) ===");
	ESP_LOGI(TAG, "PSRAM: %u KB free", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

	enable_can_mode();
	vTaskDelay(pdMS_TO_TICKS(50));

	// ---- CAN via the ESP32's built-in controller (TWAI), 250 kbit/s ----
	twaiConfig.rx_queue_len = 64; // object pool transfers arrive in fast bursts
	twaiConfig.tx_queue_len = 16;
	auto canDriver = std::make_shared<isobus::TWAIPlugin>(&twaiConfig, &twaiTiming, &twaiFilter);

	isobus::CANStackLogger::set_can_stack_logger_sink(&logger);
	isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Info);
	isobus::CANHardwareInterface::set_number_of_can_channels(1);
	isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, canDriver);

	if (!isobus::CANHardwareInterface::start() || !canDriver->get_is_valid())
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

	vt->get_on_change_active_mask_event_dispatcher().add_listener(
	  [](std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>, std::uint16_t workingSetId, std::uint16_t maskId) {
		  ESP_LOGI(TAG, "Implement switched mask: working set %u -> mask %u", workingSetId, maskId);
	  });

	std::uint32_t lastStatus = 0;
	while (true)
	{
		vt->tick();

		std::uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
		if (now - lastStatus >= 3000)
		{
			lastStatus = now;
			vt->print_status();
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}