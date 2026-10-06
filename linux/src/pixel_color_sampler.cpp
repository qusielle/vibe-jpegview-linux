#include "pixel_color_sampler.h"

#include <algorithm>
#include <limits>

namespace jpegview_linux {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

void AppendHexByte(std::string& output, std::uint8_t value) {
	output.push_back(kHexDigits[value >> 4]);
	output.push_back(kHexDigits[value & 0x0f]);
}

} // namespace

bool Contains(const PixelColorSamplerRect& rect, int x, int y) {
	return rect.width > 0 && rect.height > 0 && x >= rect.x && y >= rect.y &&
		x < rect.x + rect.width && y < rect.y + rect.height;
}

void PixelColorSamplerModel::Update(const PixelColorSamplerInput& input) {
	if (!input.enabled) {
		Clear();
		return;
	}
	const auto sameRect = [](const PixelColorSamplerRect& left,
		const PixelColorSamplerRect& right) {
		return left.x == right.x && left.y == right.y &&
			left.width == right.width && left.height == right.height;
	};
	const bool sourceChanged = !cacheValid_ ||
		cachedOwnerGeneration_ != input.ownerGeneration ||
		cachedDocumentRevision_ != input.documentRevision ||
		cachedFrameIndex_ != input.frameIndex ||
		cachedPixelOwner_ != input.pixelOwner ||
		cachedImageWidth_ != input.imageWidth ||
		cachedImageHeight_ != input.imageHeight;
	const bool geometryChanged = !cacheValid_ ||
		!sameRect(cachedImageArea_, input.imageArea) ||
		!sameRect(cachedDestination_, input.destination) ||
		cachedWindowWidth_ != input.windowWidth ||
		cachedWindowHeight_ != input.windowHeight ||
		cachedLabelWidth_ != input.labelWidth ||
		cachedLineHeight_ != input.lineHeight;
	if (plan_.pinned && (sourceChanged || geometryChanged)) plan_.pinned = false;
	const bool pointerChanged = cachedPointerX_ != input.pointerX ||
		cachedPointerY_ != input.pointerY;
	if (plan_.pinned || (!sourceChanged && !geometryChanged && !pointerChanged)) return;

	plan_.color.reset();
	plan_.hex.clear();
	plan_.label.clear();
	plan_.panel = {};
	plan_.swatch = {};
	if (input.bgra != nullptr && input.imageWidth > 0 && input.imageHeight > 0 &&
		input.pixelX >= 0 && input.pixelY >= 0) {
		plan_.color = SampleBgraPixel(*input.bgra, input.imageWidth, input.imageHeight,
			input.pixelX, input.pixelY);
	}
	if (plan_.color.has_value()) {
		plan_.hex = FormatPixelColorRgba(*plan_.color);
		plan_.label = "DOC " + plan_.hex;
		constexpr int padding = 6;
		constexpr int swatchSize = 14;
		constexpr int swatchGap = 8;
		const int width = padding * 2 + swatchSize + swatchGap + input.labelWidth;
		const int height = std::max(26, input.lineHeight + padding * 2);
		int x = input.pointerX + 16;
		int y = input.pointerY + 16;
		if (x + width > input.windowWidth - 4) x = input.pointerX - width - 12;
		if (y + height > input.windowHeight - 4) y = input.pointerY - height - 12;
		x = std::clamp(x, 4, std::max(4, input.windowWidth - width - 4));
		y = std::clamp(y, 4, std::max(4, input.windowHeight - height - 4));
		plan_.panel = {x, y, width, height};
		plan_.swatch = {x + padding, y + (height - swatchSize) / 2,
			swatchSize, swatchSize};
	}
	cachedOwnerGeneration_ = input.ownerGeneration;
	cachedDocumentRevision_ = input.documentRevision;
	cachedFrameIndex_ = input.frameIndex;
	cachedPixelOwner_ = input.pixelOwner;
	cachedImageWidth_ = input.imageWidth;
	cachedImageHeight_ = input.imageHeight;
	cachedImageArea_ = input.imageArea;
	cachedDestination_ = input.destination;
	cachedWindowWidth_ = input.windowWidth;
	cachedWindowHeight_ = input.windowHeight;
	cachedLabelWidth_ = input.labelWidth;
	cachedLineHeight_ = input.lineHeight;
	cachedPointerX_ = input.pointerX;
	cachedPointerY_ = input.pointerY;
	cacheValid_ = true;
}

void PixelColorSamplerModel::PointerMoved(int x, int y) {
	plan_.pinned = !plan_.hex.empty() && Contains(plan_.panel, x, y);
}

std::optional<std::string> PixelColorSamplerModel::CopyTextAt(int x, int y) const {
	if (plan_.hex.empty() || !Contains(plan_.panel, x, y)) return std::nullopt;
	return plan_.hex;
}

void PixelColorSamplerModel::Clear() {
	plan_ = {};
	cacheValid_ = false;
	cachedOwnerGeneration_ = 0;
	cachedDocumentRevision_ = 0;
	cachedFrameIndex_ = 0;
	cachedPixelOwner_ = nullptr;
	cachedImageWidth_ = 0;
	cachedImageHeight_ = 0;
	cachedImageArea_ = {};
	cachedDestination_ = {};
	cachedWindowWidth_ = 0;
	cachedWindowHeight_ = 0;
	cachedLabelWidth_ = 0;
	cachedLineHeight_ = 0;
	cachedPointerX_ = -1;
	cachedPointerY_ = -1;
}

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
