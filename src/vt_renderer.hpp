// Draws an ISOBUS object pool (the implement's screens) with LVGL 8.
//
// Platform independent: only depends on LVGL and AgIsoStack++, so it can also be
// built and tested on a PC.
//
// Screen layout on the 800x480 panel:
//   x   0..479  data/alarm mask (480x480, the size we report to implements)
//   x 490..549  soft key column (6 keys of 60x60)
//   x 570..799  status panel
//
// Threading: every method must be called with the LVGL lock held. Touch callbacks
// run inside lv_timer_handler (also under the lock) and only push KeyEvents to the
// handler; they never talk to the CAN stack directly.

#pragma once

#include "isobus/isobus/isobus_virtual_terminal_base.hpp"
#include "isobus/isobus/isobus_virtual_terminal_working_set_base.hpp"
#include "lvgl.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class VtRenderer
{
public:
	static constexpr lv_coord_t MASK_SIZE = 480;
	static constexpr lv_coord_t KEY_X = 490;
	static constexpr lv_coord_t KEY_SIZE = 60;
	static constexpr lv_coord_t KEY_PITCH = 80;
	static constexpr std::uint8_t NUMBER_OF_KEYS = 6;
	static constexpr lv_coord_t STATUS_X = 570;

	struct KeyEvent
	{
		bool isSoftKey;
		isobus::VirtualTerminalBase::KeyActivationCode code;
		std::uint16_t objectId;
		std::uint16_t parentMaskId;
		std::uint8_t keyCode;
	};
	using KeyEventHandler = std::function<void(const KeyEvent &)>;

	VtRenderer(lv_obj_t *screen, KeyEventHandler handler);

	// Rebuild everything for this working set and mask. Pass nullptr to show the waiting screen.
	void render(std::shared_ptr<isobus::VirtualTerminalWorkingSetBase> workingSet, std::uint16_t activeMaskId);

	// Update only the texts of numbers and strings (cheap, safe while a key is held)
	void refresh_values();

	// Text in the middle of the mask area when nothing is shown (e.g. "Waiting for implement")
	void set_waiting_text(const std::string &text);

	// Lines in the status panel on the right
	void set_status(const std::string &text);

	// True while a button or soft key is held down. Full rebuilds wait until it is released,
	// so the implement always gets the release message.
	bool is_key_held() const { return heldKey != nullptr; }

	// Picture data is decoded once per working set; call when a new pool arrives
	void clear_image_cache() { imageCache.clear(); }

private:
	struct KeyInfo
	{
		VtRenderer *owner;
		KeyEvent event;
		bool aborted;
	};

	struct TrackedText
	{
		lv_obj_t *label;
		std::shared_ptr<isobus::VTObject> object;
	};

	struct CachedImage
	{
		std::vector<std::uint8_t> pixels;
		lv_img_dsc_t descriptor;
	};

	lv_obj_t *make_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
	lv_color_t vt_colour(std::uint8_t index) const;
	const lv_font_t *font_for(std::shared_ptr<isobus::VTObject> fontAttributes) const;

	void render_mask(std::shared_ptr<isobus::VTObject> mask);
	void render_soft_keys(std::shared_ptr<isobus::VTObject> mask);
	void render_children(lv_obj_t *parent, std::shared_ptr<isobus::VTObject> object, int depth);
	void render_object(lv_obj_t *parent, std::shared_ptr<isobus::VTObject> object, lv_coord_t x, lv_coord_t y, int depth);

	void render_rectangle(lv_obj_t *parent, std::shared_ptr<isobus::VTObject> object, lv_coord_t x, lv_coord_t y);
	void render_ellipse(lv_obj_t *parent, std::shared_ptr<isobus::VTObject> object, lv_coord_t x, lv_coord_t y);
	void render_text(lv_obj_t *parent, std::shared_ptr<isobus::VTObject> object, lv_coord_t x, lv_coord_t y);
	void render_button(lv_obj_t *parent, std::shared_ptr<isobus::VTObject> object, lv_coord_t x, lv_coord_t y, int depth);
	void render_picture(lv_obj_t *parent, std::shared_ptr<isobus::VTObject> object, lv_coord_t x, lv_coord_t y);

	std::string text_for(std::shared_ptr<isobus::VTObject> object) const;
	const lv_img_dsc_t *image_for(std::shared_ptr<isobus::VTObject> picture);
	void make_pressable(lv_obj_t *box, bool isSoftKey, std::uint16_t objectId, std::uint8_t keyCode);
	static void key_event_callback(lv_event_t *e);

	lv_obj_t *screen;
	lv_obj_t *maskArea;
	lv_obj_t *keyArea;
	lv_obj_t *waitingLabel;
	lv_obj_t *statusLabel;

	KeyEventHandler handler;
	std::shared_ptr<isobus::VirtualTerminalWorkingSetBase> workingSet;
	std::uint16_t activeMaskId = 0xFFFF;

	std::vector<std::unique_ptr<KeyInfo>> keyInfos;
	std::vector<TrackedText> trackedTexts;
	std::map<std::uint16_t, std::unique_ptr<CachedImage>> imageCache;
	KeyInfo *heldKey = nullptr;
};
