#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jpegview_linux {

// Resolve the desktop UI font without depending on a particular toolkit. XFCE
// persists Gtk/FontName in xsettings.xml; the other files cover the common GTK
// and KDE equivalents. The returned value is a Pango font description.
std::string ResolveDesktopFontDescription(const std::filesystem::path& home,
	const std::filesystem::path& configHome);

struct RasterizedText {
	int width = 0;
	int height = 0;
	int offsetX = 0;
	int offsetY = 0;
	std::vector<std::uint32_t> argb;
};

class SystemFont {
public:
	explicit SystemFont(std::string description = {});
	~SystemFont();

	SystemFont(const SystemFont&) = delete;
	SystemFont& operator=(const SystemFont&) = delete;
	SystemFont(SystemFont&&) noexcept;
	SystemFont& operator=(SystemFont&&) noexcept;

	int TextWidth(std::string_view text, int scale = 1) const;
	int LineHeight(int scale = 1) const;
	RasterizedText Rasterize(std::string_view text, int scale = 1) const;
	const std::string& Description() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
