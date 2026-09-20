#include "system_font.h"
#include "bitmap_font.h"

#include <pango/pangoft2.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr const char* kFallbackFont = "Sans 10";

RasterizedText RasterizeTerminus9(std::string_view text, int scale) {
	RasterizedText result;
	if (text.empty() || scale <= 0 || !Terminus9CanRender(text)) return result;
	int cursorX = 0;
	int left = 0;
	int right = 0;
	for (unsigned char character : text) {
		const BitmapFontGlyph& glyph = Terminus9Glyph(character);
		const int glyphX = cursorX + glyph.bearingLeft * scale;
		left = std::min(left, glyphX);
		right = std::max(right, glyphX + glyph.width * scale);
		cursorX += glyph.advance * scale;
		right = std::max(right, cursorX);
	}
	result.width = right - left;
	result.height = Terminus9LineHeight() * scale;
	result.offsetX = left;
	if (result.width <= 0 || result.height <= 0) return {};
	result.argb.assign(static_cast<std::size_t>(result.width) * result.height, 0x00ffffffu);

	cursorX = 0;
	for (unsigned char character : text) {
		const BitmapFontGlyph& glyph = Terminus9Glyph(character);
		const std::uint8_t* pixels = Terminus9GlyphPixels(glyph);
		const int glyphX = cursorX + glyph.bearingLeft * scale - left;
		const int glyphY = (Terminus9Ascent() - glyph.bearingTop) * scale;
		for (int row = 0; row < glyph.height; ++row) {
			for (int column = 0; column < glyph.width; ++column) {
				if (pixels[row * glyph.width + column] == 0) continue;
				for (int scaleY = 0; scaleY < scale; ++scaleY) {
					for (int scaleX = 0; scaleX < scale; ++scaleX) {
						const int x = glyphX + column * scale + scaleX;
						const int y = glyphY + row * scale + scaleY;
						if (x >= 0 && x < result.width && y >= 0 && y < result.height) {
							result.argb[static_cast<std::size_t>(y) * result.width + x] =
								0xffffffffu;
						}
					}
				}
			}
		}
		cursorX += glyph.advance * scale;
	}
	return result;
}

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

struct FontApi {
	FontApi() {
		handle = dlopen("libpangoft2-1.0.so.0", RTLD_LAZY | RTLD_LOCAL);
		if (handle == nullptr) return;
#define LOAD_FONT_SYMBOL(member, symbol) available = Load(member, #symbol) && available
		available = true;
		LOAD_FONT_SYMBOL(fontDescriptionCopy, pango_font_description_copy);
		LOAD_FONT_SYMBOL(fontDescriptionGetSize, pango_font_description_get_size);
		LOAD_FONT_SYMBOL(fontDescriptionGetSizeIsAbsolute, pango_font_description_get_size_is_absolute);
		LOAD_FONT_SYMBOL(fontDescriptionSetAbsoluteSize, pango_font_description_set_absolute_size);
		LOAD_FONT_SYMBOL(fontDescriptionSetSize, pango_font_description_set_size);
		LOAD_FONT_SYMBOL(fontDescriptionFromString, pango_font_description_from_string);
		LOAD_FONT_SYMBOL(fontDescriptionFree, pango_font_description_free);
		LOAD_FONT_SYMBOL(ft2FontMapNew, pango_ft2_font_map_new);
		LOAD_FONT_SYMBOL(ft2FontMapSetResolution, pango_ft2_font_map_set_resolution);
		LOAD_FONT_SYMBOL(fontMapCreateContext, pango_font_map_create_context);
		LOAD_FONT_SYMBOL(layoutNew, pango_layout_new);
		LOAD_FONT_SYMBOL(layoutSetFontDescription, pango_layout_set_font_description);
		LOAD_FONT_SYMBOL(layoutSetText, pango_layout_set_text);
		LOAD_FONT_SYMBOL(layoutSetSingleParagraphMode, pango_layout_set_single_paragraph_mode);
		LOAD_FONT_SYMBOL(layoutGetPixelSize, pango_layout_get_pixel_size);
		LOAD_FONT_SYMBOL(layoutGetPixelExtents, pango_layout_get_pixel_extents);
		LOAD_FONT_SYMBOL(ft2RenderLayout, pango_ft2_render_layout);
		LOAD_FONT_SYMBOL(utf8Validate, g_utf8_validate);
		LOAD_FONT_SYMBOL(utf8MakeValid, g_utf8_make_valid);
		LOAD_FONT_SYMBOL(freeMemory, g_free);
		LOAD_FONT_SYMBOL(objectUnref, g_object_unref);
#undef LOAD_FONT_SYMBOL
		if (!available) {
			dlclose(handle);
			handle = nullptr;
		}
	}

