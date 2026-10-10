#include "test_harness.h"
#include "test_support.h"

namespace {

void TestDecodedImageCacheAndBackgroundPrefetch() {
	Expect(jpegview_linux::ImagePrefetchOrder(5, 2, 1, 4) ==
		std::vector<std::size_t>({3, 1, 4, 0}),
		"forward prefetch did not alternate nearest neighbors in the preferred direction");
	Expect(jpegview_linux::ImagePrefetchOrder(5, 2, -1, 4) ==
		std::vector<std::size_t>({1, 3, 0, 4}),
		"backward prefetch did not prioritize the previous image");
	Expect(jpegview_linux::ImagePrefetchOrder(5, 0, 1, 4) ==
		std::vector<std::size_t>({1, 4, 2, 3}),
		"prefetch order did not include folder-loop wraparound");
	Expect(jpegview_linux::ImagePrefetchOrder(0, 0, 1, 4).empty() &&
		jpegview_linux::ImagePrefetchOrder(1, 0, 1, 4).empty(),
		"prefetch order produced work without neighboring files");
	Expect(jpegview_linux::ImagePrefetchOrder(100000000, 50000000, 1, 4) ==
		std::vector<std::size_t>{50000001, 49999999, 50000002, 49999998},
		"bounded prefetch ordering scaled its allocation with the full catalog size");

	TemporaryDirectory temporary;
	std::vector<fs::path> files;
	for (int index = 0; index < 5; ++index) {
		const fs::path filename = temporary.path() / ("image-" + std::to_string(index));
		WriteText(filename, "source-" + std::to_string(index));
		files.push_back(filename);
	}

	jpegview_linux::DecodedImageCache lru(32);
	lru.Store(files[0], CachedTestImage(16));
	lru.Store(files[1], CachedTestImage(16));
	Expect(lru.CachedBytes() == 32 && lru.CachedImages() == 2,
		"decoded cache did not account for retained BGRA memory");
	lru.SetProtectionSnapshot({
		{jpegview_linux::DescribeImageSource(files[0]).Key(),
			jpegview_linux::CacheProtectionTier::DistantSpeculation},
		{jpegview_linux::DescribeImageSource(files[1]).Key(),
			jpegview_linux::CacheProtectionTier::DistantSpeculation}});
	Expect(lru.Find(files[0]) != nullptr, "decoded cache missed a retained image");
	lru.Store(files[2], CachedTestImage(16));
	Expect(lru.Find(files[0]) != nullptr && lru.Find(files[1]) == nullptr &&
		lru.Find(files[2]) != nullptr && lru.CachedBytes() == 32,
		"decoded cache did not evict the least recently used image at its byte limit");
	lru.Store(files[3], CachedTestImage(36));
	Expect(lru.Find(files[3]) == nullptr && lru.CachedBytes() == 32,
		"decoded cache retained an image larger than its complete byte budget");
	WriteText(files[0], "changed-source-with-a-different-size");
	Expect(lru.Find(files[0]) == nullptr && lru.CachedBytes() == 16,
		"decoded cache served pixel data after the source file changed");
	lru.Clear();
	Expect(lru.CachedBytes() == 0 && lru.CachedImages() == 0,
		"decoded cache clear retained entries or byte accounting");

	std::mutex decodedOrderMutex;
	std::vector<std::string> decodedOrder;
	std::vector<std::string> completedOrder;
	std::atomic<bool> missingCompletionPixels{false};
	jpegview_linux::DecodedImageCache background(64,
		[&](const fs::path& filename, DecodedImage& image, std::string&) {
			{
				std::lock_guard<std::mutex> lock(decodedOrderMutex);
				decodedOrder.push_back(filename.filename().string());
			}
			image = *CachedTestImage(4);
			return true;
		});
	background.Prefetch(files, 2, 1, 4,
		[&](const fs::path& filename, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			if (!image) missingCompletionPixels = true;
			std::lock_guard<std::mutex> lock(decodedOrderMutex);
			completedOrder.push_back(filename.filename().string());
		});
	Expect(background.WaitUntilIdle(std::chrono::seconds(2)),
		"background image prefetch did not finish");
	{
		std::lock_guard<std::mutex> lock(decodedOrderMutex);
		Expect(decodedOrder == std::vector<std::string>({
			"image-3", "image-1", "image-4", "image-0"}),
			"background decoder did not follow nearest-first prefetch order");
		Expect(completedOrder == decodedOrder,
			"decoded prefetch completion did not preserve nearest-first order");
	}
	Expect(!missingCompletionPixels, "decoded prefetch callback received no pixels");
	Expect(background.CachedImages() == 4 && background.CachedBytes() == 16,
		"background decoder did not retain completed images in the shared cache");
	std::mutex decodeFailureMutex;
	std::condition_variable decodeFailureChanged;
	bool failedDecodeReported = false;
	jpegview_linux::DecodedImageCache decodeFailure(64,
		[](const fs::path&, DecodedImage&, std::string& error) {
			error = "fixture decode failure";
			return false;
	});
	decodeFailure.Prefetch(files, 2, 1, 1,
		[&](const fs::path& filename,
			const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			{
				std::lock_guard<std::mutex> lock(decodeFailureMutex);
				failedDecodeReported = filename == files[3] && !image;
			}
			decodeFailureChanged.notify_all();
		});
	{
		std::unique_lock<std::mutex> lock(decodeFailureMutex);
		Expect(decodeFailureChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return failedDecodeReported; }),
		"decoded prefetch did not report a current-generation failure to its owner");
	}
	Expect(decodeFailure.WaitUntilIdle(std::chrono::seconds(2)),
		"failed decoded prefetch did not become idle");

	std::atomic<int> throwingDecoderCalls{0};
	std::atomic<int> throwingDecoderCompletions{0};
	std::atomic<int> throwingDecoderNullCompletions{0};
	std::mutex throwingDecoderCompletionMutex;
	std::condition_variable throwingDecoderCompletionChanged;
	jpegview_linux::DecodedImageCache throwingDecoder(64,
		[&](const fs::path&, DecodedImage& image, std::string&) {
			if (throwingDecoderCalls.fetch_add(1) == 0) {
				throw std::runtime_error("injected decoder exception");
			}
			image = *CachedTestImage(4);
			return true;
		});
	throwingDecoder.Prefetch(files, 2, 1, 2,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			{
				std::lock_guard<std::mutex> lock(throwingDecoderCompletionMutex);
				throwingDecoderCompletions.fetch_add(1);
				if (!image) throwingDecoderNullCompletions.fetch_add(1);
			}
			throwingDecoderCompletionChanged.notify_all();
		});
	const bool throwingDecoderIdle =
		throwingDecoder.WaitUntilIdle(std::chrono::seconds(2));
	bool throwingDecoderCallbacksCompleted = false;
	{
		std::unique_lock<std::mutex> lock(throwingDecoderCompletionMutex);
		throwingDecoderCallbacksCompleted = throwingDecoderCompletionChanged.wait_for(lock,
			std::chrono::seconds(2), [&] {
				return throwingDecoderCompletions.load() == 2;
			});
	}
	Expect(throwingDecoderIdle && throwingDecoderCallbacksCompleted &&
		throwingDecoderCalls.load() == 2 && throwingDecoderCompletions.load() == 2 &&
		throwingDecoderNullCompletions.load() == 1 && throwingDecoder.CachedImages() == 1 &&
		throwingDecoder.GetDiagnostics().lastWorkerFailure.kind ==
			jpegview_linux::WorkerFailureKind::Exception,
		"decoder exceptions did not produce a structured failure while keeping the worker alive");

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	const bool coordinatorWasIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingForeground == 0 &&
				snapshot.waitingSpeculative == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(3));
	std::atomic<int> admissionDecoderCalls{0};
	std::atomic<int> admissionCallbacks{0};
	std::atomic<int> missingAdmissionPixels{0};
	std::mutex admissionCallbackMutex;
	std::condition_variable admissionCallbackChanged;
	jpegview_linux::DecodedImageCache admissionFailure(64,
		[&](const fs::path&, DecodedImage& image, std::string&) {
			++admissionDecoderCalls;
			image = *CachedTestImage(4);
			return true;
		});
	SourceWorkFailureInjection admissionInjection{
		jpegview_linux::detail::SourceWorkTestHookPoint::ActiveCpuRegistration};
	coordinator.SetTestHookForTesting(ThrowAtSourceWorkHook, &admissionInjection);
	admissionFailure.RequestBackground(files[0],
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			{
				std::lock_guard<std::mutex> lock(admissionCallbackMutex);
				++admissionCallbacks;
				if (!image) ++missingAdmissionPixels;
			}
			admissionCallbackChanged.notify_all();
		});
	const bool admissionFailureIdle = admissionFailure.WaitUntilIdle(std::chrono::seconds(3));
	coordinator.SetTestHookForTesting(nullptr, nullptr);
	const bool admissionFailureReported = admissionFailure.GetDiagnostics().lastWorkerFailure.kind ==
		jpegview_linux::WorkerFailureKind::Exception;
	const int admissionDecoderCallsAfterFailure = admissionDecoderCalls.load();
	std::atomic<int> retryCallbacks{0};
	std::atomic<bool> retryProducedPixels{false};
	admissionFailure.RequestBackground(files[1],
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			{
				std::lock_guard<std::mutex> lock(admissionCallbackMutex);
				++retryCallbacks;
				retryProducedPixels.store(static_cast<bool>(image));
			}
			admissionCallbackChanged.notify_all();
		});
	const bool admissionRetryIdle = admissionFailure.WaitUntilIdle(std::chrono::seconds(3));
	bool bothCallbacksCompleted = false;
	{
		std::unique_lock<std::mutex> lock(admissionCallbackMutex);
		bothCallbacksCompleted = admissionCallbackChanged.wait_for(lock,
			std::chrono::seconds(3), [&] {
				return admissionCallbacks.load() == 1 && retryCallbacks.load() == 1;
			});
	}
	const auto coordinatorAfterRetry = coordinator.Snapshot();
	std::ostringstream admissionFailureMessage;
	admissionFailureMessage << "decoded-cache admission exception escaped its worker or blocked a later valid request "
		"(idle/globalIdle/reported/injected/callbacks/missing/decodeCalls/retryCallbacks/retryPixels/firstDecodeCalls="
		<< admissionFailureIdle << '/' << coordinatorWasIdle << '/' << admissionFailureReported << '/'
		<< admissionInjection.fired.load() << '/' << admissionCallbacks.load() << '/'
		<< missingAdmissionPixels.load() << '/' << admissionDecoderCalls.load() << '/'
		<< retryCallbacks.load() << '/' << retryProducedPixels.load() << '/'
		<< admissionDecoderCallsAfterFailure << ", permits="
		<< coordinatorAfterRetry.activeForeground << '/' << coordinatorAfterRetry.activeSpeculative
		<< '/' << coordinatorAfterRetry.activeCpu << '/' << coordinatorAfterRetry.waitingForeground
		<< '/' << coordinatorAfterRetry.waitingSpeculative << '/' << coordinatorAfterRetry.waitingCpu
		<< ')';
	Expect(coordinatorWasIdle && admissionFailureIdle && admissionFailureReported &&
		admissionInjection.fired.load() && admissionCallbacks.load() == 1 &&
		missingAdmissionPixels.load() == 1 && admissionDecoderCallsAfterFailure == 0 &&
		admissionRetryIdle && bothCallbacksCompleted && retryCallbacks.load() == 1 &&
		retryProducedPixels.load() &&
		admissionDecoderCalls.load() == 1 && coordinatorAfterRetry.activeForeground == 0 &&
		coordinatorAfterRetry.activeSpeculative == 0 && coordinatorAfterRetry.activeCpu == 0 &&
		coordinatorAfterRetry.waitingForeground == 0 &&
		coordinatorAfterRetry.waitingSpeculative == 0 && coordinatorAfterRetry.waitingCpu == 0,
		admissionFailureMessage.str());

	std::atomic<int> speculativeDecodes{0};
	jpegview_linux::DecodedImageCache fullCache(16,
		[&](const fs::path&, DecodedImage& image, std::string&) {
			++speculativeDecodes;
			image = *CachedTestImage(4);
			return true;
		});
	fullCache.Store(files[2], CachedTestImage(16));
	fullCache.Prefetch(files, 2, 1, 4);
	Expect(fullCache.WaitUntilIdle(std::chrono::seconds(2)),
		"full background image cache did not become idle");
	Expect(speculativeDecodes == 1 && fullCache.CachedImages() == 1 &&
		fullCache.Find(files[2]) != nullptr,
		"speculative decoding evicted an image retained from foreground viewing");

	std::mutex reprioritizeMutex;
	std::condition_variable reprioritizeChanged;
	bool firstDecodeStarted = false;
	bool releaseFirstDecode = false;
	std::atomic<int> firstCallbacks{0};
	std::atomic<int> latestCallbacks{0};
	std::atomic<int> retainedDecodeCount{0};
	jpegview_linux::DecodedImageCache reprioritized(64,
		[&](const fs::path& filename, DecodedImage& image, std::string&) {
			if (filename == files[3]) {
				++retainedDecodeCount;
				std::unique_lock<std::mutex> lock(reprioritizeMutex);
				firstDecodeStarted = true;
				reprioritizeChanged.notify_all();
				reprioritizeChanged.wait(lock, [&] { return releaseFirstDecode; });
			}
			image = *CachedTestImage(4);
			return true;
		});
	reprioritized.Prefetch(files, 2, 1, 1,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr&) {
			++firstCallbacks;
		});
	{
		std::unique_lock<std::mutex> lock(reprioritizeMutex);
		Expect(reprioritizeChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return firstDecodeStarted; }),
			"decoded prefetch worker did not start the nearest image");
	}
	// The same file is still the nearest neighbor after this direction change.
	// Its in-flight decode must be retained and delivered to the newest batch.
	reprioritized.Prefetch(files, 4, -1, 1,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr&) {
			++latestCallbacks;
		});
	{
		std::lock_guard<std::mutex> lock(reprioritizeMutex);
		releaseFirstDecode = true;
	}
	reprioritizeChanged.notify_all();
	Expect(reprioritized.WaitUntilIdle(std::chrono::seconds(2)),
		"reprioritized decoded prefetch did not finish");
	Expect(retainedDecodeCount == 1 && firstCallbacks == 0 && latestCallbacks == 1 &&
		reprioritized.Find(files[3]) != nullptr,
		"direction change discarded, duplicated, or misdelivered a useful in-flight decode");

	std::mutex obsoleteMutex;
	std::condition_variable obsoleteChanged;
	bool obsoleteDecodeStarted = false;
	bool releaseObsoleteDecode = false;
	std::atomic<int> obsoleteCallbacks{0};
	jpegview_linux::DecodedImageCache obsolete(64,
		[&](const fs::path& filename, DecodedImage& image, std::string&) {
			if (filename == files[3]) {
				std::unique_lock<std::mutex> lock(obsoleteMutex);
				obsoleteDecodeStarted = true;
				obsoleteChanged.notify_all();
				obsoleteChanged.wait(lock, [&] { return releaseObsoleteDecode; });
			}
			image = *CachedTestImage(4);
			return true;
		});
	ScopedConditionRelease obsoleteDecodeRelease(
		obsoleteMutex, obsoleteChanged, releaseObsoleteDecode);
	obsolete.Prefetch(files, 2, 1, 1,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr&) {
			++obsoleteCallbacks;
		});
	{
		std::unique_lock<std::mutex> lock(obsoleteMutex);
		Expect(obsoleteChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return obsoleteDecodeStarted; }),
			"obsolete decoded prefetch did not start its blocked source read");
	}
	obsolete.Prefetch({}, 0, 0, 0);
	{
		std::lock_guard<std::mutex> lock(obsoleteMutex);
		releaseObsoleteDecode = true;
	}
	obsoleteChanged.notify_all();
	Expect(obsolete.WaitUntilIdle(std::chrono::seconds(2)),
		"canceled decoded prefetch did not become idle after its source read returned");
	Expect(obsolete.CachedImages() == 0 && obsoleteCallbacks == 0,
		"canceled active decoded work was cached or published after its batch was replaced");

	std::mutex spreadMutex;
	std::condition_variable spreadChanged;
	const fs::path coldPartner = temporary.path() / "cold-spread-partner.png";
	WriteText(coldPartner, "cold non-JPEG partner source");
	bool spreadBlockerStarted = false;
	bool releaseSpreadBlocker = false;
	bool spreadCallbackCompleted = false;
	std::shared_ptr<const DecodedImage> spreadDecoded;
	jpegview_linux::PerfWorkClass spreadDecodeClass =
		jpegview_linux::PerfWorkClass::Unspecified;
	jpegview_linux::DecodedImageCache activeSpreadDecode(64,
		[&](const fs::path& filename, DecodedImage& image, std::string&) {
			if (filename == files[3]) {
				std::unique_lock<std::mutex> lock(spreadMutex);
				spreadBlockerStarted = true;
				spreadChanged.notify_all();
				spreadChanged.wait(lock, [&] { return releaseSpreadBlocker; });
			} else if (filename == coldPartner) {
				spreadDecodeClass = jpegview_linux::CurrentPerfContext().workClass;
			}
			image = *CachedTestImage(4);
			return true;
		});
	ScopedConditionRelease spreadBlockerRelease(
		spreadMutex, spreadChanged, releaseSpreadBlocker);
	activeSpreadDecode.RequestBackground(files[3], {},
		jpegview_linux::PerfWorkClass::DistantSpeculation);
	bool spreadBlockerReached = false;
	{
		std::unique_lock<std::mutex> lock(spreadMutex);
		spreadBlockerReached = spreadChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return spreadBlockerStarted; });
	}
	activeSpreadDecode.RequestBackground(coldPartner,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& decoded) {
			{
				std::lock_guard<std::mutex> lock(spreadMutex);
				spreadDecoded = decoded;
				spreadCallbackCompleted = true;
			}
			spreadChanged.notify_all();
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	const auto spreadBeforePause = activeSpreadDecode.GetDiagnostics();
	activeSpreadDecode.Prefetch({}, 0, 0, 0);
	const auto spreadDuringPause = activeSpreadDecode.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(spreadMutex);
		releaseSpreadBlocker = true;
	}
	spreadChanged.notify_all();
	bool spreadCallbackReached = false;
	{
		std::unique_lock<std::mutex> lock(spreadMutex);
		spreadCallbackReached = spreadChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return spreadCallbackCompleted; });
	}
	Expect(spreadBlockerReached && spreadCallbackReached && spreadDecoded &&
		spreadBeforePause.activeSpreadQueued == 1 &&
		spreadDuringPause.activeSpreadQueued == 1 &&
		spreadDecodeClass == jpegview_linux::PerfWorkClass::ActiveImageSpread &&
		activeSpreadDecode.Find(coldPartner) != nullptr,
		"cold non-JPEG spread partner decode did not survive paused neighbor prefetch as active-spread work");

	std::mutex joinMutex;
	std::condition_variable joinChanged;
	bool blockerStarted = false;
	bool releaseBlocker = false;
	std::atomic<int> joinedDecodeCount{0};
	jpegview_linux::DecodedImageCache joined(64,
		[&](const fs::path& filename, DecodedImage& image, std::string&) {
			if (filename == files[3]) {
				std::unique_lock<std::mutex> lock(joinMutex);
				blockerStarted = true;
				joinChanged.notify_all();
				joinChanged.wait(lock, [&] { return releaseBlocker; });
			}
			if (filename == files[1]) ++joinedDecodeCount;
			image = *CachedTestImage(4);
			return true;
		});
	joined.Prefetch(files, 2, 1, 2);
	{
		std::unique_lock<std::mutex> lock(joinMutex);
		Expect(joinChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return blockerStarted; }),
			"decoded prefetch blocker did not start");
	}
	jpegview_linux::DecodedImageCache::ImagePtr joinedImage;
	std::thread foreground([&] { joinedImage = joined.FindOrWait(files[1]); });
	{
		std::lock_guard<std::mutex> lock(joinMutex);
		releaseBlocker = true;
	}
	joinChanged.notify_all();
	foreground.join();
	Expect(joinedImage != nullptr && joinedDecodeCount == 1,
		"foreground cache miss did not join its promoted speculative decode");
}

void TestDecodedImageCacheShutdownRejectsNewWork() {
	TemporaryDirectory temporary;
	std::vector<fs::path> files;
	for (int index = 0; index < 3; ++index) {
		const fs::path filename = temporary.path() / ("shutdown-" +
			std::to_string(index) + ".jpg");
		WriteText(filename, "decoded shutdown source");
		files.push_back(filename);
	}
	std::atomic<int> decodeCalls{0};
	std::atomic<int> dimensionCalls{0};
	jpegview_linux::DecodedImageCache cache(128,
		[&](const fs::path&, DecodedImage& image, std::string&) {
			++decodeCalls;
			image = *CachedTestImage(4);
			return true;
		}, {}, 1,
		[&](const fs::path&, int& width, int& height, std::string&) {
			++dimensionCalls;
			width = height = 1;
			return true;
		});
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(files.front());
	cache.Shutdown();
	cache.Shutdown();
	cache.RequestBackground(source, {});
	cache.RequestSelectedSource(source, {});
	cache.RequestJpegDimensions(source,
		[](const fs::path&, bool, int, int) {});
	cache.Prefetch(files, 1, 1, 2);
	cache.Store(source, CachedTestImage(4));
	Expect(cache.WaitUntilIdle(std::chrono::milliseconds(100)) &&
		cache.GetDiagnostics().backgroundQueued == 0 && cache.CachedImages() == 0 &&
		decodeCalls.load() == 0 && dimensionCalls.load() == 0,
		"decoded cache accepted work or retained pixels after terminal shutdown");
}

void TestDecodedImageCacheObserverPreservesSelectedCompletion() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "observed-selected-source.jpg";
	WriteText(filename, "selected source");
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(filename);

	std::mutex mutex;
	std::condition_variable changed;
	bool decoderStarted = false;
	bool releaseDecoder = false;
	int decodeCalls = 0;
	jpegview_linux::DecodedImageCache::ImagePtr selectedImage;
	jpegview_linux::DecodedImageCache::ImagePtr observedImage;
	bool selectedCompleted = false;
	bool observerCompleted = false;
	jpegview_linux::DecodedImageCache cache(64,
		[&](const fs::path&, DecodedImage& image, std::string&) {
			std::unique_lock<std::mutex> lock(mutex);
			++decodeCalls;
			decoderStarted = true;
			changed.notify_all();
			changed.wait(lock, [&] { return releaseDecoder; });
			image = *CachedTestImage(4);
			return true;
		}, {}, 1);
	cache.RequestSelectedSource(source,
		[&](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr& image,
			const jpegview_linux::WorkerFailure&) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				selectedImage = image;
				selectedCompleted = true;
			}
			changed.notify_all();
		});
	bool decoderReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		decoderReachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return decoderStarted; });
	}
	cache.ObserveSelectedSource(source,
		[&](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr& image,
			const jpegview_linux::WorkerFailure&) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				observedImage = image;
				observerCompleted = true;
			}
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::FocusedPreview);
	Expect(cache.GetDiagnostics().activeSpreadActive == 1,
		"focused sampler observer downgraded an in-flight selected decode");
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseDecoder = true;
	}
	changed.notify_all();
	Expect(decoderReachedBarrier,
		"selected source decode did not reach its deterministic barrier");
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return selectedCompleted && observerCompleted; }),
			"selected owner and attached observer did not both receive the decode result");
	}
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)) && decodeCalls == 1 &&
		selectedImage && selectedImage == observedImage,
		"observing a selected decode replaced its owner or started a duplicate decode");

	const auto activeBudget = std::make_shared<jpegview_linux::SharedCacheBudget>(1024);
	jpegview_linux::DecodedImageCache cachedObserverCache(1024, {}, activeBudget, 1);
	cachedObserverCache.Store(source, CachedTestImage(4));
	cachedObserverCache.PromoteToActiveUse(source.Key());
	const jpegview_linux::CacheBudgetSnapshot beforeCachedObserver =
		activeBudget->Snapshot();
	bool cachedObserverCompleted = false;
	cachedObserverCache.ObserveSelectedSource(source,
		[&](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr& image,
			const jpegview_linux::WorkerFailure&) {
			cachedObserverCompleted = image != nullptr;
		}, jpegview_linux::PerfWorkClass::FocusedPreview);
	const jpegview_linux::CacheBudgetSnapshot afterCachedObserver =
		activeBudget->Snapshot();
	Expect(cachedObserverCompleted && beforeCachedObserver.activeWorkingBytes > 0 &&
		afterCachedObserver.activeWorkingBytes == beforeCachedObserver.activeWorkingBytes,
		"cache-hit observer downgraded the active source's memory protection");

	const fs::path blockerPath = temporary.path() / "observer-queue-blocker.jpg";
	const fs::path queuedPath = temporary.path() / "observer-queued-source.jpg";
	WriteText(blockerPath, "blocker source");
	WriteText(queuedPath, "queued source");
	const jpegview_linux::SourceDescriptor blockerSource =
		jpegview_linux::DescribeImageSource(blockerPath);
	const jpegview_linux::SourceDescriptor queuedSource =
		jpegview_linux::DescribeImageSource(queuedPath);
	std::mutex queuedMutex;
	std::condition_variable queuedChanged;
	bool blockerStarted = false;
	bool releaseBlocker = false;
	bool prefetchCompleted = false;
	bool queuedObserverCompleted = false;
	int queuedDecodeCalls = 0;
	jpegview_linux::DecodedImageCache queuedCache(64,
		[&](const fs::path& path, DecodedImage& image, std::string&) {
			std::unique_lock<std::mutex> lock(queuedMutex);
			if (path == blockerPath) {
				blockerStarted = true;
				queuedChanged.notify_all();
				queuedChanged.wait(lock, [&] { return releaseBlocker; });
			} else if (path == queuedPath) {
				++queuedDecodeCalls;
			}
			image = *CachedTestImage(4);
			return true;
		}, {}, 1);
	queuedCache.RequestSelectedSource(blockerSource,
		[](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr&,
			const jpegview_linux::WorkerFailure&) {});
	bool queuedBlockerReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(queuedMutex);
		queuedBlockerReachedBarrier = queuedChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return blockerStarted; });
	}
	queuedCache.Prefetch({blockerPath, queuedPath}, 0, 1, 1,
		[&](const fs::path& path,
			const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			if (path != queuedPath || !image) return;
			{
				std::lock_guard<std::mutex> lock(queuedMutex);
				prefetchCompleted = true;
			}
			queuedChanged.notify_all();
		}, {}, 1);
	queuedCache.ObserveSelectedSource(queuedSource,
		[&](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr& image,
			const jpegview_linux::WorkerFailure&) {
			{
				std::lock_guard<std::mutex> lock(queuedMutex);
				queuedObserverCompleted = image != nullptr;
			}
			queuedChanged.notify_all();
		}, jpegview_linux::PerfWorkClass::FocusedPreview);
	{
		std::lock_guard<std::mutex> lock(queuedMutex);
		releaseBlocker = true;
	}
	queuedChanged.notify_all();
	Expect(queuedBlockerReachedBarrier,
		"queued-source blocker did not reach its deterministic barrier");
	{
		std::unique_lock<std::mutex> lock(queuedMutex);
		Expect(queuedChanged.wait_for(lock, std::chrono::seconds(2), [&] {
			return prefetchCompleted && queuedObserverCompleted;
		}), "queued selected-source observer replaced the prefetch completion");
	}
	Expect(queuedCache.WaitUntilIdle(std::chrono::seconds(2)) && queuedDecodeCalls == 1,
		"queued selected-source observer caused duplicate target decoding");

	const fs::path focusedPath = temporary.path() / "focused-observer.jpg";
	WriteText(focusedPath, "focused observer source");
	const jpegview_linux::SourceDescriptor focusedSource =
		jpegview_linux::DescribeImageSource(focusedPath);
	std::mutex focusedMutex;
	std::condition_variable focusedChanged;
	bool focusedDecodeStarted = false;
	bool releaseFocusedDecode = false;
	bool focusedObserverCompleted = false;
	jpegview_linux::DecodedImageCache focusedCache(64,
		[&](const fs::path&, DecodedImage& image, std::string&) {
			std::unique_lock<std::mutex> lock(focusedMutex);
			focusedDecodeStarted = true;
			focusedChanged.notify_all();
			focusedChanged.wait(lock, [&] { return releaseFocusedDecode; });
			image = *CachedTestImage(4);
			return true;
		}, {}, 1);
	focusedCache.ObserveSelectedSource(focusedSource,
		[&](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr& image,
			const jpegview_linux::WorkerFailure&) {
			{
				std::lock_guard<std::mutex> lock(focusedMutex);
				focusedObserverCompleted = image != nullptr;
			}
			focusedChanged.notify_all();
		}, jpegview_linux::PerfWorkClass::FocusedPreview);
	bool focusedDecodeReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(focusedMutex);
		focusedDecodeReachedBarrier = focusedChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return focusedDecodeStarted; });
	}
	focusedCache.Prefetch({focusedPath}, 0, 1, 0, {}, {}, 2,
		[&focusedSource](std::size_t index) {
			return index == 0 ? &focusedSource : nullptr;
		});
	{
		std::lock_guard<std::mutex> lock(focusedMutex);
		releaseFocusedDecode = true;
	}
	focusedChanged.notify_all();
	Expect(focusedDecodeReachedBarrier,
		"focused-source observer did not reach its decoder barrier");
	{
		std::unique_lock<std::mutex> lock(focusedMutex);
		Expect(focusedChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return focusedObserverCompleted; }),
			"neighbor prefetch refresh discarded a live focused-source observer");
	}
	Expect(focusedCache.WaitUntilIdle(std::chrono::seconds(2)),
		"focused-source observer cache did not become idle after prefetch refresh");
}

void TestDecodedImageCacheFocusedObserverWaitsForQueuedActiveWork() {
	for (const bool joinQueuedWork : {false, true}) {
		TemporaryDirectory temporary;
		const fs::path blocker = temporary.path() / "observer-blocker.png";
		const fs::path active = temporary.path() / "observer-active.png";
		const fs::path focused = temporary.path() / "observer-focused.png";
		for (const fs::path& path : {blocker, active, focused}) WriteText(path, "source");
		std::mutex mutex;
		std::condition_variable changed;
		bool blockerStarted = false;
		bool releaseBlocker = false;
		bool activeCompleted = false;
		bool observerCompleted = false;
		std::vector<fs::path> decodedPaths;
		jpegview_linux::DecodedImageCache cache(4096,
			[&](const fs::path& path, DecodedImage& image, std::string&) {
				std::unique_lock<std::mutex> lock(mutex);
				decodedPaths.push_back(path);
				if (path == blocker) {
					blockerStarted = true;
					changed.notify_all();
					changed.wait(lock, [&] { return releaseBlocker; });
				}
				image = *CachedTestImage(4);
				return true;
			}, {}, 1);
		cache.RequestSelectedSource(jpegview_linux::DescribeImageSource(blocker),
			[](const jpegview_linux::SourceDescriptor&,
				const jpegview_linux::DecodedImageCache::ImagePtr&,
				const jpegview_linux::WorkerFailure&) {});
		bool blockerReachedBarrier = false;
		{
			std::unique_lock<std::mutex> lock(mutex);
			blockerReachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
				[&] { return blockerStarted; });
		}
		if (joinQueuedWork) cache.Prefetch({blocker, focused}, 0, 1, 1);
		cache.RequestSelectedSource(jpegview_linux::DescribeImageSource(active),
			[&](const jpegview_linux::SourceDescriptor&,
				const jpegview_linux::DecodedImageCache::ImagePtr& image,
				const jpegview_linux::WorkerFailure&) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					activeCompleted = image != nullptr;
				}
				changed.notify_all();
			});
		cache.ObserveSelectedSource(jpegview_linux::DescribeImageSource(focused),
			[&](const jpegview_linux::SourceDescriptor&,
				const jpegview_linux::DecodedImageCache::ImagePtr& image,
				const jpegview_linux::WorkerFailure&) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					observerCompleted = image != nullptr;
				}
				changed.notify_all();
			}, jpegview_linux::PerfWorkClass::FocusedPreview);
		{
			std::lock_guard<std::mutex> lock(mutex);
			releaseBlocker = true;
		}
		changed.notify_all();
		Expect(blockerReachedBarrier, "observer priority fixture did not reach its barrier");
		{
			std::unique_lock<std::mutex> lock(mutex);
			Expect(changed.wait_for(lock, std::chrono::seconds(2), [&] {
				return activeCompleted && observerCompleted;
			}), "active work or its lower-priority observer did not complete");
		}
		Expect(cache.WaitUntilIdle(std::chrono::seconds(2)) &&
			decodedPaths == std::vector<fs::path>{blocker, active, focused},
			"new or promoted focused observer overtook queued active source decoding");
	}
}

void TestDecodedImageCacheRetentionDenialKeepsQueuedObserver() {
	TemporaryDirectory temporary;
	const fs::path blocker = temporary.path() / "observer-budget-blocker.png";
	const fs::path focused = temporary.path() / "observer-budget-focused.png";
	WriteText(blocker, "blocker");
	WriteText(focused, "focused");
	std::mutex mutex;
	std::condition_variable changed;
	bool blockerStarted = false;
	bool releaseBlocker = false;
	bool observerCompleted = false;
	bool observerSucceeded = false;
	int focusedDecodeCalls = 0;
	jpegview_linux::DecodedImageCache cache(1,
		[&](const fs::path& path, DecodedImage& image, std::string&) {
			std::unique_lock<std::mutex> lock(mutex);
			if (path == blocker) {
				blockerStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseBlocker; });
			} else if (path == focused) {
				++focusedDecodeCalls;
			}
			image = *CachedTestImage(4);
			return true;
		}, {}, 1);
	cache.RequestSelectedSource(jpegview_linux::DescribeImageSource(blocker),
		[](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr&,
			const jpegview_linux::WorkerFailure&) {});
	bool blockerReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		blockerReachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return blockerStarted; });
	}
	cache.ObserveSelectedSource(jpegview_linux::DescribeImageSource(focused),
		[&](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr& image,
			const jpegview_linux::WorkerFailure& failure) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				observerCompleted = true;
				observerSucceeded = image != nullptr && !failure.Failed();
			}
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::FocusedPreview);
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseBlocker = true;
	}
	changed.notify_all();
	Expect(blockerReachedBarrier, "observer retention fixture did not reach its barrier");
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return observerCompleted; }),
			"retention denial silently discarded a queued observer completion");
	}
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)) && observerSucceeded &&
		focusedDecodeCalls == 1 && cache.CachedImages() == 0,
		"queued observer did not receive its uncached source after retention denial");
}

