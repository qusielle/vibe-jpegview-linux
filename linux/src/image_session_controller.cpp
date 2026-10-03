#include "image_session_controller.h"

#include <limits>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

fs::path NormalizeAbsolutePath(const fs::path& path) {
	if (path.empty()) return {};
	std::error_code error;
	const fs::path absolute = fs::absolute(path, error);
	return error ? path.lexically_normal() : absolute.lexically_normal();
}

} // namespace

ImageSessionStart ImageSessionController::BeginSelection(
	const SourceDescriptor& source, const fs::path& filename,
	const ViewportSnapshot& viewport, const ImageProcessingPreset& processing,
	bool trackRecentHistory) {
	if (generation_ == std::numeric_limits<std::uint64_t>::max()) generation_ = 1;
	else ++generation_;
	documentRevision_ = 0;
	processing_ = processing;
	const fs::path selectedPath = NormalizeAbsolutePath(filename);
	selection_ = ImageSessionSelection{generation_, selectedPath, source,
		viewport, processing, trackRecentHistory};
	if (trackRecentHistory) history_.BeginLoad(selectedPath, viewport);
	return {*selection_, {true, true, true}};
}

SelectedSourcePreparationAction ImageSessionController::PlanSelectedSourcePreparation(
	const SelectedSourcePreparationSnapshot& snapshot) const {
	if (!snapshot.preparationRequested) return SelectedSourcePreparationAction::Skip;
	if (!snapshot.jpegSource || !snapshot.displayCacheEnabled) {
		return SelectedSourcePreparationAction::DecodeSelectedSource;
	}
	if (snapshot.cachedJpegDimensions) {
		return SelectedSourcePreparationAction::UseCachedJpegDimensions;
	}
	if (snapshot.sourceValid && !snapshot.jpegDimensionProbeFailed) {
		return SelectedSourcePreparationAction::RequestJpegDimensions;
	}
	return SelectedSourcePreparationAction::DecodeSelectedSource;
}

bool ImageSessionController::ShouldPrepareDecodedSource(
	const DecodedSourcePreparationSnapshot& snapshot) const {
	return snapshot.preparationRequested && !snapshot.cachedDisplayReady &&
		!snapshot.deferredForPossibleSpread && !snapshot.waitingForJpegDimensions;
}

bool ImageSessionController::MatchesSelection(std::uint64_t generation,
	const SourceKey& source) const {
	return selection_.has_value() && selection_->generation == generation &&
		selection_->source.Key() == source;
}

std::uint64_t ImageSessionController::MarkDocumentChanged() {
	if (documentRevision_ != std::numeric_limits<std::uint64_t>::max()) ++documentRevision_;
	return documentRevision_;
}

void ImageSessionController::UpdateProcessing(const ImageProcessingPreset& processing) {
	processing_ = processing;
	if (selection_.has_value()) selection_->processing = processing;
}

ViewportSnapshot ImageSessionController::ResolveViewportForSelection(
	const fs::path& filename, bool clipboardMode,
	const ViewportSnapshot& currentViewport,
	const ViewportSnapshot& navigationViewport, const RecentFiles& recentFiles) const {
	if (clipboardMode) return navigationViewport;
	const fs::path selectedPath = NormalizeAbsolutePath(filename);
	if (clipboardReturnViewport_.has_value() && !LoadedPath().empty() &&
		selectedPath == LoadedPath()) {
		return *clipboardReturnViewport_;
	}
	return history_.ViewportForSelection(selectedPath, currentViewport,
		navigationViewport, recentFiles);
}

void ImageSessionController::SetClipboardReturnViewport(
	const ViewportSnapshot& viewport) {
	clipboardReturnViewport_ = viewport;
}

bool ImageSessionController::CommitLoad(const fs::path& filename,
	RecentFiles& recentFiles) {
	return history_.CommitLoad(filename, recentFiles);
}

bool ImageSessionController::FailLoad(const fs::path& filename) {
	return history_.FailLoad(filename);
}

} // namespace jpegview_linux
