#include "vt_renderer.hpp"

#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

using isobus::VirtualTerminalObjectType;
using isobus::VTObject;
using KeyActivationCode = isobus::VirtualTerminalBase::KeyActivationCode;

namespace
{
	constexpr int MAX_DEPTH = 16; // guards against object pools that reference themselves

	template<typename T>
	std::shared_ptr<T> as(std::shared_ptr<VTObject> object, VirtualTerminalObjectType type)
	{
		return ((nullptr != object) && (object->get_object_type() == type)) ? std::static_pointer_cast<T>(object) : nullptr;
	}

	void append_utf8(std::string &out, std::uint32_t codePoint)
	{
		if (codePoint < 0x80)
		{
			out += static_cast<char>(codePoint);
		}
		else if (codePoint < 0x800)
		{
			out += static_cast<char>(0xC0 | (codePoint >> 6));
			out += static_cast<char>(0x80 | (codePoint & 0x3F));
		}
		else
		{
			out += static_cast<char>(0xE0 | (codePoint >> 12));
			out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (codePoint & 0x3F));
		}
	}

	// VT strings are ISO 8859-1, or UTF-16LE when they start with 0xFF 0xFE. LVGL wants UTF-8.
	std::string vt_string_to_utf8(const std::string &in)
	{
		std::string out;
		if ((in.size() >= 2) && (static_cast<std::uint8_t>(in[0]) == 0xFF) && (static_cast<std::uint8_t>(in[1]) == 0xFE))
		{
			for (std::size_t i = 2; (i + 1) < in.size(); i += 2)
			{
				std::uint32_t c = static_cast<std::uint8_t>(in[i]) | (static_cast<std::uint8_t>(in[i + 1]) << 8);
				if (0 != c)
				{
					append_utf8(out, c);
				}
			}
		}
		else
		{
			for (char ch : in)
			{
				std::uint8_t c = static_cast<std::uint8_t>(ch);
				if (0 != c)
				{
					append_utf8(out, c);
				}
			}
		}
		return out;
	}
}

VtRenderer::VtRenderer(lv_obj_t *screenToUse, KeyEventHandler eventHandler) :
  screen(screenToUse),
  handler(std::move(eventHandler))
{
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x202020), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

	maskArea = make_box(screen, 0, 0, MASK_SIZE, MASK_SIZE);
	lv_obj_set_style_bg_color(maskArea, lv_color_hex(0x000000), 0);
	lv_obj_set_style_bg_opa(maskArea, LV_OPA_COVER, 0);

	keyArea = make_box(screen, KEY_X - 5, 0, KEY_SIZE + 10, MASK_SIZE);
	lv_obj_set_style_bg_color(keyArea, lv_color_hex(0x303030), 0);
	lv_obj_set_style_bg_opa(keyArea, LV_OPA_COVER, 0);

	statusLabel = lv_label_create(screen);
	lv_obj_set_pos(statusLabel, STATUS_X, 10);
	lv_obj_set_width(statusLabel, 800 - STATUS_X - 10);
	lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_color(statusLabel, lv_color_hex(0xE0E0E0), 0);
	lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
	lv_label_set_text(statusLabel, "ISOBUS VT");

	waitingLabel = nullptr;
	set_waiting_text("Starting...");
}

lv_obj_t *VtRenderer::make_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_remove_style_all(box); // transparent, no border, no padding
	lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
	lv_obj_set_pos(box, x, y);
	lv_obj_set_size(box, w, h);
	return box;
}

lv_color_t VtRenderer::vt_colour(std::uint8_t index) const
{
	if (nullptr == workingSet)
	{
		return lv_color_black();
	}
	auto c = workingSet->get_colour(index);
	return lv_color_make(static_cast<std::uint8_t>(c.r * 255.0f), static_cast<std::uint8_t>(c.g * 255.0f), static_cast<std::uint8_t>(c.b * 255.0f));
}

