#include "file_dialog_model.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace jpegview_linux {
namespace {

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

} // namespace

std::vector<FileDialogEntry> FilterFileDialogEntries(
	const std::vector<FileDialogEntry>& entries, std::string_view filter) {
	if (filter.empty()) return entries;
	const std::string loweredFilter = Lower(std::string(filter));
	std::vector<FileDialogEntry> filtered;
	filtered.reserve(entries.size());
	for (const FileDialogEntry& entry : entries) {
		if (entry.parent || Lower(entry.path.filename().string()).find(loweredFilter) != std::string::npos) {
			filtered.push_back(entry);
		}
	}
	return filtered;
}

} // namespace jpegview_linux
