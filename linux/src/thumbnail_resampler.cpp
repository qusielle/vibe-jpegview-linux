#include "thumbnail_resampler.h"

#include "thumbnail_panel_model.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <utility>

namespace jpegview_linux {

bool DownsampleThumbnailBgra(const std::vector<std::uint8_t>& source,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	std::vector<std::uint8_t>& target) {
	if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0 ||
		targetWidth > sourceWidth || targetHeight > sourceHeight) return false;
	const std::size_t maximum = std::numeric_limits<std::size_t>::max();
	const std::size_t sourcePixels = static_cast<std::size_t>(sourceWidth) * sourceHeight;
	const std::size_t targetPixels = static_cast<std::size_t>(targetWidth) * targetHeight;
	if (sourcePixels > maximum / 4 || targetPixels > maximum / 4 || source.size() < sourcePixels * 4) {
		return false;
	}
	try {
		if (sourceWidth == targetWidth && sourceHeight == targetHeight) {
			target.assign(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(sourcePixels * 4));
			return true;
		}
		target.assign(targetPixels * 4, 0);
	} catch (const std::exception&) {
		return false;
	}

	const double horizontalScale = static_cast<double>(sourceWidth) / targetWidth;
	const double verticalScale = static_cast<double>(sourceHeight) / targetHeight;
	const double coveredArea = horizontalScale * verticalScale;
	for (int targetY = 0; targetY < targetHeight; ++targetY) {
		const double sourceTop = targetY * verticalScale;
		const double sourceBottom = (targetY + 1) * verticalScale;
		const int firstY = static_cast<int>(std::floor(sourceTop));
		const int lastY = std::min(sourceHeight - 1,
			static_cast<int>(std::ceil(sourceBottom)) - 1);
		for (int targetX = 0; targetX < targetWidth; ++targetX) {
			const double sourceLeft = targetX * horizontalScale;
			const double sourceRight = (targetX + 1) * horizontalScale;
			const int firstX = static_cast<int>(std::floor(sourceLeft));
			const int lastX = std::min(sourceWidth - 1,
				static_cast<int>(std::ceil(sourceRight)) - 1);
			double weightedAlpha = 0.0;
			double weightedBlue = 0.0;
			double weightedGreen = 0.0;
			double weightedRed = 0.0;
			for (int sourceY = firstY; sourceY <= lastY; ++sourceY) {
				const double verticalCoverage = std::max(0.0,
					std::min(sourceBottom, sourceY + 1.0) - std::max(sourceTop, static_cast<double>(sourceY)));
				for (int sourceX = firstX; sourceX <= lastX; ++sourceX) {
					const double horizontalCoverage = std::max(0.0,
						std::min(sourceRight, sourceX + 1.0) - std::max(sourceLeft, static_cast<double>(sourceX)));
					const double coverage = horizontalCoverage * verticalCoverage;
					const std::size_t offset =
						(static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4;
					const double alpha = source[offset + 3];
					const double alphaCoverage = alpha * coverage;
					weightedAlpha += alphaCoverage;
					weightedBlue += source[offset] * alphaCoverage;
					weightedGreen += source[offset + 1] * alphaCoverage;
					weightedRed += source[offset + 2] * alphaCoverage;
				}
			}
			const std::size_t output =
				(static_cast<std::size_t>(targetY) * targetWidth + targetX) * 4;
			target[output + 3] = static_cast<std::uint8_t>(std::clamp(
				std::lround(weightedAlpha / coveredArea), 0l, 255l));
			if (weightedAlpha > 0.0) {
				target[output] = static_cast<std::uint8_t>(std::clamp(
					std::lround(weightedBlue / weightedAlpha), 0l, 255l));
				target[output + 1] = static_cast<std::uint8_t>(std::clamp(
					std::lround(weightedGreen / weightedAlpha), 0l, 255l));
				target[output + 2] = static_cast<std::uint8_t>(std::clamp(
					std::lround(weightedRed / weightedAlpha), 0l, 255l));
			}
		}
	}
	return true;
}

bool CanReuseDisplayPixelsForThumbnail(int width, int height,
	std::size_t maximumPixels) {
	if (width <= 0 || height <= 0 || maximumPixels == 0) return false;
	const std::size_t sourceWidth = static_cast<std::size_t>(width);
	const std::size_t sourceHeight = static_cast<std::size_t>(height);
	return sourceWidth <= maximumPixels / sourceHeight;
}

bool ThumbnailPreparationRequest::Valid() const {
	return !key.empty() && source && source->width > 0 && source->height > 0 &&
		maximumWidth > 0 && maximumHeight > 0;
}

namespace {

ThumbnailPreparationWorker::ImagePtr PrepareThumbnail(
	const ThumbnailPreparationRequest& request) {
	if (!request.Valid()) return {};
	const ThumbnailSize size = FitThumbnailSize(request.source->width,
		request.source->height, request.maximumWidth, request.maximumHeight);
	if (size.width <= 0 || size.height <= 0) return {};
	auto prepared = std::make_shared<PreparedThumbnailImage>();
	prepared->key = request.key;
	prepared->width = size.width;
	prepared->height = size.height;
	prepared->hasTransparency = request.source->hasTransparency;
	if (!DownsampleThumbnailBgra(request.source->bgra, request.source->width,
		request.source->height, size.width, size.height, prepared->bgra)) return {};
	return prepared;
}

} // namespace

struct ThumbnailPreparationWorker::Impl {
	struct Work {
		ThumbnailPreparationRequest request;
		std::uint64_t generation = 0;
	};

