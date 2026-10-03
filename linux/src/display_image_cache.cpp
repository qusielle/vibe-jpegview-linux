#include "display_image_cache.h"

#include "archive_source.h"
#include "image.h"
#include "image_cache.h"
#include "cache_policy.h"
#include "event_loop_model.h"
#include "perf_diagnostics.h"
#include "source_work_coordinator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <list>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

void RecordDisplayCancellation(PerfWorkClass workClass,
	PerfExecution execution = PerfExecution::EventThread) {
	PerfContextScope context(workClass, execution);
	PerfDiagnostics::Instance().Record(PerfMetric::Cancellation);
}

bool EstimateDisplayBytes(const DisplayImageRequest& request, std::size_t& bytes) {
	bytes = 0;
	if (request.targetWidth <= 0 || request.targetHeight <= 0) return false;
	const std::size_t width = static_cast<std::size_t>(request.targetWidth);
	const std::size_t height = static_cast<std::size_t>(request.targetHeight);
	const std::size_t maximum = std::numeric_limits<std::size_t>::max();
	if (width > maximum / height) return false;
	const std::size_t pixels = width * height;
	if (pixels > maximum / 4) return false;
	bytes = pixels * 4;
	if (request.includeSpectrum) {
		const std::size_t spectrumBytes = sizeof(GrayscaleSpectrum);
		if (bytes > maximum - spectrumBytes) return false;
		bytes += spectrumBytes;
	}
	return true;
}

bool IsSpeculativeDisplayWork(bool foreground, PerfWorkClass workClass) {
	return !foreground && workClass != PerfWorkClass::ActiveImageSpread;
}

bool ClassifyArchiveMemberWithSourceAdmission(const fs::path& path,
	WorkContext context, bool& archiveMember) {
	archiveMember = false;
	if (context.sourceAccessAlreadyAdmitted) {
		if (context.Continue()) {
			ScopedWorkContext activeContext(context);
			archiveMember = IsArchiveMemberLocation(path);
			return context.Continue();
		}
		return false;
	}
	if (context.cpuProcessingAlreadyAdmitted) return false;
	SourceWorkLease admission = SourceWorkCoordinator::Global().Acquire(context, path);
	if (!admission || !context.Continue()) return false;
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	{
		ScopedWorkContext activeContext(context);
		archiveMember = IsArchiveMemberLocation(path);
	}
	return context.Continue();
}

bool ValidateSourceWithAdmission(const SourceDescriptor& source,
	const WorkContext& ownerContext, bool& sourceCurrent) {
	WorkContext context = MakeWorkContext(source,
		SourceWorkPriority::Metadata, ownerContext.shouldContinue);
	context.currentPriority = ownerContext.currentPriority;
	context.onForegroundYield = ownerContext.onForegroundYield;
	const fs::path& path = source.LogicalPath();
	bool archiveMember = source.Metadata().archiveMember;
	if (!archiveMember && !ClassifyArchiveMemberWithSourceAdmission(path,
		context, archiveMember)) return false;
	if (archiveMember) {
		SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
			context, path);
		if (!admission || !context.Continue()) return false;
		context.sourcePriority = context.Priority();
		context.sourceAccessAlreadyAdmitted = true;
		context.cpuProcessingAlreadyAdmitted = true;
		ScopedWorkContext activeContext(context);
		sourceCurrent = IsImageSourceCurrent(source);
		return context.Continue();
	}

	SourceWorkLease admission = SourceWorkCoordinator::Global().Acquire(context, path);
	if (!admission || !context.Continue()) return false;
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	ScopedWorkContext activeContext(context);
	sourceCurrent = IsImageSourceCurrent(source);
	return context.Continue();
}

SourceDescriptor DescribeCurrentSourceWithAdmission(const fs::path& path,
	const WorkContext& ownerContext) {
	WorkContext context = MakePathWorkContext(path,
		SourceWorkPriority::Metadata, ownerContext.shouldContinue);
	context.currentPriority = ownerContext.currentPriority;
	context.onForegroundYield = ownerContext.onForegroundYield;
	bool archiveMember = false;
	if (!ClassifyArchiveMemberWithSourceAdmission(path, context, archiveMember)) return {};
	if (archiveMember) {
		SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
			context, path);
		if (!admission || !context.Continue()) return {};
		context.sourcePriority = context.Priority();
		context.sourceAccessAlreadyAdmitted = true;
		context.cpuProcessingAlreadyAdmitted = true;
		ScopedWorkContext activeContext(context);
		return DescribeImageSource(path, context);
	}

	SourceWorkLease admission = SourceWorkCoordinator::Global().Acquire(context, path);
	if (!admission || !context.Continue()) return {};
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	ScopedWorkContext activeContext(context);
	return DescribeImageSource(path, context);
}

std::shared_ptr<const void> PreparedAllocationIdentity(
	const std::shared_ptr<const PreparedDisplayImage>& image) {
	return image ? std::shared_ptr<const void>(image, image.get()) :
		std::shared_ptr<const void>{};
}

template <typename Left, typename Right>
bool SameSharedOwnership(const Left& left, const Right& right) {
	return !left.owner_before(right) && !right.owner_before(left);
}

bool FiniteProcessingValues(const ImageProcessingParams& processing) {
	for (const double value : {processing.contrast, processing.gamma,
		processing.saturation, processing.cyanRed, processing.magentaGreen,
		processing.yellowBlue, processing.lightenShadows, processing.darkenHighlights,
		processing.deepShadows, processing.colorCorrection, processing.contrastCorrection,
		processing.sharpen, processing.unsharpRadius, processing.unsharpAmount,
		processing.unsharpThreshold}) {
		if (!std::isfinite(value)) return false;
	}
	return true;
}

int NormalizeQuarterTurns(int turns) {
	turns %= 4;
	if (turns < 0) turns += 4;
	return turns;
}

DisplayImageCacheKey MakeCacheKey(const SourceDescriptor& source,
	std::size_t frameIndex, int width, int height, bool autoContrast,
	const ImageProcessingParams& processing, int rotationQuarterTurns,
	bool includeSpectrum) {
	DisplayImageCacheKey key;
	key.source = source.Key();
	key.frameIndex = frameIndex;
	key.targetWidth = width;
	key.targetHeight = height;
	key.autoContrast = autoContrast;
	key.includeSpectrum = includeSpectrum;
	key.rotationQuarterTurns = NormalizeQuarterTurns(rotationQuarterTurns);
	key.processing = EffectiveImageProcessingParams(processing, autoContrast);
	return key;
}

std::string SerializeCacheKey(const DisplayImageCacheKey& key) {
	if (!key.Valid()) return {};
	std::ostringstream result;
	result.imbue(std::locale::classic());
	result << key.source.logicalPath.size() << ':';
	result.write(key.source.logicalPath.data(),
		static_cast<std::streamsize>(key.source.logicalPath.size()));
	result << ':' << key.source.backingIdentity.device << ':'
		<< key.source.backingIdentity.inode << ':' << key.source.backingIdentity.size << ':'
		<< key.source.backingIdentity.modifiedSeconds << ':'
		<< key.source.backingIdentity.modifiedNanoseconds << ':' << key.frameIndex << ':'
		<< key.targetWidth << 'x' << key.targetHeight << ':' << key.autoContrast << ':'
		<< key.rotationQuarterTurns << ':' << key.includeSpectrum << ':' << std::hexfloat
		<< key.processing.contrast << ':' << key.processing.gamma << ':'
		<< key.processing.saturation << ':' << key.processing.cyanRed << ':'
		<< key.processing.magentaGreen << ':' << key.processing.yellowBlue << ':'
		<< key.processing.lightenShadows << ':' << key.processing.darkenHighlights << ':'
		<< key.processing.deepShadows << ':' << key.processing.colorCorrection << ':'
		<< key.processing.contrastCorrection << ':' << key.processing.sharpen << ':'
		<< key.processing.unsharpRadius << ':' << key.processing.unsharpAmount << ':'
		<< key.processing.unsharpThreshold << ':' << key.processing.localDensityEnabled;
	return result.str();
}

DisplayImageCache::ImagePtr PrepareDisplayImage(const DisplayImageRequest& request) {
	if (!request.Valid()) return {};
	WorkContext workContext = request.workContext;
	if (workContext.source.Empty()) {
		const SourceWorkPriority priority = request.workClass == PerfWorkClass::VisibleThumbnail ||
			request.workClass == PerfWorkClass::NearestNavigationNeighbor ||
			request.workClass == PerfWorkClass::DistantSpeculation ?
			SourceWorkPriority::Speculative : SourceWorkPriority::Foreground;
		workContext = MakeWorkContext(request.source, priority);
	}
	const std::function<bool()> previousContinue = workContext.shouldContinue;
	workContext.shouldContinue = [&request, previousContinue] {
		return (!request.cancellation || !request.cancellation->load()) &&
			(!previousContinue || previousContinue());
	};
	const auto cancelled = [&workContext] { return !workContext.Continue(); };
	const std::function<bool()> shouldContinue = [&workContext] {
		return workContext.Continue();
	};
	if (cancelled()) return {};
	DecodedImage displayDecoded;
	const DecodedFrame* decodedFrame = nullptr;
	if (request.decoded) {
		decodedFrame = &request.decoded->frames[request.frameIndex];
	} else {
		int sourceWidth = 0;
		int sourceHeight = 0;
		std::string errorMessage;
		bool decoded = false;
		if (request.includeSpectrum) {
			// Preserve the full-source histogram while keeping its decode and scan off
			// the event thread. The ordinary fitted-JPEG path remains reduced-DCT.
			decoded = DecodeImage(request.filename, displayDecoded, errorMessage, workContext);
			if (decoded && !displayDecoded.frames.empty()) {
				sourceWidth = displayDecoded.frames.front().width;
				sourceHeight = displayDecoded.frames.front().height;
			}
		} else {
			const bool swapsAxes = (request.rotationQuarterTurns & 1) != 0;
			const int decodeTargetWidth = swapsAxes ? request.targetHeight : request.targetWidth;
			const int decodeTargetHeight = swapsAxes ? request.targetWidth : request.targetHeight;
			decoded = DecodeJpegForDisplay(request.filename, decodeTargetWidth,
				decodeTargetHeight, displayDecoded, sourceWidth, sourceHeight,
				errorMessage, workContext);
		}
		if (!decoded || displayDecoded.frames.empty() ||
			sourceWidth != request.sourceWidth || sourceHeight != request.sourceHeight ||
			cancelled()) {
			if (!cancelled() && !errorMessage.empty()) {
				throw std::runtime_error(errorMessage);
			}
			return {};
		}
		decodedFrame = &displayDecoded.frames.front();
	}
	CpuWorkLease cpuLease;
	if (!workContext.cpuProcessingAlreadyAdmitted) {
		cpuLease = SourceWorkCoordinator::Global().AcquireCpu(workContext);
		if (!cpuLease) return {};
		workContext.cpuProcessingAlreadyAdmitted = true;
	}
	if (!workContext.Continue()) return {};
	Image image;
	if (request.decoded) {
		if (!image.StoreBGRA(decodedFrame->bgra.data(), decodedFrame->width,
			decodedFrame->height, decodedFrame->hasTransparency)) return {};
	} else {
		// File-backed JPEG pixels are private to this worker. Move them into the
		// resize stage instead of copying the reduced decode a second time.
		DecodedFrame& ownedFrame = displayDecoded.frames.front();
		image.width = image.originalWidth = ownedFrame.width;
		image.height = image.originalHeight = ownedFrame.height;
		image.bgra = std::move(ownedFrame.bgra);
		image.hasTransparency = ownedFrame.hasTransparency;
	}
	{
		PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Processing);
		if (!image.ApplyProcessing(request.processing, request.autoContrast,
			shouldContinue) || cancelled()) return {};
	}
	switch (request.rotationQuarterTurns) {
	case 1:
		if (!image.Rotate(true, shouldContinue)) return {};
		break;
	case 2:
		if (!image.Rotate(true, shouldContinue) ||
			!image.Rotate(true, shouldContinue)) return {};
		break;
	case 3:
		if (!image.Rotate(false, shouldContinue)) return {};
		break;
	default:
		break;
	}
	if (cancelled()) return {};
	GrayscaleSpectrum spectrum{};
	if (request.includeSpectrum) {
		const std::optional<GrayscaleSpectrum> preparedSpectrum =
			TryBuildGrayscaleSpectrum(image.bgra, image.width, image.height,
				shouldContinue);
		if (!preparedSpectrum || cancelled()) return {};
		spectrum = *preparedSpectrum;
	}
	if (request.targetWidth < image.width || request.targetHeight < image.height) {
		PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Resampling);
		if (!image.Resize(request.targetWidth, request.targetHeight, 3,
			shouldContinue) || cancelled()) return {};
	}

	auto prepared = std::make_shared<PreparedDisplayImage>();
	prepared->filename = request.filename;
	prepared->source = request.source.WithImageProperties(request.sourceWidth,
		request.sourceHeight, image.hasTransparency);
	prepared->cacheKey = request.cacheKey;
	prepared->key = request.key;
	prepared->width = image.width;
	prepared->height = image.height;
	prepared->hasTransparency = image.hasTransparency;
	prepared->bgra = std::move(image.bgra);
	if (request.includeSpectrum) {
		prepared->spectrum = std::make_shared<const GrayscaleSpectrum>(spectrum);
	}
	prepared->priority = request.priority;
	prepared->workClass = request.workClass;
	prepared->rotationQuarterTurns = request.rotationQuarterTurns;
	return prepared;
}

} // namespace