const lv_font_t *VtRenderer::font_for(std::shared_ptr<VTObject> fontObject) const
{
	auto font = as<isobus::FontAttributes>(fontObject, VirtualTerminalObjectType::FontAttributes);
	std::uint8_t h = (nullptr != font) ? font->get_font_height_pixels() : 16;

	// VT fonts are fixed bitmap sizes (6x8 ... 128x192). Pick the largest Montserrat whose
	// line height fits the VT font height, so wrapped lines take the space the implement planned.
	static const lv_font_t *const fonts[] = {
		&lv_font_montserrat_48, &lv_font_montserrat_40, &lv_font_montserrat_32, &lv_font_montserrat_28,
		&lv_font_montserrat_24, &lv_font_montserrat_20, &lv_font_montserrat_16, &lv_font_montserrat_14,
		&lv_font_montserrat_12, &lv_font_montserrat_10
	};
	for (const lv_font_t *candidate : fonts)
	{
		if (candidate->line_height <= h)
		{
			return candidate;
		}
	}
	return &lv_font_montserrat_8;
}

void VtRenderer::set_waiting_text(const std::string &text)
{
	if (nullptr == waitingLabel)
	{
		waitingLabel = lv_label_create(maskArea);
		lv_obj_set_style_text_color(waitingLabel, lv_color_hex(0xA0A0A0), 0);
		lv_obj_set_style_text_font(waitingLabel, &lv_font_montserrat_24, 0);
		lv_obj_center(waitingLabel);
	}
	lv_label_set_text(waitingLabel, text.c_str());
	lv_obj_center(waitingLabel);
}

void VtRenderer::set_status(const std::string &text)
{
	lv_label_set_text(statusLabel, text.c_str());
}

void VtRenderer::render(std::shared_ptr<isobus::VirtualTerminalWorkingSetBase> newWorkingSet, std::uint16_t newActiveMaskId)
{
	if (newWorkingSet != workingSet)
	{
		imageCache.clear();
	}
	workingSet = newWorkingSet;
	activeMaskId = newActiveMaskId;

	// Throw away the old drawing. Deleting the parent deletes all children.
	lv_obj_clean(maskArea);
	lv_obj_clean(keyArea);
	waitingLabel = nullptr;
	trackedTexts.clear();
	keyInfos.clear();
	heldKey = nullptr;

	lv_obj_set_style_bg_color(maskArea, lv_color_hex(0x000000), 0);
	lv_obj_set_style_bg_color(keyArea, lv_color_hex(0x303030), 0);

	if (nullptr == workingSet)
	{
		set_waiting_text("Waiting for implement...");
		return;
	}

	auto mask = workingSet->get_object_by_id(activeMaskId);
	if ((nullptr == mask) ||
	    ((VirtualTerminalObjectType::DataMask != mask->get_object_type()) && (VirtualTerminalObjectType::AlarmMask != mask->get_object_type())))
	{
		set_waiting_text("No mask to show");
		return;
	}

	render_mask(mask);
	render_soft_keys(mask);
}

void VtRenderer::render_mask(std::shared_ptr<VTObject> mask)
{
	lv_obj_set_style_bg_color(maskArea, vt_colour(mask->get_background_color()), 0);
	render_children(maskArea, mask, 0);
}

