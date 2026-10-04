#pragma once

#include "archive_source.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <zip.h>

namespace jpegview_linux::archive_detail {

namespace fs = std::filesystem;

enum class ArchiveFormat {
	Zip,
	Tar,
	Tgz,
	SevenZip,
	Rar,
};

struct ArchiveLocation {
	fs::path archive;
	std::string memberDirectory;
	ArchiveFormat format = ArchiveFormat::Zip;
};

using BackingIdentity = SourceIdentity;

struct CatalogChild {
	std::string name;
	std::string fullName;
	bool directory = false;
	std::uint64_t size = 0;
	std::int64_t modificationTime = 0;
	bool encrypted = false;
};

enum class ArchiveBackend {
	Zip,
	Libarchive,
	SevenZip,
	Rar,
};

struct ArchiveMemberRecord {
	std::uint64_t size = 0;
	std::uint64_t compressedSize = 0;
	std::uint32_t crc = 0;
	std::uint64_t index = 0;
	std::int64_t modificationTime = 0;
	std::string rawPath;
	bool encrypted = false;
	ArchiveBackend backend = ArchiveBackend::Zip;
};

struct ArchiveCatalog {
	fs::path archive;
	BackingIdentity identity;
	ArchiveFormat format = ArchiveFormat::Zip;
	bool containsEncryptedEntries = false;
	bool headerEncrypted = false;
	std::unordered_map<std::string, ArchiveMemberRecord> members;
	std::unordered_map<std::string, std::vector<CatalogChild>> directories;
	std::unordered_map<std::string,
		std::unordered_map<std::string, std::size_t>> childIndices;
};

struct PrivateMemberFile {
	int descriptor = -1;
	fs::path directory;
	fs::path link;
	~PrivateMemberFile();
};

std::string Lower(std::string value);
bool ShouldContinue(const std::function<bool()>& shouldContinue);
bool StatIdentity(const fs::path& path, BackingIdentity& identity,
	std::int64_t* birthTimeNanoseconds = nullptr, bool* hasBirthTime = nullptr);
bool SafeMemberPath(std::string_view name, bool directory, std::string& normalized);
bool SafeLibarchiveMemberPath(std::string_view name, bool directory,
	std::string& normalized);
bool ParseArchiveLocation(const fs::path& path, ArchiveLocation& location);
std::string ZipError(zip_t* archive);
std::string ZipOpenError(int errorCode);
ArchiveErrorKind ZipErrorKind(const zip_error_t* error);
void ClearPasswordString(std::string& password);
bool ReadSessionPassword(const fs::path& path, std::string& password);

bool AddCatalogChild(ArchiveCatalog& catalog, const std::string& parent,
	CatalogChild child);
bool AddDirectoryChain(ArchiveCatalog& catalog,
	const std::vector<std::string>& components, std::size_t count);
std::shared_ptr<const ArchiveCatalog> GetCatalog(const fs::path& archivePath,
	const std::function<bool()>& shouldContinue, std::string& errorMessage,
	ArchiveErrorKind* errorKind = nullptr);
void RemoveHeaderEncryptedCatalog(const std::string& key);
void RemoveAllHeaderEncryptedCatalogs();
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
void SetCatalogTestHook(detail::ArchiveCatalogTestHook hook, void* context);
#endif

bool LoadZipCatalog(const fs::path& archivePath,
	const BackingIdentity& expectedIdentity,
	std::shared_ptr<const ArchiveCatalog>& result,
	const std::function<bool()>& shouldContinue, std::string& errorMessage);
bool LoadLibarchiveCatalog(const fs::path& archivePath, ArchiveFormat format,
	const BackingIdentity& expectedIdentity,
	std::shared_ptr<const ArchiveCatalog>& result,
	const std::function<bool()>& shouldContinue, std::string& errorMessage);
bool LoadSevenZipCatalog(const fs::path& archivePath,
	const BackingIdentity& expectedIdentity,
	const std::optional<std::string>& password,
	std::shared_ptr<const ArchiveCatalog>& result,
	const std::function<bool()>& shouldContinue, std::string& errorMessage,
	ArchiveErrorKind* errorKind);
bool LoadRarCatalog(const fs::path& archivePath,
	const BackingIdentity& expectedIdentity,
	const std::optional<std::string>& password,
	std::shared_ptr<const ArchiveCatalog>& result,
	const std::function<bool()>& shouldContinue, std::string& errorMessage,
	ArchiveErrorKind* errorKind);

bool WriteAll(int descriptor, const std::uint8_t* bytes, std::size_t length,
	std::string& errorMessage);
bool MakePrivateMemberFile(const fs::path& memberPath,
	PrivateMemberFile& temporary, std::string& errorMessage);
bool WithLibarchiveMemberFile(const fs::path& path,
	const ArchiveLocation& location, const ArchiveCatalog& catalog,
	const ArchiveMemberRecord& member,
	const std::function<bool(const fs::path&, std::string&)>& callback,
	std::string& errorMessage, const std::function<bool()>& shouldContinue);
bool WithSevenZipMemberFile(const fs::path& path,
	const ArchiveLocation& location, const ArchiveCatalog& catalog,
	const ArchiveMemberRecord& member,
	const std::function<bool(const fs::path&, std::string&)>& callback,
	std::string& errorMessage, ArchiveErrorKind* errorKind,
	const std::function<bool()>& shouldContinue);
bool WithRarMemberFile(const fs::path& path,
	const ArchiveLocation& location, const ArchiveCatalog& catalog,
	const ArchiveMemberRecord& member,
	const std::function<bool(const fs::path&, std::string&)>& callback,
	std::string& errorMessage, ArchiveErrorKind* errorKind,
	const std::function<bool()>& shouldContinue);

} // namespace jpegview_linux::archive_detail
