#include "app_icon.h"

#include "jpegview_icon_data.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace jpegview_linux {
namespace {

bool CanRead(std::size_t offset, std::size_t length) {
	return offset <= kApplicationIconIcoSize && length <= kApplicationIconIcoSize - offset;
}

std::uint16_t Read16(std::size_t offset) {
	return static_cast<std::uint16_t>(kApplicationIconIco[offset]) |
		(static_cast<std::uint16_t>(kApplicationIconIco[offset + 1]) << 8);
}

std::uint32_t Read32(std::size_t offset) {
	return static_cast<std::uint32_t>(kApplicationIconIco[offset]) |
		(static_cast<std::uint32_t>(kApplicationIconIco[offset + 1]) << 8) |
		(static_cast<std::uint32_t>(kApplicationIconIco[offset + 2]) << 16) |
		(static_cast<std::uint32_t>(kApplicationIconIco[offset + 3]) << 24);
}

} // namespace

bool DecodeApplicationIcon(ApplicationIcon& icon, std::string& errorMessage) {
	icon = {};
	errorMessage.clear();
	if (!CanRead(0, 6) || Read16(0) != 0 || Read16(2) != 1) {
		errorMessage = "invalid embedded ICO header";
		return false;
	}
	const std::size_t imageCount = Read16(4);
	if (imageCount == 0 || !CanRead(6, imageCount * 16)) {
		errorMessage = "invalid embedded ICO directory";
		return false;
	}

	std::size_t selectedOffset = 0;
	std::size_t selectedSize = 0;
	int selectedWidth = 0;
	int selectedHeight = 0;
	for (std::size_t index = 0; index < imageCount; ++index) {
		const std::size_t entry = 6 + index * 16;
		const int width = kApplicationIconIco[entry] == 0 ? 256 : kApplicationIconIco[entry];
		const int height = kApplicationIconIco[entry + 1] == 0 ? 256 : kApplicationIconIco[entry + 1];
		const std::size_t size = Read32(entry + 8);
		const std::size_t offset = Read32(entry + 12);
		if (!CanRead(offset, size) || width * height <= selectedWidth * selectedHeight) continue;
		selectedOffset = offset;
		selectedSize = size;
		selectedWidth = width;
		selectedHeight = height;
	}
	if (selectedSize == 0 || !CanRead(selectedOffset, 40)) {
		errorMessage = "embedded ICO has no usable image";
		return false;
	}

	const std::size_t headerSize = Read32(selectedOffset);
	const int dibWidth = static_cast<int>(Read32(selectedOffset + 4));
	const int dibHeight = static_cast<int>(Read32(selectedOffset + 8));
	const int bitsPerPixel = Read16(selectedOffset + 14);
	const std::uint32_t compression = Read32(selectedOffset + 16);
	if (headerSize < 40 || headerSize > selectedSize || dibWidth != selectedWidth ||
		dibHeight != selectedHeight * 2 || (bitsPerPixel != 24 && bitsPerPixel != 32) || compression != 0) {
		errorMessage = "embedded ICO frame uses an unsupported bitmap format";
		return false;
	}

	const std::size_t colorStride =
		(static_cast<std::size_t>(selectedWidth) * bitsPerPixel + 31) / 32 * 4;
	const std::size_t maskStride = (static_cast<std::size_t>(selectedWidth) + 31) / 32 * 4;
	const std::size_t colorOffset = selectedOffset + headerSize;
	const std::size_t colorBytes = colorStride * static_cast<std::size_t>(selectedHeight);
	const std::size_t maskOffset = colorOffset + colorBytes;
	const std::size_t maskBytes = maskStride * static_cast<std::size_t>(selectedHeight);
	if (!CanRead(colorOffset, colorBytes) || !CanRead(maskOffset, maskBytes) ||
		static_cast<std::size_t>(selectedWidth) > std::numeric_limits<std::size_t>::max() /
			(static_cast<std::size_t>(selectedHeight) * 4)) {
		errorMessage = "embedded ICO frame is truncated";
		return false;
	}

	icon.width = selectedWidth;
	icon.height = selectedHeight;
	icon.bgra.resize(static_cast<std::size_t>(selectedWidth) * selectedHeight * 4);
	const std::size_t bytesPerPixel = static_cast<std::size_t>(bitsPerPixel / 8);
	for (int y = 0; y < selectedHeight; ++y) {
		const int sourceY = selectedHeight - y - 1;
		for (int x = 0; x < selectedWidth; ++x) {
			const std::size_t source = colorOffset + static_cast<std::size_t>(sourceY) * colorStride +
				static_cast<std::size_t>(x) * bytesPerPixel;
			const std::size_t target = (static_cast<std::size_t>(y) * selectedWidth + x) * 4;
			icon.bgra[target] = kApplicationIconIco[source];
			icon.bgra[target + 1] = kApplicationIconIco[source + 1];
			icon.bgra[target + 2] = kApplicationIconIco[source + 2];
			const std::size_t mask = maskOffset + static_cast<std::size_t>(sourceY) * maskStride + x / 8;
			const bool transparent = (kApplicationIconIco[mask] & (0x80u >> (x % 8))) != 0;
			const std::uint8_t embeddedAlpha = bitsPerPixel == 32 ? kApplicationIconIco[source + 3] : 255;
			icon.bgra[target + 3] = transparent ? 0 : embeddedAlpha;
		}
	}
	return true;
}

} // namespace jpegview_linux