	explicit Impl(Processor prepare) : processor(std::move(prepare)), worker([this] { Run(); }) {
		if (!processor) processor = PrepareThumbnail;
	}

	~Impl() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			++generation;
		}
		workAvailable.notify_one();
		if (worker.joinable()) worker.join();
	}

	void Run() {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
		for (;;) {
			Work work;
			DisplayImageCache::ImagePtr retiredSource;
			{
				std::unique_lock<std::mutex> lock(mutex);
				workAvailable.wait(lock, [this] {
					return stopping || !queue.empty() || !retired.empty();
				});
				if (stopping) {
					queue.clear();
					retired.clear();
					completed.clear();
					return;
				}
				if (!retired.empty()) {
					retiredSource = std::move(retired.front());
					retired.pop_front();
				}
				if (queue.empty()) continue;
				const auto nearest = std::min_element(queue.begin(), queue.end(),
					[](const Work& left, const Work& right) {
						return left.request.priority < right.request.priority;
					});
				work = std::move(*nearest);
				queue.erase(nearest);
				inFlightKeys.insert(work.request.key);
				++activeWorkers;
			}

			ImagePtr image = processor(work.request);
			retiredSource.reset();
			{
				std::lock_guard<std::mutex> lock(mutex);
				inFlightKeys.erase(work.request.key);
				--activeWorkers;
				if (!stopping && work.generation == generation && image &&
					image->key == work.request.key) completed.push_back(std::move(image));
				idle.notify_all();
			}
		}
	}

	static constexpr std::size_t kMaximumQueuedSources = 2;
	Processor processor;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable idle;
	std::deque<Work> queue;
	std::deque<ImagePtr> completed;
	std::deque<DisplayImageCache::ImagePtr> retired;
	std::unordered_set<std::string> inFlightKeys;
	std::size_t activeWorkers = 0;
	std::uint64_t generation = 0;
	bool stopping = false;
	std::thread worker;
};

ThumbnailPreparationWorker::ThumbnailPreparationWorker(Processor processor)
	: impl_(std::make_unique<Impl>(std::move(processor))) {}

ThumbnailPreparationWorker::~ThumbnailPreparationWorker() = default;

bool ThumbnailPreparationWorker::Request(const ThumbnailPreparationRequest& request) {
	if (!request.Valid()) return false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->stopping || impl_->inFlightKeys.find(request.key) != impl_->inFlightKeys.end() ||
			std::any_of(impl_->completed.begin(), impl_->completed.end(),
				[&request](const ImagePtr& image) { return image && image->key == request.key; })) {
			return false;
		}
		const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
			[&request](const Impl::Work& work) { return work.request.key == request.key; });
		if (queued != impl_->queue.end()) {
			queued->request.priority = request.priority;
			return true;
		}
		if (impl_->queue.size() >= Impl::kMaximumQueuedSources) {
			const auto farthest = std::max_element(impl_->queue.begin(), impl_->queue.end(),
				[](const Impl::Work& left, const Impl::Work& right) {
					return left.request.priority < right.request.priority;
				});
			if (farthest == impl_->queue.end() ||
				farthest->request.priority <= request.priority) return false;
			impl_->retired.push_back(std::move(farthest->request.source));
			impl_->queue.erase(farthest);
		}
		impl_->queue.push_back({request, impl_->generation});
	}
	impl_->workAvailable.notify_one();
	return true;
}

std::vector<ThumbnailPreparationWorker::ImagePtr>
ThumbnailPreparationWorker::TakeCompleted(std::size_t maximumCount) {
	std::vector<ImagePtr> images;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	images.reserve(std::min(maximumCount, impl_->completed.size()));
	while (images.size() < maximumCount && !impl_->completed.empty()) {
		images.push_back(std::move(impl_->completed.front()));
		impl_->completed.pop_front();
	}
	impl_->idle.notify_all();
	return images;
}

void ThumbnailPreparationWorker::Clear() {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		++impl_->generation;
		while (!impl_->queue.empty()) {
			impl_->retired.push_back(std::move(impl_->queue.front().request.source));
			impl_->queue.pop_front();
		}
		impl_->completed.clear();
		impl_->idle.notify_all();
	}
	impl_->workAvailable.notify_one();
}

bool ThumbnailPreparationWorker::HasPendingWork() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return !impl_->queue.empty() || impl_->activeWorkers != 0 || !impl_->completed.empty();
}

bool ThumbnailPreparationWorker::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(impl_->mutex);
	return impl_->idle.wait_for(lock, timeout, [this] {
		return impl_->queue.empty() && impl_->activeWorkers == 0;
	});
}

} // namespace jpegview_linux
