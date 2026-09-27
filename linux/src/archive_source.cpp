#include "archive_source.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <zip.h>

#if defined(__linux__)
#include <linux/memfd.h>
#endif

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

struct ZipLocation {
	fs::path archive;
	std::string memberDirectory;
};

struct BackingIdentity {
	std::uint64_t device = 0;
	std::uint64_t inode = 0;
	std::uint64_t size = 0;
	std::int64_t modifiedSeconds = 0;
	std::int64_t modifiedNanoseconds = 0;
	bool valid = false;
};

bool operator==(const BackingIdentity& left, const BackingIdentity& right) {
	return left.valid && right.valid && left.device == right.device &&
		left.inode == right.inode && left.size == right.size &&
		left.modifiedSeconds == right.modifiedSeconds &&
		left.modifiedNanoseconds == right.modifiedNanoseconds;
}

bool StatIdentity(const fs::path& path, BackingIdentity& identity) {
	struct stat status{};
	if (::stat(path.c_str(), &status) != 0 || status.st_size < 0) return false;
	identity.device = static_cast<std::uint64_t>(status.st_dev);
	identity.inode = static_cast<std::uint64_t>(status.st_ino);
	identity.size = static_cast<std::uint64_t>(status.st_size);
#if defined(__linux__)
	identity.modifiedSeconds = status.st_mtim.tv_sec;
	identity.modifiedNanoseconds = status.st_mtim.tv_nsec;
#else
	identity.modifiedSeconds = status.st_mtime;
#endif
	identity.valid = true;
	return true;
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

bool HasZipExtension(const fs::path& path) {
	return Lower(path.extension().string()) == ".zip";
}

bool SafeMemberPath(std::string_view name, bool directory, std::string& normalized) {
	normalized.clear();
	if (name.empty() || name.front() == '/' || name.find('\0') != std::string_view::npos) return false;
	if (directory && name.back() == '/') name.remove_suffix(1);
	if (name.empty()) return false;
	std::size_t start = 0;
	while (start < name.size()) {
		const std::size_t slash = name.find('/', start);
		const std::size_t end = slash == std::string_view::npos ? name.size() : slash;
		const std::string_view component = name.substr(start, end - start);
		if (component.empty() || component == "." || component == "..") return false;
		if (!normalized.empty()) normalized.push_back('/');
		normalized.append(component.data(), component.size());
		if (slash == std::string_view::npos) break;
		start = slash + 1;
		if (start == name.size()) return false;
	}
	return !normalized.empty();
}

bool ParseZipLocation(const fs::path& path, ZipLocation& location) {
	const std::string text = path.lexically_normal().string();
	if (text.empty()) return false;
	std::size_t componentStart = text.front() == '/' ? 1 : 0;
	while (componentStart < text.size()) {
		std::size_t componentEnd = text.find('/', componentStart);
		if (componentEnd == std::string::npos) componentEnd = text.size();
		const std::string component = text.substr(componentStart, componentEnd - componentStart);
		if (!component.empty() && HasZipExtension(fs::path(component))) {
			const fs::path candidate(text.substr(0, componentEnd));
			std::error_code statusError;
			if (fs::is_regular_file(candidate, statusError) && !statusError) {
				std::string member;
				if (componentEnd < text.size()) {
					const std::string_view remainder(text.data() + componentEnd + 1,
						text.size() - componentEnd - 1);
					if (!remainder.empty() && !SafeMemberPath(remainder, true, member)) return false;
				}
				location.archive = candidate;
				location.memberDirectory = std::move(member);
				return true;
			}
		}
		if (componentEnd == text.size()) break;
		componentStart = componentEnd + 1;
	}
	return false;
}

std::string ZipError(zip_t* archive) {
	const char* error = archive == nullptr ? nullptr : zip_strerror(archive);
	return error == nullptr ? "invalid ZIP archive" : error;
}

std::string ZipOpenError(int errorCode) {
	zip_error_t error;
	zip_error_init_with_code(&error, errorCode);
	const char* message = zip_error_strerror(&error);
	const std::string result = message == nullptr ? "cannot open ZIP archive" : message;
	zip_error_fini(&error);
	return result;
}

struct CatalogChild {
	std::string name;
	std::string fullName;
	bool directory = false;
	std::uint64_t size = 0;
	std::int64_t modificationTime = 0;
};

struct ZipMemberInfo {
	std::uint64_t size = 0;
	std::uint64_t compressedSize = 0;
	std::uint32_t crc = 0;
	std::uint64_t index = 0;
	std::int64_t modificationTime = 0;
};

struct ZipCatalog {
	fs::path archive;
	BackingIdentity identity;
	std::unordered_map<std::string, ZipMemberInfo> members;
	std::unordered_map<std::string, std::vector<CatalogChild>> directories;
	std::unordered_map<std::string,
		std::unordered_map<std::string, std::size_t>> childIndices;
};

struct CatalogCacheEntry {
	std::shared_ptr<const ZipCatalog> catalog;
	std::uint64_t lastUsed = 0;
};

struct CatalogCache {
	std::mutex mutex;
	std::unordered_map<std::string, CatalogCacheEntry> entries;
	std::uint64_t useCounter = 0;
};

CatalogCache& GlobalCatalogCache() {
	static CatalogCache cache;
	return cache;
}

bool AddCatalogChild(ZipCatalog& catalog, const std::string& parent,
	CatalogChild child) {
	auto& indices = catalog.childIndices[parent];
	const auto found = indices.find(child.name);
	if (found != indices.end()) {
		CatalogChild& current = catalog.directories[parent][found->second];
		if (current.directory == child.directory && current.modificationTime == 0 &&
			child.modificationTime != 0) {
			current.modificationTime = child.modificationTime;
		}
		return current.directory == child.directory;
	}
	const std::size_t index = catalog.directories[parent].size();
	indices.emplace(child.name, index);
	catalog.directories[parent].push_back(std::move(child));
	return true;
}

bool AddDirectoryChain(ZipCatalog& catalog, const std::vector<std::string>& components,
	std::size_t count) {
	std::string parent;
	for (std::size_t index = 0; index < count; ++index) {
		const std::string fullName = parent.empty() ? components[index] : parent + "/" + components[index];
		if (!AddCatalogChild(catalog, parent,
			CatalogChild{components[index], fullName, true, 0, 0})) return false;
		catalog.directories.try_emplace(fullName);
		parent = fullName;
	}
	return true;
}

bool LoadCatalog(const fs::path& archivePath, const BackingIdentity& expectedIdentity,
	std::shared_ptr<const ZipCatalog>& result, std::string& errorMessage) {
	int zipError = 0;
	zip_t* archive = zip_open(archivePath.c_str(), ZIP_RDONLY, &zipError);
	if (archive == nullptr) {
		errorMessage = ZipOpenError(zipError);
		return false;
	}
	std::unique_ptr<zip_t, decltype(&zip_discard)> archiveOwner(archive, &zip_discard);
	const zip_int64_t entryCount = zip_get_num_entries(archive, ZIP_FL_UNCHANGED);
	if (entryCount < 0) {
		errorMessage = ZipError(archive);
		return false;
	}
	if (static_cast<std::uint64_t>(entryCount) > kMaximumArchiveEntries) {
		errorMessage = "archive has more than 100000 entries";
		return false;
	}
	std::shared_ptr<ZipCatalog> catalog;
	try {
		catalog = std::make_shared<ZipCatalog>();
		catalog->archive = archivePath;
		catalog->identity = expectedIdentity;
		catalog->directories.try_emplace("");
		for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entryCount); ++index) {
			zip_stat_t status{};
			zip_stat_init(&status);
			if (zip_stat_index(archive, index, ZIP_FL_UNCHANGED | ZIP_FL_ENC_RAW, &status) != 0 ||
				(status.valid & ZIP_STAT_NAME) == 0) continue;
			const char* rawName = zip_get_name(archive, index, ZIP_FL_UNCHANGED | ZIP_FL_ENC_RAW);
			if (rawName == nullptr) continue;
			std::string name;
			const std::size_t rawNameLength = std::strlen(rawName);
			const bool directory = rawNameLength > 0 && rawName[rawNameLength - 1] == '/';
			if (!SafeMemberPath(std::string_view(rawName, rawNameLength), directory, name)) continue;
			if (name.empty()) continue;

			std::vector<std::string> components;
			std::size_t start = 0;
			while (start < name.size()) {
				const std::size_t slash = name.find('/', start);
				const std::size_t end = slash == std::string::npos ? name.size() : slash;
				components.push_back(name.substr(start, end - start));
				if (slash == std::string::npos) break;
				start = slash + 1;
			}
			if (components.empty() || !AddDirectoryChain(*catalog, components,
				components.size() - 1)) continue;
			const std::string parent = components.size() > 1 ? [&components] {
				std::string value;
				for (std::size_t part = 0; part + 1 < components.size(); ++part) {
					if (!value.empty()) value.push_back('/');
					value += components[part];
				}
				return value;
			}() : std::string();
			const std::string& basename = components.back();
			const std::int64_t modificationTime = (status.valid & ZIP_STAT_MTIME) != 0 ?
				static_cast<std::int64_t>(status.mtime) : 0;
			if (directory) {
				const std::string fullName = parent.empty() ? basename : parent + "/" + basename;
				if (AddCatalogChild(*catalog, parent,
					CatalogChild{basename, fullName, true, 0, modificationTime})) {
					catalog->directories.try_emplace(fullName);
				}
				continue;
			}
			if ((status.valid & ZIP_STAT_SIZE) == 0 || (status.valid & ZIP_STAT_INDEX) == 0) continue;
			const std::string fullName = parent.empty() ? basename : parent + "/" + basename;
			ZipMemberInfo info;
			info.size = status.size;
			info.compressedSize = (status.valid & ZIP_STAT_COMP_SIZE) != 0 ? status.comp_size : 0;
			info.crc = (status.valid & ZIP_STAT_CRC) != 0 ? status.crc : 0;
			info.index = status.index;
			info.modificationTime = modificationTime;
			if (AddCatalogChild(*catalog, parent,
				CatalogChild{basename, fullName, false, info.size, modificationTime})) {
				catalog->members.emplace(fullName, info);
			}
		}
	} catch (const std::bad_alloc&) {
		errorMessage = "not enough memory to index ZIP directory";
		return false;
	}
	BackingIdentity afterLoad;
	if (!StatIdentity(archivePath, afterLoad) || !(afterLoad == expectedIdentity)) {
		errorMessage = "ZIP archive changed while it was being indexed";
		return false;
	}
	result = std::move(catalog);
	return true;
}

