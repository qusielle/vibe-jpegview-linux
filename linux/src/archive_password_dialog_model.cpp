#include "archive_password_dialog_model.h"

#include <cstdint>
#include <utility>

namespace jpegview_linux {
namespace {

bool IsValidUtf8(std::string_view text) {
	std::size_t position = 0;
	while (position < text.size()) {
		const unsigned char first = static_cast<unsigned char>(text[position]);
		if (first <= 0x7f) {
			if (first == 0) return false;
			++position;
			continue;
		}

		std::size_t sequenceLength = 0;
		std::uint32_t codePoint = 0;
		if (first >= 0xc2 && first <= 0xdf) {
			sequenceLength = 2;
			codePoint = first & 0x1f;
		} else if (first >= 0xe0 && first <= 0xef) {
			sequenceLength = 3;
			codePoint = first & 0x0f;
		} else if (first >= 0xf0 && first <= 0xf4) {
			sequenceLength = 4;
			codePoint = first & 0x07;
		} else {
			return false;
		}
		if (sequenceLength > text.size() - position) return false;

		for (std::size_t offset = 1; offset < sequenceLength; ++offset) {
			const unsigned char continuation = static_cast<unsigned char>(text[position + offset]);
			if ((continuation & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (continuation & 0x3f);
		}
		if ((sequenceLength == 3 && codePoint < 0x800) ||
			(sequenceLength == 4 && codePoint < 0x10000) ||
			(codePoint >= 0xd800 && codePoint <= 0xdfff) || codePoint > 0x10ffff) {
			return false;
		}
		position += sequenceLength;
	}
	return true;
}

} // namespace

void ArchivePasswordDialogModel::Begin(std::string archivePath, std::string errorMessage) {
	Reset();
	archivePath_ = std::move(archivePath);
	errorMessage_ = std::move(errorMessage);
	open_ = true;
}

void ArchivePasswordDialogModel::Cancel() {
	Reset();
}

bool ArchivePasswordDialogModel::AppendText(std::string_view utf8Text) {
	if (!open_ || !IsValidUtf8(utf8Text)) return false;
	if (!utf8Text.empty()) password_.append(utf8Text.data(), utf8Text.size());
	if (!utf8Text.empty()) errorMessage_.clear();
	return true;
}

bool ArchivePasswordDialogModel::Backspace() {
	if (!open_ || password_.empty()) return false;

	std::size_t start = password_.size() - 1;
	while (start > 0 && (static_cast<unsigned char>(password_[start]) & 0xc0) == 0x80) {
		--start;
	}
	password_.erase(start);
	errorMessage_.clear();
	return true;
}

std::optional<std::string> ArchivePasswordDialogModel::Submit() {
	if (!open_) return std::nullopt;
	std::optional<std::string> submitted(std::move(password_));
	Reset();
	return submitted;
}

std::string ArchivePasswordDialogModel::DisplayText() const {
	std::string masked;
	for (const unsigned char byte : password_) {
		if ((byte & 0xc0) != 0x80) masked += "\xe2\x80\xa2";
	}
	return masked;
}

void ArchivePasswordDialogModel::Reset() {
	open_ = false;
	archivePath_.clear();
	volatile char* bytes = password_.empty() ? nullptr : &password_[0];
	for (std::size_t index = 0; index < password_.size(); ++index) bytes[index] = '\0';
	password_.clear();
	errorMessage_.clear();
}

} // namespace jpegview_linux