bool DisplayImageCacheKey::Valid() const {
	return source.Valid() && targetWidth > 0 && targetHeight > 0 &&
		FiniteProcessingValues(processing);
}

bool operator==(const DisplayImageCacheKey& left, const DisplayImageCacheKey& right) {
	return left.source == right.source && left.frameIndex == right.frameIndex &&
		left.targetWidth == right.targetWidth && left.targetHeight == right.targetHeight &&
		left.rotationQuarterTurns == right.rotationQuarterTurns &&
		left.autoContrast == right.autoContrast &&
		left.includeSpectrum == right.includeSpectrum &&
		EqualEffectiveImageProcessingParams(left.processing, right.processing);
}

bool operator!=(const DisplayImageCacheKey& left, const DisplayImageCacheKey& right) {
	return !(left == right);
}

bool CanReuseDisplayImageRepresentation(const DisplayImageCacheKey& requested,
	const DisplayImageCacheKey& available) {
	return requested.Valid() && available.Valid() &&
		requested.source == available.source &&
		requested.frameIndex == available.frameIndex &&
		available.targetWidth >= requested.targetWidth &&
		available.targetHeight >= requested.targetHeight &&
		requested.rotationQuarterTurns == available.rotationQuarterTurns &&
		requested.autoContrast == available.autoContrast &&
		requested.includeSpectrum == available.includeSpectrum &&
		EqualEffectiveImageProcessingParams(requested.processing, available.processing);
}

std::size_t DisplayImageCacheKeyHash::operator()(const DisplayImageCacheKey& key) const {
	const auto combine = [](std::size_t seed, std::size_t value) {
		return seed ^ (value + static_cast<std::size_t>(0x9e3779b9u) +
			(seed << 6) + (seed >> 2));
	};
	std::size_t value = SourceKeyHash{}(key.source);
	value = combine(value, std::hash<std::size_t>{}(key.frameIndex));
	value = combine(value, std::hash<int>{}(key.targetWidth));
	value = combine(value, std::hash<int>{}(key.targetHeight));
	value = combine(value, std::hash<int>{}(key.rotationQuarterTurns));
	value = combine(value, std::hash<bool>{}(key.autoContrast));
	value = combine(value, std::hash<bool>{}(key.includeSpectrum));
	const ImageProcessingParams& processing = key.processing;
	for (const double parameter : {processing.contrast, processing.gamma,
		processing.saturation, processing.cyanRed, processing.magentaGreen,
		processing.yellowBlue, processing.lightenShadows, processing.darkenHighlights,
		processing.deepShadows, processing.colorCorrection, processing.contrastCorrection,
		processing.sharpen, processing.unsharpRadius, processing.unsharpAmount,
		processing.unsharpThreshold}) {
		value = combine(value, std::hash<double>{}(parameter));
	}
	return combine(value, std::hash<bool>{}(processing.localDensityEnabled));
}

bool DisplayImageRequest::Valid() const {
	if (!cacheKey.Valid() || key.empty() || !FiniteProcessingValues(processing) ||
		filename.lexically_normal() != source.LogicalPath() ||
		source.Key() != cacheKey.source ||
		targetWidth <= 0 || targetHeight <= 0 ||
		rotationQuarterTurns < 0 || rotationQuarterTurns > 3) return false;
	if (!decoded) return sourceWidth > 0 && sourceHeight > 0 && IsJpegPath(filename);
	if (frameIndex >= decoded->frames.size()) return false;
	const DecodedFrame& frame = decoded->frames[frameIndex];
	return frame.width > 0 && frame.height > 0 && !frame.bgra.empty();
}

bool EstimateDisplayImageBytes(const DisplayImageRequest& request,
	std::size_t& bytes) {
	return EstimateDisplayBytes(request, bytes);
}

DisplayImageRequest MakeDisplayImageRequest(const fs::path& filename,
	const std::shared_ptr<const DecodedImage>& decoded, std::size_t frameIndex,
	int targetWidth, int targetHeight, bool autoContrast, std::size_t priority,
	const ImageProcessingParams& processing, int rotationQuarterTurns,
	bool includeSpectrum) {
	return MakeDisplayImageRequest(DescribeImageSource(filename), decoded, frameIndex,
		targetWidth, targetHeight, autoContrast, priority, processing,
		rotationQuarterTurns, includeSpectrum);
}

DisplayImageRequest MakeDisplayImageRequest(const SourceDescriptor& source,
	const std::shared_ptr<const DecodedImage>& decoded, std::size_t frameIndex,
	int targetWidth, int targetHeight, bool autoContrast, std::size_t priority,
	const ImageProcessingParams& processing, int rotationQuarterTurns,
	bool includeSpectrum) {
	DisplayImageRequest request;
	request.source = source;
	request.filename = source.LogicalPath();
	request.decoded = decoded;
	request.frameIndex = frameIndex;
	request.rotationQuarterTurns = NormalizeQuarterTurns(rotationQuarterTurns);
	request.targetWidth = targetWidth;
	request.targetHeight = targetHeight;
	request.autoContrast = autoContrast;
	request.includeSpectrum = includeSpectrum;
	request.processing = processing;
	request.priority = priority;
	if (!decoded || frameIndex >= decoded->frames.size() || targetWidth <= 0 || targetHeight <= 0) {
		return request;
	}
	request.sourceWidth = decoded->frames[frameIndex].width;
	request.sourceHeight = decoded->frames[frameIndex].height;
	request.cacheKey = MakeCacheKey(source, frameIndex, targetWidth, targetHeight,
		autoContrast, processing, request.rotationQuarterTurns, includeSpectrum);
	request.key = SerializeCacheKey(request.cacheKey);
	return request;
}

DisplayImageRequest MakeJpegDisplayImageRequest(const fs::path& filename,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	bool autoContrast, std::size_t priority, const ImageProcessingParams& processing,
	int rotationQuarterTurns, bool includeSpectrum) {
	return MakeJpegDisplayImageRequest(DescribeImageSource(filename), sourceWidth,
			sourceHeight, targetWidth, targetHeight, autoContrast, priority, processing,
		rotationQuarterTurns, includeSpectrum);
}

DisplayImageRequest MakeJpegDisplayImageRequest(const SourceDescriptor& source,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	bool autoContrast, std::size_t priority, const ImageProcessingParams& processing,
	int rotationQuarterTurns, bool includeSpectrum) {
	DisplayImageRequest request;
	request.source = source;
	request.filename = source.LogicalPath();
	request.sourceWidth = sourceWidth;
	request.sourceHeight = sourceHeight;
	request.rotationQuarterTurns = NormalizeQuarterTurns(rotationQuarterTurns);
	request.targetWidth = targetWidth;
	request.targetHeight = targetHeight;
	request.autoContrast = autoContrast;
	request.includeSpectrum = includeSpectrum;
	request.processing = processing;
	request.priority = priority;
	if (!IsJpegPath(request.filename) || sourceWidth <= 0 || sourceHeight <= 0 ||
		targetWidth <= 0 || targetHeight <= 0) return request;
	request.cacheKey = MakeCacheKey(source, 0, targetWidth, targetHeight,
		autoContrast, processing, request.rotationQuarterTurns, includeSpectrum);
	request.key = SerializeCacheKey(request.cacheKey);
	return request;
}

std::size_t PreparedDisplayImageBytes(const PreparedDisplayImage& image) {
	return image.bgra.size() +
		(image.spectrum ? sizeof(*image.spectrum) : 0);
}

DisplayImageTarget ClampDisplayImageTarget(int sourceWidth, int sourceHeight,
	int targetWidth, int targetHeight, int rotationQuarterTurns) {
	if (sourceWidth <= 0 || sourceHeight <= 0) return {targetWidth, targetHeight};
	const bool swapsAxes = (NormalizeQuarterTurns(rotationQuarterTurns) & 1) != 0;
	const int maximumWidth = swapsAxes ? sourceHeight : sourceWidth;
	const int maximumHeight = swapsAxes ? sourceWidth : sourceHeight;
	return {std::min(targetWidth, maximumWidth), std::min(targetHeight, maximumHeight)};
}

bool FailedDisplayRequestNeedsNewResolution(const DisplayImageRequest& request,
	const std::string& failedRequestKey,
	const DisplayImageTarget& requestedResolution) {
	return !failedRequestKey.empty() && request.key == failedRequestKey &&
		(request.targetWidth != requestedResolution.width ||
			request.targetHeight != requestedResolution.height);
}

std::size_t DisplayPrefetchCandidateLimit(std::size_t cacheBytes,
	std::size_t fileCount) {
	constexpr std::size_t maximumSpeculativeFiles = 512;
	if (cacheBytes == 0 || fileCount < 2) return 0;
	return std::min(fileCount - 1, maximumSpeculativeFiles);
}

struct DisplayImageCache::Impl {
	using ImagePtr = DisplayImageCache::ImagePtr;
	using LruList = std::list<DisplayImageCacheKey>;

	struct RetirementState {
		std::mutex mutex;
		std::condition_variable available;
		struct RetiredImage {
			ImagePtr image;
			CacheReservation reservation;
		};
		std::deque<RetiredImage> retired;
		std::deque<std::shared_ptr<const DecodedImage>> retiredDecoded;
		std::weak_ptr<const void> retiringOwner;
		std::size_t retiringBytes = 0;
		bool retiringDisplayImage = false;
		bool retiring = false;
		bool stopping = false;
	};

	struct Entry {
		ImagePtr image;
		std::size_t bytes = 0;
		CacheProtectionTier protection = CacheProtectionTier::DistantSpeculation;
		CacheReservation reservation;
		LruList::iterator lru;
	};
	using EntryMap = std::unordered_map<DisplayImageCacheKey, Entry,
		DisplayImageCacheKeyHash>;

	struct Work {
		DisplayImageRequest request;
		std::uint64_t generation = 0;
		std::uint64_t epoch = 0;
		bool foreground = false;
	};

	struct Completion {
		ImagePtr image;
		std::size_t priority = 0;
		PerfWorkClass workClass = PerfWorkClass::Unspecified;
		std::uint64_t selectionGeneration = 0;
		bool speculative = false;
		std::size_t reservedBytes = 0;
		CacheReservation temporaryReservation;
	};

	struct Reservation {
		bool speculative = false;
		std::size_t bytes = 0;
	};

	struct PendingCachedCompletion {
		std::size_t priority = 0;
		PerfWorkClass workClass = PerfWorkClass::Unspecified;
	};

	struct BorrowedAllocation {
		std::size_t bytes = 0;
		CacheReservation reservation;
	};

	static std::size_t TierIndex(CacheProtectionTier tier) {
		return static_cast<std::size_t>(tier);
	}

	void Touch(EntryMap::iterator entry) {
		LruList& list = lru[TierIndex(entry->second.protection)];
		list.splice(list.end(), list, entry->second.lru);
	}

	void SetProtection(EntryMap::iterator entry, CacheProtectionTier protection) {
		if (entry->second.protection == protection) return;
		lru[TierIndex(entry->second.protection)].erase(entry->second.lru);
		LruList& list = lru[TierIndex(protection)];
		list.push_back(entry->first);
		entry->second.lru = std::prev(list.end());
		entry->second.protection = protection;
	}

	enum class CachedCompletionPriorityMode {
		PreserveBest,
		ReplaceBackground
	};

