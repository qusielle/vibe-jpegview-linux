#include "magnifying_glass_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jpegview_linux {
namespace {

int SaturatedFloor(double value) {
	const double bounded = std::max(static_cast<double>(std::numeric_limits<int>::min()),
		std::min(static_cast<double>(std::numeric_limits<int>::max()), value));
	return static_cast<int>(std::floor(bounded));
}

int SaturatedCeil(double value) {
	const double bounded = std::max(static_cast<double>(std::numeric_limits<int>::min()),
		std::min(static_cast<double>(std::numeric_limits<int>::max()), value));
	return static_cast<int>(std::ceil(bounded));
}

int SaturatedRound(double value) {
	const double bounded = std::max(static_cast<double>(std::numeric_limits<int>::min()),
		std::min(static_cast<double>(std::numeric_limits<int>::max()), value));
	return static_cast<int>(std::round(bounded));
}

int CenteredOrigin(double center, int extent) {
	return SaturatedRound(center - static_cast<double>(extent) / 2.0);
}

struct AxisCrop {
	int sourceStart = 0;
	int sourceExtent = 0;
	int contentOffset = 0;
	int contentExtent = 0;
};

AxisCrop CalculateAxisCrop(double sourceCenter, double requestedExtent, int textureExtent,
	int outputExtent) {
	AxisCrop result;
	const int requestedStart = SaturatedFloor(sourceCenter - requestedExtent / 2.0);
	const int requestedEnd = SaturatedCeil(sourceCenter + requestedExtent / 2.0);
	if (requestedEnd <= requestedStart) return result;

	const int clippedStart = std::max(0, requestedStart);
	const int clippedEnd = std::min(textureExtent, requestedEnd);
	if (clippedEnd <= clippedStart) return result;

	const int requestedWidth = requestedEnd - requestedStart;
	if (requestedWidth <= 0) return result;
	const double outputPerSource = static_cast<double>(outputExtent) / requestedWidth;
	result.sourceStart = clippedStart;
	result.sourceExtent = clippedEnd - clippedStart;
	result.contentOffset = std::clamp(
		SaturatedRound((clippedStart - requestedStart) * outputPerSource), 0, outputExtent);
	result.contentExtent = std::clamp(SaturatedRound(result.sourceExtent * outputPerSource),
		0, outputExtent - result.contentOffset);
	return result;
}

} // namespace

int MagnifyingGlassModel::MaximumDimension(int parentDimension, int minimum) {
	if (parentDimension <= 0) return std::numeric_limits<int>::max();
	const int ninetyPercent = static_cast<int>(std::floor(parentDimension * 0.9));
	return std::max(minimum, ninetyPercent);
}

void MagnifyingGlassModel::ConstrainSize(int parentWidth, int parentHeight) {
	width_ = std::clamp(width_, kMinimumWidth, MaximumDimension(parentWidth, kMinimumWidth));
	height_ = std::clamp(height_, kMinimumHeight, MaximumDimension(parentHeight, kMinimumHeight));
}

void MagnifyingGlassModel::HandleWheel(MagnifyingGlassWheelDirection direction,
	const MagnifyingGlassWheelModifiers& modifiers, int parentWidth, int parentHeight) {
	if (direction != MagnifyingGlassWheelDirection::Up &&
		direction != MagnifyingGlassWheelDirection::Down) return;

	ConstrainSize(parentWidth, parentHeight);
	const int step = direction == MagnifyingGlassWheelDirection::Down ? 1 : -1;
	if (modifiers.shift) {
		zoomLevel_ = std::clamp(zoomLevel_ + step * 0.025,
			kMinimumZoomLevel, kMaximumZoomLevel);
		return;
	}

	if (modifiers.alt || modifiers.control) {
		if (modifiers.alt) width_ += step * 30;
		if (modifiers.control) height_ += step * 15;
	} else {
		width_ += step * 30;
		height_ += step * 15;
	}
	ConstrainSize(parentWidth, parentHeight);
}

MagnifyingGlassGeometry CalculateMagnifyingGlassGeometry(double pointerX, double pointerY,
	const MagnifyingGlassRect& displayedImage, int textureWidth, int textureHeight,
	int lensWidth, int lensHeight, double zoomLevel) {
	MagnifyingGlassGeometry result;
	if (displayedImage.width <= 0 || displayedImage.height <= 0 || textureWidth <= 0 ||
		textureHeight <= 0 || lensWidth <= 0 || lensHeight <= 0 || !std::isfinite(pointerX) ||
		!std::isfinite(pointerY) || !std::isfinite(zoomLevel) ||
		zoomLevel < MagnifyingGlassModel::kMinimumZoomLevel ||
		zoomLevel > MagnifyingGlassModel::kMaximumZoomLevel ||
		pointerX < displayedImage.x || pointerY < displayedImage.y ||
		pointerX >= static_cast<double>(displayedImage.x) + displayedImage.width ||
		pointerY >= static_cast<double>(displayedImage.y) + displayedImage.height) {
		return result;
	}

	result.lensRect = {CenteredOrigin(pointerX, lensWidth), CenteredOrigin(pointerY, lensHeight),
		lensWidth, lensHeight};

	const double sourceCenterX = (pointerX - displayedImage.x) * textureWidth / displayedImage.width;
	const double sourceCenterY = (pointerY - displayedImage.y) * textureHeight / displayedImage.height;
	const double requestedWidth = lensWidth * textureWidth * zoomLevel / displayedImage.width;
	const double requestedHeight = lensHeight * textureHeight * zoomLevel / displayedImage.height;
	const AxisCrop horizontal = CalculateAxisCrop(sourceCenterX, requestedWidth,
		textureWidth, lensWidth);
	const AxisCrop vertical = CalculateAxisCrop(sourceCenterY, requestedHeight,
		textureHeight, lensHeight);
	if (horizontal.sourceExtent <= 0 || vertical.sourceExtent <= 0 ||
		horizontal.contentExtent <= 0 || vertical.contentExtent <= 0) {
		return {};
	}

	result.sourceRect = {horizontal.sourceStart, vertical.sourceStart,
		horizontal.sourceExtent, vertical.sourceExtent};
	result.contentDestinationRect = {
		result.lensRect.x + horizontal.contentOffset,
		result.lensRect.y + vertical.contentOffset,
		horizontal.contentExtent,
		vertical.contentExtent};
	result.valid = true;
	return result;
}

} // namespace jpegview_linux
