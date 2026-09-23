#include "image_processing.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace jpegview_linux {
namespace {

constexpr LevelControlInfo kControls[] = {
	{LevelControl::Contrast, "Contrast", -0.5, 0.5, 0.0, false, false},
	{LevelControl::Brightness, "Brightness", 0.5, 2.0, 1.0, false, false},
	{LevelControl::Saturation, "Saturation", 0.0, 2.0, 1.0, false, false},
	{LevelControl::CyanRed, "Cyan - Red", -1.0, 1.0, 0.0, false, false},
	{LevelControl::MagentaGreen, "Magenta - Green", -1.0, 1.0, 0.0, false, false},
	{LevelControl::YellowBlue, "Yellow - Blue", -1.0, 1.0, 0.0, false, false},
	{LevelControl::LightenShadows, "Lighten Shadows", 0.0, 1.0, 0.0, true, false},
	{LevelControl::DarkenHighlights, "Darken Highlights", 0.0, 1.0, 0.0, true, false},
	{LevelControl::DeepShadows, "Deep Shadows", 0.0, 1.0, 0.0, true, false},
	{LevelControl::ColorCorrection, "Color Correction", -0.5, 0.5, 0.0, false, true},
	{LevelControl::ContrastCorrection, "Contrast Correction", 0.0, 1.0, 0.0, false, true},
	{LevelControl::Sharpen, "Sharpen", 0.0, 0.5, 0.0, false, false},
};

constexpr double kEqualityEpsilon = 1e-9;

} // namespace

const LevelControlInfo& GetLevelControlInfo(LevelControl control) {
	const std::size_t index = static_cast<std::size_t>(control);
	if (index >= sizeof(kControls) / sizeof(kControls[0])) {
		throw std::out_of_range("invalid picture-level control");
	}
	return kControls[index];
}

double GetLevelControlValue(const ImageProcessingParams& params, LevelControl control) {
	switch (control) {
	case LevelControl::Contrast: return params.contrast;
	case LevelControl::Brightness: return params.gamma;
	case LevelControl::Saturation: return params.saturation;
	case LevelControl::CyanRed: return params.cyanRed;
	case LevelControl::MagentaGreen: return params.magentaGreen;
	case LevelControl::YellowBlue: return params.yellowBlue;
	case LevelControl::LightenShadows: return params.lightenShadows;
	case LevelControl::DarkenHighlights: return params.darkenHighlights;
	case LevelControl::DeepShadows: return params.deepShadows;
	case LevelControl::ColorCorrection: return params.colorCorrection;
	case LevelControl::ContrastCorrection: return params.contrastCorrection;
	case LevelControl::Sharpen: return params.sharpen;
	case LevelControl::Count: break;
	}
	throw std::out_of_range("invalid picture-level control");
}

void SetLevelControlValue(ImageProcessingParams& params, LevelControl control, double value) {
	const LevelControlInfo& info = GetLevelControlInfo(control);
	if (!std::isfinite(value)) value = info.defaultValue;
	value = std::clamp(value, info.minimum, info.maximum);
	switch (control) {
	case LevelControl::Contrast: params.contrast = value; break;
	case LevelControl::Brightness: params.gamma = value; break;
	case LevelControl::Saturation: params.saturation = value; break;
	case LevelControl::CyanRed: params.cyanRed = value; break;
	case LevelControl::MagentaGreen: params.magentaGreen = value; break;
	case LevelControl::YellowBlue: params.yellowBlue = value; break;
	case LevelControl::LightenShadows: params.lightenShadows = value; break;
	case LevelControl::DarkenHighlights: params.darkenHighlights = value; break;
	case LevelControl::DeepShadows: params.deepShadows = value; break;
	case LevelControl::ColorCorrection: params.colorCorrection = value; break;
	case LevelControl::ContrastCorrection: params.contrastCorrection = value; break;
	case LevelControl::Sharpen: params.sharpen = value; break;
	case LevelControl::Count: throw std::out_of_range("invalid picture-level control");
	}
}

double LevelControlValueAtPosition(LevelControl control, double position) {
	const LevelControlInfo& info = GetLevelControlInfo(control);
	if (!std::isfinite(position)) position = 0.5;
	position = std::clamp(position, 0.0, 1.0);
	if (control == LevelControl::Brightness) {
		return std::exp(std::log(info.maximum) - position *
			(std::log(info.maximum) - std::log(info.minimum)));
	}
	return info.minimum + position * (info.maximum - info.minimum);
}

double LevelControlPosition(const ImageProcessingParams& params, LevelControl control) {
	const LevelControlInfo& info = GetLevelControlInfo(control);
	const double value = std::clamp(GetLevelControlValue(params, control), info.minimum, info.maximum);
	if (control == LevelControl::Brightness) {
		return (std::log(info.maximum) - std::log(value)) /
			(std::log(info.maximum) - std::log(info.minimum));
	}
	return (value - info.minimum) / (info.maximum - info.minimum);
}

bool IsDefaultImageProcessing(const ImageProcessingParams& params) {
	for (std::size_t index = 0; index < static_cast<std::size_t>(LevelControl::Count); ++index) {
		const LevelControl control = static_cast<LevelControl>(index);
		if (std::abs(GetLevelControlValue(params, control) -
			GetLevelControlInfo(control).defaultValue) > kEqualityEpsilon) return false;
	}
	return !params.localDensityEnabled && std::abs(params.unsharpAmount) <= kEqualityEpsilon &&
		std::abs(params.unsharpRadius - 1.0) <= kEqualityEpsilon &&
		std::abs(params.unsharpThreshold - 4.0) <= kEqualityEpsilon;
}

bool EqualImageProcessing(const ImageProcessingParams& left,
	const ImageProcessingParams& right) {
	if (left.localDensityEnabled != right.localDensityEnabled) return false;
	for (std::size_t index = 0; index < static_cast<std::size_t>(LevelControl::Count); ++index) {
		const LevelControl control = static_cast<LevelControl>(index);
		if (std::abs(GetLevelControlValue(left, control) -
			GetLevelControlValue(right, control)) > kEqualityEpsilon) return false;
	}
	return std::abs(left.unsharpRadius - right.unsharpRadius) <= kEqualityEpsilon &&
		std::abs(left.unsharpAmount - right.unsharpAmount) <= kEqualityEpsilon &&
		std::abs(left.unsharpThreshold - right.unsharpThreshold) <= kEqualityEpsilon;
}

ImageProcessingPreset ResolveImageProcessingForFile(const ImageProcessingPreset& current,
	const ImageProcessingPreset* saved, bool keepCurrent, bool defaultAutoContrast,
	const ImageProcessingParams& defaultProcessing) {
	if (keepCurrent) return current;
	return saved == nullptr ? ImageProcessingPreset{defaultProcessing, defaultAutoContrast} : *saved;
}

} // namespace jpegview_linux
