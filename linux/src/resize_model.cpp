#include "resize_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <exception>

namespace jpegview_linux {
namespace {

constexpr int kMaxImageDimension = 65535;
constexpr std::uint64_t kMaxImagePixels = 100ull * 1024ull * 1024ull;
constexpr const char* kInvalidSizeMessage =
	"Size must be positive and no larger than 65535 x 65535 / 100 MP";

} // namespace

void ResizeModel::Reset(int originalWidth, int originalHeight) {
	originalWidth_ = originalWidth;
	originalHeight_ = originalHeight;
	percentText_ = "100";
	widthText_ = std::to_string(originalWidth);
	heightText_ = std::to_string(originalHeight);
	filter_ = 2;
	validationMessage_.clear();
}

std::string& ResizeModel::FieldText(int field) {
	if (field == kWidthField) return widthText_;
	if (field == kHeightField) return heightText_;
	return percentText_;
}

const std::string& ResizeModel::FieldText(int field) const {
	if (field == kWidthField) return widthText_;
	if (field == kHeightField) return heightText_;
	return percentText_;
}

bool ResizeModel::UpdateFrom(int changedField) {
	if (originalWidth_ <= 0 || originalHeight_ <= 0) return false;
	int width = 0;
	int height = 0;
	double percent = 0.0;
	if (changedField == kPercentField) {
		if (!ParsePercent(percent)) return false;
		width = static_cast<int>(std::llround(originalWidth_ * percent / 100.0));
		height = static_cast<int>(std::llround(originalHeight_ * percent / 100.0));
	} else if (changedField == kWidthField) {
		if (!ParseInteger(widthText_, width)) return false;
		percent = 100.0 * width / originalWidth_;
		height = static_cast<int>(std::llround(originalHeight_ * percent / 100.0));
	} else if (changedField == kHeightField) {
		if (!ParseInteger(heightText_, height)) return false;
		percent = 100.0 * height / originalHeight_;
		width = static_cast<int>(std::llround(originalWidth_ * percent / 100.0));
	} else {
		return false;
	}
	if (!ValidSize(width, height)) {
		validationMessage_ = kInvalidSizeMessage;
		return false;
	}
	percentText_ = std::to_string(std::max(1, static_cast<int>(std::llround(percent))));
	widthText_ = std::to_string(width);
	heightText_ = std::to_string(height);
	validationMessage_.clear();
	return true;
}

bool ResizeModel::Target(int& width, int& height) const {
	return ParseInteger(widthText_, width) && ParseInteger(heightText_, height) && ValidSize(width, height);
}

void ResizeModel::CycleFilter(int direction) {
	filter_ = (filter_ + direction % kFilterCount + kFilterCount) % kFilterCount;
}

const char* ResizeModel::FilterName() const {
	static constexpr const char* names[kFilterCount] = {
		"BOX / POINT", "LANCZOS / BICUBIC", "SHARPEN LOW", "SHARPEN MEDIUM"
	};
	return names[std::clamp(filter_, 0, kFilterCount - 1)];
}

bool ResizeModel::ParsePercent(double& percent) const {
	try {
		std::size_t parsedCharacters = 0;
		percent = std::stod(percentText_, &parsedCharacters);
		return parsedCharacters == percentText_.size() && std::isfinite(percent) && percent > 0.0;
	} catch (const std::exception&) {
		return false;
	}
}

bool ResizeModel::ParseInteger(const std::string& text, int& value) {
	try {
		std::size_t parsedCharacters = 0;
		const long long parsed = std::stoll(text, &parsedCharacters);
		if (parsedCharacters != text.size() || parsed <= 0 || parsed > kMaxImageDimension) return false;
		value = static_cast<int>(parsed);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

bool ResizeModel::ValidSize(int width, int height) {
	return width > 0 && height > 0 && width <= kMaxImageDimension && height <= kMaxImageDimension &&
		static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) <= kMaxImagePixels;
}

void ResizeDialogController::Open(int originalWidth, int originalHeight) {
	model_.Reset(originalWidth, originalHeight);
	focusedField_ = ResizeModel::kPercentField;
	inputPrimed_ = true;
	message_.clear();
	open_ = true;
}

void ResizeDialogController::Close() {
	open_ = false;
	inputPrimed_ = false;
	message_.clear();
}

void ResizeDialogController::MoveFocus(int direction) {
	if (direction == 0) return;
	focusedField_ = (focusedField_ + (direction < 0 ? ResizeModel::kFilterCount - 1 : 1)) %
		ResizeModel::kFilterCount;
	inputPrimed_ = true;
}

void ResizeDialogController::SelectField(int field) {
	if (field < ResizeModel::kPercentField || field > ResizeModel::kFilterField) return;
	focusedField_ = field;
	inputPrimed_ = true;
}

void ResizeDialogController::SelectAll() {
	if (focusedField_ > ResizeModel::kHeightField) return;
	model_.FieldText(focusedField_).clear();
	inputPrimed_ = false;
}

void ResizeDialogController::Backspace() {
	if (focusedField_ > ResizeModel::kHeightField) return;
	PrimeField();
	std::string& text = model_.FieldText(focusedField_);
	if (!text.empty()) text.pop_back();
	UpdateFromFocusedField();
}

void ResizeDialogController::AppendText(const std::string& text) {
	if (focusedField_ > ResizeModel::kHeightField) return;
	PrimeField();
	std::string& field = model_.FieldText(focusedField_);
	for (const unsigned char character : text) {
		if (std::isdigit(character) != 0 ||
			(focusedField_ == ResizeModel::kPercentField && character == '.')) {
			field.push_back(static_cast<char>(character));
		}
	}
	UpdateFromFocusedField();
}

void ResizeDialogController::CycleFilter(int direction) {
	model_.CycleFilter(direction);
	focusedField_ = ResizeModel::kFilterField;
	inputPrimed_ = false;
}

void ResizeDialogController::PrimeField() {
	if (!inputPrimed_ || focusedField_ > ResizeModel::kHeightField) return;
	model_.FieldText(focusedField_).clear();
	inputPrimed_ = false;
}

void ResizeDialogController::UpdateFromFocusedField() {
	if (model_.UpdateFrom(focusedField_)) message_.clear();
	else if (!model_.ValidationMessage().empty()) message_ = model_.ValidationMessage();
}

} // namespace jpegview_linux
