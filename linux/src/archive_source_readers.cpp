#include "archive_source_internal.h"

#include "perf_diagnostics.h"
#include "rar_backend.h"
#include "seven_zip_backend.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/memfd.h>
#endif

namespace jpegview_linux::archive_detail {

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
			const bool encrypted = (status.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 &&
				status.encryption_method != ZIP_EM_NONE;
			ArchiveMemberRecord info;
			info.size = status.size;
			info.compressedSize = (status.valid & ZIP_STAT_COMP_SIZE) != 0 ? status.comp_size : 0;
			info.crc = (status.valid & ZIP_STAT_CRC) != 0 ? status.crc : 0;
			info.index = status.index;
			info.modificationTime = modificationTime;
			info.encrypted = encrypted;
			catalog->containsEncryptedEntries = catalog->containsEncryptedEntries || encrypted;
			if (AddCatalogChild(*catalog, parent,
				CatalogChild{basename, fullName, false, info.size, modificationTime, encrypted})) {
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

struct LibarchiveReaderContext {
	fs::path path;
	int descriptor = -1;
	std::array<std::uint8_t, 64 * 1024> buffer{};
	const std::function<bool()>* shouldContinue = nullptr;
	bool cancelled = false;
};

int OpenArchiveInput(struct archive* reader, void* clientData) {
	LibarchiveReaderContext& context = *static_cast<LibarchiveReaderContext*>(clientData);
	context.descriptor = ::open(context.path.c_str(), O_RDONLY | O_CLOEXEC);
	if (context.descriptor >= 0) return ARCHIVE_OK;
	archive_set_error(reader, errno, "cannot open archive input");
	return ARCHIVE_FATAL;
}

la_ssize_t ReadArchiveInput(struct archive* reader, void* clientData, const void** buffer) {
	LibarchiveReaderContext& context = *static_cast<LibarchiveReaderContext*>(clientData);
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

la_int64_t SkipArchiveInput(struct archive*, void* clientData, la_int64_t request) {
	LibarchiveReaderContext& context = *static_cast<LibarchiveReaderContext*>(clientData);
	if (request <= 0 || (context.shouldContinue != nullptr && !ShouldContinue(*context.shouldContinue)) ||
		static_cast<std::uint64_t>(request) >
			static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) return 0;
	const off_t start = ::lseek(context.descriptor, 0, SEEK_CUR);
	if (start < 0) return 0;
	const off_t end = ::lseek(context.descriptor, static_cast<off_t>(request), SEEK_CUR);
	return end < start ? 0 : static_cast<la_int64_t>(end - start);
}

la_int64_t SeekArchiveInput(struct archive* reader, void* clientData,
	la_int64_t offset, int whence) {
	LibarchiveReaderContext& context = *static_cast<LibarchiveReaderContext*>(clientData);
	if (context.shouldContinue != nullptr && !ShouldContinue(*context.shouldContinue)) {
		context.cancelled = true;
		archive_set_error(reader, ECANCELED, "archive input was cancelled");
		return ARCHIVE_FATAL;
	}
	const off_t convertedOffset = static_cast<off_t>(offset);
	if (static_cast<la_int64_t>(convertedOffset) != offset) {
		archive_set_error(reader, EOVERFLOW, "archive seek offset is out of range");
		return ARCHIVE_FATAL;
	}
	const off_t position = ::lseek(context.descriptor, convertedOffset, whence);
	if (position < 0) {
		archive_set_error(reader, errno, "cannot seek archive input");
		return ARCHIVE_FATAL;
	}
	return static_cast<la_int64_t>(position);
}

int CloseArchiveInput(struct archive*, void* clientData) {
	LibarchiveReaderContext& context = *static_cast<LibarchiveReaderContext*>(clientData);
	if (context.descriptor >= 0) {
		(void)::close(context.descriptor);
		context.descriptor = -1;
	}
	return ARCHIVE_OK;
}

struct LibarchiveArchiveReader {
	LibarchiveReaderContext context;
	struct archive* reader = nullptr;

	~LibarchiveArchiveReader() {
		if (reader != nullptr) {
			(void)archive_read_close(reader);
			(void)archive_read_free(reader);
		}
		if (context.descriptor >= 0) (void)::close(context.descriptor);
	}
};

bool OpenArchiveReader(const fs::path& path, ArchiveFormat format,
	const std::function<bool()>& shouldContinue, LibarchiveArchiveReader& owner,
	std::string& errorMessage) {
	owner.context.path = path;
	owner.context.shouldContinue = &shouldContinue;
	owner.reader = archive_read_new();
	if (owner.reader == nullptr) {
		errorMessage = "not enough memory to open archive";
		return false;
	}
	const bool sevenZip = format == ArchiveFormat::SevenZip;
	int formatResult = ARCHIVE_OK;
	if (sevenZip) {
		formatResult = archive_read_support_format_7zip(owner.reader);
	} else if (format == ArchiveFormat::Rar) {
		formatResult = archive_read_support_format_rar(owner.reader);
		if (formatResult == ARCHIVE_OK) {
			formatResult = archive_read_support_format_rar5(owner.reader);
		}
	} else {
		formatResult = archive_read_support_format_tar(owner.reader);
	}
	if (formatResult != ARCHIVE_OK ||
		archive_read_support_filter_none(owner.reader) != ARCHIVE_OK ||
		(format == ArchiveFormat::Tgz &&
			archive_read_support_filter_gzip(owner.reader) != ARCHIVE_OK)) {
		errorMessage = archive_error_string(owner.reader) == nullptr ?
			"archive format support is unavailable" : archive_error_string(owner.reader);
		return false;
	}
	if (sevenZip && archive_read_set_seek_callback(owner.reader, SeekArchiveInput) != ARCHIVE_OK) {
		errorMessage = archive_error_string(owner.reader) == nullptr ?
			"seekable archive input is unavailable" : archive_error_string(owner.reader);
		return false;
	}
	const archive_skip_callback* skip = format == ArchiveFormat::Tar ? SkipArchiveInput : nullptr;
	if (archive_read_open2(owner.reader, &owner.context, OpenArchiveInput, ReadArchiveInput,
		skip, CloseArchiveInput) != ARCHIVE_OK) {
		errorMessage = owner.context.cancelled ? "archive indexing was cancelled" :
			archive_error_string(owner.reader) == nullptr ? "cannot open archive" :
			archive_error_string(owner.reader);
		return false;
	}
	return true;
}

const char* LibarchiveEntryPathname(struct archive_entry* entry) {
	const char* pathname = archive_entry_pathname_utf8(entry);
	return pathname == nullptr ? archive_entry_pathname(entry) : pathname;
}

bool LibarchiveEntryPath(struct archive_entry* entry, std::string& normalized,
	bool& directory, bool allowEncrypted = false, bool* encrypted = nullptr) {
	const char* rawName = LibarchiveEntryPathname(entry);
	if (rawName == nullptr) return false;
	const std::size_t rawLength = std::strlen(rawName);
	const bool pathDirectory = rawLength > 0 && rawName[rawLength - 1] == '/';
	const mode_t type = archive_entry_filetype(entry);
	const bool entryEncrypted = archive_entry_is_encrypted(entry) > 0;
	if (encrypted != nullptr) *encrypted = entryEncrypted;
	if (archive_entry_symlink(entry) != nullptr || archive_entry_hardlink(entry) != nullptr ||
		(entryEncrypted && !allowEncrypted)) {
		return false;
	}
	if (type == AE_IFDIR) directory = true;
	else if (type == AE_IFREG) directory = pathDirectory;
	else return false;
	return SafeLibarchiveMemberPath(std::string_view(rawName, rawLength), directory, normalized);
}

bool LoadLibarchiveCatalog(const fs::path& archivePath, ArchiveFormat format,
	const BackingIdentity& expectedIdentity, std::shared_ptr<const ArchiveCatalog>& result,
	const std::function<bool()>& shouldContinue, std::string& errorMessage) {
	LibarchiveArchiveReader archive;
	if (!OpenArchiveReader(archivePath, format, shouldContinue, archive, errorMessage)) return false;
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
					archive_error_string(archive.reader) == nullptr ? "invalid archive" :
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
			bool encrypted = false;
			if (LibarchiveEntryPath(entry, name, directory,
				format == ArchiveFormat::SevenZip, &encrypted)) {
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
						info.backend = ArchiveBackend::Libarchive;
						info.encrypted = encrypted;
						catalog->containsEncryptedEntries = catalog->containsEncryptedEntries || encrypted;
						if (AddCatalogChild(*catalog, parent,
							CatalogChild{basename, fullName, false, info.size, modificationTime, encrypted})) {
							catalog->members.emplace(fullName, info);
						}
					}
				}
			}
			if (archive_read_data_skip(archive.reader) != ARCHIVE_OK) {
				errorMessage = archive.context.cancelled ? "archive indexing was cancelled" :
					archive_error_string(archive.reader) == nullptr ? "cannot skip archive member data" :
					archive_error_string(archive.reader);
				return false;
			}
		}
	} catch (const std::bad_alloc&) {
		errorMessage = "not enough memory to index archive directory";
		return false;
	}
	BackingIdentity afterLoad;
	if (!StatIdentity(archivePath, afterLoad) || !(afterLoad == expectedIdentity)) {
		errorMessage = "archive changed while it was being indexed";
		return false;
	}
	result = std::move(catalog);
	return true;
}