	bool CanReserveSpeculative(std::size_t bytes) const {
		return bytes <= DisplayImageCache::kMaximumSpeculativeCompletionBytes &&
		speculativeReservedImages < DisplayImageCache::kMaximumSpeculativeCompletions &&
		bytes <= DisplayImageCache::kMaximumSpeculativeCompletionBytes -
			speculativeReservedBytes;
	}

	bool CanStart(const Work& work) const {
		if (!IsSpeculativeDisplayWork(work.foreground, work.request.workClass)) return true;
		std::size_t bytes = 0;
		return EstimateDisplayBytes(work.request, bytes) && CanReserveSpeculative(bytes);
	}

	bool HasStartableWork() const {
		return std::any_of(queue.begin(), queue.end(), [this](const Work& work) {
			return CanStart(work);
		});
	}

	std::deque<Work>::iterator FindStartableWork() {
		return std::find_if(queue.begin(), queue.end(), [this](const Work& work) {
			return CanStart(work);
		});
	}

	void ReserveSpeculative(std::size_t bytes) {
		++speculativeReservedImages;
		speculativeReservedBytes += bytes;
	}

	void ReleaseSpeculative(std::size_t bytes) {
		if (speculativeReservedImages != 0) --speculativeReservedImages;
		speculativeReservedBytes -= std::min(speculativeReservedBytes, bytes);
	}

	void ReleaseCompletionReservation(Completion& completion) {
		if (!completion.speculative) return;
		ReleaseSpeculative(completion.reservedBytes);
		completion.speculative = false;
		completion.reservedBytes = 0;
	}

	void PromoteCompletion(Completion& completion, PerfWorkClass workClass) {
		ReleaseCompletionReservation(completion);
		completion.priority = 0;
		completion.workClass = workClass;
	}

	void QueueCachedCompletion(const ImagePtr& image, std::size_t priority,
		PerfWorkClass workClass,
		CachedCompletionPriorityMode priorityMode = CachedCompletionPriorityMode::PreserveBest) {
		if (!image) return;
		const auto existing = std::find_if(completed.begin(), completed.end(),
			[&image](const Completion& completion) {
				return completion.image && completion.image->cacheKey == image->cacheKey;
			});
		const bool preserveForeground = existing != completed.end() && existing->priority == 0;
		const bool preserveBetterPriority = existing != completed.end() &&
			priorityMode == CachedCompletionPriorityMode::PreserveBest &&
			existing->priority < priority;
		const bool preserveExisting = preserveForeground || preserveBetterPriority;
		const std::size_t effectivePriority = preserveExisting ? existing->priority : priority;
		const PerfWorkClass effectiveWorkClass = preserveExisting ? existing->workClass : workClass;
		const bool speculative = IsSpeculativeDisplayWork(effectivePriority == 0,
			effectiveWorkClass);
		const std::size_t bytes = PreparedDisplayImageBytes(*image);
		if (speculative && bytes > DisplayImageCache::kMaximumSpeculativeCompletionBytes) {
			pendingCachedCompletions.erase(image->cacheKey);
			if (existing != completed.end()) {
				ReleaseCompletionReservation(*existing);
				QueueRetirement(std::move(existing->image),
					std::move(existing->temporaryReservation));
				completed.erase(existing);
			}
			return;
		}
		if (existing != completed.end()) {
			if (existing->speculative && !speculative) {
				ReleaseCompletionReservation(*existing);
			} else if (!existing->speculative && speculative) {
				if (!CanReserveSpeculative(bytes)) {
					pendingCachedCompletions[image->cacheKey] = {
						effectivePriority, effectiveWorkClass};
					QueueRetirement(std::move(existing->image),
						std::move(existing->temporaryReservation));
					completed.erase(existing);
					return;
				}
				ReserveSpeculative(bytes);
				existing->speculative = true;
				existing->reservedBytes = bytes;
			}
			existing->priority = effectivePriority;
			existing->workClass = effectiveWorkClass;
			pendingCachedCompletions.erase(image->cacheKey);
			return;
		}
		if (speculative && !CanReserveSpeculative(bytes)) {
			pendingCachedCompletions[image->cacheKey] = {effectivePriority, workClass};
			return;
		}
		if (speculative) ReserveSpeculative(bytes);
		completed.push_back({image, effectivePriority, effectiveWorkClass, 0,
			speculative, speculative ? bytes : 0, {}});
		pendingCachedCompletions.erase(image->cacheKey);
	}

	void FillPendingCachedCompletions() {
		for (;;) {
			auto selected = pendingCachedCompletions.end();
			ImagePtr selectedImage;
			for (auto pending = pendingCachedCompletions.begin();
				pending != pendingCachedCompletions.end(); ++pending) {
				const auto cached = entries.find(pending->first);
				if (cached == entries.end()) continue;
				const bool speculative = IsSpeculativeDisplayWork(
					pending->second.priority == 0, pending->second.workClass);
				const std::size_t bytes = PreparedDisplayImageBytes(*cached->second.image);
				if (speculative && !CanReserveSpeculative(bytes)) continue;
				if (selected == pendingCachedCompletions.end() ||
					pending->second.priority < selected->second.priority) {
					selected = pending;
					selectedImage = cached->second.image;
				}
			}
			if (selected == pendingCachedCompletions.end()) return;
			bool hasStartableQueuedWork = false;
			std::size_t startableQueuedPriority = 0;
			for (const Work& work : queue) {
				if (!CanStart(work)) continue;
				const std::size_t priority = work.foreground ? 0 : work.request.priority;
				if (!hasStartableQueuedWork || priority < startableQueuedPriority) {
					startableQueuedPriority = priority;
					hasStartableQueuedWork = true;
				}
			}
			if (hasStartableQueuedWork && startableQueuedPriority <= selected->second.priority) return;
			const PendingCachedCompletion pending = selected->second;
			pendingCachedCompletions.erase(selected);
			QueueCachedCompletion(selectedImage, pending.priority, pending.workClass);
		}
	}

	explicit Impl(std::size_t budget, std::size_t requestedWorkers, Processor prepare,
		std::shared_ptr<SharedCacheBudget> shared)
		: byteBudget(budget), processor(std::move(prepare)), sharedBudget(std::move(shared)),
		  diagnosticsEnabled(PerfDiagnostics::Instance().Enabled()) {
		if (!processor) processor = PrepareDisplayImage;
		const std::size_t count = ClampCpuWorkerCount(requestedWorkers);
		workers.reserve(count);
		for (std::size_t index = 0; index < count; ++index) {
			workers.emplace_back([this] { Run(); });
		}
		retirementState = std::make_shared<RetirementState>();
		retirementWorker = std::thread([state = retirementState] { Retire(state); });
	}

