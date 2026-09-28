#include "rar_backend.h"

#include <array>
#include <dlfcn.h>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace jpegview_linux {
namespace {

using ContinueCallback = int (*)(void*);
using WriteCallback = std::int64_t (*)(const std::uint8_t*, std::uint64_t, void*);
using OpenFunction = std::uint32_t (*)(const std::uint8_t*, std::uint64_t,
	const std::uint8_t*, std::uint64_t, std::uint8_t, ContinueCallback, void*,
	void**, std::uint8_t*, std::uint8_t*, char*, std::uint64_t);
using EntryCountFunction = std::uint32_t (*)(const void*, std::uint64_t*, char*, std::uint64_t);
using EntryInfoFunction = std::uint32_t (*)(const void*, std::uint64_t, std::uint8_t*,
	std::uint64_t, std::uint64_t*, std::uint64_t*, std::int64_t*, std::uint32_t*,
	char*, std::uint64_t);
using ExtractFunction = std::uint32_t (*)(const void*, std::uint64_t,
	const std::uint8_t*, std::uint64_t, const std::uint8_t*, std::uint64_t, std::uint8_t,
	std::uint64_t, ContinueCallback, void*, WriteCallback, void*, std::uint64_t*,
	char*, std::uint64_t);
using FreeFunction = void (*)(void*);

constexpr std::uint32_t kStatusOk = 0;
constexpr std::uint32_t kStatusPasswordRequired = 1;
constexpr std::uint32_t kStatusInvalidPassword = 2;
constexpr std::uint32_t kStatusCancelled = 3;
constexpr std::uint32_t kStatusUnsupported = 4;
constexpr std::uint32_t kStatusLimitExceeded = 5;
constexpr std::uint32_t kStatusInvalidArchive = 6;
constexpr std::uint32_t kStatusIoError = 7;
constexpr std::uint32_t kStatusOutputError = 8;
constexpr std::uint32_t kStatusInternalError = 9;
constexpr std::uint32_t kStatusBufferTooSmall = 10;
constexpr std::uint32_t kStatusBadArgument = 11;
constexpr std::uint64_t kMaximumEntryNameBytes = 64ull * 1024ull * 1024ull;

struct Plugin {
	void* library = nullptr;
	OpenFunction open = nullptr;
	EntryCountFunction entryCount = nullptr;
	EntryInfoFunction entryInfo = nullptr;
	ExtractFunction extract = nullptr;
	FreeFunction free = nullptr;
	std::string error;
};

std::filesystem::path ExecutableDirectory() {
	char buffer[4096];
	const ssize_t count = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(buffer) - 1) return {};
	buffer[count] = '\0';
	return std::filesystem::path(buffer).parent_path();
}

template <typename T>
bool LoadFunction(void* library, const char* name, T& function) {
	(void)::dlerror();
	function = reinterpret_cast<T>(::dlsym(library, name));
	return function != nullptr;
}

Plugin LoadPlugin() {
	Plugin plugin;
	const std::filesystem::path executableDirectory = ExecutableDirectory();
	if (executableDirectory.empty()) {
		plugin.error = "cannot locate the executable for the private RAR reader";
		return plugin;
	}
	const std::filesystem::path candidates[] = {
		executableDirectory / "lib/jpegview-linux/librar_backend.so",
		executableDirectory / "../lib/jpegview-linux/librar_backend.so",
	};
	for (const std::filesystem::path& candidate : candidates) {
		plugin.library = ::dlopen(candidate.lexically_normal().c_str(), RTLD_NOW | RTLD_LOCAL);
		if (plugin.library == nullptr) continue;
		const bool loaded = LoadFunction(plugin.library, "jv_rar_open", plugin.open) &&
			LoadFunction(plugin.library, "jv_rar_entry_count", plugin.entryCount) &&
			LoadFunction(plugin.library, "jv_rar_entry_info", plugin.entryInfo) &&
			LoadFunction(plugin.library, "jv_rar_extract_member", plugin.extract) &&
			LoadFunction(plugin.library, "jv_rar_free", plugin.free);
		if (loaded) return plugin;
		const char* message = ::dlerror();
		plugin.error = message == nullptr ? "private RAR reader is missing a required C ABI export" : message;
		(void)::dlclose(plugin.library);
		plugin.library = nullptr;
	}
	if (plugin.error.empty()) {
		plugin.error = "private RAR reader is unavailable beside the executable";
	}
	return plugin;
}

const Plugin& GetPlugin() {
	static const Plugin plugin = LoadPlugin();
	return plugin;
}

struct ContinueContext {
	const std::function<bool()>* callback = nullptr;
};

int Continue(void* opaque) noexcept {
	auto* context = static_cast<ContinueContext*>(opaque);
	if (context == nullptr || context->callback == nullptr || !*context->callback) return 1;
	try {
		return (*context->callback)() ? 1 : 0;
	} catch (...) {
		return 0;
	}
}

ContinueCallback CallbackFor(const std::function<bool()>& shouldContinue) {
	return shouldContinue ? &Continue : nullptr;
}

