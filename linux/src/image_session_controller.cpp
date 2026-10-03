#include "image_session_controller.h"

#include "event_loop_model.h"
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

void SelectedSourceDecodeChannel::Activate(std::uint64_t generation,
	const SourceKey& source) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!active_) return;
	activeGeneration_ = generation;
	activeSource_ = source;
	ready_.reset();
}

bool SelectedSourceDecodeChannel::Publish(SelectedSourceDecodeResult result) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!active_ || result.generation != activeGeneration_ ||
			result.source.Key() != activeSource_) return false;
		ready_ = std::move(result);
	}
	UiCompletionWakeup().Notify();
	return true;
}

std::optional<SelectedSourceDecodeResult> SelectedSourceDecodeChannel::Take(
	std::uint64_t generation, const SourceKey& source) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!active_ || generation != activeGeneration_ || source != activeSource_ ||
		!ready_.has_value()) return std::nullopt;
	std::optional<SelectedSourceDecodeResult> result(std::move(ready_));
	ready_.reset();
	return result;
}

void SelectedSourceDecodeChannel::Shutdown() {
	std::lock_guard<std::mutex> lock(mutex_);
	active_ = false;
	activeGeneration_ = 0;
	activeSource_ = {};
	ready_.reset();
}

ImageSessionStart ImageSessionController::BeginSelection(
	const SourceDescriptor& source, const fs::path& filename,
	const ViewportSnapshot& viewport, const ImageProcessingPreset& processing,
	bool trackRecentHistory) {
	const bool sameSource = selection_.has_value() &&
		selection_->source.Key() == source.Key();
	if (generation_ == std::numeric_limits<std::uint64_t>::max()) generation_ = 1;
	else ++generation_;
	documentRevision_ = 0;
	processing_ = processing;
	const fs::path selectedPath = NormalizeAbsolutePath(filename);
	selection_ = ImageSessionSelection{generation_, selectedPath, source,
		viewport, processing, trackRecentHistory};
	stage_ = ImageSessionStage::Selected;
	if (trackRecentHistory) history_.BeginLoad(selectedPath, viewport);
	return {*selection_, {!sameSource, true, true}};
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

bool ImageSessionController::SetStage(std::uint64_t generation,
	const SourceKey& source, ImageSessionStage stage) {
	if (!MatchesSelection(generation, source) ||
		stage_ == ImageSessionStage::Committed || stage_ == ImageSessionStage::Failed ||
		stage == ImageSessionStage::Committed || stage == ImageSessionStage::Failed) {
		return false;
	}
	stage_ = stage;
	return true;
}

bool ImageSessionController::MarkDisplayFrameReady(std::uint64_t generation,
	const SourceKey& source) {
	if (!MatchesSelection(generation, source) ||
		stage_ == ImageSessionStage::Committed || stage_ == ImageSessionStage::Failed) {
		return false;
	}
	stage_ = ImageSessionStage::DisplayFrameReady;
	return true;
}

bool ImageSessionController::CommitSelectedLoad(std::uint64_t generation,
	const SourceKey& source, const fs::path& filename, RecentFiles& recentFiles) {
	if (!MatchesSelection(generation, source) ||
		stage_ != ImageSessionStage::DisplayFrameReady || !selection_.has_value() ||
		selection_->filename != NormalizeAbsolutePath(filename)) return false;
	if (selection_->tracksRecentHistory && !history_.CommitLoad(filename, recentFiles)) {
		return false;
	}
	stage_ = ImageSessionStage::Committed;
	return true;
}

bool ImageSessionController::FailSelectedLoad(std::uint64_t generation,
	const SourceKey& source, const fs::path& filename) {
	if (!MatchesSelection(generation, source) || !selection_.has_value() ||
		selection_->filename != NormalizeAbsolutePath(filename) ||
		stage_ == ImageSessionStage::Committed || stage_ == ImageSessionStage::Failed) {
		return false;
	}
	if (selection_->tracksRecentHistory) (void)history_.FailLoad(filename);
	stage_ = ImageSessionStage::Failed;
	return true;
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
	const ViewportSnapshot& navigationViewport, const RecentFiles& recentFiles,
	bool restoreRecentViewport) const {
	if (clipboardMode) return navigationViewport;
	const fs::path selectedPath = NormalizeAbsolutePath(filename);
	if (clipboardReturnViewport_.has_value() && !LoadedPath().empty() &&
		selectedPath == LoadedPath()) {
		return *clipboardReturnViewport_;
	}
	return history_.ViewportForSelection(selectedPath, currentViewport,
		navigationViewport, recentFiles, restoreRecentViewport);
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