std::shared_ptr<const ZipCatalog> GetCatalog(const fs::path& archivePath,
	std::string& errorMessage) {
	BackingIdentity identity;
	if (!StatIdentity(archivePath, identity)) {
		errorMessage = "cannot read ZIP archive metadata";
		return {};
	}
	const std::string key = archivePath.lexically_normal().string();
	CatalogCache& cache = GlobalCatalogCache();
	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		const auto found = cache.entries.find(key);
		if (found != cache.entries.end() && found->second.catalog->identity == identity) {
			found->second.lastUsed = ++cache.useCounter;
			return found->second.catalog;
		}
	}
	std::shared_ptr<const ZipCatalog> loaded;
	if (!LoadCatalog(archivePath, identity, loaded, errorMessage)) return {};
	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		cache.entries[key] = CatalogCacheEntry{loaded, ++cache.useCounter};
		constexpr std::size_t kMaximumCachedArchives = 4;
		while (cache.entries.size() > kMaximumCachedArchives) {
			auto oldest = cache.entries.begin();
			for (auto candidate = std::next(cache.entries.begin()); candidate != cache.entries.end(); ++candidate) {
				if (candidate->second.lastUsed < oldest->second.lastUsed) oldest = candidate;
			}
			cache.entries.erase(oldest);
		}
	}
	return loaded;
}

