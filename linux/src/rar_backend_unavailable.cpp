#include "rar_backend.h"

namespace jpegview_linux {

bool RarBackendAvailable() {
	return false;
}

bool ReadRarCatalog(const std::filesystem::path&,
	const std::optional<std::string>&, const std::function<bool()>&,
	std::vector<RarEntry>& entries, bool& headerEncrypted, std::string& errorMessage,
	ArchiveErrorKind* errorKind) {
	entries.clear();
	headerEncrypted = false;
	errorMessage = "encrypted RAR support is unavailable in this build";
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
	return false;
}

bool ExtractRarMember(const std::filesystem::path&, std::uint64_t,
	const std::string&, const std::optional<std::string>&,
	const std::function<bool()>&, const RarDataWriter&,
	std::string& errorMessage, ArchiveErrorKind* errorKind) {
	errorMessage = "encrypted RAR support is unavailable in this build";
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
	return false;
}

} // namespace jpegview_linux
