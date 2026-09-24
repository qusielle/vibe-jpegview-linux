#include "crop_size_dialog_model.h"

#include "settings.h"

#include <exception>

namespace jpegview_linux {

void CropSizeDialogController::Open(int width, int height, bool screenPixels) {
	widthText_ = std::to_string(width);
	heightText_ = std::to_string(height);
	focusedField_ = kWidthField;
	inputPrimed_ = true;
	screenPixels_ = screenPixels;
	message_.clear();
	open_ = true;
}

void CropSizeDialogController::Close() {
	open_ = false;
	inputPrimed_ = false;
	message_.clear();
}

void CropSizeDialogController::MoveFocus(int direction) {
	if (direction == 0) return;
	focusedField_ = 1 - focusedField_;
	inputPrimed_ = true;
}

void CropSizeDialogController::SelectField(int field) {
	if (field != kWidthField && field != kHeightField) return;
	focusedField_ = field;
	inputPrimed_ = true;
}

void CropSizeDialogController::SelectAll() {
	if (!open_) return;
	FocusedText().clear();
	inputPrimed_ = false;
}

void CropSizeDialogController::Backspace() {
	if (!open_) return;
	PrimeField();
	std::string& text = FocusedText();
	if (!text.empty()) text.pop_back();
}

void CropSizeDialogController::AppendText(const std::string& text) {
	if (!open_) return;
	PrimeField();
	std::string& field = FocusedText();
	for (const unsigned char character : text) {
		if (character < '0' || character > '9' || field.size() >= 5) continue;
		field.push_back(static_cast<char>(character));
	}
}

bool CropSizeDialogController::Apply(int& width, int& height, bool& screenPixels) {
	if (!open_ || !ParseDimension(widthText_, width) || !ParseDimension(heightText_, height)) {
		message_ = "Enter width and height from 1 to 65535 pixels";
		return false;
	}
	screenPixels = screenPixels_;
	message_.clear();
	return true;
}

void CropSizeDialogController::PrimeField() {
	if (!inputPrimed_) return;
	FocusedText().clear();
	inputPrimed_ = false;
}

bool CropSizeDialogController::ParseDimension(const std::string& text, int& dimension) {
	try {
		std::size_t parsedCharacters = 0;
		const int parsed = std::stoi(text, &parsedCharacters);
		if (parsedCharacters != text.size() || parsed < kMinimumFixedCropDimension ||
			parsed > kMaximumFixedCropDimension) return false;
		dimension = parsed;
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

std::string& CropSizeDialogController::FocusedText() {
	return focusedField_ == kWidthField ? widthText_ : heightText_;
}

} // namespace jpegview_linux
