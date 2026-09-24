#include "crop_selection_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jpegview_linux {
namespace {

int RoundedPositive(double value) {
	if (std::isnan(value)) return 1;
	if (!std::isfinite(value)) return value > 0.0 ? std::numeric_limits<int>::max() : 1;
	const double bounded = std::clamp(value, 1.0,
		static_cast<double>(std::numeric_limits<int>::max()));
	return static_cast<int>(std::lround(bounded));
}

bool HasLeft(CropSelectionHandle handle) {
	return handle == CropSelectionHandle::Left || handle == CropSelectionHandle::TopLeft ||
		handle == CropSelectionHandle::BottomLeft;
}

bool HasRight(CropSelectionHandle handle) {
	return handle == CropSelectionHandle::Right || handle == CropSelectionHandle::TopRight ||
		handle == CropSelectionHandle::BottomRight;
}

bool HasTop(CropSelectionHandle handle) {
	return handle == CropSelectionHandle::Top || handle == CropSelectionHandle::TopLeft ||
		handle == CropSelectionHandle::TopRight;
}

bool HasBottom(CropSelectionHandle handle) {
	return handle == CropSelectionHandle::Bottom || handle == CropSelectionHandle::BottomLeft ||
		handle == CropSelectionHandle::BottomRight;
}

} // namespace

bool ShouldStartNewCropSelection(bool selectionModeEnabled,
	bool forcedByModifier, bool imageNeedsPanning) {
	return forcedByModifier || (selectionModeEnabled && !imageNeedsPanning);
}

void CropSelectionModel::SetImageSize(int width, int height) {
	width = std::max(0, width);
	height = std::max(0, height);
	if (imageWidth_ == width && imageHeight_ == height) return;
	imageWidth_ = width;
	imageHeight_ = height;
	Clear();
}

void CropSelectionModel::SetAspectRatio(int width, int height) {
	if (width <= 0 || height <= 0) return;
	aspectWidth_ = width;
	aspectHeight_ = height;
	mode_ = CropSelectionMode::FixedAspect;
}

void CropSelectionModel::SetFixedSize(int width, int height, bool screenPixels) {
	if (width <= 0 || height <= 0) return;
	fixedWidth_ = width;
	fixedHeight_ = height;
	fixedSizeScreenPixels_ = screenPixels;
	mode_ = CropSelectionMode::FixedSize;
}

bool CropSelectionModel::StartNew(int imageX, int imageY) {
	if (imageWidth_ <= 0 || imageHeight_ <= 0) return false;
	ClampPoint(imageX, imageY);
	creationAnchor_ = {imageX, imageY};
	pointerStart_ = creationAnchor_;
	manipulationStart_ = {};
	handle_ = CropSelectionHandle::NewSelection;
	dragging_ = true;
	selection_ = {imageX, imageY, imageX + 1, imageY + 1};
	return true;
}

bool CropSelectionModel::StartManipulation(int imageX, int imageY,
	CropSelectionHandle handle) {
	if (!selection_.Valid() || handle == CropSelectionHandle::None ||
		handle == CropSelectionHandle::NewSelection) return false;
	ClampPoint(imageX, imageY);
	manipulationStart_ = selection_;
	pointerStart_ = {imageX, imageY};
	handle_ = handle;
	dragging_ = true;
	return true;
}

bool CropSelectionModel::Update(int imageX, int imageY, double zoom) {
	if (!dragging_ || imageWidth_ <= 0 || imageHeight_ <= 0) return false;
	ClampPoint(imageX, imageY);
	SelectionRect next;
	if (handle_ == CropSelectionHandle::NewSelection) {
		next = BuildNewSelection(imageX, imageY, zoom);
	} else if (handle_ == CropSelectionHandle::Move) {
		const int dx = imageX - pointerStart_.x;
		const int dy = imageY - pointerStart_.y;
		next = manipulationStart_;
		next.left += dx;
		next.right += dx;
		next.top += dy;
		next.bottom += dy;
		next = ClampRect(next);
	} else {
		next = BuildResizedSelection(imageX, imageY);
	}
	if (!next.Valid() || (next.left == selection_.left && next.top == selection_.top &&
		next.right == selection_.right && next.bottom == selection_.bottom)) return false;
	selection_ = next;
	return true;
}

