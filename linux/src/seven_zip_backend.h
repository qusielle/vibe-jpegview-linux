#pragma once

#include "archive_source.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jpegview_linux {

struct SevenZipEntry {
	std::uint64_t index = 0;
	std::string rawPath;
	bool directory = false;
	std::uint64_t size = 0;
	std::int64_t modificationTime = 0;
	bool encrypted = false;
	bool specialFile = false;
};

inline bool IsSevenZipPasswordVerificationMember(const SevenZipEntry& entry) {
	return entry.encrypted && !entry.directory && !entry.specialFile && entry.size != 0;
}

using SevenZipDataWriter = std::function<bool(const std::uint8_t*, std::size_t,
	std::string&)>;

bool SevenZipBackendAvailable();

bool ReadSevenZipCatalog(const std::filesystem::path& archive,
	const std::optional<std::string>& password, const std::function<bool()>& shouldContinue,
	std::vector<SevenZipEntry>& entries, bool& headerEncrypted, std::string& errorMessage,
	ArchiveErrorKind* errorKind = nullptr);

bool ExtractSevenZipMember(const std::filesystem::path& archive, std::uint64_t index,
	const std::string& expectedRawPath, const std::optional<std::string>& password,
	const std::function<bool()>& shouldContinue, const SevenZipDataWriter& writer,
	std::string& errorMessage, ArchiveErrorKind* errorKind = nullptr);

} // namespace jpegview_linux
