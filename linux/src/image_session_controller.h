#pragma once

#include "archive_source.h"
#include "image_processing.h"
#include "recent_files.h"

#include <cstdint>
#include <filesystem>
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
	bool jpegSource = false;
	bool displayCacheEnabled = false;
	bool cachedJpegDimensions = false;
	bool sourceValid = false;
	bool jpegDimensionProbeFailed = false;
};

enum class SelectedSourcePreparationAction {
	Skip,
	UseCachedJpegDimensions,
	RequestJpegDimensions,
	DecodeSelectedSource,
};

struct DecodedSourcePreparationSnapshot {
	bool preparationRequested = true;
	bool cachedDisplayReady = false;
	bool deferredForPossibleSpread = false;
	bool waitingForJpegDimensions = false;
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
		const RecentFiles& recentFiles) const;
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
};

} // namespace jpegview_linux
