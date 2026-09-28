#include "seven_zip_backend.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>
#include <vector>

#include "Common/MyInitGuid.h"
#include "Common/MyCom.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/IProgress.h"
#include "7zip/IStream.h"
#include "7zip/IPassword.h"
#include "7zip/PropID.h"

namespace jpegview_linux {
namespace {

constexpr std::uint64_t kMaximumSevenZipCatalogEntries = 100000;
constexpr std::uint64_t kWindowsToUnixEpochTicks = 11644473600ull * 10000000ull;
constexpr std::uint64_t kTicksPerSecond = 10000000ull;
constexpr HRESULT kPointerError = static_cast<HRESULT>(0x80004003u);

#define DEFINE_GUID_7Z_FORMAT(name, id) Z7_DEFINE_GUID(name, \
	0x23170F69, 0x40C1, 0x278A, 0x10, 0x00, 0x00, 0x01, 0x10, id, 0x00, 0x00)
DEFINE_GUID_7Z_FORMAT(CLSID_SevenZipFormat, 7);
#undef DEFINE_GUID_7Z_FORMAT

using CreateObjectFunction = HRESULT (*)(const GUID*, const GUID*, void**);

struct PluginHandle {
	void* library = nullptr;
	CreateObjectFunction createObject = nullptr;
	std::string error;
};

std::filesystem::path ExecutableDirectory() {
	char buffer[4096];
	const ssize_t count = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(buffer) - 1) return {};
	buffer[count] = '\0';
	return std::filesystem::path(buffer).parent_path();
}

PluginHandle LoadPlugin() {
	PluginHandle plugin;
	const std::filesystem::path executableDirectory = ExecutableDirectory();
	if (executableDirectory.empty()) {
		plugin.error = "cannot locate the executable for the private 7-Zip reader";
		return plugin;
	}
	const std::filesystem::path candidates[] = {
		executableDirectory / "lib/jpegview-linux/7z.so",
		executableDirectory / "../lib/jpegview-linux/7z.so",
	};
	for (const std::filesystem::path& candidate : candidates) {
		plugin.library = ::dlopen(candidate.lexically_normal().c_str(), RTLD_NOW | RTLD_LOCAL);
		if (plugin.library == nullptr) continue;
		plugin.createObject = reinterpret_cast<CreateObjectFunction>(
			::dlsym(plugin.library, "CreateObject"));
		if (plugin.createObject != nullptr) return plugin;
		const char* message = ::dlerror();
		plugin.error = message == nullptr ? "private 7-Zip reader has no CreateObject export" : message;
		(void)::dlclose(plugin.library);
		plugin.library = nullptr;
	}
	if (plugin.error.empty()) {
		plugin.error = "private 7-Zip reader is unavailable beside the executable";
	}
	return plugin;
}

const PluginHandle& Plugin() {
	static const PluginHandle plugin = LoadPlugin();
	return plugin;
}

bool ContinueWork(const std::function<bool()>& shouldContinue) {
	return !shouldContinue || shouldContinue();
}

void SetError(std::string& errorMessage, ArchiveErrorKind* errorKind,
	const char* message, ArchiveErrorKind kind = ArchiveErrorKind::Other) {
	errorMessage = message;
	if (errorKind != nullptr) *errorKind = kind;
}

void Wipe(std::string& value) {
	volatile char* bytes = value.empty() ? nullptr : &value[0];
	for (std::size_t index = 0; index < value.size(); ++index) bytes[index] = '\0';
	value.clear();
}

template <typename T>
void WipeVector(std::vector<T>& value) {
	volatile unsigned char* bytes = reinterpret_cast<volatile unsigned char*>(value.data());
	for (std::size_t index = 0; index < value.size() * sizeof(T); ++index) bytes[index] = 0;
	value.clear();
}

template <typename T>
class VectorWiper {
	std::vector<T>& value_;

public:
	explicit VectorWiper(std::vector<T>& value) : value_(value) {}
	~VectorWiper() { WipeVector(value_); }
};