void VtRenderer::render_soft_keys(std::shared_ptr<VTObject> mask)
{
	std::uint16_t softKeyMaskId = isobus::NULL_OBJECT_ID;
	if (auto dataMask = as<isobus::DataMask>(mask, VirtualTerminalObjectType::DataMask))
	{
		softKeyMaskId = dataMask->get_soft_key_mask();
	}
	else if (auto alarmMask = as<isobus::AlarmMask>(mask, VirtualTerminalObjectType::AlarmMask))
	{
		softKeyMaskId = alarmMask->get_soft_key_mask();
	}

	auto softKeyMask = workingSet->get_object_by_id(softKeyMaskId);
	if ((nullptr == softKeyMask) || (VirtualTerminalObjectType::SoftKeyMask != softKeyMask->get_object_type()))
	{
		return;
	}

	lv_obj_set_style_bg_color(keyArea, vt_colour(softKeyMask->get_background_color()), 0);

	std::uint8_t slot = 0;
	for (std::uint16_t i = 0; (i < softKeyMask->get_number_children()) && (slot < NUMBER_OF_KEYS); i++)
	{
		auto child = workingSet->get_object_by_id(softKeyMask->get_child_id(i));

		// Soft key masks may list keys directly or through object pointers
		if (auto pointer = as<isobus::ObjectPointer>(child, VirtualTerminalObjectType::ObjectPointer))
		{
			child = workingSet->get_object_by_id(pointer->get_value());
		}

		auto key = as<isobus::Key>(child, VirtualTerminalObjectType::Key);
		if (nullptr == key)
		{
			continue; // NULL pointer = empty slot is still a slot in the VT standard
		}

		lv_obj_t *keyBox = make_box(keyArea, 5, 10 + slot * KEY_PITCH, KEY_SIZE, KEY_SIZE);
		lv_obj_set_style_bg_color(keyBox, vt_colour(key->get_background_color()), 0);
		lv_obj_set_style_bg_opa(keyBox, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color(keyBox, lv_color_hex(0x808080), 0);
		lv_obj_set_style_border_width(keyBox, 1, 0);
		lv_obj_set_style_clip_corner(keyBox, true, 0);
		render_children(keyBox, key, 1);
		make_pressable(keyBox, true, key->get_id(), key->get_key_code());
		slot++;
	}
}

void VtRenderer::render_children(lv_obj_t *parent, std::shared_ptr<VTObject> object, int depth)
{
	for (std::uint16_t i = 0; i < object->get_number_children(); i++)
	{
		render_object(parent,
		              workingSet->get_object_by_id(object->get_child_id(i)),
		              object->get_child_x(i),
		              object->get_child_y(i),
		              depth + 1);
	}
}

void VtRenderer::render_object(lv_obj_t *parent, std::shared_ptr<VTObject> object, lv_coord_t x, lv_coord_t y, int depth)
{
	if ((nullptr == object) || (depth > MAX_DEPTH))
	{
		return;
	}

	switch (object->get_object_type())
	{
		case VirtualTerminalObjectType::Container:
		{
			auto container = std::static_pointer_cast<isobus::Container>(object);
			if (!container->get_hidden())
			{
				lv_obj_t *box = make_box(parent, x, y, object->get_width(), object->get_height());
				render_children(box, object, depth);
			}
		}
		break;

		case VirtualTerminalObjectType::ObjectPointer:
		{
			auto pointer = std::static_pointer_cast<isobus::ObjectPointer>(object);
			if (isobus::NULL_OBJECT_ID != pointer->get_value())
			{
				render_object(parent, workingSet->get_object_by_id(pointer->get_value()), x, y, depth + 1);
			}
		}
		break;

		case VirtualTerminalObjectType::OutputRectangle:
			render_rectangle(parent, object, x, y);
			break;

		case VirtualTerminalObjectType::OutputEllipse:
			render_ellipse(parent, object, x, y);
			break;

		case VirtualTerminalObjectType::OutputString:
		case VirtualTerminalObjectType::OutputNumber:
		case VirtualTerminalObjectType::InputString:
		case VirtualTerminalObjectType::InputNumber:
			render_text(parent, object, x, y);
			break;

		case VirtualTerminalObjectType::Button:
			render_button(parent, object, x, y, depth);
			break;

		case VirtualTerminalObjectType::PictureGraphic:
			render_picture(parent, object, x, y);
			break;

		default:
			// Not drawn yet: lines, polygons, meters, bar graphs, lists, input booleans...
			break;
	}
}

void VtRenderer::render_rectangle(lv_obj_t *parent, std::shared_ptr<VTObject> object, lv_coord_t x, lv_coord_t y)
{
	auto rect = std::static_pointer_cast<isobus::OutputRectangle>(object);
	lv_obj_t *box = make_box(parent, x, y, object->get_width(), object->get_height());

	auto line = as<isobus::LineAttributes>(workingSet->get_object_by_id(rect->get_line_attributes()), VirtualTerminalObjectType::LineAttributes);
	auto fill = as<isobus::FillAttributes>(workingSet->get_object_by_id(rect->get_fill_attributes()), VirtualTerminalObjectType::FillAttributes);

	if (nullptr != fill)
	{
		switch (fill->get_type())
		{
			case isobus::FillAttributes::FillType::FillWithLineColor:
				if (nullptr != line)
				{
					lv_obj_set_style_bg_color(box, vt_colour(line->get_background_color()), 0);
					lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
				}
				break;

			case isobus::FillAttributes::FillType::FillWithSpecifiedColorInFillColorAttribute:
			case isobus::FillAttributes::FillType::FillWithPatternGivenByFillPatternAttribute: // pattern not drawn, use the colour
				lv_obj_set_style_bg_color(box, vt_colour(fill->get_background_color()), 0);
				lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
				break;

			default:
				break;
		}
	}

	if ((nullptr != line) && (0 != line->get_width()))
	{
		// The line width is stored in the width field of the line attributes object
		std::uint8_t suppressed = rect->get_line_suppression_bitfield();
		int sides = LV_BORDER_SIDE_NONE;
		if (0 == (suppressed & 0x01)) sides |= LV_BORDER_SIDE_TOP;
		if (0 == (suppressed & 0x02)) sides |= LV_BORDER_SIDE_RIGHT;
		if (0 == (suppressed & 0x04)) sides |= LV_BORDER_SIDE_BOTTOM;
		if (0 == (suppressed & 0x08)) sides |= LV_BORDER_SIDE_LEFT;

		lv_obj_set_style_border_color(box, vt_colour(line->get_background_color()), 0);
		lv_obj_set_style_border_width(box, line->get_width(), 0);
		lv_obj_set_style_border_side(box, static_cast<lv_border_side_t>(sides), 0);
		lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
	}
}

void VtRenderer::render_ellipse(lv_obj_t *parent, std::shared_ptr<VTObject> object, lv_coord_t x, lv_coord_t y)
{
	// Only closed, full ellipses for now
	auto ellipse = std::static_pointer_cast<isobus::OutputEllipse>(object);
	lv_obj_t *box = make_box(parent, x, y, object->get_width(), object->get_height());
	lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, 0);

	auto line = as<isobus::LineAttributes>(workingSet->get_object_by_id(ellipse->get_line_attributes()), VirtualTerminalObjectType::LineAttributes);
	auto fill = as<isobus::FillAttributes>(workingSet->get_object_by_id(ellipse->get_fill_attributes()), VirtualTerminalObjectType::FillAttributes);

	if ((nullptr != fill) && (isobus::FillAttributes::FillType::NoFill != fill->get_type()))
	{
		bool useLineColour = (isobus::FillAttributes::FillType::FillWithLineColor == fill->get_type()) && (nullptr != line);
		lv_obj_set_style_bg_color(box, vt_colour(useLineColour ? line->get_background_color() : fill->get_background_color()), 0);
		lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	}
	if ((nullptr != line) && (0 != line->get_width()))
	{
		lv_obj_set_style_border_color(box, vt_colour(line->get_background_color()), 0);
		lv_obj_set_style_border_width(box, line->get_width(), 0);
		lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
	}
}

