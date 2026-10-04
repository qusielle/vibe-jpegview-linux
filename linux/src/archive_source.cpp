#include "archive_source.h"
#include "archive_source_internal.h"

#include "perf_diagnostics.h"
#include "rar_backend.h"
#include "source_work_coordinator.h"
#include "seven_zip_backend.h"
#include "work_context.h"

#include <algorithm>
#include <array>
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
#include <atomic>
#endif
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
#include <linux/stat.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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
namespace archive_detail {

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
void NotifyArchiveSourceProbe(detail::ArchiveSourceProbePoint point,
	const fs::path& path);
#endif

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
std::atomic<bool> gStatxUnavailableForTesting{false};
#endif

std::int64_t UnixNanoseconds(std::int64_t seconds, std::int64_t nanoseconds = 0) {
	constexpr std::int64_t scale = 1000000000ll;
	if (seconds > std::numeric_limits<std::int64_t>::max() / scale) {
		return std::numeric_limits<std::int64_t>::max();
	}
	if (seconds < std::numeric_limits<std::int64_t>::min() / scale) {
		return std::numeric_limits<std::int64_t>::min();
	}
	const std::int64_t base = seconds * scale;
	if (nanoseconds > 0 && base > std::numeric_limits<std::int64_t>::max() - nanoseconds) {
		return std::numeric_limits<std::int64_t>::max();
	}
	if (nanoseconds < 0 && base < std::numeric_limits<std::int64_t>::min() - nanoseconds) {
		return std::numeric_limits<std::int64_t>::min();
	}
	return base + nanoseconds;
}


struct SessionPassword {
	BackingIdentity identity;
	std::string value;
};

struct SessionPasswordCache {
	std::mutex mutex;
	std::unordered_map<std::string, SessionPassword> values;
};

SessionPasswordCache& GlobalSessionPasswordCache() {
	static SessionPasswordCache cache;
	return cache;
}

bool StatIdentity(const fs::path& path, BackingIdentity& identity,
	std::int64_t* birthTimeNanoseconds, bool* hasBirthTime) {
	if (birthTimeNanoseconds != nullptr) *birthTimeNanoseconds = 0;
	if (hasBirthTime != nullptr) *hasBirthTime = false;
#if defined(SYS_statx)
	bool tryStatx = true;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	tryStatx = !gStatxUnavailableForTesting.load();
#endif
	if (tryStatx) {
		struct statx extendedStatus{};
		const unsigned int requestedMask = STATX_BASIC_STATS | STATX_BTIME;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
		NotifyArchiveSourceProbe(detail::ArchiveSourceProbePoint::SourceIdentityStatxAttempt, path);
#endif
		if (::syscall(SYS_statx, AT_FDCWD, path.c_str(), AT_STATX_SYNC_AS_STAT,
			requestedMask, &extendedStatus) == 0) {
			constexpr unsigned int requiredMask = STATX_INO | STATX_SIZE | STATX_MTIME;
			if ((extendedStatus.stx_mask & requiredMask) == requiredMask) {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				NotifyArchiveSourceProbe(detail::ArchiveSourceProbePoint::SourceIdentityStatxComplete,
					path);
#endif
				identity.device = static_cast<std::uint64_t>(::makedev(
					extendedStatus.stx_dev_major, extendedStatus.stx_dev_minor));
				identity.inode = extendedStatus.stx_ino;
				identity.size = extendedStatus.stx_size;
				identity.modifiedSeconds = extendedStatus.stx_mtime.tv_sec;
				identity.modifiedNanoseconds = extendedStatus.stx_mtime.tv_nsec;
				identity.valid = true;
				if ((extendedStatus.stx_mask & STATX_BTIME) != 0 && birthTimeNanoseconds != nullptr) {
					*birthTimeNanoseconds = UnixNanoseconds(extendedStatus.stx_btime.tv_sec,
						extendedStatus.stx_btime.tv_nsec);
					if (hasBirthTime != nullptr) *hasBirthTime = true;
				}
				return true;
			}
		}
	}
#endif
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	NotifyArchiveSourceProbe(detail::ArchiveSourceProbePoint::SourceIdentityStatFallback, path);
#endif
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
	if (!shouldContinue) return true;
	try {
		return shouldContinue();
	} catch (...) {
		return false;
	}
}

ArchiveFormat FormatForContainerName(const fs::path& path) {
	const std::string name = Lower(path.filename().string());
	const std::string extension = Lower(path.extension().string());
	if (name.size() >= 7 && name.compare(name.size() - 7, 7, ".tar.gz") == 0) {
		return ArchiveFormat::Tgz;
	}
	if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tgz") == 0) {
		return ArchiveFormat::Tgz;
	}
	if (extension == ".7z" || extension == ".cb7") {
		return ArchiveFormat::SevenZip;
	}
	if (extension == ".rar") {
		return ArchiveFormat::Rar;
	}
	if (extension == ".zip" || extension == ".cbz") {
		return ArchiveFormat::Zip;
	}
	if (extension == ".tar") return ArchiveFormat::Tar;
	return ArchiveFormat::Zip;
}

