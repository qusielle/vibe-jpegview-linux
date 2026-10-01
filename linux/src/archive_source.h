#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace jpegview_linux {

// The filesystem identity of the physical source. For an archive member this
// describes the archive container; the logical member path remains a separate
// part of SourceKey so two members cannot alias.
struct SourceIdentity {
	std::uint64_t device = 0;
	std::uint64_t inode = 0;
	std::uint64_t size = 0;
	std::int64_t modifiedSeconds = 0;
	std::int64_t modifiedNanoseconds = 0;
	bool valid = false;
};

bool operator==(const SourceIdentity& left, const SourceIdentity& right);
bool operator!=(const SourceIdentity& left, const SourceIdentity& right);

// A structured cache identity. The logical path is stored as raw native path
// bytes, so legal filenames containing punctuation or newlines remain distinct.
struct SourceKey {
	std::string logicalPath;
	SourceIdentity backingIdentity;

	SourceKey() = default;
	// Path-only construction is a legacy adapter and never yields a cache-valid key.
	SourceKey(std::string path) : logicalPath(std::move(path)) {}
	explicit SourceKey(const char* path) : logicalPath(path == nullptr ? "" : path) {}
	SourceKey(std::string path, SourceIdentity identity)
		: logicalPath(std::move(path)), backingIdentity(identity) {}

	bool Valid() const { return !logicalPath.empty() && backingIdentity.valid; }
	bool Empty() const { return logicalPath.empty(); }
	bool empty() const { return Empty(); }
};

bool operator==(const SourceKey& left, const SourceKey& right);
bool operator!=(const SourceKey& left, const SourceKey& right);
bool operator==(const SourceKey& left, const std::string& right);
bool operator!=(const SourceKey& left, const std::string& right);
bool operator==(const std::string& left, const SourceKey& right);
bool operator!=(const std::string& left, const SourceKey& right);
bool operator==(const SourceKey& left, const char* right);
bool operator!=(const SourceKey& left, const char* right);
bool operator==(const char* left, const SourceKey& right);
bool operator!=(const char* left, const SourceKey& right);

struct SourceKeyHash {
	std::size_t operator()(const SourceKey& key) const;
};

struct SourceMetadata {
	std::uint64_t fileSize = 0;
	// Modification/creation values use Unix epoch nanoseconds for both regular
	// files and archive members. Convert only when adapting to file_time_type.
	std::int64_t modificationTimeNanoseconds = 0;
	std::int64_t creationTimeNanoseconds = 0;
	int width = 0;
	int height = 0;
	bool hasFileSize = false;
	bool hasModificationTime = false;
	bool hasCreationTime = false;
	bool hasDimensions = false;
	bool hasTransparency = false;
	bool transparencyKnown = false;
	bool archiveMember = false;
	bool archiveMemberEncrypted = false;
};

// Descriptors are immutable through their public interface. FileList exposes
// them by const reference after background enumeration has captured source and
// presentation metadata.
class SourceDescriptor {
public:
	SourceDescriptor() = default;
	SourceDescriptor(std::filesystem::path logicalPath, SourceIdentity backingIdentity,
		SourceMetadata metadata);

	const std::filesystem::path& LogicalPath() const { return logicalPath_; }
	const SourceIdentity& BackingIdentity() const { return backingIdentity_; }
	const SourceMetadata& Metadata() const { return metadata_; }
	SourceKey Key() const;
	bool Valid() const { return !logicalPath_.empty() && backingIdentity_.valid; }
	SourceDescriptor WithImageProperties(int width, int height,
		bool hasTransparency) const;

private:
	std::filesystem::path logicalPath_;
	SourceIdentity backingIdentity_;
	SourceMetadata metadata_;
};

struct SourceChangeNotice {
	SourceKey previous;
	SourceDescriptor observed;
};

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
	SourceIdentity backingIdentity;
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
// Convert archive Unix seconds to the filesystem clock's nanosecond domain for
// std::filesystem::file_time_type adapters. SourceMetadata remains Unix-based.
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

// Captures the backing filesystem identity and available logical metadata.
// Archive members include their uncompressed size, member timestamp, and lock
// state while retaining the archive's device/inode/size/nanosecond timestamp.
SourceDescriptor DescribeImageSource(const std::filesystem::path& path);
SourceDescriptor DescribeArchiveMember(const std::filesystem::path& path,
	const SourceIdentity& backingIdentity, std::uint64_t memberSize,
	std::int64_t modificationTimeSeconds, bool encrypted);
bool CaptureImageSourceIdentity(const std::filesystem::path& path,
	SourceIdentity& identity);
bool IsImageSourceCurrent(const SourceDescriptor& descriptor);

} // namespace jpegview_linux
