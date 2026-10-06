#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace jpegview_linux {

class GoToImageNumberModel {
public:
	void Open(std::size_t imageCount, std::size_t currentIndex);
	void Close();
	bool IsOpen() const { return open_; }
	const std::string& Text() const { return text_; }
	const std::string& Message() const { return message_; }
	std::size_t ImageCount() const { return imageCount_; }

	bool AppendText(const std::string& text);
	void Backspace();
	void SelectAll();
	std::optional<std::size_t> Submit();

private:
	std::size_t imageCount_ = 0;
	std::string text_;
	std::string message_;
	bool open_ = false;
	bool inputPrimed_ = false;
};

} // namespace jpegview_linux