void TestDecodedImageCacheObserverReceivesStructuredFailure() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "observed-failed-source.jpg";
	WriteText(filename, "failed source");
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(filename);
	std::mutex mutex;
	std::condition_variable changed;
	bool decoderStarted = false;
	bool releaseDecoder = false;
	bool ownerCompleted = false;
	bool observerCompleted = false;
	jpegview_linux::WorkerFailureKind ownerFailure =
		jpegview_linux::WorkerFailureKind::None;
	jpegview_linux::WorkerFailureKind observerFailure =
		jpegview_linux::WorkerFailureKind::None;
	jpegview_linux::DecodedImageCache cache(64,
		[&](const fs::path&, DecodedImage&, std::string& error) {
			std::unique_lock<std::mutex> lock(mutex);
			decoderStarted = true;
			changed.notify_all();
			changed.wait(lock, [&] { return releaseDecoder; });
			error = "intentional decoder failure";
			return false;
		}, {}, 1);
	cache.RequestSelectedSource(source,
		[&](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr&,
			const jpegview_linux::WorkerFailure& failure) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				ownerFailure = failure.kind;
				ownerCompleted = true;
			}
			changed.notify_all();
		});
	bool decoderReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		decoderReachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return decoderStarted; });
	}
	cache.ObserveSelectedSource(source,
		[&](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr&,
			const jpegview_linux::WorkerFailure& failure) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				observerFailure = failure.kind;
				observerCompleted = true;
			}
			changed.notify_all();
		});
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseDecoder = true;
	}
	changed.notify_all();
	Expect(decoderReachedBarrier,
		"failing selected decode did not reach its deterministic barrier");
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return ownerCompleted && observerCompleted;
		}), "owner and observer did not both receive the selected decode failure");
	}
	Expect(ownerFailure == jpegview_linux::WorkerFailureKind::ProcessingFailed &&
		observerFailure == ownerFailure,
		"observer did not receive the selected decode's structured failure");
}

void TestDecodedActiveSpreadBudgetPressure() {
	TemporaryDirectory temporary;
	const fs::path foregroundFile = temporary.path() / "recent-foreground.ppm";
	const fs::path speculativeFile = temporary.path() / "older-speculation.ppm";
	const fs::path spreadPartner = temporary.path() / "active-spread-partner.png";
	WriteText(foregroundFile, "foreground source");
	WriteText(speculativeFile, "speculative source");
	WriteText(spreadPartner, "spread partner source");

	std::mutex completionMutex;
	std::condition_variable completionChanged;
	bool spreadCompleted = false;
	jpegview_linux::DecodedImageCache cache(8,
		[](const fs::path&, DecodedImage& image, std::string&) {
			image = *CachedTestImage(4);
			return true;
		});
	cache.Prefetch({foregroundFile, speculativeFile}, 0, 1, 1);
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"budget-pressure fixture speculation did not finish before its deadline");
	cache.Store(foregroundFile, CachedTestImage(4));
	Expect(cache.Find(foregroundFile) != nullptr,
		"foreground fixture could not be retained beside older speculation");
	Expect(cache.CachedBytes() == 8,
		"budget-pressure fixture did not fill the decoded image budget");

	cache.RequestBackground(spreadPartner,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			{
				std::lock_guard<std::mutex> lock(completionMutex);
				spreadCompleted = image != nullptr;
			}
			completionChanged.notify_all();
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool callbackReached = false;
	{
		std::unique_lock<std::mutex> lock(completionMutex);
		callbackReached = completionChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return spreadCompleted; });
	}
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"active spread partner decode did not become idle under budget pressure");
	Expect(callbackReached && cache.Find(foregroundFile) != nullptr &&
		cache.Find(spreadPartner) != nullptr && cache.Find(speculativeFile) == nullptr,
		"active spread partner could not use the full budget while preserving the most-recent foreground image");
}

void TestDecodedActiveWorkSurvivesRetentionRefusalForSpreadFrames() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "oversized-spread-source.png";
	WriteText(filename, "oversized decoded source");
	auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(32);
	auto retainedOwner = std::make_shared<int>(1);
	std::shared_ptr<const void> retainedAllocation(retainedOwner, retainedOwner.get());
	auto otherCache = budget->TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, retainedAllocation);
	Expect(otherCache && budget->Used() == 16,
		"spread retention fixture did not reserve its shared-cache pressure");
	std::mutex mutex;
	std::condition_variable changed;
	jpegview_linux::DecodedImageCache::ImagePtr activeDecoded;
	bool decodedCompleted = false;
	jpegview_linux::DecodedImageCache decodedCache(128,
		[](const fs::path&, DecodedImage& image, std::string&) {
			image = *CachedTestImage(24);
			return true;
		}, budget);
	decodedCache.RequestBackground(filename,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				activeDecoded = image;
				decodedCompleted = true;
			}
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return decodedCompleted; }),
			"active spread decode did not complete after cache admission refusal");
	}
	Expect(activeDecoded && activeDecoded->frames.front().bgra.size() == 24 &&
		decodedCache.Find(filename) == activeDecoded &&
		budget->Snapshot().activeWorkingBytes == 24 && budget->Used() == 16,
		"retention refusal lost the active decoded source or charged it against retained capacity");
	jpegview_linux::DisplayImageCache displayCache(64, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
			auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			image->filename = request.filename;
			image->source = request.source;
			image->cacheKey = request.cacheKey;
			image->key = request.key;
			image->width = request.targetWidth;
			image->height = request.targetHeight;
			image->bgra.assign(static_cast<std::size_t>(image->width) *
				static_cast<std::size_t>(image->height) * 4, 255);
			return image;
		}, budget);
	auto request = jpegview_linux::MakeDisplayImageRequest(filename, activeDecoded,
		0, 2, 2, false, 0);
	request.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	auto prepared = displayCache.RequestAndWait(request);
	Expect(prepared && prepared->bgra.size() == 16 && displayCache.CachedBytes() == 16 &&
		budget->Used() == 32 && budget->Snapshot().activeWorkingBytes == 24,
		"fitted active spread frame could not be prepared from an oversized unretained source");
	prepared.reset();
	request.decoded.reset();
	activeDecoded.reset();
	decodedCache.Clear();
	displayCache.Clear();
	otherCache.Reset();
	retainedAllocation.reset();
	retainedOwner.reset();
	const auto retirementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while ((budget->Used() != 0 || budget->Snapshot().activeWorkingBytes != 0) &&
		std::chrono::steady_clock::now() < retirementDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(budget->Used() == 0 && budget->Snapshot().activeWorkingBytes == 0,
		"active spread working-data charges survived their final allocation owner");
}

void TestDecodedPromotedSpreadBudgetPressure() {
	TemporaryDirectory temporary;
	const fs::path foregroundFile = temporary.path() / "retained-foreground.ppm";
	const fs::path speculativeFile = temporary.path() / "older-speculation.ppm";
	const fs::path promotedFile = temporary.path() / "promoted-active-spread.ppm";
	WriteText(foregroundFile, "foreground source");
	WriteText(speculativeFile, "speculative source");
	WriteText(promotedFile, "promoted spread source");

	std::mutex mutex;
	std::condition_variable changed;
	bool decodeStarted = false;
	bool releaseDecode = false;
	bool promotedCallbackCompleted = false;
	jpegview_linux::DecodedImageCache cache(8,
		[&](const fs::path& filename, DecodedImage& image, std::string&) {
			if (filename == promotedFile) {
				std::unique_lock<std::mutex> lock(mutex);
				decodeStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseDecode; });
			}
			image = *CachedTestImage(4);
			return true;
		});
	ScopedConditionRelease decodeRelease(mutex, changed, releaseDecode);
	cache.Prefetch({foregroundFile, speculativeFile}, 0, 1, 1);
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"promoted-spread budget fixture did not finish initial speculation");
	cache.Store(foregroundFile, CachedTestImage(4));
	Expect(cache.Find(foregroundFile) != nullptr && cache.CachedBytes() == 8,
		"promoted-spread budget fixture did not retain foreground plus speculation");

	std::atomic<int> obsoleteCallbacks{0};
	std::shared_ptr<const DecodedImage> promotedDecoded;
	cache.RequestBackground(promotedFile,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr&) {
			++obsoleteCallbacks;
		}, jpegview_linux::PerfWorkClass::DistantSpeculation);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return decodeStarted; });
	}
	cache.RequestBackground(promotedFile,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				promotedDecoded = image;
				promotedCallbackCompleted = true;
			}
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	const auto promotedDiagnostics = cache.GetDiagnostics();
	cache.Prefetch({}, 0, 0, 0);
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseDecode = true;
	}
	changed.notify_all();
	bool callbackReached = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		callbackReached = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return promotedCallbackCompleted; });
	}
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"promoted active-spread decode did not become idle under cache pressure");
	Expect(reachedBarrier && callbackReached && promotedDiagnostics.activeSpreadActive == 1 &&
		obsoleteCallbacks == 0 && promotedDecoded && cache.Find(promotedFile) != nullptr &&
		cache.Find(foregroundFile) != nullptr && cache.Find(speculativeFile) == nullptr,
		"in-flight promotion lost ActiveImageSpread cache admission or evicted the retained foreground image");
}

void TestDecodedBudgetRejectionPreservesQueuedActiveSpreadWork() {
	TemporaryDirectory temporary;
	const fs::path foregroundFile = temporary.path() / "retained-foreground.ppm";
	const fs::path oversizedFile = temporary.path() / "oversized-active-spread.ppm";
	const fs::path dimensionsFile = temporary.path() / "queued-partner.jpg";
	WriteText(foregroundFile, "foreground source");
	WriteText(oversizedFile, "oversized active spread source");
	constexpr int width = 4;
	constexpr int height = 8;
	std::vector<std::uint8_t> pixels(width * height * 4, 255);
	ImageWriteOptions options;
	options.jpegQuality = 95;
	std::string error;
	Expect(jpegview_linux::WriteImage(dimensionsFile, pixels.data(), width, height,
		options, error), "could not create queued JPEG dimensions fixture: " + error);

	std::mutex mutex;
	std::condition_variable changed;
	bool oversizedStarted = false;
	bool releaseOversized = false;
	bool oversizedCallbackComplete = false;
	bool oversizedCallbackDelivered = false;
	bool dimensionsCallbackComplete = false;
	bool dimensionsSucceeded = false;
	int decodedWidth = 0;
	int decodedHeight = 0;
	std::atomic<int> pixelDecodeCount{0};
	std::atomic<int> dimensionsReadCount{0};
	jpegview_linux::DecodedImageCache cache(8,
		[&](const fs::path& filename, DecodedImage& image, std::string&) {
			++pixelDecodeCount;
			if (filename == oversizedFile) {
				std::unique_lock<std::mutex> lock(mutex);
				oversizedStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseOversized; });
			}
			image = *CachedTestImage(filename == oversizedFile ? 12 : 4);
			return true;
		}, {}, 1,
		[&](const fs::path& filename, int& imageWidth, int& imageHeight,
			std::string& message) {
			++dimensionsReadCount;
			return jpegview_linux::ReadJpegDimensions(filename, imageWidth,
				imageHeight, message);
		});
	ScopedConditionRelease releaseOnExit(mutex, changed, releaseOversized);
	cache.Store(foregroundFile, CachedTestImage(4));
	Expect(cache.Find(foregroundFile) != nullptr && cache.CachedBytes() == 4,
		"budget-rejection fixture did not retain its foreground image");

	cache.RequestBackground(oversizedFile,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				oversizedCallbackComplete = true;
				oversizedCallbackDelivered = image != nullptr;
			}
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return oversizedStarted; });
	}
	Expect(reachedBarrier,
		"oversized active-spread decode did not reach its bounded barrier");

	cache.RequestJpegDimensions(dimensionsFile,
		[&](const fs::path&, bool succeeded, int imageWidth, int imageHeight) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				dimensionsCallbackComplete = true;
				dimensionsSucceeded = succeeded;
				decodedWidth = imageWidth;
				decodedHeight = imageHeight;
			}
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	cache.Prefetch({}, 0, 0, 0);
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseOversized = true;
	}
	changed.notify_all();
	bool callbacksComplete = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		callbacksComplete = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return oversizedCallbackComplete && dimensionsCallbackComplete;
		});
	}
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"cache did not drain active spread work after an oversized admission rejection");
	Expect(callbacksComplete && oversizedCallbackDelivered && dimensionsSucceeded &&
		decodedWidth == width && decodedHeight == height && dimensionsReadCount == 1 &&
		pixelDecodeCount == 1 && cache.Find(foregroundFile) != nullptr &&
		cache.Find(oversizedFile) == nullptr,
		"oversized active work was lost, dropped queued JPEG dimensions work, or evicted foreground pixels");
}

void TestJpegActiveSpreadDimensionsSurvivePause() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "cold-active-spread.jpg";
	constexpr int sourceWidth = 4;
	constexpr int sourceHeight = 8;
	std::vector<std::uint8_t> pixels(sourceWidth * sourceHeight * 4, 255);
	for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
		pixels[offset] = static_cast<std::uint8_t>((offset / 4) * 7);
		pixels[offset + 1] = 80;
		pixels[offset + 2] = 150;
	}
	ImageWriteOptions options;
	options.jpegQuality = 95;
	std::string error;
	Expect(jpegview_linux::WriteImage(filename, pixels.data(), sourceWidth,
		sourceHeight, options, error), "could not create JPEG spread fixture: " + error);

	std::mutex mutex;
	std::condition_variable changed;
	bool readStarted = false;
	bool releaseRead = false;
	bool callbackCompleted = false;
	bool dimensionsSucceeded = false;
	int decodedWidth = 0;
	int decodedHeight = 0;
	std::atomic<int> pixelDecodes{0};
	jpegview_linux::DecodedImageCache cache(64,
		[&](const fs::path&, DecodedImage&, std::string&) {
			++pixelDecodes;
			return false;
		}, {}, 1,
		[&](const fs::path& path, int& width, int& height, std::string& message) {
			{
				std::unique_lock<std::mutex> lock(mutex);
				readStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseRead; });
			}
			return jpegview_linux::ReadJpegDimensions(path, width, height, message);
		});
	ScopedConditionRelease readRelease(mutex, changed, releaseRead);
	cache.RequestJpegDimensions(filename,
		[&](const fs::path&, bool succeeded, int width, int height) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				dimensionsSucceeded = succeeded;
				decodedWidth = width;
				decodedHeight = height;
				callbackCompleted = true;
			}
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return readStarted; });
	}
	const auto pending = cache.GetDiagnostics();
	cache.Prefetch({}, 0, 0, 0);
	const auto paused = cache.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseRead = true;
	}
	changed.notify_all();
	bool callbackReached = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		callbackReached = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return callbackCompleted; });
	}
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"JPEG dimensions worker did not become idle before its deadline");
	Expect(reachedBarrier && callbackReached && dimensionsSucceeded &&
		decodedWidth == sourceWidth && decodedHeight == sourceHeight && pixelDecodes == 0 &&
		pending.activeSpreadActive == 1 && paused.activeSpreadActive == 1 &&
		cache.CachedImages() == 0,
		"cold JPEG spread dimensions did not survive paused prefetch as metadata-only active-spread work");
}

void TestPromotedActiveSpreadJpegDimensionsKeepCurrentOwner() {
	TemporaryDirectory temporary;
	const fs::path partnerPath = temporary.path() / "selected-partner.jpg";
	const fs::path nextPartnerPath = temporary.path() / "following-partner.jpg";
	WriteText(partnerPath, "blocked JPEG dimensions source");
	WriteText(nextPartnerPath, "next JPEG dimensions source");
	const jpegview_linux::SourceDescriptor partnerSource =
		jpegview_linux::DescribeImageSource(partnerPath);
	const jpegview_linux::SourceDescriptor nextPartnerSource =
		jpegview_linux::DescribeImageSource(nextPartnerPath);
	const jpegview_linux::DecodedImageRequestIdentity partnerRequest{
		partnerSource.Key(), true};
	const jpegview_linux::DecodedImageRequestIdentity currentRequest{
		partnerSource.Key(), true};
	const jpegview_linux::DecodedImageRequestIdentity nextPartnerRequest{
		nextPartnerSource.Key(), true};

	std::mutex mutex;
	std::condition_variable changed;
	bool readStarted = false;
	bool releaseRead = false;
	bool currentCallbackCompleted = false;
	int currentWidth = 0;
	int currentHeight = 0;
	int obsoleteBatchPublications = 0;
	std::atomic<int> dimensionReads{0};
	const auto obsoleteBatch = std::make_shared<jpegview_linux::WorkBatchGate>();
	std::optional<jpegview_linux::DecodedImageRequestIdentity> trackedPartnerRequest(
		partnerRequest);
	jpegview_linux::DecodedImageCache cache(64,
		[](const fs::path&, DecodedImage&, std::string&) { return false; }, {}, 1,
		[&](const fs::path&, int& width, int& height, std::string&) {
			++dimensionReads;
			{
				std::unique_lock<std::mutex> lock(mutex);
				readStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseRead; });
			}
			width = 12;
			height = 24;
			return true;
		});
	ScopedConditionRelease releaseOnExit(mutex, changed, releaseRead);
	cache.RequestJpegDimensions(partnerSource,
		[&](const fs::path&, bool, int, int) {
			obsoleteBatch->Publish([&] {
				std::lock_guard<std::mutex> lock(mutex);
				++obsoleteBatchPublications;
			});
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return readStarted; });
	}

	const bool sameRequestTransfers = trackedPartnerRequest.has_value() &&
		jpegview_linux::CanTransferActiveSpreadRequestToCurrentImage(
			*trackedPartnerRequest, currentRequest);
	if (sameRequestTransfers) {
		obsoleteBatch->Deactivate();
		trackedPartnerRequest.reset();
	}
	cache.RequestJpegDimensions(partnerSource,
		[&](const fs::path&, bool succeeded, int width, int height) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				currentCallbackCompleted = succeeded;
				currentWidth = width;
				currentHeight = height;
			}
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	const auto diagnosticsAfterPromotion = cache.GetDiagnostics();

	// Preparing the next partner would cancel a stale tracked request. Once
	// transferred, the old partner tracker is retired and cannot cancel this key.
	bool obsoleteRequestCanceled = false;
	if (trackedPartnerRequest.has_value() &&
		!jpegview_linux::SameDecodedImageRequest(*trackedPartnerRequest, nextPartnerRequest)) {
		obsoleteBatch->Deactivate();
		obsoleteRequestCanceled = cache.CancelActiveSpreadRequest(partnerSource, true);
		trackedPartnerRequest.reset();
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseRead = true;
	}
	changed.notify_all();
	bool callbackReached = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		callbackReached = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return currentCallbackCompleted; });
	}
	const bool becameIdle = cache.WaitUntilIdle(std::chrono::seconds(2));

	Expect(reachedBarrier && sameRequestTransfers && !trackedPartnerRequest.has_value() &&
		!obsoleteRequestCanceled && callbackReached && becameIdle &&
		currentWidth == 12 && currentHeight == 24 && dimensionReads == 1 &&
		obsoleteBatchPublications == 0 &&
		diagnosticsAfterPromotion.activeSpreadActive == 1 &&
		!jpegview_linux::SameDecodedImageRequest(partnerRequest,
			jpegview_linux::DecodedImageRequestIdentity{partnerSource.Key(), false}) &&
		!jpegview_linux::SameDecodedImageRequest(partnerRequest, nextPartnerRequest),
		"selecting a cold JPEG spread partner did not transfer its blocked dimensions read to current-image ownership");
}

void TestSvgSourceDimensionsUseDecodedCacheWorker() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "vector.svg";
	WriteText(filename,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"640\" height=\"480\"/>");

	std::mutex mutex;
	std::condition_variable changed;
	bool callbackCompleted = false;
	bool dimensionsSucceeded = false;
	int decodedWidth = 0;
	int decodedHeight = 0;
	std::atomic<int> dimensionsReadCount{0};
	std::atomic<int> pixelDecodeCount{0};
	jpegview_linux::DecodedImageCache cache(64,
		[&](const fs::path&, DecodedImage&, std::string&) {
			++pixelDecodeCount;
			return false;
		}, {}, 1,
		[&](const fs::path& path, int& width, int& height, std::string&) {
			++dimensionsReadCount;
			if (path != filename) return false;
			width = 640;
			height = 480;
			return true;
		});
	cache.RequestSourceDimensions(filename,
		[&](const fs::path& completedPath, bool succeeded, int width, int height) {
			std::lock_guard<std::mutex> lock(mutex);
			callbackCompleted = completedPath == filename;
			dimensionsSucceeded = succeeded;
			decodedWidth = width;
			decodedHeight = height;
			changed.notify_all();
		});
	bool callbackReached = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		callbackReached = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return callbackCompleted; });
	}
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"SVG source dimensions request did not drain from the cache worker");
	Expect(callbackReached && dimensionsSucceeded && decodedWidth == 640 &&
		decodedHeight == 480 && dimensionsReadCount == 1 && pixelDecodeCount == 0,
		"SVG source geometry was rejected or materialized pixels instead of using the dimensions worker");

#if JPEGVIEW_HAVE_SVG
	const fs::path mislabeledFilename = temporary.path() / "vector.jpg";
	WriteText(mislabeledFilename,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"640\" height=\"480\"/>");
	std::mutex fallbackMutex;
	std::condition_variable fallbackChanged;
	bool fallbackDimensionsCompleted = false;
	bool fallbackDimensionsSucceeded = true;
	bool fallbackDecodeCompleted = false;
	bool fallbackDecodeSucceeded = false;
	jpegview_linux::DecodedImageCache fallbackCache(8 * 1024 * 1024);
	fallbackCache.RequestSourceDimensions(mislabeledFilename,
		[&](const fs::path& completedPath, bool succeeded, int, int) {
			std::lock_guard<std::mutex> lock(fallbackMutex);
			fallbackDimensionsCompleted = completedPath == mislabeledFilename;
			fallbackDimensionsSucceeded = succeeded;
			fallbackChanged.notify_all();
		});
	bool fallbackDimensionsReached = false;
	{
		std::unique_lock<std::mutex> lock(fallbackMutex);
		fallbackDimensionsReached = fallbackChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return fallbackDimensionsCompleted; });
	}
	Expect(fallbackDimensionsReached && !fallbackDimensionsSucceeded,
		"a JPEG-suffixed SVG was accepted as a raster-only dimensions result");
	fallbackCache.RequestSelectedSource(
		jpegview_linux::DescribeImageSource(mislabeledFilename),
		[&](const jpegview_linux::SourceDescriptor&,
			const jpegview_linux::DecodedImageCache::ImagePtr& image,
			const jpegview_linux::WorkerFailure& failure) {
			std::lock_guard<std::mutex> lock(fallbackMutex);
			fallbackDecodeCompleted = true;
			fallbackDecodeSucceeded = image && !failure.Failed() && image->isSvg &&
				!image->frames.empty() && image->frames.front().width == 640 &&
				image->frames.front().height == 480;
			fallbackChanged.notify_all();
		});
	bool fallbackDecodeReached = false;
	{
		std::unique_lock<std::mutex> lock(fallbackMutex);
		fallbackDecodeReached = fallbackChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return fallbackDecodeCompleted; });
	}
	Expect(fallbackCache.WaitUntilIdle(std::chrono::seconds(2)) &&
		fallbackDecodeReached && fallbackDecodeSucceeded,
		"a JPEG-suffixed SVG did not fall through to content-based selected decoding");
#endif
}

void TestActiveSpreadPartnerReplacementCancelsObsoleteRequests() {
	TemporaryDirectory temporary;
	const fs::path blockedPartner = temporary.path() / "blocked-old-partner.png";
	const fs::path queuedPartner = temporary.path() / "queued-obsolete-partner.png";
	const fs::path currentPartner = temporary.path() / "current-partner.png";
	for (const fs::path& path : {blockedPartner, queuedPartner, currentPartner}) {
		WriteText(path, path.filename().string());
	}
	std::mutex mutex;
	std::condition_variable changed;
	bool decodeStarted = false;
	bool releaseDecode = false;
	bool currentCompleted = false;
	std::atomic<int> blockedCallbacks{0};
	std::atomic<int> queuedCallbacks{0};
	std::atomic<int> staleDecodeCalls{0};
	std::atomic<int> currentDecodeCalls{0};
	jpegview_linux::DecodedImageCache cache(64,
		[&](const fs::path& filename, DecodedImage& image, std::string&) {
			if (filename == blockedPartner) {
				std::unique_lock<std::mutex> lock(mutex);
				decodeStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseDecode; });
			} else if (filename == queuedPartner) {
				++staleDecodeCalls;
			} else if (filename == currentPartner) {
				++currentDecodeCalls;
			}
			image = *CachedTestImage(4);
			return true;
		}, {}, 1);
	ScopedConditionRelease releaseOnExit(mutex, changed, releaseDecode);
	cache.RequestBackground(blockedPartner,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr&) {
			++blockedCallbacks;
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return decodeStarted; });
	}
	cache.RequestBackground(queuedPartner,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr&) {
			++queuedCallbacks;
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	const auto beforeReplace = cache.GetDiagnostics();
	const bool canceledInFlight = cache.CancelActiveSpreadRequest(blockedPartner);
	const bool canceledQueued = cache.CancelActiveSpreadRequest(queuedPartner);
	const auto afterReplace = cache.GetDiagnostics();
	const bool obsoleteForegroundDemand = jpegview_linux::ForegroundSourceWorkPending({
		false, false,
		afterReplace.foregroundQueued != 0,
		afterReplace.foregroundActive != 0,
		afterReplace.activeSpreadQueued != 0,
		afterReplace.activeSpreadActive != 0,
		false});
	cache.RequestBackground(currentPartner,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& decoded) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				currentCompleted = decoded != nullptr;
			}
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseDecode = true;
	}
	changed.notify_all();
	bool currentCallbackReached = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		currentCallbackReached = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return currentCompleted; });
	}
	const bool becameIdle = cache.WaitUntilIdle(std::chrono::seconds(2));
	Expect(reachedBarrier && beforeReplace.activeSpreadActive == 1 &&
		beforeReplace.activeSpreadQueued == 1 && canceledInFlight && canceledQueued &&
		afterReplace.activeSpreadActive == 0 && afterReplace.activeSpreadQueued == 0 &&
		!obsoleteForegroundDemand && currentCallbackReached && becameIdle &&
		blockedCallbacks == 0 && queuedCallbacks == 0 && staleDecodeCalls == 0 &&
		currentDecodeCalls == 1 && cache.CachedImages() == 1 &&
		cache.Find(currentPartner) != nullptr && cache.Find(blockedPartner) == nullptr &&
		cache.Find(queuedPartner) == nullptr,
		"partner replacement did not retire active and queued obsolete source work before preparing the current partner");
}

void TestActiveSpreadCancellationUsesSubmittedSourceIdentity() {
	TemporaryDirectory temporary;
	const fs::path activePath = temporary.path() / "active-partner.png";
	const fs::path queuedPath = temporary.path() / "queued-partner.png";
	const fs::path activeReplacement = temporary.path() / "active-replacement.tmp";
	const fs::path queuedReplacement = temporary.path() / "queued-replacement.tmp";
	WriteBytes(activePath, {1, 2, 3, 4});
	WriteBytes(queuedPath, {5, 6, 7, 8});
	const jpegview_linux::SourceDescriptor submittedActive =
		jpegview_linux::DescribeImageSource(activePath);
	const jpegview_linux::SourceDescriptor submittedQueued =
		jpegview_linux::DescribeImageSource(queuedPath);
	std::mutex mutex;
	std::condition_variable changed;
	bool decodeStarted = false;
	bool releaseDecode = false;
	std::atomic<int> callbacks{0};
	std::atomic<int> decoderReads{0};
	jpegview_linux::DecodedImageCache cache(64,
		[&](const fs::path& filename, DecodedImage& image, std::string&) {
			++decoderReads;
			if (filename == activePath) {
				std::unique_lock<std::mutex> lock(mutex);
				decodeStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseDecode; });
			}
			image = *CachedTestImage(4);
			return true;
		}, {}, 1);
	ScopedConditionRelease releaseOnExit(mutex, changed, releaseDecode);
	const auto completion = [&](const fs::path&,
		const jpegview_linux::DecodedImageCache::ImagePtr&) { ++callbacks; };
	cache.RequestBackground(submittedActive, completion,
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return decodeStarted;
		});
	}
	cache.RequestBackground(submittedQueued, completion,
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	WriteBytes(activeReplacement, {1, 2, 3, 4});
	WriteBytes(queuedReplacement, {5, 6, 7, 8});
	SetModificationTimeNanoseconds(activeReplacement,
		submittedActive.BackingIdentity().modifiedSeconds,
		static_cast<long>(submittedActive.BackingIdentity().modifiedNanoseconds));
	SetModificationTimeNanoseconds(queuedReplacement,
		submittedQueued.BackingIdentity().modifiedSeconds,
		static_cast<long>(submittedQueued.BackingIdentity().modifiedNanoseconds));
	std::ifstream keepActiveInode(activePath, std::ios::binary);
	std::ifstream keepQueuedInode(queuedPath, std::ios::binary);
	Expect(static_cast<bool>(keepActiveInode) && static_cast<bool>(keepQueuedInode),
		"could not retain submitted spread source inodes during replacement");
	std::error_code activeRenameError;
	std::error_code queuedRenameError;
	fs::rename(activeReplacement, activePath, activeRenameError);
	fs::rename(queuedReplacement, queuedPath, queuedRenameError);
	const jpegview_linux::SourceDescriptor refreshedActive =
		jpegview_linux::DescribeImageSource(activePath);
	const jpegview_linux::SourceDescriptor refreshedQueued =
		jpegview_linux::DescribeImageSource(queuedPath);
	const auto beforeCancel = cache.GetDiagnostics();
	const bool wrongActiveKeyCanceled =
		cache.CancelActiveSpreadRequest(refreshedActive);
	const bool wrongQueuedKeyCanceled =
		cache.CancelActiveSpreadRequest(refreshedQueued);
	const auto afterWrongKeyCancel = cache.GetDiagnostics();
	const bool originalActiveCanceled =
		cache.CancelActiveSpreadRequest(submittedActive);
	const bool originalQueuedCanceled =
		cache.CancelActiveSpreadRequest(submittedQueued);
	const auto afterOriginalKeyCancel = cache.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseDecode = true;
	}
	changed.notify_all();
	const bool becameIdle = cache.WaitUntilIdle(std::chrono::seconds(2));
	Expect(reachedBarrier && !activeRenameError && !queuedRenameError &&
		refreshedActive.Valid() && refreshedQueued.Valid() &&
		refreshedActive.Key() != submittedActive.Key() &&
		refreshedQueued.Key() != submittedQueued.Key() &&
		beforeCancel.activeSpreadActive == 1 && beforeCancel.activeSpreadQueued == 1 &&
		!wrongActiveKeyCanceled && !wrongQueuedKeyCanceled &&
		afterWrongKeyCancel.activeSpreadActive == 1 &&
		afterWrongKeyCancel.activeSpreadQueued == 1 && originalActiveCanceled &&
		originalQueuedCanceled && afterOriginalKeyCancel.activeSpreadActive == 0 &&
		afterOriginalKeyCancel.activeSpreadQueued == 0 && becameIdle &&
		decoderReads == 1 && callbacks == 0,
		"active spread cancellation lost the submitted identity after its path descriptor was replaced");
}

void TestDisablingDoublePageCancelsBlockedAndQueuedPartnerDimensions() {
	TemporaryDirectory temporary;
	const fs::path blockedPartner = temporary.path() / "blocked-old-partner.jpg";
	const fs::path queuedPartner = temporary.path() / "queued-old-partner.jpg";
	WriteText(blockedPartner, "blocked JPEG metadata source");
	WriteText(queuedPartner, "queued JPEG metadata source");
	std::mutex mutex;
	std::condition_variable changed;
	bool readStarted = false;
	bool releaseRead = false;
	std::atomic<int> reads{0};
	std::atomic<int> callbacks{0};
	jpegview_linux::DecodedImageCache cache(64,
		[](const fs::path&, DecodedImage&, std::string&) { return false; }, {}, 1,
		[&](const fs::path&, int& width, int& height, std::string&) {
			++reads;
			{
				std::unique_lock<std::mutex> lock(mutex);
				readStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseRead; });
			}
			width = 4;
			height = 8;
			return true;
		});
	ScopedConditionRelease releaseOnExit(mutex, changed, releaseRead);
	const auto completion = [&](const fs::path&, bool, int, int) { ++callbacks; };
	cache.RequestJpegDimensions(blockedPartner, completion,
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return readStarted; });
	}
	cache.RequestJpegDimensions(queuedPartner, completion,
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	const auto beforeDisable = cache.GetDiagnostics();
	const bool canceledActive = cache.CancelActiveSpreadRequest(blockedPartner, true);
	const bool canceledQueued = cache.CancelActiveSpreadRequest(queuedPartner, true);
	const auto afterDisable = cache.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseRead = true;
	}
	changed.notify_all();
	const bool becameIdle = cache.WaitUntilIdle(std::chrono::seconds(2));
	Expect(reachedBarrier && beforeDisable.activeSpreadActive == 1 &&
		beforeDisable.activeSpreadQueued == 1 && canceledActive && canceledQueued &&
		afterDisable.activeSpreadActive == 0 && afterDisable.activeSpreadQueued == 0 &&
		becameIdle && reads == 1 && callbacks == 0 && cache.CachedImages() == 0,
		"disabling double-page work did not cancel its active and queued JPEG partner-dimension requests");
}



void TestDisplayImageCachePromotedSpreadSurvivesPause() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "promoted-spread.png";
	WriteText(filename, "active spread source");
	auto neighborRequest = jpegview_linux::MakeDisplayImageRequest(
		filename, DisplayCacheTestImage(4, 4), 0, 2, 2, false, 1);
	neighborRequest.workClass = jpegview_linux::PerfWorkClass::NearestNavigationNeighbor;
	std::mutex mutex;
	std::condition_variable changed;
	bool processorStarted = false;
	bool releaseProcessor = false;
	jpegview_linux::DisplayImageCache cache(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			{
				std::unique_lock<std::mutex> lock(mutex);
				processorStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseProcessor; });
			}
			auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			image->key = request.key;
			image->source = request.source;
			image->cacheKey = request.cacheKey;
			image->width = image->height = 2;
			image->workClass = request.workClass;
			image->bgra.assign(16, 255);
			return image;
		});
	ScopedConditionRelease processorRelease(mutex, changed, releaseProcessor);
	cache.RequestBackground(neighborRequest);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return processorStarted; });
	}
	auto activeSpreadRequest = neighborRequest;
	activeSpreadRequest.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	cache.RequestBackgroundBatch({activeSpreadRequest});
	cache.Prefetch({});
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseProcessor = true;
	}
	changed.notify_all();
	const bool becameIdle = cache.WaitUntilIdle(std::chrono::seconds(2));
	const auto activeSpreadOnly = cache.TakeCompleted(1, {
		jpegview_linux::PerfWorkClass::ActiveImageSpread});
	Expect(reachedBarrier && becameIdle && activeSpreadOnly.size() == 1 &&
		activeSpreadOnly.front()->key == neighborRequest.key,
		"in-flight neighbor promotion lost active-spread completion eligibility during paused prefetch");
	cache.Retire(activeSpreadOnly.front());
}

