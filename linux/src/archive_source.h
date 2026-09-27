#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace jpegview_linux {

// An archive member is represented to existing path-oriented viewer modules as
// `<archive>.<extension>/<member>`. It behaves like a nested path for filename,
// extension, parent-folder, sorting, and recent-file purposes; archive_source
// alone resolves it to a backend and physical container.
struct ArchiveEntryInfo {
	std::filesystem::path path;
	bool directory = false;
	std::uint64_t size = 0;
	std::int64_t modificationTime = 0;
	bool encrypted = false;
};

struct ArchiveMemberInfo {
	std::uint64_t size = 0;
	std::int64_t modificationTime = 0;
	bool encrypted = false;
};

enum class ArchiveErrorKind {
	None,
	PasswordRequired,
	InvalidPassword,
	UnsupportedEncryption,
	Other,
};

inline constexpr std::uint64_t kMaximumArchiveMemberBytes = 128ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaximumArchiveEntries = 100000;

// Keep format recognition and member I/O here so archive-specific checks do
// not leak into file lists, dialogs, caches, or decoders.
bool IsArchiveContainerName(const std::filesystem::path& path);
bool IsArchiveContainerFile(const std::filesystem::path& path);
bool IsArchiveLocation(const std::filesystem::path& path);
bool IsArchiveMemberLocation(const std::filesystem::path& path);
std::string ArchiveFormatName(const std::filesystem::path& path);
// Archive timestamps are Unix seconds; return their equivalent in the
// filesystem clock's nanosecond domain for consistent sorting with files.
std::int64_t ArchiveTimestampNanoseconds(std::int64_t seconds);
std::filesystem::file_time_type ArchiveFileModificationTime(std::int64_t seconds);

// Return the physical container path for a virtual location, or the input unchanged
// for a regular filesystem source.
std::filesystem::path ArchiveBackingFile(const std::filesystem::path& path);
std::string ArchiveLocationDisplayName(const std::filesystem::path& path);
std::uintmax_t ImageSourceFileSize(const std::filesystem::path& path,
	std::error_code& error);
std::filesystem::file_time_type ImageSourceModificationTime(
	const std::filesystem::path& path, std::error_code& error);

// Lists immediate children from archive metadata. ZIP catalogs use their
// central directory; TAR catalogs stream headers and skip payloads. Directories
// implied by nested members are included.
bool ListArchiveDirectory(const std::filesystem::path& directory,
	std::vector<ArchiveEntryInfo>& entries, std::string& errorMessage);
// Background users can supply a generation/cancellation check. Archive reads
// stop between input blocks, including while a gzip stream is being skipped.
bool ListArchiveDirectoryCancellable(const std::filesystem::path& directory,
	std::vector<ArchiveEntryInfo>& entries, const std::function<bool()>& shouldContinue,
	std::string& errorMessage, ArchiveErrorKind* errorKind = nullptr,
	bool* containsEncryptedEntries = nullptr);
bool GetArchiveMemberInfo(const std::filesystem::path& path,
	ArchiveMemberInfo& info, std::string& errorMessage);

// Passwords are keyed to the current backing-file identity and retained only
// in process memory. They are never read from or written to viewer settings.
bool HasSessionArchivePassword(const std::filesystem::path& path);
bool SetSessionArchivePassword(const std::filesystem::path& path,
	const std::string& password);
void ForgetSessionArchivePassword(const std::filesystem::path& path);
void ClearSessionArchivePasswords();
bool ValidateArchivePassword(const std::filesystem::path& path,
	const std::string& password, std::string& errorMessage,
	ArchiveErrorKind* errorKind = nullptr);

// Reads one selected member into a private anonymous memory file exposed to
// existing path-based codecs for the duration of the callback. The callback
// must not retain the temporary path.
bool WithArchiveMemberFile(const std::filesystem::path& path,
	const std::function<bool(const std::filesystem::path&, std::string&)>& callback,
	std::string& errorMessage, ArchiveErrorKind* errorKind = nullptr);

// Identity for worker-cache validation. It includes the archive's filesystem
// identity for members, while the cache key continues to include their full
// logical path, so replacing an archive invalidates every cached member.
bool IdentifyImageSourceBackingFile(const std::filesystem::path& path,
	std::uint64_t& device, std::uint64_t& inode, std::uint64_t& size,
	std::int64_t& modifiedSeconds, std::int64_t& modifiedNanoseconds);

} // namespace jpegview_linux