bool HasArchiveExtension(const fs::path& path) {
	const std::string name = Lower(path.filename().string());
	const std::string extension = Lower(path.extension().string());
	return extension == ".zip" || extension == ".cbz" || extension == ".tar" ||
		extension == ".7z" || extension == ".cb7" ||
		extension == ".rar" ||
		(name.size() >= 7 && name.compare(name.size() - 7, 7, ".tar.gz") == 0) ||
		(name.size() >= 4 && name.compare(name.size() - 4, 4, ".tgz") == 0);
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

bool SafeLibarchiveMemberPath(std::string_view name, bool directory, std::string& normalized) {
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
		#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
			NotifyArchiveSourceProbe(
				detail::ArchiveSourceProbePoint::LocationClassificationStat, candidate);
		#endif
			std::error_code statusError;
			if (fs::is_regular_file(candidate, statusError) && !statusError) {
				std::string member;
				if (componentEnd < text.size()) {
					const std::string_view remainder(text.data() + componentEnd + 1,
						text.size() - componentEnd - 1);
					if (!remainder.empty()) {
						const bool safe = format != ArchiveFormat::Zip ?
							SafeLibarchiveMemberPath(remainder, true, member) :
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

ArchiveErrorKind ZipErrorKind(const zip_error_t* error) {
	if (error == nullptr) return ArchiveErrorKind::Other;
	switch (zip_error_code_zip(error)) {
	case ZIP_ER_NOPASSWD: return ArchiveErrorKind::PasswordRequired;
	case ZIP_ER_WRONGPASSWD: return ArchiveErrorKind::InvalidPassword;
	default: return ArchiveErrorKind::Other;
	}
}

void ClearPasswordString(std::string& password) {
	volatile char* bytes = password.empty() ? nullptr : &password[0];
	for (std::size_t index = 0; index < password.size(); ++index) bytes[index] = '\0';
	password.clear();
}

fs::path PasswordBackingPath(const fs::path& path) {
	ArchiveLocation location;
	return ParseArchiveLocation(path, location) ? location.archive : path;
}

bool PasswordCacheKey(const fs::path& path, std::string& key,
	BackingIdentity& identity) {
	const fs::path backing = PasswordBackingPath(path);
	#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	NotifyArchiveSourceProbe(
		detail::ArchiveSourceProbePoint::PasswordCacheIdentityStat, backing);
	#endif
	if (!StatIdentity(backing, identity)) return false;
	std::error_code absoluteError;
	const fs::path absolute = fs::absolute(backing, absoluteError);
	key = (absoluteError ? backing : absolute).lexically_normal().string();
	return !key.empty();
}

bool ReadSessionPassword(const fs::path& path, std::string& password) {
	std::string key;
	BackingIdentity identity;
	if (!PasswordCacheKey(path, key, identity)) return false;
	SessionPasswordCache& cache = GlobalSessionPasswordCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	const auto found = cache.values.find(key);
	if (found == cache.values.end()) return false;
	if (!(found->second.identity == identity)) {
		ClearPasswordString(found->second.value);
		cache.values.erase(found);
		return false;
	}
	password = found->second.value;
	return true;
}

void EraseSessionPassword(const std::string& key) {
	SessionPasswordCache& cache = GlobalSessionPasswordCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	const auto found = cache.values.find(key);
	if (found == cache.values.end()) return;
	ClearPasswordString(found->second.value);
	cache.values.erase(found);
}

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
struct ArchiveSourceProbeHooks {
	std::mutex mutex;
	detail::ArchiveSourceProbeHook hook = nullptr;
	void* context = nullptr;
};

ArchiveSourceProbeHooks& GlobalArchiveSourceProbeHooks() {
	static ArchiveSourceProbeHooks hooks;
	return hooks;
}

void NotifyArchiveSourceProbe(detail::ArchiveSourceProbePoint point,
	const fs::path& path) {
	ArchiveSourceProbeHooks& hooks = GlobalArchiveSourceProbeHooks();
	detail::ArchiveSourceProbeHook hook = nullptr;
	void* hookContext = nullptr;
	{
		std::lock_guard<std::mutex> lock(hooks.mutex);
		hook = hooks.hook;
		hookContext = hooks.context;
	}
	if (hook == nullptr) return;
	const WorkContext* active = ActiveWorkContextSlot();
	hook(point, path,
		active != nullptr && active->sourceAccessAlreadyAdmitted,
		active != nullptr && active->cpuProcessingAlreadyAdmitted,
		hookContext);
}
#endif

const std::function<bool()>& AlwaysContinue() {
	static const std::function<bool()> continueWork = [] { return true; };
	return continueWork;
}

PrivateMemberFile::~PrivateMemberFile() {
	if (!link.empty()) (void)::unlink(link.c_str());
	if (!directory.empty()) (void)::rmdir(directory.c_str());
	if (descriptor >= 0) (void)::close(descriptor);
}

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

} // namespace archive_detail

using namespace archive_detail;

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
	case ArchiveFormat::Zip:
		return Lower(location.archive.extension().string()) == ".cbz" ? "CBZ" : "ZIP";
	case ArchiveFormat::Tar: return "TAR";
	case ArchiveFormat::Tgz: return "TGZ";
	case ArchiveFormat::SevenZip:
		return Lower(location.archive.extension().string()) == ".cb7" ? "CB7" : ".7Z";
	case ArchiveFormat::Rar: return "RAR";
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

fs::path ArchiveBackingFileForAdmission(const fs::path& path) {
	const fs::path normalized = path.lexically_normal();
	const std::string text = normalized.string();
	if (text.empty()) return normalized;
	std::size_t componentStart = text.front() == '/' ? 1 : 0;
	while (componentStart < text.size()) {
		std::size_t componentEnd = text.find('/', componentStart);
		if (componentEnd == std::string::npos) componentEnd = text.size();
		const std::string component = text.substr(componentStart,
			componentEnd - componentStart);
		if (!component.empty() && HasArchiveExtension(fs::path(component))) {
			return fs::path(text.substr(0, componentEnd));
		}
		if (componentEnd == text.size()) break;
		componentStart = componentEnd + 1;
	}
	return normalized;
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
	std::string& errorMessage, ArchiveErrorKind* errorKind,
	bool* containsEncryptedEntries) {
	entries.clear();
	errorMessage.clear();
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	if (containsEncryptedEntries != nullptr) *containsEncryptedEntries = false;
	WorkContext context = ResolveWorkContext(directory, SourceWorkPriority::Metadata);
	if (shouldContinue) {
		const auto inheritedContinue = context.shouldContinue;
		context.shouldContinue = inheritedContinue ?
			std::function<bool()>([inheritedContinue, shouldContinue] {
				return inheritedContinue() && shouldContinue();
			}) : shouldContinue;
	}
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
		context, ArchiveBackingFileForAdmission(directory));
	if (!admission) {
		errorMessage = "archive listing source or CPU admission was cancelled";
		return false;
	}
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	const std::function<bool()> continueBeforeAdmission = context.shouldContinue;
	if (context.sourcePriority != SourceWorkPriority::Foreground) {
		const auto markForegroundYield = [&context] { context.MarkForegroundYield(); };
		context.shouldContinue = [continueBeforeAdmission, markForegroundYield] {
			if (continueBeforeAdmission && !continueBeforeAdmission()) return false;
			if (SourceWorkCoordinator::Global().Snapshot().foregroundPending) {
				markForegroundYield();
				return false;
			}
			return true;
		};
	}
	ScopedWorkContext activeContext(context);
	ArchiveLocation location;
	if (!ParseArchiveLocation(directory, location)) return false;
	const std::function<bool()> effectiveContinue = context.shouldContinue;
	const std::shared_ptr<const ArchiveCatalog> catalog =
		archive_detail::GetCatalog(location.archive, effectiveContinue, errorMessage, errorKind);
	if (!catalog) return false;
	context.sourceAccessAlreadyAdmitted = false;
	context.cpuProcessingAlreadyAdmitted = false;
	admission.Reset();
	if (containsEncryptedEntries != nullptr) {
		*containsEncryptedEntries = catalog->containsEncryptedEntries;
	}
	const SourceIdentity& backingIdentity = catalog->identity;
	if (!ShouldContinue(effectiveContinue)) {
		errorMessage = "archive listing was cancelled";
		return false;
	}
	const auto children = catalog->directories.find(location.memberDirectory);
	if (children == catalog->directories.end()) return true;
	entries.reserve(children->second.size());
	for (const CatalogChild& child : children->second) {
		if (!ShouldContinue(effectiveContinue)) {
			entries.clear();
			errorMessage = "archive listing was cancelled";
			return false;
		}
		const std::string member = location.memberDirectory.empty() ? child.name :
			location.memberDirectory + "/" + child.name;
		entries.push_back(ArchiveEntryInfo{
			location.archive / fs::path(member), child.directory, child.size,
			child.modificationTime, child.encrypted, backingIdentity});
	}
	return true;
}

bool GetArchiveMemberInfo(const fs::path& path, ArchiveMemberInfo& info,
	std::string& errorMessage) {
	return GetArchiveMemberInfo(path, info, errorMessage, nullptr, WorkContext{});
}

bool GetArchiveMemberInfo(const fs::path& path, ArchiveMemberInfo& info,
	std::string& errorMessage, ArchiveErrorKind* errorKind,
	const WorkContext& suppliedWorkContext) {
	info = {};
	errorMessage.clear();
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	WorkContext context = ResolveWorkContext(path, SourceWorkPriority::Metadata,
		suppliedWorkContext);
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
		context, ArchiveBackingFileForAdmission(path));
	if (!admission) {
		errorMessage = "archive member metadata admission was cancelled";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	const std::function<bool()> continueBeforeAdmission = context.shouldContinue;
	if (context.Priority() != SourceWorkPriority::Foreground) {
		context.shouldContinue = [&context, continueBeforeAdmission] {
			if (continueBeforeAdmission && !continueBeforeAdmission()) return false;
			if (context.Priority() != SourceWorkPriority::Foreground &&
				SourceWorkCoordinator::Global().Snapshot().foregroundPending) {
				context.MarkForegroundYield();
				return false;
			}
			return true;
		};
	}
	ScopedWorkContext activeContext(context);
	ArchiveLocation location;
	if (!ParseArchiveLocation(path, location) || location.memberDirectory.empty()) {
		errorMessage = "not an archive image member";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	const std::shared_ptr<const ArchiveCatalog> catalog =
		archive_detail::GetCatalog(location.archive, context.shouldContinue, errorMessage, errorKind);
	if (!catalog) return false;
	context.sourceAccessAlreadyAdmitted = false;
	context.cpuProcessingAlreadyAdmitted = false;
	admission.Reset();
	if (!context.Continue()) {
		errorMessage = "archive member metadata lookup was cancelled";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	const auto found = catalog->members.find(location.memberDirectory);
	if (found == catalog->members.end()) {
		errorMessage = "archive member no longer exists";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	info.size = found->second.size;
	info.modificationTime = found->second.modificationTime;
	info.encrypted = found->second.encrypted;
	return true;
}

bool HasSessionArchivePassword(const fs::path& path) {
	std::string password;
	const bool found = ReadSessionPassword(path, password);
	ClearPasswordString(password);
	return found;
}

bool HasSessionArchivePassword(const fs::path& path,
	const WorkContext& suppliedWorkContext) {
	WorkContext context = ResolveWorkContext(path, SourceWorkPriority::Metadata,
		suppliedWorkContext);
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
		context, ArchiveBackingFileForAdmission(path));
	if (!admission || !context.Continue()) return false;
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	ScopedWorkContext activeContext(context);
	return HasSessionArchivePassword(path);
}

bool SetSessionArchivePassword(const fs::path& path, const std::string& password) {
	std::string key;
	BackingIdentity identity;
	if (!PasswordCacheKey(path, key, identity)) return false;
	SessionPasswordCache& cache = GlobalSessionPasswordCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	const auto existing = cache.values.find(key);
	if (existing != cache.values.end()) ClearPasswordString(existing->second.value);
	cache.values[key] = SessionPassword{identity, password};
	return true;
}

void ForgetSessionArchivePassword(const fs::path& path) {
	std::string key;
	BackingIdentity identity;
	if (PasswordCacheKey(path, key, identity)) {
		EraseSessionPassword(key);
		archive_detail::RemoveHeaderEncryptedCatalog(ArchiveBackingFile(path).lexically_normal().string());
	}
}

void ClearSessionArchivePasswords() {
	SessionPasswordCache& cache = GlobalSessionPasswordCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	for (auto& item : cache.values) ClearPasswordString(item.second.value);
	cache.values.clear();
	archive_detail::RemoveAllHeaderEncryptedCatalogs();
}

bool ValidateArchivePassword(const fs::path& path, const std::string& password,
	std::string& errorMessage, ArchiveErrorKind* errorKind) {
	return ValidateArchivePassword(path, password, errorMessage, errorKind, WorkContext{});
}

bool ValidateArchivePassword(const fs::path& path, const std::string& password,
	std::string& errorMessage, ArchiveErrorKind* errorKind,
	const WorkContext& suppliedWorkContext) {
	errorMessage.clear();
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	WorkContext context = ResolveWorkContext(path, SourceWorkPriority::Metadata,
		suppliedWorkContext);
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
		context, ArchiveBackingFileForAdmission(path));
	if (!admission) {
		errorMessage = "archive password validation was cancelled before admission";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	ScopedWorkContext activeContext(context);
	ArchiveLocation location;
	if (!ParseArchiveLocation(path, location)) {
		errorMessage = "not an archive container";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	const std::function<bool()> shouldContinue = [&context] { return context.Continue(); };
	const auto canceled = [&] {
		errorMessage = "archive password validation was cancelled";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	};
	if (!ShouldContinue(shouldContinue)) return canceled();
	if (location.format == ArchiveFormat::SevenZip) {
		std::vector<SevenZipEntry> entries;
		bool headerEncrypted = false;
		if (!ReadSevenZipCatalog(location.archive, password, shouldContinue, entries,
			headerEncrypted, errorMessage, errorKind)) return false;
		if (!ShouldContinue(shouldContinue)) return canceled();
		const auto encrypted = std::find_if(entries.begin(), entries.end(),
			[](const SevenZipEntry& item) {
				return item.encrypted && !item.directory && !item.specialFile;
			});
		if (encrypted == entries.end()) return true;
		const auto verifiable = std::find_if(entries.begin(), entries.end(),
			IsSevenZipPasswordVerificationMember);
		if (verifiable == entries.end()) {
			errorMessage = "cannot confirm a 7z password because encrypted entries have no data";
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
			return false;
		}
		const auto& passwordProbe = *verifiable;
		if (passwordProbe.size > kMaximumArchiveMemberBytes) {
			errorMessage = "cannot validate the password: encrypted 7z member exceeds the 128 MiB limit";
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
			return false;
		}
		std::uint64_t total = 0;
		const SevenZipDataWriter discard = [&total, &passwordProbe](
			const std::uint8_t*, std::size_t size, std::string& sinkError) {
			const std::uint64_t chunk = static_cast<std::uint64_t>(size);
			if (total > passwordProbe.size || chunk > kMaximumArchiveMemberBytes - total ||
				chunk > passwordProbe.size - total) {
				sinkError = "encrypted 7z member expanded beyond its declared size or the 128 MiB limit";
				return false;
			}
			total += chunk;
			return true;
		};
		if (!ExtractSevenZipMember(location.archive, passwordProbe.index, passwordProbe.rawPath,
			password, shouldContinue, discard, errorMessage, errorKind)) return false;
		if (!ShouldContinue(shouldContinue)) return canceled();
		if (total != passwordProbe.size) {
			errorMessage = "encrypted 7z member ended before its declared size";
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::InvalidPassword;
			return false;
		}
		return true;
	}
	if (location.format == ArchiveFormat::Rar) {
		if (!RarBackendAvailable()) {
			errorMessage = "encrypted RAR support is unavailable in this build";
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
			return false;
		}
		std::vector<RarEntry> entries;
		bool headerEncrypted = false;
		if (!ReadRarCatalog(location.archive, password, shouldContinue, entries,
			headerEncrypted, errorMessage, errorKind)) return false;
		if (!ShouldContinue(shouldContinue)) return canceled();
		const auto encrypted = std::find_if(entries.begin(), entries.end(),
			[](const RarEntry& entry) {
			return entry.encrypted && !entry.directory && !entry.specialFile &&
				!entry.split && entry.size != 0 && entry.size <= kMaximumArchiveMemberBytes;
		});
		if (encrypted == entries.end()) {
			if (headerEncrypted) return true;
			const bool containsEncrypted = std::any_of(entries.begin(), entries.end(),
				[](const RarEntry& entry) { return entry.encrypted; });
			if (!containsEncrypted) return true;
			errorMessage = "cannot validate the password: encrypted RAR members exceed the 128 MiB limit";
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
			return false;
		}
		std::uint64_t total = 0;
		const RarDataWriter discard = [&total, &encrypted](
			const std::uint8_t*, std::size_t size, std::string& sinkError) {
			const std::uint64_t chunk = static_cast<std::uint64_t>(size);
			if (total > encrypted->size || chunk > kMaximumArchiveMemberBytes - total ||
				chunk > encrypted->size - total) {
				sinkError = "encrypted RAR member expanded beyond its declared size or the 128 MiB limit";
				return false;
			}
			total += chunk;
			return true;
		};
		if (!ExtractRarMember(location.archive, encrypted->index, encrypted->rawPath,
			password, shouldContinue, discard, errorMessage, errorKind)) return false;
		if (!ShouldContinue(shouldContinue)) return canceled();
		if (total != encrypted->size) {
			errorMessage = "encrypted RAR member ended before its declared size";
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::InvalidPassword;
			return false;
		}
		return true;
	}
	const std::shared_ptr<const ArchiveCatalog> catalog =
		archive_detail::GetCatalog(location.archive, shouldContinue, errorMessage, errorKind);
	if (!catalog) return false;
	if (!ShouldContinue(shouldContinue)) return canceled();
	if (!catalog->containsEncryptedEntries) return true;
	if (location.format != ArchiveFormat::Zip) {
		errorMessage = "password-protected archive format is not supported";
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::UnsupportedEncryption;
		return false;
	}
	const auto encrypted = std::find_if(catalog->members.begin(), catalog->members.end(),
		[](const auto& item) { return item.second.encrypted && item.second.size != 0; });
	if (encrypted == catalog->members.end()) return true;
	int zipError = 0;
	zip_t* archive = zip_open(location.archive.c_str(), ZIP_RDONLY, &zipError);
	if (archive == nullptr) {
		errorMessage = ZipOpenError(zipError);
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	std::unique_ptr<zip_t, decltype(&zip_discard)> archiveOwner(archive, &zip_discard);
	if (!ShouldContinue(shouldContinue)) return canceled();
	zip_file_t* file = zip_fopen_index_encrypted(archive, encrypted->second.index,
		ZIP_FL_UNCHANGED, password.c_str());
	if (file == nullptr) {
		zip_error_t* archiveError = zip_get_error(archive);
		const ArchiveErrorKind kind = ZipErrorKind(archiveError);
		errorMessage = kind == ArchiveErrorKind::PasswordRequired ?
			"password is required for this archive" :
			kind == ArchiveErrorKind::InvalidPassword ? "incorrect archive password" : ZipError(archive);
		if (errorKind != nullptr) *errorKind = kind;
		return false;
	}
	std::unique_ptr<zip_file_t, decltype(&zip_fclose)> fileOwner(file, &zip_fclose);
	if (!ShouldContinue(shouldContinue)) return canceled();
	std::uint8_t probe = 0;
	if (zip_fread(file, &probe, 1) < 0) {
		const ArchiveErrorKind kind = ZipErrorKind(zip_file_get_error(file));
		errorMessage = kind == ArchiveErrorKind::InvalidPassword ?
			"incorrect archive password" : ZipError(archive);
		if (errorKind != nullptr) *errorKind = kind;
		return false;
	}
	if (!ShouldContinue(shouldContinue)) return canceled();
	return true;
}

bool WithArchiveMemberFile(const fs::path& path,
	const std::function<bool(const fs::path&, std::string&)>& callback,
	std::string& errorMessage, ArchiveErrorKind* errorKind,
	const std::function<bool()>& shouldContinue) {
	errorMessage.clear();
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	const auto fail = [&errorMessage, errorKind](const std::string& message,
		ArchiveErrorKind kind = ArchiveErrorKind::Other) {
		errorMessage = message;
		if (errorKind != nullptr) *errorKind = kind;
		return false;
	};
	if (!callback) {
		return fail("archive member decoder callback is missing");
	}
	WorkContext supplied;
	supplied.shouldContinue = shouldContinue;
	WorkContext context = ResolveWorkContext(path,
		SourceWorkPriority::Foreground, supplied);
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
		context, ArchiveBackingFileForAdmission(path));
	if (!admission || !context.Continue()) {
		return fail("archive member read was cancelled before admission");
	}
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	ScopedWorkContext activeContext(context);
	const std::function<bool()> effectiveContinue = [&context] {
		return context.Continue();
	};
	if (!effectiveContinue()) return fail("archive member read was cancelled");
	ArchiveLocation location;
	if (!ParseArchiveLocation(path, location) || location.memberDirectory.empty()) {
		return fail("not an archive image member");
	}
	const std::shared_ptr<const ArchiveCatalog> catalog =
		archive_detail::GetCatalog(location.archive, effectiveContinue, errorMessage, errorKind);
	if (!catalog) return false;
	const auto member = catalog->members.find(location.memberDirectory);
	if (member == catalog->members.end()) {
		return fail("archive member no longer exists");
	}
	if (member->second.size > kMaximumArchiveMemberBytes) {
		return fail("archive image exceeds the 128 MiB member limit");
	}
	// Count the backing-container read separately from any mapped read of the
	// short-lived member file used by the codec callback.
	PerfDiagnostics::Instance().Record(PerfMetric::SourceRead);
	if (member->second.backend == ArchiveBackend::Libarchive) {
		if (member->second.encrypted) {
			return fail("encrypted archive format is not supported", ArchiveErrorKind::UnsupportedEncryption);
		}
		BackingIdentity currentIdentity;
		if (!StatIdentity(location.archive, currentIdentity) || !(currentIdentity == catalog->identity)) {
			return fail("archive changed before the image could be read");
		}
		return archive_detail::WithLibarchiveMemberFile(path, location, *catalog, member->second,
			callback, errorMessage, effectiveContinue);
	}
	if (member->second.backend == ArchiveBackend::SevenZip) {
		return archive_detail::WithSevenZipMemberFile(path, location, *catalog, member->second,
			callback, errorMessage, errorKind, effectiveContinue);
	}
	if (member->second.backend == ArchiveBackend::Rar) {
		return archive_detail::WithRarMemberFile(path, location, *catalog, member->second,
			callback, errorMessage, errorKind, effectiveContinue);
	}
	std::string password;
	if (member->second.encrypted && !ReadSessionPassword(location.archive, password)) {
		return fail("password required for encrypted archive image", ArchiveErrorKind::PasswordRequired);
	}
	BackingIdentity currentIdentity;
	if (!StatIdentity(location.archive, currentIdentity) || !(currentIdentity == catalog->identity)) {
		ClearPasswordString(password);
		return fail("ZIP archive changed before the image could be read");
	}
	int zipError = 0;
	zip_t* archive = zip_open(location.archive.c_str(), ZIP_RDONLY, &zipError);
	if (archive == nullptr) {
		errorMessage = ZipOpenError(zipError);
		ClearPasswordString(password);
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
		ClearPasswordString(password);
		return fail("ZIP member changed since it was listed");
	}
	zip_file_t* zipFile = member->second.encrypted ?
		zip_fopen_index_encrypted(archive, member->second.index, ZIP_FL_UNCHANGED,
			password.c_str()) :
		zip_fopen_index(archive, member->second.index, ZIP_FL_UNCHANGED);
	if (zipFile == nullptr) {
		const ArchiveErrorKind kind = ZipErrorKind(zip_get_error(archive));
		errorMessage = kind == ArchiveErrorKind::PasswordRequired ?
			"password required for encrypted archive image" :
			kind == ArchiveErrorKind::InvalidPassword ? "incorrect archive password" : ZipError(archive);
		if (errorKind != nullptr) *errorKind = kind;
		ClearPasswordString(password);
		return false;
	}
	ClearPasswordString(password);
	std::unique_ptr<zip_file_t, decltype(&zip_fclose)> zipFileOwner(zipFile, &zip_fclose);
	PrivateMemberFile temporary;
	if (!MakePrivateMemberFile(path, temporary, errorMessage)) {
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	std::vector<std::uint8_t> buffer(64 * 1024);
	std::uint64_t total = 0;
	for (;;) {
		if (!ShouldContinue(effectiveContinue)) {
			ClearPasswordString(password);
			return fail("archive member read was cancelled");
		}
		const zip_int64_t count = zip_fread(zipFile, buffer.data(), buffer.size());
		if (count < 0) {
			const ArchiveErrorKind kind = ZipErrorKind(zip_file_get_error(zipFile));
			errorMessage = kind == ArchiveErrorKind::InvalidPassword ?
				"incorrect archive password" : ZipError(archive);
			if (errorKind != nullptr) *errorKind = kind;
			return false;
		}
		if (count == 0) break;
		const std::uint64_t chunk = static_cast<std::uint64_t>(count);
		if (total > member->second.size || chunk > kMaximumArchiveMemberBytes - total ||
			chunk > member->second.size - total) {
			return fail("archive member expanded beyond its declared size or the 128 MiB limit");
		}
		if (!WriteAll(temporary.descriptor, buffer.data(), static_cast<std::size_t>(count),
			errorMessage)) {
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
			return false;
		}
		total += chunk;
	}
	if (total != member->second.size) {
		return fail("archive member ended before its declared size");
	}
	if (!ShouldContinue(effectiveContinue)) {
		return fail("archive member read was cancelled");
	}
	if (zip_fclose(zipFileOwner.release()) != 0) {
		const ArchiveErrorKind kind = ZipErrorKind(zip_get_error(archive));
		errorMessage = kind == ArchiveErrorKind::InvalidPassword ?
			"incorrect archive password" : ZipError(archive);
		if (errorKind != nullptr) *errorKind = kind;
		return false;
	}
	BackingIdentity afterRead;
	if (!StatIdentity(location.archive, afterRead) || !(afterRead == catalog->identity)) {
		return fail("ZIP archive changed while the image was being read");
	}
	if (::lseek(temporary.descriptor, 0, SEEK_SET) < 0) {
		return fail("cannot rewind the archive image memory file");
	}
	try {
		return callback(temporary.link, errorMessage);
	} catch (const std::exception& error) {
		return fail(error.what());
	}
}

bool IdentifyImageSourceBackingFile(const fs::path& path, std::uint64_t& device,
	std::uint64_t& inode, std::uint64_t& size, std::int64_t& modifiedSeconds,
	std::int64_t& modifiedNanoseconds) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Metadata);
	BackingIdentity identity;
	if (!StatIdentity(ArchiveBackingFile(path), identity)) return false;
	device = identity.device;
	inode = identity.inode;
	size = identity.size;
	modifiedSeconds = identity.modifiedSeconds;
	modifiedNanoseconds = identity.modifiedNanoseconds;
	return true;
}

bool operator==(const SourceIdentity& left, const SourceIdentity& right) {
	return left.device == right.device && left.inode == right.inode &&
		left.size == right.size && left.modifiedSeconds == right.modifiedSeconds &&
		left.modifiedNanoseconds == right.modifiedNanoseconds && left.valid == right.valid;
}

bool operator!=(const SourceIdentity& left, const SourceIdentity& right) {
	return !(left == right);
}

bool operator==(const SourceKey& left, const SourceKey& right) {
	return left.logicalPath == right.logicalPath &&
		left.backingIdentity == right.backingIdentity;
}

bool operator!=(const SourceKey& left, const SourceKey& right) {
	return !(left == right);
}

bool operator==(const SourceKey& left, const std::string& right) {
	return left.logicalPath == right;
}

bool operator!=(const SourceKey& left, const std::string& right) {
	return !(left == right);
}

bool operator==(const std::string& left, const SourceKey& right) {
	return right == left;
}

bool operator!=(const std::string& left, const SourceKey& right) {
	return !(left == right);
}

bool operator==(const SourceKey& left, const char* right) {
	return left.logicalPath == (right == nullptr ? "" : right);
}

bool operator!=(const SourceKey& left, const char* right) {
	return !(left == right);
}

bool operator==(const char* left, const SourceKey& right) {
	return right == left;
}

bool operator!=(const char* left, const SourceKey& right) {
	return !(left == right);
}

namespace {

std::size_t HashCombine(std::size_t seed, std::size_t value) {
	return seed ^ (value + static_cast<std::size_t>(0x9e3779b9u) +
		(seed << 6) + (seed >> 2));
}

fs::path NormalizeLogicalPath(const fs::path& path) {
	std::error_code error;
	const fs::path absolute = fs::absolute(path, error);
	return (error ? path : absolute).lexically_normal();
}


} // namespace

std::size_t SourceKeyHash::operator()(const SourceKey& key) const {
	std::size_t value = std::hash<std::string>{}(key.logicalPath);
	value = HashCombine(value, std::hash<std::uint64_t>{}(key.backingIdentity.device));
	value = HashCombine(value, std::hash<std::uint64_t>{}(key.backingIdentity.inode));
	value = HashCombine(value, std::hash<std::uint64_t>{}(key.backingIdentity.size));
	value = HashCombine(value, std::hash<std::int64_t>{}(key.backingIdentity.modifiedSeconds));
	value = HashCombine(value, std::hash<std::int64_t>{}(key.backingIdentity.modifiedNanoseconds));
	return HashCombine(value, std::hash<bool>{}(key.backingIdentity.valid));
}

SourceDescriptor::SourceDescriptor(fs::path logicalPath, SourceIdentity backingIdentity,
	SourceMetadata metadata)
	: logicalPath_(NormalizeLogicalPath(logicalPath)),
	  backingIdentity_(backingIdentity), metadata_(metadata) {}

SourceKey SourceDescriptor::Key() const {
	return SourceKey{logicalPath_.string(), backingIdentity_};
}

SourceDescriptor SourceDescriptor::WithImageProperties(int width, int height,
	bool hasTransparency) const {
	SourceMetadata metadata = metadata_;
	metadata.width = width;
	metadata.height = height;
	metadata.hasDimensions = width > 0 && height > 0;
	metadata.hasTransparency = hasTransparency;
	metadata.transparencyKnown = true;
	return SourceDescriptor(logicalPath_, backingIdentity_, metadata);
}

bool CaptureImageSourceIdentity(const fs::path& path, SourceIdentity& identity) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Metadata);
	return StatIdentity(ArchiveBackingFile(path), identity);
}

SourceDescriptor DescribeArchiveMember(const fs::path& path,
	const SourceIdentity& backingIdentity, std::uint64_t memberSize,
	std::int64_t modificationTimeSeconds, bool encrypted) {
	SourceMetadata metadata;
	metadata.fileSize = memberSize;
	metadata.modificationTimeNanoseconds = UnixNanoseconds(modificationTimeSeconds);
	metadata.creationTimeNanoseconds = metadata.modificationTimeNanoseconds;
	metadata.hasFileSize = true;
	metadata.hasModificationTime = true;
	metadata.hasCreationTime = true;
	metadata.archiveMember = true;
	metadata.archiveMemberEncrypted = encrypted;
	return SourceDescriptor(path, backingIdentity, metadata);
}

SourceDescriptor DescribeImageSource(const fs::path& path) {
	return DescribeImageSource(path, WorkContext{});
}

SourceDescriptor DescribeImageSource(const fs::path& path,
	const WorkContext& workContext) {
	SourceIdentity identity;
	if (IsArchiveMemberLocation(path)) {
		if (!CaptureImageSourceIdentity(path, identity)) {
			return SourceDescriptor(path, identity, {});
		}
		ArchiveMemberInfo member;
		std::string errorMessage;
		ArchiveErrorKind errorKind = ArchiveErrorKind::None;
		if (!GetArchiveMemberInfo(path, member, errorMessage, &errorKind, workContext)) {
			SourceMetadata metadata;
			metadata.archiveMember = true;
			return SourceDescriptor(path, identity, metadata);
		}
		return DescribeArchiveMember(path, identity, member.size,
			member.modificationTime, member.encrypted);
	}

	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Metadata);
	std::int64_t birthTimeNanoseconds = 0;
	bool hasBirthTime = false;
	if (!StatIdentity(path, identity, &birthTimeNanoseconds, &hasBirthTime)) {
		return SourceDescriptor(path, identity, {});
	}
	SourceMetadata metadata;
	metadata.fileSize = identity.size;
	metadata.modificationTimeNanoseconds = UnixNanoseconds(identity.modifiedSeconds,
		identity.modifiedNanoseconds);
	metadata.creationTimeNanoseconds = hasBirthTime ? birthTimeNanoseconds :
		metadata.modificationTimeNanoseconds;
	metadata.hasFileSize = true;
	metadata.hasModificationTime = true;
	metadata.hasCreationTime = true;
	return SourceDescriptor(path, identity, metadata);
}

bool IsImageSourceCurrent(const SourceDescriptor& descriptor) {
	if (!descriptor.Valid()) return false;
	SourceIdentity current;
	if (!CaptureImageSourceIdentity(descriptor.LogicalPath(), current) ||
		current != descriptor.BackingIdentity()) return false;
	if (!descriptor.Metadata().archiveMember) return true;
	ArchiveMemberInfo member;
	std::string errorMessage;
	return GetArchiveMemberInfo(descriptor.LogicalPath(), member, errorMessage) &&
		member.size == descriptor.Metadata().fileSize &&
		UnixNanoseconds(member.modificationTime) ==
			descriptor.Metadata().modificationTimeNanoseconds &&
		member.encrypted == descriptor.Metadata().archiveMemberEncrypted;
}

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
void SetArchiveCatalogTestHookForTesting(
	detail::ArchiveCatalogTestHook hook, void* context) {
	archive_detail::SetCatalogTestHook(hook, context);
}

void SetArchiveSourceProbeHookForTesting(
	detail::ArchiveSourceProbeHook hook, void* context) {
	ArchiveSourceProbeHooks& hooks = GlobalArchiveSourceProbeHooks();
	std::lock_guard<std::mutex> lock(hooks.mutex);
	hooks.hook = hook;
	hooks.context = context;
}

void SetStatxUnavailableForTesting(bool unavailable) {
	gStatxUnavailableForTesting.store(unavailable);
}
#endif

} // namespace jpegview_linux