struct WriterContext {
	const RarDataWriter* writer = nullptr;
	std::string* error = nullptr;
};

std::int64_t Write(const std::uint8_t* data, std::uint64_t size, void* opaque) noexcept {
	auto* context = static_cast<WriterContext*>(opaque);
	if (context == nullptr || context->writer == nullptr || context->error == nullptr ||
		!(*context->writer)) return -1;
	if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
		size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return -1;
	try {
		if (!(*context->writer)(data, static_cast<std::size_t>(size), *context->error)) return -1;
		return static_cast<std::int64_t>(size);
	} catch (const std::exception& exception) {
		*context->error = exception.what();
	} catch (...) {
		*context->error = "RAR output callback failed";
	}
	return -1;
}

void SetError(std::string& message, ArchiveErrorKind* kind, std::uint32_t status,
	const char* detail, const std::string* writerError = nullptr) {
	switch (status) {
	case kStatusPasswordRequired:
		message = "password required for encrypted RAR archive";
		if (kind != nullptr) *kind = ArchiveErrorKind::PasswordRequired;
		return;
	case kStatusInvalidPassword:
		message = "incorrect archive password";
		if (kind != nullptr) *kind = ArchiveErrorKind::InvalidPassword;
		return;
	case kStatusUnsupported:
		message = detail == nullptr || *detail == '\0' ?
			"RAR feature is not supported by the private reader" : detail;
		if (kind != nullptr) *kind = ArchiveErrorKind::UnsupportedEncryption;
		return;
	case kStatusCancelled:
		message = "RAR operation was cancelled";
		if (kind != nullptr) *kind = ArchiveErrorKind::Other;
		return;
	case kStatusLimitExceeded:
		message = detail == nullptr || *detail == '\0' ?
			"RAR archive exceeds a configured resource limit" : detail;
		if (kind != nullptr) *kind = ArchiveErrorKind::Other;
		return;
	case kStatusIoError:
		message = writerError != nullptr && !writerError->empty() ? *writerError :
			(detail == nullptr || *detail == '\0' ? "cannot read RAR archive" : detail);
		if (kind != nullptr) *kind = ArchiveErrorKind::Other;
		return;
	case kStatusOutputError:
		message = writerError != nullptr && !writerError->empty() ? *writerError :
			(detail == nullptr || *detail == '\0' ? "cannot write extracted RAR image" : detail);
		if (kind != nullptr) *kind = ArchiveErrorKind::Other;
		return;
	case kStatusInternalError:
	case kStatusInvalidArchive:
	case kStatusBadArgument:
	case kStatusBufferTooSmall:
	default:
		message = detail == nullptr || *detail == '\0' ? "cannot read RAR archive" : detail;
		if (kind != nullptr) *kind = ArchiveErrorKind::Other;
		return;
	}
}

struct ArchiveOwner {
	const Plugin& plugin;
	void* value = nullptr;
	~ArchiveOwner() { if (value != nullptr) plugin.free(value); }
};

bool Open(const Plugin& plugin, const std::filesystem::path& archive,
	const std::optional<std::string>& password, const std::function<bool()>& shouldContinue,
	ArchiveOwner& owner, bool& headerEncrypted, bool& containsEncrypted,
	std::uint32_t& status, std::array<char, 512>& error) {
	const std::string path = archive.native();
	const std::uint8_t* passwordData = password.has_value() ?
		reinterpret_cast<const std::uint8_t*>(password->data()) : nullptr;
	const std::uint64_t passwordLength = password.has_value() ?
		static_cast<std::uint64_t>(password->size()) : 0;
	ContinueContext continueContext{&shouldContinue};
	std::uint8_t headerFlag = 0;
	std::uint8_t encryptedFlag = 0;
	status = plugin.open(
		reinterpret_cast<const std::uint8_t*>(path.data()), static_cast<std::uint64_t>(path.size()),
		passwordData, passwordLength, static_cast<std::uint8_t>(password.has_value()),
		CallbackFor(shouldContinue), &continueContext, &owner.value,
		&headerFlag, &encryptedFlag, error.data(), error.size());
	headerEncrypted = headerFlag != 0;
	containsEncrypted = encryptedFlag != 0;
	if (status == kStatusOk) return true;
	return false;
}

} // namespace

bool RarBackendAvailable() {
	return GetPlugin().library != nullptr && GetPlugin().open != nullptr;
}

