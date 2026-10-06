#include "pixel_color_sampler.h"

#include <limits>

namespace jpegview_linux {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

void AppendHexByte(std::string& output, std::uint8_t value) {
	output.push_back(kHexDigits[value >> 4]);
	output.push_back(kHexDigits[value & 0x0f]);
}

} // namespace

std::optional<PixelColorRgba> SampleBgraPixel(const std::vector<std::uint8_t>& bgra,
	int width, int height, int x, int y) {
	if (width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height) {
		return std::nullopt;
	}
	const std::size_t imageWidth = static_cast<std::size_t>(width);
	const std::size_t imageHeight = static_cast<std::size_t>(height);
	if (imageWidth > std::numeric_limits<std::size_t>::max() / imageHeight) {
		return std::nullopt;
	}
	const std::size_t pixelCount = imageWidth * imageHeight;
	if (pixelCount > std::numeric_limits<std::size_t>::max() / 4 ||
		bgra.size() < pixelCount * 4) {
		return std::nullopt;
	}
	const std::size_t offset = (static_cast<std::size_t>(y) * imageWidth +
		static_cast<std::size_t>(x)) * 4;
	return PixelColorRgba{bgra[offset + 2], bgra[offset + 1], bgra[offset],
		bgra[offset + 3]};
}

std::string FormatPixelColorRgba(const PixelColorRgba& color) {
	std::string result;
	result.reserve(9);
	result.push_back('#');
	AppendHexByte(result, color.red);
	AppendHexByte(result, color.green);
	AppendHexByte(result, color.blue);
	AppendHexByte(result, color.alpha);
	return result;
}

} // namespace jpegview_linux