	~Impl() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			++generation;
			++epoch;
			for (const auto& active : inFlightCancellation) {
				if (const auto cancellation = active.second.lock()) cancellation->store(true);
			}
			for (Work& work : queue) {
				RecordDisplayCancellation(work.request.workClass);
				QueueDecodedRetirement(std::move(work.request.decoded));
			}
			queue.clear();
			queuedKeys.clear();
			while (!completed.empty()) {
				ReleaseCompletionReservation(completed.front());
				QueueRetirement(std::move(completed.front().image),
					std::move(completed.front().temporaryReservation));
				completed.pop_front();
			}
			pendingCachedCompletions.clear();
			pendingForegroundRetries.clear();
			while (!entries.empty()) Erase(entries.begin());
		}
		workAvailable.notify_all();
		for (std::thread& worker : workers) {
			if (worker.joinable()) worker.join();
		}
		bool externalRetirementOwner = false;
		{
			std::lock_guard<std::mutex> lock(retirementState->mutex);
			retirementState->stopping = true;
			externalRetirementOwner = retirementState->retiring &&
				retirementState->retiringOwner.use_count() > 1;
			for (const RetirementState::RetiredImage& retired : retirementState->retired) {
				if (retired.image && retired.image.use_count() > 1) externalRetirementOwner = true;
			}
			for (const std::shared_ptr<const DecodedImage>& image :
				retirementState->retiredDecoded) {
				if (image.use_count() > 1) externalRetirementOwner = true;
			}
		}
		retirementState->available.notify_all();
		if (retirementWorker.joinable()) {
			if (externalRetirementOwner) {
				// Keep retirement independent of cache teardown while a pixel handle survives.
				retirementWorker.detach();
			} else {
				retirementWorker.join();
			}
		}
	}

	CacheReservation ForgetBorrowed(const ImagePtr& image) {
		if (!image) return {};
		const auto borrowedImage = borrowedAllocations.find(image.get());
		if (borrowedImage == borrowedAllocations.end()) return {};
		borrowedBytes -= borrowedImage->second.bytes;
		CacheReservation reservation = std::move(borrowedImage->second.reservation);
		borrowedAllocations.erase(borrowedImage);
		return reservation;
	}

	void QueueRetirement(ImagePtr image, CacheReservation reservation = {}) {
		if (!image) return;
		CacheReservation borrowed = ForgetBorrowed(image);
		if (!reservation) reservation = std::move(borrowed);
		const auto cached = entries.find(image->cacheKey);
		if (cached != entries.end() && cached->second.image.get() == image.get()) return;
		reservation.RelinquishRetainedOwnership();
		if (!reservation && sharedBudget) {
			reservation = sharedBudget->TrackTemporary(PreparedDisplayImageBytes(*image),
				CacheMemoryCategory::ActiveWorkingData,
				PreparedAllocationIdentity(image));
		}
		std::lock_guard<std::mutex> retirementLock(retirementState->mutex);
		if (SameSharedOwnership(image, retirementState->retiringOwner) ||
			std::any_of(retirementState->retired.begin(), retirementState->retired.end(), [&image](const auto& queued) {
				return SameSharedOwnership(image, queued.image);
			})) return;
		retirementState->retired.push_back({std::move(image), std::move(reservation)});
		retirementState->available.notify_one();
	}

	void ReleaseForUse(const ImagePtr& image, CacheMemoryCategory category) {
		if (!image) return;
		std::lock_guard<std::mutex> lock(mutex);
		CacheReservation reservation = ForgetBorrowed(image);
		auto found = entries.find(image->cacheKey);
		if (found != entries.end() && found->second.image.get() == image.get()) {
			const std::size_t bytes = found->second.bytes;
			reservation = std::move(found->second.reservation);
			lru[TierIndex(found->second.protection)].erase(found->second.lru);
			pendingCachedCompletions.erase(found->first);
			entries.erase(found);
			cachedBytes -= bytes;
		}
		if (reservation && !reservation.Reclassify(category)) {
			reservation.RelinquishRetainedOwnership();
			(void)reservation.ReclassifyIfNoRetainedOwners(category);
		}
		if (!reservation && sharedBudget) {
			reservation = sharedBudget->TrackTemporary(PreparedDisplayImageBytes(*image),
				category, PreparedAllocationIdentity(image));
		}
		QueueRetirement(image, std::move(reservation));
		workAvailable.notify_one();
	}

	void QueueDecodedRetirement(std::shared_ptr<const DecodedImage> image) {
		if (!image) return;
		if (sharedBudget) {
			std::shared_ptr<const void> allocation(image, image.get());
			CacheReservation working = sharedBudget->TrackTemporary(
				DecodedImageBytes(*image), CacheMemoryCategory::ActiveWorkingData,
				allocation);
			sharedBudget->RetireAllocation(std::move(allocation), std::move(working));
			return;
		}
		const DecodedImage* identity = image.get();
		std::lock_guard<std::mutex> retirementLock(retirementState->mutex);
		if (SameSharedOwnership(image, retirementState->retiringOwner) ||
			std::any_of(retirementState->retiredDecoded.begin(),
				retirementState->retiredDecoded.end(), [identity](const auto& queued) {
					return queued.get() == identity;
				})) return;
		retirementState->retiredDecoded.push_back(std::move(image));
		retirementState->available.notify_one();
	}

	std::size_t Erase(EntryMap::iterator entry) {
		const std::size_t bytes = entry->second.bytes;
		ImagePtr retiredImage = std::move(entry->second.image);
		CacheReservation reservation = std::move(entry->second.reservation);
		lru[TierIndex(entry->second.protection)].erase(entry->second.lru);
		pendingCachedCompletions.erase(entry->first);
		entries.erase(entry);
		cachedBytes -= bytes;
		QueueRetirement(std::move(retiredImage), std::move(reservation));
		return bytes;
	}

	EntryMap::iterator Oldest(CacheProtectionTier maximumTier) {
		for (std::size_t tier = 0; tier <= TierIndex(maximumTier); ++tier) {
			if (lru[tier].empty()) continue;
			const auto entry = entries.find(lru[tier].front());
			if (entry != entries.end()) return entry;
		}
		return entries.end();
	}

	bool Insert(const ImagePtr& image, bool mayEvict, CacheProtectionTier protection) {
		if (!image || !image->cacheKey.Valid()) return false;
		const std::size_t bytes = PreparedDisplayImageBytes(*image);
		if (bytes == 0 || bytes > byteBudget) return false;
		const auto existing = entries.find(image->cacheKey);
		if (existing != entries.end()) Erase(existing);
		if (!mayEvict && bytes > byteBudget - cachedBytes) return false;
		while (bytes > byteBudget - cachedBytes && !entries.empty()) {
			auto oldest = Oldest(CacheProtectionTier::DistantSpeculation);
			if (oldest == entries.end() && mayEvict &&
				protection == CacheProtectionTier::Active) {
				oldest = Oldest(CacheProtectionTier::Neighbor);
			}
			if (oldest == entries.end()) return false;
			Erase(oldest);
		}
		if (bytes > byteBudget - cachedBytes) return false;
		CacheReservation reservation;
		if (sharedBudget) {
			reservation = sharedBudget->TryReserve(bytes,
				CacheMemoryCategory::RetainedPreparedFrames,
				PreparedAllocationIdentity(image));
			while (!reservation) {
				if (!mayEvict) return false;
				auto oldest = Oldest(CacheProtectionTier::DistantSpeculation);
				if (oldest == entries.end() && protection == CacheProtectionTier::Active) {
					oldest = Oldest(CacheProtectionTier::Neighbor);
				}
				if (oldest == entries.end()) return false;
				const std::uint64_t releaseRevision =
					sharedBudget->RetainedCapacityRevision();
				Erase(oldest);
				if (sharedBudget->RetainedCapacityRevision() == releaseRevision) return false;
				reservation = sharedBudget->TryReserve(bytes,
					CacheMemoryCategory::RetainedPreparedFrames,
					PreparedAllocationIdentity(image));
				if (!reservation) return false;
			}
		}
		LruList& list = lru[TierIndex(protection)];
		list.push_back(image->cacheKey);
		entries.emplace(image->cacheKey, Entry{image, bytes, protection,
			std::move(reservation), std::prev(list.end())});
		cachedBytes += bytes;
		return true;
	}

	bool ResolveExternalKey(const std::string& externalKey,
		DisplayImageCacheKey& key) const {
		const auto matches = [&externalKey, &key](const DisplayImageCacheKey& candidate) {
			if (SerializeCacheKey(candidate) != externalKey) return false;
			key = candidate;
			return true;
		};
		for (const auto& entry : entries) if (matches(entry.first)) return true;
		for (const Work& work : queue) if (matches(work.request.cacheKey)) return true;
		for (const Completion& completion : completed) {
			if (completion.image && matches(completion.image->cacheKey)) return true;
		}
		for (const DisplayImageFailureInfo& failure : failedCompletions) {
			if (matches(failure.cacheKey)) return true;
		}
		for (const DisplayImageCacheKey& candidate : inFlightKeys) if (matches(candidate)) return true;
		return false;
	}

	void Run() {
		// Keep speculative scaling below the UI and decoder threads in scheduler
		// priority while still allowing several independent frames to use CPUs.
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
		for (;;) {
			Work work;
			{
				std::unique_lock<std::mutex> lock(mutex);
				workAvailable.wait(lock, [this] {
					return stopping || HasStartableWork();
				});
				if (stopping) return;
				auto selected = FindStartableWork();
				if (selected == queue.end()) continue;
				work = std::move(*selected);
				queue.erase(selected);
				queuedKeys.erase(work.request.cacheKey);
				Reservation reservation;
				if (IsSpeculativeDisplayWork(work.foreground, work.request.workClass)) {
					if (!EstimateDisplayBytes(work.request, reservation.bytes) ||
						!CanReserveSpeculative(reservation.bytes)) {
						// Queue selection and reservation share this lock. This protects
						// against changes to the admission limits without preparing pixels.
						queue.push_front(std::move(work));
						queuedKeys.insert(queue.front().request.cacheKey);
						continue;
					}
					reservation.speculative = true;
					ReserveSpeculative(reservation.bytes);
				}
				inFlightReservations[work.request.cacheKey] = reservation;
				inFlightKeys.insert(work.request.cacheKey);
				inFlightCancellation[work.request.cacheKey] = work.request.cancellation;
				inFlightWorkClasses[work.request.cacheKey] = work.request.workClass;
				inFlightPriorities[work.request.cacheKey] =
					work.foreground ? 0 : work.request.priority;
				++activeWorkers;
			}

			PerfContextScope context(work.request.workClass, PerfExecution::WorkerThread);
			const bool cancelledBeforeOpen = work.request.cancellation &&
				work.request.cancellation->load();
			const auto cancellation = work.request.cancellation;
			const auto previousContinue = work.request.workContext.shouldContinue;
			const SourceWorkPriority sourcePriority = work.foreground ||
				work.request.workClass == PerfWorkClass::ActiveImageSpread ?
				SourceWorkPriority::Foreground : SourceWorkPriority::Speculative;
			work.request.workContext = MakeWorkContext(work.request.source,
				sourcePriority,
				[cancellation, previousContinue] {
					if (cancellation && cancellation->load()) return false;
					if (!previousContinue) return true;
					try {
						return previousContinue();
					} catch (...) {
						return false;
					}
				});
			work.request.workContext.currentPriority = [this,
				cacheKey = work.request.cacheKey, sourcePriority] {
				std::lock_guard<std::mutex> lock(mutex);
				if (foregroundKeys.find(cacheKey) != foregroundKeys.end()) {
					return SourceWorkPriority::Foreground;
				}
				const auto activeClass = inFlightWorkClasses.find(cacheKey);
				return activeClass != inFlightWorkClasses.end() &&
					activeClass->second == PerfWorkClass::ActiveImageSpread ?
					SourceWorkPriority::Foreground : sourcePriority;
			};
			ScopedWorkContext activeContext(work.request.workContext);
			bool sourceCurrentBeforeOpen = false;
			bool sourceCheckSucceededBeforeOpen = false;
			ImagePtr image;
			WorkerFailure workerFailure;
			if (!cancelledBeforeOpen) {
				try {
					sourceCheckSucceededBeforeOpen = ValidateSourceWithAdmission(
						work.request.source, work.request.workContext,
						sourceCurrentBeforeOpen);
					if (!sourceCheckSucceededBeforeOpen) {
						workerFailure = {WorkerFailureKind::Cancelled,
							"display source validation was cancelled"};
					}
				} catch (const std::exception& error) {
					workerFailure = {WorkerFailureKind::Exception, error.what()};
				} catch (...) {
					workerFailure = {WorkerFailureKind::Exception,
						"unknown display source validation failure"};
				}
			}
			if (cancelledBeforeOpen) {
				workerFailure = {WorkerFailureKind::Cancelled,
					"display preparation was cancelled"};
			} else if (sourceCheckSucceededBeforeOpen && !sourceCurrentBeforeOpen) {
				workerFailure = {WorkerFailureKind::SourceUnavailable,
					"display source changed before preparation"};
			}
			if (sourceCheckSucceededBeforeOpen && sourceCurrentBeforeOpen &&
				!workerFailure.Failed()) {
				try {
					image = processor(work.request);
				} catch (const std::exception& error) {
					image.reset();
					workerFailure = {WorkerFailureKind::Exception, error.what()};
				} catch (...) {
					image.reset();
					workerFailure = {WorkerFailureKind::Exception,
						"unknown display preparation failure"};
				}
			}
			const bool cancelledAfterWork =
				(work.request.cancellation && work.request.cancellation->load()) ||
				!work.request.workContext.Continue();
			bool sourceCurrentBeforePublish = false;
			bool sourceCheckSucceededBeforePublish = false;
			if (!cancelledAfterWork && !workerFailure.Failed()) {
				try {
					sourceCheckSucceededBeforePublish = ValidateSourceWithAdmission(
						work.request.source, work.request.workContext,
						sourceCurrentBeforePublish);
					if (!sourceCheckSucceededBeforePublish) {
						workerFailure = {WorkerFailureKind::Cancelled,
							"display source validation was cancelled"};
					}
				} catch (const std::exception& error) {
					workerFailure = {WorkerFailureKind::Exception, error.what()};
				} catch (...) {
					workerFailure = {WorkerFailureKind::Exception,
						"unknown display source validation failure"};
				}
			}
			if (cancelledAfterWork) {
				workerFailure = {WorkerFailureKind::Cancelled,
					"display preparation was cancelled"};
			} else if (sourceCheckSucceededBeforePublish && !sourceCurrentBeforePublish) {
				workerFailure = {WorkerFailureKind::SourceUnavailable,
					"display source changed while it was being prepared"};
			} else if (!image && !workerFailure.Failed()) {
				workerFailure = {WorkerFailureKind::ProcessingFailed,
					"display preparation produced no image"};
			}
			const bool sourceChanged = !cancelledBeforeOpen &&
				((sourceCheckSucceededBeforeOpen && !sourceCurrentBeforeOpen) ||
					(!cancelledAfterWork && sourceCheckSucceededBeforePublish &&
						!sourceCurrentBeforePublish));
			SourceDescriptor changedSource;
			if (sourceChanged && workerFailure.kind != WorkerFailureKind::Exception) {
				try {
					changedSource = DescribeCurrentSourceWithAdmission(
						work.request.source.LogicalPath(), work.request.workContext);
				} catch (const std::exception& error) {
					workerFailure = {WorkerFailureKind::Exception, error.what()};
				} catch (...) {
					workerFailure = {WorkerFailureKind::Exception,
						"unknown display source refresh failure"};
				}
				image.reset();
			}
			const SourceChangeNotice sourceChange{
				work.request.source.Key(), changedSource};
			const DisplayImageCacheKey currentKey = MakeCacheKey(work.request.source,
				work.request.frameIndex, work.request.targetWidth,
				work.request.targetHeight, work.request.autoContrast,
				work.request.processing, work.request.rotationQuarterTurns,
				work.request.includeSpectrum);

			bool publishedCompletion = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (sourceChanged) {
					const auto existing = std::find_if(changedSources.begin(), changedSources.end(),
					[&sourceChange](const SourceChangeNotice& notice) {
							return notice.previous == sourceChange.previous;
						});
					if (existing == changedSources.end()) changedSources.push_back(sourceChange);
					else *existing = sourceChange;
					publishedCompletion = true;
				}
				inFlightKeys.erase(work.request.cacheKey);
				inFlightPriorities.erase(work.request.cacheKey);
				inFlightCancellation.erase(work.request.cacheKey);
				if (workerFailure.Failed()) lastWorkerFailure = workerFailure;
				Reservation reservation;
				const auto activeReservation = inFlightReservations.find(work.request.cacheKey);
				if (activeReservation != inFlightReservations.end()) {
					reservation = activeReservation->second;
					inFlightReservations.erase(activeReservation);
				}
				PerfWorkClass effectiveWorkClass = work.request.workClass;
				const auto activeWorkClass = inFlightWorkClasses.find(work.request.cacheKey);
				if (activeWorkClass != inFlightWorkClasses.end()) {
					effectiveWorkClass = activeWorkClass->second;
					inFlightWorkClasses.erase(activeWorkClass);
				}
				std::uint64_t selectionGeneration = work.request.selectionGeneration;
				const auto activeSelectionGeneration =
					inFlightSelectionGenerations.find(work.request.cacheKey);
				if (activeSelectionGeneration != inFlightSelectionGenerations.end()) {
					selectionGeneration = activeSelectionGeneration->second;
					inFlightSelectionGenerations.erase(activeSelectionGeneration);
				}
				--activeWorkers;
				const bool foreground = work.foreground ||
					foregroundKeys.erase(work.request.cacheKey) != 0;
				const bool stillCurrentForeground = !foreground ||
					work.request.cacheKey == latestForegroundKey;
				const auto desiredPriority = desiredPrefetchPriorities.find(work.request.cacheKey);
				const std::size_t completionPriority = foreground ? 0 :
					(desiredPriority == desiredPrefetchPriorities.end() ? work.request.priority :
						desiredPriority->second);
				bool publish = !stopping && sourceCurrentBeforePublish && stillCurrentForeground &&
					work.epoch == epoch && image && image->cacheKey == currentKey &&
					(foreground || desiredPrefetchKeys.find(work.request.cacheKey) !=
						desiredPrefetchKeys.end());
				const bool speculative = publish && IsSpeculativeDisplayWork(foreground,
					effectiveWorkClass);
				if (reservation.speculative && !speculative) {
					ReleaseSpeculative(reservation.bytes);
					reservation = {};
				} else if (speculative && !reservation.speculative) {
					reservation.bytes = PreparedDisplayImageBytes(*image);
					if (!CanReserveSpeculative(reservation.bytes)) {
						publish = false;
					} else {
						ReserveSpeculative(reservation.bytes);
						reservation.speculative = true;
					}
				} else if (speculative && reservation.speculative) {
					const std::size_t actualBytes = PreparedDisplayImageBytes(*image);
					if (actualBytes > reservation.bytes) {
						const std::size_t extra = actualBytes - reservation.bytes;
						if (extra > DisplayImageCache::kMaximumSpeculativeCompletionBytes -
							speculativeReservedBytes ||
							actualBytes > DisplayImageCache::kMaximumSpeculativeCompletionBytes) {
							publish = false;
						} else {
							speculativeReservedBytes += extra;
							reservation.bytes = actualBytes;
						}
					}
				}
			if (publish) {
					const CacheProtectionTier protection =
						CacheProtectionForWorkClass(effectiveWorkClass, foreground);
					const bool retained = Insert(image,
						foreground || protection != CacheProtectionTier::DistantSpeculation,
						protection);
					CacheReservation temporaryReservation;
					if (!retained && sharedBudget) {
						temporaryReservation = sharedBudget->TrackTemporary(
							PreparedDisplayImageBytes(*image),
							CacheMemoryCategory::ActiveWorkingData,
							PreparedAllocationIdentity(image));
					}
					completed.push_back({image, completionPriority, effectiveWorkClass,
						selectionGeneration, reservation.speculative, reservation.bytes,
						std::move(temporaryReservation)});
					publishedCompletion = true;
				} else {
					if (reservation.speculative) ReleaseSpeculative(reservation.bytes);
					if (image) {
					if (!stopping) {
						RecordDisplayCancellation(work.request.workClass,
							PerfExecution::WorkerThread);
					}
					QueueRetirement(std::move(image));
					}
					if (!stopping && foreground && stillCurrentForeground &&
						work.epoch == epoch && workerFailure.Failed() &&
						workerFailure.kind != WorkerFailureKind::Cancelled) {
						failedCompletions.erase(std::remove_if(failedCompletions.begin(),
							failedCompletions.end(), [&work](
								const DisplayImageFailureInfo& failure) {
								return failure.key == work.request.key;
							}), failedCompletions.end());
						if (failedCompletions.size() >= 8) failedCompletions.pop_front();
						failedCompletions.push_back({work.request.cacheKey,
							work.request.key, workerFailure, completionPriority,
							effectiveWorkClass, selectionGeneration});
						publishedCompletion = true;
					}
				}
				const auto retry = pendingForegroundRetries.find(work.request.cacheKey);
				if (!stopping && !publish && workerFailure.kind == WorkerFailureKind::Cancelled &&
					foreground && stillCurrentForeground && work.epoch == epoch &&
					retry != pendingForegroundRetries.end() &&
					queuedKeys.find(work.request.cacheKey) == queuedKeys.end()) {
					Work retryWork = std::move(retry->second);
					retryWork.epoch = epoch;
					retryWork.foreground = true;
					retryWork.request.workClass = effectiveWorkClass;
					retryWork.request.selectionGeneration = selectionGeneration;
					retryWork.request.cancellation =
						std::make_shared<std::atomic<bool>>(false);
					queue.push_front(std::move(retryWork));
					queuedKeys.insert(work.request.cacheKey);
					pendingForegroundRetries.erase(retry);
				} else if (retry != pendingForegroundRetries.end() &&
					(publish || workerFailure.kind != WorkerFailureKind::Cancelled ||
						!stillCurrentForeground || work.epoch != epoch)) {
					pendingForegroundRetries.erase(retry);
				}
				FillPendingCachedCompletions();
				idle.notify_all();
				workAvailable.notify_all();
			}
			if (publishedCompletion) UiCompletionWakeup().Notify();
		}
	}

	static void Retire(const std::shared_ptr<RetirementState>& state) {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
		for (;;) {
			ImagePtr image;
			std::shared_ptr<const DecodedImage> decoded;
			CacheReservation reservation;
			{
				std::unique_lock<std::mutex> lock(state->mutex);
				auto readyDisplay = std::find_if(state->retired.begin(),
					state->retired.end(), [](const RetirementState::RetiredImage& candidate) {
						return candidate.image && candidate.image.use_count() == 1;
					});
				auto readyDecoded = std::find_if(state->retiredDecoded.begin(),
					state->retiredDecoded.end(), [](const auto& candidate) {
						return candidate.use_count() == 1;
					});
				if (readyDisplay == state->retired.end() &&
					readyDecoded == state->retiredDecoded.end()) {
					if (state->retired.empty() && state->retiredDecoded.empty()) {
						if (state->stopping) return;
						state->available.wait(lock, [&state] {
							return state->stopping || !state->retired.empty() ||
								!state->retiredDecoded.empty();
						});
					} else {
						state->available.wait_for(lock, std::chrono::milliseconds(2));
					}
					continue;
				}
				if (readyDisplay != state->retired.end()) {
					image = std::move(readyDisplay->image);
					reservation = std::move(readyDisplay->reservation);
					state->retired.erase(readyDisplay);
					state->retiringOwner = image;
					state->retiringBytes = image ? PreparedDisplayImageBytes(*image) : 0;
					state->retiringDisplayImage = true;
				} else {
					decoded = std::move(*readyDecoded);
					state->retiredDecoded.erase(readyDecoded);
					state->retiringOwner = decoded;
					state->retiringBytes = 0;
					state->retiringDisplayImage = false;
				}
				state->retiring = true;
			}
			image.reset();
			decoded.reset();
			reservation.Reset();
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				state->retiringOwner.reset();
				state->retiring = false;
				state->retiringBytes = 0;
				state->retiringDisplayImage = false;
			}
		}
	}

	const std::size_t byteBudget;
	Processor processor;
	std::shared_ptr<SharedCacheBudget> sharedBudget;
	const bool diagnosticsEnabled;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable idle;
	std::vector<std::thread> workers;
	std::thread retirementWorker;
	std::shared_ptr<RetirementState> retirementState;
	EntryMap entries;
	std::array<LruList, 3> lru;
	std::deque<Work> queue;
	std::deque<Completion> completed;
	std::deque<DisplayImageFailureInfo> failedCompletions;
	std::unordered_map<DisplayImageCacheKey, PendingCachedCompletion,
		DisplayImageCacheKeyHash> pendingCachedCompletions;
	std::vector<SourceChangeNotice> changedSources;
	std::unordered_map<const PreparedDisplayImage*, BorrowedAllocation> borrowedAllocations;
	std::unordered_set<DisplayImageCacheKey, DisplayImageCacheKeyHash> queuedKeys;
	std::unordered_set<DisplayImageCacheKey, DisplayImageCacheKeyHash> inFlightKeys;
	std::unordered_map<DisplayImageCacheKey, std::size_t,
		DisplayImageCacheKeyHash> inFlightPriorities;
	std::unordered_map<DisplayImageCacheKey, Reservation,
		DisplayImageCacheKeyHash> inFlightReservations;
	std::unordered_map<DisplayImageCacheKey, std::weak_ptr<std::atomic<bool>>,
		DisplayImageCacheKeyHash> inFlightCancellation;
	std::unordered_map<DisplayImageCacheKey, PerfWorkClass,
		DisplayImageCacheKeyHash> inFlightWorkClasses;
	std::unordered_map<DisplayImageCacheKey, std::uint64_t,
		DisplayImageCacheKeyHash> inFlightSelectionGenerations;
	std::unordered_map<DisplayImageCacheKey, Work, DisplayImageCacheKeyHash>
		pendingForegroundRetries;
	std::unordered_set<DisplayImageCacheKey, DisplayImageCacheKeyHash> desiredPrefetchKeys;
	std::unordered_map<DisplayImageCacheKey, std::size_t,
		DisplayImageCacheKeyHash> desiredPrefetchPriorities;
	std::unordered_set<DisplayImageCacheKey, DisplayImageCacheKeyHash> foregroundKeys;
	DisplayImageCacheKey latestForegroundKey;
	std::size_t cachedBytes = 0;
	std::size_t activeWorkers = 0;
	WorkerFailure lastWorkerFailure;
	std::size_t speculativeReservedBytes = 0;
	std::size_t speculativeReservedImages = 0;
	std::uint64_t generation = 0;
	std::uint64_t epoch = 0;
	std::size_t borrowedBytes = 0;
	bool stopping = false;
};

