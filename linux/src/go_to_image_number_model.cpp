#include "go_to_image_number_model.h"

#include <limits>

namespace jpegview_linux {
namespace {

constexpr std::size_t kMaximumImageNumberDigits = 20;

} // namespace

void GoToImageNumberModel::Open(std::size_t imageCount, std::size_t currentIndex) {
	imageCount_ = imageCount;
	open_ = imageCount_ != 0;
	message_.clear();
	inputPrimed_ = open_;
	text_ = open_ && currentIndex < imageCount_ ? std::to_string(currentIndex + 1) : std::string();
}

void GoToImageNumberModel::Close() {
	open_ = false;
	inputPrimed_ = false;
	text_.clear();
	message_.clear();
	imageCount_ = 0;
}

bool GoToImageNumberModel::AppendText(const std::string& text) {
	if (!open_ || text.empty() || text.size() > kMaximumImageNumberDigits) return false;
	for (const char character : text) {
		if (character < '0' || character > '9') {
			message_ = "Enter a whole image number.";
			return false;
		}
	}
	const std::size_t existingBytes = inputPrimed_ ? 0 : text_.size();
	if (existingBytes > kMaximumImageNumberDigits ||
		text.size() > kMaximumImageNumberDigits - existingBytes) {
		message_ = "Image number is too long.";
		return false;
	}
	if (inputPrimed_) {
		text_.clear();
		inputPrimed_ = false;
	}
	text_ += text;
	message_.clear();
	return true;
}

void GoToImageNumberModel::Backspace() {
	if (!open_) return;
	if (inputPrimed_) {
		text_.clear();
		inputPrimed_ = false;
	} else if (!text_.empty()) {
		text_.pop_back();
	}
	message_.clear();
}

void GoToImageNumberModel::SelectAll() {
	if (!open_) return;
	text_.clear();
	inputPrimed_ = false;
	message_.clear();
}

std::optional<std::size_t> GoToImageNumberModel::Submit() {
	if (!open_) return std::nullopt;
	if (text_.empty()) {
		message_ = "Enter an image number.";
		return std::nullopt;
	}
	std::size_t number = 0;
	for (const char character : text_) {
		const std::size_t digit = static_cast<std::size_t>(character - '0');
		if (number > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
			message_ = "Image number is outside the valid range.";
			return std::nullopt;
		}
		number = number * 10 + digit;
	}
	if (number == 0 || number > imageCount_) {
		message_ = "Enter a number from 1 to " + std::to_string(imageCount_) + ".";
		return std::nullopt;
	}
	message_.clear();
	return number - 1;
}

} // namespace jpegview_linux
