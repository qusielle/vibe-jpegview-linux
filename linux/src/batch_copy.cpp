#include "batch_copy.h"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

void ReplaceAll(std::string& value, const std::string& from, const std::string& to) {
	if (from.empty()) return;
	std::size_t position = 0;
	while ((position = value.find(from, position)) != std::string::npos) {
		value.replace(position, from.size(), to);
		position += to.size();
	}
}

std::string FirstNumber(const std::string& filename) {
	std::size_t start = std::string::npos;
	for (std::size_t index = 0; index < filename.size(); ++index) {
		if (std::isdigit(static_cast<unsigned char>(filename[index]))) {
			if (start == std::string::npos) start = index;
		} else if (start != std::string::npos) {
			return filename.substr(start, index - start);
		}
	}
	return start == std::string::npos ? std::string() : filename.substr(start);
}

fs::path AbsoluteNormalized(const fs::path& filename) {
	std::error_code error;
	const fs::path absolute = fs::absolute(filename, error);
	return error ? filename.lexically_normal() : absolute.lexically_normal();
}

fs::path PicturesDirectory() {
	if (const char* pictures = std::getenv("XDG_PICTURES_DIR"); pictures != nullptr && *pictures != '\0') {
		return AbsoluteNormalized(fs::path(pictures));
	}
	if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
		return AbsoluteNormalized(fs::path(home) / "Pictures");
	}
	return {};
}

} // namespace

std::string FormatBatchDate(std::time_t timestamp) {
	if (timestamp == 0) return {};
	std::tm localTime{};
	if (localtime_r(&timestamp, &localTime) == nullptr) return {};
	char formatted[32]{};
	if (std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &localTime) == 0) return {};
	return formatted;
}

std::string ExpandBatchPattern(const std::string& pattern, std::size_t selectedIndex,
	const fs::path& source, std::time_t modificationTime,
	const std::optional<fs::path>& picturesDirectory) {
	std::string result = pattern;
	const std::string title = source.filename().string();
	const std::string stem = source.stem().string();
	const std::string extension = source.extension().string();
	const std::string number = FirstNumber(title);
	ReplaceAll(result, "%x", std::to_string(selectedIndex + 1));
	ReplaceAll(result, "%n", number);
	for (int digits = 2; digits <= 9; ++digits) {
		std::ostringstream formatted;
		formatted << std::setw(digits) << std::setfill('0') << selectedIndex + 1;
		ReplaceAll(result, "%" + std::to_string(digits) + "x", formatted.str());
	}
	ReplaceAll(result, "%f", title);
	ReplaceAll(result, "%F", stem);
	ReplaceAll(result, "%e", extension.empty() ? std::string() : extension.substr(1));

	std::tm localTime{};
	if (modificationTime != 0) localtime_r(&modificationTime, &localTime);
	char datePart[32]{};
	if (modificationTime != 0) {
		std::strftime(datePart, sizeof(datePart), "%H", &localTime);
		ReplaceAll(result, "%h", datePart);
		std::strftime(datePart, sizeof(datePart), "%M", &localTime);
		ReplaceAll(result, "%min", datePart);
		std::strftime(datePart, sizeof(datePart), "%d", &localTime);
		ReplaceAll(result, "%d", datePart);
		std::strftime(datePart, sizeof(datePart), "%m", &localTime);
		ReplaceAll(result, "%m", datePart);
		std::strftime(datePart, sizeof(datePart), "%Y", &localTime);
		ReplaceAll(result, "%y", datePart);
		std::strftime(datePart, sizeof(datePart), "%y", &localTime);
		ReplaceAll(result, "%2y", datePart);
		std::strftime(datePart, sizeof(datePart), "%B", &localTime);
		ReplaceAll(result, "%3M", std::string(datePart).substr(0, 3));
		ReplaceAll(result, "%M", datePart);
	}
	const fs::path pictures = picturesDirectory.has_value() ? *picturesDirectory : PicturesDirectory();
	if (!pictures.empty()) ReplaceAll(result, "%pictures%", pictures.string());
	// Windows JPEGView patterns use backslashes for subdirectories. Accept
	// those templates on Linux while still allowing the native '/' separator.
	std::replace(result.begin(), result.end(), '\\', '/');
	return result;
}

fs::path BatchCopyDestination(const std::string& pattern, const BatchCopyItem& item,
	std::size_t selectedIndex) {
	if (pattern.empty()) return {};
	const std::string expanded = ExpandBatchPattern(pattern, selectedIndex,
		item.source, item.modificationTime);
	if (expanded.empty()) return {};
	const fs::path target(expanded);
	return AbsoluteNormalized(target.is_absolute() ? target : item.source.parent_path() / target);
}

