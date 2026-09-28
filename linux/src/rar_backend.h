#pragma once

#include "archive_source.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jpegview_linux {

struct RarEntry {
	std::uint64_t index = 0;
	std::string rawPath;
	bool directory = false;
	std::uint64_t size = 0;
	std::int64_t modificationTime = 0;
	bool encrypted = false;
	bool specialFile = false;
	bool split = false;
};

using RarDataWriter = std::function<bool(const std::uint8_t*, std::size_t,
	std::string&)>;

bool RarBackendAvailable();

bool ReadRarCatalog(const std::filesystem::path& archive,
	const std::optional<std::string>& password, const std::function<bool()>& shouldContinue,
	std::vector<RarEntry>& entries, bool& headerEncrypted, std::string& errorMessage,
	ArchiveErrorKind* errorKind = nullptr);

bool ExtractRarMember(const std::filesystem::path& archive, std::uint64_t index,
	const std::string& expectedRawPath, const std::optional<std::string>& password,
	const std::function<bool()>& shouldContinue, const RarDataWriter& writer,
	std::string& errorMessage, ArchiveErrorKind* errorKind = nullptr);

} // namespace jpegview_linux