void TestActiveSpreadDecodeDemandGatesBackgroundWork() {
	TemporaryDirectory temporary;
	const fs::path partner = temporary.path() / "active-partner.png";
	WriteText(partner, "active spread partner");
	std::mutex decodeMutex;
	std::condition_variable decodeChanged;
	bool decodeStarted = false;
	bool releaseDecode = false;
	jpegview_linux::DecodedImageCache cache(64,
		[&](const fs::path&, DecodedImage& image, std::string&) {
			{
				std::unique_lock<std::mutex> lock(decodeMutex);
				decodeStarted = true;
				decodeChanged.notify_all();
				decodeChanged.wait(lock, [&] { return releaseDecode; });
			}
			image = *CachedTestImage(4);
			return true;
		});
	ScopedConditionRelease decodeRelease(decodeMutex, decodeChanged, releaseDecode);
	cache.RequestBackground(partner, {}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(decodeMutex);
		reachedBarrier = decodeChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return decodeStarted; });
	}
	const auto diagnostics = cache.GetDiagnostics();
	const bool foregroundPending = jpegview_linux::ForegroundSourceWorkPending({
		false,
		false,
		diagnostics.foregroundQueued != 0,
		diagnostics.foregroundActive != 0,
		diagnostics.activeSpreadQueued != 0,
		diagnostics.activeSpreadActive != 0,
		false});
	jpegview_linux::InteractionWorkPolicy policy;
	const auto plan = policy.Plan(foregroundPending, {0, 1});
	jpegview_linux::ThumbnailCacheScheduler thumbnails;
	thumbnails.SetCatalog({"current", "visible", "distant"});
	thumbnails.SetCurrent(0);
	thumbnails.SetGeometry(100, 60);
	const auto thumbnail = thumbnails.TakeNext(0, 1, [&](const auto& request) {
		return plan.AllowsThumbnail(request.fileIndex) ||
			plan.Allows(jpegview_linux::PerfWorkClass::DistantSpeculation);
	});

	FileListScanWorker scanner;
	scanner.SetForegroundPending(plan.foregroundPending);
	const std::uint64_t scanGeneration = scanner.Request(FileList::InitialScanRequest(
		{temporary.path().string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	const auto yieldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!scanner.IsYieldingForForeground() &&
		std::chrono::steady_clock::now() < yieldDeadline) {
		std::this_thread::yield();
	}
	const bool scanYielded = scanner.IsYieldingForForeground();
	{
		std::lock_guard<std::mutex> lock(decodeMutex);
		releaseDecode = true;
	}
	decodeChanged.notify_all();
	const bool decodeIdle = cache.WaitUntilIdle(std::chrono::seconds(2));
	scanner.SetForegroundPending(false);
	std::vector<FileListScanResult> scanResults;
	const auto scanDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (scanResults.empty() && std::chrono::steady_clock::now() < scanDeadline) {
		scanResults = scanner.TakeReady();
		if (scanResults.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(reachedBarrier && diagnostics.foregroundActive == 0 &&
		diagnostics.backgroundActive == 1 && diagnostics.activeSpreadActive == 1 &&
		plan.foregroundPending && !plan.AllowsThumbnail(1) && thumbnail.empty() &&
		thumbnails.PendingCount() == 3 && scanYielded && scanResults.size() == 1 &&
		scanResults.front().generation == scanGeneration && decodeIdle,
		"pending active-spread decode did not pause thumbnail admission and yield list scanning");
}


void TestActiveSpreadAdmissionDuringForegroundPending() {
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "active-spread.png";
	WriteText(filename, "active spread source");

	std::mutex decodeMutex;
	std::condition_variable decodeChanged;
	bool decodeStarted = false;
	bool releaseDecode = false;
	coordinator.SetForegroundPending(true);
	jpegview_linux::DecodedImageCache decodedCache(1024,
		[&](const fs::path&, DecodedImage& image, std::string&) {
			{
				std::unique_lock<std::mutex> lock(decodeMutex);
				decodeStarted = true;
				decodeChanged.notify_all();
				decodeChanged.wait(lock, [&] { return releaseDecode; });
			}
			image = *CachedTestImage(64);
			return true;
		});
	decodedCache.RequestBackground(filename, {},
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool decodedWhileForegroundPending = false;
	{
		std::unique_lock<std::mutex> lock(decodeMutex);
		decodedWhileForegroundPending = decodeChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return decodeStarted; });
	}
	coordinator.SetForegroundPending(false);
	{
		std::lock_guard<std::mutex> lock(decodeMutex);
		releaseDecode = true;
	}
	decodeChanged.notify_all();
	const bool decodedIdle = decodedCache.WaitUntilIdle(std::chrono::seconds(2));
	const bool decodedCached = static_cast<bool>(decodedCache.Find(filename));

	std::atomic<bool> foregroundPending{true};
	std::atomic<bool> displayCpuAdmittedWhilePending{false};
	auto displayRequest = jpegview_linux::MakeDisplayImageRequest(filename,
		DisplayCacheTestImage(4, 4), 0, 2, 2, false, 1);
	displayRequest.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	coordinator.SetForegroundPending(true);
	jpegview_linux::DisplayImageCache displayCache(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			auto cpuLease = jpegview_linux::SourceWorkCoordinator::Global().AcquireCpu(
				request.workContext);
			displayCpuAdmittedWhilePending.store(static_cast<bool>(cpuLease) &&
				foregroundPending.load());
			return DisplayCachePreparedTestImage(request);
		});
	displayCache.RequestBackground(displayRequest);
	const bool displayIdleWhilePending = displayCache.WaitUntilIdle(
		std::chrono::seconds(2));
	foregroundPending.store(false);
	coordinator.SetForegroundPending(false);
	const bool displayIdleAfterRelease = displayIdleWhilePending ||
		displayCache.WaitUntilIdle(std::chrono::seconds(2));

	Expect(decodedWhileForegroundPending && decodedIdle && decodedCached &&
		displayIdleWhilePending && displayIdleAfterRelease &&
		displayCpuAdmittedWhilePending.load(),
		"active-spread decode or display processing yielded to its own foreground-pending state");
}


void TestDisplaySourceValidationWaitsForSourceAdmission() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "validation-admission.png";
	const fs::path blockerPath = temporary.path() / "validation-admission-blocker.jpg";
	WriteText(filename, "display source validation fixture");
	WriteText(blockerPath, "source lane blocker");
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(filename);
	Expect(source.Valid(), "display validation source fixture has no descriptor");

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const bool coordinatorIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(2));
	const jpegview_linux::WorkContext blockerContext =
		jpegview_linux::MakePathWorkContext(blockerPath,
			jpegview_linux::SourceWorkPriority::Metadata);
	jpegview_linux::SourceWorkLease blocker = coordinator.Acquire(blockerContext, blockerPath);
	const bool blockerAcquired = static_cast<bool>(blocker);
	std::atomic<int> processorCalls{0};
	jpegview_linux::DisplayImageCache cache(64, 1,
		[&processorCalls](const jpegview_linux::DisplayImageRequest& request) {
			++processorCalls;
			return DisplayCachePreparedTestImage(request);
		});
	auto request = jpegview_linux::MakeDisplayImageRequest(source,
		DisplayCacheTestImage(2, 2), 0, 2, 2, false, 2);
	request.workClass = jpegview_linux::PerfWorkClass::NearestNavigationNeighbor;
	cache.RequestBackground(request);
	const bool validationQueuedBehindSourceWork = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 1 && snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	blocker.Reset();
	const bool cacheIdle = cache.WaitUntilIdle(std::chrono::seconds(2));
	const auto completed = cache.TakeCompleted(1);
	Expect(coordinatorIdle && blockerAcquired &&
		validationQueuedBehindSourceWork && cacheIdle && processorCalls.load() == 1 &&
		completed.size() == 1,
		"ordinary display source validation bypassed cancellable source admission");

	const fs::path archive = temporary.path() / "display-validation.zip";
	const fs::path archivePayload = temporary.path() / "display-validation-payload.jpg";
	WriteText(archivePayload, "archive member validation fixture");
	WriteZipArchive(archive, {{"member.jpg", archivePayload}});
	const jpegview_linux::SourceDescriptor archiveSource =
		jpegview_linux::DescribeImageSource(archive / "member.jpg");
	std::atomic<int> archiveProcessorCalls{0};
	jpegview_linux::DisplayImageCache archiveCache(64, 1,
		[&archiveProcessorCalls](const jpegview_linux::DisplayImageRequest& request) {
			++archiveProcessorCalls;
			return DisplayCachePreparedTestImage(request);
		});
	auto archiveRequest = jpegview_linux::MakeDisplayImageRequest(archiveSource,
		DisplayCacheTestImage(2, 2), 0, 2, 2, false, 2);
	archiveRequest.workClass = jpegview_linux::PerfWorkClass::NearestNavigationNeighbor;
	archiveCache.RequestBackground(archiveRequest);
	const bool archiveCacheIdle = archiveCache.WaitUntilIdle(std::chrono::seconds(2));
	const auto archiveCompleted = archiveCache.TakeCompleted(1);
	const bool archivePermitsReleased = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(2));
	Expect(archiveSource.Valid() && archiveCacheIdle && archiveProcessorCalls.load() == 1 &&
		archiveCompleted.size() == 1 && archivePermitsReleased,
		"archive display validation failed to reuse its paired source and CPU admission");
}

void TestDisplayCachePromotionMetadataReachesUploadScheduler() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "promoted-spread.png";
	WriteText(filename, "promoted display request");
	const auto decoded = DisplayCacheTestImage(4, 4);
	auto neighborRequest = jpegview_linux::MakeDisplayImageRequest(
		filename, decoded, 0, 2, 2, false, 5);
	neighborRequest.workClass = jpegview_linux::PerfWorkClass::NearestNavigationNeighbor;
	jpegview_linux::DisplayImageCache cache(64, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
			auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			image->filename = request.filename;
			image->source = request.source;
			image->cacheKey = request.cacheKey;
			image->key = request.key;
			image->width = image->height = 2;
			image->priority = request.priority;
			image->workClass = request.workClass;
			image->bgra.assign(16, 255);
			return image;
		});
	cache.RequestBackground(neighborRequest);
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"neighbor display preparation did not complete before promotion");
	auto activeSpreadRequest = neighborRequest;
	activeSpreadRequest.priority = 1;
	activeSpreadRequest.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	cache.RequestBackgroundBatch({activeSpreadRequest});
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"cached display completion did not become available after active-spread promotion");
	const std::set<jpegview_linux::PerfWorkClass> activeOnly{
		jpegview_linux::PerfWorkClass::ActiveImageSpread};
	jpegview_linux::DisplayImageCompletionBatch completions(cache,
		cache.TakeCompletedWithMetadata(1, activeOnly));
	const auto& completion = completions.CompletionAt(0);
	Expect(completions.Size() == 1 && completion.priority == 1 &&
		completion.workClass == jpegview_linux::PerfWorkClass::ActiveImageSpread &&
		completion.image && completion.image->priority == 5 &&
		completion.image->workClass ==
			jpegview_linux::PerfWorkClass::NearestNavigationNeighbor,
		"cached promotion metadata was lost or the prepared payload was unexpectedly rewritten");
	const std::vector<jpegview_linux::DisplayUploadPriority> activeCandidate{{
		completion.workClass, completion.priority}};
	const auto selected = jpegview_linux::PlanDisplayTextureUploads(
		activeCandidate, activeOnly, 1);
	Expect(selected == std::vector<std::size_t>{0},
		"active-only upload scheduling rejected a promoted prepared neighbor");
	jpegview_linux::DisplayUploadPriority pendingPriority{
		jpegview_linux::PerfWorkClass::NearestNavigationNeighbor, 5};
	Expect(jpegview_linux::MergeDisplayUploadPriority(pendingPriority,
		{completion.workClass, completion.priority}) &&
		pendingPriority.workClass == jpegview_linux::PerfWorkClass::ActiveImageSpread &&
		pendingPriority.requestPriority == 1 &&
		jpegview_linux::PlanDisplayTextureUploads({pendingPriority}, activeOnly, 1) ==
			std::vector<std::size_t>{0},
		"a duplicate deferred upload did not adopt the promoted work classification");
}

void TestDisplayPrefetchDecodedOwnershipPolicy() {
	auto neighborPixels = DisplayCacheTestImage(4, 4);
	std::weak_ptr<const DecodedImage> neighborWeak = neighborPixels;
	auto neighborBatchPixels = jpegview_linux::RetainDisplayPrefetchDecodedImage(
		jpegview_linux::DisplayPrefetchBatchOwner::NeighborPlanner, neighborPixels);
	neighborPixels.reset();
	Expect(!neighborBatchPixels && neighborWeak.expired(),
		"neighbor prefetch bookkeeping retained decoded source pixels");

	TemporaryDirectory temporary;
	const fs::path evictedNeighbor = temporary.path() / "evicted-neighbor.png";
	const fs::path nearestImage = temporary.path() / "nearest-image.png";
	WriteText(evictedNeighbor, "neighbor decoded source");
	WriteText(nearestImage, "nearest decoded source");
	auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(32);
	jpegview_linux::DecodedImageCache decodedCache(32,
		[](const fs::path&, DecodedImage& image, std::string&) {
			image = *CachedTestImage(32);
			return true;
		}, budget);
	const auto neighborSource = jpegview_linux::DescribeImageSource(evictedNeighbor);
	decodedCache.Store(neighborSource, CachedTestImage(16));
	auto neighborCacheAlias = decodedCache.Find(neighborSource);
	neighborBatchPixels = jpegview_linux::RetainDisplayPrefetchDecodedImage(
		jpegview_linux::DisplayPrefetchBatchOwner::NeighborPlanner, neighborCacheAlias);
	neighborCacheAlias.reset();
	decodedCache.SetProtectionSnapshot({{neighborSource.Key(),
		jpegview_linux::CacheProtectionTier::DistantSpeculation}});
	const std::size_t evictedBytes = decodedCache.EvictLeastRecentlyUsed(
		jpegview_linux::CacheProtectionTier::DistantSpeculation);
	const auto capacityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (budget->Used() != 0 && std::chrono::steady_clock::now() < capacityDeadline) {
		std::this_thread::yield();
	}
	const bool capacityReturnedWhileBatchAlive = budget->Used() == 0 &&
		!neighborBatchPixels;
	const auto nearestSource = jpegview_linux::DescribeImageSource(nearestImage);
	std::mutex completionMutex;
	std::condition_variable completionChanged;
	bool nearestCompleted = false;
	decodedCache.RequestBackground(nearestSource,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			{
				std::lock_guard<std::mutex> lock(completionMutex);
				nearestCompleted = image && !image->frames.empty();
			}
			completionChanged.notify_all();
		}, jpegview_linux::PerfWorkClass::NearestNavigationNeighbor);
	const bool nearestIdle = decodedCache.WaitUntilIdle(std::chrono::seconds(2));
	bool callbackReceived = false;
	{
		std::unique_lock<std::mutex> lock(completionMutex);
		callbackReceived = completionChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return nearestCompleted; });
	}
	Expect(evictedBytes == 16 && capacityReturnedWhileBatchAlive && nearestIdle &&
		callbackReceived && decodedCache.Find(nearestSource) &&
		budget->Snapshot().decodedPixelBytes == 32,
		"live neighbor-batch bookkeeping blocked nearest decoded-cache admission after eviction");

	auto spreadPixels = DisplayCacheTestImage(4, 4);
	std::weak_ptr<const DecodedImage> spreadWeak = spreadPixels;
	auto spreadBatchPixels = jpegview_linux::RetainDisplayPrefetchDecodedImage(
		jpegview_linux::DisplayPrefetchBatchOwner::ActiveSpread, spreadPixels);
	spreadPixels.reset();
	Expect(spreadBatchPixels && !spreadWeak.expired(),
		"active-spread batch lost its required decoded source fallback");
	spreadBatchPixels.reset();
	Expect(spreadWeak.expired(),
		"active-spread source fallback remained owned after its batch released it");
}

void TestDisplayCacheProtectionSnapshotPreservesNeighborLru() {
	TemporaryDirectory temporary;
	const fs::path neighborAFile = temporary.path() / "neighbor-a.png";
	const fs::path neighborBFile = temporary.path() / "neighbor-b.png";
	const fs::path activeFile = temporary.path() / "active.png";
	WriteText(neighborAFile, "neighbor a");
	WriteText(neighborBFile, "neighbor b");
	WriteText(activeFile, "active");
	const auto decoded = DisplayCacheTestImage(2, 2);
	auto neighborA = jpegview_linux::MakeDisplayImageRequest(
		neighborAFile, decoded, 0, 2, 2, false, 1);
	auto neighborB = jpegview_linux::MakeDisplayImageRequest(
		neighborBFile, decoded, 0, 2, 2, false, 1);
	const std::vector<std::pair<jpegview_linux::DisplayImageCacheKey,
		jpegview_linux::CacheProtectionTier>> snapshot{
		{neighborA.cacheKey, jpegview_linux::CacheProtectionTier::Neighbor},
		{neighborB.cacheKey, jpegview_linux::CacheProtectionTier::Neighbor}};

	const auto evictsLeastRecentlyUsedNeighbor = [&](bool touchAFirst) {
		jpegview_linux::DisplayImageCache cache(32, 1,
			[](const jpegview_linux::DisplayImageRequest& request) {
				return DisplayCachePreparedTestImage(request);
			});
		cache.RequestBackgroundBatch({neighborA, neighborB});
		if (!cache.WaitUntilIdle(std::chrono::seconds(2))) return false;
		auto initialCompletions = cache.TakeCompleted(2);
		if (initialCompletions.size() != 2 || cache.CachedBytes() != 32) return false;
		initialCompletions.clear();

		cache.SetProtectionSnapshot(snapshot);
		if (touchAFirst) {
			if (!cache.Find(neighborA) || !cache.Find(neighborB)) return false;
		} else if (!cache.Find(neighborB) || !cache.Find(neighborA)) {
			return false;
		}
		for (int refresh = 0; refresh < 3; ++refresh) {
			cache.SetProtectionSnapshot(snapshot);
		}

		auto activeRequest = jpegview_linux::MakeDisplayImageRequest(
			activeFile, decoded, 0, 2, 2, false);
		activeRequest.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
		cache.RequestBackground(activeRequest);
		if (!cache.WaitUntilIdle(std::chrono::seconds(2))) return false;
		const auto& leastRecentlyUsed = touchAFirst ? neighborA : neighborB;
		const auto& mostRecentlyUsed = touchAFirst ? neighborB : neighborA;
		return cache.CachedBytes() == 32 && cache.CachedImages() == 2 &&
			!cache.Find(leastRecentlyUsed) && cache.Find(mostRecentlyUsed) &&
			cache.Find(activeRequest);
	};

	const bool aWasLeastRecent = evictsLeastRecentlyUsedNeighbor(true);
	const bool bWasLeastRecent = evictsLeastRecentlyUsedNeighbor(false);
	Expect(aWasLeastRecent && bWasLeastRecent,
		"repeated identical protection snapshots changed which touched neighbor was evicted");
}

void TestDecodedCacheProtectionSnapshotPreservesLruAcrossTierChanges() {
	TemporaryDirectory temporary;
	const fs::path imageAFile = temporary.path() / "decoded-a.png";
	const fs::path imageBFile = temporary.path() / "decoded-b.png";
	WriteText(imageAFile, "decoded a");
	WriteText(imageBFile, "decoded b");
	const auto imageA = jpegview_linux::DescribeImageSource(imageAFile).Key();
	const auto imageB = jpegview_linux::DescribeImageSource(imageBFile).Key();
	const auto exerciseTransition = [&](jpegview_linux::CacheProtectionTier initialTier,
		jpegview_linux::CacheProtectionTier targetTier, bool touchAFirst) {
		jpegview_linux::DecodedImageCache cache(32);
		cache.Store(imageAFile, CachedTestImage(16));
		cache.Store(imageBFile, CachedTestImage(16));
		cache.SetProtectionSnapshot({{imageA, initialTier}, {imageB, initialTier}});
		const bool touched = touchAFirst ?
			static_cast<bool>(cache.Find(imageAFile)) &&
			static_cast<bool>(cache.Find(imageBFile)) :
			static_cast<bool>(cache.Find(imageBFile)) &&
			static_cast<bool>(cache.Find(imageAFile));
		if (!touched) return false;

		cache.SetProtectionSnapshot({{imageA, targetTier}, {imageB, targetTier}});
		const auto& leastRecentlyUsed = touchAFirst ? imageAFile : imageBFile;
		const auto& mostRecentlyUsed = touchAFirst ? imageBFile : imageAFile;
		return cache.EvictLeastRecentlyUsed(targetTier) == 16 &&
			!cache.Find(leastRecentlyUsed) && cache.Find(mostRecentlyUsed);
	};
	const auto distant = jpegview_linux::CacheProtectionTier::DistantSpeculation;
	const auto neighbor = jpegview_linux::CacheProtectionTier::Neighbor;
	Expect(exerciseTransition(distant, neighbor, true) &&
		exerciseTransition(distant, neighbor, false),
		"decoded-cache promotion snapshot did not preserve both opposite neighbor touch orders");
	Expect(exerciseTransition(neighbor, distant, true) &&
		exerciseTransition(neighbor, distant, false),
		"decoded-cache demotion snapshot did not preserve both opposite distant touch orders");
}

void TestDecodedCacheProtectionReconcilesActiveReservationsFirst() {
	TemporaryDirectory temporary;
	const fs::path leavingActiveFile = temporary.path() / "leaving-active.png";
	const fs::path stayingActiveFile = temporary.path() / "staying-active.png";
	WriteText(leavingActiveFile, "leaving active");
	WriteText(stayingActiveFile, "staying active");
	const auto leavingActive = jpegview_linux::DescribeImageSource(leavingActiveFile).Key();
	const auto stayingActive = jpegview_linux::DescribeImageSource(stayingActiveFile).Key();
	auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(16);
	jpegview_linux::DecodedImageCache cache(32, {}, budget);

	cache.Store(leavingActiveFile, CachedTestImage(16));
	cache.PromoteToActiveUse(leavingActive);
	cache.Store(stayingActiveFile, CachedTestImage(16));
	Expect(budget->Snapshot().retainedBytes == 16 &&
		budget->Snapshot().activeWorkingBytes == 16,
		"decoded protection ordering fixture did not fill retained capacity with an active entry");

	// The lower-tier transition appears first in active LRU order. Its working
	// allocation can be retained only after the unchanged active entry releases
	// its previously retained reservation.
	cache.SetProtectionSnapshot({
		{leavingActive, jpegview_linux::CacheProtectionTier::Neighbor},
		{stayingActive, jpegview_linux::CacheProtectionTier::Active}});
	const jpegview_linux::CacheBudgetSnapshot snapshot = budget->Snapshot();
	Expect(cache.CachedImages() == 2 && cache.Find(leavingActiveFile) &&
		cache.Find(stayingActiveFile) && snapshot.retainedBytes == 16 &&
		snapshot.activeWorkingBytes == 16,
		"neighbor promotion ran before active reservation reconciliation released capacity");
}

void TestDisplayTexturePinHandoffPreservesBorrowedWorkingTexture() {
	const std::string borrowedKey = "outgoing-active-working-frame";
	std::string transitionKey = "older-transition-frame";
	std::string pendingTransitionKey;
	std::string captureKey;
	std::string lastPresentedKey;
	bool activeWorkingTextureDestroyed = false;
	Expect(!jpegview_linux::IsDisplayTexturePinned(borrowedKey, "", "", "", "", false),
		"transition handoff fixture accidentally starts with another pin protecting its texture");
	const auto sweepActiveWorkingTexture = [&] {
		if (!jpegview_linux::IsDisplayTexturePinned(borrowedKey, transitionKey,
			pendingTransitionKey, captureKey, lastPresentedKey, false)) {
			activeWorkingTextureDestroyed = true;
		}
	};

	{
		jpegview_linux::DisplayTexturePinHandoff handoff(captureKey, borrowedKey);
		transitionKey.clear();
		pendingTransitionKey.clear();
		sweepActiveWorkingTexture();
		Expect(!activeWorkingTextureDestroyed,
			"clearing the older transition destroyed the borrowed outgoing texture");

		transitionKey = borrowedKey;
		handoff.Restore();
		sweepActiveWorkingTexture();
		Expect(!activeWorkingTextureDestroyed,
			"releasing the temporary pin destroyed a texture already owned by the new transition");
	}
	Expect(captureKey.empty(), "transition pin handoff did not restore the prior capture pin");
	transitionKey.clear();
	Expect(!jpegview_linux::IsDisplayTexturePinned(borrowedKey, transitionKey,
		pendingTransitionKey, captureKey, lastPresentedKey, false),
		"completed transition retained a stale borrowed-texture pin");
	Expect(!jpegview_linux::IsDisplayTexturePinned("", "", "", "", "", true),
		"an empty display key was considered pinned");
	Expect(jpegview_linux::IsDisplayTexturePinned(borrowedKey, "", "", "",
		borrowedKey, true) && !jpegview_linux::IsDisplayTexturePinned(borrowedKey,
		"", "", "", borrowedKey, false),
		"last-presented display texture was not pinned only for the current source");
	const std::string spreadAnchorKey = "active-spread-anchor";
	const std::string spreadPartnerKey = "active-spread-partner";
	Expect(jpegview_linux::IsDisplayTexturePinned(spreadAnchorKey, "", "", "",
		"", false, spreadAnchorKey, spreadPartnerKey) &&
		jpegview_linux::IsDisplayTexturePinned(spreadPartnerKey, "", "", "",
			"", false, spreadAnchorKey, spreadPartnerKey) &&
		!jpegview_linux::IsDisplayTexturePinned("unrelated-frame", "", "", "",
			"", false, spreadAnchorKey, spreadPartnerKey),
		"active spread textures were not pinned as a pair while awaiting presentation");
	Expect(jpegview_linux::IsDisplayTexturePinned(spreadAnchorKey, "", "", "",
		"new-selected-frame", false, "", "", spreadAnchorKey, spreadPartnerKey) &&
		jpegview_linux::IsDisplayTexturePinned(spreadPartnerKey, "", "", "",
			"new-selected-frame", false, "", "", spreadAnchorKey, spreadPartnerKey) &&
		!jpegview_linux::IsDisplayTexturePinned("unrelated-frame", "", "", "",
			"new-selected-frame", false, "", "", spreadAnchorKey, spreadPartnerKey),
		"replacing selection unpinned an outgoing spread before its replacement was drawable");
	Expect(!jpegview_linux::IsDisplayTexturePinned(spreadAnchorKey, "", "", "",
		"new-selected-frame", true, "", "", "", "") &&
		!jpegview_linux::IsDisplayTexturePinned(spreadPartnerKey, "", "", "",
			"new-selected-frame", true, "", "", "", ""),
		"releasing an outgoing presentation left stale texture pins");
}

void TestDisplayImageCacheForegroundActiveClassification() {
	TemporaryDirectory temporary;
	const fs::path foregroundFile = temporary.path() / "fresh-foreground.jpg";
	const fs::path neighborFile = temporary.path() / "promoted-neighbor.jpg";
	WriteText(foregroundFile, "fresh foreground");
	WriteText(neighborFile, "promoted neighbor");
	const auto decoded = DisplayCacheTestImage(4, 4);
	const auto freshRequest = jpegview_linux::MakeDisplayImageRequest(
		foregroundFile, decoded, 0, 2, 2, false);
	const auto promotedRequest = jpegview_linux::MakeDisplayImageRequest(
		neighborFile, decoded, 0, 2, 2, false, 1);

	std::mutex freshMutex;
	std::condition_variable freshChanged;
	bool freshStarted = false;
	bool releaseFresh = false;
	jpegview_linux::PerfContext freshContext;
	jpegview_linux::DisplayImageCache fresh(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			{
				std::unique_lock<std::mutex> lock(freshMutex);
				freshContext = jpegview_linux::CurrentPerfContext();
				freshStarted = true;
				freshChanged.notify_all();
				freshChanged.wait(lock, [&] { return releaseFresh; });
			}
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = request.key;
			result->source = request.source;
			result->cacheKey = request.cacheKey;
			result->width = result->height = 2;
			result->bgra.assign(16, 255);
			return result;
		});
	fresh.Request(freshRequest);
	bool freshReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(freshMutex);
		freshReachedBarrier = freshChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return freshStarted; });
	}
	for (int repeat = 0; repeat < 10; ++repeat) fresh.Request(freshRequest);
	const auto freshStats = fresh.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(freshMutex);
		releaseFresh = true;
	}
	freshChanged.notify_all();
	Expect(freshReachedBarrier && fresh.WaitUntilIdle(std::chrono::seconds(2)),
		"fresh foreground display work did not reach and leave its bounded barrier");
	Expect(freshStats.foregroundActive == 1 && freshStats.backgroundActive == 0 &&
		freshContext.workClass == jpegview_linux::PerfWorkClass::ActiveImageSpread &&
		freshContext.execution == jpegview_linux::PerfExecution::WorkerThread,
		"fresh foreground display request was not classified from active Work state");

	std::mutex promoteMutex;
	std::condition_variable promoteChanged;
	bool promoteStarted = false;
	bool releasePromoted = false;
	jpegview_linux::PerfContext promotedContext;
	jpegview_linux::DisplayImageCache promoted(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			{
				std::unique_lock<std::mutex> lock(promoteMutex);
				promotedContext = jpegview_linux::CurrentPerfContext();
				promoteStarted = true;
				promoteChanged.notify_all();
				promoteChanged.wait(lock, [&] { return releasePromoted; });
			}
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = request.key;
			result->source = request.source;
			result->cacheKey = request.cacheKey;
			result->width = result->height = 2;
			result->bgra.assign(16, 255);
			return result;
		});
	promoted.RequestBackground(promotedRequest);
	bool promotionReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(promoteMutex);
		promotionReachedBarrier = promoteChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return promoteStarted; });
	}
	if (promotionReachedBarrier) promoted.Request(promotedRequest);
	const auto promotedStats = promoted.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(promoteMutex);
		releasePromoted = true;
	}
	promoteChanged.notify_all();
	Expect(promotionReachedBarrier && promoted.WaitUntilIdle(std::chrono::seconds(2)),
		"promoted speculative display work did not leave its bounded barrier");
	Expect(promotedStats.foregroundActive == 1 && promotedStats.backgroundActive == 0 &&
		promotedContext.workClass == jpegview_linux::PerfWorkClass::NearestNavigationNeighbor &&
		promotedContext.execution == jpegview_linux::PerfExecution::WorkerThread,
		"in-flight foreground promotion was not reflected in active diagnostics");

	auto previewRequest = jpegview_linux::MakeDisplayImageRequest(
		foregroundFile, decoded, 0, 1, 1, false, 1000000);
	previewRequest.workClass = jpegview_linux::PerfWorkClass::FocusedPreview;
	jpegview_linux::PerfContext previewContext;
	jpegview_linux::DisplayImageCache preview(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			previewContext = jpegview_linux::CurrentPerfContext();
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = request.key;
			result->source = request.source;
			result->cacheKey = request.cacheKey;
			result->width = result->height = 1;
			result->bgra.assign(4, 255);
			return result;
		});
	preview.RequestBackground(previewRequest);
	Expect(preview.WaitUntilIdle(std::chrono::seconds(2)) &&
		previewContext.workClass == jpegview_linux::PerfWorkClass::FocusedPreview &&
		previewContext.execution == jpegview_linux::PerfExecution::WorkerThread,
		"focused preview work-class context did not reach its display worker");
}

void TestDisplayImageCachePrefetchPreservesActiveForeground() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "active-foreground-prefetch.jpg";
	WriteText(filename, "active foreground");
	const fs::path neighborFilename = temporary.path() / "closer-neighbor.jpg";
	WriteText(neighborFilename, "closer neighbor");
	const auto decoded = DisplayCacheTestImage(4, 4);
	const auto request = jpegview_linux::MakeDisplayImageRequest(
		filename, decoded, 0, 2, 2, false);
	const auto neighborRequest = jpegview_linux::MakeDisplayImageRequest(
		neighborFilename, decoded, 0, 2, 2, false, 1);
	std::mutex mutex;
	std::condition_variable changed;
	bool started = false;
	bool release = false;
	bool neighborFinished = false;
	jpegview_linux::DisplayImageCache cache(64, 2,
		[&](const jpegview_linux::DisplayImageRequest& pending) {
			if (pending.key == request.key) {
				std::unique_lock<std::mutex> lock(mutex);
				started = true;
				changed.notify_all();
				changed.wait(lock, [&] { return release; });
			} else {
				std::lock_guard<std::mutex> lock(mutex);
				neighborFinished = true;
				changed.notify_all();
			}
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = pending.key;
			result->source = pending.source;
			result->cacheKey = pending.cacheKey;
			result->width = result->height = 2;
			result->bgra.assign(16, 255);
			return result;
		});
	cache.Request(request);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return started;
		});
	}
	cache.RequestBackground(neighborRequest);
	bool neighborReachedCompletion = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		neighborReachedCompletion = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return neighborFinished;
		});
	}
	bool neighborPublished = false;
	const auto neighborDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (neighborReachedCompletion && std::chrono::steady_clock::now() < neighborDeadline) {
		if (cache.GetDiagnostics().preparedImages == 1) {
			neighborPublished = true;
			break;
		}
		std::this_thread::yield();
	}
	const auto blockedNeighbor = cache.TakeCompleted(1);
	const auto beforePrefetch = cache.GetDiagnostics();
	cache.Prefetch({request});
	const auto afterPrefetch = cache.GetDiagnostics();
	const auto blockedAfterPrefetch = cache.TakeCompleted(1);
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	changed.notify_all();
	Expect(reachedBarrier && cache.WaitUntilIdle(std::chrono::seconds(2)),
		"foreground-prefetch fixture did not leave its bounded worker barrier");
	Expect(neighborPublished && blockedNeighbor.empty() && blockedAfterPrefetch.empty(),
		"prefetch weakened the zero-priority foreground barrier ahead of a closer ready neighbor");
	Expect(beforePrefetch.foregroundActive == 1 && beforePrefetch.backgroundActive == 0 &&
		afterPrefetch.foregroundActive == 1 && afterPrefetch.backgroundActive == 0,
		"prefetch changed active foreground/background classification or lowered foreground priority");
}