struct PrivateMemberFile {
	int descriptor = -1;
	fs::path directory;
	fs::path link;

	~PrivateMemberFile() {
		if (!link.empty()) (void)::unlink(link.c_str());
		if (!directory.empty()) (void)::rmdir(directory.c_str());
		if (descriptor >= 0) (void)::close(descriptor);
	}
};

bool WriteAll(int descriptor, const std::uint8_t* bytes, std::size_t length,
	std::string& errorMessage) {
	std::size_t written = 0;
	while (written < length) {
		const ssize_t result = ::write(descriptor, bytes + written, length - written);
		if (result < 0 && errno == EINTR) continue;
		if (result <= 0) {
			errorMessage = "cannot write ZIP member to memory file";
			return false;
		}
		written += static_cast<std::size_t>(result);
	}
	return true;
}

bool MakePrivateMemberFile(const std::filesystem::path& memberPath,
	PrivateMemberFile& temporary, std::string& errorMessage) {
#if !defined(SYS_memfd_create)
	(void)memberPath;
	(void)temporary;
	errorMessage = "anonymous memory files are not available on this Linux system";
	return false;
#else
	temporary.descriptor = static_cast<int>(::syscall(SYS_memfd_create,
		"jpegview-zip-member", MFD_CLOEXEC));
	if (temporary.descriptor < 0) {
		errorMessage = "cannot create anonymous memory file for ZIP member";
		return false;
	}
	std::error_code temporaryError;
	const fs::path temporaryRoot = fs::temp_directory_path(temporaryError);
	if (temporaryError || temporaryRoot.empty()) {
		errorMessage = "cannot find a temporary directory for ZIP member";
		return false;
	}
	std::string pattern = (temporaryRoot / "jpegview-zip-XXXXXX").string();
	std::vector<char> patternBytes(pattern.begin(), pattern.end());
	patternBytes.push_back('\0');
	char* createdDirectory = ::mkdtemp(patternBytes.data());
	if (createdDirectory == nullptr) {
		errorMessage = "cannot create private temporary directory for ZIP member";
		return false;
	}
	temporary.directory = createdDirectory;
	std::string extension = memberPath.extension().string();
	if (extension.size() > 16) extension.clear();
	temporary.link = temporary.directory / ("image" + extension);
	const std::string descriptorPath = "/proc/self/fd/" + std::to_string(temporary.descriptor);
	if (::symlink(descriptorPath.c_str(), temporary.link.c_str()) != 0) {
		errorMessage = "cannot expose ZIP member to the image decoder";
		return false;
	}
	return true;
#endif
}

} // namespace

