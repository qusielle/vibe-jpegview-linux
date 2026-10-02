#pragma once

#include "archive_source.h"
#include "exif_reader.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jpegview_linux {

struct ExifMetadataResult {
	std::uint64_t generation = 0;
	SourceKey source;
	ExifInfo metadata;
	std::string jpegComment;
	bool metadataAvailable = false;
};

// Loads optional JPEG metadata away from the event thread. Requests replace
// older work, and only the latest source generation is published.
class ExifMetadataWorker {
public:
	using Reader = std::function<bool(const std::filesystem::path&, ExifInfo&,
		std::string&)>;
	using SourceValidator = std::function<bool(const SourceDescriptor&)>;

	explicit ExifMetadataWorker(Reader reader = {}, SourceValidator sourceValidator = {});
	~ExifMetadataWorker();
	ExifMetadataWorker(const ExifMetadataWorker&) = delete;
	ExifMetadataWorker& operator=(const ExifMetadataWorker&) = delete;

	std::uint64_t Request(SourceDescriptor source);
	void Cancel();
	void Stop();
	std::vector<ExifMetadataResult> TakeReady();
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

bool IsCurrentExifMetadataResult(const ExifMetadataResult& result,
	std::uint64_t expectedGeneration, const SourceKey& expectedSource);

} // namespace jpegview_linux