bool DecodeUtf8(const std::string& utf8, std::vector<std::uint32_t>& codepoints) {
	codepoints.clear();
	for (std::size_t index = 0; index < utf8.size();) {
		const unsigned char lead = static_cast<unsigned char>(utf8[index]);
		std::uint32_t codepoint = 0;
		std::size_t count = 0;
		if (lead <= 0x7fu) { codepoint = lead; count = 1; }
		else if (lead >= 0xc2u && lead <= 0xdfu) { codepoint = lead & 0x1fu; count = 2; }
		else if (lead >= 0xe0u && lead <= 0xefu) { codepoint = lead & 0x0fu; count = 3; }
		else if (lead >= 0xf0u && lead <= 0xf4u) { codepoint = lead & 0x07u; count = 4; }
		else return false;
		if (count > utf8.size() - index) return false;
		for (std::size_t part = 1; part < count; ++part) {
			const unsigned char continuation = static_cast<unsigned char>(utf8[index + part]);
			if ((continuation & 0xc0u) != 0x80u) return false;
			codepoint = (codepoint << 6) | (continuation & 0x3fu);
		}
		if ((count == 2 && codepoint < 0x80u) || (count == 3 && codepoint < 0x800u) ||
			(count == 4 && codepoint < 0x10000u) || codepoint > 0x10ffffu ||
			(codepoint >= 0xd800u && codepoint <= 0xdfffu)) return false;
		codepoints.push_back(codepoint);
		index += count;
	}
	return true;
}

void AppendUtf8(std::uint32_t codepoint, std::string& output) {
	if (codepoint <= 0x7fu) {
		output.push_back(static_cast<char>(codepoint));
	} else if (codepoint <= 0x7ffu) {
		output.push_back(static_cast<char>(0xc0u | (codepoint >> 6)));
		output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
	} else if (codepoint <= 0xffffu) {
		output.push_back(static_cast<char>(0xe0u | (codepoint >> 12)));
		output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
		output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
	} else {
		output.push_back(static_cast<char>(0xf0u | (codepoint >> 18)));
		output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3fu)));
		output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
		output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
	}
}

HRESULT PasswordToBstr(const std::string& password, BSTR* result) {
	if (result == nullptr) return kPointerError;
	*result = nullptr;
	std::vector<std::uint32_t> codepoints;
	VectorWiper<std::uint32_t> codepointWiper(codepoints);
	try {
		if (!DecodeUtf8(password, codepoints)) return E_INVALIDARG;
	} catch (const std::bad_alloc&) {
		return E_OUTOFMEMORY;
	}
	std::vector<OLECHAR> unicode;
	VectorWiper<OLECHAR> unicodeWiper(unicode);
	try {
		for (const std::uint32_t codepoint : codepoints) {
			if (sizeof(OLECHAR) == 2 && codepoint > 0xffffu) {
				const std::uint32_t scalar = codepoint - 0x10000u;
				unicode.push_back(static_cast<OLECHAR>(0xd800u + (scalar >> 10)));
				unicode.push_back(static_cast<OLECHAR>(0xdc00u + (scalar & 0x3ffu)));
			} else {
				unicode.push_back(static_cast<OLECHAR>(codepoint));
			}
		}
	} catch (const std::bad_alloc&) {
		return E_OUTOFMEMORY;
	}
	if (unicode.size() > (std::numeric_limits<std::uint32_t>::max() - sizeof(OLECHAR)) /
		sizeof(OLECHAR)) return E_INVALIDARG;
	const std::uint32_t byteLength = static_cast<std::uint32_t>(unicode.size() * sizeof(OLECHAR));
	const std::size_t allocationSize = sizeof(byteLength) + byteLength + sizeof(OLECHAR);
	auto* allocation = static_cast<std::uint8_t*>(std::malloc(allocationSize));
	if (allocation == nullptr) return E_OUTOFMEMORY;
	std::memcpy(allocation, &byteLength, sizeof(byteLength));
	BSTR value = reinterpret_cast<BSTR>(allocation + sizeof(byteLength));
	if (byteLength != 0) std::memcpy(value, unicode.data(), byteLength);
	value[unicode.size()] = 0;
	*result = value;
	return S_OK;
}

