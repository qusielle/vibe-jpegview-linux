#pragma once

#include "archive_source.h"
#include "image_decoder.h"
#include "image_processing.h"
#include "recent_files.h"
#include "work_context.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

namespace jpegview_linux {

struct ImageSessionSelection {
	std::uint64_t generation = 0;
	std::filesystem::path filename;
	SourceDescriptor source;
	ViewportSnapshot viewport;
	ImageProcessingPreset processing;
	bool tracksRecentHistory = false;
};

enum class ImageSessionStage {
	Selected,
	AwaitingSourceDimensions,
	AwaitingDecodedSource,
	AwaitingDisplayFrame,
	DisplayFrameReady,
	Committed,
	Failed,
};

struct SelectedSourceDecodeResult {
	std::uint64_t generation = 0;
	SourceDescriptor source;
	std::shared_ptr<const DecodedImage> image;
	WorkerFailure failure;
};

// A one-result mailbox for the currently selected decode. Activation replaces
// the owner generation; stale A->B->A completions cannot overwrite the newest A.
class SelectedSourceDecodeChannel {
public:
	void Activate(std::uint64_t generation, const SourceKey& source);
	bool Publish(SelectedSourceDecodeResult result);
	std::optional<SelectedSourceDecodeResult> Take(
		std::uint64_t generation, const SourceKey& source);
	void Shutdown();

private:
	std::mutex mutex_;
	std::optional<SelectedSourceDecodeResult> ready_;
	std::uint64_t activeGeneration_ = 0;
	SourceKey activeSource_;
	bool active_ = true;
};

struct ImageSessionEffects {
	// The SDL adapter applies these effects in its existing order. The controller
	// records decisions only; it never owns a texture, window, or renderer.
	bool clearPreviousPresentation = true;
	bool restoreViewport = true;
	bool requestSelectedSourcePreparation = true;
};

struct ImageSessionStart {
	ImageSessionSelection selection;
	ImageSessionEffects effects;
};

struct SelectedSourcePreparationSnapshot {
	bool preparationRequested = true;
	bool sourceSupportsDimensionsProbe = false;
	bool displayCacheEnabled = false;
	bool cachedSourceDimensions = false;
	bool sourceValid = false;
	bool sourceDimensionProbeFailed = false;
};

enum class SelectedSourcePreparationAction {
	Skip,
	UseCachedSourceDimensions,
	RequestSourceDimensions,
	DecodeSelectedSource,
};

struct DecodedSourcePreparationSnapshot {
	bool preparationRequested = true;
	bool cachedDisplayReady = false;
	bool deferredForPossibleSpread = false;
	bool waitingForSourceDimensions = false;
};

// Owns the value state that identifies one selected image independently from
// the last successful Recents owner. A selection ticket binds asynchronous
// work to a source and generation; the Viewer remains the SDL effect adapter.
class ImageSessionController {
public:
	ImageSessionStart BeginSelection(const SourceDescriptor& source,
		const std::filesystem::path& filename, const ViewportSnapshot& viewport,
		const ImageProcessingPreset& processing, bool trackRecentHistory);
	SelectedSourcePreparationAction PlanSelectedSourcePreparation(
		const SelectedSourcePreparationSnapshot& snapshot) const;
	bool ShouldPrepareDecodedSource(
		const DecodedSourcePreparationSnapshot& snapshot) const;
	bool MatchesSelection(std::uint64_t generation, const SourceKey& source) const;
	ImageSessionStage Stage() const { return stage_; }
	bool SetStage(std::uint64_t generation, const SourceKey& source,
		ImageSessionStage stage);
	bool MarkDisplayFrameReady(std::uint64_t generation, const SourceKey& source);
	bool CommitSelectedLoad(std::uint64_t generation, const SourceKey& source,
		const std::filesystem::path& filename, RecentFiles& recentFiles);
	bool FailSelectedLoad(std::uint64_t generation, const SourceKey& source,
		const std::filesystem::path& filename);
	const std::optional<ImageSessionSelection>& Selection() const { return selection_; }
	std::uint64_t Generation() const { return generation_; }
	std::uint64_t DocumentRevision() const { return documentRevision_; }
	std::uint64_t MarkDocumentChanged();
	void UpdateProcessing(const ImageProcessingPreset& processing);
	const ImageProcessingPreset& Processing() const { return processing_; }

	ViewportSnapshot ResolveViewportForSelection(
		const std::filesystem::path& filename, bool clipboardMode,
		const ViewportSnapshot& currentViewport,
		const ViewportSnapshot& navigationViewport,
		const RecentFiles& recentFiles, bool restoreRecentViewport = false) const;
	void SetClipboardReturnViewport(const ViewportSnapshot& viewport);
	void ClearClipboardReturnViewport() { clipboardReturnViewport_.reset(); }
	const std::optional<ViewportSnapshot>& ClipboardReturnViewport() const {
		return clipboardReturnViewport_;
	}

	// Keep the existing RecentImageLoadState API available through the adapter
	// while the async foreground loader is introduced in a later step.
	const std::filesystem::path& LoadedPath() const { return history_.LoadedPath(); }
	bool OwnsLoadedPath(const std::filesystem::path& filename) const {
		return history_.OwnsLoadedPath(filename);
	}
	void SaveCurrentBeforeLoad(const std::filesystem::path& target,
		const ViewportSnapshot& currentViewport, const DoublePageModeState& currentModes,
		RecentFiles& recentFiles) {
		history_.SaveCurrentBeforeLoad(target, currentViewport, currentModes, recentFiles);
	}
	void BeginLoad(const std::filesystem::path& filename,
		const ViewportSnapshot& viewport) { history_.BeginLoad(filename, viewport); }
	bool UpdatePendingViewport(const std::filesystem::path& filename,
		const ViewportSnapshot& viewport) {
		return history_.UpdatePendingViewport(filename, viewport);
	}
	std::optional<PendingRecentImageLoad> TakePendingLoad(
		const std::filesystem::path& filename) {
		return history_.TakePendingLoad(filename);
	}
	bool CommitLoad(const std::filesystem::path& filename, RecentFiles& recentFiles);
	bool FailLoad(const std::filesystem::path& filename);
	void CancelPendingLoad() { history_.CancelPendingLoad(); }

private:
	RecentImageLoadState history_;
	std::optional<ImageSessionSelection> selection_;
	std::optional<ViewportSnapshot> clipboardReturnViewport_;
	ImageProcessingPreset processing_;
	std::uint64_t generation_ = 0;
	std::uint64_t documentRevision_ = 0;
	ImageSessionStage stage_ = ImageSessionStage::Selected;
};

} // namespace jpegview_linux