DisplayImageCache::DisplayImageCache(std::size_t byteBudget,
	std::size_t workerCount, Processor processor,
	std::shared_ptr<SharedCacheBudget> sharedBudget)
	: impl_(std::make_unique<Impl>(byteBudget, workerCount, std::move(processor),
		std::move(sharedBudget))) {}

DisplayImageCache::~DisplayImageCache() = default;

DisplayImageCache::ImagePtr DisplayImageCache::Find(const DisplayImageRequest& request) {
	if (!request.Valid()) return {};
	if (request.cacheKey != MakeCacheKey(request.source, request.frameIndex,
		request.targetWidth, request.targetHeight, request.autoContrast,
		request.processing, request.rotationQuarterTurns, request.includeSpectrum) ||
		request.key != SerializeCacheKey(request.cacheKey)) return {};
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto found = impl_->entries.find(request.cacheKey);
	if (found != impl_->entries.end()) {
		impl_->Touch(found);
		return found->second.image;
	}
	const auto completed = std::find_if(impl_->completed.begin(), impl_->completed.end(),
		[&request](const Impl::Completion& completion) {
			return completion.image && completion.image->cacheKey == request.cacheKey;
		});
	return completed == impl_->completed.end() ? ImagePtr{} : completed->image;
}

bool DisplayImageCache::HasPendingOrCached(const std::string& key) const {
	if (key.empty()) return false;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	DisplayImageCacheKey cacheKey;
	if (!impl_->ResolveExternalKey(key, cacheKey)) return false;
	return impl_->entries.find(cacheKey) != impl_->entries.end() ||
		impl_->queuedKeys.find(cacheKey) != impl_->queuedKeys.end() ||
		impl_->inFlightKeys.find(cacheKey) != impl_->inFlightKeys.end() ||
		std::any_of(impl_->completed.begin(), impl_->completed.end(),
			[&cacheKey](const Impl::Completion& completion) {
				return completion.image && completion.image->cacheKey == cacheKey;
			}) ||
		std::any_of(impl_->failedCompletions.begin(), impl_->failedCompletions.end(),
			[&key](const DisplayImageFailureInfo& completion) {
				return completion.key == key;
			});
}

