#include "system_font.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr const char* kFallbackFont = "Sans 10";

std::string Trim(std::string value) {
	auto isSpace = [](unsigned char character) { return std::isspace(character) != 0; };
	value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
	value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
	return value;
}

std::string Unquote(std::string value) {
	value = Trim(std::move(value));
	if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
		(value.front() == '\'' && value.back() == '\''))) {
		value = value.substr(1, value.size() - 2);
	}
	return Trim(std::move(value));
}

std::string ReadSmallFile(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) return {};
	std::ostringstream contents;
	contents << input.rdbuf();
	std::string result = contents.str();
	if (result.size() > 1024 * 1024) return {};
	return result;
}

std::string DecodeXmlEntities(std::string value) {
	const std::pair<const char*, const char*> entities[] = {
		{"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
	};
	for (const auto& entity : entities) {
		std::size_t position = 0;
		while ((position = value.find(entity.first, position)) != std::string::npos) {
			value.replace(position, std::char_traits<char>::length(entity.first), entity.second);
			position += std::char_traits<char>::length(entity.second);
		}
	}
	return value;
}

std::string XmlAttribute(const std::string& tag, const std::string& name) {
	const std::string marker = name + "=";
	std::size_t position = tag.find(marker);
	if (position == std::string::npos) return {};
	position += marker.size();
	if (position >= tag.size() || (tag[position] != '"' && tag[position] != '\'')) return {};
	const char quote = tag[position++];
	const std::size_t end = tag.find(quote, position);
	return end == std::string::npos ? std::string() : DecodeXmlEntities(tag.substr(position, end - position));
}

std::string XfceFont(const std::filesystem::path& path) {
	const std::string contents = ReadSmallFile(path);
	std::size_t position = 0;
	while ((position = contents.find("<property", position)) != std::string::npos) {
		const std::size_t end = contents.find('>', position);
		if (end == std::string::npos) break;
		const std::string tag = contents.substr(position, end - position + 1);
		if (XmlAttribute(tag, "name") == "FontName") {
			return Trim(XmlAttribute(tag, "value"));
		}
		position = end + 1;
	}
	return {};
}

std::string IniValue(const std::filesystem::path& path, const std::string& wantedSection,
	const std::string& wantedKey) {
	std::istringstream input(ReadSmallFile(path));
	std::string section;
	std::string line;
	while (std::getline(input, line)) {
		line = Trim(std::move(line));
		if (line.empty() || line.front() == '#' || line.front() == ';') continue;
		if (line.front() == '[' && line.back() == ']') {
			section = Trim(line.substr(1, line.size() - 2));
			continue;
		}
		const std::size_t equals = line.find('=');
		if (equals == std::string::npos || (!wantedSection.empty() && section != wantedSection)) continue;
		if (Trim(line.substr(0, equals)) == wantedKey) return Unquote(line.substr(equals + 1));
	}
	return {};
}

std::string Gtk2Font(const std::filesystem::path& path) {
	std::istringstream input(ReadSmallFile(path));
	std::string line;
	while (std::getline(input, line)) {
		line = Trim(std::move(line));
		const std::size_t equals = line.find('=');
		if (equals != std::string::npos && Trim(line.substr(0, equals)) == "gtk-font-name") {
			return Unquote(line.substr(equals + 1));
		}
	}
	return {};
}

std::string XsettingsdFont(const std::filesystem::path& path) {
	std::istringstream input(ReadSmallFile(path));
	std::string line;
	while (std::getline(input, line)) {
		line = Trim(std::move(line));
		if (line.rfind("Gtk/FontName", 0) != 0) continue;
		return Unquote(line.substr(std::char_traits<char>::length("Gtk/FontName")));
	}
	return {};
}

std::string KdeFont(const std::filesystem::path& path) {
	const std::string setting = IniValue(path, "General", "font");
	if (setting.empty()) return {};
	const std::size_t firstComma = setting.find(',');
	if (firstComma == std::string::npos) return setting;
	const std::size_t secondComma = setting.find(',', firstComma + 1);
	const std::string family = Trim(setting.substr(0, firstComma));
	const std::string size = Trim(setting.substr(firstComma + 1, secondComma - firstComma - 1));
	return family.empty() || size.empty() ? std::string() : family + " " + size;
}

std::string EnvironmentFont() {
	const char* font = std::getenv("JPEGVIEW_FONT");
	return font == nullptr ? std::string() : Trim(font);
}

std::string ValidUtf8(std::string_view text) {
	const std::string copy(text);
	if (g_utf8_validate(copy.c_str(), static_cast<gssize>(copy.size()), nullptr)) return copy;
	char* valid = g_utf8_make_valid(copy.c_str(), static_cast<gssize>(copy.size()));
	if (valid == nullptr) return {};
	std::string result(valid);
	g_free(valid);
	return result;
}

PangoFontDescription* ScaledDescription(const PangoFontDescription* source, int scale) {
	PangoFontDescription* result = pango_font_description_copy(source);
	const int currentSize = pango_font_description_get_size(result);
	const int baseSize = currentSize > 0 ? currentSize : 10 * PANGO_SCALE;
	if (pango_font_description_get_size_is_absolute(result)) {
		pango_font_description_set_absolute_size(result, static_cast<double>(baseSize) * scale);
	} else {
		pango_font_description_set_size(result, baseSize * scale);
	}
	return result;
}

} // namespace

