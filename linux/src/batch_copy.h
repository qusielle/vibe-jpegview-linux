#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jpegview_linux {

struct BatchCopyItem {
	std::filesystem::path source;
	std::time_t modificationTime = 0;
	bool selected = false;
	bool copy = false;
	std::filesystem::path destination;
	std::string destinationText;
};

std::string FormatBatchDate(std::time_t timestamp);

// Expands JPEGView's batch copy/rename template language. The optional
// pictures directory is explicit to keep this function deterministic in tests.
std::string ExpandBatchPattern(const std::string& pattern, std::size_t selectedIndex,
	const std::filesystem::path& source, std::time_t modificationTime,
	const std::optional<std::filesystem::path>& picturesDirectory = std::nullopt);

std::filesystem::path BatchCopyDestination(const std::string& pattern,
	const BatchCopyItem& item, std::size_t selectedIndex);

void UpdateBatchCopyPreview(const std::string& pattern, std::vector<BatchCopyItem>& items);

class BatchCopyDialogController {
public:
	void Open(std::vector<BatchCopyItem> items, std::size_t currentIndex,
		std::string pattern, int visibleRows);
	void Close();
	void ReplaceItems(std::vector<BatchCopyItem> items, std::size_t currentIndex,
		int visibleRows);
	void SetPatternFocused(bool focused) { patternFocused_ = focused; }
	void TogglePatternFocus() { patternFocused_ = !patternFocused_; }
	void AppendPattern(const std::string& text);
	void BackspacePattern();
	void SelectAll(bool selected);
	void ToggleItem(int index);
	void MoveCursor(int direction, int visibleRows);
	void FocusItem(int index);
	void ScrollBy(int rows, int visibleRows);
	void Preview();

	bool IsOpen() const { return open_; }
	bool PatternFocused() const { return patternFocused_; }
	const std::string& Pattern() const { return pattern_; }
	const std::string& Message() const { return message_; }
	int Cursor() const { return cursor_; }
	std::size_t Scroll() const { return scroll_; }
	const std::vector<BatchCopyItem>& Items() const { return items_; }
	std::vector<BatchCopyItem>& Items() { return items_; }
	void SetMessage(std::string message) { message_ = std::move(message); }

private:
	void EnsureCursorVisible(int visibleRows);

	bool open_ = false;
	bool patternFocused_ = false;
	std::string pattern_;
	std::string message_;
	std::vector<BatchCopyItem> items_;
	std::size_t scroll_ = 0;
	int cursor_ = 0;
};

} // namespace jpegview_linux
