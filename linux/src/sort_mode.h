#pragma once

#include "file_list.h"

#include <string_view>

namespace jpegview_linux {

const char* SortModeSettingName(FileList::SortMode mode);
const char* SortModeShortLabel(FileList::SortMode mode);
const char* SortModeDescription(FileList::SortMode mode);
bool ParseSortMode(std::string_view value, FileList::SortMode& mode);

} // namespace jpegview_linux