std::string VtRenderer::text_for(std::shared_ptr<VTObject> object) const
{
	const auto &tree = workingSet->get_object_tree();

	switch (object->get_object_type())
	{
		case VirtualTerminalObjectType::OutputString:
		case VirtualTerminalObjectType::InputString:
		{
			auto s = std::static_pointer_cast<isobus::StringVTObject>(object);
			return vt_string_to_utf8(s->displayed_value(tree));
		}

		case VirtualTerminalObjectType::OutputNumber:
		case VirtualTerminalObjectType::InputNumber:
		{
			auto n = std::static_pointer_cast<isobus::NumberVTObject>(object);
			std::uint32_t raw = n->get_value();

			if (isobus::NULL_OBJECT_ID != n->get_variable_reference())
			{
				auto variable = as<isobus::NumberVariable>(VTObject::get_object_by_id(n->get_variable_reference(), tree), VirtualTerminalObjectType::NumberVariable);
				if (nullptr != variable)
				{
					raw = variable->get_value();
				}
			}

			// Same formula as AgIsoVirtualTerminal: (value + offset) * scale
			double value = (static_cast<double>(raw) + n->get_offset()) * n->get_scale();
			std::uint8_t decimals = n->get_number_of_decimals();

			if ((0.0 == value) && n->get_option(isobus::NumberVTObject::Options::DisplayZeroAsBlank))
			{
				return "";
			}
			if (n->get_option(isobus::NumberVTObject::Options::Truncate))
			{
				double factor = std::pow(10.0, decimals);
				value = std::trunc(value * factor) / factor;
			}

			std::ostringstream text;
			if (n->get_format()) // exponential format
			{
				text << std::scientific << std::setprecision(decimals) << value;
			}
			else
			{
				text << std::fixed << std::setprecision(decimals) << value;
			}
			return text.str();
		}

		default:
			return "";
	}
}

