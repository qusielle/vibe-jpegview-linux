#pragma once

#include <filesystem>
#include <memory>
#include <ostream>
#include <string>

namespace jpegview_linux {

// Writes beside the destination and publishes only after the full stream is
// flushed and closed successfully. An uncommitted temporary file is removed.
class AtomicFileWriter {
public:
	explicit AtomicFileWriter(const std::filesystem::path& destination);
	~AtomicFileWriter();
	AtomicFileWriter(const AtomicFileWriter&) = delete;
	AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

	explicit operator bool() const;
	std::ostream& Stream();
	// When replacement is disabled, publication atomically fails if the target exists.
	bool Commit(bool replaceExisting = true);
	const std::string& ErrorMessage() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

bool WriteFileAtomically(const std::filesystem::path& destination,
	const std::string& contents, std::string& errorMessage);

} // namespace jpegview_linux