void DisplayImageCache::Request(const DisplayImageRequest& request) {
	if (!request.Valid() || request.cacheKey != MakeCacheKey(request.source,
		request.frameIndex, request.targetWidth, request.targetHeight,
		request.autoContrast, request.processing, request.rotationQuarterTurns,
		request.includeSpectrum) ||
		request.key != SerializeCacheKey(request.cacheKey)) return;
	const DisplayImageCacheKey& cacheKey = request.cacheKey;
	DisplayImageRequest foregroundRequest = request;
	if (!foregroundRequest.cancellation) {
		foregroundRequest.cancellation = std::make_shared<std::atomic<bool>>(false);
	} else {
		foregroundRequest.cancellation->store(false);
	}
	const PerfWorkClass foregroundWorkClass = request.workClass == PerfWorkClass::Unspecified ?
		PerfWorkClass::ActiveImageSpread : request.workClass;
	bool removedQueuedForeground = false;
	bool hasQueuedWork = false;
	bool wakeWorkers = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		const bool startableBefore = impl_->HasStartableWork();
		const std::size_t reservedImagesBefore = impl_->speculativeReservedImages;
		const std::size_t reservedBytesBefore = impl_->speculativeReservedBytes;
		auto failed = std::find_if(impl_->failedCompletions.begin(),
			impl_->failedCompletions.end(), [&request](
				const DisplayImageFailureInfo& completion) {
				return completion.key == request.key;
			});
		if (failed != impl_->failedCompletions.end()) {
			if (request.selectionGeneration != 0 &&
				failed->selectionGeneration == request.selectionGeneration) return;
			impl_->failedCompletions.erase(failed);
		}
		if (impl_->latestForegroundKey != cacheKey) {
			impl_->latestForegroundKey = cacheKey;
			for (auto retry = impl_->pendingForegroundRetries.begin();
				retry != impl_->pendingForegroundRetries.end();) {
				if (retry->first == cacheKey) ++retry;
				else retry = impl_->pendingForegroundRetries.erase(retry);
			}
			for (auto queued = impl_->queue.begin(); queued != impl_->queue.end();) {
				if (queued->foreground && queued->request.cacheKey != cacheKey) {
					RecordDisplayCancellation(queued->request.workClass);
					impl_->QueueDecodedRetirement(std::move(queued->request.decoded));
					impl_->queuedKeys.erase(queued->request.cacheKey);
					impl_->foregroundKeys.erase(queued->request.cacheKey);
					queued = impl_->queue.erase(queued);
					removedQueuedForeground = true;
				} else {
					++queued;
				}
			}
			for (auto completed = impl_->completed.begin(); completed != impl_->completed.end();) {
				if (completed->priority == 0 && completed->image && completed->image->cacheKey != cacheKey) {
					impl_->ReleaseCompletionReservation(*completed);
					impl_->QueueRetirement(std::move(completed->image),
						std::move(completed->temporaryReservation));
					completed = impl_->completed.erase(completed);
				} else {
					++completed;
				}
			}
		}
		const auto completed = std::find_if(impl_->completed.begin(), impl_->completed.end(),
			[&cacheKey](const Impl::Completion& completion) {
				return completion.image && completion.image->cacheKey == cacheKey;
			});
		if (completed != impl_->completed.end()) {
			impl_->pendingForegroundRetries.erase(cacheKey);
			impl_->PromoteCompletion(*completed, foregroundWorkClass);
			completed->selectionGeneration = foregroundRequest.selectionGeneration;
			impl_->pendingCachedCompletions.erase(cacheKey);
		} else if (const auto cached = impl_->entries.find(cacheKey); cached != impl_->entries.end()) {
			impl_->pendingForegroundRetries.erase(cacheKey);
			// A speculative frame can become the foreground between worker
			// completion and renderer upload. Promote the shared completion object
			// so it cannot wait behind any neighboring frame.
			impl_->pendingCachedCompletions.erase(cacheKey);
		impl_->completed.push_front({cached->second.image, 0,
			foregroundWorkClass, foregroundRequest.selectionGeneration,
			false, 0, {}});
		} else if (impl_->inFlightKeys.find(cacheKey) != impl_->inFlightKeys.end()) {
			impl_->foregroundKeys.insert(cacheKey);
			impl_->inFlightPriorities[cacheKey] = 0;
			impl_->inFlightWorkClasses[cacheKey] = foregroundWorkClass;
			impl_->inFlightSelectionGenerations[cacheKey] =
				foregroundRequest.selectionGeneration;
			const auto reservation = impl_->inFlightReservations.find(cacheKey);
			if (reservation != impl_->inFlightReservations.end() &&
				reservation->second.speculative) {
				impl_->ReleaseSpeculative(reservation->second.bytes);
				reservation->second = {};
			}
			// Keep a fresh foreground request in case the active processor has
			// already observed cancellation and cannot be revived in place.
			impl_->pendingForegroundRetries[cacheKey] = Impl::Work{
				foregroundRequest, 0, impl_->epoch, true};
		} else if (impl_->queuedKeys.find(cacheKey) != impl_->queuedKeys.end()) {
			impl_->pendingForegroundRetries.erase(cacheKey);
			const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
				[&cacheKey](const Impl::Work& work) { return work.request.cacheKey == cacheKey; });
			if (queued != impl_->queue.end()) {
				Impl::Work promoted = std::move(*queued);
				impl_->queue.erase(queued);
				if (promoted.request.cancellation) promoted.request.cancellation->store(false);
				promoted.foreground = true;
				promoted.epoch = impl_->epoch;
				promoted.request.workClass = foregroundWorkClass;
				promoted.request.selectionGeneration =
					foregroundRequest.selectionGeneration;
				promoted.request.cancellation = foregroundRequest.cancellation;
				impl_->queue.push_front(std::move(promoted));
				hasQueuedWork = true;
			}
		} else {
			impl_->pendingForegroundRetries.erase(cacheKey);
			foregroundRequest.workClass = foregroundWorkClass;
			impl_->queue.push_front(Impl::Work{
				std::move(foregroundRequest), 0, impl_->epoch, true});
			impl_->queuedKeys.insert(cacheKey);
			hasQueuedWork = true;
		}
		impl_->FillPendingCachedCompletions();
		wakeWorkers = hasQueuedWork ||
			impl_->speculativeReservedImages < reservedImagesBefore ||
			impl_->speculativeReservedBytes < reservedBytesBefore ||
			(!startableBefore && impl_->HasStartableWork());
	}
	if (removedQueuedForeground) impl_->idle.notify_all();
	if (wakeWorkers) impl_->workAvailable.notify_all();
}

void DisplayImageCache::RequestBackground(const DisplayImageRequest& request) {
	RequestBackgroundBatch({request});
}

void DisplayImageCache::RequestBackgroundBatch(
	const std::vector<DisplayImageRequest>& requests) {
	bool queuedWork = false;
	bool wakeWorkers = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		const bool startableBefore = impl_->HasStartableWork();
		const std::size_t reservedImagesBefore = impl_->speculativeReservedImages;
		const std::size_t reservedBytesBefore = impl_->speculativeReservedBytes;
		for (const DisplayImageRequest& request : requests) {
			if (!request.Valid() || request.cacheKey != MakeCacheKey(request.source,
				request.frameIndex, request.targetWidth, request.targetHeight,
				request.autoContrast, request.processing, request.rotationQuarterTurns,
				request.includeSpectrum) ||
				request.key != SerializeCacheKey(request.cacheKey)) continue;
			const DisplayImageCacheKey& cacheKey = request.cacheKey;
			DisplayImageRequest classifiedRequest = request;
			if (!classifiedRequest.cancellation) {
				classifiedRequest.cancellation = std::make_shared<std::atomic<bool>>(false);
			} else {
				classifiedRequest.cancellation->store(false);
			}
			const std::size_t priority = std::max<std::size_t>(1, request.priority);
			if (classifiedRequest.workClass == PerfWorkClass::Unspecified) {
				classifiedRequest.workClass = priority <= 2 ?
					PerfWorkClass::NearestNavigationNeighbor : PerfWorkClass::DistantSpeculation;
			}
			std::size_t estimatedBytes = 0;
			if (IsSpeculativeDisplayWork(false, classifiedRequest.workClass) &&
				(!EstimateDisplayBytes(classifiedRequest, estimatedBytes) ||
					estimatedBytes > DisplayImageCache::kMaximumSpeculativeCompletionBytes)) {
				continue;
			}
			impl_->pendingCachedCompletions.erase(cacheKey);
			impl_->desiredPrefetchKeys.insert(cacheKey);
			auto desiredPriority = impl_->desiredPrefetchPriorities.emplace(cacheKey, priority);
			if (!desiredPriority.second) desiredPriority.first->second =
				std::min(desiredPriority.first->second, priority);

		const auto cached = impl_->entries.find(cacheKey);
		if (cached != impl_->entries.end()) {
				impl_->Touch(cached);
				impl_->SetProtection(cached,
					CacheProtectionForWorkClass(classifiedRequest.workClass));
				impl_->QueueCachedCompletion(cached->second.image, priority,
					classifiedRequest.workClass);
				continue;
			}

			const auto completion = std::find_if(impl_->completed.begin(), impl_->completed.end(),
				[&cacheKey](const Impl::Completion& candidate) {
					return candidate.image && candidate.image->cacheKey == cacheKey;
			});
			if (completion != impl_->completed.end()) {
				ImagePtr completedImage = completion->image;
				impl_->QueueCachedCompletion(completedImage, priority,
					classifiedRequest.workClass);
				continue;
			}

			const auto inFlight = impl_->inFlightPriorities.find(cacheKey);
			if (inFlight != impl_->inFlightPriorities.end()) {
				if (priority <= inFlight->second) {
					impl_->inFlightWorkClasses[cacheKey] = classifiedRequest.workClass;
				}
				inFlight->second = std::min(inFlight->second, priority);
				const auto reservation = impl_->inFlightReservations.find(cacheKey);
				if (reservation != impl_->inFlightReservations.end() &&
					reservation->second.speculative &&
					classifiedRequest.workClass == PerfWorkClass::ActiveImageSpread) {
					impl_->ReleaseSpeculative(reservation->second.bytes);
					reservation->second = {};
				}
				const auto cancellation = impl_->inFlightCancellation.find(cacheKey);
				const bool hasForegroundRetry =
					impl_->pendingForegroundRetries.find(cacheKey) !=
						impl_->pendingForegroundRetries.end();
				if (!hasForegroundRetry && cancellation != impl_->inFlightCancellation.end()) {
					if (const auto token = cancellation->second.lock()) token->store(false);
				}
				continue;
			}

			const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
				[&cacheKey](const Impl::Work& work) { return work.request.cacheKey == cacheKey; });
			if (queued != impl_->queue.end()) {
				if (queued->foreground) continue;
				Impl::Work work = std::move(*queued);
				impl_->queue.erase(queued);
				if (work.request.cancellation) work.request.cancellation->store(false);
				if (priority <= work.request.priority) {
					work.request.workClass = classifiedRequest.workClass;
				}
				work.request.priority = std::min(work.request.priority, priority);
				auto insertion = std::find_if(impl_->queue.begin(), impl_->queue.end(),
					[&work](const Impl::Work& candidate) {
						return !candidate.foreground &&
							candidate.request.priority > work.request.priority;
					});
				impl_->queue.insert(insertion, std::move(work));
				queuedWork = true;
				continue;
			}

			Impl::Work work{classifiedRequest, impl_->generation, impl_->epoch, false};
			work.request = classifiedRequest;
			work.request.priority = priority;
			auto insertion = std::find_if(impl_->queue.begin(), impl_->queue.end(),
				[&work](const Impl::Work& candidate) {
					return !candidate.foreground &&
						candidate.request.priority > work.request.priority;
				});
			impl_->queue.insert(insertion, std::move(work));
			impl_->queuedKeys.insert(cacheKey);
			queuedWork = true;
		}
		impl_->FillPendingCachedCompletions();
		wakeWorkers = queuedWork ||
			impl_->speculativeReservedImages < reservedImagesBefore ||
			impl_->speculativeReservedBytes < reservedBytesBefore ||
			(!startableBefore && impl_->HasStartableWork());
	}
	if (wakeWorkers) impl_->workAvailable.notify_all();
}

void DisplayImageCache::CancelBackground(const std::string& key) {
	if (key.empty()) return;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	DisplayImageCacheKey cacheKey;
	if (!impl_->ResolveExternalKey(key, cacheKey)) return;
	const auto activePriority = impl_->inFlightPriorities.find(cacheKey);
	const bool foreground = impl_->foregroundKeys.find(cacheKey) != impl_->foregroundKeys.end() ||
		(activePriority != impl_->inFlightPriorities.end() && activePriority->second == 0);
	impl_->desiredPrefetchKeys.erase(cacheKey);
	impl_->desiredPrefetchPriorities.erase(cacheKey);
	impl_->pendingCachedCompletions.erase(cacheKey);
	if (!foreground) {
		const auto active = impl_->inFlightCancellation.find(cacheKey);
		if (active != impl_->inFlightCancellation.end()) {
			if (const auto cancellation = active->second.lock()) cancellation->store(true);
		}
		impl_->inFlightWorkClasses.erase(cacheKey);
	}
	for (auto queued = impl_->queue.begin(); queued != impl_->queue.end();) {
		if (!queued->foreground && queued->request.cacheKey == cacheKey) {
			RecordDisplayCancellation(queued->request.workClass);
			impl_->QueueDecodedRetirement(std::move(queued->request.decoded));
			impl_->queuedKeys.erase(cacheKey);
			queued = impl_->queue.erase(queued);
		} else {
			++queued;
		}
	}
	for (auto completed = impl_->completed.begin(); completed != impl_->completed.end();) {
		if (completed->priority != 0 && completed->image && completed->image->cacheKey == cacheKey) {
			impl_->ReleaseCompletionReservation(*completed);
			impl_->QueueRetirement(std::move(completed->image),
				std::move(completed->temporaryReservation));
			completed = impl_->completed.erase(completed);
		} else {
			++completed;
		}
	}
	impl_->FillPendingCachedCompletions();
	impl_->idle.notify_all();
	impl_->workAvailable.notify_all();
}