void VtRenderer::render_text(lv_obj_t *parent, std::shared_ptr<VTObject> object, lv_coord_t x, lv_coord_t y)
{
	auto textual = std::static_pointer_cast<isobus::TextualVTObject>(object);
	lv_obj_t *box = make_box(parent, x, y, object->get_width(), object->get_height());

	bool transparent = false;
	bool wrap = false;
	if ((VirtualTerminalObjectType::OutputString == object->get_object_type()) || (VirtualTerminalObjectType::InputString == object->get_object_type()))
	{
		auto s = std::static_pointer_cast<isobus::StringVTObject>(object);
		transparent = s->get_option(isobus::StringVTObject::Options::Transparent);
		wrap = s->get_option(isobus::StringVTObject::Options::AutoWrap);
	}
	else
	{
		transparent = std::static_pointer_cast<isobus::NumberVTObject>(object)->get_option(isobus::NumberVTObject::Options::Transparent);
	}

	if (!transparent)
	{
		lv_obj_set_style_bg_color(box, vt_colour(object->get_background_color()), 0);
		lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	}

	auto fontObject = workingSet->get_object_by_id(textual->get_font_attributes());
	auto font = as<isobus::FontAttributes>(fontObject, VirtualTerminalObjectType::FontAttributes);

	lv_obj_t *label = lv_label_create(box);
	lv_obj_set_width(label, object->get_width());
	lv_label_set_long_mode(label, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_CLIP);
	lv_obj_set_style_text_font(label, font_for(fontObject), 0);
	lv_obj_set_style_text_color(label, (nullptr != font) ? vt_colour(font->get_colour()) : lv_color_black(), 0);

	lv_text_align_t textAlign = LV_TEXT_ALIGN_LEFT;
	switch (textual->get_horizontal_justification())
	{
		case isobus::TextualVTObject::HorizontalJustification::PositionMiddle:
			textAlign = LV_TEXT_ALIGN_CENTER;
			break;
		case isobus::TextualVTObject::HorizontalJustification::PositionRight:
			textAlign = LV_TEXT_ALIGN_RIGHT;
			break;
		default:
			break;
	}
	lv_obj_set_style_text_align(label, textAlign, 0);

	switch (textual->get_vertical_justification())
	{
		case isobus::TextualVTObject::VerticalJustification::PositionMiddle:
			lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
			break;
		case isobus::TextualVTObject::VerticalJustification::PositionBottom:
			lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
			break;
		default:
			lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
			break;
	}

	lv_label_set_text(label, text_for(object).c_str());
	trackedTexts.push_back({ label, object });
}