bool CropSelectionModel::ReapplyMode(double zoom) {
	if (!selection_.Valid() || imageWidth_ <= 0 || imageHeight_ <= 0) return false;
	SelectionRect next = selection_;
	if (mode_ == CropSelectionMode::FixedSize) {
		const double safeZoom = std::isfinite(zoom) && zoom > 0.0 ? zoom : 1.0;
		const int width = std::min(imageWidth_, fixedSizeScreenPixels_ ?
			RoundedPositive(fixedWidth_ / safeZoom) : fixedWidth_);
		const int height = std::min(imageHeight_, fixedSizeScreenPixels_ ?
			RoundedPositive(fixedHeight_ / safeZoom) : fixedHeight_);
		next.right = next.left + width;
		next.bottom = next.top + height;
	} else if (mode_ == CropSelectionMode::FixedAspect ||
		mode_ == CropSelectionMode::ImageAspect) {
		const double ratio = EffectiveAspectRatio();
		if (ratio > 0.0) {
			// Keep the existing height anchored at the top-left, as when creating
			// a selection in an aspect-constrained mode.
			const int width = std::min(imageWidth_,
				RoundedPositive((next.Height() - 1) * ratio + 1.0));
			next.right = next.left + width;
		}
	}
	next = ClampRect(next);
	if (next.left == selection_.left && next.top == selection_.top &&
		next.right == selection_.right && next.bottom == selection_.bottom) return false;
	selection_ = next;
	return true;
}

void CropSelectionModel::Clear() {
	selection_ = {};
	manipulationStart_ = {};
	dragging_ = false;
	handle_ = CropSelectionHandle::None;
}

double CropSelectionModel::AspectRatio() const {
	return EffectiveAspectRatio();
}

SelectionScreenRect CropSelectionModel::ToScreen(const SelectionRect& selection,
	const SelectionScreenRect& imageDestination, int imageWidth, int imageHeight) {
	if (!selection.Valid() || imageDestination.width <= 0 || imageDestination.height <= 0 ||
		imageWidth <= 0 || imageHeight <= 0) return {};
	const double scaleX = static_cast<double>(imageDestination.width) / imageWidth;
	const double scaleY = static_cast<double>(imageDestination.height) / imageHeight;
	const int left = imageDestination.x + static_cast<int>(std::lround(selection.left * scaleX));
	const int top = imageDestination.y + static_cast<int>(std::lround(selection.top * scaleY));
	const int right = imageDestination.x + static_cast<int>(std::lround(selection.right * scaleX));
	const int bottom = imageDestination.y + static_cast<int>(std::lround(selection.bottom * scaleY));
	return {left, top, std::max(1, right - left), std::max(1, bottom - top)};
}

SelectionPoint CropSelectionModel::ScreenToImage(int screenX, int screenY,
	const SelectionScreenRect& imageDestination, int imageWidth, int imageHeight) {
	if (imageDestination.width <= 0 || imageDestination.height <= 0 ||
		imageWidth <= 0 || imageHeight <= 0) return {};
	const double scaleX = static_cast<double>(imageWidth) / imageDestination.width;
	const double scaleY = static_cast<double>(imageHeight) / imageDestination.height;
	const int imageX = static_cast<int>(std::floor((screenX - imageDestination.x) * scaleX));
	const int imageY = static_cast<int>(std::floor((screenY - imageDestination.y) * scaleY));
	return {std::clamp(imageX, 0, imageWidth - 1),
		std::clamp(imageY, 0, imageHeight - 1)};
}

