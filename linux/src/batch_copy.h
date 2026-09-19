#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
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

} // namespace jpegview_linux
