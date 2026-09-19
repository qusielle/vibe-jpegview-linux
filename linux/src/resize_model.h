#pragma once

#include <string>
#include <utility>

namespace jpegview_linux {

class ResizeModel {
public:
	static constexpr int kPercentField = 0;
	static constexpr int kWidthField = 1;
	static constexpr int kHeightField = 2;
	static constexpr int kFilterField = 3;
	static constexpr int kFilterCount = 4;

	void Reset(int originalWidth, int originalHeight);
	std::string& FieldText(int field);
	const std::string& FieldText(int field) const;
	bool UpdateFrom(int changedField);
	bool Target(int& width, int& height) const;
	void CycleFilter(int direction);

	int OriginalWidth() const { return originalWidth_; }
	int OriginalHeight() const { return originalHeight_; }
	int Filter() const { return filter_; }
	const char* FilterName() const;
	const std::string& ValidationMessage() const { return validationMessage_; }

private:
	bool ParsePercent(double& percent) const;
	static bool ParseInteger(const std::string& text, int& value);
	static bool ValidSize(int width, int height);

	int originalWidth_ = 0;
	int originalHeight_ = 0;
	std::string percentText_;
	std::string widthText_;
	std::string heightText_;
	int filter_ = 2;
	std::string validationMessage_;
};

class ResizeDialogController {
public:
	void Open(int originalWidth, int originalHeight);
	void Close();
	void MoveFocus(int direction);
	void SelectField(int field);
	void SelectAll();
	void Backspace();
	void AppendText(const std::string& text);
	void CycleFilter(int direction);

	bool IsOpen() const { return open_; }
	int FocusedField() const { return focusedField_; }
	const ResizeModel& Model() const { return model_; }
	ResizeModel& Model() { return model_; }
	const std::string& Message() const { return message_; }
	void SetMessage(std::string message) { message_ = std::move(message); }
	bool Target(int& width, int& height) const { return model_.Target(width, height); }

private:
	void PrimeField();
	void UpdateFromFocusedField();

	bool open_ = false;
	int focusedField_ = ResizeModel::kPercentField;
	bool inputPrimed_ = false;
	ResizeModel model_;
	std::string message_;
};

} // namespace jpegview_linux