DisplayImageCache::ImagePtr DisplayImageCache::RequestAndWait(
	const DisplayImageRequest& request) {
	if (!request.Valid()) return {};
	Request(request);
	const DisplayImageCacheKey& cacheKey = request.cacheKey;
	std::unique_lock<std::mutex> lock(impl_->mutex);
	const auto findCompleted = [&]() -> ImagePtr {
		const auto cached = impl_->entries.find(cacheKey);
		if (cached != impl_->entries.end()) {
			impl_->Touch(cached);
			return cached->second.image;
		}
		const auto completed = std::find_if(impl_->completed.begin(), impl_->completed.end(),
			[&cacheKey](const Impl::Completion& completion) {
				return completion.image && completion.image->cacheKey == cacheKey;
			});
		return completed == impl_->completed.end() ? ImagePtr{} : completed->image;
	};
	if (ImagePtr ready = findCompleted()) return ready;
	impl_->idle.wait(lock, [&] {
		if (impl_->stopping || impl_->entries.find(cacheKey) != impl_->entries.end()) return true;
		if (std::any_of(impl_->completed.begin(), impl_->completed.end(),
			[&cacheKey](const Impl::Completion& completion) {
				return completion.image && completion.image->cacheKey == cacheKey;
			})) return true;
		return impl_->queuedKeys.find(cacheKey) == impl_->queuedKeys.end() &&
			impl_->inFlightKeys.find(cacheKey) == impl_->inFlightKeys.end();
	});
	return findCompleted();
}

void DisplayImageCache::Prefetch(const std::vector<DisplayImageRequest>& requests) {
	std::vector<DisplayImageRequest> prioritizedRequests = requests;
	for (DisplayImageRequest& request : prioritizedRequests) {
		if (!request.cancellation || request.cancellation->load()) {
			request.cancellation = std::make_shared<std::atomic<bool>>(false);
		}
		if (request.workClass == PerfWorkClass::Unspecified) {
			request.workClass = request.priority <= 2 ?
				PerfWorkClass::NearestNavigationNeighbor : PerfWorkClass::DistantSpeculation;
		}
	}
	prioritizedRequests.erase(std::remove_if(prioritizedRequests.begin(),
		prioritizedRequests.end(), [](const DisplayImageRequest& request) {
			if (!request.Valid()) return false;
			std::size_t bytes = 0;
			return IsSpeculativeDisplayWork(false, request.workClass) &&
				(!EstimateDisplayBytes(request, bytes) || bytes >
					DisplayImageCache::kMaximumSpeculativeCompletionBytes);
		}), prioritizedRequests.end());
	std::stable_sort(prioritizedRequests.begin(), prioritizedRequests.end(),
		[](const DisplayImageRequest& left, const DisplayImageRequest& right) {
			return left.priority < right.priority;
		});
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		const auto previouslyDesiredKeys = impl_->desiredPrefetchKeys;
		const auto previouslyDesiredPriorities = impl_->desiredPrefetchPriorities;
		++impl_->generation;
		std::unordered_set<DisplayImageCacheKey, DisplayImageCacheKeyHash> requestedKeys;
		for (const DisplayImageRequest& request : prioritizedRequests) {
			if (request.Valid()) requestedKeys.insert(request.cacheKey);
		}
		for (auto completed = impl_->completed.begin(); completed != impl_->completed.end();) {
			const bool stillDesiredSpread = completed->image &&
				completed->workClass == PerfWorkClass::ActiveImageSpread &&
				previouslyDesiredKeys.find(completed->image->cacheKey) !=
					previouslyDesiredKeys.end();
			const bool stillRequested = completed->image &&
				requestedKeys.find(completed->image->cacheKey) != requestedKeys.end();
			if (completed->priority == 0 || stillDesiredSpread || stillRequested) {
				++completed;
				continue;
			}
			impl_->ReleaseCompletionReservation(*completed);
			impl_->QueueRetirement(std::move(completed->image),
				std::move(completed->temporaryReservation));
			completed = impl_->completed.erase(completed);
		}
		for (auto queued = impl_->queue.begin(); queued != impl_->queue.end();) {
			const bool stillDesiredSpread =
				queued->request.workClass == PerfWorkClass::ActiveImageSpread &&
				previouslyDesiredKeys.find(queued->request.cacheKey) !=
					previouslyDesiredKeys.end();
			if (!queued->foreground && !stillDesiredSpread) {
				RecordDisplayCancellation(queued->request.workClass);
				impl_->QueueDecodedRetirement(std::move(queued->request.decoded));
				queued = impl_->queue.erase(queued);
			} else {
				++queued;
			}
		}
		impl_->queuedKeys.clear();
		impl_->desiredPrefetchKeys.clear();
		impl_->desiredPrefetchPriorities.clear();
		for (auto pending = impl_->pendingCachedCompletions.begin();
			pending != impl_->pendingCachedCompletions.end();) {
			if (pending->second.workClass == PerfWorkClass::ActiveImageSpread &&
				previouslyDesiredKeys.find(pending->first) != previouslyDesiredKeys.end()) {
				impl_->desiredPrefetchKeys.insert(pending->first);
				impl_->desiredPrefetchPriorities[pending->first] = pending->second.priority;
				++pending;
			} else {
				pending = impl_->pendingCachedCompletions.erase(pending);
			}
		}
		for (const auto& active : impl_->inFlightCancellation) {
			const auto priority = impl_->inFlightPriorities.find(active.first);
			const auto workClass = impl_->inFlightWorkClasses.find(active.first);
			const bool keepForeground = impl_->foregroundKeys.find(active.first) !=
				impl_->foregroundKeys.end() || (priority != impl_->inFlightPriorities.end() &&
				priority->second == 0);
			const bool keepActiveSpread = workClass != impl_->inFlightWorkClasses.end() &&
				workClass->second == PerfWorkClass::ActiveImageSpread &&
				previouslyDesiredKeys.find(active.first) != previouslyDesiredKeys.end();
			if (keepForeground || keepActiveSpread) {
				impl_->desiredPrefetchKeys.insert(active.first);
				impl_->desiredPrefetchPriorities[active.first] = keepForeground ? 0 :
					(priority == impl_->inFlightPriorities.end() ? 1 : priority->second);
				const bool hasForegroundRetry =
					impl_->pendingForegroundRetries.find(active.first) !=
						impl_->pendingForegroundRetries.end();
				// Foreground promotion leaves a fresh request behind when the active
				// speculative generation may already have observed cancellation. Do not
				// revive that generation while preserving it through a prefetch update.
				if (!hasForegroundRetry) {
					if (const auto cancellation = active.second.lock()) cancellation->store(false);
				}
				continue;
			}
			const bool remainsDesired = std::any_of(prioritizedRequests.begin(),
				prioritizedRequests.end(), [&active](const DisplayImageRequest& request) {
					return request.cacheKey == active.first;
				});
			if (const auto cancellation = active.second.lock()) {
				cancellation->store(!remainsDesired);
			}
		}
		for (const Impl::Completion& completion : impl_->completed) {
			if (!completion.image || completion.priority == 0 ||
				completion.workClass != PerfWorkClass::ActiveImageSpread ||
				previouslyDesiredKeys.find(completion.image->cacheKey) ==
					previouslyDesiredKeys.end()) continue;
			impl_->desiredPrefetchKeys.insert(completion.image->cacheKey);
			const auto priority = previouslyDesiredPriorities.find(completion.image->cacheKey);
			impl_->desiredPrefetchPriorities[completion.image->cacheKey] =
				priority == previouslyDesiredPriorities.end() ? completion.priority :
					priority->second;
		}
		for (const Impl::Work& work : impl_->queue) {
			impl_->queuedKeys.insert(work.request.cacheKey);
			if (work.foreground || work.request.workClass == PerfWorkClass::ActiveImageSpread) {
				impl_->desiredPrefetchKeys.insert(work.request.cacheKey);
				impl_->desiredPrefetchPriorities[work.request.cacheKey] = work.foreground ? 0 :
					std::max<std::size_t>(1, work.request.priority);
			}
		}
		for (const DisplayImageRequest& request : prioritizedRequests) {
			if (!request.Valid() || request.cacheKey != MakeCacheKey(request.source,
				request.frameIndex, request.targetWidth, request.targetHeight,
				request.autoContrast, request.processing, request.rotationQuarterTurns,
				request.includeSpectrum) ||
				request.key != SerializeCacheKey(request.cacheKey)) continue;
			const DisplayImageCacheKey& cacheKey = request.cacheKey;
			const std::size_t requestPriority = std::max<std::size_t>(1, request.priority);
			impl_->desiredPrefetchKeys.insert(cacheKey);
			impl_->desiredPrefetchPriorities[cacheKey] = requestPriority;
			const auto retained = impl_->entries.find(cacheKey);
			if (retained != impl_->entries.end()) {
				impl_->QueueCachedCompletion(retained->second.image, requestPriority,
					request.workClass, Impl::CachedCompletionPriorityMode::ReplaceBackground);
				continue;
			}
			const auto completion = std::find_if(impl_->completed.begin(),
				impl_->completed.end(), [&cacheKey](const Impl::Completion& candidate) {
					return candidate.image && candidate.image->cacheKey == cacheKey;
				});
			if (completion != impl_->completed.end()) {
				ImagePtr completedImage = completion->image;
				impl_->QueueCachedCompletion(completedImage, requestPriority,
					request.workClass, Impl::CachedCompletionPriorityMode::ReplaceBackground);
				continue;
			}
			if (impl_->queuedKeys.find(cacheKey) != impl_->queuedKeys.end()) continue;
			if (impl_->inFlightKeys.find(cacheKey) != impl_->inFlightKeys.end()) {
				if (impl_->foregroundKeys.find(cacheKey) == impl_->foregroundKeys.end()) {
					impl_->inFlightPriorities[cacheKey] = std::min(
						impl_->inFlightPriorities[cacheKey], requestPriority);
					impl_->inFlightWorkClasses[cacheKey] = request.workClass;
					const auto reservation = impl_->inFlightReservations.find(cacheKey);
					if (reservation != impl_->inFlightReservations.end() &&
						reservation->second.speculative &&
						request.workClass == PerfWorkClass::ActiveImageSpread) {
						impl_->ReleaseSpeculative(reservation->second.bytes);
						reservation->second = {};
					}
				}
				continue;
			}
			DisplayImageRequest queuedRequest = request;
			queuedRequest.priority = requestPriority;
			impl_->queue.push_back(Impl::Work{queuedRequest, impl_->generation, impl_->epoch, false});
			impl_->queuedKeys.insert(cacheKey);
		}
		impl_->FillPendingCachedCompletions();
		impl_->idle.notify_all();
	}
	impl_->workAvailable.notify_all();
}

std::vector<DisplayImageCache::ImagePtr> DisplayImageCache::TakeCompleted(std::size_t maximumCount) {
	static const std::set<PerfWorkClass> allWorkClasses{
		PerfWorkClass::Unspecified,
		PerfWorkClass::ActiveImageSpread,
		PerfWorkClass::FocusedPreview,
		PerfWorkClass::VisibleThumbnail,
		PerfWorkClass::NearestNavigationNeighbor,
		PerfWorkClass::DistantSpeculation};
	return TakeCompleted(maximumCount, allWorkClasses);
}

std::vector<DisplayImageCache::ImagePtr> DisplayImageCache::TakeCompleted(
	std::size_t maximumCount, const std::set<PerfWorkClass>& permittedWorkClasses) {
	std::vector<ImagePtr> images;
	std::vector<DisplayImageCompletionInfo> completions = TakeCompletedWithMetadata(
		maximumCount, permittedWorkClasses);
	images.reserve(completions.size());
	for (DisplayImageCompletionInfo& completion : completions) {
		images.push_back(std::move(completion.image));
	}
	return images;
}

