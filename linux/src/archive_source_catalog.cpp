#include "archive_source_internal.h"

#include "rar_backend.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace jpegview_linux::archive_detail {

struct CatalogCacheEntry {
	std::shared_ptr<const ArchiveCatalog> catalog;
	std::uint64_t lastUsed = 0;
};

struct CatalogLoadState {
	BackingIdentity identity;
	std::condition_variable condition;
	std::shared_ptr<const ArchiveCatalog> catalog;
	std::string error;
	ArchiveErrorKind errorKind = ArchiveErrorKind::Other;
	bool done = false;
};

struct CatalogCache {
	std::mutex mutex;
	std::unordered_map<std::string, CatalogCacheEntry> entries;
	std::unordered_map<std::string, std::shared_ptr<CatalogLoadState>> loading;
	std::uint64_t useCounter = 0;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	detail::ArchiveCatalogTestHook testHook = nullptr;
	void* testHookContext = nullptr;
#endif
};


CatalogCache& GlobalCatalogCache() {
	static CatalogCache cache;
	return cache;
}

void SetCatalogFailureMessage(std::string& message, const char* fallback) noexcept {
	try {
		message.assign(fallback);
	} catch (...) {
		message.clear();
	}
}

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
void InvokeArchiveCatalogTestHook(detail::ArchiveCatalogTestHookPoint point) {
	CatalogCache& cache = GlobalCatalogCache();
	detail::ArchiveCatalogTestHook hook = nullptr;
	void* context = nullptr;
	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		hook = cache.testHook;
		context = cache.testHookContext;
	}
	if (hook != nullptr) hook(point, context);
}
#endif

void RemoveHeaderEncryptedCatalog(const std::string& key) {
	CatalogCache& cache = GlobalCatalogCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	const auto found = cache.entries.find(key);
	if (found != cache.entries.end() && found->second.catalog->headerEncrypted) {
		cache.entries.erase(found);
	}
}

void RemoveAllHeaderEncryptedCatalogs() {
	CatalogCache& cache = GlobalCatalogCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	for (auto entry = cache.entries.begin(); entry != cache.entries.end();) {
		if (entry->second.catalog->headerEncrypted) entry = cache.entries.erase(entry);
		else ++entry;
	}
}