bool IsArchiveContainerName(const fs::path& path) {
	return HasZipExtension(path);
}

bool IsArchiveContainerFile(const fs::path& path) {
	if (!HasZipExtension(path)) return false;
	std::error_code error;
	return fs::is_regular_file(path, error) && !error;
}

bool IsArchiveLocation(const fs::path& path) {
	ZipLocation location;
	return ParseZipLocation(path, location);
}

bool IsArchiveMemberLocation(const fs::path& path) {
	ZipLocation location;
	return ParseZipLocation(path, location) && !location.memberDirectory.empty();
}

std::string ArchiveFormatName(const fs::path& path) {
	ZipLocation location;
	if (!ParseZipLocation(path, location)) return {};
	return "ZIP";
}

namespace {

std::int64_t FileClockUnixOffsetNanoseconds() {
	static const std::int64_t offset = [] {
		const std::chrono::system_clock::time_point systemBefore =
			std::chrono::system_clock::now();
		const fs::file_time_type fileNow = fs::file_time_type::clock::now();
		const std::chrono::system_clock::time_point systemAfter =
			std::chrono::system_clock::now();
		const std::chrono::system_clock::time_point systemMidpoint =
			systemBefore + (systemAfter - systemBefore) / 2;
		const std::int64_t fileNanoseconds =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				fileNow.time_since_epoch()).count();
		const std::int64_t systemNanoseconds =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				systemMidpoint.time_since_epoch()).count();
		if (systemNanoseconds > 0 &&
			fileNanoseconds < std::numeric_limits<std::int64_t>::min() + systemNanoseconds) {
			return std::numeric_limits<std::int64_t>::min();
		}
		if (systemNanoseconds < 0 &&
			fileNanoseconds > std::numeric_limits<std::int64_t>::max() + systemNanoseconds) {
			return std::numeric_limits<std::int64_t>::max();
		}
		return fileNanoseconds - systemNanoseconds;
	}();
	return offset;
}

} // namespace

std::int64_t ArchiveTimestampNanoseconds(std::int64_t seconds) {
	constexpr std::int64_t kNanosecondsPerSecond = 1000000000ll;
	constexpr std::int64_t kMaximumSeconds =
		std::numeric_limits<std::int64_t>::max() / kNanosecondsPerSecond;
	constexpr std::int64_t kMinimumSeconds =
		std::numeric_limits<std::int64_t>::min() / kNanosecondsPerSecond;
	if (seconds > kMaximumSeconds) return std::numeric_limits<std::int64_t>::max();
	if (seconds < kMinimumSeconds) return std::numeric_limits<std::int64_t>::min();
	const std::int64_t unixNanoseconds = seconds * kNanosecondsPerSecond;
	const std::int64_t offset = FileClockUnixOffsetNanoseconds();
	if (offset > 0 && unixNanoseconds > std::numeric_limits<std::int64_t>::max() - offset) {
		return std::numeric_limits<std::int64_t>::max();
	}
	if (offset < 0 && unixNanoseconds < std::numeric_limits<std::int64_t>::min() - offset) {
		return std::numeric_limits<std::int64_t>::min();
	}
	return unixNanoseconds + offset;
}