void VtRenderer::refresh_values()
{
	if (nullptr == workingSet)
	{
		return;
	}
	for (const auto &tracked : trackedTexts)
	{
		std::string text = text_for(tracked.object);
		if (text != lv_label_get_text(tracked.label))
		{
			lv_label_set_text(tracked.label, text.c_str());
		}
	}
}

void VtRenderer::render_button(lv_obj_t *parent, std::shared_ptr<VTObject> object, lv_coord_t x, lv_coord_t y, int depth)
{
	auto button = std::static_pointer_cast<isobus::Button>(object);
	lv_obj_t *box = make_box(parent, x, y, object->get_width(), object->get_height());

	if (!button->get_option(isobus::Button::Options::TransparentBackground))
	{
		lv_obj_set_style_bg_color(box, vt_colour(object->get_background_color()), 0);
		lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	}

	bool drawBorder = !button->get_option(isobus::Button::Options::NoBorder) &&
	  !button->get_option(isobus::Button::Options::SuppressBorder);
	if (drawBorder)
	{
		lv_obj_set_style_border_color(box, vt_colour(button->get_border_colour()), 0);
		lv_obj_set_style_border_width(box, 2, 0);
		lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
	}

	render_children(box, object, depth);

	if (button->get_option(isobus::Button::Options::Disabled))
	{
		lv_obj_set_style_opa(box, LV_OPA_50, 0);
	}
	else
	{
		make_pressable(box, false, object->get_id(), button->get_key_code());
	}
}

const lv_img_dsc_t *VtRenderer::image_for(std::shared_ptr<VTObject> object)
{
	auto found = imageCache.find(object->get_id());
	if (found != imageCache.end())
	{
		return &found->second->descriptor;
	}

	auto picture = std::static_pointer_cast<isobus::PictureGraphic>(object);
	const std::uint16_t srcW = picture->get_actual_width();
	const std::uint16_t srcH = picture->get_actual_height();
	auto &raw = picture->get_raw_data(); // the parser has expanded it to one colour index per pixel

	if ((0 == srcW) || (0 == srcH) || (raw.size() < static_cast<std::size_t>(srcW) * srcH))
	{
		return nullptr;
	}

	// The pool says how wide to draw the picture; the height follows the aspect ratio.
	// Scale once here (nearest neighbour) so LVGL never has to transform images at runtime.
	std::uint16_t w = (0 != picture->get_width()) ? picture->get_width() : srcW;
	std::uint16_t h = static_cast<std::uint16_t>((static_cast<std::uint32_t>(srcH) * w + srcW / 2) / srcW);
	if (0 == h)
	{
		h = 1;
	}

	const bool transparent = picture->get_option(isobus::PictureGraphic::Options::Transparent);
	const std::uint8_t transparentIndex = picture->get_transparency_colour();
	const std::size_t bytesPerPixel = transparent ? LV_IMG_PX_SIZE_ALPHA_BYTE : sizeof(lv_color_t);

	auto cached = std::unique_ptr<CachedImage>(new CachedImage());
	cached->pixels.resize(static_cast<std::size_t>(w) * h * bytesPerPixel);

	// Colour lookup once per palette index instead of once per pixel
	lv_color_t palette[256];
	for (int i = 0; i < 256; i++)
	{
		palette[i] = vt_colour(static_cast<std::uint8_t>(i));
	}

	std::uint8_t *out = cached->pixels.data();
	for (std::uint16_t y = 0; y < h; y++)
	{
		const std::size_t srcRow = (static_cast<std::size_t>(y) * srcH / h) * srcW;
		for (std::uint16_t x = 0; x < w; x++)
		{
			std::uint8_t index = raw[srcRow + static_cast<std::size_t>(x) * srcW / w];
			lv_color_t c = palette[index];
			std::memcpy(out, &c, sizeof(lv_color_t));
			out += sizeof(lv_color_t);
			if (transparent)
			{
				*out++ = (index == transparentIndex) ? LV_OPA_TRANSP : LV_OPA_COVER;
			}
		}
	}

	std::memset(&cached->descriptor, 0, sizeof(cached->descriptor));
	cached->descriptor.header.always_zero = 0;
	cached->descriptor.header.w = w;
	cached->descriptor.header.h = h;
	cached->descriptor.header.cf = transparent ? LV_IMG_CF_TRUE_COLOR_ALPHA : LV_IMG_CF_TRUE_COLOR;
	cached->descriptor.data_size = static_cast<std::uint32_t>(cached->pixels.size());
	cached->descriptor.data = cached->pixels.data();

	const lv_img_dsc_t *result = &cached->descriptor;
	imageCache[object->get_id()] = std::move(cached);
	return result;
}

