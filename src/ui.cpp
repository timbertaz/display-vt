#include "ui.hpp"

#include "assets/assets.h"

#include <algorithm>
#include <cstdlib>

namespace ui
{
	namespace
	{
		// Where the lamps are on the tractor drawing (page coordinates, see build_lights_page)
		constexpr lv_coord_t TRACTOR_X = 96;
		constexpr lv_coord_t TRACTOR_Y = 58;
		constexpr lv_coord_t FRONT_LAMP_X = TRACTOR_X + 300;
		constexpr lv_coord_t FRONT_LAMP_Y = TRACTOR_Y + 22;
		constexpr lv_coord_t REAR_LAMP_X = TRACTOR_X + 166;
		constexpr lv_coord_t REAR_LAMP_Y = TRACTOR_Y + 22;
		constexpr lv_coord_t BEACON_X = TRACTOR_X + 184;
		constexpr lv_coord_t BEACON_Y = TRACTOR_Y + 5;

		const char *const LIGHT_TITLES[] = { "Front lights", "Rear lights", "Beacon" };

		lv_obj_t *plain_box(lv_obj_t *parent)
		{
			lv_obj_t *box = lv_obj_create(parent);
			lv_obj_remove_style_all(box);
			lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
			return box;
		}

		lv_obj_t *icon(lv_obj_t *parent, const lv_img_dsc_t *image, std::uint32_t colour)
		{
			lv_obj_t *img = lv_img_create(parent);
			lv_img_set_src(img, image);
			lv_obj_set_style_img_recolor(img, lv_color_hex(colour), 0);
			lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
			lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
			return img;
		}

		lv_obj_t *text(lv_obj_t *parent, const char *string, const lv_font_t *font, std::uint32_t colour)
		{
			lv_obj_t *label = lv_label_create(parent);
			lv_label_set_text(label, string);
			lv_obj_set_style_text_font(label, font, 0);
			lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
			return label;
		}

		// A slight "push in" while a finger is on it
		void add_press_feedback(lv_obj_t *obj)
		{
			lv_obj_set_style_transform_width(obj, -3, LV_STATE_PRESSED);
			lv_obj_set_style_transform_height(obj, -3, LV_STATE_PRESSED);
		}
	}

	Ui::Ui(lv_obj_t *screenToUse, LightRequestHandler handler) :
	  onLightRequest(std::move(handler)),
	  screen(screenToUse)
	{
		lv_obj_set_style_bg_color(screen, lv_color_hex(colour::BACKGROUND), 0);
		lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
		lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

		for (int i = 0; i < 3; i++)
		{
			pages[i] = plain_box(screen);
			lv_obj_set_pos(pages[i], RAIL_WIDTH, 0);
			lv_obj_set_size(pages[i], PAGE_WIDTH, PAGE_HEIGHT);
			lv_obj_set_style_bg_color(pages[i], lv_color_hex(colour::BACKGROUND), 0);
			lv_obj_set_style_bg_opa(pages[i], LV_OPA_COVER, 0);
		}

		build_rail();
		build_lights_page(pages[static_cast<int>(Tab::Lights)]);
		build_camera_page(pages[static_cast<int>(Tab::Camera)]);
		show_tab(Tab::Lights);
	}

	// ------------------------------------------------------------------ rail

	void Ui::build_rail()
	{
		lv_obj_t *rail = plain_box(screen);
		lv_obj_set_size(rail, RAIL_WIDTH, PAGE_HEIGHT);
		lv_obj_set_style_bg_color(rail, lv_color_hex(colour::RAIL), 0);
		lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);