bool LoadSevenZipCatalog(const fs::path& archivePath,
	const BackingIdentity& expectedIdentity,
	const std::optional<std::string>& password,
	std::shared_ptr<const ArchiveCatalog>& result,
	const std::function<bool()>& shouldContinue, std::string& errorMessage,
	ArchiveErrorKind* errorKind) {
	std::vector<SevenZipEntry> entries;
	bool headerEncrypted = false;
	if (!ReadSevenZipCatalog(archivePath, password, shouldContinue, entries,
		headerEncrypted, errorMessage, errorKind)) return false;
	std::shared_ptr<ArchiveCatalog> catalog;
	try {
		catalog = std::make_shared<ArchiveCatalog>();
		catalog->archive = archivePath;
		catalog->identity = expectedIdentity;
		catalog->format = ArchiveFormat::SevenZip;
		catalog->containsEncryptedEntries = headerEncrypted;
		catalog->headerEncrypted = headerEncrypted;
		catalog->directories.try_emplace("");
		for (const SevenZipEntry& entry : entries) {
			if (!ShouldContinue(shouldContinue)) {
				errorMessage = "archive indexing was cancelled";
				if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
				return false;
			}
			if (entry.specialFile) continue;
			std::string name;
			if (!SafeLibarchiveMemberPath(entry.rawPath, entry.directory, name)) continue;
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
			std::string parent;
			for (std::size_t index = 0; index + 1 < components.size(); ++index) {
				if (!parent.empty()) parent.push_back('/');
				parent += components[index];
			}
			const std::string& basename = components.back();
			const std::string fullName = parent.empty() ? basename : parent + "/" + basename;
			if (entry.directory) {
				if (AddCatalogChild(*catalog, parent,
					CatalogChild{basename, fullName, true, 0, entry.modificationTime})) {
					catalog->directories.try_emplace(fullName);
				}
				continue;
			}
			ArchiveMemberRecord member;
			member.size = entry.size;
			member.index = entry.index;
			member.modificationTime = entry.modificationTime;
			member.rawPath = entry.rawPath;
			member.encrypted = entry.encrypted;
			member.backend = ArchiveBackend::SevenZip;
			catalog->containsEncryptedEntries = catalog->containsEncryptedEntries || member.encrypted;
			if (AddCatalogChild(*catalog, parent,
				CatalogChild{basename, fullName, false, member.size,
					member.modificationTime, member.encrypted})) {
				catalog->members.emplace(fullName, std::move(member));
			}
		}
	} catch (const std::bad_alloc&) {
		errorMessage = "not enough memory to index 7z archive";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	BackingIdentity afterLoad;
	if (!StatIdentity(archivePath, afterLoad) || !(afterLoad == expectedIdentity)) {
		errorMessage = "7z archive changed while it was being indexed";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	result = std::move(catalog);
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	return true;
}

bool LoadRarCatalog(const fs::path& archivePath, const BackingIdentity& expectedIdentity,
	const std::optional<std::string>& password,
	std::shared_ptr<const ArchiveCatalog>& result,
	const std::function<bool()>& shouldContinue, std::string& errorMessage,
	ArchiveErrorKind* errorKind) {
	std::vector<RarEntry> entries;
	bool headerEncrypted = false;
	if (!ReadRarCatalog(archivePath, password, shouldContinue, entries,
		headerEncrypted, errorMessage, errorKind)) return false;
	std::shared_ptr<ArchiveCatalog> catalog;
	try {
		catalog = std::make_shared<ArchiveCatalog>();
		catalog->archive = archivePath;
		catalog->identity = expectedIdentity;
		catalog->format = ArchiveFormat::Rar;
		catalog->containsEncryptedEntries = headerEncrypted;
		catalog->headerEncrypted = headerEncrypted;
		catalog->directories.try_emplace("");
		for (const RarEntry& entry : entries) {
			if (!ShouldContinue(shouldContinue)) {
				errorMessage = "archive indexing was cancelled";
				if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
				return false;
			}
			if (entry.specialFile || entry.split) continue;
			std::string name;
			if (!SafeLibarchiveMemberPath(entry.rawPath, entry.directory, name)) continue;
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
			std::string parent;
			for (std::size_t index = 0; index + 1 < components.size(); ++index) {
				if (!parent.empty()) parent.push_back('/');
				parent += components[index];
			}
			const std::string& basename = components.back();
			const std::string fullName = parent.empty() ? basename : parent + "/" + basename;
			if (entry.directory) {
				if (AddCatalogChild(*catalog, parent,
					CatalogChild{basename, fullName, true, 0, entry.modificationTime})) {
					catalog->directories.try_emplace(fullName);
				}
				continue;
			}
			ArchiveMemberRecord member;
			member.size = entry.size;
			member.index = entry.index;
			member.modificationTime = entry.modificationTime;
			member.rawPath = entry.rawPath;
			member.encrypted = entry.encrypted;
			member.backend = ArchiveBackend::Rar;
			catalog->containsEncryptedEntries = catalog->containsEncryptedEntries || member.encrypted;
			if (AddCatalogChild(*catalog, parent,
				CatalogChild{basename, fullName, false, member.size,
					member.modificationTime, member.encrypted})) {
				catalog->members.emplace(fullName, std::move(member));
			}
		}
	} catch (const std::bad_alloc&) {
		errorMessage = "not enough memory to index RAR archive";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	BackingIdentity afterLoad;
	if (!StatIdentity(archivePath, afterLoad) || !(afterLoad == expectedIdentity)) {
		errorMessage = "RAR archive changed while it was being indexed";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	result = std::move(catalog);
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	return true;
}



bool WithLibarchiveMemberFile(const fs::path& path, const ArchiveLocation& location,
	const ArchiveCatalog& catalog, const ArchiveMemberRecord& member,
	const std::function<bool(const fs::path&, std::string&)>& callback,
	std::string& errorMessage, const std::function<bool()>& shouldContinue) {
	LibarchiveArchiveReader archive;
	if (!OpenArchiveReader(location.archive, location.format, shouldContinue, archive, errorMessage)) {
		return false;
	}
	for (std::uint64_t ordinal = 0; ordinal <= member.index; ++ordinal) {
		if (!ShouldContinue(shouldContinue)) {
			errorMessage = "archive member read was cancelled";
			return false;
		}
		struct archive_entry* entry = nullptr;
		const int resultCode = archive_read_next_header(archive.reader, &entry);
		if (resultCode != ARCHIVE_OK || entry == nullptr) {
			errorMessage = resultCode == ARCHIVE_EOF ? "archive member no longer exists" :
				archive_error_string(archive.reader) == nullptr ? "cannot read archive member header" :
				archive_error_string(archive.reader);
			return false;
		}
		if (ordinal != member.index) {
			if (archive_read_data_skip(archive.reader) != ARCHIVE_OK) {
				errorMessage = archive_error_string(archive.reader) == nullptr ?
					"cannot skip archive member data" : archive_error_string(archive.reader);
				return false;
			}
			continue;
		}
		std::string currentName;
		bool directory = false;
		const bool hasSafePath = LibarchiveEntryPath(entry, currentName, directory);
		const la_int64_t rawModificationTime = archive_entry_mtime(entry);
		const std::int64_t modificationTime = archive_entry_mtime_is_set(entry) ?
			static_cast<std::int64_t>(rawModificationTime) : 0;
		if (!hasSafePath || directory || currentName != location.memberDirectory ||
			!archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0 ||
			static_cast<std::uint64_t>(archive_entry_size(entry)) != member.size ||
			modificationTime != member.modificationTime) {
			errorMessage = "archive member changed since it was listed";
			return false;
		}

		PrivateMemberFile temporary;
		if (!MakePrivateMemberFile(path, temporary, errorMessage)) return false;
		std::vector<std::uint8_t> buffer(64 * 1024);
		std::uint64_t total = 0;
		for (;;) {
			if (!ShouldContinue(shouldContinue)) {
				errorMessage = "archive member read was cancelled";
				return false;
			}
			const la_ssize_t count = archive_read_data(archive.reader, buffer.data(), buffer.size());
			if (count < 0) {
				errorMessage = archive.context.cancelled ? "archive member read was cancelled" :
					archive_error_string(archive.reader) == nullptr ? "cannot read archive member data" :
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
			errorMessage = "archive changed while the image was being read";
			return false;
		}
		if (::lseek(temporary.descriptor, 0, SEEK_SET) < 0) {
			errorMessage = "cannot rewind the archive image memory file";
			return false;
		}
		if (!ShouldContinue(shouldContinue)) {
			errorMessage = "archive member read was cancelled";
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

bool WithSevenZipMemberFile(const fs::path& path, const ArchiveLocation& location,
	const ArchiveCatalog& catalog, const ArchiveMemberRecord& member,
	const std::function<bool(const fs::path&, std::string&)>& callback,
	std::string& errorMessage, ArchiveErrorKind* errorKind,
	const std::function<bool()>& shouldContinue) {
	std::string password;
	std::optional<std::string> suppliedPassword;
	if (catalog.headerEncrypted || member.encrypted) {
		if (!ReadSessionPassword(location.archive, password)) {
			errorMessage = "password required for encrypted 7z archive image";
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::PasswordRequired;
			return false;
		}
		suppliedPassword = password;
	}
	BackingIdentity currentIdentity;
	if (!StatIdentity(location.archive, currentIdentity) || !(currentIdentity == catalog.identity)) {
		ClearPasswordString(password);
		errorMessage = "7z archive changed before the image could be read";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	PrivateMemberFile temporary;
	if (!MakePrivateMemberFile(path, temporary, errorMessage)) {
		ClearPasswordString(password);
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	std::uint64_t total = 0;
	const SevenZipDataWriter writer = [&temporary, &member, &total](
		const std::uint8_t* data, std::size_t size, std::string& sinkError) {
		const std::uint64_t chunk = static_cast<std::uint64_t>(size);
		if (total > member.size || chunk > kMaximumArchiveMemberBytes - total ||
			chunk > member.size - total) {
			sinkError = "7z member expanded beyond its declared size or the 128 MiB limit";
			return false;
		}
		if (!WriteAll(temporary.descriptor, data, size, sinkError)) return false;
		total += chunk;
		return true;
	};
	if (!ExtractSevenZipMember(location.archive, member.index, member.rawPath,
		suppliedPassword, shouldContinue, writer, errorMessage, errorKind)) {
		ClearPasswordString(password);
		return false;
	}
	ClearPasswordString(password);
	if (total != member.size) {
		errorMessage = "7z member ended before its declared size";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	BackingIdentity afterRead;
	if (!StatIdentity(location.archive, afterRead) || !(afterRead == catalog.identity)) {
		errorMessage = "7z archive changed while the image was being read";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	if (::lseek(temporary.descriptor, 0, SEEK_SET) < 0) {
		errorMessage = "cannot rewind the archive image memory file";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	if (!ShouldContinue(shouldContinue)) {
		errorMessage = "archive member read was cancelled";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	try {
		return callback(temporary.link, errorMessage);
	} catch (const std::exception& error) {
		errorMessage = error.what();
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
}

bool WithRarMemberFile(const fs::path& path, const ArchiveLocation& location,
	const ArchiveCatalog& catalog, const ArchiveMemberRecord& member,
	const std::function<bool(const fs::path&, std::string&)>& callback,
	std::string& errorMessage, ArchiveErrorKind* errorKind,
	const std::function<bool()>& shouldContinue) {
	std::string password;
	std::optional<std::string> suppliedPassword;
	if (catalog.headerEncrypted || member.encrypted) {
		if (!ReadSessionPassword(location.archive, password)) {
			errorMessage = "password required for encrypted RAR image";
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::PasswordRequired;
			return false;
		}
		suppliedPassword = password;
	}
	if (!RarBackendAvailable()) {
		ClearPasswordString(password);
		errorMessage = "encrypted RAR support is unavailable in this build";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
		return false;
	}
	BackingIdentity currentIdentity;
	if (!StatIdentity(location.archive, currentIdentity) || !(currentIdentity == catalog.identity)) {
		ClearPasswordString(password);
		errorMessage = "RAR archive changed before the image could be read";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	PrivateMemberFile temporary;
	if (!MakePrivateMemberFile(path, temporary, errorMessage)) {
		ClearPasswordString(password);
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	std::uint64_t total = 0;
	const RarDataWriter writer = [&temporary, &member, &total](
		const std::uint8_t* data, std::size_t size, std::string& sinkError) {
		const std::uint64_t chunk = static_cast<std::uint64_t>(size);
		if (total > member.size || chunk > kMaximumArchiveMemberBytes - total ||
			chunk > member.size - total) {
			sinkError = "RAR member expanded beyond its declared size or the 128 MiB limit";
			return false;
		}
		if (!WriteAll(temporary.descriptor, data, size, sinkError)) return false;
		total += chunk;
		return true;
	};
	const bool extracted = ExtractRarMember(location.archive, member.index, member.rawPath,
		suppliedPassword, shouldContinue, writer, errorMessage, errorKind);
	ClearPasswordString(password);
	if (!extracted) return false;
	if (total != member.size) {
		errorMessage = "RAR member ended before its declared size";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	BackingIdentity afterRead;
	if (!StatIdentity(location.archive, afterRead) || !(afterRead == catalog.identity)) {
		errorMessage = "RAR archive changed while the image was being read";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	if (::lseek(temporary.descriptor, 0, SEEK_SET) < 0) {
		errorMessage = "cannot rewind the archive image memory file";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	if (!ShouldContinue(shouldContinue)) {
		errorMessage = "archive member read was cancelled";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	try {
		return callback(temporary.link, errorMessage);
	} catch (const std::exception& error) {
		errorMessage = error.what();
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
}

} // namespace jpegview_linux::archive_detail