void TestDisplayImageCacheEmptyPrefetchPreservesForegroundAndSpread() {
	TemporaryDirectory temporary;
	const auto makeRequest = [&](const char* leaf, std::size_t priority,
		jpegview_linux::PerfWorkClass workClass) {
		const fs::path filename = temporary.path() / leaf;
		WriteText(filename, leaf);
		auto request = jpegview_linux::MakeDisplayImageRequest(filename,
			DisplayCacheTestImage(4, 4), 0, 2, 2, false, priority);
		request.workClass = workClass;
		return request;
	};
	const auto inFlightSpread = makeRequest("in-flight-spread.png", 1,
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	const auto directForeground = makeRequest("direct-foreground.png", 0,
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	const auto queuedSpread = makeRequest("queued-spread.png", 1,
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	std::mutex mutex;
	std::condition_variable changed;
	bool spreadStarted = false;
	bool releaseSpread = false;
	const auto processor = [&](const jpegview_linux::DisplayImageRequest& request) {
		if (request.key == inFlightSpread.key) {
			std::unique_lock<std::mutex> lock(mutex);
			spreadStarted = true;
			changed.notify_all();
			changed.wait(lock, [&] { return releaseSpread; });
		}
		auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>();
		image->key = request.key;
		image->source = request.source;
		image->cacheKey = request.cacheKey;
		image->width = image->height = 2;
		image->workClass = request.workClass;
		image->bgra.assign(16, 255);
		return image;
	};
	jpegview_linux::DisplayImageCache cache(128, 1, processor);
	ScopedConditionRelease spreadRelease(mutex, changed, releaseSpread);
	cache.RequestBackground(inFlightSpread);
	bool inFlightReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		inFlightReachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return spreadStarted; });
	}
	cache.Request(directForeground);
	cache.RequestBackgroundBatch({queuedSpread});
	const auto queuedStats = cache.GetDiagnostics();
	cache.Prefetch({});
	const auto retainedStats = cache.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseSpread = true;
	}
	changed.notify_all();
	const bool cacheIdle = cache.WaitUntilIdle(std::chrono::seconds(2));
	const auto completed = cache.TakeCompleted(3);
	std::set<std::string> completedKeys;
	for (const auto& image : completed) {
		if (image) completedKeys.insert(image->key);
	}
	Expect(inFlightReachedBarrier && cacheIdle &&
		queuedStats.foregroundQueued == 1 && queuedStats.backgroundQueued == 1 &&
		retainedStats.foregroundQueued == 1 && retainedStats.backgroundQueued == 1 &&
		completedKeys == std::set<std::string>{inFlightSpread.key,
			directForeground.key, queuedSpread.key},
		"empty neighbor prefetch canceled foreground or active-spread display work");
}

void TestDisplayCacheCanceledSpreadStaysCanceledAcrossEmptyPrefetch() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "retired-spread-partner.png";
	WriteText(filename, "retired spread partner source");
	auto request = jpegview_linux::MakeDisplayImageRequest(
		filename, DisplayCacheTestImage(4, 4), 0, 2, 2, false, 1);
	request.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	std::mutex mutex;
	std::condition_variable changed;
	bool processorStarted = false;
	bool releaseProcessor = false;
	std::shared_ptr<std::atomic<bool>> cancellation;
	jpegview_linux::DisplayImageCache cache(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& activeRequest) {
			{
				std::unique_lock<std::mutex> lock(mutex);
				cancellation = activeRequest.cancellation;
				processorStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseProcessor; });
			}
			auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			image->key = activeRequest.key;
			image->source = activeRequest.source;
			image->cacheKey = activeRequest.cacheKey;
			image->width = image->height = 2;
			image->workClass = activeRequest.workClass;
			image->bgra.assign(16, 255);
			return image;
		});
	ScopedConditionRelease releaseOnExit(mutex, changed, releaseProcessor);
	cache.RequestBackground(request);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return processorStarted; });
	}
	const auto beforeCancel = cache.GetDiagnostics();
	cache.CancelBackground(request.key);
	const bool canceledBeforePrefetch = cancellation && cancellation->load();
	const auto afterCancel = cache.GetDiagnostics();
	cache.Prefetch({});
	const bool stayedCanceled = cancellation && cancellation->load();
	const auto afterEmptyPrefetch = cache.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseProcessor = true;
	}
	changed.notify_all();
	const bool becameIdle = cache.WaitUntilIdle(std::chrono::seconds(2));
	const bool canceledAfterCompletion = cancellation && cancellation->load();
	const auto completed = cache.TakeCompleted(1, {
		jpegview_linux::PerfWorkClass::ActiveImageSpread});
	Expect(reachedBarrier && beforeCancel.backgroundActive == 1 &&
		canceledBeforePrefetch && afterCancel.backgroundActive == 1 && stayedCanceled &&
		afterEmptyPrefetch.backgroundActive == 1 && becameIdle && completed.empty() &&
		canceledAfterCompletion && cache.Find(request) == nullptr && cache.CachedImages() == 0,
		"empty prefetch resurrected explicitly canceled in-flight active-spread display work");
}

void TestWorkBatchGateSerializesDeactivateAndPublish() {
	jpegview_linux::WorkBatchGate delayedGate;
	std::mutex delayedMutex;
	std::condition_variable delayedChanged;
	bool completionCopied = false;
	bool releaseCompletion = false;
	bool cacheCleared = false;
	int delayedEnqueues = 0;
	std::thread delayedCompletion([&] {
		{
			std::unique_lock<std::mutex> lock(delayedMutex);
			completionCopied = true;
			delayedChanged.notify_all();
			delayedChanged.wait(lock, [&] { return releaseCompletion; });
		}
		(void)delayedGate.Publish([&] { ++delayedEnqueues; });
	});
	bool copied = false;
	{
		std::unique_lock<std::mutex> lock(delayedMutex);
		copied = delayedChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return completionCopied; });
	}
	delayedGate.Deactivate();
	cacheCleared = true;
	{
		std::lock_guard<std::mutex> lock(delayedMutex);
		releaseCompletion = true;
	}
	delayedChanged.notify_all();
	delayedCompletion.join();
	const bool lateCompletionRejected = copied && cacheCleared && delayedEnqueues == 0;

	jpegview_linux::WorkBatchGate publishingGate;
	std::mutex publishingMutex;
	std::condition_variable publishingChanged;
	bool publicationEntered = false;
	bool releasePublication = false;
	std::vector<int> ordering;
	std::thread inFlightCompletion([&] {
		publishingGate.Publish([&] {
			std::unique_lock<std::mutex> lock(publishingMutex);
			publicationEntered = true;
			publishingChanged.notify_all();
			publishingChanged.wait(lock, [&] { return releasePublication; });
			ordering.push_back(1);
		});
	});
	bool entered = false;
	{
		std::unique_lock<std::mutex> lock(publishingMutex);
		entered = publishingChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return publicationEntered; });
	}
	bool deactivated = false;
	std::thread deactivation([&] {
		publishingGate.Deactivate();
		std::lock_guard<std::mutex> lock(publishingMutex);
		ordering.push_back(2);
		deactivated = true;
	});
	{
		std::lock_guard<std::mutex> lock(publishingMutex);
		releasePublication = true;
	}
	publishingChanged.notify_all();
	inFlightCompletion.join();
	deactivation.join();
	Expect(lateCompletionRejected && entered && deactivated &&
		ordering == std::vector<int>{1, 2},
		"batch deactivation raced a copied or already-publishing completion");
}

void TestDisplayCacheRetirementIdentityAccounting() {
	TemporaryDirectory temporary;
	const fs::path fileA = temporary.path() / "borrowed.jpg";
	const fs::path fileB = temporary.path() / "find-retire.jpg";
	const fs::path fileC = temporary.path() / "wait-retire.jpg";
	WriteText(fileA, "borrowed");
	WriteText(fileB, "find-retire");
	WriteText(fileC, "wait-retire");
	const auto decoded = DisplayCacheTestImage(4, 4);
	const auto requestA = jpegview_linux::MakeDisplayImageRequest(fileA, decoded, 0, 2, 2, false, 1);
	const auto requestB = jpegview_linux::MakeDisplayImageRequest(fileB, decoded, 0, 2, 2, false);
	const auto requestC = jpegview_linux::MakeDisplayImageRequest(fileC, decoded, 0, 2, 2, false);
	jpegview_linux::DisplayImageCache cache(128, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = request.key;
			result->source = request.source;
			result->cacheKey = request.cacheKey;
			result->width = result->height = 2;
			result->bgra.assign(16, 255);
			return result;
		});
	cache.RequestBackground(requestA);
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"borrowed-image fixture did not finish before its bounded deadline");
	const auto completed = cache.TakeCompleted(1);
	Expect(completed.size() == 1 && completed.front(),
		"display cache did not return one completed allocation for borrowing");
	const jpegview_linux::DisplayImageCache::ImagePtr borrowedA = completed.front();
	const jpegview_linux::DisplayImageCache::ImagePtr requestedB = cache.RequestAndWait(requestB);
	const jpegview_linux::DisplayImageCache::ImagePtr foundB = cache.Find(requestB);
	Expect(requestedB && foundB && requestedB.get() == foundB.get(),
		"Find and RequestAndWait did not resolve to the same retained display allocation");
	cache.Release(requestB.key);
	cache.Retire(foundB);
	cache.Retire(requestedB);
	const auto afterB = cache.GetDiagnostics();
	Expect(afterB.borrowedBytes == 16 && afterB.borrowedImages == 1 &&
		afterB.retiredBytes == 16 && afterB.retiredImages == 1,
		"retiring a looked-up image changed another allocation's borrow or counted twice");

	const jpegview_linux::DisplayImageCache::ImagePtr requestedC = cache.RequestAndWait(requestC);
	Expect(requestedC != nullptr, "RequestAndWait did not return its prepared display allocation");
	cache.Release(requestC.key);
	cache.Retire(requestedC);
	cache.Retire(requestedC);
	const auto afterC = cache.GetDiagnostics();
	Expect(afterC.borrowedBytes == 16 && afterC.borrowedImages == 1 &&
		afterC.retiredBytes == 32 && afterC.retiredImages == 2,
		"duplicate release/retirement counted one allocation more than once");

	cache.Retire(borrowedA);
	cache.Release(requestA.key);
	cache.Retire(borrowedA);
	const auto afterA = cache.GetDiagnostics();
	Expect(afterA.borrowedBytes == 0 && afterA.borrowedImages == 0 &&
		afterA.retiredImages == 3 && afterA.retiredBytes == 48,
		"retiring a borrowed allocation did not reconcile bytes by image identity");
}

void TestDisplayCacheFinalPixelsAreDestroyedByRetirementWorker() {
	struct DestructionProbe {
		std::mutex mutex;
		std::condition_variable changed;
		std::thread::id thread;
		bool destroyed = false;
	};
	const auto makeProcessor = [](const std::shared_ptr<DestructionProbe>& probe) {
		return [probe](const jpegview_linux::DisplayImageRequest& request) {
			auto* image = new jpegview_linux::PreparedDisplayImage;
			image->key = request.key;
			image->source = request.source;
			image->cacheKey = request.cacheKey;
			image->width = image->height = 2;
			image->bgra.assign(16, 0);
			return jpegview_linux::DisplayImageCache::ImagePtr(image,
				[probe](const jpegview_linux::PreparedDisplayImage* retired) {
					{
						std::lock_guard<std::mutex> lock(probe->mutex);
						probe->thread = std::this_thread::get_id();
						probe->destroyed = true;
					}
					delete retired;
					probe->changed.notify_all();
				});
		};
	};
	const auto wasDestroyed = [](const std::shared_ptr<DestructionProbe>& probe) {
		std::unique_lock<std::mutex> lock(probe->mutex);
		return probe->changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return probe->destroyed;
		});
	};

	TemporaryDirectory temporary;
	const fs::path uploadRaceFile = temporary.path() / "retire-after-drain.jpg";
	WriteText(uploadRaceFile, "retirement race");
	const auto uploadRaceRequest = jpegview_linux::MakeJpegDisplayImageRequest(
		uploadRaceFile, 4, 4, 2, 2, false);
	const std::thread::id eventThread = std::this_thread::get_id();
	auto uploadRaceProbe = std::make_shared<DestructionProbe>();
	jpegview_linux::DisplayImageCache uploadRace(64, 1, makeProcessor(uploadRaceProbe));
	uploadRace.Request(uploadRaceRequest);
	Expect(uploadRace.WaitUntilIdle(std::chrono::seconds(2)),
		"retirement-race fixture did not finish preparing before its bounded deadline");
	auto completed = uploadRace.TakeCompleted(1);
	Expect(completed.size() == 1 && completed.front(),
		"retirement-race fixture did not publish its prepared image");
	jpegview_linux::DisplayImageCache::ImagePtr pixels = std::move(completed.front());
	completed.clear();
	uploadRace.Release(uploadRaceRequest.key);
	bool queueDrained = false;
	const auto retirementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < retirementDeadline) {
		const auto stats = uploadRace.GetDiagnostics();
		if (stats.retiredBytes == 16 && stats.retiredImages == 1) {
			queueDrained = true;
			break;
		}
		std::this_thread::yield();
	}
	Expect(queueDrained && pixels.use_count() == 2,
		"retirement worker did not retain the final owner after draining the upload queue");
	uploadRace.Retire(pixels);
	uploadRace.Retire(pixels);
	const auto duplicateStats = uploadRace.GetDiagnostics();
	Expect(duplicateStats.retiredImages == 1 && duplicateStats.retiredBytes == 16,
		"retiring again while ownership is shared duplicated its queued owner or byte count");
	pixels.reset();
	Expect(wasDestroyed(uploadRaceProbe),
		"the retired display pixels were not destroyed before the bounded deadline");
	{
		std::lock_guard<std::mutex> lock(uploadRaceProbe->mutex);
		Expect(uploadRaceProbe->thread != eventThread,
			"the final prepared-pixel owner was destroyed on the event thread after upload retirement");
	}

	const fs::path shutdownFile = temporary.path() / "retire-on-shutdown.jpg";
	WriteText(shutdownFile, "retirement shutdown");
	const auto shutdownRequest = jpegview_linux::MakeJpegDisplayImageRequest(
		shutdownFile, 4, 4, 2, 2, false);
	auto shutdownProbe = std::make_shared<DestructionProbe>();
	auto shutdownCache = std::make_unique<jpegview_linux::DisplayImageCache>(
		64, 1, makeProcessor(shutdownProbe));
	shutdownCache->Request(shutdownRequest);
	Expect(shutdownCache->WaitUntilIdle(std::chrono::seconds(2)),
		"shutdown-retirement fixture did not finish before cache teardown");
	shutdownCache.reset();
	Expect(wasDestroyed(shutdownProbe),
		"cache shutdown returned before draining its prepared-pixel retirement");
	{
		std::lock_guard<std::mutex> lock(shutdownProbe->mutex);
		Expect(shutdownProbe->thread != eventThread,
			"cache shutdown destroyed its final prepared pixels on the event thread");
	}

	const fs::path outlivingFile = temporary.path() / "handle-outlives-cache.jpg";
	WriteText(outlivingFile, "handle outlives cache");
	const auto outlivingRequest = jpegview_linux::MakeJpegDisplayImageRequest(
		outlivingFile, 4, 4, 2, 2, false);
	auto outlivingProbe = std::make_shared<DestructionProbe>();
	auto cache = std::make_unique<jpegview_linux::DisplayImageCache>(
		64, 1, makeProcessor(outlivingProbe));
	jpegview_linux::DisplayImageCache::ImagePtr outlivingPixels =
		cache->RequestAndWait(outlivingRequest);
	Expect(outlivingPixels != nullptr,
		"RequestAndWait did not return the handle used for the cache-lifetime regression");
	std::mutex teardownMutex;
	std::condition_variable teardownChanged;
	bool teardownFinished = false;
	std::thread teardown([ownedCache = std::move(cache), &teardownMutex,
		&teardownChanged, &teardownFinished]() mutable {
		ownedCache.reset();
		{
			std::lock_guard<std::mutex> lock(teardownMutex);
			teardownFinished = true;
		}
		teardownChanged.notify_all();
	});
	const std::thread::id teardownThread = teardown.get_id();
	bool cacheReturnedBeforeHandleRelease = false;
	{
		std::unique_lock<std::mutex> lock(teardownMutex);
		cacheReturnedBeforeHandleRelease = teardownChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return teardownFinished; });
	}
	outlivingPixels.reset();
	teardown.join();
	Expect(cacheReturnedBeforeHandleRelease,
		"cache destruction waited for a valid ImagePtr that outlived the cache");
	Expect(wasDestroyed(outlivingProbe),
		"outliving display pixels were not destroyed after the external handle was released");
	{
		std::lock_guard<std::mutex> lock(outlivingProbe->mutex);
		Expect(outlivingProbe->thread != eventThread && outlivingProbe->thread != teardownThread,
			"outliving display pixels were not destroyed by the retirement worker");
	}
}

void TestDisplayCompletionBatchRetiresEveryDeferredPairResultOffCaller() {
	struct DestructionState {
		std::mutex mutex;
		std::condition_variable changed;
		std::size_t destroyed = 0;
		bool destroyedOffCaller = true;
	};

	TemporaryDirectory temporary;
	const std::thread::id caller = std::this_thread::get_id();
	auto sharedBudget = std::make_shared<jpegview_linux::SharedCacheBudget>(0);
	auto destruction = std::make_shared<DestructionState>();
	const auto decoded = DisplayCacheTestImage(2, 2);
	const auto makeRequest = [&](const std::string& name, std::size_t priority) {
		const fs::path filename = temporary.path() / name;
		WriteText(filename, name);
		auto request = jpegview_linux::MakeDisplayImageRequest(filename, decoded,
			0, 512, 512, false, priority);
		request.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
		return request;
	};
	const auto first = makeRequest("first.png", 0);
	const auto second = makeRequest("second.png", 1);
	const auto processor = [destruction, caller](
		const jpegview_linux::DisplayImageRequest& request) {
		auto* image = new jpegview_linux::PreparedDisplayImage;
		image->filename = request.filename;
		image->source = request.source;
		image->cacheKey = request.cacheKey;
		image->key = request.key;
		image->width = image->height = 512;
		image->bgra.assign(512u * 512u * 4u, 96);
		return jpegview_linux::DisplayImageCache::ImagePtr(image,
			[destruction, caller](const jpegview_linux::PreparedDisplayImage* retired) {
				{
					std::lock_guard<std::mutex> lock(destruction->mutex);
					++destruction->destroyed;
					destruction->destroyedOffCaller = destruction->destroyedOffCaller &&
						std::this_thread::get_id() != caller;
				}
				destruction->changed.notify_all();
				delete retired;
			});
	};
	jpegview_linux::DisplayImageCache cache(0, 2, processor, sharedBudget);
	cache.RequestBackgroundBatch({first, second});
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"two-page upload completions did not finish before their bounded deadline");
	const std::set<jpegview_linux::PerfWorkClass> activeOnly{
		jpegview_linux::PerfWorkClass::ActiveImageSpread};
	{
		jpegview_linux::DisplayImageCompletionBatch batch(cache,
			cache.TakeCompleted(2, activeOnly));
		Expect(batch.Size() == 2 && sharedBudget->Snapshot().activeWorkingBytes ==
			2u * 512u * 512u * 4u,
			"unretained spread completions did not keep both pixel reservations accounted");
		jpegview_linux::DisplayImageCache::ImagePtr deferred = batch.Take(0);
		cache.ReleaseForUpload(deferred);
		jpegview_linux::CacheAdmissionPolicy admission(*sharedBudget);
		const auto blockedTexture = admission.Reserve(512u * 512u * 4u,
			jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
			jpegview_linux::CacheProtectionTier::Active, {}, false);
		Expect(!blockedTexture,
			"zero retained capacity did not defer the first spread texture upload");
		cache.Retire(deferred);
		deferred.reset();
	}
	{
		std::unique_lock<std::mutex> lock(destruction->mutex);
		Expect(destruction->changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return destruction->destroyed == 2;
		}), "deferred spread batch did not retire both prepared pixel buffers");
		Expect(destruction->destroyedOffCaller,
			"a deferred spread completion destroyed large pixels on the renderer caller");
	}
	const auto accountingDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < accountingDeadline) {
		const auto snapshot = sharedBudget->Snapshot();
		if (snapshot.retainedBytes == 0 && snapshot.activeWorkingBytes == 0 &&
			snapshot.uploadStagingBytes == 0 && cache.GetDiagnostics().borrowedBytes == 0) break;
		std::this_thread::yield();
	}
	const auto snapshot = sharedBudget->Snapshot();
	Expect(snapshot.retainedBytes == 0 && snapshot.activeWorkingBytes == 0 &&
		snapshot.uploadStagingBytes == 0 && cache.GetDiagnostics().borrowedBytes == 0,
		"deferred spread upload left retained, staging, working, or borrowed bytes accounted");
}

void TestDisplayRetirementDoesNotDeduplicateReusedAddress() {
	using Image = jpegview_linux::PreparedDisplayImage;
	using ImagePtr = jpegview_linux::DisplayImageCache::ImagePtr;
	alignas(std::max_align_t) unsigned char storage[1024];
	static_assert(sizeof(Image) <= sizeof(storage), "display retirement fixture storage is too small");
	static_assert(alignof(Image) <= alignof(std::max_align_t),
		"display retirement fixture needs stronger alignment");
	std::mutex mutex;
	std::condition_variable changed;
	bool firstLifetimeEnded = false;
	bool releaseFirstDeleter = false;
	bool secondDestroyed = false;
	std::thread::id secondDestroyedOn;
	const std::thread::id caller = std::this_thread::get_id();
	jpegview_linux::DisplayImageCache cache(0, 1);
	ScopedConditionRelease releaseFirstOnExit(mutex, changed, releaseFirstDeleter);

	Image* firstRaw = new (storage) Image();
	const void* reusedAddress = firstRaw;
	ImagePtr first(firstRaw, [&](const Image* retired) {
		const_cast<Image*>(retired)->~Image();
		std::unique_lock<std::mutex> lock(mutex);
		firstLifetimeEnded = true;
		changed.notify_all();
		changed.wait_for(lock, std::chrono::seconds(3), [&] {
			return releaseFirstDeleter;
		});
	});
	cache.Retire(first);
	first.reset();
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return firstLifetimeEnded;
		}), "display retirement did not reach the address-reuse barrier");
	}

	Image* secondRaw = new (storage) Image();
	Expect(secondRaw == reusedAddress,
		"display retirement fixture did not reuse the first image's address");
	ImagePtr second(secondRaw, [&](const Image* retired) {
		const_cast<Image*>(retired)->~Image();
		{
			std::lock_guard<std::mutex> lock(mutex);
			secondDestroyedOn = std::this_thread::get_id();
			secondDestroyed = true;
		}
		changed.notify_all();
	});
	cache.Retire(second);
	second.reset();
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseFirstDeleter = true;
	}
	changed.notify_all();
	bool secondFinished = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		secondFinished = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return secondDestroyed;
		});
	}
	Expect(secondFinished && secondDestroyedOn != caller,
		"a new display owner at a reused address was discarded or destroyed on the caller");
}

void TestDecodedRetirementDoesNotDeduplicateReusedAddress() {
	using DisplayImage = jpegview_linux::PreparedDisplayImage;
	using Decoded = jpegview_linux::DecodedImage;
	using DisplayImagePtr = jpegview_linux::DisplayImageCache::ImagePtr;
	alignas(std::max_align_t) unsigned char storage[1024];
	static_assert(sizeof(DisplayImage) <= sizeof(storage) && sizeof(Decoded) <= sizeof(storage),
		"decoded retirement fixture storage is too small");
	static_assert(alignof(DisplayImage) <= alignof(std::max_align_t) &&
		alignof(Decoded) <= alignof(std::max_align_t),
		"decoded retirement fixture needs stronger alignment");
	TemporaryDirectory temporary;
	const fs::path blockerFile = temporary.path() / "retirement-address-blocker.png";
	const fs::path queuedFile = temporary.path() / "retirement-address-decoded.png";
	WriteText(blockerFile, "block retirement worker's sibling");
	WriteText(queuedFile, "queue decoded retirement");
	std::mutex mutex;
	std::condition_variable changed;
	bool processorStarted = false;
	bool releaseProcessor = false;
	bool firstLifetimeEnded = false;
	bool releaseFirstDeleter = false;
	bool decodedDestroyed = false;
	std::thread::id decodedDestroyedOn;
	const std::thread::id caller = std::this_thread::get_id();
	jpegview_linux::DisplayImageCache cache(0, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			if (request.filename == blockerFile) {
				std::unique_lock<std::mutex> lock(mutex);
				processorStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseProcessor; });
			}
			return DisplayCachePreparedTestImage(request);
		});
	ScopedConditionRelease releaseProcessorOnExit(mutex, changed, releaseProcessor);
	ScopedConditionRelease releaseFirstOnExit(mutex, changed, releaseFirstDeleter);
	const auto blockerRequest = jpegview_linux::MakeDisplayImageRequest(
		blockerFile, DisplayCacheTestImage(2, 2), 0, 2, 2, false, 1);
	cache.RequestBackground(blockerRequest);
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return processorStarted;
		}), "decoded retirement fixture did not block its preparation worker");
	}

	DisplayImage* firstRaw = new (storage) DisplayImage();
	const void* reusedAddress = firstRaw;
	DisplayImagePtr first(firstRaw, [&](const DisplayImage* retired) {
		const_cast<DisplayImage*>(retired)->~DisplayImage();
		std::unique_lock<std::mutex> lock(mutex);
		firstLifetimeEnded = true;
		changed.notify_all();
		changed.wait_for(lock, std::chrono::seconds(3), [&] {
			return releaseFirstDeleter;
		});
	});
	cache.Retire(first);
	first.reset();
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return firstLifetimeEnded;
		}), "display retirement did not reach the decoded address-reuse barrier");
	}

	Decoded* decodedRaw = new (storage) Decoded();
	Expect(decodedRaw == reusedAddress,
		"decoded retirement fixture did not reuse the display image's address");
	jpegview_linux::DecodedFrame frame;
	frame.width = frame.height = 2;
	frame.bgra.assign(16, 255);
	decodedRaw->frames.push_back(std::move(frame));
	std::shared_ptr<Decoded> decoded(decodedRaw, [&](Decoded* retired) {
		retired->~Decoded();
		{
			std::lock_guard<std::mutex> lock(mutex);
			decodedDestroyedOn = std::this_thread::get_id();
			decodedDestroyed = true;
		}
		changed.notify_all();
	});
	auto queuedRequest = jpegview_linux::MakeDisplayImageRequest(
		queuedFile, decoded, 0, 2, 2, false, 2);
	Expect(queuedRequest.Valid(), "decoded retirement fixture built an invalid request");
	cache.RequestBackground(queuedRequest);
	Expect(cache.GetDiagnostics().backgroundQueued == 1,
		"decoded retirement request did not remain queued behind blocked preparation");
	cache.CancelBackground(queuedRequest.key);
	queuedRequest.decoded.reset();
	decoded.reset();
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseFirstDeleter = true;
		releaseProcessor = true;
	}
	changed.notify_all();
	bool decodedFinished = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		decodedFinished = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return decodedDestroyed;
		});
	}
	Expect(decodedFinished && decodedDestroyedOn != caller,
		"a decoded owner at a reused display address was discarded or destroyed on the caller");
}

