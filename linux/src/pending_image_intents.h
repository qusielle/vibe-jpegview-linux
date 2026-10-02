#pragma once

#include "archive_source.h"
#include "exif_metadata_worker.h"
#include "viewport.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace jpegview_linux {

inline constexpr std::size_t kMaximumPendingImageIntents = 256;

enum class PendingImageIntentType {
	Viewport,
	Transform,
};

struct PendingImageIntent {
	PendingImageIntentType type = PendingImageIntentType::Viewport;
	ViewportIntent viewport;
	int transform = 0;
};

struct PendingImageIntentBatch {
	std::filesystem::path filename;
	SourceKey source;
	std::uint64_t loadGeneration = 0;
	std::vector<PendingImageIntent> actions;
	bool startTransition = false;
};

// Holds ordered user actions for a cold current-image header request. Actions
// are released only to the matching filename, source, and load generation.
class PendingImageIntents {
public:
	void Begin(const std::filesystem::path& filename, const SourceKey& source,
		std::uint64_t loadGeneration);
	bool CanQueue(const std::filesystem::path& filename, const SourceKey& source,
		std::uint64_t loadGeneration) const;
	bool MatchesSelection(const std::filesystem::path& filename,
		const SourceKey& source, std::uint64_t loadGeneration) const;
	bool MatchesStartupLoad(bool startupScan, bool headerPending,
		bool dimensionsRequestPending, bool startupLoad,
		const std::filesystem::path& filename, const SourceKey& source,
		std::uint64_t loadGeneration) const;
	bool QueueViewport(const std::filesystem::path& filename, const SourceKey& source,
		std::uint64_t loadGeneration, const ViewportIntent& intent);
	bool QueueTransform(const std::filesystem::path& filename, const SourceKey& source,
		std::uint64_t loadGeneration, int command);
	bool RequestTransition(const SourceKey& source, std::uint64_t loadGeneration);
	std::optional<PendingImageIntentBatch> Take(const SourceKey& source,
		std::uint64_t loadGeneration);
	std::size_t ActionCount() const;
	void Cancel();

private:
	std::optional<PendingImageIntentBatch> pending_;
};

struct ExifDateActionCompletion {
	bool matchedPendingRead = false;
	bool runDeferredAction = false;
};

// Separates "metadata has not arrived" from "the image has no usable EXIF
// date" and validates a deferred action against its source generation.
class DeferredExifDateAction {
public:
	void Begin(const std::filesystem::path& filename, const SourceKey& source,
		std::uint64_t metadataGeneration);
	bool MustDeferFor(const std::filesystem::path& filename,
		const SourceKey& source) const;
	bool Defer(const std::filesystem::path& filename, const SourceKey& source);
	ExifDateActionCompletion Complete(const ExifMetadataResult& result,
		const std::filesystem::path& selectedFilename, const SourceKey& selectedSource);
	ExifDateActionCompletion MarkImageCommitted(
		const std::filesystem::path& filename, const SourceKey& source);
	void Cancel();

private:
	std::filesystem::path filename_;
	SourceKey source_;
	std::uint64_t metadataGeneration_ = 0;
	bool metadataPending_ = false;
	bool imageCommitted_ = false;
	bool actionDeferred_ = false;
};

} // namespace jpegview_linux