std::uint32_t BstrByteLength(BSTR value) {
	if (value == nullptr) return 0;
	std::uint32_t byteLength = 0;
	std::memcpy(&byteLength, reinterpret_cast<const std::uint8_t*>(value) -
		sizeof(byteLength), sizeof(byteLength));
	return byteLength;
}

void FreeBstr(BSTR value) {
	if (value != nullptr) {
		const std::uint32_t byteLength = BstrByteLength(value);
		std::uint8_t* allocation = reinterpret_cast<std::uint8_t*>(value) -
			sizeof(std::uint32_t);
		volatile unsigned char* bytes = allocation;
		const std::size_t allocationSize = sizeof(std::uint32_t) + byteLength + sizeof(OLECHAR);
		for (std::size_t index = 0; index < allocationSize; ++index) bytes[index] = 0;
		std::free(allocation);
	}
}

bool BstrToUtf8(BSTR value, std::string& output) {
	output.clear();
	const std::uint32_t byteLength = BstrByteLength(value);
	if (byteLength % sizeof(OLECHAR) != 0) return false;
	const std::size_t length = byteLength / sizeof(OLECHAR);
	try {
		for (std::size_t index = 0; index < length; ++index) {
			std::uint32_t codepoint = static_cast<std::uint32_t>(value[index]);
			if (sizeof(OLECHAR) == 2 && codepoint >= 0xd800u && codepoint <= 0xdbffu) {
				if (index + 1 >= length) return false;
				const std::uint32_t low = static_cast<std::uint32_t>(value[++index]);
				if (low < 0xdc00u || low > 0xdfffu) return false;
				codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) + (low - 0xdc00u);
			} else if ((codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
				codepoint > 0x10ffffu) {
				return false;
			}
			AppendUtf8(codepoint, output);
		}
	} catch (const std::bad_alloc&) {
		return false;
	}
	return true;
}

class ArchiveInputStream final : public IInStream, public IStreamGetSize,
	public CMyUnknownImp {
	Z7_COM_UNKNOWN_IMP_3(IInStream, ISequentialInStream, IStreamGetSize)
	Z7_IFACE_COM7_IMP(ISequentialInStream)
	Z7_IFACE_COM7_IMP(IInStream)
	Z7_IFACE_COM7_IMP(IStreamGetSize)
	public:

	int descriptor_;
	std::uint64_t size_;
	std::uint64_t position_ = 0;
	const std::function<bool()>* shouldContinue_;
	std::mutex mutex_;

public:
	ArchiveInputStream(int descriptor, std::uint64_t size,
		const std::function<bool()>* shouldContinue)
		: descriptor_(descriptor), size_(size), shouldContinue_(shouldContinue) {}

	~ArchiveInputStream() {
		if (descriptor_ >= 0) (void)::close(descriptor_);
	}
};

Z7_COM7F_IMF(ArchiveInputStream::Read(void* data, UInt32 size, UInt32* processedSize)) {
	if (processedSize == nullptr) return kPointerError;
	*processedSize = 0;
	if (size == 0) return S_OK;
	if (shouldContinue_ != nullptr && !ContinueWork(*shouldContinue_)) return E_ABORT;
	std::lock_guard<std::mutex> lock(mutex_);
	if (position_ >= size_) return S_OK;
	const std::uint64_t remaining = size_ - position_;
	const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(size, remaining));
	if (position_ > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) return E_FAIL;
	for (;;) {
		const ssize_t count = ::pread(descriptor_, data, requested,
			static_cast<off_t>(position_));
		if (count < 0 && errno == EINTR) continue;
		if (count < 0) return E_FAIL;
		position_ += static_cast<std::uint64_t>(count);
		*processedSize = static_cast<UInt32>(count);
		return S_OK;
	}
}