	FontApi(const FontApi&) = delete;
	FontApi& operator=(const FontApi&) = delete;

	static FontApi& Instance() {
		static FontApi api;
		return api;
	}

	template<typename Function>
	bool Load(Function& function, const char* name) {
		void* symbol = dlsym(handle, name);
		static_assert(sizeof(function) == sizeof(symbol));
		std::memcpy(&function, &symbol, sizeof(symbol));
		return function != nullptr;
	}

	void* handle = nullptr;
	bool available = false;
	decltype(&pango_font_description_copy) fontDescriptionCopy = nullptr;
	decltype(&pango_font_description_get_size) fontDescriptionGetSize = nullptr;
	decltype(&pango_font_description_get_size_is_absolute) fontDescriptionGetSizeIsAbsolute = nullptr;
	decltype(&pango_font_description_set_absolute_size) fontDescriptionSetAbsoluteSize = nullptr;
	decltype(&pango_font_description_set_size) fontDescriptionSetSize = nullptr;
	decltype(&pango_font_description_from_string) fontDescriptionFromString = nullptr;
	decltype(&pango_font_description_free) fontDescriptionFree = nullptr;
	decltype(&pango_ft2_font_map_new) ft2FontMapNew = nullptr;
	decltype(&pango_ft2_font_map_set_resolution) ft2FontMapSetResolution = nullptr;
	decltype(&pango_font_map_create_context) fontMapCreateContext = nullptr;
	decltype(&pango_layout_new) layoutNew = nullptr;
	decltype(&pango_layout_set_font_description) layoutSetFontDescription = nullptr;
	decltype(&pango_layout_set_text) layoutSetText = nullptr;
	decltype(&pango_layout_set_single_paragraph_mode) layoutSetSingleParagraphMode = nullptr;
	decltype(&pango_layout_get_pixel_size) layoutGetPixelSize = nullptr;
	decltype(&pango_layout_get_pixel_extents) layoutGetPixelExtents = nullptr;
	decltype(&pango_ft2_render_layout) ft2RenderLayout = nullptr;
	decltype(&g_utf8_validate) utf8Validate = nullptr;
	decltype(&g_utf8_make_valid) utf8MakeValid = nullptr;
	decltype(&g_free) freeMemory = nullptr;
	decltype(&g_object_unref) objectUnref = nullptr;
};

std::string ValidUtf8(std::string_view text, const FontApi& api) {
	const std::string copy(text);
	if (api.utf8Validate(copy.c_str(), static_cast<gssize>(copy.size()), nullptr)) return copy;
	char* valid = api.utf8MakeValid(copy.c_str(), static_cast<gssize>(copy.size()));
	if (valid == nullptr) return {};
	std::string result(valid);
	api.freeMemory(valid);
	return result;
}

