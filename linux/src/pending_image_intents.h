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
	CopySelection,
	CopyImage,
	CropSelection,
};

struct PendingImageIntent {
	PendingImageIntentType type = PendingImageIntentType::Viewport;
	ViewportIntent viewport;
	int transform = 0;
	int selectionLeft = 0;
	int selectionTop = 0;
	int selectionRight = 0;
	int selectionBottom = 0;
	bool fullSize = false;
};

struct PendingImageIntentBatch {
	std::filesystem::path filename;
	SourceKey source;
	std::uint64_t loadGeneration = 0;
	std::vector<PendingImageIntent> actions;
	bool startTransition = false;
};

enum class PendingImageOperationAdmission {
	Rejected,
	WaitForSelectedCommit,
	StartNow,
};

// A direct operation may follow previously accepted intents only when it is
// itself the front intent being replayed. The selected image's display commit
// is the point where an operation deferred for a cold load becomes startable.
PendingImageOperationAdmission PlanPendingImageOperationAdmission(
	bool operationAlreadyPending, std::size_t acceptedEarlierIntentCount,
	bool selectedLoadPending, bool replayingFrontIntent);

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
	bool MatchesStartupLoad(bool startupScan, bool selectedLoadPending,
		bool startupLoad,
		const std::filesystem::path& filename, const SourceKey& source,
		std::uint64_t loadGeneration) const;
	bool QueueViewport(const std::filesystem::path& filename, const SourceKey& source,
		std::uint64_t loadGeneration, const ViewportIntent& intent);
	bool QueueTransform(const std::filesystem::path& filename, const SourceKey& source,
		std::uint64_t loadGeneration, int command);
	bool QueueCopySelection(const std::filesystem::path& filename,
		const SourceKey& source, std::uint64_t loadGeneration,
		int left, int top, int right, int bottom);
	bool QueueCopyImage(const std::filesystem::path& filename,
		const SourceKey& source, std::uint64_t loadGeneration, bool fullSize);
	bool QueueCropSelection(const std::filesystem::path& filename,
		const SourceKey& source, std::uint64_t loadGeneration,
		int left, int top, int right, int bottom);
	bool RequestTransition(const SourceKey& source, std::uint64_t loadGeneration);
	// Drains actions already captured for this selection while keeping the
	// generation owner active for input received during the next async stage.
	std::optional<PendingImageIntentBatch> Drain(
		const SourceKey& source, std::uint64_t loadGeneration);
	std::optional<PendingImageIntentBatch> Take(const SourceKey& source,
		std::uint64_t loadGeneration);
	std::size_t ActionCount() const;
	const std::vector<PendingImageIntent>* Actions() const {
		return pending_.has_value() ? &pending_->actions : nullptr;
	}
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