Z7_COM7F_IMF(ArchiveInputStream::Seek(Int64 offset, UInt32 seekOrigin,
	UInt64* newPosition)) {
	if (shouldContinue_ != nullptr && !ContinueWork(*shouldContinue_)) return E_ABORT;
	std::lock_guard<std::mutex> lock(mutex_);
	Int64 base = 0;
	if (seekOrigin == 0) base = 0;
	else if (seekOrigin == 1) {
		if (position_ > static_cast<std::uint64_t>(std::numeric_limits<Int64>::max())) return E_FAIL;
		base = static_cast<Int64>(position_);
	} else if (seekOrigin == 2) {
		if (size_ > static_cast<std::uint64_t>(std::numeric_limits<Int64>::max())) return E_FAIL;
		base = static_cast<Int64>(size_);
	} else {
		return E_INVALIDARG;
	}
	if ((offset > 0 && base > std::numeric_limits<Int64>::max() - offset) ||
		(offset < 0 && base < std::numeric_limits<Int64>::min() - offset)) return E_FAIL;
	const Int64 target = base + offset;
	if (target < 0 || static_cast<std::uint64_t>(target) >
		static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) return E_FAIL;
	position_ = static_cast<std::uint64_t>(target);
	if (newPosition != nullptr) *newPosition = position_;
	return S_OK;
}

Z7_COM7F_IMF(ArchiveInputStream::GetSize(UInt64* size)) {
	if (size == nullptr) return kPointerError;
	*size = size_;
	return S_OK;
}

class ArchiveOpenCallback final : public IArchiveOpenCallback,
	public ICryptoGetTextPassword, public CMyUnknownImp {
	Z7_IFACES_IMP_UNK_2(IArchiveOpenCallback, ICryptoGetTextPassword)
	public:

	std::optional<std::string> password_;
	const std::function<bool()>* shouldContinue_;

public:
	bool passwordRequested = false;

	ArchiveOpenCallback(const std::optional<std::string>& password,
		const std::function<bool()>* shouldContinue)
		: password_(password), shouldContinue_(shouldContinue) {}

	~ArchiveOpenCallback() {
		if (password_) Wipe(*password_);
	}
};

Z7_COM7F_IMF(ArchiveOpenCallback::SetTotal(const UInt64*, const UInt64*)) {
	return shouldContinue_ == nullptr || ContinueWork(*shouldContinue_) ? S_OK : E_ABORT;
}

Z7_COM7F_IMF(ArchiveOpenCallback::SetCompleted(const UInt64*, const UInt64*)) {
	return shouldContinue_ == nullptr || ContinueWork(*shouldContinue_) ? S_OK : E_ABORT;
}

Z7_COM7F_IMF(ArchiveOpenCallback::CryptoGetTextPassword(BSTR* password)) {
	if (password == nullptr) return kPointerError;
	*password = nullptr;
	passwordRequested = true;
	if (!password_) return E_ABORT;
	return PasswordToBstr(*password_, password);
}

struct ExtractState {
	std::uint32_t targetIndex = 0;
	const SevenZipDataWriter* writer = nullptr;
	const std::function<bool()>* shouldContinue = nullptr;
	bool sinkFailed = false;
	std::string sinkError;
	Int32 operationResult = -1;
};

class ArchiveOutputStream final : public ISequentialOutStream, public CMyUnknownImp {
	Z7_IFACES_IMP_UNK_1(ISequentialOutStream)
	public:

	ExtractState* state_;
	std::mutex mutex_;

public:
	explicit ArchiveOutputStream(ExtractState* state) : state_(state) {}
};

Z7_COM7F_IMF(ArchiveOutputStream::Write(const void* data, UInt32 size,
	UInt32* processedSize)) {
	if (processedSize == nullptr) return kPointerError;
	*processedSize = 0;
	if (size == 0) return S_OK;
	if (state_ == nullptr || state_->writer == nullptr) return E_FAIL;
	if (state_->shouldContinue != nullptr &&
		!ContinueWork(*state_->shouldContinue)) return E_ABORT;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!(*state_->writer)(static_cast<const std::uint8_t*>(data), size,
		state_->sinkError)) {
		state_->sinkFailed = true;
		return E_ABORT;
	}
	*processedSize = size;
	return S_OK;
}

