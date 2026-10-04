#include "pending_image_intents.h"

#include <utility>

namespace jpegview_linux {

PendingImageOperationAdmission PlanPendingImageOperationAdmission(
	bool operationAlreadyPending, std::size_t acceptedEarlierIntentCount,
	bool selectedLoadPending, bool replayingFrontIntent) {
	if (operationAlreadyPending ||
		(acceptedEarlierIntentCount != 0 && !replayingFrontIntent)) {
		return PendingImageOperationAdmission::Rejected;
	}
	return selectedLoadPending ?
		PendingImageOperationAdmission::WaitForSelectedCommit :
		PendingImageOperationAdmission::StartNow;
}

void PendingImageIntents::Begin(const std::filesystem::path& filename,
	const SourceKey& source, std::uint64_t loadGeneration) {
	Cancel();
	if (filename.empty() || !source.Valid() || loadGeneration == 0) return;
	pending_ = PendingImageIntentBatch{filename.lexically_normal(), source,
		loadGeneration, {}, false};
}

bool PendingImageIntents::CanQueue(const std::filesystem::path& filename,
	const SourceKey& source, std::uint64_t loadGeneration) const {
	return pending_.has_value() &&
		pending_->filename == filename.lexically_normal() &&
		pending_->source == source && pending_->loadGeneration == loadGeneration &&
		pending_->actions.size() < kMaximumPendingImageIntents;
}

bool PendingImageIntents::MatchesSelection(const std::filesystem::path& filename,
	const SourceKey& source, std::uint64_t loadGeneration) const {
	return pending_.has_value() &&
		pending_->filename == filename.lexically_normal() &&
		pending_->source == source && pending_->loadGeneration == loadGeneration;
}

bool PendingImageIntents::MatchesStartupLoad(bool startupScan,
	bool selectedLoadPending, bool startupLoad,
	const std::filesystem::path& filename, const SourceKey& source,
	std::uint64_t loadGeneration) const {
	return startupScan && selectedLoadPending && startupLoad &&
		MatchesSelection(filename, source, loadGeneration);
}

bool PendingImageIntents::QueueViewport(const std::filesystem::path& filename,
	const SourceKey& source, std::uint64_t loadGeneration,
	const ViewportIntent& intent) {
	if (!CanQueue(filename, source, loadGeneration)) return false;
	pending_->actions.push_back(PendingImageIntent{
		PendingImageIntentType::Viewport, intent, 0});
	return true;
}

bool PendingImageIntents::QueueTransform(const std::filesystem::path& filename,
	const SourceKey& source, std::uint64_t loadGeneration, int command) {
	if (!CanQueue(filename, source, loadGeneration) || command == 0) return false;
	pending_->actions.push_back(PendingImageIntent{
		PendingImageIntentType::Transform, {}, command});
	return true;
}

bool PendingImageIntents::QueueCopySelection(
	const std::filesystem::path& filename, const SourceKey& source,
	std::uint64_t loadGeneration, int left, int top, int right, int bottom) {
	if (!CanQueue(filename, source, loadGeneration) || left < 0 || top < 0 ||
		right <= left || bottom <= top) return false;
	PendingImageIntent action;
	action.type = PendingImageIntentType::CopySelection;
	action.selectionLeft = left;
	action.selectionTop = top;
	action.selectionRight = right;
	action.selectionBottom = bottom;
	pending_->actions.push_back(action);
	return true;
}

bool PendingImageIntents::QueueCopyImage(const std::filesystem::path& filename,
	const SourceKey& source, std::uint64_t loadGeneration, bool fullSize) {
	if (!CanQueue(filename, source, loadGeneration)) return false;
	PendingImageIntent action;
	action.type = PendingImageIntentType::CopyImage;
	action.fullSize = fullSize;
	pending_->actions.push_back(action);
	return true;
}

bool PendingImageIntents::QueueCropSelection(
	const std::filesystem::path& filename, const SourceKey& source,
	std::uint64_t loadGeneration, int left, int top, int right, int bottom) {
	if (!CanQueue(filename, source, loadGeneration) || left < 0 || top < 0 ||
		right <= left || bottom <= top) return false;
	PendingImageIntent action;
	action.type = PendingImageIntentType::CropSelection;
	action.selectionLeft = left;
	action.selectionTop = top;
	action.selectionRight = right;
	action.selectionBottom = bottom;
	pending_->actions.push_back(action);
	return true;
}

bool PendingImageIntents::RequestTransition(const SourceKey& source,
	std::uint64_t loadGeneration) {
	if (!pending_.has_value() || pending_->source != source ||
		pending_->loadGeneration != loadGeneration) return false;
	pending_->startTransition = true;
	return true;
}

std::optional<PendingImageIntentBatch> PendingImageIntents::Drain(
	const SourceKey& source, std::uint64_t loadGeneration) {
	if (!pending_.has_value() || pending_->source != source ||
		pending_->loadGeneration != loadGeneration) return std::nullopt;
	std::vector<PendingImageIntent> actions = std::move(pending_->actions);
	pending_->actions.clear();
	PendingImageIntentBatch result{pending_->filename, pending_->source,
		pending_->loadGeneration, std::move(actions),
		pending_->startTransition};
	pending_->startTransition = false;
	return result;
}

std::optional<PendingImageIntentBatch> PendingImageIntents::Take(
	const SourceKey& source, std::uint64_t loadGeneration) {
	if (!pending_.has_value() || pending_->source != source ||
		pending_->loadGeneration != loadGeneration) return std::nullopt;
	std::optional<PendingImageIntentBatch> result(std::move(pending_));
	pending_.reset();
	return result;
}

std::size_t PendingImageIntents::ActionCount() const {
	return pending_.has_value() ? pending_->actions.size() : 0;
}

void PendingImageIntents::Cancel() {
	pending_.reset();
}

void DeferredExifDateAction::Begin(const std::filesystem::path& filename,
	const SourceKey& source, std::uint64_t metadataGeneration) {
	Cancel();
	if (filename.empty() || !source.Valid() || metadataGeneration == 0) return;
	filename_ = filename;
	source_ = source;
	metadataGeneration_ = metadataGeneration;
	metadataPending_ = true;
	imageCommitted_ = false;
}

bool DeferredExifDateAction::MustDeferFor(
	const std::filesystem::path& filename, const SourceKey& source) const {
	return filename_ == filename && source_ == source &&
		(metadataPending_ || !imageCommitted_);
}

bool DeferredExifDateAction::Defer(const std::filesystem::path& filename,
	const SourceKey& source) {
	if (!MustDeferFor(filename, source)) return false;
	actionDeferred_ = true;
	return true;
}

ExifDateActionCompletion DeferredExifDateAction::Complete(
	const ExifMetadataResult& result, const std::filesystem::path& selectedFilename,
	const SourceKey& selectedSource) {
	if (!metadataPending_ || filename_ != selectedFilename || source_ != selectedSource ||
		!IsCurrentExifMetadataResult(result, metadataGeneration_, source_)) return {};
	metadataPending_ = false;
	const bool runDeferredAction = actionDeferred_ && imageCommitted_;
	const ExifDateActionCompletion completion{true, runDeferredAction};
	if (runDeferredAction || imageCommitted_) Cancel();
	return completion;
}

ExifDateActionCompletion DeferredExifDateAction::MarkImageCommitted(
	const std::filesystem::path& filename, const SourceKey& source) {
	if (filename_ != filename || source_ != source) return {};
	imageCommitted_ = true;
	const bool runDeferredAction = actionDeferred_ && !metadataPending_;
	const ExifDateActionCompletion completion{false, runDeferredAction};
	if (runDeferredAction || !metadataPending_) Cancel();
	return completion;
}

void DeferredExifDateAction::Cancel() {
	filename_.clear();
	source_ = {};
	metadataGeneration_ = 0;
	metadataPending_ = false;
	imageCommitted_ = false;
	actionDeferred_ = false;
}

} // namespace jpegview_linux