void TestDisplayImageCacheBackgroundPreparation() {
	Expect(jpegview_linux::DisplayPrefetchCandidateLimit(1024, 100) == 99,
		"display prefetch candidate limit did not exclude the current image");
	Expect(jpegview_linux::DisplayPrefetchCandidateLimit(1024 * 1024, 10000) == 512,
		"display prefetch candidate limit did not enforce its speculative work cap");
	Expect(jpegview_linux::DisplayPrefetchCandidateLimit(0, 5) == 0 &&
		jpegview_linux::DisplayPrefetchCandidateLimit(1024, 1) == 0,
		"display prefetch candidate limit accepted a disabled cache or single image");

	TemporaryDirectory temporary;
	const fs::path firstFile = temporary.path() / "first.jpg";
	const fs::path secondFile = temporary.path() / "second.jpg";
	const fs::path thirdFile = temporary.path() / "third.jpg";
	WriteText(firstFile, "first");
	WriteText(secondFile, "second");
	WriteText(thirdFile, "third");
	const std::shared_ptr<DecodedImage> decoded = DisplayCacheTestImage(4, 4);

	const jpegview_linux::DisplayImageRequest invalid =
		jpegview_linux::MakeDisplayImageRequest(firstFile, decoded, 1, 2, 2, false);
	Expect(!invalid.Valid(), "display cache accepted an invalid decoded frame");
	const jpegview_linux::DisplayImageRequest scaled =
		jpegview_linux::MakeDisplayImageRequest(firstFile, decoded, 0, 2, 2, false);
	Expect(scaled.Valid(), "display cache rejected a valid preparation request");
	auto spectrumRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 4, 4, false, 0, {}, 0, true);
	spectrumRequest.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	spectrumRequest.selectionGeneration = 23;
	jpegview_linux::DisplayImageCache spectrumDisplay(0, 1);
	spectrumDisplay.Request(spectrumRequest);
	Expect(spectrumDisplay.WaitUntilIdle(std::chrono::seconds(2)),
		"display worker did not finish selected-frame spectrum preparation");
	const auto spectrumCompletions = spectrumDisplay.TakeCompletedWithMetadata(1);
	const jpegview_linux::GrayscaleSpectrum expectedSpectrum =
		jpegview_linux::BuildGrayscaleSpectrum(decoded->frames.front().bgra, 4, 4);
	Expect(spectrumCompletions.size() == 1 && spectrumCompletions.front().image &&
		spectrumCompletions.front().selectionGeneration == 23 &&
		spectrumCompletions.front().image->spectrum &&
		*spectrumCompletions.front().image->spectrum == expectedSpectrum &&
		jpegview_linux::PreparedDisplayImageBytes(*spectrumCompletions.front().image) ==
			decoded->frames.front().bgra.size() + sizeof(expectedSpectrum),
		"selected histogram work was not prepared on the display worker or budgeted");
	const fs::path histogramJpeg = temporary.path() / "histogram.jpg";
	std::vector<std::uint8_t> histogramPixels(8 * 4 * 4);
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 8; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * 8 + x) * 4;
			histogramPixels[offset] = static_cast<std::uint8_t>(x * 24);
			histogramPixels[offset + 1] = static_cast<std::uint8_t>(y * 50);
			histogramPixels[offset + 2] = static_cast<std::uint8_t>(255 - x * 20);
			histogramPixels[offset + 3] = 255;
		}
	}
	jpegview_linux::ImageWriteOptions histogramWriteOptions;
	std::string histogramWriteError;
	Expect(jpegview_linux::WriteImage(histogramJpeg, histogramPixels.data(), 8, 4,
		histogramWriteOptions, histogramWriteError),
		"could not create a JPEG histogram fixture: " + histogramWriteError);
	jpegview_linux::DecodedImage fullHistogramDecode;
	std::string fullHistogramError;
	Expect(jpegview_linux::DecodeImage(histogramJpeg, fullHistogramDecode,
		fullHistogramError) && !fullHistogramDecode.frames.empty(),
		"could not decode the full JPEG histogram fixture: " + fullHistogramError);
	auto fileBackedSpectrumRequest = jpegview_linux::MakeJpegDisplayImageRequest(
		jpegview_linux::DescribeImageSource(histogramJpeg), 8, 4, 2, 2, false,
		0, {}, 0, true);
	fileBackedSpectrumRequest.workClass =
		jpegview_linux::PerfWorkClass::ActiveImageSpread;
	jpegview_linux::DisplayImageCache fileBackedSpectrumDisplay(0, 1);
	fileBackedSpectrumDisplay.Request(fileBackedSpectrumRequest);
	Expect(fileBackedSpectrumDisplay.WaitUntilIdle(std::chrono::seconds(2)),
		"file-backed histogram preparation did not finish on its worker");
	const auto fileBackedSpectrum =
		fileBackedSpectrumDisplay.TakeCompletedWithMetadata(1);
	const jpegview_linux::GrayscaleSpectrum fullSourceSpectrum =
		jpegview_linux::BuildGrayscaleSpectrum(
			fullHistogramDecode.frames.front().bgra, 8, 4);
	Expect(fileBackedSpectrum.size() == 1 && fileBackedSpectrum.front().image &&
		fileBackedSpectrum.front().image->spectrum &&
		*fileBackedSpectrum.front().image->spectrum == fullSourceSpectrum,
		"fitted JPEG histogram changed when display pixels were reduced for presentation");
	const auto secondScaled = jpegview_linux::MakeDisplayImageRequest(
		secondFile, decoded, 0, 2, 2, false, 1);
	const auto thirdScaled = jpegview_linux::MakeDisplayImageRequest(
		thirdFile, decoded, 0, 2, 2, false, 2);
	jpegview_linux::ImageProcessingParams canceledProcessing;
	canceledProcessing.contrast = 0.2;
	auto canceledProcessingRequest = jpegview_linux::MakeDisplayImageRequest(
		thirdFile, DisplayCacheTestImage(32, 32), 0, 16, 16, false, 0,
		canceledProcessing);
	int displayContinuationChecks = 0;
	canceledProcessingRequest.workContext.shouldContinue = [&displayContinuationChecks] {
		return ++displayContinuationChecks < 8;
	};
	jpegview_linux::DisplayImageCache canceledProcessingDisplay(4096, 1);
	canceledProcessingDisplay.Request(canceledProcessingRequest);
	Expect(canceledProcessingDisplay.WaitUntilIdle(std::chrono::seconds(2)) &&
		canceledProcessingDisplay.TakeCompleted(1).empty() &&
		canceledProcessingDisplay.CachedImages() == 0 && displayContinuationChecks >= 8 &&
		canceledProcessingDisplay.GetDiagnostics().lastWorkerFailure.kind ==
			jpegview_linux::WorkerFailureKind::Cancelled,
		"canceled color processing published partial display pixels or lost its failure state");

	std::atomic<int> displayProcessorCalls{0};
	jpegview_linux::DisplayImageCache resilientDisplay(64, 1,
		[&displayProcessorCalls](const jpegview_linux::DisplayImageRequest& request) {
			if (displayProcessorCalls.fetch_add(1) == 0) {
				throw std::runtime_error("injected display processor exception");
			}
			return DisplayCachePreparedTestImage(request);
		});
	resilientDisplay.Request(scaled);
	Expect(resilientDisplay.WaitUntilIdle(std::chrono::seconds(2)) &&
		resilientDisplay.GetDiagnostics().lastWorkerFailure.kind ==
			jpegview_linux::WorkerFailureKind::Exception,
		"display preparation exception was not recorded as a structured worker failure");
	std::atomic<int> failedProcessorCalls{0};
	jpegview_linux::DisplayImageCache failedSelectedDisplay(64, 1,
		[&failedProcessorCalls](const jpegview_linux::DisplayImageRequest&) {
			++failedProcessorCalls;
			throw std::runtime_error("selected display preparation failed");
			return jpegview_linux::DisplayImageCache::ImagePtr{};
		});
	auto failedSelectedRequest = secondScaled;
	failedSelectedRequest.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	failedSelectedRequest.selectionGeneration = 77;
	failedSelectedDisplay.Request(failedSelectedRequest);
	Expect(failedSelectedDisplay.WaitUntilIdle(std::chrono::seconds(2)),
		"selected display worker did not finish its failing request");
	Expect(failedSelectedDisplay.HasPendingOrCached(failedSelectedRequest.key),
		"selected display failure was discarded before the renderer could observe it");
	failedSelectedDisplay.Request(failedSelectedRequest);
	const auto selectedFailures = failedSelectedDisplay.TakeFailedCompletions();
	Expect(selectedFailures.size() == 1 &&
		selectedFailures.front().key == failedSelectedRequest.key &&
		selectedFailures.front().selectionGeneration == 77 &&
		selectedFailures.front().failure.message ==
			"selected display preparation failed" &&
		failedProcessorCalls.load() == 1 &&
		!failedSelectedDisplay.HasPendingOrCached(failedSelectedRequest.key),
		"selected display failure lost its owner identity or retried before resolution");
	resilientDisplay.Request(secondScaled);
	Expect(resilientDisplay.WaitUntilIdle(std::chrono::seconds(2)) &&
		displayProcessorCalls.load() == 2,
		"display worker did not survive a processor exception for a later request");
	const auto afterDisplayException = resilientDisplay.TakeCompleted(1);
	Expect(afterDisplayException.size() == 1 &&
		afterDisplayException.front()->key == secondScaled.key,
		"display worker did not publish the request following a processor exception");

	std::mutex pairMutex;
	std::condition_variable pairChanged;
	int pairWorkersStarted = 0;
	bool pairWorkersOverlapped = false;
	bool releasePairWorkers = false;
	jpegview_linux::DisplayImageCache pairedPreparation(64, 2,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			{
				std::unique_lock<std::mutex> lock(pairMutex);
				++pairWorkersStarted;
				pairWorkersOverlapped = pairWorkersStarted >= 2;
				pairChanged.notify_all();
				pairChanged.wait(lock, [&] {
					return pairWorkersOverlapped || releasePairWorkers;
				});
			}
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = request.key;
			result->source = request.source;
			result->cacheKey = request.cacheKey;
			result->width = request.targetWidth;
			result->height = request.targetHeight;
			result->bgra.assign(static_cast<std::size_t>(result->width) *
				static_cast<std::size_t>(result->height) * 4, 255);
			return result;
		});
	pairedPreparation.RequestBackgroundBatch({scaled, secondScaled});
	pairedPreparation.RequestBackgroundBatch({thirdScaled});
	Expect(pairedPreparation.HasPendingOrCached(scaled.key) &&
		pairedPreparation.HasPendingOrCached(secondScaled.key) &&
		pairedPreparation.HasPendingOrCached(thirdScaled.key),
		"adding a neighbor request discarded active or queued background work");
	bool pairedWorkersStartedTogether = false;
	{
		std::unique_lock<std::mutex> lock(pairMutex);
		pairedWorkersStartedTogether = pairChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return pairWorkersOverlapped; });
		releasePairWorkers = true;
	}
	pairChanged.notify_all();
	Expect(pairedWorkersStartedTogether,
		"paired display requests were not made available to workers together with later neighbors");
	const bool thirdRemainedBackpressured = !pairedPreparation.WaitUntilIdle(
		std::chrono::milliseconds(100));
	const auto pairedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < pairedDeadline &&
		pairedPreparation.GetDiagnostics().preparedImages != 2) {
		std::this_thread::yield();
	}
	const jpegview_linux::DisplayImageCacheDiagnostics preparedStats =
		pairedPreparation.GetDiagnostics();
	Expect(thirdRemainedBackpressured && preparedStats.preparedImages == 2 &&
		preparedStats.preparedBytes == 32 &&
		pairedPreparation.HasPendingOrCached(scaled.key),
		"display completion backpressure did not hold at two paired frames");
	auto pairedFrames = pairedPreparation.TakeCompleted(2);
	Expect(pairedFrames.size() == 2 &&
		pairedPreparation.WaitUntilIdle(std::chrono::seconds(2)),
		"consuming paired completions did not admit the next neighbor");
	const auto thirdFrame = pairedPreparation.TakeCompleted(1);
	pairedFrames.insert(pairedFrames.end(), thirdFrame.begin(), thirdFrame.end());
	Expect(pairedFrames.size() == 3 &&
		pairedPreparation.GetDiagnostics().borrowedBytes == preparedStats.preparedBytes + 16,
		"display cache diagnostics did not account for frames borrowed for upload");
	for (const auto& frame : pairedFrames) pairedPreparation.Retire(frame);
	Expect(pairedPreparation.GetDiagnostics().borrowedBytes == 0,
		"display cache diagnostics retained borrowed bytes after retirement");
	auto nearestRequest = scaled;
	nearestRequest.priority = 1;
	nearestRequest.workClass = jpegview_linux::PerfWorkClass::NearestNavigationNeighbor;
	auto distantRequest = secondScaled;
	distantRequest.priority = 2;
	distantRequest.workClass = jpegview_linux::PerfWorkClass::DistantSpeculation;
	jpegview_linux::DisplayImageCache gatedUploads(64, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
			auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			image->key = request.key;
			image->source = request.source;
			image->cacheKey = request.cacheKey;
			image->width = image->height = 2;
			image->workClass = request.workClass;
			image->bgra.assign(16, 255);
			return image;
		});
	gatedUploads.RequestBackgroundBatch({nearestRequest, distantRequest});
	Expect(gatedUploads.WaitUntilIdle(std::chrono::seconds(2)),
		"display frames did not reach the interaction upload gate");
	const std::set<jpegview_linux::PerfWorkClass> foregroundOnly{
		jpegview_linux::PerfWorkClass::ActiveImageSpread,
		jpegview_linux::PerfWorkClass::FocusedPreview};
	const std::set<jpegview_linux::PerfWorkClass> nearestOnly{
		jpegview_linux::PerfWorkClass::NearestNavigationNeighbor};
	const std::set<jpegview_linux::PerfWorkClass> distantOnly{
		jpegview_linux::PerfWorkClass::DistantSpeculation};
	const auto deniedUploads = gatedUploads.TakeCompleted(2, foregroundOnly);
	const auto nearestUpload = gatedUploads.TakeCompleted(1, nearestOnly);
	const auto distantUpload = gatedUploads.TakeCompleted(1, distantOnly);
	Expect(deniedUploads.empty() && nearestUpload.size() == 1 &&
		nearestUpload.front()->key == nearestRequest.key && distantUpload.size() == 1 &&
		distantUpload.front()->key == distantRequest.key,
		"class-filtered uploads discarded permitted frames or uploaded paused speculation");
	for (const auto& image : nearestUpload) gatedUploads.Retire(image);
	for (const auto& image : distantUpload) gatedUploads.Retire(image);
	std::mutex neighborCancelMutex;
	std::condition_variable neighborCancelChanged;
	bool neighborStarted = false;
	bool releaseNeighbor = false;
	std::shared_ptr<std::atomic<bool>> neighborCancellation;
	jpegview_linux::DisplayImageCache canceledNeighbor(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			std::unique_lock<std::mutex> lock(neighborCancelMutex);
			neighborCancellation = request.cancellation;
			neighborStarted = true;
			neighborCancelChanged.notify_all();
			neighborCancelChanged.wait(lock, [&] { return releaseNeighbor; });
			auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			image->key = request.key;
			image->source = request.source;
			image->cacheKey = request.cacheKey;
			image->width = image->height = 2;
			image->workClass = request.workClass;
			image->bgra.assign(16, 255);
			return image;
		});
	canceledNeighbor.RequestBackground(nearestRequest);
	bool neighborReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(neighborCancelMutex);
		neighborReachedBarrier = neighborCancelChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return neighborStarted; });
	}
	if (neighborReachedBarrier) canceledNeighbor.Prefetch({});
	const bool cancellationWasRequested = neighborCancellation && neighborCancellation->load();
	{
		std::lock_guard<std::mutex> lock(neighborCancelMutex);
		releaseNeighbor = true;
	}
	neighborCancelChanged.notify_all();
	Expect(neighborReachedBarrier && cancellationWasRequested &&
		canceledNeighbor.WaitUntilIdle(std::chrono::seconds(2)) &&
		canceledNeighbor.TakeCompleted(1).empty() && !canceledNeighbor.Find(nearestRequest),
		"obsolete active neighbor work was not canceled or uploaded after interaction began");
	std::mutex promotedMutex;
	std::condition_variable promotedChanged;
	bool promotedStarted = false;
	bool cancellationObserved = false;
	bool releasePromotedProcessor = false;
	std::shared_ptr<std::atomic<bool>> promotedCancellation;
	std::atomic<int> promotedProcessorCalls{0};
	jpegview_linux::DisplayImageCache promotedAfterCancellation(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			const int call = ++promotedProcessorCalls;
			if (call == 1) {
				std::unique_lock<std::mutex> lock(promotedMutex);
				promotedCancellation = request.cancellation;
				promotedStarted = true;
				promotedChanged.notify_all();
				while (request.workContext.Continue()) {
					promotedChanged.wait_for(lock, std::chrono::milliseconds(2));
				}
				cancellationObserved = true;
				promotedChanged.notify_all();
				promotedChanged.wait(lock, [&] { return releasePromotedProcessor; });
				return jpegview_linux::DisplayImageCache::ImagePtr{};
			}
			return DisplayCachePreparedTestImage(request);
		});
	promotedAfterCancellation.RequestBackground(nearestRequest);
	bool promotedProcessorStarted = false;
	{
		std::unique_lock<std::mutex> lock(promotedMutex);
		promotedProcessorStarted = promotedChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return promotedStarted; });
	}
	if (promotedProcessorStarted) {
		promotedAfterCancellation.CancelBackground(nearestRequest.key);
	}
	bool observedPromotedCancellation = false;
	{
		std::unique_lock<std::mutex> lock(promotedMutex);
		observedPromotedCancellation = promotedChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return cancellationObserved; });
	}
	auto promotedForegroundRequest = nearestRequest;
	promotedForegroundRequest.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	promotedForegroundRequest.selectionGeneration = 41;
	if (observedPromotedCancellation) {
		promotedAfterCancellation.Request(promotedForegroundRequest);
		promotedAfterCancellation.Prefetch({});
		promotedAfterCancellation.RequestBackgroundBatch({promotedForegroundRequest});
	}
	const bool promotionCancellationPreserved = promotedCancellation &&
		promotedCancellation->load();
	{
		std::lock_guard<std::mutex> lock(promotedMutex);
		releasePromotedProcessor = true;
	}
	promotedChanged.notify_all();
	const bool promotedRetryIdle = promotedAfterCancellation.WaitUntilIdle(
		std::chrono::seconds(2));
	const auto promotedRetryCompletion =
		promotedAfterCancellation.TakeCompletedWithMetadata(1);
	Expect(promotedProcessorStarted && observedPromotedCancellation &&
		promotionCancellationPreserved && promotedRetryIdle &&
		promotedProcessorCalls.load() == 2 && promotedRetryCompletion.size() == 1 &&
		promotedRetryCompletion.front().image &&
		promotedRetryCompletion.front().selectionGeneration == 41,
		"background scheduling revived canceled work or lost its foreground retry");
	jpegview_linux::DisplayImageCache failedPreparation(64, 1,
		[](const jpegview_linux::DisplayImageRequest&) {
			return jpegview_linux::DisplayImageCache::ImagePtr{};
		});
	failedPreparation.RequestBackground(scaled);
	Expect(failedPreparation.WaitUntilIdle(std::chrono::seconds(2)) &&
		!failedPreparation.HasPendingOrCached(scaled.key),
		"failed background display preparation stayed pending indefinitely");
	jpegview_linux::ImageProcessingParams adjustedLevels;
	adjustedLevels.contrast = 0.2;
	const jpegview_linux::DisplayImageRequest adjusted =
		jpegview_linux::MakeDisplayImageRequest(firstFile, decoded, 0, 2, 2, false, 0, adjustedLevels);
	Expect(adjusted.Valid() && adjusted.key != scaled.key,
		"display cache key omitted image processing parameters");
	jpegview_linux::ImageProcessingParams subEpsilonAdjustment;
	subEpsilonAdjustment.contrast = 0.0000000005;
	const auto subEpsilonRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, subEpsilonAdjustment);
	Expect(jpegview_linux::EqualImageProcessing(subEpsilonAdjustment,
		jpegview_linux::ImageProcessingParams{}) &&
		subEpsilonRequest.cacheKey != scaled.cacheKey,
		"display cache key used tolerant processing equality and conflated pixel-affecting values");
	jpegview_linux::ImageProcessingParams negativeZeroAdjustment;
	negativeZeroAdjustment.contrast = -0.0;
	const auto negativeZeroRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, negativeZeroAdjustment);
	Expect(negativeZeroRequest.cacheKey == scaled.cacheKey &&
		negativeZeroRequest.key == scaled.key &&
		!std::signbit(negativeZeroRequest.cacheKey.processing.contrast),
		"signed zero split structured and renderer cache identities");
	jpegview_linux::ImageProcessingParams inactiveLevels;
	inactiveLevels.colorCorrection = 0.25;
	inactiveLevels.contrastCorrection = 0.5;
	inactiveLevels.deepShadows = 0.75;
	inactiveLevels.unsharpRadius = 4.0;
	inactiveLevels.unsharpThreshold = 10.0;
	const auto inactiveRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, inactiveLevels);
	Expect(inactiveRequest.key == scaled.key,
		"display cache key changed for controls that are disabled or have no effect");
	Expect(inactiveRequest.cacheKey == scaled.cacheKey &&
		jpegview_linux::DisplayImageCacheKeyHash{}(inactiveRequest.cacheKey) ==
			jpegview_linux::DisplayImageCacheKeyHash{}(scaled.cacheKey),
		"disabled processing controls changed structured-key equality or its hash");
	Expect(jpegview_linux::EqualImageProcessing(
		jpegview_linux::EffectiveImageProcessingParams(inactiveLevels, false),
		jpegview_linux::EffectiveImageProcessingParams(
			jpegview_linux::ImageProcessingParams{}, false)),
		"effective image processing retained controls that cannot affect source pixels");
	inactiveLevels.unsharpAmount = 1.0;
	const auto activeUnsharpRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, inactiveLevels);
	Expect(activeUnsharpRequest.key != scaled.key &&
		activeUnsharpRequest.cacheKey != scaled.cacheKey,
		"display cache key omitted an enabled unsharp-mask adjustment");
	jpegview_linux::ImageProcessingParams nonFiniteProcessing;
	nonFiniteProcessing.gamma = std::numeric_limits<double>::quiet_NaN();
	nonFiniteProcessing.colorCorrection = std::numeric_limits<double>::infinity();
	const auto nonFiniteRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, nonFiniteProcessing);
	Expect(!nonFiniteRequest.Valid() && !nonFiniteRequest.cacheKey.Valid() &&
		nonFiniteRequest.key.empty(),
		"non-finite processing values were admitted to a display cache key");
	jpegview_linux::ImageProcessingParams inactiveLocalDensity;
	inactiveLocalDensity.lightenShadows = 0.6;
	inactiveLocalDensity.darkenHighlights = 0.3;
	inactiveLocalDensity.deepShadows = 0.2;
	const auto disabledLocalDensity = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, inactiveLocalDensity);
	inactiveLocalDensity.localDensityEnabled = true;
	jpegview_linux::ImageProcessingParams enabledLocalDensityWithoutEffect;
	enabledLocalDensityWithoutEffect.localDensityEnabled = true;
	enabledLocalDensityWithoutEffect.deepShadows = 0.2;
	const auto controlsEnabledWithoutEffect = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, enabledLocalDensityWithoutEffect);
	Expect(disabledLocalDensity.cacheKey == scaled.cacheKey &&
		controlsEnabledWithoutEffect.cacheKey == scaled.cacheKey,
		"disabled local-density controls or an enabled zero-effect control changed the key");
	inactiveLocalDensity.lightenShadows = 0.6;
	const auto activeLocalDensity = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, inactiveLocalDensity);
	Expect(activeLocalDensity.cacheKey != scaled.cacheKey &&
		activeLocalDensity.cacheKey.processing.localDensityEnabled &&
		activeLocalDensity.cacheKey.processing.lightenShadows == 0.6,
		"enabled local-density processing was omitted from the structured key");
	const auto autoContrastBase = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, true);
	const auto autoContrastAdjusted = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, true, 0, inactiveLevels);
	Expect(autoContrastBase.cacheKey != scaled.cacheKey &&
		autoContrastAdjusted.cacheKey != autoContrastBase.cacheKey &&
		autoContrastAdjusted.cacheKey.processing.colorCorrection ==
			inactiveLevels.colorCorrection,
		"auto-contrast or its enabled correction controls were omitted from structured identity");
	const auto adjustedTarget = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 3, 2, false);
	Expect(adjustedTarget.cacheKey != scaled.cacheKey,
		"display structured key omitted target geometry");
	auto twoFrames = std::make_shared<DecodedImage>(*decoded);
	twoFrames->frames.push_back(twoFrames->frames.front());
	const auto firstFrame = jpegview_linux::MakeDisplayImageRequest(
		firstFile, twoFrames, 0, 2, 2, false);
	const auto secondFrame = jpegview_linux::MakeDisplayImageRequest(
		firstFile, twoFrames, 1, 2, 2, false);
	Expect(firstFrame.cacheKey != secondFrame.cacheKey,
		"display structured key omitted animation frame identity");

	std::mutex coalesceMutex;
	std::condition_variable coalesceChanged;
	bool firstPreparationStarted = false;
	bool releaseFirstPreparation = false;
	std::vector<double> coalescedContrasts;
	jpegview_linux::ImageProcessingParams intermediateLevels;
	intermediateLevels.contrast = 0.1;
	jpegview_linux::ImageProcessingParams latestLevels;
	latestLevels.contrast = 0.4;
	const auto intermediateRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, intermediateLevels);
	const auto latestRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, latestLevels);
	jpegview_linux::DisplayImageCache coalesced(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			{
				std::unique_lock<std::mutex> lock(coalesceMutex);
				coalescedContrasts.push_back(request.processing.contrast);
				if (coalescedContrasts.size() == 1) {
					firstPreparationStarted = true;
					coalesceChanged.notify_all();
					coalesceChanged.wait(lock, [&] { return releaseFirstPreparation; });
				}
			}
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = request.key;
			result->source = request.source;
			result->cacheKey = request.cacheKey;
			result->width = 2;
			result->height = 2;
			result->bgra.assign(16, 255);
			return result;
		});
	coalesced.Request(scaled);
	{
		std::unique_lock<std::mutex> lock(coalesceMutex);
		Expect(coalesceChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return firstPreparationStarted; }),
			"foreground display preparation did not start for coalescing test");
	}
	coalesced.Request(intermediateRequest);
	coalesced.Request(latestRequest);
	{
		std::lock_guard<std::mutex> lock(coalesceMutex);
		releaseFirstPreparation = true;
	}
	coalesceChanged.notify_all();
	Expect(coalesced.WaitUntilIdle(std::chrono::seconds(2)),
		"coalesced foreground display work did not finish");
	{
		std::lock_guard<std::mutex> lock(coalesceMutex);
		Expect(coalescedContrasts == std::vector<double>({0.0, 0.4}),
			"obsolete queued picture-level previews were not coalesced to the latest value");
	}
	Expect(coalesced.Find(latestRequest) != nullptr && coalesced.Find(scaled) == nullptr &&
		coalesced.Find(intermediateRequest) == nullptr,
		"obsolete foreground preview replaced or remained alongside the latest one");

	jpegview_linux::DisplayImageCache realProcessor(64, 1);
	realProcessor.Request(scaled);
	Expect(realProcessor.WaitUntilIdle(std::chrono::seconds(2)),
		"display image preparation did not finish");
	const jpegview_linux::DisplayImageCache::ImagePtr prepared = realProcessor.Find(scaled);
	Expect(prepared && prepared->width == 2 && prepared->height == 2 &&
		prepared->bgra.size() == 16,
		"display image worker did not produce exact target-size pixels");
	Expect(prepared && !prepared->hasTransparency,
		"display image worker invented transparency for an opaque frame");
	const auto rotatedClockwiseRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, DisplayCacheTestImage(2, 3), 0, 3, 2, false, 0, {}, 1);
	const auto rotatedCounterClockwiseRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, DisplayCacheTestImage(2, 3), 0, 3, 2, false, 0, {}, 3);
	const auto unrotatedRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, DisplayCacheTestImage(2, 3), 0, 2, 3, false);
	const auto halfTurnRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, DisplayCacheTestImage(2, 3), 0, 2, 3, false, 0, {}, 2);
	Expect(rotatedClockwiseRequest.Valid() && rotatedCounterClockwiseRequest.Valid() &&
		unrotatedRequest.Valid() && halfTurnRequest.Valid() &&
		rotatedClockwiseRequest.key != rotatedCounterClockwiseRequest.key &&
		unrotatedRequest.key != halfTurnRequest.key &&
		rotatedClockwiseRequest.cacheKey != rotatedCounterClockwiseRequest.cacheKey &&
		unrotatedRequest.cacheKey != halfTurnRequest.cacheKey,
		"display cache keys did not distinguish quarter-turn orientation");
	jpegview_linux::DisplayImageCache rotationProcessor(256, 1);
	const auto rotatedClockwise = rotationProcessor.RequestAndWait(rotatedClockwiseRequest);
	const auto rotatedCounterClockwise =
		rotationProcessor.RequestAndWait(rotatedCounterClockwiseRequest);
	const auto halfTurn = rotationProcessor.RequestAndWait(halfTurnRequest);
	const auto blueChannel = [](const jpegview_linux::DisplayImageCache::ImagePtr& image) {
		std::vector<std::uint8_t> values;
		if (!image) return values;
		for (std::size_t offset = 0; offset < image->bgra.size(); offset += 4) {
			values.push_back(image->bgra[offset]);
		}
		return values;
	};
	Expect(rotatedClockwise && rotatedClockwise->width == 3 &&
		rotatedClockwise->height == 2 && rotatedClockwise->rotationQuarterTurns == 1 &&
		blueChannel(rotatedClockwise) == std::vector<std::uint8_t>({4, 2, 0, 5, 3, 1}),
		"display worker did not rotate decoded pixels clockwise before scaling");
	Expect(rotatedCounterClockwise && rotatedCounterClockwise->width == 3 &&
		rotatedCounterClockwise->height == 2 &&
		blueChannel(rotatedCounterClockwise) ==
			std::vector<std::uint8_t>({1, 3, 5, 0, 2, 4}),
		"display worker did not rotate decoded pixels counterclockwise");
	Expect(halfTurn && halfTurn->width == 2 && halfTurn->height == 3 &&
		blueChannel(halfTurn) == std::vector<std::uint8_t>({5, 4, 3, 2, 1, 0}),
		"display worker did not rotate decoded pixels through a half turn");
	auto transparentDecoded = DisplayCacheTestImage(4, 4);
	transparentDecoded->frames.front().bgra[3] = 0;
	transparentDecoded->frames.front().hasTransparency = true;
	const auto transparentRequest = jpegview_linux::MakeDisplayImageRequest(
		thirdFile, transparentDecoded, 0, 2, 2, false);
	jpegview_linux::DisplayImageCache transparencyProcessor(64, 1);
	const auto transparentPrepared = transparencyProcessor.RequestAndWait(transparentRequest);
	Expect(transparentPrepared && transparentPrepared->hasTransparency,
		"display image preparation lost source transparency metadata");
	Expect(realProcessor.TakeCompleted(1).size() == 1 && realProcessor.TakeCompleted(1).empty(),
		"display image completion queue did not drain exactly once");
	realProcessor.Prefetch({});
	realProcessor.Prefetch({scaled});
	Expect(realProcessor.TakeCompleted(1).size() == 1,
		"retained display pixels were not rescheduled for upload without recomputation");
	realProcessor.Release(scaled.key);
	Expect(realProcessor.CachedImages() == 0 && realProcessor.CachedBytes() == 0,
		"display image release retained uploaded staging pixels");

	std::mutex orderMutex;
	std::vector<int> preparationOrder;
	const std::thread::id callingThread = std::this_thread::get_id();
	std::atomic<bool> usedBackgroundThread{false};
	jpegview_linux::DisplayImageCache ordered(32, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			usedBackgroundThread = std::this_thread::get_id() != callingThread;
			{
				std::lock_guard<std::mutex> lock(orderMutex);
				preparationOrder.push_back(request.targetWidth);
			}
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = request.key;
			result->source = request.source;
			result->cacheKey = request.cacheKey;
			result->width = request.targetWidth;
			result->height = 1;
			result->bgra.assign(16, static_cast<std::uint8_t>(request.targetWidth));
			return result;
		});
	const std::vector<jpegview_linux::DisplayImageRequest> requests = {
		jpegview_linux::MakeDisplayImageRequest(firstFile, decoded, 0, 11, 1, false, 3),
		jpegview_linux::MakeDisplayImageRequest(secondFile, decoded, 0, 22, 1, false, 1),
		jpegview_linux::MakeDisplayImageRequest(thirdFile, decoded, 0, 33, 1, false, 2),
	};
	ordered.Prefetch(requests);
	const auto orderedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < orderedDeadline &&
		ordered.GetDiagnostics().speculativePreparedImages != 2) {
		std::this_thread::yield();
	}
	std::vector<jpegview_linux::DisplayImageCache::ImagePtr> orderedFrames =
		ordered.TakeCompleted(2);
	Expect(orderedFrames.size() == 2,
		"nearest display prefetch did not fill its bounded completion queue");
	Expect(ordered.WaitUntilIdle(std::chrono::seconds(2)),
		"display image prefetch did not finish");
	{
		std::lock_guard<std::mutex> lock(orderMutex);
		Expect(preparationOrder == std::vector<int>({22, 33, 11}),
			"display image workers did not sort preparation by nearest-first priority");
	}
	Expect(usedBackgroundThread, "display image processing ran on the calling thread");
	Expect(ordered.CachedImages() == 2 && ordered.CachedBytes() == 32,
		"display image cache did not enforce its pixel-memory budget");
	const auto finalOrderedFrames = ordered.TakeCompleted(10);
	orderedFrames.insert(orderedFrames.end(), finalOrderedFrames.begin(), finalOrderedFrames.end());
	for (const auto& frame : orderedFrames) ordered.Retire(frame);

	std::mutex backgroundMutex;
	std::condition_variable backgroundChanged;
	bool foregroundStarted = false;
	bool releaseForeground = false;
	std::vector<int> backgroundOrder;
	const auto prepareWithPriority = [&](const jpegview_linux::DisplayImageRequest& request) {
		{
			std::unique_lock<std::mutex> lock(backgroundMutex);
			backgroundOrder.push_back(request.targetWidth);
			if (request.targetWidth == 2) {
				foregroundStarted = true;
				backgroundChanged.notify_all();
				backgroundChanged.wait(lock, [&] { return releaseForeground; });
			}
		}
		auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
		result->key = request.key;
		result->source = request.source;
		result->cacheKey = request.cacheKey;
		result->width = request.targetWidth;
		result->height = 1;
		result->bgra.assign(static_cast<std::size_t>(request.targetWidth) * 4, 255);
		return result;
	};
	jpegview_linux::DisplayImageCache backgroundCache(64, 1, prepareWithPriority);
	const auto neighborRequest = jpegview_linux::MakeDisplayImageRequest(
		secondFile, decoded, 0, 3, 1, false, 1);
	const auto lensRequest = jpegview_linux::MakeDisplayImageRequest(
		thirdFile, decoded, 0, 4, 1, false, 1000000);
	backgroundCache.Request(scaled);
	{
		std::unique_lock<std::mutex> lock(backgroundMutex);
		Expect(backgroundChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return foregroundStarted; }),
			"foreground display request did not start before the optional background request");
	}
	backgroundCache.Prefetch({neighborRequest});
	backgroundCache.RequestBackground(lensRequest);
	{
		std::lock_guard<std::mutex> lock(backgroundMutex);
		releaseForeground = true;
	}
	backgroundChanged.notify_all();
	Expect(backgroundCache.WaitUntilIdle(std::chrono::seconds(2)) &&
		backgroundOrder == std::vector<int>({2, 3, 4}),
		"optional magnifier preparation displaced the foreground or nearest neighbor work");

	std::mutex cancelMutex;
	std::condition_variable cancelChanged;
	bool cancelForegroundStarted = false;
	bool releaseCancelForeground = false;
	std::vector<int> canceledOrder;
	jpegview_linux::DisplayImageCache cancelBackground(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			{
				std::unique_lock<std::mutex> lock(cancelMutex);
				canceledOrder.push_back(request.targetWidth);
				if (request.targetWidth == 2) {
					cancelForegroundStarted = true;
					cancelChanged.notify_all();
					cancelChanged.wait(lock, [&] { return releaseCancelForeground; });
				}
			}
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = request.key;
			result->source = request.source;
			result->cacheKey = request.cacheKey;
			result->width = request.targetWidth;
			result->height = 1;
			result->bgra.assign(static_cast<std::size_t>(request.targetWidth) * 4, 255);
			return result;
		});
	const auto canceledLensRequest = jpegview_linux::MakeDisplayImageRequest(
		thirdFile, decoded, 0, 4, 1, false, 1000000);
	cancelBackground.Request(scaled);
	{
		std::unique_lock<std::mutex> lock(cancelMutex);
		Expect(cancelChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return cancelForegroundStarted; }),
			"foreground display request did not start before background cancellation");
	}
	cancelBackground.RequestBackground(canceledLensRequest);
	cancelBackground.CancelBackground(canceledLensRequest.key);
	{
		std::lock_guard<std::mutex> lock(cancelMutex);
		releaseCancelForeground = true;
	}
	cancelChanged.notify_all();
	Expect(cancelBackground.WaitUntilIdle(std::chrono::seconds(2)) &&
		canceledOrder == std::vector<int>({2}) &&
		cancelBackground.Find(canceledLensRequest) == nullptr,
		"canceled optional background work still ran or entered the display cache");

	ordered.Request(requests[0]);
	Expect(ordered.WaitUntilIdle(std::chrono::seconds(2)) && ordered.Find(requests[0]) != nullptr &&
		ordered.CachedImages() == 2 && ordered.CachedBytes() == 32,
		"foreground display preparation did not evict the least-recently-used entry");

	WriteText(firstFile, "first-file-was-modified");
	const jpegview_linux::SourceDescriptor refreshedSource =
		jpegview_linux::DescribeImageSource(firstFile);
	const auto refreshedRequest = jpegview_linux::MakeDisplayImageRequest(
		refreshedSource, decoded, 0, 11, 1, false, 3);
	Expect(refreshedSource.Valid() &&
		refreshedSource.Key() != requests[0].source.Key() &&
		ordered.Find(refreshedRequest) == nullptr,
		"display cache served old prepared pixels for the refreshed source identity");
	ordered.Clear();
	Expect(ordered.CachedImages() == 0 && ordered.CachedBytes() == 0 &&
		ordered.TakeCompleted(10).empty(),
		"display cache clear retained pixels or completion notifications");

	std::mutex priorityMutex;
	std::condition_variable priorityChanged;
	bool releaseClosest = false;
	bool fartherFinished = false;
	jpegview_linux::DisplayImageCache prioritized(64, 2,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			if (request.filename == firstFile) {
				std::unique_lock<std::mutex> lock(priorityMutex);
				priorityChanged.wait(lock, [&] { return releaseClosest; });
			}
			auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			result->key = request.key;
			result->source = request.source;
			result->cacheKey = request.cacheKey;
			result->width = 1;
			result->height = 1;
			result->priority = request.priority;
			result->bgra.assign(4, 255);
			if (request.priority == 2) {
				{
					std::lock_guard<std::mutex> lock(priorityMutex);
					fartherFinished = true;
				}
				priorityChanged.notify_all();
			}
			return result;
		});
	const jpegview_linux::DisplayImageRequest closest =
		jpegview_linux::MakeDisplayImageRequest(firstFile, decoded, 0, 2, 2, false, 3);
	const jpegview_linux::DisplayImageRequest farther =
		jpegview_linux::MakeDisplayImageRequest(secondFile, decoded, 0, 2, 2, false, 2);
	prioritized.Prefetch({closest, farther});
	bool fartherCompletedInTime = false;
	{
		std::unique_lock<std::mutex> lock(priorityMutex);
		fartherCompletedInTime = priorityChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return fartherFinished; });
	}
	jpegview_linux::DisplayImageRequest reprioritizedClosest = closest;
	reprioritizedClosest.priority = 1;
	prioritized.Prefetch({reprioritizedClosest, farther});
	const bool fartherUploadBlocked = prioritized.TakeCompleted(1).empty();
	{
		std::lock_guard<std::mutex> lock(priorityMutex);
		releaseClosest = true;
	}
	priorityChanged.notify_all();
	Expect(fartherCompletedInTime,
		"farther display preparation did not finish while its closest neighbor was active");
	Expect(fartherUploadBlocked,
		"farther display upload bypassed an unfinished closer neighbor");
	Expect(prioritized.WaitUntilIdle(std::chrono::seconds(2)),
		"prioritized display preparation did not finish");
	const std::vector<jpegview_linux::DisplayImageCache::ImagePtr> priorityCompletions =
		prioritized.TakeCompleted(2);
	Expect(priorityCompletions.size() == 2 && priorityCompletions[0]->key == closest.key &&
		priorityCompletions[1]->key == farther.key,
		"display upload queue did not apply the latest priority to in-flight neighbors");
}

void TestDisplayCompletionQueueBackpressure() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "completion-backpressure.png";
	WriteText(filename, "completion queue source");
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::atomic<std::size_t> processorCalls{0};
	jpegview_linux::DisplayImageCache cache(0, 4,
		[&processorCalls](const jpegview_linux::DisplayImageRequest& request) {
			++processorCalls;
			return DisplayCachePreparedTestImage(request);
		});
	std::vector<jpegview_linux::DisplayImageRequest> requests;
	for (std::size_t priority = 1; priority <= 6; ++priority) {
		requests.push_back(jpegview_linux::MakeDisplayImageRequest(filename,
			decoded, 0, static_cast<int>(priority), 1, false, priority));
	}
	cache.RequestBackgroundBatch(requests);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (processorCalls.load() < 2 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	const bool queueStayedBackpressured = !cache.WaitUntilIdle(
		std::chrono::milliseconds(100));
	const jpegview_linux::DisplayImageCacheDiagnostics diagnostics = cache.GetDiagnostics();
	Expect(processorCalls.load() == 2 && diagnostics.preparedImages == 2 &&
		diagnostics.speculativePreparedImages == 2 &&
		diagnostics.speculativeReservedImages == 2 && queueStayedBackpressured,
		"display workers prepared speculative frames beyond the two-frame completion allowance");
	Expect(diagnostics.preparedBytes == 12 && diagnostics.speculativeReservedBytes == 12 &&
		diagnostics.preparedBytes <= jpegview_linux::DisplayImageCache::
			kMaximumSpeculativeCompletionBytes,
		"display speculative completion accounting did not reflect two requested frames");
}