class ArchiveExtractCallback final : public IArchiveExtractCallback,
	public ICryptoGetTextPassword, public CMyUnknownImp {
	Z7_IFACES_IMP_UNK_2(IArchiveExtractCallback, ICryptoGetTextPassword)
	Z7_IFACE_COM7_IMP(IProgress)
	public:

	std::optional<std::string> password_;
	const std::function<bool()>* shouldContinue_;
	ExtractState* state_;
	CMyComPtr<ISequentialOutStream> output_;

public:
	bool passwordRequested = false;

	ArchiveExtractCallback(std::uint32_t targetIndex,
		const std::optional<std::string>& password,
		const std::function<bool()>* shouldContinue, ExtractState* state)
		: password_(password), shouldContinue_(shouldContinue), state_(state) {
		state_->targetIndex = targetIndex;
	}

	~ArchiveExtractCallback() {
		if (password_) Wipe(*password_);
	}
};

Z7_COM7F_IMF(ArchiveExtractCallback::SetTotal(UInt64)) {
	return shouldContinue_ == nullptr || ContinueWork(*shouldContinue_) ? S_OK : E_ABORT;
}

Z7_COM7F_IMF(ArchiveExtractCallback::SetCompleted(const UInt64*)) {
	return shouldContinue_ == nullptr || ContinueWork(*shouldContinue_) ? S_OK : E_ABORT;
}

Z7_COM7F_IMF(ArchiveExtractCallback::GetStream(UInt32 index,
	ISequentialOutStream** outStream, Int32 askExtractMode)) {
	if (outStream == nullptr) return kPointerError;
	*outStream = nullptr;
	if (shouldContinue_ != nullptr && !ContinueWork(*shouldContinue_)) return E_ABORT;
	if (index != state_->targetIndex || askExtractMode != NArchive::NExtract::NAskMode::kExtract) {
		return S_OK;
	}
	output_ = new ArchiveOutputStream(state_);
	*outStream = output_;
	(*outStream)->AddRef();
	return S_OK;
}

Z7_COM7F_IMF(ArchiveExtractCallback::PrepareOperation(Int32)) {
	return shouldContinue_ == nullptr || ContinueWork(*shouldContinue_) ? S_OK : E_ABORT;
}

Z7_COM7F_IMF(ArchiveExtractCallback::SetOperationResult(Int32 result)) {
	state_->operationResult = result;
	return shouldContinue_ == nullptr || ContinueWork(*shouldContinue_) ? S_OK : E_ABORT;
}

Z7_COM7F_IMF(ArchiveExtractCallback::CryptoGetTextPassword(BSTR* password)) {
	if (password == nullptr) return kPointerError;
	*password = nullptr;
	passwordRequested = true;
	if (!password_) return E_ABORT;
	return PasswordToBstr(*password_, password);
}

struct ArchiveOperation {
	CMyComPtr<IInArchive> handler;
	CMyComPtr<IInStream> input;
	CMyComPtr<IArchiveOpenCallback> openCallback;
	ArchiveOpenCallback* openCallbackObject = nullptr;
	bool opened = false;

	~ArchiveOperation() {
		if (opened && handler) (void)handler->Close();
	}
};