void VtRenderer::render_picture(lv_obj_t *parent, std::shared_ptr<VTObject> object, lv_coord_t x, lv_coord_t y)
{
	const lv_img_dsc_t *image = image_for(object); // already scaled to the size the pool asks for
	if (nullptr == image)
	{
		return;
	}

	lv_obj_t *img = lv_img_create(parent);
	lv_img_set_src(img, image);
	lv_obj_set_pos(img, x, y);
}

void VtRenderer::make_pressable(lv_obj_t *box, bool isSoftKey, std::uint16_t objectId, std::uint8_t keyCode)
{
	auto info = std::unique_ptr<KeyInfo>(new KeyInfo());
	info->owner = this;
	info->event = { isSoftKey, KeyActivationCode::ButtonPressedOrLatched, objectId, activeMaskId, keyCode };
	info->aborted = false;

	lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

	// Visual feedback while pressed: a bright outline (works whatever the implement drew inside)
	lv_obj_set_style_outline_color(box, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
	lv_obj_set_style_outline_width(box, 3, LV_STATE_PRESSED);
	lv_obj_set_style_outline_pad(box, -3, LV_STATE_PRESSED);
	lv_obj_set_style_outline_opa(box, LV_OPA_COVER, LV_STATE_PRESSED);

	lv_obj_add_event_cb(box, key_event_callback, LV_EVENT_ALL, info.get());
	keyInfos.push_back(std::move(info));
}

void VtRenderer::key_event_callback(lv_event_t *e)
{
	auto *info = static_cast<KeyInfo *>(lv_event_get_user_data(e));
	VtRenderer *self = info->owner;
	KeyEvent event = info->event;

	switch (lv_event_get_code(e))
	{
		case LV_EVENT_PRESSED:
			info->aborted = false;
			self->heldKey = info;
			event.code = KeyActivationCode::ButtonPressedOrLatched;
			self->handler(event);
			break;

		case LV_EVENT_LONG_PRESSED_REPEAT:
			if (!info->aborted)
			{
				event.code = KeyActivationCode::ButtonStillHeld;
				self->handler(event);
			}
			break;

		case LV_EVENT_PRESS_LOST:
			if (!info->aborted)
			{
				info->aborted = true;
				self->heldKey = nullptr;
				event.code = KeyActivationCode::ButtonPressAborted;
				self->handler(event);
			}
			break;

		case LV_EVENT_RELEASED:
			if (!info->aborted)
			{
				self->heldKey = nullptr;
				event.code = KeyActivationCode::ButtonUnlatchedOrReleased;
				self->handler(event);
			}
			break;

		default:
			break;
	}
}