PangoFontDescription* ScaledDescription(const FontApi& api,
	const PangoFontDescription* source, int scale) {
	PangoFontDescription* result = api.fontDescriptionCopy(source);
	const int currentSize = api.fontDescriptionGetSize(result);
	const int baseSize = currentSize > 0 ? currentSize : 10 * PANGO_SCALE;
	if (api.fontDescriptionGetSizeIsAbsolute(result)) {
		api.fontDescriptionSetAbsoluteSize(result, static_cast<double>(baseSize) * scale);
	} else {
		api.fontDescriptionSetSize(result, baseSize * scale);
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
	}

	bool EnsureSystemFont() const {
		if (initializationAttempted) return layout != nullptr && font != nullptr;
		initializationAttempted = true;
		api = &FontApi::Instance();
		if (!api->available) return false;
		font = api->fontDescriptionFromString(description.c_str());
		if (font == nullptr) {
			description = kFallbackFont;
			font = api->fontDescriptionFromString(description.c_str());
		}
		if (font == nullptr) return false;
		map = api->ft2FontMapNew();
		if (map == nullptr) return false;
		api->ft2FontMapSetResolution(reinterpret_cast<PangoFT2FontMap*>(map), 96.0, 96.0);
		context = api->fontMapCreateContext(map);
		if (context == nullptr) return false;
		layout = api->layoutNew(context);
		return layout != nullptr;
	}

	~Impl() {
		if (api == nullptr || !api->available) return;
		if (layout != nullptr) api->objectUnref(layout);
		if (context != nullptr) api->objectUnref(context);
		if (map != nullptr) api->objectUnref(map);
		if (font != nullptr) api->fontDescriptionFree(font);
	}

	PangoLayout* Layout(std::string_view text, int scale) const {
		if (!EnsureSystemFont()) return nullptr;
		if (scale == 1) {
			api->layoutSetFontDescription(layout, font);
		} else {
			PangoFontDescription* scaled = ScaledDescription(*api, font, std::max(1, scale));
			api->layoutSetFontDescription(layout, scaled);
			api->fontDescriptionFree(scaled);
		}
		const std::string valid = ValidUtf8(text, *api);
		api->layoutSetText(layout, valid.c_str(), static_cast<int>(valid.size()));
		api->layoutSetSingleParagraphMode(layout, TRUE);
		return layout;
	}

	mutable bool initializationAttempted = false;
	mutable FontApi* api = nullptr;
	mutable std::string description;
	mutable PangoFontDescription* font = nullptr;
	mutable PangoFontMap* map = nullptr;
	mutable PangoContext* context = nullptr;
	mutable PangoLayout* layout = nullptr;
};

SystemFont::SystemFont(std::string description) : impl_(std::make_unique<Impl>(std::move(description))) {}
SystemFont::~SystemFont() = default;
SystemFont::SystemFont(SystemFont&&) noexcept = default;
SystemFont& SystemFont::operator=(SystemFont&&) noexcept = default;

int SystemFont::TextWidth(std::string_view text, int scale) const {
	if (text.empty() || scale <= 0) return 0;
	if (Terminus9CanRender(text)) return Terminus9TextWidth(text, scale);
	PangoLayout* layout = impl_->Layout(text, scale);
	if (layout == nullptr) return 0;
	int width = 0;
	impl_->api->layoutGetPixelSize(layout, &width, nullptr);
	return width;
}

int SystemFont::LineHeight(int scale) const {
	if (scale <= 0) return 0;
	return Terminus9LineHeight() * scale;
}

RasterizedText SystemFont::Rasterize(std::string_view text, int scale) const {
	RasterizedText result;
	if (text.empty() || scale <= 0) return result;
	if (Terminus9CanRender(text)) return RasterizeTerminus9(text, scale);
	PangoLayout* layout = impl_->Layout(text, scale);
	if (layout == nullptr) return result;
	PangoRectangle ink{};
	PangoRectangle logical{};
	impl_->api->layoutGetPixelExtents(layout, &ink, &logical);
	const int left = std::min(0, ink.x);
	const int top = std::min(0, ink.y);
	const int right = std::max(logical.width, ink.x + ink.width);
	const int bottom = std::max(logical.height, ink.y + ink.height);
	result.width = std::max(0, right - left);
	result.height = std::max(0, bottom - top);
	result.offsetX = left;
	result.offsetY = top;
	if (result.width == 0 || result.height == 0) return result;

	std::vector<unsigned char> alpha(static_cast<std::size_t>(result.width) * result.height);
	FT_Bitmap bitmap{};
	bitmap.rows = static_cast<unsigned int>(result.height);
	bitmap.width = static_cast<unsigned int>(result.width);
	bitmap.pitch = result.width;
	bitmap.buffer = alpha.data();
	bitmap.num_grays = 256;
	bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
	impl_->api->ft2RenderLayout(&bitmap, layout, -left, -top);

	result.argb.resize(static_cast<std::size_t>(result.width) * result.height);
	for (int y = 0; y < result.height; ++y) {
		for (int x = 0; x < result.width; ++x) {
			const std::uint32_t coverage = alpha[static_cast<std::size_t>(y) * result.width + x];
			result.argb[static_cast<std::size_t>(y) * result.width + x] =
				(coverage << 24) | 0x00ffffffu;
		}
	}
	return result;
}

const std::string& SystemFont::Description() const {
	return impl_->description;
}

} // namespace jpegview_linux