bool CreateAndOpenArchive(const std::filesystem::path& path,
	const std::optional<std::string>& password, const std::function<bool()>& shouldContinue,
	ArchiveOperation& operation, std::string& errorMessage, ArchiveErrorKind* errorKind) {
	const PluginHandle& plugin = Plugin();
	if (plugin.createObject == nullptr) {
		SetError(errorMessage, errorKind,
			"the private 7-Zip 24.09 reader is unavailable in this build",
			ArchiveErrorKind::UnsupportedEncryption);
		return false;
	}
	if (!ContinueWork(shouldContinue)) {
		SetError(errorMessage, errorKind, "7z archive operation was cancelled");
		return false;
	}
	int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		SetError(errorMessage, errorKind, "cannot open 7z archive input");
		return false;
	}
	struct stat status{};
	if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
		(void)::close(descriptor);
		SetError(errorMessage, errorKind, "cannot read 7z archive input metadata");
		return false;
	}
	operation.input = new ArchiveInputStream(descriptor,
		static_cast<std::uint64_t>(status.st_size), &shouldContinue);
	operation.openCallbackObject = new ArchiveOpenCallback(password, &shouldContinue);
	operation.openCallback = operation.openCallbackObject;
	if (plugin.createObject(&CLSID_SevenZipFormat, &IID_IInArchive,
		(reinterpret_cast<void**>(&operation.handler))) != S_OK || !operation.handler) {
		SetError(errorMessage, errorKind, "cannot create a 7-Zip archive handler");
		return false;
	}
	const UInt64 maximumStartPosition = 0;
	const HRESULT openResult = operation.handler->Open(operation.input.Interface(),
		&maximumStartPosition, operation.openCallback.Interface());
	if (openResult != S_OK) {
		if (!ContinueWork(shouldContinue)) {
			SetError(errorMessage, errorKind, "7z archive operation was cancelled");
		} else if (operation.openCallbackObject->passwordRequested) {
			SetError(errorMessage, errorKind, password ? "incorrect archive password" :
				"password required for encrypted 7z archive",
				password ? ArchiveErrorKind::InvalidPassword : ArchiveErrorKind::PasswordRequired);
		} else {
			SetError(errorMessage, errorKind, "cannot open 7z archive");
		}
		return false;
	}
	operation.opened = true;
	return true;
}

struct PropertyValue {
	PROPVARIANT value{};

	PropertyValue() { value.vt = VT_EMPTY; }
	~PropertyValue() {
		if (value.vt == VT_BSTR) FreeBstr(value.bstrVal);
	}
};

bool ReadProperty(IInArchive* handler, UInt32 index, PROPID propertyId,
	PropertyValue& value) {
	return handler != nullptr && handler->GetProperty(index, propertyId, &value.value) == S_OK;
}

bool ReadEntryPath(IInArchive* handler, UInt32 index, std::string& path) {
	PropertyValue property;
	if (!ReadProperty(handler, index, kpidPath, property)) return false;
	if (property.value.vt == VT_EMPTY) {
		path.clear();
		return true;
	}
	if (property.value.vt != VT_BSTR || property.value.bstrVal == nullptr) return false;
	return BstrToUtf8(property.value.bstrVal, path);
}

bool ReadBooleanProperty(IInArchive* handler, UInt32 index, PROPID propertyId,
	bool& result) {
	PropertyValue property;
	if (!ReadProperty(handler, index, propertyId, property)) return false;
	if (property.value.vt == VT_EMPTY) {
		result = false;
		return true;
	}
	if (property.value.vt != VT_BOOL) return false;
	result = property.value.boolVal != VARIANT_FALSE;
	return true;
}

bool ReadPropertyPresent(IInArchive* handler, UInt32 index, PROPID propertyId,
	bool& result) {
	PropertyValue property;
	if (!ReadProperty(handler, index, propertyId, property)) return false;
	result = false;
	if (property.value.vt == VT_BOOL) result = property.value.boolVal != VARIANT_FALSE;
	else if (property.value.vt == VT_BSTR) result = property.value.bstrVal != nullptr &&
		BstrByteLength(property.value.bstrVal) != 0;
	else if (property.value.vt != VT_EMPTY) result = true;
	return true;
}

bool ReadSizeProperty(IInArchive* handler, UInt32 index, std::uint64_t& result,
	bool& defined) {
	PropertyValue property;
	if (!ReadProperty(handler, index, kpidSize, property)) return false;
	if (property.value.vt == VT_EMPTY) {
		result = 0;
		defined = false;
		return true;
	}
	if (property.value.vt == VT_UI8) result = property.value.uhVal.QuadPart;
	else if (property.value.vt == VT_UI4) result = property.value.ulVal;
	else return false;
	defined = true;
	return true;
}

