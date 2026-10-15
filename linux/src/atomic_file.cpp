#include "atomic_file.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <limits>
#include <streambuf>
#include <system_error>
#include <sys/syscall.h>
#include <unistd.h>

namespace jpegview_linux {
namespace {

class FileDescriptorBuffer final : public std::streambuf {
public:
	explicit FileDescriptorBuffer(int& descriptor) : descriptor_(descriptor) {
		setp(buffer_, buffer_ + sizeof(buffer_));
	}

	int Error() const { return error_; }

protected:
	int_type overflow(int_type value) override {
		if (!Flush()) return traits_type::eof();
		if (!traits_type::eq_int_type(value, traits_type::eof())) {
			*pptr() = traits_type::to_char_type(value);
			pbump(1);
		}
		return traits_type::not_eof(value);
	}

	std::streamsize xsputn(const char* source, std::streamsize count) override {
		std::streamsize written = 0;
		while (written < count) {
			const std::streamsize available = epptr() - pptr();
			if (available == 0 && !Flush()) break;
			const std::streamsize amount = std::min<std::streamsize>(
				count - written, epptr() - pptr());
			std::memcpy(pptr(), source + written, static_cast<std::size_t>(amount));
			pbump(static_cast<int>(amount));
			written += amount;
		}
		return written;
	}

	int sync() override { return Flush() ? 0 : -1; }

private:
	bool Flush() {
		std::size_t remaining = static_cast<std::size_t>(pptr() - pbase());
		char* next = pbase();
		while (remaining != 0) {
			const ssize_t count = ::write(descriptor_, next, remaining);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) {
				error_ = count == 0 ? EIO : errno;
				return false;
			}
			const std::size_t written = static_cast<std::size_t>(count);
			next += written;
			remaining -= written;
		}
		setp(buffer_, buffer_ + sizeof(buffer_));
		return true;
	}

	int& descriptor_;
	int error_ = 0;
	char buffer_[16 * 1024]{};
};

std::atomic<unsigned long long> nextTemporaryId{0};

std::string ErrorText(const std::string& operation,
	const std::filesystem::path& path, int error) {
	return operation + " " + path.string() + ": " + std::strerror(error);
}

} // namespace

struct AtomicFileWriter::Impl {
	explicit Impl(std::filesystem::path outputPath)
		: destination(std::move(outputPath)), buffer(descriptor), stream(&buffer) {
		if (destination.empty()) {
			error = "empty destination path";
			return;
		}
		std::error_code filesystemError;
		if (!destination.parent_path().empty()) {
			std::filesystem::create_directories(destination.parent_path(), filesystemError);
			if (filesystemError) {
				error = "cannot create " + destination.parent_path().string() + ": " +
					filesystemError.message();
				return;
			}
		}
		static constexpr int kMaximumNameAttempts = 32;
		for (int attempt = 0; attempt < kMaximumNameAttempts; ++attempt) {
			temporary = destination;
			temporary += ".tmp." + std::to_string(static_cast<long long>(::getpid())) + "." +
				std::to_string(nextTemporaryId.fetch_add(1, std::memory_order_relaxed));
			descriptor = ::open(temporary.c_str(),
				O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
			if (descriptor >= 0) {
				ownsTemporary = true;
				return;
			}
			if (errno != EEXIST) {
				error = ErrorText("cannot create", temporary, errno);
				return;
			}
		}
		error = "cannot find a unique temporary path beside " + destination.string();
	}

	~Impl() {
		if (descriptor >= 0) (void)::close(descriptor);
		if (ownsTemporary && !temporary.empty()) (void)::unlink(temporary.c_str());
	}

	bool Commit(bool replaceExisting) {
		if (descriptor < 0) return false;
		stream.flush();
		if (!stream || buffer.Error() != 0) {
			const int failure = buffer.Error() == 0 ? EIO : buffer.Error();
			error = ErrorText("cannot write", temporary, failure);
			return false;
		}
		if (::fsync(descriptor) != 0) {
			error = ErrorText("cannot sync", temporary, errno);
			return false;
		}
		const int closing = descriptor;
		descriptor = -1;
		if (::close(closing) != 0) {
			error = ErrorText("cannot close", temporary, errno);
			return false;
		}
		if (replaceExisting) {
			if (::rename(temporary.c_str(), destination.c_str()) != 0) {
				error = ErrorText("cannot replace", destination, errno);
				return false;
			}
		} else {
			// Prefer renameat2: unlike the fallback, it also works where hard links
			// are disabled. Older kernels and filesystems use the same-dir link path.
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
			if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
				destination.c_str(), RENAME_NOREPLACE) == 0) {
				ownsTemporary = false;
				temporary.clear();
				return true;
			}
			const int renameError = errno;
			if (renameError == EEXIST) {
				error = ErrorText("cannot publish without replacing", destination, renameError);
				return false;
			}
			if (renameError != ENOSYS && renameError != EINVAL &&
				renameError != EOPNOTSUPP) {
				error = ErrorText("cannot publish without replacing", destination, renameError);
				return false;
			}
#endif
			// link() gives the fallback an atomic no-replace check.
			if (::link(temporary.c_str(), destination.c_str()) != 0) {
				error = ErrorText("cannot publish without replacing", destination, errno);
				return false;
			}
			if (::unlink(temporary.c_str()) == 0) {
				ownsTemporary = false;
				temporary.clear();
			} else {
				// The complete destination is already published; destructor retries temp cleanup.
				return true;
			}
		}
		ownsTemporary = false;
		temporary.clear();
		return true;
	}

	std::filesystem::path destination;
	std::filesystem::path temporary;
	int descriptor = -1;
	bool ownsTemporary = false;
	FileDescriptorBuffer buffer;
	std::ostream stream;
	std::string error;
};

AtomicFileWriter::AtomicFileWriter(const std::filesystem::path& destination)
	: impl_(std::make_unique<Impl>(destination)) {}

AtomicFileWriter::~AtomicFileWriter() = default;

AtomicFileWriter::operator bool() const {
	return impl_ && impl_->descriptor >= 0 && impl_->error.empty();
}

std::ostream& AtomicFileWriter::Stream() { return impl_->stream; }

bool AtomicFileWriter::Commit(bool replaceExisting) {
	return impl_ && impl_->Commit(replaceExisting);
}

const std::string& AtomicFileWriter::ErrorMessage() const {
	static const std::string kNoError;
	return impl_ ? impl_->error : kNoError;
}

bool WriteFileAtomically(const std::filesystem::path& destination,
	const std::string& contents, std::string& errorMessage) {
	AtomicFileWriter output(destination);
	if (!output) {
		errorMessage = output.ErrorMessage();
		return false;
	}
	output.Stream().write(contents.data(), static_cast<std::streamsize>(contents.size()));
	if (!output.Commit()) {
		errorMessage = output.ErrorMessage();
		return false;
	}
	errorMessage.clear();
	return true;
}

} // namespace jpegview_linux