		// Yellow accent along the edge, like the stripe on the machine
		lv_obj_t *stripe = plain_box(rail);
		lv_obj_set_pos(stripe, RAIL_WIDTH - 3, 0);
		lv_obj_set_size(stripe, 3, PAGE_HEIGHT);
		lv_obj_set_style_bg_color(stripe, lv_color_hex(colour::YELLOW), 0);
		lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);

		const lv_img_dsc_t *icons[] = { &img_icon_lights, &img_icon_isobus, &img_icon_camera };
		const char *names[] = { "LIGHTS", "ISOBUS", "CAMERA" };

		for (int i = 0; i < 3; i++)
		{
			lv_obj_t *button = plain_box(rail);
			lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_set_pos(button, 8, 16 + i * 112);
			lv_obj_set_size(button, RAIL_WIDTH - 19, 100);
			lv_obj_set_style_radius(button, 14, 0);
			add_press_feedback(button);

			tabIcons[i] = icon(button, icons[i], colour::TEXT_DIM);
			lv_obj_align(tabIcons[i], LV_ALIGN_TOP_MID, 0, 16);
			tabLabels[i] = text(button, names[i], &lv_font_montserrat_12, colour::TEXT_DIM);
			lv_obj_set_style_text_letter_space(tabLabels[i], 1, 0);
			lv_obj_align(tabLabels[i], LV_ALIGN_BOTTOM_MID, 0, -14);

			tabTargets[i] = { this, i };
			lv_obj_add_event_cb(button, tab_clicked, LV_EVENT_CLICKED, &tabTargets[i]);
			tabButtons[i] = button;
		}

		// Implement status dot on the ISOBUS tab
		isobusDot = plain_box(tabButtons[static_cast<int>(Tab::Isobus)]);
		lv_obj_set_size(isobusDot, 12, 12);
		lv_obj_set_style_radius(isobusDot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(isobusDot, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(isobusDot, 2, 0);
		lv_obj_set_style_border_color(isobusDot, lv_color_hex(colour::RAIL), 0);
		lv_obj_align(isobusDot, LV_ALIGN_TOP_RIGHT, -10, 10);
		set_isobus_state(IsobusState::NoImplement);

		// Speed from the GPS, below the tabs (visible on every tab)
		lv_obj_t *divider = plain_box(rail);
		lv_obj_set_pos(divider, 16, 352);
		lv_obj_set_size(divider, RAIL_WIDTH - 35, 1);
		lv_obj_set_style_bg_color(divider, lv_color_hex(colour::BORDER), 0);
		lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

		speedValue = text(rail, "--", &lv_font_montserrat_28, colour::TEXT);
		lv_obj_set_width(speedValue, RAIL_WIDTH - 3);
		lv_obj_set_style_text_align(speedValue, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_pos(speedValue, 0, 362);
		speedUnit = text(rail, "km/h", &lv_font_montserrat_12, colour::TEXT_DIM);
		lv_obj_set_width(speedUnit, RAIL_WIDTH - 3);
		lv_obj_set_style_text_align(speedUnit, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_pos(speedUnit, 0, 396);

		gpsDot = plain_box(rail);
		lv_obj_set_size(gpsDot, 10, 10);
		lv_obj_set_style_radius(gpsDot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(gpsDot, LV_OPA_COVER, 0);
		lv_obj_set_pos(gpsDot, 14, PAGE_HEIGHT - 53);
		gpsLabel = text(rail, "GPS", &lv_font_montserrat_12, colour::TEXT_DIM);
		lv_obj_set_pos(gpsLabel, 30, PAGE_HEIGHT - 56);
		set_speed(false, false, 0.0f, 0);

		// CAN bus status at the bottom of the rail
		canDot = plain_box(rail);
		lv_obj_set_size(canDot, 10, 10);
		lv_obj_set_style_radius(canDot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(canDot, LV_OPA_COVER, 0);
		lv_obj_set_pos(canDot, 14, PAGE_HEIGHT - 31);
		canLabel = text(rail, "CAN", &lv_font_montserrat_12, colour::TEXT_DIM);
		lv_obj_set_pos(canLabel, 30, PAGE_HEIGHT - 34);
		set_can_status(false, "CAN");
	}

	void Ui::style_tab(int index, bool selected)
	{
		lv_obj_set_style_bg_color(tabButtons[index], lv_color_hex(colour::YELLOW), 0);
		lv_obj_set_style_bg_opa(tabButtons[index], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		std::uint32_t c = selected ? colour::TEXT_ON_YELLOW : colour::TEXT_DIM;
		lv_obj_set_style_img_recolor(tabIcons[index], lv_color_hex(c), 0);
		lv_obj_set_style_text_color(tabLabels[index], lv_color_hex(c), 0);
	}

	void Ui::show_tab(Tab tab)
	{
		currentTab = tab;
		for (int i = 0; i < 3; i++)
		{
			bool selected = (i == static_cast<int>(tab));
			style_tab(i, selected);
			if (selected)
			{
				lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
			}
			else
			{
				lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
			}
		}
	}

	void Ui::tab_clicked(lv_event_t *e)
	{
		auto *target = static_cast<Target *>(lv_event_get_user_data(e));
		target->owner->show_tab(static_cast<Tab>(target->index));
	}

	void Ui::set_isobus_state(IsobusState state)
	{
		std::uint32_t c = colour::TEXT_DIM;
		if (IsobusState::Connected == state)
		{
			c = colour::GREEN;
		}
		else if (IsobusState::Alarm == state)
		{
			c = colour::RED;
		}
		lv_obj_set_style_bg_color(isobusDot, lv_color_hex(c), 0);
	}

	void Ui::set_can_status(bool busOk, const char *label)
	{
		lv_obj_set_style_bg_color(canDot, lv_color_hex(busOk ? colour::GREEN : colour::RED), 0);
		lv_label_set_text(canLabel, label);
	}

	void Ui::set_speed(bool gpsConnected, bool fix, float speedKmh, int satellites)
	{
		if (fix)
		{
			// GPS speed jitters by a few tenths when standing still
			if (speedKmh < 0.5f)
			{
				speedKmh = 0.0f;
			}
			int tenths = static_cast<int>(speedKmh * 10.0f + 0.5f);
			lv_label_set_text_fmt(speedValue, "%d.%d", tenths / 10, tenths % 10);
			lv_obj_set_style_text_color(speedValue, lv_color_hex(colour::TEXT), 0);
			lv_label_set_text_fmt(gpsLabel, "GPS %d", satellites);
		}
		else
		{
			lv_label_set_text(speedValue, "--");
			lv_obj_set_style_text_color(speedValue, lv_color_hex(colour::TEXT_DIM), 0);
			lv_label_set_text(gpsLabel, gpsConnected ? "NO FIX" : "NO GPS");
		}

		std::uint32_t dot = fix ? colour::GREEN : (gpsConnected ? colour::YELLOW : colour::RED);
		lv_obj_set_style_bg_color(gpsDot, lv_color_hex(dot), 0);
	}

	// ------------------------------------------------------------------ lights page

	void Ui::build_lights_page(lv_obj_t *page)
	{
		lv_obj_t *title = text(page, "LIGHTS", &lv_font_montserrat_24, colour::TEXT);
		lv_obj_set_style_text_letter_space(title, 3, 0);
		lv_obj_set_pos(title, 24, 18);

		lightsSummary = text(page, "All lights off", &lv_font_montserrat_14, colour::TEXT_DIM);
		lv_obj_set_pos(lightsSummary, 140, 25);

		// "All off" button, top right
		lv_obj_t *allOff = plain_box(page);
		lv_obj_add_flag(allOff, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(allOff, 140, 44);
		lv_obj_align(allOff, LV_ALIGN_TOP_RIGHT, -20, 10);
		lv_obj_set_style_radius(allOff, 22, 0);
		lv_obj_set_style_border_width(allOff, 2, 0);
		lv_obj_set_style_border_color(allOff, lv_color_hex(colour::BORDER), 0);
		lv_obj_set_style_bg_color(allOff, lv_color_hex(colour::PANEL), 0);
		lv_obj_set_style_bg_opa(allOff, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(allOff, lv_color_hex(colour::BORDER), LV_STATE_PRESSED);
		add_press_feedback(allOff);
		lv_obj_t *powerIcon = icon(allOff, &img_icon_power, colour::TEXT);
		lv_obj_align(powerIcon, LV_ALIGN_LEFT_MID, 16, 0);
		lv_obj_t *allOffText = text(allOff, "ALL OFF", &lv_font_montserrat_16, colour::TEXT);
		lv_obj_align(allOffText, LV_ALIGN_LEFT_MID, 52, 0);
		lv_obj_add_event_cb(allOff, all_off_clicked, LV_EVENT_CLICKED, this);

		// Light beams sit behind the drawing, so they come out from under the cab roof
		beamFront = icon(page, &img_beam_front, colour::YELLOW);
		lv_obj_set_pos(beamFront, FRONT_LAMP_X - 6, FRONT_LAMP_Y - (img_beam_front.header.h * 3) / 10); // lamp is 30 % down the beam image
		beamRear = icon(page, &img_beam_rear, colour::YELLOW);
		lv_obj_set_pos(beamRear, REAR_LAMP_X + 6 - img_beam_rear.header.w, REAR_LAMP_Y - (img_beam_rear.header.h * 3) / 10);

		beaconGlow = icon(page, &img_glow, colour::AMBER);
		lv_obj_set_pos(beaconGlow, BEACON_X - img_glow.header.w / 2, BEACON_Y - img_glow.header.h / 2);

		lv_obj_t *tractor = lv_img_create(page);
		lv_img_set_src(tractor, &img_tractor);
		lv_obj_set_pos(tractor, TRACTOR_X, TRACTOR_Y);
		lv_obj_clear_flag(tractor, LV_OBJ_FLAG_CLICKABLE);

		// Ground line under the wheels
		lv_obj_t *ground = plain_box(page);
		lv_obj_set_pos(ground, 40, TRACTOR_Y + img_tractor.header.h - 2);
		lv_obj_set_size(ground, PAGE_WIDTH - 80, 2);
		lv_obj_set_style_bg_color(ground, lv_color_hex(colour::BORDER), 0);
		lv_obj_set_style_bg_opa(ground, LV_OPA_COVER, 0);
		lv_obj_move_background(ground);

		// Glow on top of the drawing so the beacon itself lights up
		lv_obj_move_foreground(beaconGlow);

		// Tap the lamps on the drawing too
		make_hotspot(page, Light::Front, FRONT_LAMP_X, FRONT_LAMP_Y + 4);
		make_hotspot(page, Light::Rear, REAR_LAMP_X, REAR_LAMP_Y + 4);
		make_hotspot(page, Light::Beacon, BEACON_X, BEACON_Y + 6);

		// Three big switches along the bottom
		constexpr lv_coord_t CARD_W = 208;
		constexpr lv_coord_t CARD_H = 100;
		constexpr lv_coord_t GAP = 16;
		const lv_coord_t startX = (PAGE_WIDTH - (3 * CARD_W + 2 * GAP)) / 2;
		const lv_coord_t cardY = PAGE_HEIGHT - CARD_H - 18;
		make_light_card(page, Light::Front, &img_icon_front, LIGHT_TITLES[0], startX, cardY);
		make_light_card(page, Light::Rear, &img_icon_rear, LIGHT_TITLES[1], startX + CARD_W + GAP, cardY);
		make_light_card(page, Light::Beacon, &img_icon_beacon, LIGHT_TITLES[2], startX + 2 * (CARD_W + GAP), cardY);

		for (int i = 0; i < 3; i++)
		{
			set_light(static_cast<Light>(i), false);
		}
	}

	lv_obj_t *Ui::make_light_card(lv_obj_t *parent, Light light, const lv_img_dsc_t *image, const char *title, lv_coord_t x, lv_coord_t y)
	{
		const int i = static_cast<int>(light);
		lv_obj_t *card = plain_box(parent);
		lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_pos(card, x, y);
		lv_obj_set_size(card, 208, 100);
		lv_obj_set_style_radius(card, 16, 0);
		lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(card, 2, 0);
		add_press_feedback(card);

		cardIcons[i] = icon(card, image, colour::TEXT_DIM);
		lv_obj_align(cardIcons[i], LV_ALIGN_LEFT_MID, 18, 0);

		cardTitles[i] = text(card, title, &lv_font_montserrat_16, colour::TEXT);
		lv_obj_align(cardTitles[i], LV_ALIGN_TOP_LEFT, 80, 26);
		cardStates[i] = text(card, "OFF", &lv_font_montserrat_20, colour::TEXT_DIM);
		lv_obj_align(cardStates[i], LV_ALIGN_TOP_LEFT, 80, 50);

		lightTargets[i] = { this, i };
		lv_obj_add_event_cb(card, light_clicked, LV_EVENT_CLICKED, &lightTargets[i]);
		cards[i] = card;
		return card;
	}

	void Ui::make_hotspot(lv_obj_t *parent, Light light, lv_coord_t centreX, lv_coord_t centreY)
	{
		lv_obj_t *spot = plain_box(parent);
		lv_obj_add_flag(spot, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(spot, 64, 64);
		lv_obj_set_pos(spot, centreX - 32, centreY - 32);
		lv_obj_add_event_cb(spot, light_clicked, LV_EVENT_CLICKED, &lightTargets[static_cast<int>(light)]);
	}

	void Ui::set_light(Light light, bool on)
	{
		const int i = static_cast<int>(light);
		lightOn[i] = on;

		lv_obj_t *card = cards[i];
		lv_obj_set_style_bg_color(card, lv_color_hex(on ? colour::YELLOW : colour::PANEL), 0);
		lv_obj_set_style_border_color(card, lv_color_hex(on ? colour::YELLOW : colour::BORDER), 0);
		lv_obj_set_style_img_recolor(cardIcons[i], lv_color_hex(on ? colour::TEXT_ON_YELLOW : colour::TEXT_DIM), 0);
		lv_obj_set_style_text_color(cardTitles[i], lv_color_hex(on ? colour::TEXT_ON_YELLOW : colour::TEXT), 0);
		lv_obj_set_style_text_color(cardStates[i], lv_color_hex(on ? colour::TEXT_ON_YELLOW : colour::TEXT_DIM), 0);
		lv_label_set_text(cardStates[i], on ? "ON" : "OFF");

		switch (light)
		{
			case Light::Front:
				on ? lv_obj_clear_flag(beamFront, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(beamFront, LV_OBJ_FLAG_HIDDEN);
				break;

			case Light::Rear:
				on ? lv_obj_clear_flag(beamRear, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(beamRear, LV_OBJ_FLAG_HIDDEN);
				break;

			case Light::Beacon:
				lv_anim_del(beaconGlow, beacon_anim);
				if (on)
				{
					lv_obj_clear_flag(beaconGlow, LV_OBJ_FLAG_HIDDEN);
					lv_anim_t a;
					lv_anim_init(&a);
					lv_anim_set_var(&a, beaconGlow);
					lv_anim_set_exec_cb(&a, beacon_anim);
					lv_anim_set_values(&a, 40, 255);
					lv_anim_set_time(&a, 350);
					lv_anim_set_playback_time(&a, 350);
					lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
					lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
					lv_anim_start(&a);
				}
				else
				{
					lv_obj_add_flag(beaconGlow, LV_OBJ_FLAG_HIDDEN);
				}
				break;

			default:
				break;
		}

		update_lights_summary();
	}

	void Ui::set_lights_online(bool online)
	{
		lightsOnline = online;
		update_lights_summary();
	}

	void Ui::update_lights_summary()
	{
		if (!lightsOnline)
		{
			lv_label_set_text(lightsSummary, "Lights module offline");
			lv_obj_set_style_text_color(lightsSummary, lv_color_hex(colour::RED), 0);
			return;
		}

		lv_obj_set_style_text_color(lightsSummary, lv_color_hex(colour::TEXT_DIM), 0);
		int count = (lightOn[0] ? 1 : 0) + (lightOn[1] ? 1 : 0) + (lightOn[2] ? 1 : 0);
		if (0 == count)
		{
			lv_label_set_text(lightsSummary, "All lights off");
		}
		else
		{
			lv_label_set_text_fmt(lightsSummary, "%d of 3 on", count);
		}
	}

	void Ui::request_toggle(Light light)
	{
		if (onLightRequest)
		{
			onLightRequest(light, !lightOn[static_cast<int>(light)]);
		}
	}

	void Ui::light_clicked(lv_event_t *e)
	{
		auto *target = static_cast<Target *>(lv_event_get_user_data(e));
		target->owner->request_toggle(static_cast<Light>(target->index));
	}

	void Ui::all_off_clicked(lv_event_t *e)
	{
		auto *self = static_cast<Ui *>(lv_event_get_user_data(e));
		for (int i = 0; i < 3; i++)
		{
			if (self->lightOn[i] && self->onLightRequest)
			{
				self->onLightRequest(static_cast<Light>(i), false);
			}
		}
	}

	void Ui::beacon_anim(void *object, int32_t value)
	{
		lv_obj_set_style_img_opa(static_cast<lv_obj_t *>(object), static_cast<lv_opa_t>(value), 0);
	}

	// ------------------------------------------------------------------ camera page

	namespace
	{
		constexpr lv_coord_t VIDEO_W = 640;
		constexpr lv_coord_t VIDEO_H = 480;

		// Reversing guide lines on the 640x480 picture: red (close), yellow, green (far)
		lv_point_t GUIDES[6][3] = {
			{ { 150, 480 }, { 187, 400 }, { 217, 400 } }, // red left
			{ { 490, 480 }, { 453, 400 }, { 423, 400 } }, // red right
			{ { 187, 400 }, { 223, 320 }, { 248, 320 } }, // yellow left
			{ { 453, 400 }, { 417, 320 }, { 392, 320 } }, // yellow right
			{ { 223, 320 }, { 255, 250 }, { 280, 250 } }, // green left
			{ { 417, 320 }, { 385, 250 }, { 360, 250 } }, // green right
		};
		const std::uint32_t GUIDE_COLOURS[3] = { 0xFF3B30, 0xFFB800, 0x3DD68C };

		// Small "guide lines" icon for the side button
		lv_point_t GUIDE_ICON[2][2] = {
			{ { 8, 34 }, { 18, 2 } },
			{ { 40, 34 }, { 30, 2 } },
		};
	}

	lv_obj_t *Ui::make_side_button(lv_obj_t *parent, lv_coord_t y, const char *label, lv_event_cb_t callback)
	{
		lv_obj_t *button = plain_box(parent);
		lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_pos(button, 4, y);
		lv_obj_set_size(button, PAGE_WIDTH - VIDEO_W - 8, 100);
		lv_obj_set_style_radius(button, 12, 0);
		lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(button, 2, 0);
		add_press_feedback(button);
		lv_obj_t *name = text(button, label, &lv_font_montserrat_12, colour::TEXT_DIM);
		lv_obj_set_style_text_letter_space(name, 1, 0);
		lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -10);
		lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, this);
		return button;
	}

	void Ui::style_side_button(lv_obj_t *button, bool on)
	{
		lv_obj_set_style_bg_color(button, lv_color_hex(on ? colour::YELLOW : colour::PANEL), 0);
		lv_obj_set_style_border_color(button, lv_color_hex(on ? colour::YELLOW : colour::BORDER), 0);
		std::uint32_t ink = on ? colour::TEXT_ON_YELLOW : colour::TEXT_DIM;
		std::uint32_t count = lv_obj_get_child_cnt(button);
		for (std::uint32_t i = 0; i < count; i++)
		{
			lv_obj_t *child = lv_obj_get_child(button, i);
			lv_obj_set_style_text_color(child, lv_color_hex(ink), 0);
			lv_obj_set_style_img_recolor(child, lv_color_hex(ink), 0);
			lv_obj_set_style_line_color(child, lv_color_hex(ink), 0);
		}
	}

	void Ui::build_camera_page(lv_obj_t *page)
	{
		// ---- picture area: LVGL only paints it black; the picture is drawn straight to the screen ----
		lv_obj_t *video = plain_box(page);
		lv_obj_set_size(video, VIDEO_W, VIDEO_H);
		lv_obj_set_style_bg_color(video, lv_color_hex(0x000000), 0);
		lv_obj_set_style_bg_opa(video, LV_OPA_COVER, 0);

		videoPlaceholder = plain_box(video);
		lv_obj_set_size(videoPlaceholder, VIDEO_W, 220);
		lv_obj_center(videoPlaceholder);
		lv_obj_t *camIcon = icon(videoPlaceholder, &img_icon_camera_big, 0x4A4A4A);
		lv_obj_align(camIcon, LV_ALIGN_TOP_MID, 0, 0);
		placeholderTitle = text(videoPlaceholder, "No camera", &lv_font_montserrat_20, colour::TEXT);
		lv_obj_align(placeholderTitle, LV_ALIGN_TOP_MID, 0, 116);
		placeholderText = text(videoPlaceholder, "Looking for the camera's WiFi (HESAB-CAM)", &lv_font_montserrat_14, colour::TEXT_DIM);
		lv_obj_align(placeholderText, LV_ALIGN_TOP_MID, 0, 150);

		// ---- side column: camera LEDs, guide lines, status ----
		lv_obj_t *side = plain_box(page);
		lv_obj_set_pos(side, VIDEO_W, 0);
		lv_obj_set_size(side, PAGE_WIDTH - VIDEO_W, PAGE_HEIGHT);

		ledButton = make_side_button(side, 12, "LEDS", camera_led_clicked);
		lv_obj_t *ledIcon = icon(ledButton, &img_icon_rear, colour::TEXT_DIM);
		lv_obj_align(ledIcon, LV_ALIGN_TOP_MID, 0, 14);
		style_side_button(ledButton, false);

		linesButton = make_side_button(side, 124, "GUIDE", guide_lines_clicked);
		for (auto &points : GUIDE_ICON)
		{
			lv_obj_t *line = lv_line_create(linesButton);
			lv_line_set_points(line, points, 2);
			lv_obj_set_style_line_width(line, 4, 0);
			lv_obj_set_style_line_rounded(line, true, 0);
			lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 22);
			lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
		}
		style_side_button(linesButton, guidesShown);

		cameraChipDot = plain_box(side);
		lv_obj_set_size(cameraChipDot, 12, 12);
		lv_obj_set_style_radius(cameraChipDot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(cameraChipDot, LV_OPA_COVER, 0);
		lv_obj_align(cameraChipDot, LV_ALIGN_BOTTOM_MID, 0, -64);
		cameraChipText = text(side, "NO CAM", &lv_font_montserrat_12, colour::TEXT);
		lv_obj_set_style_text_letter_space(cameraChipText, 1, 0);
		lv_obj_align(cameraChipText, LV_ALIGN_BOTTOM_MID, 0, -40);
		cameraFpsText = text(side, "", &lv_font_montserrat_12, colour::TEXT_DIM);
		lv_obj_align(cameraFpsText, LV_ALIGN_BOTTOM_MID, 0, -20);

		set_camera_state(CameraState::NoWifi, 0.0f);
	}

	void Ui::camera_frame_shown()
	{
		if (!haveFrame)
		{
			haveFrame = true;
			lv_obj_add_flag(videoPlaceholder, LV_OBJ_FLAG_HIDDEN);
		}
	}

	void Ui::draw_guides(std::uint16_t *pixels, int width, int height)
	{
		// Thick line by stamping squares along it (only ~600 points per picture)
		auto stamp = [&](int cx, int cy, int half, std::uint16_t colour) {
			for (int y = cy - half; y <= cy + half; y++)
			{
				if ((y < 0) || (y >= height))
				{
					continue;
				}
				std::uint16_t *row = pixels + y * width;
				for (int x = cx - half; x <= cx + half; x++)
				{
					if ((x >= 0) && (x < width))
					{
						row[x] = colour;
					}
				}
			}
		};
		auto rgb565 = [](std::uint32_t rgb) {
			return static_cast<std::uint16_t>(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
		};
		for (int pass = 0; pass < 2; pass++) // dark edge first, colour on top
		{
			for (int i = 0; i < 6; i++)
			{
				std::uint16_t colour = (0 == pass) ? 0x0000 : rgb565(GUIDE_COLOURS[i / 2]);
				int half = (0 == pass) ? 4 : 2;
				for (int segment = 0; segment < 2; segment++)
				{
					const lv_point_t &a = GUIDES[i][segment];
					const lv_point_t &b = GUIDES[i][segment + 1];
					int steps = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
					for (int step = 0; step <= steps; step++)
					{
						int x = a.x + (b.x - a.x) * step / std::max(steps, 1);
						int y = a.y + (b.y - a.y) * step / std::max(steps, 1);
						stamp(x * width / VIDEO_W, y * height / VIDEO_H, half, colour);
					}
				}
			}
		}
	}

	void Ui::set_camera_state(CameraState state, float fps)
	{
		std::uint32_t dot = colour::RED;
		const char *status = "NO CAM";
		char rate[16] = "";
		if (CameraState::Streaming == state)
		{
			dot = colour::GREEN;
			status = "LIVE";
			lv_snprintf(rate, sizeof(rate), "%d fps", static_cast<int>(fps + 0.5f));
		}
		else if (CameraState::Connecting == state)
		{
			dot = colour::YELLOW;
			status = "WAIT";
		}
		lv_obj_set_style_bg_color(cameraChipDot, lv_color_hex(dot), 0);
		lv_label_set_text(cameraChipText, status);
		lv_label_set_text(cameraFpsText, rate);

		if (CameraState::Streaming != state)
		{
			// Never leave a frozen picture on screen: it would look like a live view
			haveFrame = false;
			lv_obj_clear_flag(videoPlaceholder, LV_OBJ_FLAG_HIDDEN);
			lv_obj_invalidate(lv_obj_get_parent(videoPlaceholder)); // repaint the whole picture area black
			bool noWifi = (CameraState::NoWifi == state);
			lv_label_set_text(placeholderTitle, noWifi ? "No camera" : "Connecting...");
			lv_label_set_text(placeholderText, noWifi ? "Looking for the camera's WiFi (HESAB-CAM)" : "Camera found, waiting for the picture");
		}
	}

	void Ui::set_camera_led(bool on)
	{
		cameraLedOn = on;
		style_side_button(ledButton, on);
	}

	void Ui::camera_led_clicked(lv_event_t *e)
	{
		auto *self = static_cast<Ui *>(lv_event_get_user_data(e));
		if (self->onCameraLed)
		{
			self->onCameraLed(!self->cameraLedOn);
		}
	}

	void Ui::guide_lines_clicked(lv_event_t *e)
	{
		auto *self = static_cast<Ui *>(lv_event_get_user_data(e));
		self->guidesShown = !self->guidesShown; // drawn into each picture
		self->style_side_button(self->linesButton, self->guidesShown);
	}
}