bool ReadModificationTime(IInArchive* handler, UInt32 index, std::int64_t& result) {
	PropertyValue property;
	if (!ReadProperty(handler, index, kpidMTime, property)) return false;
	result = 0;
	if (property.value.vt == VT_EMPTY) return true;
	if (property.value.vt != VT_FILETIME) return false;
	const std::uint64_t ticks = static_cast<std::uint64_t>(property.value.filetime.dwLowDateTime) |
		(static_cast<std::uint64_t>(property.value.filetime.dwHighDateTime) << 32);
	if (ticks >= kWindowsToUnixEpochTicks) {
		result = static_cast<std::int64_t>((ticks - kWindowsToUnixEpochTicks) / kTicksPerSecond);
	} else {
		const std::uint64_t seconds =
			(kWindowsToUnixEpochTicks - ticks + kTicksPerSecond - 1) / kTicksPerSecond;
		result = -static_cast<std::int64_t>(seconds);
	}
	return true;
}

ArchiveErrorKind ExtractFailureKind(const ArchiveExtractCallback& callback,
	const std::optional<std::string>& password) {
	if (callback.passwordRequested && !password) return ArchiveErrorKind::PasswordRequired;
	if (callback.passwordRequested && password) return ArchiveErrorKind::InvalidPassword;
	return ArchiveErrorKind::Other;
}

} // namespace

bool SevenZipBackendAvailable() {
	return Plugin().createObject != nullptr;
}

bool ReadSevenZipCatalog(const std::filesystem::path& archive,
	const std::optional<std::string>& password, const std::function<bool()>& shouldContinue,
	std::vector<SevenZipEntry>& entries, bool& headerEncrypted, std::string& errorMessage,
	ArchiveErrorKind* errorKind) {
	entries.clear();
	headerEncrypted = false;
	errorMessage.clear();
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	ArchiveOperation operation;
	if (!CreateAndOpenArchive(archive, password, shouldContinue, operation,
		errorMessage, errorKind)) {
	headerEncrypted = operation.openCallbackObject != nullptr &&
		operation.openCallbackObject->passwordRequested;
		return false;
	}
	headerEncrypted = operation.openCallbackObject->passwordRequested;
	UInt32 itemCount = 0;
	if (operation.handler->GetNumberOfItems(&itemCount) != S_OK) {
		SetError(errorMessage, errorKind, "cannot read 7z archive item count");
		return false;
	}
	if (itemCount > kMaximumSevenZipCatalogEntries) {
		SetError(errorMessage, errorKind, "archive has more than 100000 entries");
		return false;
	}
	try {
		entries.reserve(itemCount);
		for (UInt32 index = 0; index < itemCount; ++index) {
			if (!ContinueWork(shouldContinue)) {
				SetError(errorMessage, errorKind, "7z archive indexing was cancelled");
				entries.clear();
				return false;
			}
			SevenZipEntry item;
			item.index = index;
			bool hasSize = false;
			bool symbolicLink = false;
			bool hardLink = false;
			bool alternateStream = false;
			bool antiItem = false;
			if (!ReadEntryPath(operation.handler.Interface(), index, item.rawPath) ||
				!ReadBooleanProperty(operation.handler.Interface(), index, kpidIsDir,
					item.directory) ||
				!ReadSizeProperty(operation.handler.Interface(), index, item.size, hasSize) ||
				!ReadModificationTime(operation.handler.Interface(), index,
					item.modificationTime) ||
				!ReadBooleanProperty(operation.handler.Interface(), index, kpidEncrypted,
					item.encrypted) ||
				!ReadPropertyPresent(operation.handler.Interface(), index, kpidSymLink,
					symbolicLink) ||
				!ReadPropertyPresent(operation.handler.Interface(), index, kpidHardLink,
					hardLink) ||
				!ReadBooleanProperty(operation.handler.Interface(), index, kpidIsAltStream,
					alternateStream) ||
				!ReadBooleanProperty(operation.handler.Interface(), index, kpidIsAnti,
					antiItem)) {
				SetError(errorMessage, errorKind, "cannot read 7z archive item metadata");
				entries.clear();
				return false;
			}
			item.specialFile = symbolicLink || hardLink || alternateStream || antiItem;
			if (!item.directory && !hasSize) continue;
			entries.push_back(std::move(item));
		}
	} catch (const std::bad_alloc&) {
		SetError(errorMessage, errorKind, "not enough memory to index 7z archive");
		entries.clear();
		return false;
	}
	if (!ContinueWork(shouldContinue)) {
		SetError(errorMessage, errorKind, "7z archive indexing was cancelled");
		entries.clear();
		return false;
	}
	return true;
}