std::vector<DisplayImageCompletionInfo>
DisplayImageCache::TakeCompletedWithMetadata(std::size_t maximumCount) {
	static const std::set<PerfWorkClass> allWorkClasses{
		PerfWorkClass::Unspecified,
		PerfWorkClass::ActiveImageSpread,
		PerfWorkClass::FocusedPreview,
		PerfWorkClass::VisibleThumbnail,
		PerfWorkClass::NearestNavigationNeighbor,
		PerfWorkClass::DistantSpeculation};
	return TakeCompletedWithMetadata(maximumCount, allWorkClasses);
}

std::vector<DisplayImageCompletionInfo> DisplayImageCache::TakeCompletedWithMetadata(
	std::size_t maximumCount, const std::set<PerfWorkClass>& permittedWorkClasses) {
	std::vector<DisplayImageCompletionInfo> result;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	result.reserve(std::min(maximumCount, impl_->completed.size()));
	while (result.size() < maximumCount && !impl_->completed.empty()) {
		auto next = impl_->completed.end();
		for (auto candidate = impl_->completed.begin(); candidate != impl_->completed.end(); ++candidate) {
			if (!candidate->image || (candidate->priority != 0 &&
				permittedWorkClasses.find(candidate->workClass) ==
					permittedWorkClasses.end())) continue;
			if (next == impl_->completed.end() || candidate->priority < next->priority) next = candidate;
		}
		if (next == impl_->completed.end()) break;
		std::size_t outstandingPriority = std::numeric_limits<std::size_t>::max();
		for (const Impl::Work& work : impl_->queue) {
			if (!work.foreground && permittedWorkClasses.find(work.request.workClass) ==
				permittedWorkClasses.end()) continue;
			if (!impl_->CanStart(work)) continue;
			outstandingPriority = std::min(outstandingPriority,
				work.foreground ? 0 : work.request.priority);
		}
		for (const auto& inFlight : impl_->inFlightPriorities) {
			const auto workClass = impl_->inFlightWorkClasses.find(inFlight.first);
			if (inFlight.second != 0 && (workClass == impl_->inFlightWorkClasses.end() ||
				permittedWorkClasses.find(workClass->second) == permittedWorkClasses.end())) continue;
			outstandingPriority = std::min(outstandingPriority, inFlight.second);
		}
		if (outstandingPriority < next->priority) break;
		const std::size_t effectivePriority = next->priority;
		const PerfWorkClass effectiveWorkClass = next->workClass;
		result.push_back({std::move(next->image), effectivePriority,
			effectiveWorkClass, next->selectionGeneration});
		if (result.back().image) {
			CacheReservation temporary = std::move(next->temporaryReservation);
			if (impl_->diagnosticsEnabled || temporary) {
				const std::size_t bytes = PreparedDisplayImageBytes(*result.back().image);
				const auto inserted = impl_->borrowedAllocations.emplace(
					result.back().image.get(), Impl::BorrowedAllocation{bytes, std::move(temporary)});
				if (inserted.second) impl_->borrowedBytes += inserted.first->second.bytes;
			}
		}
		impl_->ReleaseCompletionReservation(*next);
		impl_->completed.erase(next);
	}
	impl_->FillPendingCachedCompletions();
	impl_->workAvailable.notify_all();
	impl_->idle.notify_all();
	return result;
}

std::vector<DisplayImageFailureInfo> DisplayImageCache::TakeFailedCompletions() {
	std::vector<DisplayImageFailureInfo> result;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	result.reserve(impl_->failedCompletions.size());
	while (!impl_->failedCompletions.empty()) {
		result.push_back(std::move(impl_->failedCompletions.front()));
		impl_->failedCompletions.pop_front();
	}
	return result;
}

void DisplayImageCache::Release(const std::string& key) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	DisplayImageCacheKey cacheKey;
	if (impl_->ResolveExternalKey(key, cacheKey)) {
		const auto found = impl_->entries.find(cacheKey);
		if (found != impl_->entries.end()) impl_->Erase(found);
	}
	impl_->workAvailable.notify_one();
}

void DisplayImageCache::ReleaseForUpload(const ImagePtr& image) {
	impl_->ReleaseForUse(image, CacheMemoryCategory::TemporaryUploadStaging);
}

void DisplayImageCache::ReleaseForUpload(const std::vector<ImagePtr>& images) {
	for (const ImagePtr& image : images) ReleaseForUpload(image);
}

void DisplayImageCache::ReleaseForActiveUse(const ImagePtr& image) {
	impl_->ReleaseForUse(image, CacheMemoryCategory::ActiveWorkingData);
}

void DisplayImageCache::SetProtectionSnapshot(
	const std::vector<std::pair<DisplayImageCacheKey, CacheProtectionTier>>& protections) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::unordered_map<DisplayImageCacheKey, CacheProtectionTier,
		DisplayImageCacheKeyHash> strongest;
	for (const auto& protection : protections) {
		auto inserted = strongest.emplace(protection.first, protection.second);
		if (!inserted.second && static_cast<unsigned>(protection.second) >
			static_cast<unsigned>(inserted.first->second)) {
			inserted.first->second = protection.second;
		}
	}
	std::vector<std::pair<DisplayImageCacheKey, CacheProtectionTier>> changes;
	for (std::size_t tier = 0; tier < impl_->lru.size(); ++tier) {
		const std::vector<DisplayImageCacheKey> keys(impl_->lru[tier].begin(),
			impl_->lru[tier].end());
		for (const DisplayImageCacheKey& key : keys) {
			const auto entry = impl_->entries.find(key);
			if (entry == impl_->entries.end()) continue;
			const auto desired = strongest.find(key);
			const CacheProtectionTier protection = desired == strongest.end() ?
				CacheProtectionTier::DistantSpeculation : desired->second;
			if (entry->second.protection != protection) {
				changes.emplace_back(key, protection);
			}
		}
	}
	for (const auto& change : changes) {
		const auto entry = impl_->entries.find(change.first);
		if (entry != impl_->entries.end()) impl_->SetProtection(entry, change.second);
	}
}

std::size_t DisplayImageCache::EvictLeastRecentlyUsed(CacheProtectionTier maximumTier) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto oldest = impl_->Oldest(maximumTier);
	if (oldest == impl_->entries.end()) return 0;
	const std::size_t bytes = impl_->Erase(oldest);
	impl_->workAvailable.notify_one();
	return bytes;
}

void DisplayImageCache::Retire(const ImagePtr& image) {
	if (!image) return;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		CacheReservation borrowed = impl_->ForgetBorrowed(image);
		const auto cached = impl_->entries.find(image->cacheKey);
		const bool stillCached = cached != impl_->entries.end() &&
			cached->second.image.get() == image.get();
		if (!stillCached) impl_->QueueRetirement(image, std::move(borrowed));
	}
}

void DisplayImageCache::Clear() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	++impl_->generation;
	++impl_->epoch;
	for (const auto& active : impl_->inFlightCancellation) {
		if (const auto cancellation = active.second.lock()) cancellation->store(true);
	}
	for (Impl::Work& work : impl_->queue) {
		RecordDisplayCancellation(work.request.workClass);
		impl_->QueueDecodedRetirement(std::move(work.request.decoded));
	}
	impl_->queue.clear();
	while (!impl_->completed.empty()) {
		impl_->ReleaseCompletionReservation(impl_->completed.front());
		impl_->QueueRetirement(std::move(impl_->completed.front().image),
			std::move(impl_->completed.front().temporaryReservation));
		impl_->completed.pop_front();
	}
	impl_->pendingCachedCompletions.clear();
	impl_->pendingForegroundRetries.clear();
	impl_->failedCompletions.clear();
	impl_->queuedKeys.clear();
	impl_->desiredPrefetchKeys.clear();
	impl_->desiredPrefetchPriorities.clear();
		impl_->foregroundKeys.clear();
		impl_->inFlightSelectionGenerations.clear();
	impl_->latestForegroundKey = {};
	while (!impl_->entries.empty()) impl_->Erase(impl_->entries.begin());
	impl_->idle.notify_all();
	impl_->workAvailable.notify_all();
}

std::size_t DisplayImageCache::CachedBytes() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->cachedBytes;
}

std::size_t DisplayImageCache::CachedImages() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->entries.size();
}

DisplayImageCacheDiagnostics DisplayImageCache::GetDiagnostics() const {
	DisplayImageCacheDiagnostics diagnostics;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	diagnostics.cachedBytes = impl_->cachedBytes;
	diagnostics.cachedImages = impl_->entries.size();
	diagnostics.lastWorkerFailure = impl_->lastWorkerFailure;
	diagnostics.borrowedBytes = impl_->borrowedBytes;
	diagnostics.borrowedImages = impl_->borrowedAllocations.size();
	diagnostics.speculativeReservedImages = impl_->speculativeReservedImages;
	diagnostics.speculativeReservedBytes = impl_->speculativeReservedBytes;
	for (const Impl::Work& work : impl_->queue) {
		if (work.foreground) ++diagnostics.foregroundQueued;
		else ++diagnostics.backgroundQueued;
	}
	for (const auto& inFlight : impl_->inFlightPriorities) {
		if (inFlight.second == 0) {
			++diagnostics.foregroundActive;
		} else {
			++diagnostics.backgroundActive;
		}
	}
	diagnostics.preparedImages = impl_->completed.size();
	for (const Impl::Completion& completion : impl_->completed) {
		if (completion.image) {
			const std::size_t bytes = PreparedDisplayImageBytes(*completion.image);
			diagnostics.preparedBytes += bytes;
			if (completion.speculative) {
				++diagnostics.speculativePreparedImages;
				diagnostics.speculativePreparedBytes += bytes;
			}
		}
	}
	std::lock_guard<std::mutex> retirementLock(impl_->retirementState->mutex);
	diagnostics.retiredImages = impl_->retirementState->retired.size();
	for (const Impl::RetirementState::RetiredImage& retired :
		impl_->retirementState->retired) {
		if (retired.image) diagnostics.retiredBytes += PreparedDisplayImageBytes(*retired.image);
	}
	if (impl_->retirementState->retiring &&
		impl_->retirementState->retiringDisplayImage) {
		++diagnostics.retiredImages;
		diagnostics.retiredBytes += impl_->retirementState->retiringBytes;
	}
	diagnostics.activeRetiredBytes = impl_->retirementState->retiringDisplayImage ?
		impl_->retirementState->retiringBytes : 0;
	return diagnostics;
}

std::vector<SourceChangeNotice> DisplayImageCache::TakeChangedSources() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<SourceChangeNotice> changed;
	changed.swap(impl_->changedSources);
	return changed;
}

bool DisplayImageCache::HasPendingWork() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return !impl_->queue.empty() || impl_->activeWorkers != 0 || !impl_->completed.empty();
}

bool DisplayImageCache::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(impl_->mutex);
	return impl_->idle.wait_for(lock, timeout, [this] {
		return impl_->queue.empty() && impl_->activeWorkers == 0;
	});
}

DisplayImageCompletionBatch::DisplayImageCompletionBatch(DisplayImageCache& cache,
	std::vector<ImagePtr> images)
	: cache_(cache) {
	completions_.reserve(images.size());
	for (ImagePtr& image : images) {
		completions_.push_back({std::move(image), 0,
			PerfWorkClass::Unspecified});
	}
}

DisplayImageCompletionBatch::DisplayImageCompletionBatch(DisplayImageCache& cache,
	std::vector<CompletionInfo> completions)
	: cache_(cache), completions_(std::move(completions)) {}

DisplayImageCompletionBatch::~DisplayImageCompletionBatch() {
	for (const CompletionInfo& completion : completions_) {
		if (completion.image) cache_.Retire(completion.image);
	}
}

std::size_t DisplayImageCompletionBatch::Size() const {
	return completions_.size();
}

const DisplayImageCompletionBatch::ImagePtr& DisplayImageCompletionBatch::At(
	std::size_t index) const {
	return completions_.at(index).image;
}

const DisplayImageCompletionBatch::CompletionInfo&
DisplayImageCompletionBatch::CompletionAt(std::size_t index) const {
	return completions_.at(index);
}

DisplayImageCompletionBatch::ImagePtr DisplayImageCompletionBatch::Take(
	std::size_t index) {
	return std::move(completions_.at(index).image);
}

DisplayImageCompletionBatch::CompletionInfo
DisplayImageCompletionBatch::TakeCompletion(std::size_t index) {
	return std::move(completions_.at(index));
}

} // namespace jpegview_linux