CropSelectionHandle CropSelectionModel::HitTest(int screenX, int screenY,
	const SelectionScreenRect& rect, bool fixedSizeMode, int tolerance) {
	if (rect.width <= 0 || rect.height <= 0 || tolerance < 0 ||
		screenX < rect.x - tolerance || screenX > rect.x + rect.width + tolerance ||
		screenY < rect.y - tolerance || screenY > rect.y + rect.height + tolerance) {
		return CropSelectionHandle::None;
	}
	const int left = rect.x;
	const int right = rect.x + rect.width;
	const int top = rect.y;
	const int bottom = rect.y + rect.height;
	const int middleX = left + rect.width / 2;
	const int middleY = top + rect.height / 2;
	const auto near = [tolerance](int a, int b) { return std::abs(a - b) <= tolerance; };
	const bool nearHorizontalEdge = near(screenX, left) || near(screenX, right);
	const bool nearVerticalEdge = near(screenY, top) || near(screenY, bottom);
	const bool inside = screenX >= left && screenX <= right && screenY >= top && screenY <= bottom;
	if (!inside && !(nearHorizontalEdge || nearVerticalEdge)) return CropSelectionHandle::None;
	if (fixedSizeMode) return CropSelectionHandle::Move;
	if (!nearHorizontalEdge && !nearVerticalEdge) return CropSelectionHandle::Move;
	if (near(screenX, left) && near(screenY, top)) return CropSelectionHandle::TopLeft;
	if (near(screenX, right) && near(screenY, top)) return CropSelectionHandle::TopRight;
	if (near(screenX, left) && near(screenY, bottom)) return CropSelectionHandle::BottomLeft;
	if (near(screenX, right) && near(screenY, bottom)) return CropSelectionHandle::BottomRight;
	if (near(screenY, top) && std::abs(screenX - middleX) <= rect.width / 3) return CropSelectionHandle::Top;
	if (near(screenY, bottom) && std::abs(screenX - middleX) <= rect.width / 3) return CropSelectionHandle::Bottom;
	if (near(screenX, left) && std::abs(screenY - middleY) <= rect.height / 3) return CropSelectionHandle::Left;
	if (near(screenX, right) && std::abs(screenY - middleY) <= rect.height / 3) return CropSelectionHandle::Right;
	return CropSelectionHandle::Move;
}

SelectionRect CropSelectionModel::AlignToMcu(const SelectionRect& selection,
	int imageWidth, int imageHeight, int mcuWidth, int mcuHeight) {
	if (!selection.Valid() || imageWidth <= 0 || imageHeight <= 0 ||
		mcuWidth <= 0 || mcuHeight <= 0 || selection.right > imageWidth ||
		selection.bottom > imageHeight) return {};
	SelectionRect aligned = selection;
	aligned.left = (aligned.left / mcuWidth) * mcuWidth;
	aligned.top = (aligned.top / mcuHeight) * mcuHeight;
	aligned.right = ((aligned.right + mcuWidth - 1) / mcuWidth) * mcuWidth;
	aligned.bottom = ((aligned.bottom + mcuHeight - 1) / mcuHeight) * mcuHeight;
	if (aligned.right > imageWidth) aligned.right -= mcuWidth;
	if (aligned.bottom > imageHeight) aligned.bottom -= mcuHeight;
	if (aligned.right <= aligned.left || aligned.bottom <= aligned.top) return {};
	return aligned;
}

SelectionRect CropSelectionModel::BuildNewSelection(int imageX, int imageY, double zoom) const {
	if (mode_ == CropSelectionMode::FixedSize) {
		const double safeZoom = std::isfinite(zoom) && zoom > 0.0 ? zoom : 1.0;
		const int width = std::min(imageWidth_, fixedSizeScreenPixels_ ?
			RoundedPositive(fixedWidth_ / safeZoom) : fixedWidth_);
		const int height = std::min(imageHeight_, fixedSizeScreenPixels_ ?
			RoundedPositive(fixedHeight_ / safeZoom) : fixedHeight_);
		// Windows' fixed-size crop follows the pointer as its top-left corner;
		// unlike free/aspect selection, the initial mouse-down is not an anchor.
		return ClampRect({imageX, imageY, imageX + width, imageY + height});
	}
	const int dx = imageX - creationAnchor_.x;
	const int dy = imageY - creationAnchor_.y;
	int width = std::abs(dx) + 1;
	int height = std::abs(dy) + 1;
	const double ratio = EffectiveAspectRatio();
	if (ratio > 0.0 && mode_ != CropSelectionMode::Free) {
		const int widthFromHeight = std::min(imageWidth_, RoundedPositive(height * ratio));
		const int heightFromWidth = std::min(imageHeight_, RoundedPositive(width / ratio));
		if (std::abs(dx) >= std::abs(dy) * ratio) height = heightFromWidth;
		else width = widthFromHeight;
	}
	const int left = dx < 0 ? creationAnchor_.x - width + 1 : creationAnchor_.x;
	const int top = dy < 0 ? creationAnchor_.y - height + 1 : creationAnchor_.y;
	return ClampRect({left, top, left + width, top + height});
}

