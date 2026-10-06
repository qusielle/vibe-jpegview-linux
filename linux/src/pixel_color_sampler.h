#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jpegview_linux {

struct PixelColorRgba {
	std::uint8_t red = 0;
	std::uint8_t green = 0;
	std::uint8_t blue = 0;
	std::uint8_t alpha = 0;
};

struct PixelColorSamplerRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

bool Contains(const PixelColorSamplerRect& rect, int x, int y);

struct PixelColorSamplerInput {
	bool enabled = false;
	std::uint64_t ownerGeneration = 0;
	std::uint64_t documentRevision = 0;
	std::size_t frameIndex = 0;
	const void* pixelOwner = nullptr;
	const std::vector<std::uint8_t>* bgra = nullptr;
	int imageWidth = 0;
	int imageHeight = 0;
	int pixelX = -1;
	int pixelY = -1;
	int pointerX = -1;
	int pointerY = -1;
	int windowWidth = 0;
	int windowHeight = 0;
	int labelWidth = 0;
	int lineHeight = 0;
	PixelColorSamplerRect imageArea;
	PixelColorSamplerRect destination;
};

struct PixelColorSamplerDecodeDemand {
	bool ownerCommitted = false;
	bool postCommitPointerMotion = false;
	bool pointerOverImage = false;
	bool pointerButtonsDown = false;
	bool hasSampleablePixels = false;
	bool requestPending = false;
	bool requestFailed = false;
	std::uint32_t currentPointerButtons = 0;
};

// Full-resolution source decoding is optional work. Admit it only for an
// explicit idle hover over the committed image when no usable pixels exist.
// The current button mask guards against stationary presses after motion.
bool ShouldDecodePixelColorSamplerSource(const PixelColorSamplerDecodeDemand& demand);

struct PixelColorSamplerPaintPlan {
	PixelColorSamplerRect panel;
	PixelColorSamplerRect swatch;
	std::optional<PixelColorRgba> color;
	std::string hex;
	std::string label;
	bool pinned = false;
};

// Owns the renderer-independent sampling cache and click-to-pin interaction.
// Update this during the event-thread update phase; rendering consumes only the
// resulting immutable paint plan.
class PixelColorSamplerModel {
public:
	void Update(const PixelColorSamplerInput& input);
	void PointerMoved(int x, int y);
	std::optional<std::string> CopyTextAt(int x, int y) const;
	const PixelColorSamplerPaintPlan& PaintPlan() const { return plan_; }
	void Clear();

private:
	PixelColorSamplerPaintPlan plan_;
	bool cacheValid_ = false;
	std::uint64_t cachedOwnerGeneration_ = 0;
	std::uint64_t cachedDocumentRevision_ = 0;
	std::size_t cachedFrameIndex_ = 0;
	const void* cachedPixelOwner_ = nullptr;
	int cachedImageWidth_ = 0;
	int cachedImageHeight_ = 0;
	PixelColorSamplerRect cachedImageArea_;
	PixelColorSamplerRect cachedDestination_;
	int cachedWindowWidth_ = 0;
	int cachedWindowHeight_ = 0;
	int cachedLabelWidth_ = 0;
	int cachedLineHeight_ = 0;
	int cachedPointerX_ = -1;
	int cachedPointerY_ = -1;
};

// Reads one top-to-bottom BGRA8 document pixel and returns channels in RGBA order.
std::optional<PixelColorRgba> SampleBgraPixel(const std::vector<std::uint8_t>& bgra,
	int width, int height, int x, int y);

// Formats the color as CSS-style hexadecimal in #RRGGBBAA order.
std::string FormatPixelColorRgba(const PixelColorRgba& color);

} // namespace jpegview_linux
