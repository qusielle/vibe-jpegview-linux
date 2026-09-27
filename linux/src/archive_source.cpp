#include "archive_source.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <archive.h>
#include <archive_entry.h>
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

enum class ArchiveFormat {
	Zip,
	Tar,
	Tgz,
};

struct ArchiveLocation {
	fs::path archive;
	std::string memberDirectory;
	ArchiveFormat format = ArchiveFormat::Zip;
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

bool ShouldContinue(const std::function<bool()>& shouldContinue) {
	return !shouldContinue || shouldContinue();
}

ArchiveFormat FormatForContainerName(const fs::path& path) {
	const std::string name = Lower(path.filename().string());
	if (name.size() >= 7 && name.compare(name.size() - 7, 7, ".tar.gz") == 0) {
		return ArchiveFormat::Tgz;
	}
	if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tgz") == 0) {
		return ArchiveFormat::Tgz;
	}
	if (Lower(path.extension().string()) == ".zip") {
		return ArchiveFormat::Zip;
	}
	if (Lower(path.extension().string()) == ".tar") return ArchiveFormat::Tar;
	return ArchiveFormat::Zip;
}

bool HasArchiveExtension(const fs::path& path) {
	const std::string name = Lower(path.filename().string());
	return FormatForContainerName(path) != ArchiveFormat::Zip ||
		(name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0);
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

bool SafeTarMemberPath(std::string_view name, bool directory, std::string& normalized) {
	normalized.clear();
	if (name.empty() || name.front() == '/' || name.find('\0') != std::string_view::npos) return false;
	if (directory && name.back() == '/') name.remove_suffix(1);
	if (name.empty()) return false;
	std::size_t start = 0;
	while (start < name.size()) {
		const std::size_t slash = name.find('/', start);
		const std::size_t end = slash == std::string_view::npos ? name.size() : slash;
		const std::string_view component = name.substr(start, end - start);
		if (component.empty() || component == "..") return false;
		if (component != ".") {
			if (!normalized.empty()) normalized.push_back('/');
			normalized.append(component.data(), component.size());
		}
		if (slash == std::string_view::npos) break;
		start = slash + 1;
		if (start == name.size()) return false;
	}
	return !normalized.empty() && normalized.size() <= 4096;
}

bool ParseArchiveLocation(const fs::path& path, ArchiveLocation& location) {
	const std::string text = path.lexically_normal().string();
	if (text.empty()) return false;
	std::size_t componentStart = text.front() == '/' ? 1 : 0;
	while (componentStart < text.size()) {
		std::size_t componentEnd = text.find('/', componentStart);
		if (componentEnd == std::string::npos) componentEnd = text.size();
		const std::string component = text.substr(componentStart, componentEnd - componentStart);
		const ArchiveFormat format = FormatForContainerName(fs::path(component));
		if (!component.empty() && HasArchiveExtension(fs::path(component))) {
			const fs::path candidate(text.substr(0, componentEnd));
			std::error_code statusError;
			if (fs::is_regular_file(candidate, statusError) && !statusError) {
				std::string member;
				if (componentEnd < text.size()) {
					const std::string_view remainder(text.data() + componentEnd + 1,
						text.size() - componentEnd - 1);
					if (!remainder.empty()) {
						const bool safe = format == ArchiveFormat::Tar || format == ArchiveFormat::Tgz ?
							SafeTarMemberPath(remainder, true, member) :
							SafeMemberPath(remainder, true, member);
						if (!safe) return false;
					}
				}
				location.archive = candidate;
				location.memberDirectory = std::move(member);
				location.format = format;
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

enum class ArchiveBackend {
	Zip,
	Tar,
};

struct ArchiveMemberRecord {
	std::uint64_t size = 0;
	std::uint64_t compressedSize = 0;
	std::uint32_t crc = 0;
	std::uint64_t index = 0;
	std::int64_t modificationTime = 0;
	ArchiveBackend backend = ArchiveBackend::Zip;
};

struct ArchiveCatalog {
	fs::path archive;
	BackingIdentity identity;
	ArchiveFormat format = ArchiveFormat::Zip;
	std::unordered_map<std::string, ArchiveMemberRecord> members;
	std::unordered_map<std::string, std::vector<CatalogChild>> directories;
	std::unordered_map<std::string,
		std::unordered_map<std::string, std::size_t>> childIndices;
};

struct CatalogCacheEntry {
	std::shared_ptr<const ArchiveCatalog> catalog;
	std::uint64_t lastUsed = 0;
};

struct CatalogLoadState {
	BackingIdentity identity;
	std::condition_variable condition;
	std::shared_ptr<const ArchiveCatalog> catalog;
	std::string error;
	bool done = false;
};

struct CatalogCache {
	std::mutex mutex;
	std::unordered_map<std::string, CatalogCacheEntry> entries;
	std::unordered_map<std::string, std::shared_ptr<CatalogLoadState>> loading;
	std::uint64_t useCounter = 0;
};

CatalogCache& GlobalCatalogCache() {
	static CatalogCache cache;
	return cache;
}

bool AddCatalogChild(ArchiveCatalog& catalog, const std::string& parent,
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

bool AddDirectoryChain(ArchiveCatalog& catalog, const std::vector<std::string>& components,
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

bool LoadZipCatalog(const fs::path& archivePath, const BackingIdentity& expectedIdentity,
	std::shared_ptr<const ArchiveCatalog>& result, const std::function<bool()>& shouldContinue,
	std::string& errorMessage) {
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
	std::shared_ptr<ArchiveCatalog> catalog;
	try {
		catalog = std::make_shared<ArchiveCatalog>();
		catalog->archive = archivePath;
		catalog->identity = expectedIdentity;
		catalog->format = ArchiveFormat::Zip;
		catalog->directories.try_emplace("");
		for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entryCount); ++index) {
			if (!ShouldContinue(shouldContinue)) {
				errorMessage = "archive indexing was cancelled";
				return false;
			}
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
			ArchiveMemberRecord info;
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

struct TarReaderContext {
	fs::path path;
	int descriptor = -1;
	std::array<std::uint8_t, 64 * 1024> buffer{};
	const std::function<bool()>* shouldContinue = nullptr;
	bool cancelled = false;
};

int OpenTarInput(struct archive* reader, void* clientData) {
	TarReaderContext& context = *static_cast<TarReaderContext*>(clientData);
	context.descriptor = ::open(context.path.c_str(), O_RDONLY | O_CLOEXEC);
	if (context.descriptor >= 0) return ARCHIVE_OK;
	archive_set_error(reader, errno, "cannot open archive input");
	return ARCHIVE_FATAL;
}

la_ssize_t ReadTarInput(struct archive* reader, void* clientData, const void** buffer) {
	TarReaderContext& context = *static_cast<TarReaderContext*>(clientData);
	if (context.shouldContinue != nullptr && !ShouldContinue(*context.shouldContinue)) {
		context.cancelled = true;
		archive_set_error(reader, ECANCELED, "archive input was cancelled");
		return -1;
	}
	for (;;) {
		const ssize_t count = ::read(context.descriptor, context.buffer.data(), context.buffer.size());
		if (count < 0 && errno == EINTR) continue;
		if (count < 0) {
			archive_set_error(reader, errno, "cannot read archive input");
			return -1;
		}
		*buffer = context.buffer.data();
		return static_cast<la_ssize_t>(count);
	}
}

la_int64_t SkipTarInput(struct archive*, void* clientData, la_int64_t request) {
	TarReaderContext& context = *static_cast<TarReaderContext*>(clientData);
	if (request <= 0 || (context.shouldContinue != nullptr && !ShouldContinue(*context.shouldContinue)) ||
		static_cast<std::uint64_t>(request) >
			static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) return 0;
	const off_t start = ::lseek(context.descriptor, 0, SEEK_CUR);
	if (start < 0) return 0;
	const off_t end = ::lseek(context.descriptor, static_cast<off_t>(request), SEEK_CUR);
	return end < start ? 0 : static_cast<la_int64_t>(end - start);
}

int CloseTarInput(struct archive*, void* clientData) {
	TarReaderContext& context = *static_cast<TarReaderContext*>(clientData);
	if (context.descriptor >= 0) {
		(void)::close(context.descriptor);
		context.descriptor = -1;
	}
	return ARCHIVE_OK;
}

struct TarArchiveReader {
	TarReaderContext context;
	struct archive* reader = nullptr;

	~TarArchiveReader() {
		if (reader != nullptr) {
			(void)archive_read_close(reader);
			(void)archive_read_free(reader);
		}
		if (context.descriptor >= 0) (void)::close(context.descriptor);
	}
};

bool OpenTarReader(const fs::path& path, ArchiveFormat format,
	const std::function<bool()>& shouldContinue, TarArchiveReader& owner,
	std::string& errorMessage) {
	owner.context.path = path;
	owner.context.shouldContinue = &shouldContinue;
	owner.reader = archive_read_new();
	if (owner.reader == nullptr) {
		errorMessage = "not enough memory to open TAR archive";
		return false;
	}
	if (archive_read_support_format_tar(owner.reader) != ARCHIVE_OK ||
		archive_read_support_filter_none(owner.reader) != ARCHIVE_OK ||
		(format == ArchiveFormat::Tgz &&
			archive_read_support_filter_gzip(owner.reader) != ARCHIVE_OK)) {
		errorMessage = archive_error_string(owner.reader) == nullptr ?
			"TAR support is unavailable" : archive_error_string(owner.reader);
		return false;
	}
	const archive_skip_callback* skip = format == ArchiveFormat::Tar ? SkipTarInput : nullptr;
	if (archive_read_open2(owner.reader, &owner.context, OpenTarInput, ReadTarInput,
		skip, CloseTarInput) != ARCHIVE_OK) {
		errorMessage = owner.context.cancelled ? "archive indexing was cancelled" :
			archive_error_string(owner.reader) == nullptr ? "cannot open TAR archive" :
			archive_error_string(owner.reader);
		return false;
	}
	return true;
}

const char* TarEntryPathname(struct archive_entry* entry) {
	const char* pathname = archive_entry_pathname_utf8(entry);
	return pathname == nullptr ? archive_entry_pathname(entry) : pathname;
}

bool TarEntryPath(struct archive_entry* entry, std::string& normalized,
	bool& directory) {
	const char* rawName = TarEntryPathname(entry);
	if (rawName == nullptr) return false;
	const std::size_t rawLength = std::strlen(rawName);
	const bool pathDirectory = rawLength > 0 && rawName[rawLength - 1] == '/';
	const mode_t type = archive_entry_filetype(entry);
	if (archive_entry_symlink(entry) != nullptr || archive_entry_hardlink(entry) != nullptr) {
		return false;
	}
	if (type == AE_IFDIR) directory = true;
	else if (type == AE_IFREG) directory = pathDirectory;
	else return false;
	return SafeTarMemberPath(std::string_view(rawName, rawLength), directory, normalized);
}

bool LoadTarCatalog(const fs::path& archivePath, ArchiveFormat format,
	const BackingIdentity& expectedIdentity, std::shared_ptr<const ArchiveCatalog>& result,
	const std::function<bool()>& shouldContinue, std::string& errorMessage) {
	TarArchiveReader archive;
	if (!OpenTarReader(archivePath, format, shouldContinue, archive, errorMessage)) return false;
	std::shared_ptr<ArchiveCatalog> catalog;
	try {
		catalog = std::make_shared<ArchiveCatalog>();
		catalog->archive = archivePath;
		catalog->identity = expectedIdentity;
		catalog->format = format;
		catalog->directories.try_emplace("");
		std::uint64_t ordinal = 0;
		for (;;) {
			if (!ShouldContinue(shouldContinue)) {
				errorMessage = "archive indexing was cancelled";
				return false;
			}
			struct archive_entry* entry = nullptr;
			const int resultCode = archive_read_next_header(archive.reader, &entry);
			if (resultCode == ARCHIVE_EOF) break;
			if (resultCode != ARCHIVE_OK || entry == nullptr) {
				errorMessage = archive.context.cancelled ? "archive indexing was cancelled" :
					archive_error_string(archive.reader) == nullptr ? "invalid TAR archive" :
					archive_error_string(archive.reader);
				return false;
			}
			if (ordinal >= kMaximumArchiveEntries) {
				errorMessage = "archive has more than 100000 entries";
				return false;
			}
			const std::uint64_t currentOrdinal = ordinal++;
			std::string name;
			bool directory = false;
			if (TarEntryPath(entry, name, directory)) {
				std::vector<std::string> components;
				std::size_t start = 0;
				while (start < name.size()) {
					const std::size_t slash = name.find('/', start);
					const std::size_t end = slash == std::string::npos ? name.size() : slash;
					components.push_back(name.substr(start, end - start));
					if (slash == std::string::npos) break;
					start = slash + 1;
				}
				if (!components.empty() && AddDirectoryChain(*catalog, components,
					components.size() - 1)) {
					std::string parent;
					for (std::size_t index = 0; index + 1 < components.size(); ++index) {
						if (!parent.empty()) parent.push_back('/');
						parent += components[index];
					}
					const std::string& basename = components.back();
					const la_int64_t rawModificationTime = archive_entry_mtime(entry);
					const std::int64_t modificationTime =
						archive_entry_mtime_is_set(entry) ? static_cast<std::int64_t>(rawModificationTime) : 0;
					const std::string fullName = parent.empty() ? basename : parent + "/" + basename;
					if (directory) {
						if (AddCatalogChild(*catalog, parent,
							CatalogChild{basename, fullName, true, 0, modificationTime})) {
							catalog->directories.try_emplace(fullName);
						}
					} else if (archive_entry_size_is_set(entry) && archive_entry_size(entry) >= 0) {
						ArchiveMemberRecord info;
						info.size = static_cast<std::uint64_t>(archive_entry_size(entry));
						info.index = currentOrdinal;
						info.modificationTime = modificationTime;
						info.backend = ArchiveBackend::Tar;
						if (AddCatalogChild(*catalog, parent,
							CatalogChild{basename, fullName, false, info.size, modificationTime})) {
							catalog->members.emplace(fullName, info);
						}
					}
				}
			}
			if (archive_read_data_skip(archive.reader) != ARCHIVE_OK) {
				errorMessage = archive.context.cancelled ? "archive indexing was cancelled" :
					archive_error_string(archive.reader) == nullptr ? "cannot skip TAR member data" :
					archive_error_string(archive.reader);
				return false;
			}
		}
	} catch (const std::bad_alloc&) {
		errorMessage = "not enough memory to index TAR directory";
		return false;
	}
	BackingIdentity afterLoad;
	if (!StatIdentity(archivePath, afterLoad) || !(afterLoad == expectedIdentity)) {
		errorMessage = "TAR archive changed while it was being indexed";
		return false;
	}
	result = std::move(catalog);
	return true;
}


std::shared_ptr<const ArchiveCatalog> GetCatalog(const fs::path& archivePath,
	const std::function<bool()>& shouldContinue, std::string& errorMessage) {
	BackingIdentity identity;
	if (!StatIdentity(archivePath, identity)) {
		errorMessage = "cannot read archive metadata";
		return {};
	}
	const std::string key = archivePath.lexically_normal().string();
	CatalogCache& cache = GlobalCatalogCache();
	ArchiveLocation location;
	if (!ParseArchiveLocation(archivePath, location)) {
		errorMessage = "unrecognized archive format";
		return {};
	}
	std::shared_ptr<CatalogLoadState> loadState;
	for (;;) {
		std::unique_lock<std::mutex> lock(cache.mutex);
		const auto cached = cache.entries.find(key);
		if (cached != cache.entries.end() && cached->second.catalog->identity == identity) {
			cached->second.lastUsed = ++cache.useCounter;
			return cached->second.catalog;
		}
		const auto loading = cache.loading.find(key);
		if (loading != cache.loading.end()) {
			loadState = loading->second;
			while (!loadState->done && ShouldContinue(shouldContinue)) {
				loadState->condition.wait_for(lock, std::chrono::milliseconds(20));
			}
			if (!ShouldContinue(shouldContinue)) {
				errorMessage = "archive indexing was cancelled";
				return {};
			}
			if (loadState->identity == identity && loadState->catalog) {
				return loadState->catalog;
			}
			if (loadState->identity == identity && !loadState->error.empty() &&
				loadState->error != "archive indexing was cancelled") {
				errorMessage = loadState->error;
				return {};
			}
			continue;
		}
		loadState = std::make_shared<CatalogLoadState>();
		loadState->identity = identity;
		cache.loading.emplace(key, loadState);
		break;
	}

	std::shared_ptr<const ArchiveCatalog> loaded;
	bool loadedSuccessfully = false;
	if (location.format == ArchiveFormat::Zip) {
		loadedSuccessfully = LoadZipCatalog(archivePath, identity, loaded,
			shouldContinue, errorMessage);
	} else if (!LoadTarCatalog(archivePath, location.format, identity, loaded,
		shouldContinue, errorMessage)) {
		loadedSuccessfully = false;
	} else {
		loadedSuccessfully = true;
	}
	if (loadedSuccessfully && !ShouldContinue(shouldContinue)) {
		loadedSuccessfully = false;
		errorMessage = "archive indexing was cancelled";
	}
	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		if (loadedSuccessfully) {
			cache.entries[key] = CatalogCacheEntry{loaded, ++cache.useCounter};
			constexpr std::size_t kMaximumCachedArchives = 4;
			while (cache.entries.size() > kMaximumCachedArchives) {
				auto oldest = cache.entries.begin();
				for (auto candidate = std::next(cache.entries.begin());
					candidate != cache.entries.end(); ++candidate) {
					if (candidate->second.lastUsed < oldest->second.lastUsed) oldest = candidate;
				}
				cache.entries.erase(oldest);
			}
		}
		loadState->catalog = loadedSuccessfully ? loaded : nullptr;
		loadState->error = loadedSuccessfully ? std::string{} : errorMessage;
		loadState->done = true;
		const auto activeLoad = cache.loading.find(key);
		if (activeLoad != cache.loading.end() && activeLoad->second == loadState) {
			cache.loading.erase(activeLoad);
		}
	}
	loadState->condition.notify_all();
	return loadedSuccessfully ? loaded : nullptr;
}

const std::function<bool()>& AlwaysContinue() {
	static const std::function<bool()> continueWork = [] { return true; };
	return continueWork;
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
			errorMessage = "cannot write archive member to memory file";
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
		"jpegview-archive-member", MFD_CLOEXEC));
	if (temporary.descriptor < 0) {
		errorMessage = "cannot create anonymous memory file for archive member";
		return false;
	}
	std::error_code temporaryError;
	const fs::path temporaryRoot = fs::temp_directory_path(temporaryError);
	if (temporaryError || temporaryRoot.empty()) {
		errorMessage = "cannot find a temporary directory for archive member";
		return false;
	}
	std::string pattern = (temporaryRoot / "jpegview-archive-XXXXXX").string();
	std::vector<char> patternBytes(pattern.begin(), pattern.end());
	patternBytes.push_back('\0');
	char* createdDirectory = ::mkdtemp(patternBytes.data());
	if (createdDirectory == nullptr) {
		errorMessage = "cannot create private temporary directory for archive member";
		return false;
	}
	temporary.directory = createdDirectory;
	std::string extension = memberPath.extension().string();
	if (extension.size() > 16) extension.clear();
	temporary.link = temporary.directory / ("image" + extension);
	const std::string descriptorPath = "/proc/self/fd/" + std::to_string(temporary.descriptor);
	if (::symlink(descriptorPath.c_str(), temporary.link.c_str()) != 0) {
		errorMessage = "cannot expose archive member to the image decoder";
		return false;
	}
	return true;
#endif
}

bool WithTarMemberFile(const fs::path& path, const ArchiveLocation& location,
	const ArchiveCatalog& catalog, const ArchiveMemberRecord& member,
	const std::function<bool(const fs::path&, std::string&)>& callback,
	std::string& errorMessage) {
	TarArchiveReader archive;
	if (!OpenTarReader(location.archive, location.format, AlwaysContinue(), archive, errorMessage)) {
		return false;
	}
	for (std::uint64_t ordinal = 0; ordinal <= member.index; ++ordinal) {
		struct archive_entry* entry = nullptr;
		const int resultCode = archive_read_next_header(archive.reader, &entry);
		if (resultCode != ARCHIVE_OK || entry == nullptr) {
			errorMessage = resultCode == ARCHIVE_EOF ? "archive member no longer exists" :
				archive_error_string(archive.reader) == nullptr ? "cannot read TAR member header" :
				archive_error_string(archive.reader);
			return false;
		}
		if (ordinal != member.index) {
			if (archive_read_data_skip(archive.reader) != ARCHIVE_OK) {
				errorMessage = archive_error_string(archive.reader) == nullptr ?
					"cannot skip TAR member data" : archive_error_string(archive.reader);
				return false;
			}
			continue;
		}
		std::string currentName;
		bool directory = false;
		const bool hasSafePath = TarEntryPath(entry, currentName, directory);
		const la_int64_t rawModificationTime = archive_entry_mtime(entry);
		const std::int64_t modificationTime = archive_entry_mtime_is_set(entry) ?
			static_cast<std::int64_t>(rawModificationTime) : 0;
		if (!hasSafePath || directory || currentName != location.memberDirectory ||
			!archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0 ||
			static_cast<std::uint64_t>(archive_entry_size(entry)) != member.size ||
			modificationTime != member.modificationTime) {
			errorMessage = "TAR member changed since it was listed";
			return false;
		}

		PrivateMemberFile temporary;
		if (!MakePrivateMemberFile(path, temporary, errorMessage)) return false;
		std::vector<std::uint8_t> buffer(64 * 1024);
		std::uint64_t total = 0;
		for (;;) {
			const la_ssize_t count = archive_read_data(archive.reader, buffer.data(), buffer.size());
			if (count < 0) {
				errorMessage = archive.context.cancelled ? "archive member read was cancelled" :
					archive_error_string(archive.reader) == nullptr ? "cannot read TAR member data" :
					archive_error_string(archive.reader);
				return false;
			}
			if (count == 0) break;
			const std::uint64_t chunk = static_cast<std::uint64_t>(count);
			if (total > member.size || chunk > kMaximumArchiveMemberBytes - total ||
				chunk > member.size - total) {
				errorMessage = "archive member expanded beyond its declared size or the 128 MiB limit";
				return false;
			}
			if (!WriteAll(temporary.descriptor, buffer.data(), static_cast<std::size_t>(count),
				errorMessage)) return false;
			total += chunk;
		}
		if (total != member.size) {
			errorMessage = "archive member ended before its declared size";
			return false;
		}
		BackingIdentity afterRead;
		if (!StatIdentity(location.archive, afterRead) || !(afterRead == catalog.identity)) {
			errorMessage = "TAR archive changed while the image was being read";
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
	errorMessage = "archive member no longer exists";
	return false;
}

} // namespace

bool IsArchiveContainerName(const fs::path& path) {
	return HasArchiveExtension(path);
}

bool IsArchiveContainerFile(const fs::path& path) {
	if (!HasArchiveExtension(path)) return false;
	std::error_code error;
	return fs::is_regular_file(path, error) && !error;
}

bool IsArchiveLocation(const fs::path& path) {
	ArchiveLocation location;
	return ParseArchiveLocation(path, location);
}

bool IsArchiveMemberLocation(const fs::path& path) {
	ArchiveLocation location;
	return ParseArchiveLocation(path, location) && !location.memberDirectory.empty();
}

std::string ArchiveFormatName(const fs::path& path) {
	ArchiveLocation location;
	if (!ParseArchiveLocation(path, location)) return {};
	switch (location.format) {
	case ArchiveFormat::Zip: return "ZIP";
	case ArchiveFormat::Tar: return "TAR";
	case ArchiveFormat::Tgz: return "TGZ";
	}
	return {};
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
	ArchiveLocation location;
	return ParseArchiveLocation(path, location) ? location.archive : path;
}

std::string ArchiveLocationDisplayName(const fs::path& path) {
	ArchiveLocation location;
	if (!ParseArchiveLocation(path, location)) return path.string();
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
	return ListArchiveDirectoryCancellable(directory, entries, AlwaysContinue(), errorMessage);
}

bool ListArchiveDirectoryCancellable(const fs::path& directory,
	std::vector<ArchiveEntryInfo>& entries, const std::function<bool()>& shouldContinue,
	std::string& errorMessage) {
	entries.clear();
	errorMessage.clear();
	ArchiveLocation location;
	if (!ParseArchiveLocation(directory, location)) return false;
	const std::shared_ptr<const ArchiveCatalog> catalog =
		GetCatalog(location.archive, shouldContinue, errorMessage);
	if (!catalog) return false;
	if (!ShouldContinue(shouldContinue)) {
		errorMessage = "archive listing was cancelled";
		return false;
	}
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
	ArchiveLocation location;
	if (!ParseArchiveLocation(path, location) || location.memberDirectory.empty()) {
		errorMessage = "not an archive image member";
		return false;
	}
	const std::shared_ptr<const ArchiveCatalog> catalog =
		GetCatalog(location.archive, AlwaysContinue(), errorMessage);
	if (!catalog) return false;
	const auto found = catalog->members.find(location.memberDirectory);
	if (found == catalog->members.end()) {
		errorMessage = "archive member no longer exists";
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
		errorMessage = "archive member decoder callback is missing";
		return false;
	}
	ArchiveLocation location;
	if (!ParseArchiveLocation(path, location) || location.memberDirectory.empty()) {
		errorMessage = "not an archive image member";
		return false;
	}
	const std::shared_ptr<const ArchiveCatalog> catalog =
		GetCatalog(location.archive, AlwaysContinue(), errorMessage);
	if (!catalog) return false;
	const auto member = catalog->members.find(location.memberDirectory);
	if (member == catalog->members.end()) {
		errorMessage = "archive member no longer exists";
		return false;
	}
	if (member->second.size > kMaximumArchiveMemberBytes) {
		errorMessage = "archive image exceeds the 128 MiB member limit";
		return false;
	}
	if (member->second.backend == ArchiveBackend::Tar) {
		BackingIdentity currentIdentity;
		if (!StatIdentity(location.archive, currentIdentity) || !(currentIdentity == catalog->identity)) {
			errorMessage = "TAR archive changed before the image could be read";
			return false;
		}
		return WithTarMemberFile(path, location, *catalog, member->second, callback, errorMessage);
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