bool AddCatalogChild(ArchiveCatalog& catalog, const std::string& parent,
	CatalogChild child) {
	auto& indices = catalog.childIndices[parent];
	const auto found = indices.find(child.name);
	if (found != indices.end()) {
		CatalogChild& current = catalog.directories[parent][found->second];
		current.encrypted = current.encrypted || child.encrypted;
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

std::shared_ptr<const ArchiveCatalog> GetCatalog(const fs::path& archivePath,
	const std::function<bool()>& shouldContinue, std::string& errorMessage,
	ArchiveErrorKind* errorKind) {
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	BackingIdentity identity;
	if (!StatIdentity(archivePath, identity)) {
		errorMessage = "cannot read archive metadata";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return {};
	}
	const std::string key = archivePath.lexically_normal().string();
	CatalogCache& cache = GlobalCatalogCache();
	ArchiveLocation location;
	if (!ParseArchiveLocation(archivePath, location)) {
		errorMessage = "unrecognized archive format";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
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
				if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
				return {};
			}
			if (loadState->identity == identity && loadState->catalog) {
				return loadState->catalog;
			}
			if (loadState->identity == identity && loadState->done) {
				errorMessage = loadState->error;
				if (errorMessage.empty()) {
					errorMessage = "archive catalog construction failed";
				}
				if (errorKind != nullptr) *errorKind = loadState->errorKind;
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
	ArchiveErrorKind loadErrorKind = ArchiveErrorKind::Other;
	try {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
		InvokeArchiveCatalogTestHook(
			detail::ArchiveCatalogTestHookPoint::LoadingStateRegistered);
#endif
		if (location.format == ArchiveFormat::Zip) {
			loadedSuccessfully = LoadZipCatalog(archivePath, identity, loaded,
				shouldContinue, errorMessage);
			if (loadedSuccessfully) loadErrorKind = ArchiveErrorKind::None;
		} else if (location.format == ArchiveFormat::SevenZip) {
			loadedSuccessfully = LoadLibarchiveCatalog(archivePath, location.format, identity,
				loaded, shouldContinue, errorMessage);
			if (!ShouldContinue(shouldContinue)) {
				loadedSuccessfully = false;
				errorMessage = "archive indexing was cancelled";
			} else if (loadedSuccessfully && !loaded->containsEncryptedEntries) {
				loadErrorKind = ArchiveErrorKind::None;
			} else {
				std::string ignoredLibarchiveError = std::move(errorMessage);
				errorMessage.clear();
				loaded.reset();
				std::string password;
				std::optional<std::string> suppliedPassword;
				if (ReadSessionPassword(archivePath, password)) suppliedPassword = password;
				loadedSuccessfully = LoadSevenZipCatalog(archivePath, identity, suppliedPassword,
					loaded, shouldContinue, errorMessage, &loadErrorKind);
				ClearPasswordString(password);
				if (!loadedSuccessfully && errorMessage.empty()) {
					errorMessage = std::move(ignoredLibarchiveError);
				}
			}
		} else if (location.format == ArchiveFormat::Rar) {
			std::shared_ptr<const ArchiveCatalog> libarchiveCatalog;
			std::string libarchiveError;
			const bool libarchiveLoaded = LoadLibarchiveCatalog(archivePath, location.format,
				identity, libarchiveCatalog, shouldContinue, libarchiveError);
			if (!ShouldContinue(shouldContinue)) {
				loadedSuccessfully = false;
				errorMessage = "archive indexing was cancelled";
			} else if (!RarBackendAvailable() && libarchiveLoaded) {
				loaded = std::move(libarchiveCatalog);
				loadedSuccessfully = true;
				loadErrorKind = ArchiveErrorKind::None;
			} else if (RarBackendAvailable()) {
				std::string password;
				std::optional<std::string> suppliedPassword;
				if (ReadSessionPassword(archivePath, password)) suppliedPassword = password;
				std::shared_ptr<const ArchiveCatalog> rarCatalog;
				loadedSuccessfully = LoadRarCatalog(archivePath, identity, suppliedPassword,
					rarCatalog, shouldContinue, errorMessage, &loadErrorKind);
				ClearPasswordString(password);
				if (loadedSuccessfully &&
					(rarCatalog->containsEncryptedEntries || !libarchiveLoaded)) {
					loaded = std::move(rarCatalog);
					loadErrorKind = ArchiveErrorKind::None;
				} else if (loadedSuccessfully) {
					loaded = std::move(libarchiveCatalog);
					loadErrorKind = ArchiveErrorKind::None;
				} else if (libarchiveLoaded &&
					loadErrorKind == ArchiveErrorKind::UnsupportedEncryption) {
					loaded = std::move(libarchiveCatalog);
					loadedSuccessfully = true;
					loadErrorKind = ArchiveErrorKind::None;
				} else {
					loadedSuccessfully = false;
					if (errorMessage.empty()) errorMessage = std::move(libarchiveError);
				}
			} else if (libarchiveLoaded) {
				loaded = std::move(libarchiveCatalog);
				loadedSuccessfully = true;
				loadErrorKind = ArchiveErrorKind::None;
			} else {
				loadedSuccessfully = false;
				errorMessage = libarchiveError.empty() ?
					"RAR archive support is unavailable in this build" : libarchiveError;
				loadErrorKind = ArchiveErrorKind::UnsupportedEncryption;
			}
		} else if (!LoadLibarchiveCatalog(archivePath, location.format, identity, loaded,
			shouldContinue, errorMessage)) {
			loadedSuccessfully = false;
		} else {
			loadedSuccessfully = true;
			loadErrorKind = ArchiveErrorKind::None;
		}
		if (!ShouldContinue(shouldContinue)) {
			loadedSuccessfully = false;
			errorMessage = "archive indexing was cancelled";
			loadErrorKind = ArchiveErrorKind::Other;
		}
	} catch (const std::bad_alloc&) {
		loaded.reset();
		loadedSuccessfully = false;
		loadErrorKind = ArchiveErrorKind::Other;
		SetCatalogFailureMessage(errorMessage, "catalog allocation failed");
	} catch (const std::exception& error) {
		loaded.reset();
		loadedSuccessfully = false;
		loadErrorKind = ArchiveErrorKind::Other;
		try {
			errorMessage.assign(error.what());
		} catch (...) {
			SetCatalogFailureMessage(errorMessage, "catalog construction failed");
		}
	} catch (...) {
		loaded.reset();
		loadedSuccessfully = false;
		loadErrorKind = ArchiveErrorKind::Other;
		SetCatalogFailureMessage(errorMessage, "catalog construction failed");
	}
	{
		std::lock_guard<std::mutex> lock(cache.mutex);
		if (loadedSuccessfully) {
			try {
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
			} catch (...) {
				// Catalog retention is optional. Keep a successfully built catalog
				// available to this caller even if the cache cannot grow.
			}
		}
		loadState->catalog = loadedSuccessfully ? loaded : nullptr;
		if (loadedSuccessfully) {
			errorMessage.clear();
			loadState->error.clear();
		} else {
			try {
				loadState->error = errorMessage;
			} catch (...) {
				SetCatalogFailureMessage(loadState->error,
					"catalog construction failed");
			}
		}
		loadState->errorKind = loadedSuccessfully ? ArchiveErrorKind::None : loadErrorKind;
		loadState->done = true;
		const auto activeLoad = cache.loading.find(key);
		if (activeLoad != cache.loading.end() && activeLoad->second == loadState) {
			cache.loading.erase(activeLoad);
		}
	}
	loadState->condition.notify_all();
	if (errorKind != nullptr) *errorKind = loadedSuccessfully ? ArchiveErrorKind::None : loadErrorKind;
	return loadedSuccessfully ? loaded : nullptr;
}


#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
void SetCatalogTestHook(detail::ArchiveCatalogTestHook hook, void* context) {
	CatalogCache& cache = GlobalCatalogCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	cache.testHook = hook;
	cache.testHookContext = context;
}
#endif

} // namespace jpegview_linux::archive_detail