void UpdateBatchCopyPreview(const std::string& pattern, std::vector<BatchCopyItem>& items) {
	std::size_t selectedIndex = 0;
	for (BatchCopyItem& item : items) {
		item.destination = fs::path{};
		item.destinationText.clear();
		item.copy = false;
		if (!item.selected) continue;
		item.destinationText = ExpandBatchPattern(pattern, selectedIndex,
			item.source, item.modificationTime);
		if (!item.destinationText.empty()) {
			item.destination = BatchCopyDestination(pattern, item, selectedIndex);
			item.copy = item.destination.parent_path() != item.source.parent_path();
		}
		++selectedIndex;
	}
}

void BatchCopyDialogController::Open(std::vector<BatchCopyItem> items,
	std::size_t currentIndex, std::string pattern, int visibleRows) {
	open_ = true;
	patternFocused_ = true;
	pattern_ = std::move(pattern);
	message_.clear();
	ReplaceItems(std::move(items), currentIndex, visibleRows);
}

void BatchCopyDialogController::Close() {
	open_ = false;
	patternFocused_ = false;
}

void BatchCopyDialogController::ReplaceItems(std::vector<BatchCopyItem> items,
	std::size_t currentIndex, int visibleRows) {
	items_ = std::move(items);
	scroll_ = 0;
	cursor_ = items_.empty() ? 0 : static_cast<int>(std::min(currentIndex, items_.size() - 1));
	EnsureCursorVisible(visibleRows);
}

void BatchCopyDialogController::AppendPattern(const std::string& text) {
	if (!patternFocused_ || text.empty()) return;
	pattern_ += text;
	Preview();
}

void BatchCopyDialogController::BackspacePattern() {
	if (!patternFocused_ || pattern_.empty()) return;
	std::size_t start = pattern_.size() - 1;
	while (start > 0 && (static_cast<unsigned char>(pattern_[start]) & 0xC0u) == 0x80u) --start;
	pattern_.erase(start);
	Preview();
}

void BatchCopyDialogController::SelectAll(bool selected) {
	for (BatchCopyItem& item : items_) item.selected = selected;
	Preview();
}

void BatchCopyDialogController::ToggleItem(int index) {
	if (index < 0 || index >= static_cast<int>(items_.size())) return;
	cursor_ = index;
	patternFocused_ = false;
	BatchCopyItem& item = items_[static_cast<std::size_t>(index)];
	item.selected = !item.selected;
	Preview();
}

void BatchCopyDialogController::MoveCursor(int direction, int visibleRows) {
	if (patternFocused_ || items_.empty() || direction == 0) return;
	cursor_ = std::clamp(cursor_ + (direction < 0 ? -1 : 1), 0,
		static_cast<int>(items_.size()) - 1);
	EnsureCursorVisible(visibleRows);
}

void BatchCopyDialogController::FocusItem(int index) {
	if (index >= 0 && index < static_cast<int>(items_.size())) cursor_ = index;
}

void BatchCopyDialogController::ScrollBy(int rows, int visibleRows) {
	const int maximumScroll = std::max(0, static_cast<int>(items_.size()) - std::max(1, visibleRows));
	scroll_ = static_cast<std::size_t>(std::clamp(
		static_cast<int>(scroll_) + rows, 0, maximumScroll));
}

void BatchCopyDialogController::Preview() {
	UpdateBatchCopyPreview(pattern_, items_);
	if (pattern_.empty()) {
		message_ = "Enter a target pattern first";
		return;
	}
	int selected = 0;
	int copies = 0;
	for (const BatchCopyItem& item : items_) {
		if (!item.selected) continue;
		++selected;
		if (item.copy) ++copies;
	}
	message_ = selected == 0 ? "Select one or more files" :
		"Preview: " + std::to_string(selected) + " selected, " + std::to_string(copies) +
		" copied, " + std::to_string(selected - copies) + " renamed";
}

void BatchCopyDialogController::EnsureCursorVisible(int visibleRows) {
	const int rows = std::max(1, visibleRows);
	if (cursor_ < static_cast<int>(scroll_)) scroll_ = static_cast<std::size_t>(std::max(0, cursor_));
	if (cursor_ >= static_cast<int>(scroll_) + rows) {
		scroll_ = static_cast<std::size_t>(cursor_ - rows + 1);
	}
	const int maximumScroll = std::max(0, static_cast<int>(items_.size()) - rows);
	scroll_ = std::min(scroll_, static_cast<std::size_t>(maximumScroll));
}

} // namespace jpegview_linux
