#pragma once

#include <cstddef>

namespace jpegview_linux {

// Geometry-independent picture adjustments exposed by JPEGView's levels panel.
// Defaults are identity values; apply these to a fresh decoded image for each preview.
struct ImageProcessingParams {
	double contrast = 0.0;
	double gamma = 1.0;
	double saturation = 1.0;
	double cyanRed = 0.0;
	double magentaGreen = 0.0;
	double yellowBlue = 0.0;
	double lightenShadows = 0.0;
	double darkenHighlights = 0.0;
	double deepShadows = 0.0;
	double colorCorrection = 0.0;
	double contrastCorrection = 0.0;
	double sharpen = 0.0;
	// Unsharp mask is a separate modal operation rather than a levels slider.
	double unsharpRadius = 1.0;
	double unsharpAmount = 0.0;
	double unsharpThreshold = 4.0;
	bool localDensityEnabled = false;
};

struct ImageProcessingPreset {
	ImageProcessingParams processing;
	bool autoContrast = false;
};

enum class LevelControl : std::size_t {
	Contrast,
	Brightness,
	Saturation,
	CyanRed,
	MagentaGreen,
	YellowBlue,
	LightenShadows,
	DarkenHighlights,
	DeepShadows,
	ColorCorrection,
	ContrastCorrection,
	Sharpen,
	Count,
};

struct LevelControlInfo {
	LevelControl control;
	const char* label;
	double minimum;
	double maximum;
	double defaultValue;
	bool enabledByLocalDensity;
	bool enabledByAutoContrast;
};

const LevelControlInfo& GetLevelControlInfo(LevelControl control);
double GetLevelControlValue(const ImageProcessingParams& params, LevelControl control);
void SetLevelControlValue(ImageProcessingParams& params, LevelControl control, double value);
double LevelControlValueAtPosition(LevelControl control, double position);
double LevelControlPosition(const ImageProcessingParams& params, LevelControl control);
bool IsDefaultImageProcessing(const ImageProcessingParams& params);
bool EqualImageProcessing(const ImageProcessingParams& left,
	const ImageProcessingParams& right);
ImageProcessingPreset ResolveImageProcessingForFile(const ImageProcessingPreset& current,
	const ImageProcessingPreset* saved, bool keepCurrent, bool defaultAutoContrast);

} // namespace jpegview_linux
