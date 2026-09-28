#include "seven_zip_backend.h"

namespace jpegview_linux {

bool SevenZipBackendAvailable() {
	return false;
}

bool ReadSevenZipCatalog(const std::filesystem::path&,
	const std::optional<std::string>&, const std::function<bool()>&,
	std::vector<SevenZipEntry>& entries, bool& headerEncrypted, std::string& errorMessage,
	ArchiveErrorKind* errorKind) {
	entries.clear();
	headerEncrypted = false;
	errorMessage = "the private 7-Zip password reader is not bundled in this build";
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
	return false;
}

bool ExtractSevenZipMember(const std::filesystem::path&, std::uint64_t,
	const std::string&, const std::optional<std::string>&,
	const std::function<bool()>&, const SevenZipDataWriter&,
	std::string& errorMessage, ArchiveErrorKind* errorKind) {
	errorMessage = "the private 7-Zip password reader is not bundled in this build";
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
	return false;
}

} // namespace jpegview_linux