void TestDisplayQueuedPromotionWakesBackpressuredWorker() {
	TemporaryDirectory temporary;
	const fs::path firstFile = temporary.path() / "promotion-first.png";
	const fs::path secondFile = temporary.path() / "promotion-second.png";
	const fs::path promotedFile = temporary.path() / "promotion-queued.png";
	WriteText(firstFile, "first completion");
	WriteText(secondFile, "second completion");
	WriteText(promotedFile, "queued promotion");
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::mutex startedMutex;
	std::condition_variable startedChanged;
	bool promotedStarted = false;
	std::atomic<std::size_t> processorCalls{0};
	jpegview_linux::DisplayImageCache cache(0, 2,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			{
				std::lock_guard<std::mutex> lock(startedMutex);
				++processorCalls;
				if (request.filename == promotedFile) promotedStarted = true;
			}
			startedChanged.notify_all();
			return DisplayCachePreparedTestImage(request);
		});
	const auto first = jpegview_linux::MakeDisplayImageRequest(firstFile,
		decoded, 0, 1, 1, false, 1);
	const auto second = jpegview_linux::MakeDisplayImageRequest(secondFile,
		decoded, 0, 1, 1, false, 2);
	const auto queued = jpegview_linux::MakeDisplayImageRequest(promotedFile,
		decoded, 0, 1, 1, false, 3);
	cache.RequestBackgroundBatch({first, second, queued});
	const auto fullDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool fullAndQueued = false;
	while (std::chrono::steady_clock::now() < fullDeadline) {
		const auto state = cache.GetDiagnostics();
		if (state.speculativePreparedImages == 2 &&
			state.speculativeReservedImages == 2 && state.backgroundQueued == 1 &&
			state.backgroundActive == 0) {
			fullAndQueued = true;
			break;
		}
		std::this_thread::yield();
	}
	Expect(fullAndQueued,
		"display promotion fixture did not fill speculative completions with queued work");

	auto activeSpread = queued;
	activeSpread.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	cache.RequestBackground(activeSpread);
	bool startedWithoutRendererPolling = false;
	{
		std::unique_lock<std::mutex> lock(startedMutex);
		startedWithoutRendererPolling = startedChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return promotedStarted; });
	}
	const bool preparedWithoutRendererPolling = cache.WaitUntilIdle(
		std::chrono::seconds(2));
	const std::set<jpegview_linux::PerfWorkClass> activeSpreadClass{
		jpegview_linux::PerfWorkClass::ActiveImageSpread};
	const auto promotedCompletion = cache.TakeCompleted(1, activeSpreadClass);
	Expect(startedWithoutRendererPolling && preparedWithoutRendererPolling &&
		processorCalls.load() == 3 && promotedCompletion.size() == 1 &&
		promotedCompletion.front()->key == queued.key,
		"queued ActiveImageSpread promotion did not wake a worker without renderer polling");
}

void TestDisplayCompletedPromotionReleasesSpeculativeCapacity() {
	TemporaryDirectory temporary;
	const fs::path firstFile = temporary.path() / "completed-promotion-first.png";
	const fs::path secondFile = temporary.path() / "completed-promotion-second.png";
	const fs::path queuedFile = temporary.path() / "completed-promotion-queued.png";
	WriteText(firstFile, "first completion");
	WriteText(secondFile, "second completion");
	WriteText(queuedFile, "queued request");
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::mutex startedMutex;
	std::condition_variable startedChanged;
	bool queuedStarted = false;
	std::atomic<std::size_t> processorCalls{0};
	jpegview_linux::DisplayImageCache cache(0, 2,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			{
				std::lock_guard<std::mutex> lock(startedMutex);
				++processorCalls;
				if (request.filename == queuedFile) queuedStarted = true;
			}
			startedChanged.notify_all();
			return DisplayCachePreparedTestImage(request);
		});
	const auto first = jpegview_linux::MakeDisplayImageRequest(firstFile,
		decoded, 0, 1, 1, false, 1);
	const auto second = jpegview_linux::MakeDisplayImageRequest(secondFile,
		decoded, 0, 1, 1, false, 2);
	const auto queued = jpegview_linux::MakeDisplayImageRequest(queuedFile,
		decoded, 0, 1, 1, false, 3);
	cache.RequestBackgroundBatch({first, second, queued});
	const auto fullDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool fullAndQueued = false;
	while (std::chrono::steady_clock::now() < fullDeadline) {
		const auto state = cache.GetDiagnostics();
		if (state.speculativePreparedImages == 2 &&
			state.speculativeReservedImages == 2 && state.backgroundQueued == 1 &&
			state.backgroundActive == 0) {
			fullAndQueued = true;
			break;
		}
		std::this_thread::yield();
	}
	Expect(fullAndQueued,
		"completed-promotion fixture did not fill speculative completions with queued work");

	cache.Request(first);
	bool startedWithoutRendererPolling = false;
	{
		std::unique_lock<std::mutex> lock(startedMutex);
		startedWithoutRendererPolling = startedChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return queuedStarted; });
	}
	const bool preparedWithoutRendererPolling = cache.WaitUntilIdle(
		std::chrono::seconds(2));
	const std::set<jpegview_linux::PerfWorkClass> activeSpreadClass{
		jpegview_linux::PerfWorkClass::ActiveImageSpread};
	const auto promotedCompletion = cache.TakeCompleted(1, activeSpreadClass);
	Expect(startedWithoutRendererPolling && preparedWithoutRendererPolling &&
		processorCalls.load() == 3 && promotedCompletion.size() == 1 &&
		promotedCompletion.front()->key == first.key,
		"promoting a completed frame did not release capacity and wake queued work");
}

void TestDisplayInFlightPromotionReleasesSpeculativeCapacity() {
	TemporaryDirectory temporary;
	const fs::path firstFile = temporary.path() / "inflight-promotion-first.png";
	const fs::path promotedFile = temporary.path() / "inflight-promotion-active.png";
	const fs::path queuedFile = temporary.path() / "inflight-promotion-queued.png";
	WriteText(firstFile, "first completion");
	WriteText(promotedFile, "active promotion");
	WriteText(queuedFile, "queued request");
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::mutex processorMutex;
	std::condition_variable processorChanged;
	bool promotedStarted = false;
	bool releasePromoted = false;
	bool queuedStarted = false;
	std::atomic<std::size_t> processorCalls{0};
	jpegview_linux::DisplayImageCache cache(0, 2,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			++processorCalls;
			if (request.filename == promotedFile) {
				std::unique_lock<std::mutex> lock(processorMutex);
				promotedStarted = true;
				processorChanged.notify_all();
				processorChanged.wait(lock, [&] { return releasePromoted; });
			} else if (request.filename == queuedFile) {
				{
					std::lock_guard<std::mutex> lock(processorMutex);
					queuedStarted = true;
				}
				processorChanged.notify_all();
			}
			return DisplayCachePreparedTestImage(request);
		});
	const auto first = jpegview_linux::MakeDisplayImageRequest(firstFile,
		decoded, 0, 1, 1, false, 1);
	const auto promoted = jpegview_linux::MakeDisplayImageRequest(promotedFile,
		decoded, 0, 1, 1, false, 2);
	const auto queued = jpegview_linux::MakeDisplayImageRequest(queuedFile,
		decoded, 0, 1, 1, false, 3);
	cache.RequestBackgroundBatch({first, promoted, queued});
	const auto fullDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool fullAndActive = false;
	while (std::chrono::steady_clock::now() < fullDeadline) {
		const auto state = cache.GetDiagnostics();
		if (state.speculativePreparedImages == 1 &&
			state.speculativeReservedImages == 2 && state.backgroundActive == 1 &&
			state.backgroundQueued == 1) {
			fullAndActive = true;
			break;
		}
		std::this_thread::yield();
	}
	cache.Request(promoted);
	bool queuedStartedWithoutRendererPolling = false;
	{
		std::unique_lock<std::mutex> lock(processorMutex);
		queuedStartedWithoutRendererPolling = processorChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return queuedStarted; });
		releasePromoted = true;
	}
	processorChanged.notify_all();
	const bool preparedWithoutRendererPolling = cache.WaitUntilIdle(
		std::chrono::seconds(2));
	Expect(fullAndActive && promotedStarted && queuedStartedWithoutRendererPolling &&
		preparedWithoutRendererPolling && processorCalls.load() == 3,
		"in-flight foreground promotion did not release capacity and wake queued work");
}

void TestDisplayCompletionByteBudgetAndForegroundAdmission() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "completion-byte-budget.png";
	WriteText(filename, "completion byte budget source");
	const auto decoded = DisplayCacheTestImage(2, 2);
	constexpr std::size_t mebibyte = 1024u * 1024u;
	const std::size_t fortyMiB = 40u * mebibyte;
	std::atomic<std::size_t> processorCalls{0};
	std::atomic<std::size_t> foregroundCalls{0};
	jpegview_linux::DisplayImageCache cache(0, 4,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			++processorCalls;
			if (request.workClass == jpegview_linux::PerfWorkClass::ActiveImageSpread) {
				++foregroundCalls;
			}
			return DisplayCachePreparedTestImage(request);
		});
	auto forty = jpegview_linux::MakeDisplayImageRequest(filename, decoded, 0,
		4096, 2560, false, 1);
	auto thirty = jpegview_linux::MakeDisplayImageRequest(filename, decoded, 0,
		4096, 1920, false, 2);
	auto onePixel = jpegview_linux::MakeDisplayImageRequest(filename, decoded, 0,
		1, 1, false, 3);
	cache.RequestBackgroundBatch({forty, thirty, onePixel});
	const auto preparedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < preparedDeadline) {
		const auto state = cache.GetDiagnostics();
		if (state.speculativePreparedImages == 2) break;
		std::this_thread::yield();
	}
	const bool largerFrameStayedQueued = !cache.WaitUntilIdle(
		std::chrono::milliseconds(100));
	const auto fullState = cache.GetDiagnostics();
	Expect(processorCalls.load() == 2 && fullState.speculativePreparedImages == 2 &&
		fullState.speculativePreparedBytes == fortyMiB + 4 &&
		fullState.speculativeReservedImages == 2 &&
		fullState.speculativeReservedBytes == fortyMiB + 4 &&
		fullState.backgroundQueued == 1 && largerFrameStayedQueued,
		"display speculation exceeded the 64 MiB completion-byte limit or failed to skip blocked work");

	auto foreground = jpegview_linux::MakeDisplayImageRequest(filename, decoded, 0,
		2, 2, false, 0);
	foreground.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	cache.Request(foreground);
	const auto foregroundDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	std::vector<jpegview_linux::DisplayImageCache::ImagePtr> admittedForeground;
	const std::set<jpegview_linux::PerfWorkClass> foregroundClass{
		jpegview_linux::PerfWorkClass::ActiveImageSpread};
	while (std::chrono::steady_clock::now() < foregroundDeadline &&
		admittedForeground.empty()) {
		admittedForeground = cache.TakeCompleted(1, foregroundClass);
		if (admittedForeground.empty()) std::this_thread::yield();
	}
	const auto afterForeground = cache.GetDiagnostics();
	Expect(foregroundCalls.load() == 1 && admittedForeground.size() == 1 &&
		admittedForeground.front()->key == foreground.key &&
		afterForeground.speculativeReservedImages == 2 &&
		processorCalls.load() == 3,
		"foreground display preparation was blocked by full speculative completion capacity");

	cache.Prefetch({});
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)) && processorCalls.load() == 3 &&
		cache.GetDiagnostics().backgroundQueued == 0 &&
		cache.GetDiagnostics().speculativePreparedImages == 0,
		"canceling backpressured speculation did not discard queued and completed work");
}

void TestCapacityBlockedSpeculationDoesNotBlockCompletedFrames() {
	TemporaryDirectory temporary;
	const fs::path countFirstFile = temporary.path() / "count-ready-first.png";
	const fs::path countSecondFile = temporary.path() / "count-ready-second.png";
	const fs::path countCloserFile = temporary.path() / "count-queued-closer.png";
	const fs::path bytesReadyFile = temporary.path() / "bytes-ready.png";
	const fs::path bytesCloserFile = temporary.path() / "bytes-queued-closer.png";
	for (const fs::path& filename : {countFirstFile, countSecondFile,
		countCloserFile, bytesReadyFile, bytesCloserFile}) {
		WriteText(filename, "completion priority barrier source");
	}
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::atomic<std::size_t> countProcessorCalls{0};
	jpegview_linux::DisplayImageCache countLimited(0, 2,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			++countProcessorCalls;
			return DisplayCachePreparedTestImage(request);
		});
	const auto countFirst = jpegview_linux::MakeDisplayImageRequest(countFirstFile,
		decoded, 0, 1, 1, false, 2);
	const auto countSecond = jpegview_linux::MakeDisplayImageRequest(countSecondFile,
		decoded, 0, 1, 1, false, 3);
	const auto countCloser = jpegview_linux::MakeDisplayImageRequest(countCloserFile,
		decoded, 0, 1, 1, false, 1);
	countLimited.RequestBackgroundBatch({countFirst, countSecond});
	const bool countCompletionsReady = countLimited.WaitUntilIdle(std::chrono::seconds(2));
	const auto countFull = countLimited.GetDiagnostics();
	countLimited.RequestBackground(countCloser);
	const auto countQueued = countLimited.GetDiagnostics();
	const auto countReleased = countLimited.TakeCompleted(1);
	const bool countDrained = countLimited.WaitUntilIdle(std::chrono::seconds(2));
	const auto countCloserCompleted = countLimited.TakeCompleted(1);
	Expect(countCompletionsReady && countFull.speculativeReservedImages == 2 &&
		countQueued.backgroundQueued == 1 && countReleased.size() == 1 &&
		countReleased.front()->key == countFirst.key && countDrained &&
		countProcessorCalls.load() == 3 && countCloserCompleted.size() == 1 &&
		countCloserCompleted.front()->key == countCloser.key,
		"count-blocked closer prevented a ready frame from releasing completion capacity");
	countLimited.Retire(countReleased.front());
	countLimited.Retire(countCloserCompleted.front());

	constexpr int fullBudgetWidth = 4096;
	constexpr int fullBudgetHeight = 4096;
	const std::size_t fullBudgetBytes = jpegview_linux::DisplayImageCache::
		kMaximumSpeculativeCompletionBytes;
	std::atomic<std::size_t> byteProcessorCalls{0};
	jpegview_linux::DisplayImageCache byteLimited(0, 2,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			++byteProcessorCalls;
			auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>();
			image->filename = request.filename;
			image->source = request.source;
			image->cacheKey = request.cacheKey;
			image->key = request.key;
			image->width = image->height = 1;
			image->bgra.assign(4, 255);
			return image;
		});
	const auto bytesReady = jpegview_linux::MakeDisplayImageRequest(bytesReadyFile,
		decoded, 0, fullBudgetWidth, fullBudgetHeight, false, 2);
	const auto bytesCloser = jpegview_linux::MakeDisplayImageRequest(bytesCloserFile,
		decoded, 0, 1, 1, false, 1);
	byteLimited.RequestBackground(bytesReady);
	const bool byteCompletionReady = byteLimited.WaitUntilIdle(std::chrono::seconds(2));
	const auto bytesFull = byteLimited.GetDiagnostics();
	byteLimited.RequestBackground(bytesCloser);
	const auto bytesQueued = byteLimited.GetDiagnostics();
	const auto bytesReleased = byteLimited.TakeCompleted(1);
	const bool bytesDrained = byteLimited.WaitUntilIdle(std::chrono::seconds(2));
	const auto bytesCloserCompleted = byteLimited.TakeCompleted(1);
	Expect(byteCompletionReady && bytesFull.speculativeReservedImages == 1 &&
		bytesFull.speculativeReservedBytes == fullBudgetBytes &&
		bytesQueued.backgroundQueued == 1 && bytesReleased.size() == 1 &&
		bytesReleased.front()->key == bytesReady.key && bytesDrained &&
		byteProcessorCalls.load() == 2 && bytesCloserCompleted.size() == 1 &&
		bytesCloserCompleted.front()->key == bytesCloser.key,
		"byte-blocked closer prevented a ready frame from releasing completion capacity");
	byteLimited.Retire(bytesReleased.front());
	byteLimited.Retire(bytesCloserCompleted.front());
}

void TestDisplayPrefetchReplacesCachedPrioritiesAndPreservesForeground() {
	TemporaryDirectory temporary;
	const fs::path firstFile = temporary.path() / "replacement-first.png";
	const fs::path secondFile = temporary.path() / "replacement-second.png";
	const fs::path foregroundFile = temporary.path() / "replacement-foreground.png";
	for (const fs::path& filename : {firstFile, secondFile, foregroundFile}) {
		WriteText(filename, "cached priority replacement source");
	}
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::atomic<std::size_t> processorCalls{0};
	const auto prepare = [&processorCalls](const jpegview_linux::DisplayImageRequest& request) {
		++processorCalls;
		auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>(
			*DisplayCachePreparedTestImage(request));
		image->priority = request.priority;
		image->workClass = request.workClass;
		return image;
	};
	jpegview_linux::DisplayImageCache cache(64, 1, prepare);
	const auto first = jpegview_linux::MakeDisplayImageRequest(firstFile,
		decoded, 0, 1, 1, false, 1);
	const auto second = jpegview_linux::MakeDisplayImageRequest(secondFile,
		decoded, 0, 1, 1, false, 2);
	cache.Prefetch({first, second});
	const bool initialBatchPrepared = cache.WaitUntilIdle(std::chrono::seconds(2));
	const auto initialState = cache.GetDiagnostics();
	auto reversedFirst = first;
	auto reversedSecond = second;
	reversedFirst.priority = 2;
	reversedSecond.priority = 1;
	cache.Prefetch({reversedSecond, reversedFirst});
	const auto reversed = cache.TakeCompleted(2);
	Expect(initialBatchPrepared && initialState.cachedImages == 2 &&
		processorCalls.load() == 2 && reversed.size() == 2 &&
		reversed[0]->key == second.key && reversed[1]->key == first.key &&
		processorCalls.load() == 2,
		"replacement Prefetch did not apply its new priorities to retained completions");

	jpegview_linux::DisplayImageCache foregroundCache(64, 1, prepare);
	auto foreground = jpegview_linux::MakeDisplayImageRequest(foregroundFile,
		decoded, 0, 1, 1, false, 0);
	foreground.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	foregroundCache.Request(foreground);
	const bool foregroundPrepared = foregroundCache.WaitUntilIdle(std::chrono::seconds(2));
	auto backgroundView = foreground;
	backgroundView.priority = 3;
	backgroundView.workClass = jpegview_linux::PerfWorkClass::DistantSpeculation;
	foregroundCache.Prefetch({backgroundView});
	const std::set<jpegview_linux::PerfWorkClass> foregroundClass{
		jpegview_linux::PerfWorkClass::ActiveImageSpread};
	const auto preservedForeground = foregroundCache.TakeCompleted(1, foregroundClass);
	Expect(foregroundPrepared && preservedForeground.size() == 1 &&
		preservedForeground.front()->key == foreground.key &&
		preservedForeground.front()->priority == 0 &&
		preservedForeground.front()->workClass ==
			jpegview_linux::PerfWorkClass::ActiveImageSpread,
		"replacement Prefetch demoted or reclassified a foreground completion");
}

void TestPendingCachedCompletionsYieldToStartableColdWork() {
	TemporaryDirectory temporary;
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::vector<fs::path> cachedFiles;
	std::vector<jpegview_linux::DisplayImageRequest> cachedRequests;
	for (std::size_t index = 0; index < 4; ++index) {
		const fs::path filename = temporary.path() /
			("cached-far-" + std::to_string(index) + ".png");
		WriteText(filename, "retained distant completion");
		cachedFiles.push_back(filename);
	}
	const fs::path nearestFile = temporary.path() / "cold-nearest.png";
	WriteText(nearestFile, "cold nearest completion");
	std::mutex nearestMutex;
	std::condition_variable nearestChanged;
	bool nearestStarted = false;
	bool releaseNearest = false;
	std::atomic<std::size_t> processorCalls{0};
	jpegview_linux::DisplayImageCache cache(128, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			++processorCalls;
			if (request.filename == nearestFile) {
				std::unique_lock<std::mutex> lock(nearestMutex);
				nearestStarted = true;
				nearestChanged.notify_all();
				nearestChanged.wait(lock, [&] { return releaseNearest; });
			}
			return DisplayCachePreparedTestImage(request);
		});
	ScopedConditionRelease releaseNearestOnExit(nearestMutex, nearestChanged, releaseNearest);
	for (const fs::path& filename : cachedFiles) {
		const auto request = jpegview_linux::MakeDisplayImageRequest(
			filename, decoded, 0, 1, 1, false);
		const auto prepared = cache.RequestAndWait(request);
		const auto consumed = cache.TakeCompleted(1);
		Expect(prepared && consumed.size() == 1 && consumed.front()->key == request.key,
			"could not seed a retained display image for the cached refill test");
		cachedRequests.push_back(request);
	}
	const bool allSourcesRetained = cache.GetDiagnostics().cachedImages == cachedRequests.size();
	for (std::size_t index = 0; index < cachedRequests.size(); ++index) {
		cachedRequests[index].priority = index + 3;
	}
	cache.Prefetch(cachedRequests);
	const auto filled = cache.GetDiagnostics();
	auto nearest = jpegview_linux::MakeDisplayImageRequest(
		nearestFile, decoded, 0, 1, 1, false, 1);
	cache.RequestBackground(nearest);
	const auto queued = cache.GetDiagnostics();
	const auto released = cache.TakeCompleted(1);
	bool nearestEnteredProcessor = false;
	{
		std::unique_lock<std::mutex> lock(nearestMutex);
		nearestEnteredProcessor = nearestChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return nearestStarted; });
	}
	const auto whileNearestBlocked = cache.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(nearestMutex);
		releaseNearest = true;
	}
	nearestChanged.notify_all();
	const bool nearestDrained = nearestEnteredProcessor &&
		cache.WaitUntilIdle(std::chrono::seconds(2));
	const auto nearestCompletion = nearestDrained ? cache.TakeCompleted(1) :
		std::vector<jpegview_linux::DisplayImageCache::ImagePtr>{};
	const auto fartherCompletion = cache.TakeCompleted(1);
	const auto afterRefill = cache.GetDiagnostics();
	const std::size_t expectedProcessorCalls = cachedRequests.size() + 1;
	Expect(allSourcesRetained && filled.cachedImages == cachedRequests.size() &&
		filled.speculativePreparedImages == 2 && filled.speculativeReservedImages == 2 &&
		queued.backgroundQueued == 1 && queued.speculativeReservedImages == 2 &&
		released.size() == 1 && released.front()->key == cachedRequests.front().key &&
		nearestEnteredProcessor && whileNearestBlocked.backgroundActive == 1 &&
		whileNearestBlocked.backgroundQueued == 0 &&
		whileNearestBlocked.speculativePreparedImages == 1 &&
		whileNearestBlocked.speculativeReservedImages == 2 &&
		whileNearestBlocked.speculativeReservedImages <=
			jpegview_linux::DisplayImageCache::kMaximumSpeculativeCompletions &&
		whileNearestBlocked.speculativeReservedBytes <=
			jpegview_linux::DisplayImageCache::kMaximumSpeculativeCompletionBytes &&
		nearestDrained && processorCalls.load() == expectedProcessorCalls &&
		nearestCompletion.size() == 1 && nearestCompletion.front()->key == nearest.key &&
		fartherCompletion.size() == 1 &&
		fartherCompletion.front()->key == cachedRequests[1].key &&
		afterRefill.speculativeReservedImages <=
			jpegview_linux::DisplayImageCache::kMaximumSpeculativeCompletions,
		"cached refill reserved capacity ahead of the startable nearest cold request");
}

void TestPendingCachedCompletionsYieldToEqualPriorityColdWork() {
	TemporaryDirectory temporary;
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::vector<jpegview_linux::DisplayImageRequest> cachedRequests;
	for (std::size_t index = 0; index < 4; ++index) {
		const fs::path filename = temporary.path() /
			("equal-priority-cached-" + std::to_string(index) + ".png");
		WriteText(filename, "equal-priority retained completion");
		cachedRequests.push_back(jpegview_linux::MakeDisplayImageRequest(
			filename, decoded, 0, 1, 1, false));
	}
	const fs::path coldFile = temporary.path() / "equal-priority-cold.png";
	WriteText(coldFile, "equal-priority cold completion");
	std::mutex mutex;
	std::condition_variable changed;
	bool coldStarted = false;
	bool releaseCold = false;
	std::atomic<std::size_t> processorCalls{0};
	jpegview_linux::DisplayImageCache cache(128, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			++processorCalls;
			if (request.filename == coldFile) {
				std::unique_lock<std::mutex> lock(mutex);
				coldStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseCold; });
			}
			return DisplayCachePreparedTestImage(request);
		});
	ScopedConditionRelease releaseColdOnExit(mutex, changed, releaseCold);
	for (const auto& request : cachedRequests) {
		const auto prepared = cache.RequestAndWait(request);
		const auto consumed = cache.TakeCompleted(1);
		Expect(prepared && consumed.size() == 1 && consumed.front()->key == request.key,
			"could not seed retained images for the equal-priority refill test");
	}
	for (std::size_t index = 0; index < cachedRequests.size(); ++index) {
		cachedRequests[index].priority = index + 3;
	}
	cache.Prefetch(cachedRequests);
	const auto filled = cache.GetDiagnostics();
	auto cold = jpegview_linux::MakeDisplayImageRequest(
		coldFile, decoded, 0, 1, 1, false, 5);
	cache.RequestBackground(cold);
	const auto queued = cache.GetDiagnostics();
	const auto released = cache.TakeCompleted(1);
	bool entered = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		entered = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return coldStarted;
		});
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseCold = true;
	}
	changed.notify_all();
	const bool drained = cache.WaitUntilIdle(std::chrono::seconds(2));
	Expect(filled.speculativePreparedImages == 2 &&
		queued.backgroundQueued == 1 && released.size() == 1 &&
		released.front()->key == cachedRequests.front().key && entered && drained &&
		processorCalls.load() == cachedRequests.size() + 1,
		"equal-priority cached refill reserved capacity ahead of startable cold work");
}

void TestDisplayOversizedSpeculationAndReservationRecovery() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "oversized-speculation.png";
	const fs::path failure = temporary.path() / "allocation-failure.png";
	const fs::path recovery = temporary.path() / "allocation-recovery.png";
	WriteText(filename, "oversized source");
	WriteText(failure, "allocation failure source");
	WriteText(recovery, "allocation recovery source");
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::atomic<std::size_t> oversizedCalls{0};
	jpegview_linux::DisplayImageCache oversized(0, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			++oversizedCalls;
			return DisplayCachePreparedTestImage(request);
		});
	const auto tooLarge = jpegview_linux::MakeDisplayImageRequest(filename, decoded,
		0, 4096, 4097, false, 1);
	oversized.RequestBackground(tooLarge);
	Expect(oversized.WaitUntilIdle(std::chrono::milliseconds(100)) &&
		oversizedCalls.load() == 0 && !oversized.HasPendingOrCached(tooLarge.key) &&
		oversized.GetDiagnostics().backgroundQueued == 0,
		"an oversized speculative result stayed queued or reached the image processor");

	std::atomic<std::size_t> allocationCalls{0};
	const std::size_t expectedBytes = 40u * 1024u * 1024u;
	jpegview_linux::DisplayImageCache allocation(0, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			if (++allocationCalls == 1) throw std::bad_alloc();
			return DisplayCachePreparedTestImage(request);
		});
	const auto throws = jpegview_linux::MakeDisplayImageRequest(failure, decoded,
		0, 4096, 2560, false, 1);
	const auto succeeds = jpegview_linux::MakeDisplayImageRequest(recovery, decoded,
		0, 4096, 2560, false, 2);
	allocation.RequestBackgroundBatch({throws, succeeds});
	Expect(allocation.WaitUntilIdle(std::chrono::seconds(3)) && allocationCalls.load() == 2,
		"display allocation failure retained a reservation and blocked later speculation");
	const auto recovered = allocation.GetDiagnostics();
	Expect(recovered.speculativePreparedImages == 1 &&
		recovered.speculativeReservedImages == 1 &&
		recovered.speculativeReservedBytes == expectedBytes,
		"display reservation accounting did not recover after allocation failure");
}

void TestDisplayBackpressuredShutdownRetiresWithoutPumping() {
	TemporaryDirectory temporary;
	const auto decoded = DisplayCacheTestImage(2, 2);
	std::mutex destructionMutex;
	std::condition_variable destructionChanged;
	std::size_t destroyedImages = 0;
	bool destroyedOffCaller = true;
	const std::thread::id caller = std::this_thread::get_id();
	auto cache = std::make_unique<jpegview_linux::DisplayImageCache>(0, 2,
		[&](const jpegview_linux::DisplayImageRequest& request) {
		auto* raw = new jpegview_linux::PreparedDisplayImage();
		raw->filename = request.filename;
		raw->source = request.source;
		raw->cacheKey = request.cacheKey;
		raw->key = request.key;
		raw->width = request.targetWidth;
		raw->height = request.targetHeight;
		raw->bgra.assign(1024u * 1024u, 255);
		return std::shared_ptr<const jpegview_linux::PreparedDisplayImage>(raw,
			[&](const jpegview_linux::PreparedDisplayImage* image) {
				{
					std::lock_guard<std::mutex> lock(destructionMutex);
					++destroyedImages;
					destroyedOffCaller = destroyedOffCaller &&
						std::this_thread::get_id() != caller;
				}
				destructionChanged.notify_all();
				delete image;
			});
		});
	std::vector<jpegview_linux::DisplayImageRequest> requests;
	for (std::size_t priority = 1; priority <= 4; ++priority) {
		const fs::path filename = temporary.path() /
			("shutdown-" + std::to_string(priority) + ".png");
		WriteText(filename, "shutdown image");
		requests.push_back(jpegview_linux::MakeDisplayImageRequest(filename, decoded,
			0, 512, 512, false, priority));
	}
	cache->RequestBackgroundBatch(requests);
	const auto completionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < completionDeadline &&
		cache->GetDiagnostics().speculativePreparedImages < 2) {
		std::this_thread::yield();
	}
	Expect(cache->GetDiagnostics().speculativePreparedImages == 2 &&
		cache->GetDiagnostics().backgroundQueued == 2,
		"shutdown fixture did not fill the bounded completion queue");
	cache->Shutdown();
	cache->RequestBackgroundBatch(requests);
	Expect(!cache->HasPendingWork() &&
		cache->GetDiagnostics().backgroundQueued == 0,
		"display cache accepted speculative work after terminal shutdown");
	cache.reset();
	std::unique_lock<std::mutex> lock(destructionMutex);
	const bool allImagesRetired = destructionChanged.wait_for(lock,
		std::chrono::seconds(3), [&] { return destroyedImages == 2; });
	Expect(allImagesRetired && destroyedOffCaller,
		"display destruction did not retire both completed pixels off-caller without event-loop pumping");
}

void TestQueuedDecodedImageRetiresOffCaller() {
	TemporaryDirectory temporary;
	const fs::path activeFile = temporary.path() / "decoded-retirement-active.png";
	const fs::path queuedFile = temporary.path() / "decoded-retirement-queued.png";
	WriteText(activeFile, "active");
	WriteText(queuedFile, "queued");
	const auto activeDecoded = DisplayCacheTestImage(2, 2);
	std::mutex mutex;
	std::condition_variable changed;
	bool activeStarted = false;
	bool releaseActive = false;
	std::thread::id destroyedOn;
	bool destroyed = false;
	const auto caller = std::this_thread::get_id();
	jpegview_linux::DisplayImageCache cache(0, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			if (request.filename == activeFile) {
				std::unique_lock<std::mutex> lock(mutex);
				activeStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseActive; });
			}
			return DisplayCachePreparedTestImage(request);
		});
	ScopedConditionRelease releaseActiveOnExit(mutex, changed, releaseActive);
	const auto active = jpegview_linux::MakeDisplayImageRequest(activeFile,
		activeDecoded, 0, 2, 2, false, 1);
	cache.RequestBackground(active);
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return activeStarted; }),
			"decoded-retirement worker did not enter its active request");
	}
	auto* raw = new DecodedImage(*DisplayCacheTestImage(2, 2));
	auto queuedDecoded = std::shared_ptr<DecodedImage>(raw,
		[&](DecodedImage* image) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				destroyedOn = std::this_thread::get_id();
				destroyed = true;
			}
			changed.notify_all();
			delete image;
		});
	auto queued = jpegview_linux::MakeDisplayImageRequest(queuedFile,
		queuedDecoded, 0, 2, 2, false, 2);
	cache.RequestBackground(queued);
	queued.decoded.reset();
	queuedDecoded.reset();
	cache.CancelBackground(queued.key);
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return destroyed; }),
			"canceled queued decoded pixels were not drained by the retirement worker");
		releaseActive = true;
	}
	changed.notify_all();
	Expect(destroyedOn != caller && cache.WaitUntilIdle(std::chrono::seconds(2)),
		"canceling a queued decoded request destroyed its last pixels on the caller thread");
}