SelectionRect CropSelectionModel::BuildResizedSelection(int imageX, int imageY) const {
	SelectionRect result = manipulationStart_;
	const int dx = imageX - pointerStart_.x;
	const int dy = imageY - pointerStart_.y;
	if (HasLeft(handle_)) result.left += dx;
	if (HasRight(handle_)) result.right += dx;
	if (HasTop(handle_)) result.top += dy;
	if (HasBottom(handle_)) result.bottom += dy;
	result.left = std::clamp(result.left, 0, imageWidth_ - 1);
	result.top = std::clamp(result.top, 0, imageHeight_ - 1);
	result.right = std::clamp(result.right, result.left + 1, imageWidth_);
	result.bottom = std::clamp(result.bottom, result.top + 1, imageHeight_);
	const double ratio = EffectiveAspectRatio();
	if (ratio <= 0.0 || mode_ == CropSelectionMode::Free || mode_ == CropSelectionMode::FixedSize) {
		return result;
	}
	const bool horizontal = HasLeft(handle_) || HasRight(handle_);
	const bool vertical = HasTop(handle_) || HasBottom(handle_);
	if (horizontal && vertical) {
		const int width = result.Width();
		const int height = result.Height();
		if (width > height * ratio) {
			result.bottom = HasTop(handle_) ? result.bottom : result.top +
				std::min(imageHeight_, RoundedPositive(width / ratio));
		} else {
			result.right = HasLeft(handle_) ? result.right : result.left +
				std::min(imageWidth_, RoundedPositive(height * ratio));
		}
	} else if (horizontal) {
		const int height = std::min(imageHeight_, RoundedPositive(result.Width() / ratio));
		const int center = (manipulationStart_.top + manipulationStart_.bottom) / 2;
		result.top = center - height / 2;
		result.bottom = result.top + height;
	} else if (vertical) {
		const int width = std::min(imageWidth_, RoundedPositive(result.Height() * ratio));
		const int center = (manipulationStart_.left + manipulationStart_.right) / 2;
		result.left = center - width / 2;
		result.right = result.left + width;
	}
	return ClampRect(result);
}

SelectionRect CropSelectionModel::ClampRect(SelectionRect rect) const {
	if (imageWidth_ <= 0 || imageHeight_ <= 0) return {};
	int width = std::max(1, rect.Width());
	int height = std::max(1, rect.Height());
	if (width > imageWidth_ || height > imageHeight_) {
		const double scale = std::min(static_cast<double>(imageWidth_) / width,
			static_cast<double>(imageHeight_) / height);
		width = std::max(1, static_cast<int>(std::floor(width * scale)));
		height = std::max(1, static_cast<int>(std::floor(height * scale)));
	}
	int left = std::clamp(rect.left, 0, imageWidth_ - width);
	int top = std::clamp(rect.top, 0, imageHeight_ - height);
	if (mode_ != CropSelectionMode::Free && mode_ != CropSelectionMode::FixedSize) {
		const double ratio = EffectiveAspectRatio();
		if (ratio > 0.0 && std::abs(static_cast<double>(width) / height - ratio) > 0.0001) {
			const int fitWidth = std::min(imageWidth_, RoundedPositive(height * ratio));
			const int fitHeight = std::min(imageHeight_, RoundedPositive(width / ratio));
			const double widthRatioError = std::abs(static_cast<double>(fitWidth) / height - ratio);
			const double heightRatioError = std::abs(static_cast<double>(width) / fitHeight - ratio);
			if (widthRatioError <= heightRatioError) width = fitWidth;
			else height = fitHeight;
			left = std::clamp(left, 0, imageWidth_ - width);
			top = std::clamp(top, 0, imageHeight_ - height);
		}
	}
	return {left, top, left + width, top + height};
}

double CropSelectionModel::EffectiveAspectRatio() const {
	if (mode_ == CropSelectionMode::ImageAspect && imageWidth_ > 0 && imageHeight_ > 0) {
		return static_cast<double>(imageWidth_) / imageHeight_;
	}
	if ((mode_ == CropSelectionMode::FixedAspect || mode_ == CropSelectionMode::ImageAspect) &&
		aspectWidth_ > 0 && aspectHeight_ > 0) {
		return static_cast<double>(aspectWidth_) / aspectHeight_;
	}
	return 0.0;
}

void CropSelectionModel::ClampPoint(int& x, int& y) const {
	x = std::clamp(x, 0, imageWidth_ - 1);
	y = std::clamp(y, 0, imageHeight_ - 1);
}

} // namespace jpegview_linux