fs::file_time_type ArchiveFileModificationTime(std::int64_t seconds) {
	return fs::file_time_type(std::chrono::duration_cast<fs::file_time_type::duration>(
		std::chrono::nanoseconds(ArchiveTimestampNanoseconds(seconds))));
}

fs::path ArchiveBackingFile(const fs::path& path) {
	ZipLocation location;
	return ParseZipLocation(path, location) ? location.archive : path;
}

std::string ArchiveLocationDisplayName(const fs::path& path) {
	ZipLocation location;
	if (!ParseZipLocation(path, location)) return path.string();
	const std::string format = ArchiveFormatName(path);
	if (location.memberDirectory.empty()) return location.archive.string() + " (" + format + ")";
	return location.archive.string() + "!/" + location.memberDirectory;
}

std::uintmax_t ImageSourceFileSize(const fs::path& path, std::error_code& error) {
	error.clear();
	if (IsArchiveMemberLocation(path)) {
		ArchiveMemberInfo info;
		std::string message;
		if (GetArchiveMemberInfo(path, info, message)) return static_cast<std::uintmax_t>(info.size);
		error = std::make_error_code(std::errc::io_error);
		return 0;
	}
	return fs::file_size(path, error);
}

fs::file_time_type ImageSourceModificationTime(const fs::path& path, std::error_code& error) {
	error.clear();
	if (IsArchiveMemberLocation(path)) {
		ArchiveMemberInfo info;
		std::string message;
		if (!GetArchiveMemberInfo(path, info, message)) {
			error = std::make_error_code(std::errc::io_error);
			return {};
		}
		return ArchiveFileModificationTime(info.modificationTime);
	}
	return fs::last_write_time(path, error);
}

bool ListArchiveDirectory(const fs::path& directory, std::vector<ArchiveEntryInfo>& entries,
	std::string& errorMessage) {
	entries.clear();
	errorMessage.clear();
	ZipLocation location;
	if (!ParseZipLocation(directory, location)) return false;
	const std::shared_ptr<const ZipCatalog> catalog = GetCatalog(location.archive, errorMessage);
	if (!catalog) return false;
	const auto children = catalog->directories.find(location.memberDirectory);
	if (children == catalog->directories.end()) return true;
	entries.reserve(children->second.size());
	for (const CatalogChild& child : children->second) {
		const std::string member = location.memberDirectory.empty() ? child.name :
			location.memberDirectory + "/" + child.name;
		entries.push_back(ArchiveEntryInfo{
			location.archive / fs::path(member), child.directory, child.size,
			child.modificationTime});
	}
	return true;
}

bool GetArchiveMemberInfo(const fs::path& path, ArchiveMemberInfo& info,
	std::string& errorMessage) {
	info = {};
	errorMessage.clear();
	ZipLocation location;
	if (!ParseZipLocation(path, location) || location.memberDirectory.empty()) {
		errorMessage = "not a ZIP image member";
		return false;
	}
	const std::shared_ptr<const ZipCatalog> catalog = GetCatalog(location.archive, errorMessage);
	if (!catalog) return false;
	const auto found = catalog->members.find(location.memberDirectory);
	if (found == catalog->members.end()) {
		errorMessage = "ZIP member no longer exists";
		return false;
	}
	info.size = found->second.size;
	info.modificationTime = found->second.modificationTime;
	return true;
}