void TestDisplayRetirementSkipsSharedDecodedOwner() {
	TemporaryDirectory temporary;
	const fs::path activeFile = temporary.path() / "retirement-ready-display.png";
	const fs::path queuedFile = temporary.path() / "retirement-shared-decoded.png";
	WriteText(activeFile, "active");
	WriteText(queuedFile, "queued");
	const auto activeDecoded = DisplayCacheTestImage(2, 2);
	std::mutex mutex;
	std::condition_variable changed;
	bool activeStarted = false;
	bool releaseActive = false;
	bool displayDestroyed = false;
	bool decodedDestroyed = false;
	std::thread::id displayDestroyedOn;
	std::thread::id decodedDestroyedOn;
	const std::thread::id caller = std::this_thread::get_id();
	auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(64);
	jpegview_linux::DisplayImageCache cache(0, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			if (request.filename == activeFile) {
				std::unique_lock<std::mutex> lock(mutex);
				activeStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseActive; });
			}
		auto* raw = new jpegview_linux::PreparedDisplayImage();
		raw->filename = request.filename;
		raw->source = request.source;
		raw->cacheKey = request.cacheKey;
		raw->key = request.key;
		raw->width = raw->height = 2;
		raw->bgra.assign(16, 255);
		return std::shared_ptr<const jpegview_linux::PreparedDisplayImage>(raw,
			[&](const jpegview_linux::PreparedDisplayImage* image) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					displayDestroyed = true;
					displayDestroyedOn = std::this_thread::get_id();
				}
				changed.notify_all();
			delete image;
			});
		}, budget);
	ScopedConditionRelease releaseActiveOnExit(mutex, changed, releaseActive);
	const auto active = jpegview_linux::MakeDisplayImageRequest(activeFile,
		activeDecoded, 0, 2, 2, false, 1);
	cache.RequestBackground(active);
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return activeStarted; }),
			"retirement worker did not reach the active decode barrier");
	}
	DecodedImage* rawDecoded = new DecodedImage(*DisplayCacheTestImage(2, 2));
	auto externalDecoded = std::shared_ptr<DecodedImage>(rawDecoded,
		[&](DecodedImage* image) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				decodedDestroyed = true;
				decodedDestroyedOn = std::this_thread::get_id();
			}
			changed.notify_all();
			delete image;
		});
	auto queued = jpegview_linux::MakeDisplayImageRequest(queuedFile,
		externalDecoded, 0, 2, 2, false, 2);
	cache.RequestBackground(queued);
	queued.decoded.reset();
	cache.CancelBackground(queued.key);
	const auto workingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (budget->Snapshot().activeWorkingBytes == 0 &&
		std::chrono::steady_clock::now() < workingDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(budget->Snapshot().activeWorkingBytes == 16,
		"uncached current-display pixels were not tracked while a caller alias remained");
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseActive = true;
	}
	changed.notify_all();
	const auto publishDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < publishDeadline &&
		cache.GetDiagnostics().preparedImages != 1) {
		std::this_thread::yield();
	}
	cache.Prefetch({});
	bool displayRetiredBeforeExternalOwner = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		displayRetiredBeforeExternalOwner = changed.wait_for(lock,
			std::chrono::seconds(2), [&] { return displayDestroyed; });
	}
	const bool decodedStillOwned = externalDecoded.use_count() == 1;
	externalDecoded.reset();
	{
		std::unique_lock<std::mutex> lock(mutex);
		(void)changed.wait_for(lock, std::chrono::seconds(2), [&] { return decodedDestroyed; });
	}
	Expect(displayRetiredBeforeExternalOwner,
		"a shared decoded alias blocked independent prepared-image retirement");
	Expect(!decodedStillOwned,
		"the retirement coordinator failed to retain the decoded allocation while its alias existed");
	Expect(decodedDestroyed && displayDestroyedOn != caller && decodedDestroyedOn != caller,
		"shared decoded or prepared pixels were destroyed on the caller or not destroyed");
	const auto releasedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (budget->Snapshot().activeWorkingBytes != 0 &&
		std::chrono::steady_clock::now() < releasedDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(budget->Snapshot().activeWorkingBytes == 0,
		"retirement workers did not release active working charges after destruction");
}

void TestCrossCacheDecodedRetirementCancellationAndEviction() {
	TemporaryDirectory temporary;
	const fs::path blockerFile = temporary.path() / "cross-retirement-blocker.png";
	const fs::path queuedFile = temporary.path() / "cross-retirement-queued.png";
	WriteText(blockerFile, "blocker");
	WriteText(queuedFile, "queued");
	auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(64);
	std::mutex mutex;
	std::condition_variable changed;
	bool blockerStarted = false;
	bool releaseBlocker = false;
	bool decodedDestroyed = false;
	std::thread::id decodedDestroyedOn;
	const std::thread::id caller = std::this_thread::get_id();
	jpegview_linux::DecodedImageCache decodedCache(64, {}, budget);
	auto* rawDecoded = new DecodedImage(*CachedTestImage(16));
	auto decodedOwner = std::shared_ptr<DecodedImage>(rawDecoded,
		[&](DecodedImage* image) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				decodedDestroyed = true;
				decodedDestroyedOn = std::this_thread::get_id();
			}
			changed.notify_all();
			delete image;
		});
	decodedCache.Store(queuedFile, decodedOwner);
	jpegview_linux::DisplayImageCache displayCache(64, 1,
		[&](const jpegview_linux::DisplayImageRequest& request) {
			if (request.filename == blockerFile) {
				std::unique_lock<std::mutex> lock(mutex);
				blockerStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseBlocker; });
			}
			return DisplayCachePreparedTestImage(request);
		}, budget);
	ScopedConditionRelease unblock(mutex, changed, releaseBlocker);
	const auto blocker = jpegview_linux::MakeDisplayImageRequest(blockerFile,
		DisplayCacheTestImage(2, 2), 0, 2, 2, false, 1);
	displayCache.RequestBackground(blocker);
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return blockerStarted; }),
			"cross-cache retirement worker did not reach the blocker");
	}
	auto decodedAlias = decodedCache.Find(queuedFile);
	Expect(decodedAlias != nullptr, "cross-cache queued decoded entry was not available");
	auto decodedRetirementHold = decodedAlias;
	auto queued = jpegview_linux::MakeDisplayImageRequest(queuedFile,
		decodedAlias, 0, 2, 2, false, 2);
	displayCache.RequestBackground(queued);
	queued.decoded.reset();
	decodedAlias.reset();
	decodedOwner.reset();
	displayCache.CancelBackground(queued.key);
	Expect(decodedCache.EvictLeastRecentlyUsed(
		jpegview_linux::CacheProtectionTier::Active) == 16 && budget->Used() == 16,
		"decoded cache eviction did not transfer its reservation to shared retirement");
	decodedRetirementHold.reset();
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseBlocker = true;
	}
	changed.notify_all();
	Expect(displayCache.WaitUntilIdle(std::chrono::seconds(2)),
		"display cancellation did not settle after releasing the blocker");
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return decodedDestroyed; }),
			"cross-cache canceled and evicted pixels remained multiply retired");
	}
	displayCache.Clear();
	decodedCache.Clear();
	const auto budgetDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (budget->Used() != 0 && std::chrono::steady_clock::now() < budgetDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(decodedDestroyedOn != caller && budget->Used() == 0 &&
		budget->Snapshot().decodedPixelBytes == 0 &&
		budget->Snapshot().preparedFrameBytes == 0,
		"cross-cache retirement destroyed pixels on the caller or kept budget charges alive");
}

void TestForegroundCompletionKeepsCachedPriorityClass() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "foreground-completion-priority.png";
	WriteText(filename, "foreground completion");
	const auto decoded = DisplayCacheTestImage(2, 2);
	jpegview_linux::DisplayImageCache cache(0, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
			return DisplayCachePreparedTestImage(request);
		});
	auto foreground = jpegview_linux::MakeDisplayImageRequest(filename, decoded,
		0, 2, 2, false, 0);
	cache.Request(foreground);
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"foreground completion fixture did not prepare its active image");
	auto backgroundMerge = foreground;
	backgroundMerge.priority = 3;
	backgroundMerge.workClass = jpegview_linux::PerfWorkClass::DistantSpeculation;
	cache.RequestBackgroundBatch({backgroundMerge});
	const std::set<jpegview_linux::PerfWorkClass> foregroundClass{
		jpegview_linux::PerfWorkClass::ActiveImageSpread};
	const auto completion = cache.TakeCompleted(1, foregroundClass);
	Expect(completion.size() == 1 && completion.front()->key == foreground.key,
		"a background batch demoted an already queued foreground completion");
}

void TestDeferredDisplayUploadStagesAliasedPreparedCompletion() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "pending-neighbor.png";
	WriteText(filename, "pending prepared image");
	const auto decoded = DisplayCacheTestImage(2, 2);
	const auto makeNeighborRequest = [&]() {
		auto request = jpegview_linux::MakeDisplayImageRequest(filename, decoded,
			0, 2, 2, false, 1);
		request.workClass = jpegview_linux::PerfWorkClass::NearestNavigationNeighbor;
		return request;
	};
	const std::set<jpegview_linux::PerfWorkClass> permitted{
		jpegview_linux::PerfWorkClass::ActiveImageSpread,
		jpegview_linux::PerfWorkClass::NearestNavigationNeighbor};
	const std::vector<jpegview_linux::DisplayUploadPriority> priorities{
		{jpegview_linux::PerfWorkClass::NearestNavigationNeighbor, 1},
		{jpegview_linux::PerfWorkClass::ActiveImageSpread, 0}};
	const std::vector<std::size_t> selected =
		jpegview_linux::PlanDisplayTextureUploads(priorities, permitted, 1);
	Expect(selected == std::vector<std::size_t>{1},
		"active texture admission test did not leave the lower-priority completion pending");

	// Establish the failure this handoff must prevent: eviction removes the
	// retained entry, but its queued alias keeps the retained reservation alive.
	{
		auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(16);
		jpegview_linux::DisplayImageCache cache(16, 1, {}, budget);
		cache.RequestBackground(makeNeighborRequest());
		Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
			"lower-priority prepared completion did not finish before its deadline");
		auto queued = cache.TakeCompleted(1);
		Expect(queued.size() == 1 && cache.CachedBytes() == 16 &&
			budget->Snapshot().preparedFrameBytes == 16,
			"pending completion did not retain its prepared-frame reservation");
		jpegview_linux::CacheAdmissionPolicy admission(*budget);
		const jpegview_linux::CacheReservation blocked = admission.Reserve(16,
			jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
			jpegview_linux::CacheProtectionTier::Active,
			[&cache](jpegview_linux::CacheProtectionTier tier) {
				return cache.EvictLeastRecentlyUsed(tier) != 0;
			});
		Expect(!blocked && cache.CachedBytes() == 0 &&
			budget->Snapshot().preparedFrameBytes == 16 &&
			budget->Snapshot().imageTextureBytes == 0,
			"active admission reclaimed a prepared frame still aliased by a pending upload");
	}

	// A foreground insert can evict a completed neighbor before either result is
	// taken. The retired reservation still accounts the pixels until the renderer
	// stages both returned completions.
	{
		const fs::path foregroundFilename = temporary.path() / "foreground-active.png";
		WriteText(foregroundFilename, "foreground prepared image");
		auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(16);
		jpegview_linux::DisplayImageCache cache(16, 1, {}, budget);
		const auto background = makeNeighborRequest();
		auto foreground = jpegview_linux::MakeDisplayImageRequest(foregroundFilename,
			decoded, 0, 2, 2, false, 0);
		foreground.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
		cache.RequestBackground(background);
		Expect(cache.WaitUntilIdle(std::chrono::seconds(2)) &&
			cache.CachedBytes() == 16,
			"background completion was not retained before foreground replacement");
		cache.Request(foreground);
		Expect(cache.WaitUntilIdle(std::chrono::seconds(2)) &&
			cache.CachedBytes() == 0,
			"foreground preparation did not evict the still-completed neighbor");
		auto completed = cache.TakeCompleted(2);
		const auto active = std::find_if(completed.begin(), completed.end(),
			[&foreground](const auto& image) {
				return image && image->cacheKey == foreground.cacheKey;
			});
		const auto evicted = std::find_if(completed.begin(), completed.end(),
			[&background](const auto& image) {
				return image && image->cacheKey == background.cacheKey;
			});
		Expect(completed.size() == 2 && active != completed.end() &&
			evicted != completed.end() && budget->Snapshot().preparedFrameBytes == 16,
			"foreground replacement did not preserve both completions and the retired charge");
		std::vector<jpegview_linux::DisplayImageCache::ImagePtr> stagedImages{
			*evicted, *active};
		cache.ReleaseForUpload(stagedImages);
		const auto staged = budget->Snapshot();
		Expect(staged.preparedFrameBytes == 0 && staged.uploadStagingBytes == 32 &&
			staged.retainedBytes == 0,
			"ownerless retired completion did not transfer its one charge to upload staging");

		jpegview_linux::CacheAdmissionPolicy admission(*budget);
		auto texture = admission.Reserve(16,
			jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
			jpegview_linux::CacheProtectionTier::Active, {});
		Expect(texture && budget->Snapshot().imageTextureBytes == 16 &&
			budget->Snapshot().retainedBytes == 16,
			"staged retired neighbor blocked foreground texture admission");
		texture.Reset();
		completed.clear();
		stagedImages.clear();
		const auto retiredDeadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (budget->Snapshot().uploadStagingBytes != 0 &&
			std::chrono::steady_clock::now() < retiredDeadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Expect(budget->Snapshot().uploadStagingBytes == 0,
			"staged foreground completions remained charged after their aliases retired");
	}

	// The renderer stages an unselected completion before it attempts the active
	// texture reservation. The pending alias remains charged, but no longer uses
	// retained capacity and therefore cannot block foreground admission.
	auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(16);
	{
		jpegview_linux::DisplayImageCache cache(16, 1, {}, budget);
		cache.RequestBackground(makeNeighborRequest());
		Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
			"prepared completion did not finish before staging");
		auto queued = cache.TakeCompleted(1);
		Expect(queued.size() == 1,
			"prepared completion was not available for pending upload staging");
		std::vector<jpegview_linux::DisplayImageCache::ImagePtr> deferredImages{
			queued.front()};
		cache.ReleaseForUpload(deferredImages);
		const jpegview_linux::CacheBudgetSnapshot staged = budget->Snapshot();
		Expect(cache.CachedBytes() == 0 && staged.preparedFrameBytes == 0 &&
			staged.uploadStagingBytes == 16,
			"pending upload staging did not transfer its retained reservation exactly once");

		jpegview_linux::CacheAdmissionPolicy admission(*budget);
		bool triedEviction = false;
		jpegview_linux::CacheReservation active = admission.Reserve(16,
			jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
			jpegview_linux::CacheProtectionTier::Active,
			[&cache, &triedEviction](jpegview_linux::CacheProtectionTier tier) {
				triedEviction = true;
				return cache.EvictLeastRecentlyUsed(tier) != 0;
			});
		const jpegview_linux::CacheBudgetSnapshot admitting = budget->Snapshot();
		Expect(active && !triedEviction && admitting.imageTextureBytes == 16 &&
			admitting.preparedFrameBytes == 0 && admitting.uploadStagingBytes == 16 &&
			admitting.retainedBytes == 16,
			"staged lower-priority pixels blocked active texture admission or lost accounting");
		active.Reset();
		Expect(budget->Snapshot().uploadStagingBytes == 16,
			"pending staging charge disappeared while its pixel aliases were still live");

		queued.clear();
		deferredImages.clear();
		const auto retiredDeadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (budget->Snapshot().uploadStagingBytes != 0 &&
			std::chrono::steady_clock::now() < retiredDeadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Expect(budget->Snapshot().uploadStagingBytes == 0,
			"pending upload staging remained charged after its final alias retired");
	}
}

void TestSharedCacheBudgetAccounting() {
	jpegview_linux::SharedCacheBudget budget(32);
	Expect(budget.Capacity() == 32 && budget.Used() == 0 && budget.Available() == 32,
		"shared cache budget did not expose its initial capacity");
	auto firstOwner = std::make_shared<int>(1);
	std::shared_ptr<const void> firstAllocation(firstOwner, firstOwner.get());
	auto first = budget.TryReserve(20,
		jpegview_linux::CacheMemoryCategory::RetainedDecodedPixels, firstAllocation);
	Expect(first && budget.Used() == 20 && budget.Available() == 12,
		"shared cache budget did not account for a reservation");
	Expect(!budget.TryReserve(13,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames) && budget.Used() == 20,
		"shared cache budget exceeded its total capacity");
	auto second = budget.TryReserve(12,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames);
	Expect(second && budget.Used() == 32 && budget.Available() == 0,
		"shared cache budget rejected its exact remaining capacity");
	first.Reset();
	Expect(budget.Used() == 12 && budget.Available() == 20,
		"move-only reservation destruction did not release exactly its bytes");
	budget.SetCapacity(16);
	Expect(budget.Capacity() == 16 && budget.Used() == 12 && budget.Available() == 4 &&
		!budget.TryReserve(5,
			jpegview_linux::CacheMemoryCategory::RetainedDecodedPixels),
		"lowered shared cache capacity incorrectly discarded or admitted reservations");
	second.Reset();
	Expect(budget.Used() == 0 && budget.Available() == 16,
		"move-only reservation rollback did not return the remaining bytes");
	Expect(jpegview_linux::CacheBytesFromMiB(2) == 2u * 1024u * 1024u,
		"cache MiB conversion returned the wrong byte count");

	jpegview_linux::SharedCacheBudget aliasBudget(32);
	auto sharedPixels = std::make_shared<int>(2);
	std::shared_ptr<const void> sharedPixelOwner(sharedPixels, sharedPixels.get());
	auto retainedOwner = aliasBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedDecodedPixels, sharedPixelOwner);
	auto uploadAlias = retainedOwner.ShareAlias();
	Expect(retainedOwner && uploadAlias && aliasBudget.Used() == 16,
		"shared allocation aliases charged their bytes more than once");
	Expect(!uploadAlias.Reclassify(
		jpegview_linux::CacheMemoryCategory::TemporaryUploadStaging) &&
		aliasBudget.Snapshot().decodedPixelBytes == 16 &&
		aliasBudget.Snapshot().uploadStagingBytes == 0,
		"an upload alias reclassified pixels while a retained cache owner still needed them");
	retainedOwner.Reset();
	Expect(aliasBudget.Used() == 16 && uploadAlias.Reclassify(
		jpegview_linux::CacheMemoryCategory::TemporaryUploadStaging) &&
		aliasBudget.Used() == 0 &&
		aliasBudget.Snapshot().uploadStagingBytes == 16,
		"the final retained owner could not transfer its allocation to staging accounting");
	uploadAlias.Reset();
	Expect(aliasBudget.Snapshot().uploadStagingBytes == 0,
		"temporary alias destruction did not release its accounting charge");

	jpegview_linux::SharedCacheBudget retainedAliasBudget(16);
	auto retainedAliasPixels = std::make_shared<int>(4);
	std::shared_ptr<const void> retainedAliasOwner(retainedAliasPixels,
		retainedAliasPixels.get());
	auto retainedEntryA = retainedAliasBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames,
		retainedAliasOwner);
	auto retainedEntryB = retainedAliasBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames,
		retainedAliasOwner);
	retainedEntryA.RelinquishRetainedOwnership();
	auto firstStagingAlias = retainedAliasBudget.TrackTemporary(16,
		jpegview_linux::CacheMemoryCategory::TemporaryUploadStaging,
		retainedAliasOwner);
	Expect(retainedEntryA && retainedEntryB && firstStagingAlias &&
		retainedAliasBudget.Used() == 16 &&
		retainedAliasBudget.Snapshot().preparedFrameBytes == 16 &&
		retainedAliasBudget.Snapshot().uploadStagingBytes == 0,
		"one live retained cache alias did not keep the shared allocation retained");
	retainedEntryB.RelinquishRetainedOwnership();
	auto secondStagingAlias = retainedAliasBudget.TrackTemporary(16,
		jpegview_linux::CacheMemoryCategory::TemporaryUploadStaging,
		retainedAliasOwner);
	Expect(secondStagingAlias && retainedAliasBudget.Used() == 0 &&
		retainedAliasBudget.Snapshot().preparedFrameBytes == 0 &&
		retainedAliasBudget.Snapshot().uploadStagingBytes == 16,
		"ownerless retained aliases did not transfer their shared charge exactly once");
	firstStagingAlias.Reset();
	secondStagingAlias.Reset();
	retainedEntryA.Reset();
	retainedEntryB.Reset();
	Expect(retainedAliasBudget.Snapshot().uploadStagingBytes == 0,
		"shared staging charge remained after every allocation alias retired");

	// A new control block at the same raw address is a distinct allocation even
	// while the old reservation keeps the expired control block observable.
	char sameAddressStorage = 0;
	jpegview_linux::SharedCacheBudget addressReuseBudget(24);
	auto oldOwner = std::shared_ptr<const void>(&sameAddressStorage,
		[](const void*) {});
	auto oldAddressCharge = addressReuseBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedDecodedPixels, oldOwner);
	oldOwner.reset();
	auto newOwner = std::shared_ptr<const void>(&sameAddressStorage,
		[](const void*) {});
	Expect(!addressReuseBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames, newOwner) &&
		addressReuseBudget.Used() == 16,
		"same-address allocation reuse shared an expired allocation's reservation");
	oldAddressCharge.Reset();
	auto newAddressCharge = addressReuseBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames, newOwner);
	Expect(newAddressCharge && addressReuseBudget.Used() == 16,
		"new control-block allocation could not reserve after the old charge retired");

	jpegview_linux::SharedCacheBudget rollbackBudget(32);
	auto stagingPixels = std::make_shared<int>(3);
	std::shared_ptr<const void> stagingOwner(stagingPixels, stagingPixels.get());
	auto staging = rollbackBudget.TrackTemporary(16,
		jpegview_linux::CacheMemoryCategory::TemporaryUploadStaging, stagingOwner);
	jpegview_linux::CacheAdmissionPolicy admission(rollbackBudget);
	{
		auto textureReservation = admission.Reserve(16,
			jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
			jpegview_linux::CacheProtectionTier::Active, {});
		Expect(textureReservation && rollbackBudget.Used() == 16 &&
			rollbackBudget.Snapshot().uploadStagingBytes == 16,
			"texture upload admission did not account for CPU staging overlap");
		// Returning from a failed upload path destroys this still-local token.
	}
	Expect(rollbackBudget.Used() == 0 &&
		rollbackBudget.Snapshot().imageTextureBytes == 0 &&
		rollbackBudget.Snapshot().uploadStagingBytes == 16,
		"a failed texture upload leaked its retained texture reservation");
	staging.Reset();
	Expect(rollbackBudget.Snapshot().uploadStagingBytes == 0,
		"upload staging charge outlived its final CPU alias");

	TemporaryDirectory temporary;
	const fs::path decodedFile = temporary.path() / "decoded.jpg";
	const fs::path displayFile = temporary.path() / "display.jpg";
	WriteText(decodedFile, "decoded");
	WriteText(displayFile, "display");
	auto shared = std::make_shared<jpegview_linux::SharedCacheBudget>(32);
	jpegview_linux::DecodedImageCache decodedCache(64, {}, shared);
	decodedCache.Store(decodedFile, CachedTestImage(16));
	jpegview_linux::DisplayImageCache displayCache(64, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
				auto result = std::make_shared<jpegview_linux::PreparedDisplayImage>();
				result->key = request.key;
				result->source = request.source;
				result->cacheKey = request.cacheKey;
				result->width = 2;
			result->height = 2;
			result->bgra.assign(16, 0);
			return result;
		}, shared);
	const jpegview_linux::DisplayImageRequest displayRequest =
		jpegview_linux::MakeDisplayImageRequest(
			displayFile, DisplayCacheTestImage(2, 2), 0, 2, 2, false);
	displayCache.Request(displayRequest);
	Expect(displayCache.WaitUntilIdle(std::chrono::seconds(2)) && shared->Used() == 32 &&
		decodedCache.CachedBytes() == 16 && displayCache.CachedBytes() == 16,
		"decoded and display caches did not share one aggregate limit");
	auto displayAlias = displayCache.TakeCompleted(1);
	Expect(displayAlias.size() == 1,
		"prepared cache did not publish an alias for upload accounting");
	displayCache.ReleaseForUpload(displayAlias.front());
	Expect(displayCache.CachedBytes() == 0 && shared->Used() == 16 &&
		shared->Snapshot().uploadStagingBytes == 16,
		"upload handoff did not expose temporary CPU staging accounting");
	displayAlias.clear();
	const auto displayRetiredDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (shared->Snapshot().uploadStagingBytes != 0 &&
		std::chrono::steady_clock::now() < displayRetiredDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(shared->Snapshot().uploadStagingBytes == 0 && shared->Used() == 16,
		"upload staging remained charged after the final prepared-image alias retired");
	jpegview_linux::CacheAdmissionPolicy sharedAdmission(*shared);
	auto textureReservation = sharedAdmission.Reserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
		jpegview_linux::CacheProtectionTier::Active, {});
	Expect(textureReservation && shared->Used() == 32,
		"prepared-cache retirement did not make shared capacity available to textures");
	textureReservation.Reset();
	Expect(shared->Used() == 16,
		"texture reservation rollback failed to return shared capacity");
	decodedCache.Clear();
	const auto decodedRetiredDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (shared->Used() != 0 && std::chrono::steady_clock::now() < decodedRetiredDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(shared->Used() == 0,
		"decoded eviction did not return its shared cache charge after retirement");
}

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
struct CacheBudgetRaceProbe {
	std::mutex mutex;
	std::condition_variable changed;
	bool resetUnlocked = false;
	bool chargePromoted = false;
	bool allowResetDrop = false;
	bool resetCompleted = false;
	bool allowReserveReturn = false;
	bool reserveCompleted = false;
	bool rejected = false;
	bool hookTimedOut = false;
};

void CacheBudgetRaceTestHook(jpegview_linux::detail::CacheBudgetTestHookPoint point,
	void* context) {
	auto& probe = *static_cast<CacheBudgetRaceProbe*>(context);
	std::unique_lock<std::mutex> lock(probe.mutex);
	if (point == jpegview_linux::detail::CacheBudgetTestHookPoint::ReservationResetUnlocked) {
		probe.resetUnlocked = true;
		probe.changed.notify_all();
		if (!probe.changed.wait_for(lock, std::chrono::seconds(2), [&probe] {
			return probe.allowResetDrop;
		})) probe.hookTimedOut = true;
		return;
	}
	probe.chargePromoted = true;
	probe.changed.notify_all();
	if (!probe.changed.wait_for(lock, std::chrono::seconds(2), [&probe] {
		return probe.allowReserveReturn;
	})) probe.hookTimedOut = true;
}

bool WaitForCacheBudgetRaceFlag(CacheBudgetRaceProbe& probe, bool CacheBudgetRaceProbe::*flag) {
	std::unique_lock<std::mutex> lock(probe.mutex);
	return probe.changed.wait_for(lock, std::chrono::seconds(2), [&probe, flag] {
		return probe.*flag;
	});
}

void SetCacheBudgetRaceFlag(CacheBudgetRaceProbe& probe,
	bool CacheBudgetRaceProbe::*flag) {
	{
		std::lock_guard<std::mutex> lock(probe.mutex);
		probe.*flag = true;
	}
	probe.changed.notify_all();
}

int RunCacheBudgetReservationRejectionRaceChild(bool mismatch) {
	jpegview_linux::SharedCacheBudget budget(0);
	std::shared_ptr<const void> allocationOwner = std::make_shared<int>(42);
	auto reservation = budget.TrackTemporary(16,
		jpegview_linux::CacheMemoryCategory::ActiveWorkingData, allocationOwner);
	if (!reservation || budget.Snapshot().activeWorkingBytes != 16) return 1;

	CacheBudgetRaceProbe probe;
	budget.SetTestHookForTesting(CacheBudgetRaceTestHook, &probe);
	std::thread resetThread([reservation = std::move(reservation), &probe]() mutable {
		reservation.Reset();
		{
			std::lock_guard<std::mutex> lock(probe.mutex);
			probe.resetCompleted = true;
		}
		probe.changed.notify_all();
	});
	if (!WaitForCacheBudgetRaceFlag(probe, &CacheBudgetRaceProbe::resetUnlocked)) {
		::_exit(11);
	}

	std::thread reserveThread([&budget, &allocationOwner, &probe, mismatch] {
		const auto rejected = !budget.TryReserve(mismatch ? 17 : 16,
			jpegview_linux::CacheMemoryCategory::RetainedDecodedPixels, allocationOwner);
		{
			std::lock_guard<std::mutex> lock(probe.mutex);
			probe.rejected = static_cast<bool>(rejected);
			probe.reserveCompleted = true;
		}
		probe.changed.notify_all();
	});
	if (!WaitForCacheBudgetRaceFlag(probe, &CacheBudgetRaceProbe::chargePromoted)) {
		::_exit(12);
	}

	SetCacheBudgetRaceFlag(probe, &CacheBudgetRaceProbe::allowResetDrop);
	if (!WaitForCacheBudgetRaceFlag(probe, &CacheBudgetRaceProbe::resetCompleted)) {
		::_exit(13);
	}
	SetCacheBudgetRaceFlag(probe, &CacheBudgetRaceProbe::allowReserveReturn);
	if (!WaitForCacheBudgetRaceFlag(probe, &CacheBudgetRaceProbe::reserveCompleted)) {
		// The parent process enforces a deadline and can terminate this child if
		// the rejected final charge recursively locks the budget mutex here.
		::_exit(14);
	}
	resetThread.join();
	reserveThread.join();

	bool rejected = false;
	bool hookTimedOut = false;
	{
		std::lock_guard<std::mutex> lock(probe.mutex);
		rejected = probe.rejected;
		hookTimedOut = probe.hookTimedOut;
	}
	const jpegview_linux::CacheBudgetSnapshot snapshot = budget.Snapshot();
	budget.SetTestHookForTesting(nullptr, nullptr);
	return rejected && !hookTimedOut && snapshot.capacityBytes == 0 &&
		snapshot.retainedBytes == 0 && snapshot.decodedPixelBytes == 0 &&
		snapshot.preparedFrameBytes == 0 && snapshot.imageTextureBytes == 0 &&
		snapshot.uploadStagingBytes == 0 && snapshot.activeWorkingBytes == 0 &&
		snapshot.releaseRevision == 1 && snapshot.retainedCapacityRevision == 0 ? 0 : 15;
}

bool RunCacheBudgetReservationRejectionRace(const std::string& executable,
	const char* variant) {
	std::string executableArgument = executable;
	std::string modeArgument = "--cache-budget-reservation-rejection-race";
	std::string variantArgument = variant;
	char* arguments[] = {
		executableArgument.data(), modeArgument.data(), variantArgument.data(), nullptr};
	pid_t child = 0;
	if (::posix_spawnp(&child, executableArgument.c_str(), nullptr, nullptr,
		arguments, ::environ) != 0) return false;

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	int status = 0;
	while (std::chrono::steady_clock::now() < deadline) {
		const pid_t waited = ::waitpid(child, &status, WNOHANG);
		if (waited == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
		if (waited < 0 && errno != EINTR) return false;
		std::this_thread::yield();
	}
	(void)::kill(child, SIGKILL);
	while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	return false;
}

void TestSharedCacheBudgetReservationRejectionRaces() {
	Expect(RunCacheBudgetReservationRejectionRace(gTestExecutablePath, "matching"),
		"matching-byte rejection racing final Reset deadlocked or corrupted budget accounting");
	Expect(RunCacheBudgetReservationRejectionRace(gTestExecutablePath, "mismatch"),
		"mismatch rejection racing final Reset deadlocked or corrupted budget accounting");
}
#endif

void TestActiveDecodedPixelsUseWorkingCapacityAndDemoteSafely() {
	TemporaryDirectory temporary;
	const fs::path activeFile = temporary.path() / "active-source.png";
	WriteText(activeFile, "active source");
	const jpegview_linux::SourceKey source =
		jpegview_linux::DescribeImageSource(activeFile).Key();
	auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(64);
	jpegview_linux::DecodedImageCache cache(64, {}, budget);
	auto decoded = CachedTestImage(32);
	cache.Store(activeFile, decoded);
	decoded.reset();
	auto activeHandle = cache.Find(source.logicalPath);
	cache.PromoteToActiveUse(source);
	Expect(activeHandle && budget->Snapshot().retainedBytes == 0 &&
		budget->Snapshot().activeWorkingBytes == 32,
		"selected decoded pixels still consumed retained texture capacity");
	auto spreadTextureBytes = budget->TryReserve(64,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures);
	Expect(spreadTextureBytes && budget->Used() == 64,
		"active decoded pixels prevented retained textures from using the configured budget");
	cache.SetProtectionSnapshot({
		{source, jpegview_linux::CacheProtectionTier::Neighbor}});
	Expect(cache.CachedImages() == 0 && budget->Used() == 64 &&
		budget->Snapshot().activeWorkingBytes == 32,
		"a selected source was dropped or under-accounted when retained capacity was full");
	activeHandle.reset();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (budget->Snapshot().activeWorkingBytes != 0 &&
		std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	Expect(budget->Snapshot().activeWorkingBytes == 0 && budget->Used() == 64,
		"retired active decoded pixels kept working accounting after their final owner left");
	spreadTextureBytes.Reset();
}

void TestRetainedCapacityRevisionIgnoresTemporaryRetirement() {
	auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(32);
	auto heldOwner = std::make_shared<int>(1);
	auto otherOwner = std::make_shared<int>(2);
	std::shared_ptr<const void> heldAllocation(heldOwner, heldOwner.get());
	std::shared_ptr<const void> otherAllocation(otherOwner, otherOwner.get());
	auto heldVictim = budget->TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames, heldAllocation);
	auto secondVictim = budget->TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, otherAllocation);
	jpegview_linux::CacheAdmissionPolicy admission(*budget);
	std::size_t evictions = 0;
	const std::uint64_t beforeEviction = budget->RetainedCapacityRevision();
	auto firstAttempt = admission.Reserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
		jpegview_linux::CacheProtectionTier::Active,
		[&](jpegview_linux::CacheProtectionTier) {
			++evictions;
			budget->RetireAllocation(heldAllocation, std::move(heldVictim));
			return true;
		});
	Expect(!firstAttempt && evictions == 1 &&
		budget->RetainedCapacityRevision() == beforeEviction && budget->Used() == 32,
		"an aliased retained victim was treated as returned capacity before retirement");
	const std::uint64_t generalReleaseBefore = budget->ReleaseRevision();
	auto temporaryOwner = std::make_shared<int>(3);
	std::shared_ptr<const void> temporaryAllocation(temporaryOwner,
		temporaryOwner.get());
	auto unrelatedTemporary = budget->TrackTemporary(8,
		jpegview_linux::CacheMemoryCategory::ActiveWorkingData, temporaryAllocation);
	unrelatedTemporary.Reset();
	Expect(budget->ReleaseRevision() > generalReleaseBefore &&
		budget->RetainedCapacityRevision() == beforeEviction && budget->Used() == 32,
		"unrelated working-data retirement changed retained-capacity revision");
	const bool mayEvictAgain = budget->RetainedCapacityRevision() > beforeEviction;
	const std::size_t evictionsBeforeRetry = evictions;
	auto secondAttempt = admission.Reserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
		jpegview_linux::CacheProtectionTier::Active,
		[&](jpegview_linux::CacheProtectionTier) {
			++evictions;
			return false;
		}, mayEvictAgain);
	Expect(!secondAttempt && evictions == evictionsBeforeRetry,
		"temporary charge retirement triggered another retained-cache eviction");

	heldAllocation.reset();
	heldOwner.reset();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (budget->RetainedCapacityRevision() == beforeEviction &&
		std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	Expect(budget->RetainedCapacityRevision() > beforeEviction && budget->Used() == 16,
		"retained capacity did not advance when the held victim finally retired");
	const std::uint64_t returnedCapacity = budget->RetainedCapacityRevision();
	auto afterRetirement = admission.Reserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
		jpegview_linux::CacheProtectionTier::Active, {},
		budget->RetainedCapacityRevision() > returnedCapacity);
	Expect(afterRetirement && budget->Used() == 32,
		"texture admission did not proceed after retained capacity actually returned");
	afterRetirement.Reset();
	secondVictim.Reset();
}

