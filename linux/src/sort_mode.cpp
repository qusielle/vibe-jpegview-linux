#include "sort_mode.h"

namespace jpegview_linux {

const char* SortModeSettingName(FileList::SortMode mode) {
	switch (mode) {
	case FileList::SortMode::LastModificationTime: return "modification_date";
	case FileList::SortMode::CreationTime: return "creation_date";
	case FileList::SortMode::FileName: return "file_name";
	case FileList::SortMode::Random: return "random";
	case FileList::SortMode::FileSize: return "file_size";
	}
	return "modification_date";
}

const char* SortModeShortLabel(FileList::SortMode mode) {
	switch (mode) {
	case FileList::SortMode::LastModificationTime: return "D";
	case FileList::SortMode::CreationTime: return "C";
	case FileList::SortMode::FileName: return "N";
	case FileList::SortMode::Random: return "R";
	case FileList::SortMode::FileSize: return "S";
	}
	return "?";
}

const char* SortModeDescription(FileList::SortMode mode) {
	switch (mode) {
	case FileList::SortMode::LastModificationTime: return "modification date";
	case FileList::SortMode::CreationTime: return "creation date";
	case FileList::SortMode::FileName: return "file name";
	case FileList::SortMode::Random: return "random";
	case FileList::SortMode::FileSize: return "file size";
	}
	return "unknown";
}

bool ParseSortMode(std::string_view value, FileList::SortMode& mode) {
	if (value == "modification_date") {
		mode = FileList::SortMode::LastModificationTime;
	} else if (value == "creation_date") {
		mode = FileList::SortMode::CreationTime;
	} else if (value == "file_name") {
		mode = FileList::SortMode::FileName;
	} else if (value == "random") {
		mode = FileList::SortMode::Random;
	} else if (value == "file_size") {
		mode = FileList::SortMode::FileSize;
	} else {
		return false;
	}
	return true;
}

} // namespace jpegview_linux