bool WithArchiveMemberFile(const fs::path& path,
	const std::function<bool(const fs::path&, std::string&)>& callback,
	std::string& errorMessage) {
	errorMessage.clear();
	if (!callback) {
		errorMessage = "ZIP member decoder callback is missing";
		return false;
	}
	ZipLocation location;
	if (!ParseZipLocation(path, location) || location.memberDirectory.empty()) {
		errorMessage = "not a ZIP image member";
		return false;
	}
	const std::shared_ptr<const ZipCatalog> catalog = GetCatalog(location.archive, errorMessage);
	if (!catalog) return false;
	const auto member = catalog->members.find(location.memberDirectory);
	if (member == catalog->members.end()) {
		errorMessage = "ZIP member no longer exists";
		return false;
	}
	if (member->second.size > kMaximumArchiveMemberBytes) {
		errorMessage = "archive image exceeds the 128 MiB member limit";
		return false;
	}
	BackingIdentity currentIdentity;
	if (!StatIdentity(location.archive, currentIdentity) || !(currentIdentity == catalog->identity)) {
		errorMessage = "ZIP archive changed before the image could be read";
		return false;
	}
	int zipError = 0;
	zip_t* archive = zip_open(location.archive.c_str(), ZIP_RDONLY, &zipError);
	if (archive == nullptr) {
		errorMessage = ZipOpenError(zipError);
		return false;
	}
	std::unique_ptr<zip_t, decltype(&zip_discard)> archiveOwner(archive, &zip_discard);
	zip_stat_t status{};
	zip_stat_init(&status);
	if (zip_stat_index(archive, member->second.index, ZIP_FL_UNCHANGED | ZIP_FL_ENC_RAW,
		&status) != 0 || (status.valid & ZIP_STAT_NAME) == 0 || status.name == nullptr ||
		location.memberDirectory != status.name ||
		(status.valid & ZIP_STAT_SIZE) == 0 || status.size != member->second.size ||
		((status.valid & ZIP_STAT_CRC) != 0 && status.crc != member->second.crc)) {
		errorMessage = "ZIP member changed since it was listed";
		return false;
	}
	zip_file_t* zipFile = zip_fopen_index(archive, member->second.index, ZIP_FL_UNCHANGED);
	if (zipFile == nullptr) {
		errorMessage = ZipError(archive);
		return false;
	}
	std::unique_ptr<zip_file_t, decltype(&zip_fclose)> zipFileOwner(zipFile, &zip_fclose);
	PrivateMemberFile temporary;
	if (!MakePrivateMemberFile(path, temporary, errorMessage)) return false;
	std::vector<std::uint8_t> buffer(64 * 1024);
	std::uint64_t total = 0;
	for (;;) {
		const zip_int64_t count = zip_fread(zipFile, buffer.data(), buffer.size());
		if (count < 0) {
			errorMessage = ZipError(archive);
			return false;
		}
		if (count == 0) break;
		const std::uint64_t chunk = static_cast<std::uint64_t>(count);
		if (total > member->second.size || chunk > kMaximumArchiveMemberBytes - total ||
			chunk > member->second.size - total) {
			errorMessage = "archive member expanded beyond its declared size or the 128 MiB limit";
			return false;
		}
		if (!WriteAll(temporary.descriptor, buffer.data(), static_cast<std::size_t>(count),
			errorMessage)) return false;
		total += chunk;
	}
	if (total != member->second.size) {
		errorMessage = "archive member ended before its declared size";
		return false;
	}
	if (zip_fclose(zipFileOwner.release()) != 0) {
		errorMessage = ZipError(archive);
		return false;
	}
	BackingIdentity afterRead;
	if (!StatIdentity(location.archive, afterRead) || !(afterRead == catalog->identity)) {
		errorMessage = "ZIP archive changed while the image was being read";
		return false;
	}
	if (::lseek(temporary.descriptor, 0, SEEK_SET) < 0) {
		errorMessage = "cannot rewind the archive image memory file";
		return false;
	}
	try {
		return callback(temporary.link, errorMessage);
	} catch (const std::exception& error) {
		errorMessage = error.what();
		return false;
	}
}

bool IdentifyImageSourceBackingFile(const fs::path& path, std::uint64_t& device,
	std::uint64_t& inode, std::uint64_t& size, std::int64_t& modifiedSeconds,
	std::int64_t& modifiedNanoseconds) {
	BackingIdentity identity;
	if (!StatIdentity(ArchiveBackingFile(path), identity)) return false;
	device = identity.device;
	inode = identity.inode;
	size = identity.size;
	modifiedSeconds = identity.modifiedSeconds;
	modifiedNanoseconds = identity.modifiedNanoseconds;
	return true;
}

} // namespace jpegview_linux