void TestCachePolicyCrossOwnerProtectionAndAliasLifetime() {
	TemporaryDirectory temporary;
	auto aliasBudget = std::make_shared<jpegview_linux::SharedCacheBudget>(32);
	jpegview_linux::DecodedImageCache aliasCache(64, {}, aliasBudget);
	const fs::path aliasFileA = temporary.path() / "alias-a.jpg";
	const fs::path aliasFileB = temporary.path() / "alias-b.jpg";
	WriteText(aliasFileA, "alias-a");
	WriteText(aliasFileB, "alias-b");
	auto sharedPixels = CachedTestImage(16);
	aliasCache.Store(aliasFileA, sharedPixels);
	aliasCache.Store(aliasFileB, sharedPixels);
	Expect(aliasCache.CachedBytes() == 32 && aliasBudget->Used() == 16 &&
		aliasBudget->Snapshot().decodedPixelBytes == 16,
		"multiple cache keys charged one shared BGRA allocation more than once");
	aliasCache.Clear();
	Expect(aliasBudget->Used() == 16 && aliasCache.CachedBytes() == 0 &&
		aliasBudget->Snapshot().decodedPixelBytes == 16,
		"cleared cache entries stopped accounting while a shared pixel alias remained");
	sharedPixels.reset();
	const auto aliasDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (aliasBudget->Used() != 0 && std::chrono::steady_clock::now() < aliasDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(aliasBudget->Used() == 0,
		"shared allocation remained charged after the final image alias retired");

	const fs::path activeFile = temporary.path() / "active.jpg";
	const fs::path neighborFile = temporary.path() / "active-neighbor.jpg";
	WriteText(activeFile, "active");
	WriteText(neighborFile, "active-neighbor");
	jpegview_linux::DecodedImageCache pinned(64);
	pinned.Store(activeFile, CachedTestImage(16));
	pinned.Store(neighborFile, CachedTestImage(16));
	pinned.SetProtectionSnapshot({
		{jpegview_linux::DescribeImageSource(activeFile).Key(),
			jpegview_linux::CacheProtectionTier::Active},
		{jpegview_linux::DescribeImageSource(neighborFile).Key(),
			jpegview_linux::CacheProtectionTier::Neighbor}});
	Expect(pinned.EvictLeastRecentlyUsed(
		jpegview_linux::CacheProtectionTier::DistantSpeculation) == 0 &&
		pinned.Find(activeFile) != nullptr && pinned.Find(neighborFile) != nullptr,
		"distant eviction reclaimed a pinned active image or immediate neighbor");
	Expect(pinned.EvictLeastRecentlyUsed(jpegview_linux::CacheProtectionTier::Neighbor) == 16 &&
		pinned.Find(activeFile) != nullptr && pinned.Find(neighborFile) == nullptr,
		"neighbor-tier eviction did not preserve the active image");

	auto shared = std::make_shared<jpegview_linux::SharedCacheBudget>(32);
	const fs::path distantFile = temporary.path() / "distant.jpg";
	const fs::path neighborPreparedFile = temporary.path() / "neighbor-prepared.jpg";
	WriteText(distantFile, "distant");
	WriteText(neighborPreparedFile, "neighbor-prepared");
	jpegview_linux::DecodedImageCache decoded(64, {}, shared);
	decoded.Store(distantFile, CachedTestImage(16));
	decoded.SetProtectionSnapshot({
		{jpegview_linux::DescribeImageSource(distantFile).Key(),
			jpegview_linux::CacheProtectionTier::DistantSpeculation}});
	jpegview_linux::DisplayImageCache prepared(64, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
			return DisplayCachePreparedTestImage(request);
		}, shared);
	const auto neighborRequest = jpegview_linux::MakeDisplayImageRequest(
		neighborPreparedFile, DisplayCacheTestImage(2, 2), 0, 2, 2, false, 1);
	prepared.Request(neighborRequest);
	Expect(prepared.WaitUntilIdle(std::chrono::seconds(2)),
		"cross-owner prepared frame did not complete");
	auto completion = prepared.TakeCompleted(1);
	Expect(completion.size() == 1 && shared->Used() == 32 &&
		prepared.CachedBytes() == 16,
		"decoded and prepared caches did not compete for their shared budget");
	completion.clear();
	prepared.SetProtectionSnapshot({
		{neighborRequest.cacheKey, jpegview_linux::CacheProtectionTier::Neighbor}});
	const fs::path activePressureFile = temporary.path() / "active-under-pressure.jpg";
	WriteText(activePressureFile, "active-under-pressure");
	jpegview_linux::DisplayImageCache active(64, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
			return DisplayCachePreparedTestImage(request);
		}, shared);
	auto activeRequest = jpegview_linux::MakeDisplayImageRequest(
		activePressureFile, DisplayCacheTestImage(2, 2), 0, 2, 2, false, 0);
	activeRequest.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
	auto activePixels = active.RequestAndWait(activeRequest);
	Expect(activePixels && activePixels->bgra.size() == 16 && active.CachedBytes() == 0 &&
		shared->Used() == 32 && shared->Snapshot().activeWorkingBytes == 16,
		"worker admission evicted another owner or lost the current frame under shared pressure");
	activePixels.reset();
	active.Clear();
	const auto activeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (shared->Snapshot().activeWorkingBytes != 0 &&
		std::chrono::steady_clock::now() < activeDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(shared->Snapshot().activeWorkingBytes == 0,
		"active working pixels remained accounted after their final owner retired");
	jpegview_linux::CacheAdmissionPolicy policy(*shared);
	auto externalDistant = decoded.Find(distantFile);
	Expect(externalDistant != nullptr, "external-alias admission victim was not retained");
	std::vector<jpegview_linux::CacheProtectionTier> evictionOrder;
	const auto admissionStart = std::chrono::steady_clock::now();
	auto deferredTexture = policy.Reserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
		jpegview_linux::CacheProtectionTier::Active,
		[&](jpegview_linux::CacheProtectionTier tier) {
			evictionOrder.push_back(tier);
			return decoded.EvictLeastRecentlyUsed(tier) != 0;
		});
	const auto admissionElapsed = std::chrono::steady_clock::now() - admissionStart;
	Expect(!deferredTexture && admissionElapsed < std::chrono::milliseconds(750) &&
		evictionOrder == std::vector<jpegview_linux::CacheProtectionTier>{
			jpegview_linux::CacheProtectionTier::DistantSpeculation} &&
		decoded.CachedBytes() == 0 && prepared.CachedBytes() == 16 &&
		shared->Used() == 32,
		"aliased-victim admission blocked or continued evicting before capacity returned");
	externalDistant.reset();
	const auto distantReleasedDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (shared->Used() > 16 && std::chrono::steady_clock::now() < distantReleasedDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(shared->Used() == 16,
		"asynchronous retirement did not return the aliased victim reservation");
	evictionOrder.clear();
	auto texture = policy.Reserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
		jpegview_linux::CacheProtectionTier::Active,
		[&](jpegview_linux::CacheProtectionTier tier) {
			evictionOrder.push_back(tier);
			return false;
		});
	Expect(texture && evictionOrder.empty() && shared->Used() == 32 &&
		prepared.CachedBytes() == 16,
		"deferred admission did not retry after its external alias released capacity");
	texture.Reset();
	Expect(shared->Used() == 16,
		"cross-owner texture admission rollback leaked retained bytes");
	prepared.Clear();
	const auto preparedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (shared->Used() != 0 && std::chrono::steady_clock::now() < preparedDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(shared->Used() == 0,
		"prepared owner did not release its reservation after final retirement");
}

void TestCacheAdmissionPriorityAndMixedProtectionSnapshots() {
	TemporaryDirectory temporary;
	const fs::path jpegFile = temporary.path() / "nearest.jpg";
	const fs::path pngFile = temporary.path() / "retained-neighbor.png";
	const fs::path webpFile = temporary.path() / "distant.webp";
	WriteText(jpegFile, "jpeg");
	WriteText(pngFile, "png");
	WriteText(webpFile, "webp");
	const auto decoded = DisplayCacheTestImage(2, 2);
	auto jpegRequest = jpegview_linux::MakeDisplayImageRequest(
		jpegFile, decoded, 0, 2, 2, false, 1);
	auto pngRequest = jpegview_linux::MakeDisplayImageRequest(
		pngFile, decoded, 0, 2, 2, false, 2);
	auto webpRequest = jpegview_linux::MakeDisplayImageRequest(
		webpFile, decoded, 0, 2, 2, false, 3);
	jpegRequest.workClass = pngRequest.workClass = webpRequest.workClass =
		jpegview_linux::PerfWorkClass::ActiveImageSpread;
	jpegview_linux::CacheProtectionSnapshot snapshot;
	jpegview_linux::AddDecodedProtection(snapshot, jpegRequest.source.Key(),
		jpegview_linux::CacheProtectionTier::Neighbor);
	jpegview_linux::AddDecodedProtection(snapshot, jpegRequest.source.Key(),
		jpegview_linux::CacheProtectionTier::DistantSpeculation);
	jpegview_linux::AddDecodedProtection(snapshot, pngRequest.source.Key(),
		jpegview_linux::CacheProtectionTier::Neighbor);
	jpegview_linux::AddDecodedProtection(snapshot, webpRequest.source.Key(),
		jpegview_linux::CacheProtectionTier::DistantSpeculation);
	jpegview_linux::AddPreparedProtection(snapshot, jpegRequest.cacheKey,
		jpegview_linux::CacheProtectionTier::Neighbor);
	jpegview_linux::AddPreparedProtection(snapshot, jpegRequest.cacheKey,
		jpegview_linux::CacheProtectionTier::DistantSpeculation);
	// This key is already retained, so it is absent from the incoming work list.
	// It must still remain a nearest neighbor in the protection snapshot.
	jpegview_linux::AddPreparedProtection(snapshot, pngRequest.cacheKey,
		jpegview_linux::CacheProtectionTier::Neighbor);
	jpegview_linux::AddPreparedProtection(snapshot, webpRequest.cacheKey,
		jpegview_linux::CacheProtectionTier::DistantSpeculation);
	Expect(snapshot.decoded.size() == 3 && snapshot.prepared.size() == 3 &&
		std::find_if(snapshot.decoded.begin(), snapshot.decoded.end(),
			[&](const auto& item) { return item.first == jpegRequest.source.Key(); })->second ==
			jpegview_linux::CacheProtectionTier::Neighbor &&
		std::find_if(snapshot.prepared.begin(), snapshot.prepared.end(),
			[&](const auto& item) { return item.first == pngRequest.cacheKey; })->second ==
			jpegview_linux::CacheProtectionTier::Neighbor,
		"duplicate protection input demoted a neighbor or omitted the retained candidate");

	jpegview_linux::DecodedImageCache decodedCache(192);
	decodedCache.Store(jpegFile, DisplayCacheTestImage(4, 4));
	decodedCache.Store(pngFile, DisplayCacheTestImage(4, 4));
	decodedCache.Store(webpFile, DisplayCacheTestImage(4, 4));
	decodedCache.SetProtectionSnapshot(snapshot.decoded);
	Expect(decodedCache.EvictLeastRecentlyUsed(
		jpegview_linux::CacheProtectionTier::DistantSpeculation) == 64 &&
		decodedCache.Find(jpegFile) && decodedCache.Find(pngFile) &&
		!decodedCache.Find(webpFile),
		"mixed-format decoded snapshot did not retain immediate neighbors first");

	jpegview_linux::DisplayImageCache preparedCache(64, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
			return DisplayCachePreparedTestImage(request);
		});
	preparedCache.RequestBackgroundBatch({jpegRequest, pngRequest, webpRequest});
	Expect(preparedCache.WaitUntilIdle(std::chrono::seconds(2)),
		"mixed-format prepared entries did not complete");
	const auto completions = preparedCache.TakeCompleted(3);
	Expect(completions.size() == 3, "prepared snapshot fixture lost completed entries");
	preparedCache.SetProtectionSnapshot(snapshot.prepared);
	Expect(preparedCache.EvictLeastRecentlyUsed(
		jpegview_linux::CacheProtectionTier::DistantSpeculation) == 16 &&
		preparedCache.Find(jpegRequest) && preparedCache.Find(pngRequest) &&
		!preparedCache.Find(webpRequest),
		"retained-neighbor prepared texture candidate lost protection under distant pressure");
	preparedCache.Clear();
	decodedCache.Clear();

	auto protectedBudget = std::make_shared<jpegview_linux::SharedCacheBudget>(32);
	const fs::path protectedActiveFile = temporary.path() / "protected-active.png";
	const fs::path protectedNeighborFile = temporary.path() / "protected-neighbor.png";
	WriteText(protectedActiveFile, "active");
	WriteText(protectedNeighborFile, "neighbor");
	jpegview_linux::DecodedImageCache protectedEntries(32, {}, protectedBudget);
	protectedEntries.Store(protectedActiveFile, CachedTestImage(16));
	protectedEntries.Store(protectedNeighborFile, CachedTestImage(16));
	protectedEntries.SetProtectionSnapshot({
		{jpegview_linux::DescribeImageSource(protectedActiveFile).Key(),
			jpegview_linux::CacheProtectionTier::Active},
		{jpegview_linux::DescribeImageSource(protectedNeighborFile).Key(),
			jpegview_linux::CacheProtectionTier::Neighbor}});
	jpegview_linux::CacheAdmissionPolicy protectedAdmission(*protectedBudget);
	std::vector<jpegview_linux::CacheProtectionTier> distantTextureEvictions;
	auto distantTexture = protectedAdmission.Reserve(24,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
		jpegview_linux::CacheProtectionTier::DistantSpeculation,
		[&](jpegview_linux::CacheProtectionTier tier) {
			distantTextureEvictions.push_back(tier);
			return protectedEntries.EvictLeastRecentlyUsed(tier) != 0;
		});
	const jpegview_linux::CacheBudgetSnapshot protectedSnapshot =
		protectedBudget->Snapshot();
	Expect(!distantTexture && distantTextureEvictions.empty() &&
		protectedEntries.Find(protectedActiveFile) && protectedEntries.Find(protectedNeighborFile) &&
		protectedSnapshot.retainedBytes == 16 &&
		protectedSnapshot.activeWorkingBytes == 16,
		"distant texture admission evicted a retained neighbor or kept active decoded pixels retained");
	protectedEntries.Clear();

	jpegview_linux::SharedCacheBudget priorityBudget(32);
	auto activeOwner = std::make_shared<int>(1);
	auto distantOwner = std::make_shared<int>(2);
	std::shared_ptr<const void> activeAllocation(activeOwner, activeOwner.get());
	std::shared_ptr<const void> distantAllocation(distantOwner, distantOwner.get());
	auto activeReservation = priorityBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, activeAllocation);
	auto distantReservation = priorityBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames, distantAllocation);
	jpegview_linux::CacheAdmissionPolicy admission(priorityBudget);
	int distantIncomingEvictions = 0;
	auto distantIncoming = admission.Reserve(1,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames, {},
		jpegview_linux::CacheProtectionTier::DistantSpeculation,
		[&](jpegview_linux::CacheProtectionTier) {
			++distantIncomingEvictions;
			return true;
		});
	Expect(!distantIncoming && distantIncomingEvictions == 0 &&
		priorityBudget.Used() == 32,
		"distant incoming work evicted an active or neighbor cache entry");
	std::vector<jpegview_linux::CacheProtectionTier> neighborEvictionOrder;
	auto neighborIncoming = admission.Reserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
		jpegview_linux::CacheProtectionTier::Neighbor,
		[&](jpegview_linux::CacheProtectionTier tier) {
			neighborEvictionOrder.push_back(tier);
			if (tier != jpegview_linux::CacheProtectionTier::DistantSpeculation) return false;
			distantReservation.Reset();
			return true;
		});
	Expect(neighborIncoming && neighborEvictionOrder == std::vector<
		jpegview_linux::CacheProtectionTier>{jpegview_linux::CacheProtectionTier::DistantSpeculation} &&
		priorityBudget.Used() == 32,
		"neighbor admission evicted an active texture before distant cache data");
	activeReservation.Reset();
	neighborIncoming.Reset();
	distantAllocation.reset();
	activeAllocation.reset();

	jpegview_linux::SharedCacheBudget activePriorityBudget(16);
	auto neighborReservation = activePriorityBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures);
	jpegview_linux::CacheAdmissionPolicy activeAdmission(activePriorityBudget);
	std::vector<jpegview_linux::CacheProtectionTier> activeEvictionOrder;
	auto activeIncoming = activeAdmission.Reserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedPreparedFrames, {},
		jpegview_linux::CacheProtectionTier::Active,
		[&](jpegview_linux::CacheProtectionTier tier) {
			activeEvictionOrder.push_back(tier);
			if (tier != jpegview_linux::CacheProtectionTier::Neighbor) return false;
			neighborReservation.Reset();
			return true;
		});
	Expect(activeIncoming && activeEvictionOrder == std::vector<
		jpegview_linux::CacheProtectionTier>{
			jpegview_linux::CacheProtectionTier::DistantSpeculation,
			jpegview_linux::CacheProtectionTier::Neighbor},
		"active admission did not progress from distant to neighbor protection");
}

void TestFocusedPreviewProtectionUnderPressure() {
	TemporaryDirectory temporary;
	const fs::path currentFile = temporary.path() / "current.png";
	const fs::path spreadFile = temporary.path() / "spread.png";
	const fs::path nearestFile = temporary.path() / "nearest.png";
	const fs::path distantFile = temporary.path() / "distant.png";
	for (const fs::path& filename : {currentFile, spreadFile, nearestFile, distantFile}) {
		WriteText(filename, filename.filename().string());
	}
	const auto decoded = DisplayCacheTestImage(4, 4);
	const auto makeRequest = [&](const fs::path& filename, int width, int height,
		jpegview_linux::PerfWorkClass workClass) {
		auto request = jpegview_linux::MakeDisplayImageRequest(
			filename, decoded, 0, width, height, false);
		request.workClass = workClass;
		return request;
	};
	auto current = makeRequest(currentFile, 2, 2,
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	auto spread = makeRequest(spreadFile, 2, 2,
		jpegview_linux::PerfWorkClass::ActiveImageSpread);
	auto nearest = makeRequest(nearestFile, 2, 2,
		jpegview_linux::PerfWorkClass::NearestNavigationNeighbor);
	auto distant = makeRequest(distantFile, 2, 2,
		jpegview_linux::PerfWorkClass::DistantSpeculation);
	auto lens = makeRequest(currentFile, 3, 2,
		jpegview_linux::PerfWorkClass::FocusedPreview);
	jpegview_linux::CacheProtectionSnapshot snapshot;
	jpegview_linux::AddDisplayRequestProtection(snapshot, current,
		jpegview_linux::CacheProtectionTier::Active);
	jpegview_linux::AddDisplayRequestProtection(snapshot, spread,
		jpegview_linux::CacheProtectionTier::Active);
	jpegview_linux::AddDisplayRequestProtection(snapshot, nearest,
		jpegview_linux::CacheProtectionTier::Neighbor);
	jpegview_linux::AddDisplayRequestProtection(snapshot, distant,
		jpegview_linux::CacheProtectionTier::DistantSpeculation);
	jpegview_linux::AddMagnifyingGlassProtection(snapshot, lens);
	const std::size_t preparedCountWithLens = snapshot.prepared.size();
	const std::size_t textureCountWithLens = snapshot.textures.size();
	jpegview_linux::CacheProtectionSnapshot clearedSnapshot;
	jpegview_linux::AddMagnifyingGlassProtection(clearedSnapshot, std::nullopt);
	auto payloadReleasedSpread = spread;
	payloadReleasedSpread.decoded.reset();
	jpegview_linux::CacheProtectionSnapshot payloadReleasedSnapshot;
	jpegview_linux::AddDisplayRequestProtection(payloadReleasedSnapshot,
		payloadReleasedSpread, jpegview_linux::CacheProtectionTier::Active);
	const auto tierFor = [](const auto& entries, const auto& key)
		-> std::optional<jpegview_linux::CacheProtectionTier> {
		const auto found = std::find_if(entries.begin(), entries.end(),
			[&key](const auto& entry) { return entry.first == key; });
		if (found == entries.end()) return std::nullopt;
		return found->second;
	};
	Expect(jpegview_linux::CacheProtectionForWorkClass(
		jpegview_linux::PerfWorkClass::FocusedPreview) ==
			jpegview_linux::CacheProtectionTier::Neighbor &&
		jpegview_linux::CacheProtectionForWorkClass(
			jpegview_linux::PerfWorkClass::ActiveImageSpread) ==
			jpegview_linux::CacheProtectionTier::Active &&
		tierFor(snapshot.decoded, current.source.Key()) ==
			jpegview_linux::CacheProtectionTier::Active &&
		tierFor(snapshot.prepared, lens.cacheKey) ==
			jpegview_linux::CacheProtectionTier::Neighbor &&
		tierFor(snapshot.prepared, current.cacheKey) ==
			jpegview_linux::CacheProtectionTier::Active &&
		tierFor(snapshot.prepared, nearest.cacheKey) ==
			jpegview_linux::CacheProtectionTier::Neighbor &&
		tierFor(snapshot.prepared, distant.cacheKey) ==
			jpegview_linux::CacheProtectionTier::DistantSpeculation &&
		tierFor(snapshot.textures, lens.key) ==
			jpegview_linux::CacheProtectionTier::Neighbor &&
		preparedCountWithLens == 5 && textureCountWithLens == 5 &&
		clearedSnapshot.decoded.empty() && clearedSnapshot.prepared.empty() &&
		clearedSnapshot.textures.empty() &&
		tierFor(payloadReleasedSnapshot.prepared, spread.cacheKey) ==
			jpegview_linux::CacheProtectionTier::Active &&
		tierFor(payloadReleasedSnapshot.textures, spread.key) ==
			jpegview_linux::CacheProtectionTier::Active,
		"focused-preview snapshot did not retain its texture at neighbor priority or preserve stronger owners");

	jpegview_linux::DisplayImageCache cache(80, 1,
		[](const jpegview_linux::DisplayImageRequest& request) {
			return DisplayCachePreparedTestImage(request);
		});
	cache.RequestBackgroundBatch({current, spread, nearest, distant});
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)) && cache.CachedBytes() == 64,
		"cache pressure fixture did not retain the active, spread, nearest, and distant frames");
	const auto initialCompletions = cache.TakeCompletedWithMetadata(4);
	Expect(initialCompletions.size() == 4,
		"cache pressure fixture did not consume all initial prepared completions");
	cache.SetProtectionSnapshot(snapshot.prepared);
	cache.RequestBackground(lens);
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)) &&
		cache.Find(lens) != nullptr && cache.Find(current) != nullptr &&
		cache.Find(spread) != nullptr && cache.Find(nearest) != nullptr &&
		cache.Find(distant) == nullptr && cache.CachedBytes() == 72,
		"focused preview admission did not evict distant work while preserving active and nearest frames");

	jpegview_linux::SharedCacheBudget textureBudget(48);
	auto activeOwner = std::make_shared<int>(1);
	auto nearestOwner = std::make_shared<int>(2);
	auto distantOwner = std::make_shared<int>(3);
	std::shared_ptr<const void> activeAllocation(activeOwner, activeOwner.get());
	std::shared_ptr<const void> nearestAllocation(nearestOwner, nearestOwner.get());
	std::shared_ptr<const void> distantAllocation(distantOwner, distantOwner.get());
	auto activeTexture = textureBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, activeAllocation);
	auto nearestTexture = textureBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, nearestAllocation);
	auto distantTexture = textureBudget.TryReserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, distantAllocation);
	jpegview_linux::CacheAdmissionPolicy admission(textureBudget);
	std::vector<jpegview_linux::CacheProtectionTier> evictedTiers;
	auto lensTexture = admission.Reserve(16,
		jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
		jpegview_linux::CacheProtectionForWorkClass(lens.workClass),
		[&](jpegview_linux::CacheProtectionTier tier) {
			evictedTiers.push_back(tier);
			if (tier != jpegview_linux::CacheProtectionTier::DistantSpeculation) return false;
			distantTexture.Reset();
			return true;
		});
	Expect(lensTexture && activeTexture && nearestTexture && !distantTexture &&
		textureBudget.Used() == 48 && evictedTiers == std::vector<
			jpegview_linux::CacheProtectionTier>{
				jpegview_linux::CacheProtectionTier::DistantSpeculation},
		"focused preview texture admission did not evict only distant retained work");
}

void TestDisplayCacheZeroBudgetAndOversizedActiveFrameStayUsable() {
	TemporaryDirectory temporary;
	const auto decoded = DisplayCacheTestImage(2, 2);
	const auto prepare = [](const jpegview_linux::DisplayImageRequest& request) {
		return DisplayCachePreparedTestImage(request);
	};

	const fs::path zeroBudgetFile = temporary.path() / "zero-budget.png";
	WriteText(zeroBudgetFile, "zero budget");
	jpegview_linux::DisplayImageCache zeroBudget(0, 1, prepare);
	const auto zeroBudgetRequest = jpegview_linux::MakeDisplayImageRequest(
		zeroBudgetFile, decoded, 0, 2, 2, false);
	const auto zeroBudgetFrame = zeroBudget.RequestAndWait(zeroBudgetRequest);
	Expect(zeroBudgetFrame && zeroBudgetFrame->bgra.size() == 16 &&
		zeroBudget.CachedBytes() == 0 && zeroBudget.CachedImages() == 0,
		"zero cache budget prevented an active prepared frame from being produced");
	zeroBudget.Retire(zeroBudgetFrame);

	const fs::path oversizedFile = temporary.path() / "oversized-active.png";
	WriteText(oversizedFile, "oversized active");
	jpegview_linux::DisplayImageCache smallBudget(8, 1, prepare);
	const auto oversizedRequest = jpegview_linux::MakeDisplayImageRequest(
		oversizedFile, decoded, 0, 2, 2, false);
	const auto oversizedFrame = smallBudget.RequestAndWait(oversizedRequest);
	Expect(oversizedFrame && oversizedFrame->bgra.size() == 16 &&
		smallBudget.CachedBytes() == 0 && smallBudget.CachedImages() == 0,
		"an active frame larger than retained capacity was not available for display");
	smallBudget.Retire(oversizedFrame);
}
const TestCase kTests[] = {
	{"display-cache-promotion-metadata-reaches-upload-scheduler", &TestDisplayCachePromotionMetadataReachesUploadScheduler},
	{"display-prefetch-retains-only-active-spread-decoded-fallback", &TestDisplayPrefetchDecodedOwnershipPolicy},
	{"work-batch-gate-serializes-deactivate-and-publish", &TestWorkBatchGateSerializesDeactivateAndPublish},
	{"decoded-image-cache-shutdown-rejects-new-work", &TestDecodedImageCacheShutdownRejectsNewWork},
	{"decoded-image-cache-observer-preserves-selected-completion", &TestDecodedImageCacheObserverPreservesSelectedCompletion},
	{"decoded-image-cache-focused-observer-waits-for-queued-active-work", &TestDecodedImageCacheFocusedObserverWaitsForQueuedActiveWork},
	{"decoded-image-cache-retention-denial-keeps-queued-observer", &TestDecodedImageCacheRetentionDenialKeepsQueuedObserver},
	{"decoded-image-cache-observer-receives-structured-failure", &TestDecodedImageCacheObserverReceivesStructuredFailure},
	{"decoded-image-cache-and-background-prefetch-cold-spread-retention", &TestDecodedImageCacheAndBackgroundPrefetch},
	{"decoded-active-spread-budget-pressure-preserves-foreground", &TestDecodedActiveSpreadBudgetPressure},
	{"decoded-active-work-retention-refusal-keeps-fitted-spread-frame", &TestDecodedActiveWorkSurvivesRetentionRefusalForSpreadFrames},
	{"decoded-promoted-spread-budget-pressure-preserves-foreground", &TestDecodedPromotedSpreadBudgetPressure},
	{"decoded-budget-rejection-preserves-queued-active-spread-work", &TestDecodedBudgetRejectionPreservesQueuedActiveSpreadWork},
	{"jpeg-active-spread-dimensions-survive-paused-prefetch", &TestJpegActiveSpreadDimensionsSurvivePause},
	{"svg-source-dimensions-use-decoded-cache-worker", &TestSvgSourceDimensionsUseDecodedCacheWorker},
	{"selected-spread-partner-transfers-blocked-jpeg-dimensions", &TestPromotedActiveSpreadJpegDimensionsKeepCurrentOwner},
	{"active-spread-partner-replacement-cancels-obsolete-source-work", &TestActiveSpreadPartnerReplacementCancelsObsoleteRequests},
	{"active-spread-cancellation-uses-submitted-source-identity", &TestActiveSpreadCancellationUsesSubmittedSourceIdentity},
	{"double-page-disable-cancels-active-and-queued-partner-dimensions", &TestDisablingDoublePageCancelsBlockedAndQueuedPartnerDimensions},
	{"display-cache-foreground-active-classification", &TestDisplayImageCacheForegroundActiveClassification},
	{"display-cache-protection-snapshot-preserves-neighbor-lru", &TestDisplayCacheProtectionSnapshotPreservesNeighborLru},
	{"decoded-cache-protection-snapshot-preserves-lru-across-tier-changes", &TestDecodedCacheProtectionSnapshotPreservesLruAcrossTierChanges},
	{"decoded-cache-active-reservations-reconcile-before-retained-tiers", &TestDecodedCacheProtectionReconcilesActiveReservationsFirst},
	{"display-texture-pin-handoff-preserves-borrowed-working-texture", &TestDisplayTexturePinHandoffPreservesBorrowedWorkingTexture},
	{"display-cache-prefetch-preserves-active-foreground", &TestDisplayImageCachePrefetchPreservesActiveForeground},
	{"display-cache-empty-prefetch-preserves-foreground-and-spread", &TestDisplayImageCacheEmptyPrefetchPreservesForegroundAndSpread},
	{"display-cache-cancelled-spread-stays-cancelled-across-empty-prefetch", &TestDisplayCacheCanceledSpreadStaysCanceledAcrossEmptyPrefetch},
	{"display-cache-promoted-spread-survives-paused-prefetch", &TestDisplayImageCachePromotedSpreadSurvivesPause},
	{"active-spread-decode-demand-gates-background-work", &TestActiveSpreadDecodeDemandGatesBackgroundWork},
	{"active-spread-admission-proceeds-during-foreground-pending", &TestActiveSpreadAdmissionDuringForegroundPending},
	{"display-source-validation-waits-for-source-admission", &TestDisplaySourceValidationWaitsForSourceAdmission},
	{"display-cache-retirement-identity-accounting", &TestDisplayCacheRetirementIdentityAccounting},
	{"display-cache-final-pixels-retirement-thread", &TestDisplayCacheFinalPixelsAreDestroyedByRetirementWorker},
	{"display-completion-batch-retires-unprocessed-pair-results", &TestDisplayCompletionBatchRetiresEveryDeferredPairResultOffCaller},
	{"display-retirement-address-reuse-uses-owner-identity", &TestDisplayRetirementDoesNotDeduplicateReusedAddress},
	{"decoded-retirement-address-reuse-uses-owner-identity", &TestDecodedRetirementDoesNotDeduplicateReusedAddress},
	{"display-image-cache-background-preparation", &TestDisplayImageCacheBackgroundPreparation},
	{"display-completion-queue-backpressure", &TestDisplayCompletionQueueBackpressure},
	{"display-queued-promotion-wakes-backpressured-worker", &TestDisplayQueuedPromotionWakesBackpressuredWorker},
	{"display-completed-promotion-releases-speculative-capacity", &TestDisplayCompletedPromotionReleasesSpeculativeCapacity},
	{"display-inflight-promotion-releases-speculative-capacity", &TestDisplayInFlightPromotionReleasesSpeculativeCapacity},
	{"display-completion-byte-budget-and-foreground-admission", &TestDisplayCompletionByteBudgetAndForegroundAdmission},
	{"capacity-blocked-speculation-does-not-block-ready-frames", &TestCapacityBlockedSpeculationDoesNotBlockCompletedFrames},
	{"display-prefetch-replaces-cached-priorities-and-preserves-foreground", &TestDisplayPrefetchReplacesCachedPrioritiesAndPreservesForeground},
	{"pending-cached-completions-yield-to-startable-cold-work", &TestPendingCachedCompletionsYieldToStartableColdWork},
	{"pending-cached-completions-yield-to-equal-priority-cold-work", &TestPendingCachedCompletionsYieldToEqualPriorityColdWork},
	{"display-oversized-speculation-and-reservation-recovery", &TestDisplayOversizedSpeculationAndReservationRecovery},
	{"display-backpressured-shutdown-retires-without-pumping", &TestDisplayBackpressuredShutdownRetiresWithoutPumping},
	{"queued-decoded-image-retires-off-caller", &TestQueuedDecodedImageRetiresOffCaller},
	{"display-retirement-skips-shared-decoded-owner", &TestDisplayRetirementSkipsSharedDecodedOwner},
	{"cross-cache-decoded-retirement-cancellation-and-eviction", &TestCrossCacheDecodedRetirementCancellationAndEviction},
	{"foreground-completion-keeps-cached-priority-class", &TestForegroundCompletionKeepsCachedPriorityClass},
	{"deferred-display-upload-stages-aliased-prepared-completion", &TestDeferredDisplayUploadStagesAliasedPreparedCompletion},
	{"shared-cache-budget-accounting", &TestSharedCacheBudgetAccounting},
	{"shared-cache-budget-rejection-reset-races", &TestSharedCacheBudgetReservationRejectionRaces},
	{"active-decoded-pixels-use-working-capacity", &TestActiveDecodedPixelsUseWorkingCapacityAndDemoteSafely},
	{"retained-capacity-revision-ignores-temporary-retirement", &TestRetainedCapacityRevisionIgnoresTemporaryRetirement},
	{"cache-policy-cross-owner-protection-alias-lifetime", &TestCachePolicyCrossOwnerProtectionAndAliasLifetime},
	{"cache-admission-priority-and-mixed-protection-snapshots", &TestCacheAdmissionPriorityAndMixedProtectionSnapshots},
	{"focused-preview-protection-under-pressure", &TestFocusedPreviewProtectionUnderPressure},
	{"display-cache-zero-budget-and-oversized-active-frame", &TestDisplayCacheZeroBudgetAndOversizedActiveFrameStayUsable},
};

} // namespace

const TestSuite& GetImageCacheSuite() {
	static const TestSuite suite{"image_cache", kTests, sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}

int RunImageCacheSpecialMode(const std::string& mode) {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	(void)::unsetenv("JPEGVIEW_PERF_TRACE");
	if (mode == "matching") return RunCacheBudgetReservationRejectionRaceChild(false);
	if (mode == "mismatch") return RunCacheBudgetReservationRejectionRaceChild(true);
#else
	(void)mode;
#endif
	return 2;
}