bool ExtractSevenZipMember(const std::filesystem::path& archive, std::uint64_t index,
	const std::string& expectedRawPath, const std::optional<std::string>& password,
	const std::function<bool()>& shouldContinue, const SevenZipDataWriter& writer,
	std::string& errorMessage, ArchiveErrorKind* errorKind) {
	errorMessage.clear();
	if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
	if (!writer) {
		SetError(errorMessage, errorKind, "7z member output callback is missing");
		return false;
	}
	if (index > std::numeric_limits<UInt32>::max()) {
		SetError(errorMessage, errorKind, "7z member index is out of range");
		return false;
	}
	ArchiveOperation operation;
	if (!CreateAndOpenArchive(archive, password, shouldContinue, operation,
		errorMessage, errorKind)) return false;
	UInt32 itemCount = 0;
	if (operation.handler->GetNumberOfItems(&itemCount) != S_OK || index >= itemCount) {
		SetError(errorMessage, errorKind, "7z member no longer exists");
		return false;
	}
	std::string currentRawPath;
	bool directory = false;
	if (!ReadEntryPath(operation.handler.Interface(), static_cast<UInt32>(index), currentRawPath) ||
		!ReadBooleanProperty(operation.handler.Interface(), static_cast<UInt32>(index),
			kpidIsDir, directory) || directory || currentRawPath != expectedRawPath) {
		SetError(errorMessage, errorKind, "7z member changed since it was listed");
		return false;
	}
	ExtractState state;
	state.writer = &writer;
	state.shouldContinue = &shouldContinue;
	ArchiveExtractCallback* callbackObject = new ArchiveExtractCallback(
		static_cast<std::uint32_t>(index), password, &shouldContinue, &state);
	CMyComPtr<IArchiveExtractCallback> callback;
	callback = callbackObject;
	const UInt32 targetIndex = static_cast<UInt32>(index);
	const HRESULT extractResult = operation.handler->Extract(&targetIndex, 1, 0,
		callback.Interface());
	if (!ContinueWork(shouldContinue)) {
		SetError(errorMessage, errorKind, "7z member extraction was cancelled");
		return false;
	}
	if (state.sinkFailed) {
		errorMessage = state.sinkError.empty() ? "cannot write extracted 7z member" : state.sinkError;
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::Other;
		return false;
	}
	if (callbackObject->passwordRequested && !password) {
		SetError(errorMessage, errorKind, "password required for encrypted 7z image",
			ArchiveErrorKind::PasswordRequired);
		return false;
	}
	if (extractResult != S_OK || state.operationResult != NArchive::NExtract::NOperationResult::kOK) {
		const ArchiveErrorKind kind = ExtractFailureKind(*callbackObject, password);
		SetError(errorMessage, errorKind,
			kind == ArchiveErrorKind::InvalidPassword ? "incorrect archive password" :
			kind == ArchiveErrorKind::PasswordRequired ? "password required for encrypted 7z image" :
			"cannot extract 7z image member", kind);
		return false;
	}
	return true;
}

} // namespace jpegview_linux
