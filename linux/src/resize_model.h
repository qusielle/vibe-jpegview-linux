#pragma once

#include <string>

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

} // namespace jpegview_linux