std::string ResolveDesktopFontDescription(const std::filesystem::path& home,
	const std::filesystem::path& configHome) {
	if (const std::string overrideFont = EnvironmentFont(); !overrideFont.empty()) return overrideFont;
	const std::filesystem::path config = configHome.empty() ? home / ".config" : configHome;
	const std::pair<std::filesystem::path, std::string (*)(const std::filesystem::path&)> candidates[] = {
		{config / "xfce4/xfconf/xfce-perchannel-xml/xsettings.xml", XfceFont},
		{config / "gtk-4.0/settings.ini", [](const std::filesystem::path& path) {
			return IniValue(path, "Settings", "gtk-font-name");
		}},
		{config / "gtk-3.0/settings.ini", [](const std::filesystem::path& path) {
			return IniValue(path, "Settings", "gtk-font-name");
		}},
		{config / "xsettingsd/xsettingsd.conf", XsettingsdFont},
		{home / ".gtkrc-2.0", Gtk2Font},
		{config / "kdeglobals", KdeFont},
	};
	for (const auto& candidate : candidates) {
		const std::string font = candidate.second(candidate.first);
		if (!font.empty()) return font;
	}
	return kFallbackFont;
}

struct SystemFont::Impl {
	explicit Impl(std::string requestedDescription) {
		if (requestedDescription.empty()) {
			const char* homeValue = std::getenv("HOME");
			const char* configValue = std::getenv("XDG_CONFIG_HOME");
			const std::filesystem::path home = homeValue == nullptr ? std::filesystem::path() : homeValue;
			const std::filesystem::path config = configValue == nullptr ? std::filesystem::path() : configValue;
			requestedDescription = ResolveDesktopFontDescription(home, config);
		}
		description = requestedDescription.empty() ? kFallbackFont : std::move(requestedDescription);
		font = pango_font_description_from_string(description.c_str());
		if (font == nullptr) {
			description = kFallbackFont;
			font = pango_font_description_from_string(description.c_str());
		}
		PangoFontMap* map = pango_cairo_font_map_get_default();
		context = pango_font_map_create_context(map);
	}

	~Impl() {
		if (font != nullptr) pango_font_description_free(font);
		if (context != nullptr) g_object_unref(context);
	}

	PangoLayout* Layout(std::string_view text, int scale) const {
		PangoLayout* layout = pango_layout_new(context);
		PangoFontDescription* scaled = ScaledDescription(font, std::max(1, scale));
		pango_layout_set_font_description(layout, scaled);
		pango_font_description_free(scaled);
		const std::string valid = ValidUtf8(text);
		pango_layout_set_text(layout, valid.c_str(), static_cast<int>(valid.size()));
		pango_layout_set_single_paragraph_mode(layout, TRUE);
		return layout;
	}

	std::string description;
	PangoFontDescription* font = nullptr;
	PangoContext* context = nullptr;
};

SystemFont::SystemFont(std::string description) : impl_(std::make_unique<Impl>(std::move(description))) {}
SystemFont::~SystemFont() = default;
SystemFont::SystemFont(SystemFont&&) noexcept = default;
SystemFont& SystemFont::operator=(SystemFont&&) noexcept = default;

int SystemFont::TextWidth(std::string_view text, int scale) const {
	if (text.empty() || scale <= 0) return 0;
	PangoLayout* layout = impl_->Layout(text, scale);
	int width = 0;
	pango_layout_get_pixel_size(layout, &width, nullptr);
	g_object_unref(layout);
	return width;
}

int SystemFont::LineHeight(int scale) const {
	if (scale <= 0) return 0;
	PangoLayout* layout = impl_->Layout("Mg", scale);
	int height = 0;
	pango_layout_get_pixel_size(layout, nullptr, &height);
	g_object_unref(layout);
	return height;
}

RasterizedText SystemFont::Rasterize(std::string_view text, int scale) const {
	RasterizedText result;
	if (text.empty() || scale <= 0) return result;
	PangoLayout* layout = impl_->Layout(text, scale);
	PangoRectangle ink{};
	PangoRectangle logical{};
	pango_layout_get_pixel_extents(layout, &ink, &logical);
	const int left = std::min(0, ink.x);
	const int top = std::min(0, ink.y);
	const int right = std::max(logical.width, ink.x + ink.width);
	const int bottom = std::max(logical.height, ink.y + ink.height);
	result.width = std::max(0, right - left);
	result.height = std::max(0, bottom - top);
	result.offsetX = left;
	result.offsetY = top;
	if (result.width == 0 || result.height == 0) {
		g_object_unref(layout);
		return result;
	}

	cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, result.width, result.height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		g_object_unref(layout);
		return {};
	}
	cairo_t* cairo = cairo_create(surface);
	if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
		cairo_destroy(cairo);
		cairo_surface_destroy(surface);
		g_object_unref(layout);
		return {};
	}
	cairo_set_source_rgba(cairo, 1.0, 1.0, 1.0, 1.0);
	cairo_move_to(cairo, -left, -top);
	pango_cairo_show_layout(cairo, layout);
	cairo_destroy(cairo);
	cairo_surface_flush(surface);

	const unsigned char* data = cairo_image_surface_get_data(surface);
	const int stride = cairo_image_surface_get_stride(surface);
	result.argb.resize(static_cast<std::size_t>(result.width) * result.height);
	for (int y = 0; y < result.height; ++y) {
		for (int x = 0; x < result.width; ++x) {
			const std::uint32_t alpha = data[y * stride + x];
			result.argb[static_cast<std::size_t>(y) * result.width + x] =
				(alpha << 24) | 0x00ffffffu;
		}
	}
	cairo_surface_destroy(surface);
	g_object_unref(layout);
	return result;
}

const std::string& SystemFont::Description() const {
	return impl_->description;
}

} // namespace jpegview_linux
