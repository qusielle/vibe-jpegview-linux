#pragma once

#include <string>

namespace jpegview_linux {

class CropSizeDialogController {
public:
	static constexpr int kWidthField = 0;
	static constexpr int kHeightField = 1;

	void Open(int width, int height, bool screenPixels);
	void Close();
	void MoveFocus(int direction);
	void SelectField(int field);
	void SelectAll();
	void Backspace();
	void AppendText(const std::string& text);
	void ToggleUnits() { screenPixels_ = !screenPixels_; }
	void SetScreenPixels(bool screenPixels) { screenPixels_ = screenPixels; }
	bool Apply(int& width, int& height, bool& screenPixels);

	bool IsOpen() const { return open_; }
	int FocusedField() const { return focusedField_; }
	const std::string& WidthText() const { return widthText_; }
	const std::string& HeightText() const { return heightText_; }
	bool UsesScreenPixels() const { return screenPixels_; }
	const std::string& Message() const { return message_; }

private:
	void PrimeField();
	static bool ParseDimension(const std::string& text, int& dimension);
	std::string& FocusedText();

	bool open_ = false;
	int focusedField_ = kWidthField;
	bool inputPrimed_ = false;
	bool screenPixels_ = true;
	std::string widthText_;
	std::string heightText_;
	std::string message_;
};

} // namespace jpegview_linux