bool ReadRarCatalog(const std::filesystem::path& archive,
	const std::optional<std::string>& password, const std::function<bool()>& shouldContinue,
	std::vector<RarEntry>& entries, bool& headerEncrypted, std::string& errorMessage,
	ArchiveErrorKind* errorKind) {
	entries.clear();
	errorMessage.clear();
	headerEncrypted = false;
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	if (shouldContinue && !shouldContinue()) {
		errorMessage = "RAR archive indexing was cancelled";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	const Plugin& plugin = GetPlugin();
	if (plugin.library == nullptr || plugin.open == nullptr) {
		errorMessage = plugin.error;
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
		return false;
	}
	std::array<char, 512> detail{};
	ArchiveOwner owner{plugin};
	bool containsEncrypted = false;
	std::uint32_t status = kStatusInvalidArchive;
	if (!Open(plugin, archive, password, shouldContinue, owner,
		headerEncrypted, containsEncrypted, status, detail)) {
		SetError(errorMessage, errorKind, status, detail.data());
		return false;
	}
	std::uint64_t count = 0;
	const std::uint32_t countStatus = plugin.entryCount(owner.value, &count,
		detail.data(), detail.size());
	if (countStatus != kStatusOk || count > kMaximumArchiveEntries) {
		SetError(errorMessage, errorKind,
			countStatus == kStatusOk ? kStatusLimitExceeded : countStatus,
			countStatus == kStatusOk ? "RAR archive has more than 100000 members" : detail.data());
		return false;
	}
	try {
		entries.reserve(static_cast<std::size_t>(count));
		for (std::uint64_t index = 0; index < count; ++index) {
			if (shouldContinue && !shouldContinue()) {
				errorMessage = "RAR archive indexing was cancelled";
				if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
				return false;
			}
			std::uint64_t nameLength = 0;
			std::uint64_t size = 0;
			std::int64_t modificationTime = 0;
			std::uint32_t flags = 0;
			std::uint32_t status = plugin.entryInfo(owner.value, index, nullptr, 0,
				&nameLength, &size, &modificationTime, &flags, detail.data(), detail.size());
			if (status != kStatusBufferTooSmall && status != kStatusOk) {
				SetError(errorMessage, errorKind, status, detail.data());
				return false;
			}
			if (nameLength > kMaximumEntryNameBytes ||
				nameLength > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
				errorMessage = "RAR member name exceeds the configured metadata limit";
				if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
				return false;
			}
			RarEntry entry;
			entry.index = index;
			entry.rawPath.resize(static_cast<std::size_t>(nameLength));
			if (nameLength != 0) {
				status = plugin.entryInfo(owner.value, index,
					reinterpret_cast<std::uint8_t*>(&entry.rawPath[0]), nameLength,
					&nameLength, &size, &modificationTime, &flags, detail.data(), detail.size());
				if (status != kStatusOk) {
					SetError(errorMessage, errorKind, status, detail.data());
					return false;
				}
			}
			entry.size = size;
			entry.modificationTime = modificationTime;
			entry.directory = (flags & 1u) != 0;
			entry.encrypted = (flags & 2u) != 0;
			entry.specialFile = (flags & 4u) != 0;
			entry.split = (flags & 8u) != 0;
			entries.push_back(std::move(entry));
		}
	} catch (const std::bad_alloc&) {
		entries.clear();
		errorMessage = "not enough memory to index RAR archive";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	return true;
}

bool ExtractRarMember(const std::filesystem::path& archive, std::uint64_t index,
	const std::string& expectedRawPath, const std::optional<std::string>& password,
	const std::function<bool()>& shouldContinue, const RarDataWriter& writer,
	std::string& errorMessage, ArchiveErrorKind* errorKind) {
	errorMessage.clear();
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	if (!writer) {
		errorMessage = "RAR member output callback is missing";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	if (shouldContinue && !shouldContinue()) {
		errorMessage = "RAR member extraction was cancelled";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	const Plugin& plugin = GetPlugin();
	if (plugin.library == nullptr || plugin.open == nullptr) {
		errorMessage = plugin.error;
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
		return false;
	}
	std::array<char, 512> detail{};
	ArchiveOwner owner{plugin};
	bool headerEncrypted = false;
	bool containsEncrypted = false;
	std::uint32_t status = kStatusInvalidArchive;
	if (!Open(plugin, archive, password, shouldContinue, owner,
		headerEncrypted, containsEncrypted, status, detail)) {
		SetError(errorMessage, errorKind, status, detail.data());
		return false;
	}
	if (shouldContinue && !shouldContinue()) {
		errorMessage = "RAR member extraction was cancelled";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	WriterContext writerContext{&writer, &errorMessage};
	std::uint64_t written = 0;
	ContinueContext continueContext{&shouldContinue};
	status = plugin.extract(owner.value, index,
		reinterpret_cast<const std::uint8_t*>(expectedRawPath.data()),
		static_cast<std::uint64_t>(expectedRawPath.size()),
		password.has_value() ? reinterpret_cast<const std::uint8_t*>(password->data()) : nullptr,
		password.has_value() ? static_cast<std::uint64_t>(password->size()) : 0,
		static_cast<std::uint8_t>(password.has_value()), kMaximumArchiveMemberBytes,
		CallbackFor(shouldContinue), &continueContext, &Write, &writerContext, &written,
		detail.data(), detail.size());
	if (status != kStatusOk) {
		SetError(errorMessage, errorKind, status, detail.data(), &errorMessage);
		return false;
	}
	return true;
}

} // namespace jpegview_linux
