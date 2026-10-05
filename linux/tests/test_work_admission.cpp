#include "test_harness.h"
#include "test_support.h"

namespace {

void TestInteractionWorkPolicyIdleDeadlineAndCapture() {
	using Clock = std::chrono::steady_clock;
	Clock::time_point now{};
	jpegview_linux::InteractionWorkPolicy policy([&now] { return now; });
	const std::vector<std::size_t> visible = {4, 5, 6};
	const auto initial = policy.Plan(false, visible);
	Expect(!policy.NextIdleDeadline().has_value(),
		"inactive work policy exposed a spurious idle timer");
	Expect(!initial.interactionActive && initial.Allows(jpegview_linux::PerfWorkClass::ActiveImageSpread) &&
		initial.Allows(jpegview_linux::PerfWorkClass::FocusedPreview) &&
		initial.Allows(jpegview_linux::PerfWorkClass::VisibleThumbnail) &&
		initial.Allows(jpegview_linux::PerfWorkClass::NearestNavigationNeighbor) &&
		initial.Allows(jpegview_linux::PerfWorkClass::DistantSpeculation),
		"idle work policy did not allow normal work classes");
	Expect(initial.AllowsThumbnail(5) && !initial.AllowsThumbnail(3),
		"visible-thumbnail permissions did not use the supplied indices");

	policy.NotifyActivity(jpegview_linux::InteractionActivity::Pan);
	Expect(policy.NextIdleDeadline() == now + jpegview_linux::InteractionWorkPolicy::IdleDelay(),
		"pan activity did not expose its quiet-period deadline");
	const auto panning = policy.Plan(false, visible);
	Expect(panning.interactionActive && panning.Allows(jpegview_linux::PerfWorkClass::ActiveImageSpread) &&
		panning.Allows(jpegview_linux::PerfWorkClass::FocusedPreview) &&
		panning.AllowsThumbnail(5) && !panning.AllowsThumbnail(3) &&
		!panning.Allows(jpegview_linux::PerfWorkClass::NearestNavigationNeighbor) &&
		!panning.Allows(jpegview_linux::PerfWorkClass::DistantSpeculation) &&
		panning.cancelQueuedSpeculation && panning.requestActiveSpeculationCancellation &&
		panning.suspendSpeculativeUploads,
		"active panning did not preserve foreground/visible work while suspending speculation");

	// Repeated wheel activity restarts the quiet interval instead of allowing
	// speculation based on the first event in a burst.
	now += std::chrono::milliseconds(200);
	policy.NotifyActivity(jpegview_linux::InteractionActivity::Wheel);
	Expect(policy.NextIdleDeadline() == now + jpegview_linux::InteractionWorkPolicy::IdleDelay(),
		"later interaction did not move the event-loop idle deadline");
	now += std::chrono::milliseconds(200);
	Expect(policy.Plan(false, visible).interactionActive,
		"a later wheel event did not restart the idle deadline");
	now += std::chrono::milliseconds(50);
	const auto resumed = policy.Plan(false, visible);
	Expect(!resumed.interactionActive &&
		resumed.Allows(jpegview_linux::PerfWorkClass::NearestNavigationNeighbor) &&
		resumed.Allows(jpegview_linux::PerfWorkClass::DistantSpeculation),
		"speculation did not resume exactly 250 ms after the last wheel event");
	Expect(!policy.NextIdleDeadline().has_value(),
		"an expired idle deadline would keep the SDL wait loop spinning");

	// Every activity source uses the same deadline, including held navigation.
	const std::array<jpegview_linux::InteractionActivity, 5> activities = {{
		jpegview_linux::InteractionActivity::Zoom,
		jpegview_linux::InteractionActivity::Resize,
		jpegview_linux::InteractionActivity::CropDrag,
		jpegview_linux::InteractionActivity::NavigatorDrag,
		jpegview_linux::InteractionActivity::HeldNavigation,
	}};
	for (const auto activity : activities) {
		policy.NotifyActivity(activity);
		now += std::chrono::milliseconds(249);
		Expect(policy.Plan(false, visible).interactionActive,
			"continuous interaction resumed speculative work before the idle deadline");
	}
	now += std::chrono::milliseconds(1);
	Expect(!policy.Plan(false, visible).interactionActive,
		"work policy remained active after all interaction input stopped");

	policy.SetCaptureActive(true);
	Expect(!policy.NextIdleDeadline().has_value(),
		"active capture exposed an idle deadline while the pointer was held");
	now += std::chrono::seconds(2);
	Expect(policy.Plan(false, visible).interactionActive,
		"explicit drag capture expired while the pointer was stationary");
	policy.SetCaptureActive(false);
	Expect(policy.NextIdleDeadline() == now + jpegview_linux::InteractionWorkPolicy::IdleDelay(),
		"capture release did not expose a new quiet-period deadline");
	now += std::chrono::milliseconds(249);
	Expect(policy.Plan(false, visible).interactionActive,
		"capture release did not start a fresh idle interval");
	now += std::chrono::milliseconds(1);
	Expect(!policy.Plan(false, visible).interactionActive,
		"capture release did not resume work after the idle interval");

	policy.NotifyActivity(jpegview_linux::InteractionActivity::Pan);
	now += std::chrono::milliseconds(249);
	policy.NotifyActivity(jpegview_linux::InteractionActivity::Zoom);
	now += std::chrono::milliseconds(249);
	Expect(policy.Plan(false, visible).interactionActive,
		"a new gesture did not restart the deadline before the previous deadline");
	now += std::chrono::milliseconds(1);
	Expect(!policy.Plan(false, visible).interactionActive,
		"restarted gesture deadline did not expire at 250 ms");

	const auto foregroundPending = policy.Plan(true, visible);
	Expect(foregroundPending.Allows(jpegview_linux::PerfWorkClass::ActiveImageSpread) &&
		foregroundPending.Allows(jpegview_linux::PerfWorkClass::FocusedPreview) &&
		!foregroundPending.Allows(jpegview_linux::PerfWorkClass::VisibleThumbnail) &&
		!foregroundPending.Allows(jpegview_linux::PerfWorkClass::NearestNavigationNeighbor) &&
		!foregroundPending.Allows(jpegview_linux::PerfWorkClass::DistantSpeculation),
		"pending foreground source work did not suppress background admission");
}

void TestDisplayUploadSchedulerPrioritizesPermittedForeground() {
	using jpegview_linux::DisplayUploadPriority;
	using jpegview_linux::PerfWorkClass;
	const std::vector<DisplayUploadPriority> candidates{
		{PerfWorkClass::NearestNavigationNeighbor, 1}, // blocked pending upload
		{PerfWorkClass::ActiveImageSpread, 1}, // ready partner
		{PerfWorkClass::ActiveImageSpread, 0}, // ready anchor
		{PerfWorkClass::DistantSpeculation, 0},
		{PerfWorkClass::FocusedPreview, 0},
	};
	const std::set<PerfWorkClass> interactionClasses{
		PerfWorkClass::ActiveImageSpread, PerfWorkClass::FocusedPreview};
	const std::vector<std::size_t> firstTick =
		jpegview_linux::PlanDisplayTextureUploads(candidates, interactionClasses, 1);
	Expect(firstTick == std::vector<std::size_t>{2},
		"a lower-priority deferred upload blocked a ready active anchor within the tick limit");
	const std::vector<std::size_t> spreadTick =
		jpegview_linux::PlanDisplayTextureUploads(candidates, interactionClasses, 2);
	Expect(spreadTick == std::vector<std::size_t>({2, 1}),
		"the bounded spread upload plan did not schedule the active anchor and partner first");
	Expect(std::find(spreadTick.begin(), spreadTick.end(), 0) == spreadTick.end() &&
		std::find(spreadTick.begin(), spreadTick.end(), 3) == spreadTick.end(),
		"interaction-time upload planning admitted pending neighbor or distant work");

	const std::vector<std::size_t> oneSpeculativePerOpportunity =
		jpegview_linux::PlanDisplayTextureUploads(candidates,
			{PerfWorkClass::ActiveImageSpread, PerfWorkClass::FocusedPreview,
				PerfWorkClass::NearestNavigationNeighbor, PerfWorkClass::DistantSpeculation},
		4, 1);
	const std::size_t speculativeSelected = static_cast<std::size_t>(std::count_if(
		oneSpeculativePerOpportunity.begin(), oneSpeculativePerOpportunity.end(),
		[&candidates](std::size_t index) {
			return candidates[index].workClass == PerfWorkClass::NearestNavigationNeighbor ||
				candidates[index].workClass == PerfWorkClass::DistantSpeculation;
		}));
	Expect(oneSpeculativePerOpportunity == std::vector<std::size_t>({2, 1, 4, 0}) &&
		speculativeSelected == 1 &&
		candidates[oneSpeculativePerOpportunity.back()].workClass ==
			PerfWorkClass::NearestNavigationNeighbor,
		"renderer maintenance admitted more than one speculative image upload opportunity");
}

void TestDisplayTextureBudgetAndBandedUploadRollback() {
	constexpr std::size_t mebibyte = 1024u * 1024u;
	Expect(jpegview_linux::SpeculativeDisplayTextureBudgetBytes(0) == 0 &&
		jpegview_linux::SpeculativeDisplayTextureBudgetBytes(128 * mebibyte) ==
			64 * mebibyte &&
		jpegview_linux::SpeculativeDisplayTextureBudgetBytes(1024 * mebibyte) ==
			256 * mebibyte &&
		jpegview_linux::SpeculativeDisplayTextureBudgetBytes(4096 * mebibyte) ==
			256 * mebibyte,
		"speculative image textures exceeded half the shared budget or the 256 MiB ceiling");
	Expect(jpegview_linux::CanRetainSpeculativeDisplayTexture(60, 4, 64) &&
		!jpegview_linux::CanRetainSpeculativeDisplayTexture(61, 4, 64) &&
		!jpegview_linux::CanRetainSpeculativeDisplayTexture(
			std::numeric_limits<std::size_t>::max(), 1,
			std::numeric_limits<std::size_t>::max()),
		"speculative texture admission failed at its exact boundary or overflowed");

	const std::size_t largeImageBytes = 4000u * 2500u * 4u;
	jpegview_linux::DisplayTextureUploadPlan bands(4000, 2500, largeImageBytes,
		8 * mebibyte, 4 * mebibyte);
	Expect(bands.Valid() && bands.IsBanded() && !bands.Complete(),
		"large foreground texture did not start as a private incomplete upload");
	int expectedY = 0;
	std::size_t bandCount = 0;
	while (const auto band = bands.CurrentBand()) {
		const std::size_t bandBytes = static_cast<std::size_t>(band->height) * 4000u * 4u;
		Expect(band->y == expectedY && band->height > 0 && bandBytes <= 4 * mebibyte,
			"large texture upload plan produced an overlapping or over-budget band");
		expectedY += band->height;
		++bandCount;
		Expect(bands.MarkCurrentBandUploaded(),
			"texture upload plan rejected a current successful band");
		Expect(bands.Complete() == (expectedY == 2500),
			"texture upload plan published before every band was complete");
	}
	Expect(expectedY == 2500 && bandCount > 1 && bands.Complete(),
		"successful band uploads did not complete the entire source texture");

	jpegview_linux::DisplayTextureUploadPlan failed(4000, 2500, largeImageBytes,
		8 * mebibyte, 4 * mebibyte);
	Expect(failed.MarkCurrentBandUploaded() && failed.CurrentBand().has_value(),
		"upload failure rollback fixture did not advance past its first private band");
	failed.Fail();
	Expect(failed.Failed() && !failed.Complete() && !failed.CurrentBand().has_value() &&
		!failed.MarkCurrentBandUploaded(),
		"failed band left a partially uploaded texture publishable or retryable");

	jpegview_linux::DisplayTextureUploadPlan cancelled(4000, 2500, largeImageBytes,
		8 * mebibyte, 4 * mebibyte);
	Expect(cancelled.MarkCurrentBandUploaded(),
		"cancellation fixture did not advance its private upload");
	cancelled.Cancel();
	Expect(cancelled.Cancelled() && !cancelled.Complete() &&
		!cancelled.CurrentBand().has_value(),
		"canceling a partial texture upload left another band eligible for publication");

	jpegview_linux::DisplayTextureUploadPlan oneShot(640, 480, 640u * 480u * 4u,
		8 * mebibyte, 4 * mebibyte);
	const auto oneShotBand = oneShot.CurrentBand();
	Expect(oneShot.Valid() && !oneShot.IsBanded() &&
		oneShotBand.has_value() && oneShotBand->y == 0 && oneShotBand->height == 480 &&
		oneShot.MarkCurrentBandUploaded() && oneShot.Complete(),
		"small texture no longer uploads in one maintenance opportunity");
	Expect(!jpegview_linux::DisplayTextureUploadPlan(0, 480, 0,
		8 * mebibyte, 4 * mebibyte).Valid() &&
		!jpegview_linux::DisplayTextureUploadPlan(640, 480, 1,
		8 * mebibyte, 4 * mebibyte).Valid(),
		"invalid texture geometry or pixel accounting was accepted for upload");
}

void TestDisplayTextureRetirementMakesProgressUnderSustainedInteraction() {
	constexpr std::size_t maximumPerTick = 1;
	constexpr std::size_t pressureThreshold = 4;
	Expect(jpegview_linux::DisplayTextureRetirementsForTick(0, false,
		maximumPerTick, pressureThreshold) == 0 &&
		jpegview_linux::DisplayTextureRetirementsForTick(3, true,
			maximumPerTick, pressureThreshold) == 0 &&
		jpegview_linux::DisplayTextureRetirementsForTick(4, true,
			maximumPerTick, pressureThreshold) == 1 &&
		jpegview_linux::DisplayTextureRetirementsForTick(20, true,
			maximumPerTick, pressureThreshold) == 1 &&
		jpegview_linux::DisplayTextureRetirementsForTick(20, false,
			maximumPerTick, pressureThreshold) == 1 &&
		jpegview_linux::DisplayTextureRetirementsForTick(20, false,
			0, pressureThreshold) == 0,
		"renderer texture retirement violated its interaction threshold or per-tick bound");

	std::size_t queued = 0;
	std::size_t destroyed = 0;
	std::size_t maximumQueued = 0;
	for (std::size_t navigation = 0; navigation < 100; ++navigation) {
		++queued; // Each successful held-navigation step makes the old texture obsolete.
		maximumQueued = std::max(maximumQueued, queued);
		const std::size_t retire = jpegview_linux::DisplayTextureRetirementsForTick(
			queued, true, maximumPerTick, pressureThreshold);
		queued -= retire;
		destroyed += retire;
	}
	Expect(destroyed == 97 && queued == 3 && maximumQueued == pressureThreshold,
		"obsolete GPU textures accumulated without bound during sustained held navigation");
}

void TestThumbnailPreparationRetryPreservesIdentityAcrossRepeatedFailuresAndRecovery() {
	std::optional<jpegview_linux::ThumbnailPreparationResult> retry;
	jpegview_linux::ThumbnailPreparationResult result;
	result.key = jpegview_linux::SourceKey("/photos/retry-me.jpg");
	result.fileIndex = 27;
	result.catalogRevision = 5;
	result.geometryRevision = 9;
	result.maximumWidth = 164;
	result.maximumHeight = 109;
	auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
	image->key = result.key;
	image->width = 1;
	image->height = 1;
	image->bgra.assign(4, 31);
	result.image = image;

	jpegview_linux::PreserveThumbnailPreparationRetry(retry, result);
	Expect(retry.has_value() && retry->key == "/photos/retry-me.jpg" &&
		retry->fileIndex == 27 && retry->image == image,
		"first thumbnail repository allocation failure did not preserve the completion for retry");
	const jpegview_linux::SourceKey retainedKey = retry->key;
	const auto retainedImage = retry->image;
	// Each self-alias call represents another failed Store attempt on the retry.
	jpegview_linux::PreserveThumbnailPreparationRetry(retry, *retry);
	Expect(retry->key == retainedKey && retry->fileIndex == 27 && retry->image == retainedImage &&
		jpegview_linux::ThumbnailPreparationResultMatches(*retry, 5, 9, 27,
			retainedKey, 164, 109),
		"a repeated thumbnail allocation failure self-moved and invalidated its pending retry");
	jpegview_linux::PreserveThumbnailPreparationRetry(retry, *retry);
	Expect(retry->key == retainedKey && retry->image == retainedImage,
		"a later thumbnail allocation retry corrupted the original request identity");
	jpegview_linux::ThumbnailPixelRepository repository;
	repository.SetGeometry(164, 109);
	Expect(repository.Store(retry->image) ==
		jpegview_linux::ThumbnailPixelStoreOutcome::Stored &&
		repository.Find(retainedKey) == retainedImage,
		"the preserved thumbnail retry could not be retained after allocation recovered");
	retry.reset();
	Expect(!retry && repository.Find(retainedKey) == retainedImage,
		"successful thumbnail retry did not transfer ownership to the pixel repository");
}

void TestFileListDateSortingAndSelectionPreservation() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "images";
	fs::create_directories(directory);
	const fs::path oldFile = directory / "z-old.png";
	const fs::path middleFile = directory / "m-middle.png";
	const fs::path newFile = directory / "a-new.png";
	WriteTinyImage(oldFile);
	WriteTinyImage(middleFile);
	WriteTinyImage(newFile);
	SetModificationTime(oldFile, 10);
	SetModificationTime(middleFile, 20);
	SetModificationTime(newFile, 30);

	FileList files({directory.string()}, FileList::SortMode::LastModificationTime, true, false);
	Expect(FileNames(files) == std::vector<std::string>({"z-old.png", "m-middle.png", "a-new.png"}),
		"modification-date ordering is not based on filesystem modification time");
	FileList directInput({middleFile.string()}, FileList::SortMode::LastModificationTime,
		true, false);
	Expect(directInput.Current() == middleFile,
		"a directly requested image was replaced by a different timestamp-sorted entry");
	FileList::ScanRequest directScanRequest = FileList::InitialScanRequest(
		{middleFile.string()}, FileList::SortMode::LastModificationTime, true, false,
		FileList::NavigationMode::LoopDirectory);
	FileListPreparedScan directScan = FileList::PrepareScan(directScanRequest,
		[] { return true; });
	Expect(directScan.completed && directScan.replacement.Current() == middleFile,
		"the asynchronous initial scan did not preserve its explicitly requested image");
	files.Next();
	Expect(files.Current().filename() == "m-middle.png", "date-sorted navigation selected the wrong image");
	files.SetSorting(FileList::SortMode::FileName, true);
	Expect(files.Current().filename() == "m-middle.png", "changing sort order lost the current image");
	Expect(FileNames(files) == std::vector<std::string>({"a-new.png", "m-middle.png", "z-old.png"}),
		"filename ordering after a sort change is incorrect");
	files.SetSorting(FileList::SortMode::LastModificationTime, false);
	Expect(FileNames(files) == std::vector<std::string>({"a-new.png", "m-middle.png", "z-old.png"}),
		"descending modification-date ordering is incorrect");
}

void TestFileListNaturalNameSortingAndMarkedSelection() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "logical-names";
	fs::create_directories(directory);
	const std::vector<std::string> names = {
		"page10.jpg", "page2.jpg", "page02.jpg", "Page2.jpg", "space name.jpg",
		"line\nbreak.jpg", "z-last.jpg", "\xC3\xA9.jpg",
	};
	for (const std::string& name : names) WriteTinyImage(directory / fs::path(name));

	FileList files({directory.string()}, FileList::SortMode::FileName, true, false);
	const std::vector<std::string> ascending = {
		"\xC3\xA9.jpg", "line\nbreak.jpg", "Page2.jpg", "page2.jpg", "page02.jpg",
		"page10.jpg", "space name.jpg", "z-last.jpg",
	};
	const std::vector<std::string> actual = FileNames(files);
	std::string actualOrder;
	for (const std::string& name : actual) actualOrder += "{" + name + "} ";
	Expect(actual == ascending,
		"natural filename order changed numeric runs, case ties, unusual names, or path ties: " +
		actualOrder);

	const fs::path marked = directory / "page2.jpg";
	const fs::path selected = directory / "z-last.jpg";
	const std::optional<std::size_t> markedPosition = files.IndexOf(marked);
	const std::optional<std::size_t> selectedPosition = files.IndexOf(selected);
	Expect(markedPosition.has_value() && selectedPosition.has_value() &&
		files.Select(*markedPosition) && files.MarkCurrentForToggle() &&
		files.Select(*selectedPosition),
		"could not prepare marked and selected owners before reordering");
	files.SetSorting(FileList::SortMode::FileName, false);
	const std::vector<std::string> descending(ascending.rbegin(), ascending.rend());
	const std::optional<std::size_t> markedIndex = files.MarkedIndex();
	Expect(FileNames(files) == descending && files.Current() == selected &&
		markedIndex.has_value() && files.Files()[*markedIndex] == marked &&
		files.MarkedToggleTarget() == marked,
		"descending sort failed to preserve selected and marked source identities");
}

void TestFileListAsynchronousSortingKeepsLatestSelection() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "async-sort";
	fs::create_directories(directory);
	for (const char* name : {"page10.jpg", "page2.jpg", "page02.jpg", "Page2.jpg"}) {
		WriteTinyImage(directory / name);
	}

	FileList files({directory.string()}, FileList::SortMode::FileName, true, false);
	const std::vector<fs::path> oldOrder = files.Files();
	const fs::path markedPath = directory / "page10.jpg";
	const fs::path selectedPath = directory / "page2.jpg";
	const auto markedIndex = files.IndexOf(markedPath);
	Expect(markedIndex && files.Select(*markedIndex) && files.MarkCurrentForToggle(),
		"could not mark a source before asynchronous sorting");
	const jpegview_linux::FileListSortRequest request = files.MakeSortRequest(
		FileList::SortMode::FileName, false);
	jpegview_linux::ThumbnailCatalogRevisionTracker thumbnailCatalog;
	const auto selectedIndex = files.IndexOf(selectedPath);
	Expect(selectedIndex && files.Select(*selectedIndex) && files.Files() == oldOrder &&
		files.Current() == selectedPath,
		"pending sorting changed the active order or lost navigation before completion");
	thumbnailCatalog.MarkUpdated(request.expectedRevision,
		request.expectedDescriptorRevision);

	FileListSortWorker worker;
	const std::uint64_t generation = worker.Request(request);
	std::vector<FileListSortResult> results;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < deadline) {
		results = worker.TakeReady();
		if (results.empty()) std::this_thread::yield();
	}
	Expect(results.size() == 1 && results.front().generation == generation &&
		results.front().prepared.completed,
		"sort worker did not publish one complete, generation-matched result");
	FileListPreparedSort& prepared = results.front().prepared;
	Expect(files.Files() == oldOrder && files.Current() == selectedPath,
		"worker sorting mutated the active catalog before result application");
	Expect(files.ApplyPreparedSort(prepared),
		"current asynchronous sort result was unexpectedly rejected");
	Expect(files.MutationRevision() > request.expectedRevision &&
		thumbnailCatalog.NeedsUpdate(files.MutationRevision(), files.DescriptorRevision()),
		"applying a prepared order did not invalidate thumbnail consumers that observed the old order");
	const std::vector<fs::path> expectedDescending(oldOrder.rbegin(), oldOrder.rend());
	const auto newMarkedIndex = files.MarkedIndex();
	Expect(files.Files() == expectedDescending && files.Current() == selectedPath &&
		newMarkedIndex && files.Files()[*newMarkedIndex] == markedPath &&
		files.MarkedToggleTarget() == markedPath,
		"applying the worker result did not preserve the latest selection and marked identity");
	worker.Retire(std::move(prepared.retiredEntries));
	worker.Retire(std::move(prepared.retiredPaths));
	worker.Retire(std::move(prepared.retiredPathIndices));
	worker.Stop();

	FileList staleFiles({directory.string()}, FileList::SortMode::FileName, true, false);
	const fs::path staleSelection = staleFiles.Current();
	jpegview_linux::FileListPreparedSort stale = FileList::PrepareSort(
		staleFiles.MakeSortRequest(FileList::SortMode::FileName, false), [] { return true; });
	staleFiles.SetProvisionalInputs({staleSelection.string()});
	Expect(stale.completed && !staleFiles.ApplyPreparedSort(stale) &&
		staleFiles.Size() == 1 && staleFiles.Current() == staleSelection,
		"stale sort result replaced a newer list generation");
}

void TestFileListDefersMetadataResortToPreparedSort() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "deferred-metadata-sort";
	fs::create_directories(directory);
	const fs::path earlier = directory / "a.jpg";
	const fs::path later = directory / "b.jpg";
	WriteTinyImage(earlier);
	WriteTinyImage(later);
	SetModificationTimeNanoseconds(earlier, 1700000000, 100000000);
	SetModificationTimeNanoseconds(later, 1700000000, 200000000);
	FileList files({directory.string()}, FileList::SortMode::LastModificationTime, true, false);
	Expect(files.Files() == std::vector<fs::path>({earlier, later}),
		"metadata-sort fixture did not begin in modification-time order");
	const jpegview_linux::SourceKey oldKey = files.DescriptorAt(0)->Key();
	SetModificationTimeNanoseconds(earlier, 1700000000, 300000000);
	const jpegview_linux::SourceDescriptor refreshed =
		jpegview_linux::DescribeImageSource(earlier);
	const jpegview_linux::SourceRefreshOutcome outcome =
		jpegview_linux::RefreshFileListSource(files, oldKey, refreshed, true);
	Expect(outcome.applied && outcome.sortKeyChanged && !outcome.orderChanged &&
		files.Files() == std::vector<fs::path>({earlier, later}),
		"deferred descriptor refresh changed active ordering synchronously");
	FileListPreparedSort prepared = FileList::PrepareSort(
		files.MakeSortRequest(files.GetSorting(), files.IsSortedAscending()),
		[] { return true; });
	Expect(prepared.completed && files.ApplyPreparedSort(prepared) &&
		files.Files() == std::vector<fs::path>({later, earlier}) && files.Current() == earlier,
		"prepared metadata resort did not publish the refreshed sort key");
}

void TestFileListAsynchronousSortRejectsStaleDescriptors() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "stale-sort-descriptor";
	fs::create_directories(directory);
	const fs::path first = directory / "a.jpg";
	const fs::path second = directory / "b.jpg";
	WriteTinyImage(first);
	WriteTinyImage(second);
	FileList files({directory.string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Files() == std::vector<fs::path>({first, second}),
		"stale-sort fixture did not begin in filename order");
	const jpegview_linux::SourceKey oldSecondKey = files.DescriptorAt(1)->Key();
	FileListPreparedSort stale = FileList::PrepareSort(
		files.MakeSortRequest(FileList::SortMode::FileName, false), [] { return true; });
	SetModificationTimeNanoseconds(second, 1700000100, 456000000);
	const jpegview_linux::SourceDescriptor refreshed =
		jpegview_linux::DescribeImageSource(second);
	const jpegview_linux::SourceRefreshOutcome refresh =
		jpegview_linux::RefreshFileListSource(files, oldSecondKey, refreshed);
	Expect(refresh.applied && !refresh.sortKeyChanged && !refresh.orderChanged &&
		files.Files() == std::vector<fs::path>({first, second}),
		"a non-sort-key metadata refresh unexpectedly reordered the active list");
	Expect(stale.completed && !files.ApplyPreparedSort(stale),
		"a sort snapshot with an obsolete source descriptor was accepted");
	FileListPreparedSort current = FileList::PrepareSort(
		files.MakeSortRequest(files.GetSorting(), files.IsSortedAscending()),
		[] { return true; });
	Expect(current.completed && files.ApplyPreparedSort(current) &&
		files.Files() == std::vector<fs::path>({second, first}) &&
		files.DescriptorAt(0)->Key() == refreshed.Key() &&
		files.DescriptorAt(0)->Metadata().modificationTimeNanoseconds ==
			refreshed.Metadata().modificationTimeNanoseconds,
		"retrying sort from the current catalog lost refreshed metadata or ordering");
}

void TestFileListCreationTimeArchiveFallback() {
	TemporaryDirectory temporary;
	const fs::path sources = temporary.path() / "sources";
	fs::create_directories(sources);
	const fs::path oldSource = sources / "old.ppm";
	const fs::path newSource = sources / "new.ppm";
	WriteTinyImage(oldSource);
	WriteTinyImage(newSource);
	SetModificationTimeNanoseconds(oldSource, 1600000000, 0);
	SetModificationTimeNanoseconds(newSource, 1600000300, 0);
	const fs::path archive = temporary.path() / "fallback.zip";
	WriteZipArchive(archive, {{"z-old.jpg", oldSource}, {"a-new.jpg", newSource}});

	FileList files({archive.string()}, FileList::SortMode::CreationTime, true, false);
	Expect(FileNames(files) == std::vector<std::string>({"z-old.jpg", "a-new.jpg"}),
		"archive creation-time fallback did not use the member modification timestamps");
	const jpegview_linux::SourceDescriptor* descriptor = files.DescriptorAt(0);
	Expect(descriptor != nullptr && descriptor->Metadata().hasCreationTime &&
		descriptor->Metadata().creationTimeNanoseconds ==
			descriptor->Metadata().modificationTimeNanoseconds,
		"archive member metadata did not preserve its modification-time creation fallback");
}

void TestFileListSizeAndRandomSorting() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "images";
	fs::create_directories(directory);
	WriteBytes(directory / "small.ppm", {'P', '6', '\n', '1', ' ', '1', '\n', '2', '5', '5', '\n', 0, 0, 0});
	WriteBytes(directory / "large.ppm", {'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n',
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
	FileList files({directory.string()}, FileList::SortMode::FileSize, true, false);
	Expect(FileNames(files) == std::vector<std::string>({"small.ppm", "large.ppm"}),
		"ascending file-size ordering is incorrect");
	files.SetSorting(FileList::SortMode::FileSize, false);
	Expect(FileNames(files) == std::vector<std::string>({"large.ppm", "small.ppm"}),
		"descending file-size ordering is incorrect");
	files.SetSorting(FileList::SortMode::Random, true);
	const std::vector<std::string> randomOrder = FileNames(files);
	Expect(randomOrder.size() == 2 && randomOrder[0] != randomOrder[1],
		"random ordering did not retain all files");
	files.SetSorting(FileList::SortMode::Random, true);
	Expect(FileNames(files) == randomOrder, "random ordering was not deterministic for the same files");
}

void TestFileListNavigationModesAndReload() {
	TemporaryDirectory temporary;
	const fs::path root = temporary.path() / "root";
	const fs::path first = root / "01-first";
	const fs::path empty = root / "02-empty";
	const fs::path last = root / "03-last";
	fs::create_directories(first);
	fs::create_directories(empty);
	fs::create_directories(last);
	WriteTinyImage(root / "root.png");
	WriteTinyImage(first / "first.png");
	WriteTinyImage(last / "last.png");

	FileList recursive({root.string()}, FileList::SortMode::FileName, true, false);
	recursive.SetNavigationMode(FileList::NavigationMode::LoopSubDirectories);
	Expect(recursive.Current().filename() == "root.png", "recursive navigation changed the initial image");
	Expect(recursive.Next(), "recursive navigation did not enter the first non-empty folder");
	Expect(recursive.Current().parent_path().filename() == "01-first", "wrong recursive folder entered");
	Expect(recursive.Next(), "recursive navigation did not skip the empty folder");
	Expect(recursive.Current().parent_path().filename() == "03-last", "wrong recursive folder after an empty folder");
	Expect(recursive.Previous(), "recursive navigation did not restore the previous folder");
	Expect(recursive.Current().parent_path().filename() == "01-first", "recursive previous navigation restored the wrong folder");
	Expect(recursive.Reload(recursive.Current()), "reload should succeed for a populated directory");
	Expect(recursive.Current().filename() == "first.png", "reload did not preserve the selected image");

	const fs::path siblings = temporary.path() / "siblings";
	const fs::path siblingA = siblings / "a";
	const fs::path siblingEmpty = siblings / "b-empty";
	const fs::path siblingB = siblings / "c";
	fs::create_directories(siblingA);
	fs::create_directories(siblingEmpty);
	fs::create_directories(siblingB);
	WriteTinyImage(siblingA / "a.png");
	WriteTinyImage(siblingB / "z-last.png");
	WriteTinyImage(siblingB / "a-first.png");
	FileList directSiblings({(siblingA / "a.png").string()}, FileList::SortMode::FileName, true, false);
	directSiblings.SetNavigationMode(FileList::NavigationMode::LoopSubDirectories);
	Expect(directSiblings.NextSiblingDirectory() &&
		directSiblings.Current().parent_path().filename() == "c" &&
		directSiblings.Current().filename() == "a-first.png" &&
		directSiblings.GetNavigationMode() == FileList::NavigationMode::LoopSubDirectories,
		"direct sibling navigation did not skip empty folders, select the first sorted image, or preserve its mode");
	Expect(!directSiblings.NextSiblingDirectory(),
		"direct sibling navigation wrapped past the final sibling folder");
	Expect(directSiblings.PreviousSiblingDirectory() &&
		directSiblings.Current().parent_path().filename() == "a" &&
		directSiblings.Current().filename() == "a.png",
		"previous sibling navigation did not return to the prior populated folder");
	Expect(!directSiblings.PreviousSiblingDirectory(),
		"previous sibling navigation wrapped before the first sibling folder");
	FileList loopingSiblingJump({(siblingA / "a.png").string()}, FileList::SortMode::FileName, true, true);
	Expect(loopingSiblingJump.NextSiblingDirectory(),
		"direct sibling navigation did not enter the next folder for looping navigation");
	Expect(loopingSiblingJump.Previous() && loopingSiblingJump.Current().parent_path().filename() == "c" &&
		loopingSiblingJump.Current().filename() == "z-last.png",
		"previous navigation restored the pre-jump folder instead of wrapping in the current folder");
	FileList siblingList({siblingA.string()}, FileList::SortMode::FileName, true, false);
	siblingList.SetNavigationMode(FileList::NavigationMode::LoopSameDirectoryLevel);
	Expect(siblingList.Next(), "sibling navigation did not enter the next populated sibling");
	Expect(siblingList.Current().parent_path().filename() == "c", "sibling navigation entered the wrong folder");
	Expect(siblingList.Previous(), "sibling navigation did not restore the previous sibling");
	Expect(siblingList.Current().parent_path().filename() == "a", "sibling previous navigation restored the wrong folder");

	FileList wrapping({root.string()}, FileList::SortMode::FileName, true, true);
	wrapping.Last();
	Expect(wrapping.Next(), "wrapping navigation should advance from the last image");
	Expect(wrapping.Current().filename() == "root.png", "wrapping navigation selected the wrong image");
	wrapping.SetWrapAroundFolder(false);
	wrapping.Last();
	Expect(wrapping.NextLoaded() == FileList::LoadedNavigationResult::NoMove &&
		wrapping.Current().filename() == "root.png",
		"disabling folder wrap did not stop forward navigation at the final image");
	wrapping.First();
	Expect(wrapping.PreviousLoaded() == FileList::LoadedNavigationResult::NoMove &&
		wrapping.Current().filename() == "root.png",
		"disabling folder wrap did not stop backward navigation at the first image");
	wrapping.SetWrapAroundFolder(true);
	wrapping.Last();
	Expect(wrapping.NextLoaded() == FileList::LoadedNavigationResult::Moved &&
		wrapping.Current().filename() == "root.png",
		"re-enabling folder wrap did not restore the default boundary behavior");
}

void TestFileListMultipleInputs() {
	TemporaryDirectory temporary;
	const fs::path first = temporary.path() / "first";
	const fs::path second = temporary.path() / "second";
	fs::create_directories(first);
	fs::create_directories(second);
	WriteTinyImage(first / "same.png");
	WriteTinyImage(second / "same.png");
	WriteTinyImage(second / "other.png");

	FileList files({first.string(), second.string(), first.string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 3, "multiple input mode did not deduplicate repeated paths correctly");
	Expect(files.Files()[0].filename() == "other.png", "multiple input filename ordering is incorrect");
	Expect(files.Files()[1].parent_path().filename() == "first", "multiple input tie ordering is unstable");
	Expect(files.Files()[2].parent_path().filename() == "second", "multiple input tie ordering is unstable");
	files.Last();
	Expect(files.NextLoaded() == FileList::LoadedNavigationResult::NoMove,
		"folder-wrap-disabled multiple inputs did not stop at the list boundary");
	files.SetWrapAroundFolder(true);
	files.Last();
	Expect(files.NextLoaded() == FileList::LoadedNavigationResult::Moved,
		"folder-wrap-enabled multiple inputs did not loop at the list boundary");
}

void TestResolveWorkContextComposesInheritedCallbacks() {
	const fs::path path = "/virtual/context-compose.jpg";
	{
		bool activeContinueCalled = false;
		bool localContinueCalled = false;
		bool activePriorityCalled = false;
		bool localPriorityCalled = false;
		jpegview_linux::WorkContext active = jpegview_linux::MakePathWorkContext(path,
			jpegview_linux::SourceWorkPriority::Metadata,
			[&activeContinueCalled] {
				activeContinueCalled = true;
				return true;
			});
		active.currentPriority = [&activePriorityCalled] {
			activePriorityCalled = true;
			return jpegview_linux::SourceWorkPriority::Metadata;
		};
		jpegview_linux::ScopedWorkContext activeScope(active);
		jpegview_linux::WorkContext local;
		local.shouldContinue = [&localContinueCalled] {
			localContinueCalled = true;
			return false;
		};
		local.currentPriority = [&localPriorityCalled] {
			localPriorityCalled = true;
			return jpegview_linux::SourceWorkPriority::Foreground;
		};
		const jpegview_linux::WorkContext resolved = jpegview_linux::ResolveWorkContext(
			path, jpegview_linux::SourceWorkPriority::Metadata, local);
		Expect(resolved.source.logicalPath == active.source.logicalPath &&
			!resolved.Continue() && activeContinueCalled && localContinueCalled,
			"resolving nested work dropped its local cancellation callback or inherited source");
		Expect(resolved.Priority() == jpegview_linux::SourceWorkPriority::Foreground &&
			activePriorityCalled && localPriorityCalled,
			"resolving nested work dropped or weakened its local priority callback");
	}

	jpegview_linux::WorkContext withoutActive;
	withoutActive.currentPriority = [] {
		return jpegview_linux::SourceWorkPriority::Speculative;
	};
	const jpegview_linux::WorkContext fallback = jpegview_linux::ResolveWorkContext(
		path, jpegview_linux::SourceWorkPriority::Metadata, withoutActive);
	Expect(fallback.Priority() == jpegview_linux::SourceWorkPriority::Speculative,
		"path fallback discarded a supplied priority callback");
}

void TestSourceWorkAdmissionAcrossWorkerPools() {
	const auto sourceKey = [](const std::string& path, std::uint64_t inode) {
		jpegview_linux::SourceIdentity identity;
		identity.device = 7;
		identity.inode = inode;
		identity.size = 1024;
		identity.modifiedSeconds = 1700000000;
		identity.valid = true;
		return jpegview_linux::SourceKey{path, identity};
	};
	const auto contextFor = [](jpegview_linux::SourceKey source,
		jpegview_linux::SourceWorkPriority priority) {
		jpegview_linux::WorkContext context;
		context.source = std::move(source);
		context.sourcePriority = priority;
		return context;
	};
	auto partialAdmissionContext = contextFor(sourceKey("/partial/source-only.jpg", 10),
		jpegview_linux::SourceWorkPriority::Foreground);
	partialAdmissionContext.sourceAccessAlreadyAdmitted = true;
	jpegview_linux::SourceWorkCoordinator partialAdmissionPools;
	Expect(!partialAdmissionPools.AcquireSourceAndCpu(partialAdmissionContext),
		"paired admission accepted a context holding only the source lane");
	partialAdmissionContext.sourceAccessAlreadyAdmitted = false;
	partialAdmissionContext.cpuProcessingAlreadyAdmitted = true;
	Expect(!partialAdmissionPools.AcquireSourceAndCpu(partialAdmissionContext),
		"paired admission accepted a context holding only a CPU permit");

	jpegview_linux::SourceWorkCoordinator pools;
	auto firstSpeculation = pools.Acquire(contextFor(sourceKey("/photos/first.jpg", 11),
		jpegview_linux::SourceWorkPriority::Speculative));
	Expect(static_cast<bool>(firstSpeculation),
		"the first speculative worker did not acquire source access");
	std::mutex stateMutex;
	std::condition_variable stateChanged;
	const auto waitForAdmissionEntered = [&](const std::atomic<bool>& entered) {
		std::unique_lock<std::mutex> lock(stateMutex);
		return stateChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return entered.load(); });
	};
	std::atomic<bool> secondEntered{false};
	bool releaseSecond = false;
	std::thread secondPool([&] {
		auto lease = pools.Acquire(contextFor(sourceKey("/photos/second.jpg", 12),
			jpegview_linux::SourceWorkPriority::Metadata));
		std::unique_lock<std::mutex> lock(stateMutex);
		secondEntered.store(static_cast<bool>(lease));
		stateChanged.notify_all();
		stateChanged.wait(lock, [&] { return releaseSecond; });
	});
	const bool secondQueued = pools.WaitForSnapshot([](const jpegview_linux::SourceWorkSnapshot& snapshot) {
		return snapshot.waitingSpeculative == 1;
	}, std::chrono::seconds(2));
	const bool firstSerialized = pools.Snapshot().activeSpeculative == 1 &&
		!secondEntered.load();
	firstSpeculation.Reset();
	bool secondAdmitted = false;
	{
		std::unique_lock<std::mutex> lock(stateMutex);
		secondAdmitted = stateChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return secondEntered.load(); });
		releaseSecond = true;
	}
	stateChanged.notify_all();
	secondPool.join();
	Expect(secondQueued && firstSerialized && secondAdmitted,
		"speculative source admission was not shared across independent worker pools");

	jpegview_linux::SourceWorkCoordinator foregroundPools;
	auto activeSpeculation = foregroundPools.Acquire(contextFor(
		sourceKey("/photos/speculative.jpg", 21),
		jpegview_linux::SourceWorkPriority::Speculative));
	foregroundPools.SetForegroundPending(true);
	std::atomic<bool> queuedSpeculationEntered{false};
	bool releaseQueuedSpeculation = false;
	std::thread queuedSpeculation([&] {
		auto lease = foregroundPools.Acquire(contextFor(
			sourceKey("/photos/queued.jpg", 22),
			jpegview_linux::SourceWorkPriority::Speculative));
		std::unique_lock<std::mutex> lock(stateMutex);
		queuedSpeculationEntered.store(static_cast<bool>(lease));
		stateChanged.notify_all();
		stateChanged.wait(lock, [&] { return releaseQueuedSpeculation; });
	});
	const bool speculativeQueued = foregroundPools.WaitForSnapshot([](
		const jpegview_linux::SourceWorkSnapshot& snapshot) {
		return snapshot.waitingSpeculative == 1;
	}, std::chrono::seconds(2));
	auto foreground = foregroundPools.Acquire(contextFor(
		sourceKey("/photos/selected.jpg", 23),
		jpegview_linux::SourceWorkPriority::Foreground));
	const bool foregroundOvertook = static_cast<bool>(foreground) &&
		foregroundPools.Snapshot().activeForeground == 1;
	activeSpeculation.Reset();
	const bool speculationStayedQueued = foregroundPools.WaitForSnapshot([](
		const jpegview_linux::SourceWorkSnapshot& snapshot) {
		return snapshot.activeForeground == 1 && snapshot.activeSpeculative == 0 &&
			snapshot.waitingSpeculative == 1;
	}, std::chrono::seconds(2)) && !queuedSpeculationEntered.load();
	foreground.Reset();
	foregroundPools.SetForegroundPending(false);
	bool speculationResumed = false;
	{
		std::unique_lock<std::mutex> lock(stateMutex);
		speculationResumed = stateChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return queuedSpeculationEntered.load(); });
		releaseQueuedSpeculation = true;
	}
	stateChanged.notify_all();
	queuedSpeculation.join();
	Expect(speculativeQueued && foregroundOvertook && speculationStayedQueued &&
		speculationResumed,
		"foreground admission did not hold queued speculation until its demand cleared");

	jpegview_linux::SourceWorkCoordinator archivePools;
	const auto firstMember = contextFor(sourceKey("/photos/shared.zip!/a.jpg", 31),
		jpegview_linux::SourceWorkPriority::Speculative);
	const auto secondMember = contextFor(sourceKey("/photos/shared.zip!/b.jpg", 31),
		jpegview_linux::SourceWorkPriority::Foreground);
	auto firstArchiveRead = archivePools.Acquire(firstMember);
	std::atomic<bool> sameContainerEntered{false};
	bool releaseContainerRead = false;
	std::thread secondArchivePool([&] {
		auto lease = archivePools.Acquire(secondMember);
		std::unique_lock<std::mutex> lock(stateMutex);
		sameContainerEntered.store(static_cast<bool>(lease));
		stateChanged.notify_all();
		stateChanged.wait(lock, [&] { return releaseContainerRead; });
	});
	const bool archiveForegroundQueued = archivePools.WaitForSnapshot([](
		const jpegview_linux::SourceWorkSnapshot& snapshot) {
		return snapshot.waitingForeground == 1;
	}, std::chrono::seconds(2)) &&
		archivePools.Snapshot().activeSpeculative == 1 && !sameContainerEntered.load();
	firstArchiveRead.Reset();
	bool archiveResumed = false;
	{
		std::unique_lock<std::mutex> lock(stateMutex);
		archiveResumed = stateChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return sameContainerEntered.load(); });
		releaseContainerRead = true;
	}
	stateChanged.notify_all();
	secondArchivePool.join();
	Expect(archiveForegroundQueued && archiveResumed,
		"archive members with one backing identity did not serialize across worker pools");

	const auto versionedSource = [](const std::string& path, std::uint64_t device,
		std::uint64_t inode, std::uint64_t size, std::int64_t modifiedSeconds,
		std::int64_t modifiedNanoseconds) {
		jpegview_linux::SourceIdentity identity;
		identity.device = device;
		identity.inode = inode;
		identity.size = size;
		identity.modifiedSeconds = modifiedSeconds;
		identity.modifiedNanoseconds = modifiedNanoseconds;
		identity.valid = true;
		return jpegview_linux::SourceKey{path, identity};
	};
	{
		jpegview_linux::SourceWorkCoordinator versionCoordinator;
		const std::string path = "/photos/versioned.zip/image.jpg";
		const auto older = contextFor(versionedSource(path, 61, 62, 100, 1700000000, 10),
			jpegview_linux::SourceWorkPriority::Speculative);
		const auto newer = contextFor(versionedSource(path, 61, 62, 120, 1700000001, 20),
			jpegview_linux::SourceWorkPriority::Foreground);
		auto olderLease = versionCoordinator.Acquire(older, path);
		auto newerAdmission = std::async(std::launch::async,
			[&versionCoordinator, newer, path] {
				return versionCoordinator.Acquire(newer, path);
			});
		const bool newerVersionWaitedForSameObject = versionCoordinator.WaitForSnapshot(
			[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
				return snapshot.activeSpeculative == 1 && snapshot.activeForeground == 0 &&
					snapshot.waitingForeground == 1;
			}, std::chrono::seconds(2));
		olderLease.Reset();
		auto newerLease = newerAdmission.get();
		const bool newerVersionAdmittedAfterRelease = static_cast<bool>(newerLease);
		newerLease.Reset();
		Expect(newerVersionWaitedForSameObject && newerVersionAdmittedAfterRelease,
			"source-only admission overlapped two versions of one device/inode backing object");
	}
	{
		jpegview_linux::SourceWorkCoordinator versionCoordinator;
		const std::string path = "/photos/paired-versioned.zip/image.jpg";
		const auto older = contextFor(versionedSource(path, 71, 72, 100, 1700000010, 10),
			jpegview_linux::SourceWorkPriority::Speculative);
		const auto newer = contextFor(versionedSource(path, 71, 72, 120, 1700000011, 20),
			jpegview_linux::SourceWorkPriority::Foreground);
		auto olderLease = versionCoordinator.AcquireSourceAndCpu(older, path);
		auto newerAdmission = std::async(std::launch::async,
			[&versionCoordinator, newer, path] {
				return versionCoordinator.AcquireSourceAndCpu(newer, path);
			});
		const bool newerVersionWaitedForSameObject = versionCoordinator.WaitForSnapshot(
			[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
				return snapshot.activeSpeculative == 1 && snapshot.activeForeground == 0 &&
					snapshot.waitingForeground == 1 && snapshot.activeCpu == 1 &&
					snapshot.waitingCpu == 1;
			}, std::chrono::seconds(2));
		olderLease.Reset();
		auto newerLease = newerAdmission.get();
		const bool newerVersionAdmittedAfterRelease = static_cast<bool>(newerLease);
		newerLease.Reset();
		Expect(newerVersionWaitedForSameObject && newerVersionAdmittedAfterRelease,
			"paired admission overlapped two versions of one device/inode backing object");
	}

	jpegview_linux::SourceWorkCoordinator cancellationPools;
	auto heldForCancellation = cancellationPools.Acquire(contextFor(
		sourceKey("/photos/active.jpg", 51),
		jpegview_linux::SourceWorkPriority::Speculative));
	std::atomic<bool> keepWaiting{true};
	std::atomic<bool> canceledRequestEntered{false};
	std::atomic<bool> canceledRequestReturned{false};
	bool releaseCanceledRequest = false;
	auto canceledContext = contextFor(sourceKey("/photos/canceled.jpg", 52),
		jpegview_linux::SourceWorkPriority::Speculative);
	canceledContext.shouldContinue = [&keepWaiting] { return keepWaiting.load(); };
	std::thread canceledWaiter([&] {
		auto lease = cancellationPools.Acquire(canceledContext);
		canceledRequestEntered.store(static_cast<bool>(lease));
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			canceledRequestReturned.store(true);
		}
		stateChanged.notify_all();
		if (lease) {
			std::unique_lock<std::mutex> lock(stateMutex);
			stateChanged.wait(lock, [&] { return releaseCanceledRequest; });
		}
	});
	const bool cancellationQueued = cancellationPools.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	keepWaiting.store(false);
	cancellationPools.NotifyWaiters();
	const bool canceledWaiterLeftQueue = cancellationPools.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(2));
	heldForCancellation.Reset();
	auto afterCancellation = cancellationPools.Acquire(contextFor(
		sourceKey("/photos/after-cancel.jpg", 53),
		jpegview_linux::SourceWorkPriority::Speculative));
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		releaseCanceledRequest = true;
	}
	stateChanged.notify_all();
	canceledWaiter.join();
	Expect(cancellationQueued && canceledWaiterLeftQueue &&
		!canceledRequestEntered.load() && canceledRequestReturned.load() &&
		static_cast<bool>(afterCancellation),
		"canceling a waiting source task leaked its queue slot or admission permit");

	Expect(jpegview_linux::HardwareAwareCpuWorkerCount(1) == 1 &&
		jpegview_linux::HardwareAwareCpuWorkerCount(2) == 1 &&
		jpegview_linux::HardwareAwareCpuWorkerCount(3) == 2 &&
		jpegview_linux::HardwareAwareCpuWorkerCount(16) == 4 &&
		jpegview_linux::ClampCpuWorkerCount(12, 16) == 4 &&
		jpegview_linux::ClampCpuWorkerCount(0, 16) == 4,
		"CPU processing worker counts no longer honor hardware limits or the four-worker cap");

	jpegview_linux::SourceWorkCoordinator cpuPools;
	const std::size_t cpuLimit = jpegview_linux::HardwareAwareCpuWorkerCount();
	std::atomic<std::size_t> activeCpu{0};
	std::atomic<std::size_t> peakCpu{0};
	std::atomic<std::size_t> completedCpuWorkers{0};
	bool releaseCpuWorkers = false;
	std::vector<std::thread> cpuWorkers;
	for (std::size_t index = 0; index < cpuLimit + 3; ++index) {
		cpuWorkers.emplace_back([&, index] {
			auto context = contextFor(sourceKey("/cpu/" + std::to_string(index), 100 + index),
				jpegview_linux::SourceWorkPriority::Speculative);
			auto lease = cpuPools.AcquireCpu(context);
			if (!lease) return;
			const std::size_t active = activeCpu.fetch_add(1) + 1;
			std::size_t peak = peakCpu.load();
			while (peak < active && !peakCpu.compare_exchange_weak(peak, active)) {}
			{
				std::unique_lock<std::mutex> lock(stateMutex);
				stateChanged.notify_all();
				stateChanged.wait(lock, [&] { return releaseCpuWorkers; });
			}
			activeCpu.fetch_sub(1);
			completedCpuWorkers.fetch_add(1);
		});
	}
	const bool cpuCapReached = cpuPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.waitingCpu >= 1;
		}, std::chrono::seconds(2));
	std::atomic<bool> keepWaitingForCpu{true};
	std::atomic<bool> canceledCpuReturned{false};
	auto canceledCpuContext = contextFor(sourceKey("/cpu/canceled", 120),
		jpegview_linux::SourceWorkPriority::Speculative);
	canceledCpuContext.shouldContinue = [&keepWaitingForCpu] {
		return keepWaitingForCpu.load();
	};
	std::thread canceledCpuWaiter([&] {
		auto lease = cpuPools.AcquireCpu(canceledCpuContext);
		canceledCpuReturned.store(!lease);
	});
	const bool canceledCpuQueued = cpuPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.waitingCpu >= 4;
		}, std::chrono::seconds(2));
	keepWaitingForCpu.store(false);
	cpuPools.NotifyWaiters();
	const bool canceledCpuRemoved = cpuPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.waitingCpu == 3;
		}, std::chrono::seconds(2));
	canceledCpuWaiter.join();
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		releaseCpuWorkers = true;
	}
	stateChanged.notify_all();
	for (std::thread& worker : cpuWorkers) worker.join();
	Expect(cpuCapReached && canceledCpuQueued && canceledCpuRemoved &&
		canceledCpuReturned.load() && completedCpuWorkers.load() == cpuLimit + 3 &&
		peakCpu.load() <= cpuLimit && cpuPools.Snapshot().activeCpu == 0 &&
		cpuPools.Snapshot().waitingCpu == 0,
		"CPU work was not capped across pools or canceled waiters retained shared permits");

	jpegview_linux::SourceWorkCoordinator pairedPools;
	std::vector<jpegview_linux::CpuWorkLease> occupiedPermits;
	for (std::size_t index = 0; index < cpuLimit; ++index) {
		auto blocker = pairedPools.AcquireCpu(contextFor(
			sourceKey("/paired/cpu-" + std::to_string(index), 200 + index),
			jpegview_linux::SourceWorkPriority::Speculative));
		Expect(static_cast<bool>(blocker), "could not saturate paired source/CPU admission");
		occupiedPermits.push_back(std::move(blocker));
	}
	const auto pairedSource = sourceKey("/paired/shared.jpg", 220);
	std::atomic<bool> pairedSpeculationEntered{false};
	std::atomic<bool> pairedForegroundEntered{false};
	bool releasePairedSpeculation = false;
	bool releasePairedForeground = false;
	auto pairedSpeculativeContext = contextFor(pairedSource,
		jpegview_linux::SourceWorkPriority::Speculative);
	std::thread pairedSpeculation([&] {
		auto admission = pairedPools.AcquireSourceAndCpu(pairedSpeculativeContext);
		pairedSpeculationEntered.store(static_cast<bool>(admission));
		if (admission) {
			std::unique_lock<std::mutex> lock(stateMutex);
			stateChanged.notify_all();
			stateChanged.wait(lock, [&] { return releasePairedSpeculation; });
		}
	});
	const bool pairedSpeculationWaitedForCpu = pairedPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.waitingCpu == 1 &&
				snapshot.waitingSpeculative == 1 && snapshot.activeSpeculative == 0;
		}, std::chrono::seconds(2));
	pairedPools.SetForegroundPending(true);
	auto pairedForegroundContext = contextFor(pairedSource,
		jpegview_linux::SourceWorkPriority::Foreground);
	std::thread pairedForeground([&] {
		auto admission = pairedPools.AcquireSourceAndCpu(pairedForegroundContext);
		pairedForegroundEntered.store(static_cast<bool>(admission));
		if (admission) {
			std::unique_lock<std::mutex> lock(stateMutex);
			stateChanged.notify_all();
			stateChanged.wait(lock, [&] { return releasePairedForeground; });
		}
	});
	const bool pairedForegroundQueued = pairedPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.waitingCpu == 2 &&
				snapshot.waitingForeground == 1 && snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	occupiedPermits.front().Reset();
	const bool pairedForegroundActive = pairedPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.activeForeground == 1 &&
				snapshot.activeSpeculative == 0 && snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	const bool pairedForegroundOvertook = pairedForegroundActive &&
		waitForAdmissionEntered(pairedForegroundEntered);
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		releasePairedForeground = true;
	}
	stateChanged.notify_all();
	pairedForeground.join();
	const bool pairedSpeculationStayedYielded = pairedPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingSpeculative == 1 && snapshot.activeCpu == cpuLimit - 1;
		}, std::chrono::seconds(2)) && !pairedSpeculationEntered.load();
	pairedPools.SetForegroundPending(false);
	const bool pairedSpeculationActive = pairedPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 1 && snapshot.activeCpu == cpuLimit;
		}, std::chrono::seconds(2));
	const bool pairedSpeculationResumed = pairedSpeculationActive &&
		waitForAdmissionEntered(pairedSpeculationEntered);
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		releasePairedSpeculation = true;
	}
	stateChanged.notify_all();
	pairedSpeculation.join();
	for (auto& permit : occupiedPermits) permit.Reset();
	std::ostringstream pairedAdmissionFailure;
	pairedAdmissionFailure << "foreground source work did not overtake paired speculative admission "
		"or speculation did not resume (specQueued=" << pairedSpeculationWaitedForCpu
		<< ", foregroundQueued=" << pairedForegroundQueued << ", foregroundOvertook="
		<< pairedForegroundOvertook << ", speculationStayedYielded="
		<< pairedSpeculationStayedYielded << ", speculationResumed="
		<< pairedSpeculationResumed << ", activeCpu=" << pairedPools.Snapshot().activeCpu
		<< ')';
	Expect(pairedSpeculationWaitedForCpu && pairedForegroundQueued &&
		pairedForegroundOvertook && pairedSpeculationStayedYielded &&
		pairedSpeculationResumed && pairedPools.Snapshot().activeCpu == 0,
		pairedAdmissionFailure.str());

	jpegview_linux::SourceWorkCoordinator promotionPools;
	std::vector<jpegview_linux::CpuWorkLease> promotionPermits;
	for (std::size_t index = 0; index < cpuLimit; ++index) {
		auto blocker = promotionPools.AcquireCpu(contextFor(
			sourceKey("/promotion/cpu-" + std::to_string(index), 240 + index),
				jpegview_linux::SourceWorkPriority::Speculative));
		Expect(static_cast<bool>(blocker), "could not saturate promotion admission");
		promotionPermits.push_back(std::move(blocker));
	}
	std::atomic<bool> promoteQueuedWork{false};
	std::atomic<bool> promotedAdmissionEntered{false};
	bool releasePromotedAdmission = false;
	auto promotedContext = contextFor(sourceKey("/promotion/selected.jpg", 230),
		jpegview_linux::SourceWorkPriority::Speculative);
	promotedContext.currentPriority = [&promoteQueuedWork] {
		return promoteQueuedWork.load() ? jpegview_linux::SourceWorkPriority::Foreground :
			jpegview_linux::SourceWorkPriority::Speculative;
	};
	std::thread promotedWorker([&] {
		auto admission = promotionPools.AcquireSourceAndCpu(promotedContext);
		promotedAdmissionEntered.store(static_cast<bool>(admission));
		if (admission) {
			std::unique_lock<std::mutex> lock(stateMutex);
			stateChanged.notify_all();
			stateChanged.wait(lock, [&] { return releasePromotedAdmission; });
		}
	});
	const bool promotionOriginallyQueued = promotionPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.waitingCpu == 1 &&
				snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	promotionPools.SetForegroundPending(true);
	promoteQueuedWork.store(true);
	promotionPools.NotifyWaiters();
	const bool promotionMovedBothQueues = promotionPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.waitingCpu == 1 &&
				snapshot.waitingForeground == 1 && snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(2));
	promotionPermits.front().Reset();
	const bool promotionForegroundActive = promotionPools.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.activeForeground == 1 &&
				snapshot.activeSpeculative == 0;
		}, std::chrono::seconds(2));
	const bool promotionEnteredForegroundLane = promotionForegroundActive &&
		waitForAdmissionEntered(promotedAdmissionEntered);
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		releasePromotedAdmission = true;
	}
	stateChanged.notify_all();
	promotionPools.SetForegroundPending(false);
	promotedWorker.join();
	for (auto& permit : promotionPermits) permit.Reset();
	Expect(promotionOriginallyQueued && promotionMovedBothQueues &&
		promotionEnteredForegroundLane && promotionPools.Snapshot().activeCpu == 0,
		"promoting in-flight source work did not move its paired source and CPU admission to foreground");

	const auto throwingContext = contextFor(sourceKey("/photos/throws.jpg", 44),
		jpegview_linux::SourceWorkPriority::Speculative);
	try {
		auto lease = archivePools.Acquire(throwingContext);
		Expect(static_cast<bool>(lease), "exception-release fixture could not acquire source access");
		throw std::runtime_error("injected worker exception");
	} catch (const std::runtime_error&) {
	}
	Expect(static_cast<bool>(archivePools.Acquire(throwingContext)),
		"exception unwinding leaked a source admission permit");

	const auto coordinatorEmpty = [](const jpegview_linux::SourceWorkSnapshot& snapshot) {
		return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
			snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0 &&
			snapshot.activeCpu == 0 && snapshot.waitingCpu == 0;
	};
	for (const auto point : {
		jpegview_linux::detail::SourceWorkTestHookPoint::InitialSourceQueueRegistration,
		jpegview_linux::detail::SourceWorkTestHookPoint::InitialCpuQueueRegistration}) {
		jpegview_linux::SourceWorkCoordinator failurePools;
		SourceWorkFailureInjection injection{point};
		failurePools.SetTestHookForTesting(ThrowAtSourceWorkHook, &injection);
		bool registrationFailed = false;
		try {
			(void)failurePools.AcquireSourceAndCpu(contextFor(
				sourceKey("/admission/initial-registration.jpg", 300),
				jpegview_linux::SourceWorkPriority::Speculative));
		} catch (const std::bad_alloc&) {
			registrationFailed = true;
		}
		failurePools.SetTestHookForTesting(nullptr, nullptr);
		const bool rolledBack = coordinatorEmpty(failurePools.Snapshot());
		auto validAdmission = failurePools.AcquireSourceAndCpu(contextFor(
			sourceKey("/admission/initial-retry.jpg", 301),
			jpegview_linux::SourceWorkPriority::Speculative));
		const bool laterAdmissionWorked = static_cast<bool>(validAdmission);
		validAdmission.Reset();
		Expect(injection.fired.load() && registrationFailed && rolledBack &&
			laterAdmissionWorked && coordinatorEmpty(failurePools.Snapshot()),
			"paired initial queue registration left a partial record or blocked a later request");
	}

	for (const auto point : {
		jpegview_linux::detail::SourceWorkTestHookPoint::InitialSourceQueueRegistration,
		jpegview_linux::detail::SourceWorkTestHookPoint::InitialCpuQueueRegistration}) {
		jpegview_linux::SourceWorkCoordinator failurePools;
		SourceWorkFailureInjection injection{point};
		failurePools.SetTestHookForTesting(ThrowAtSourceWorkHook, &injection);
		bool registrationFailed = false;
		try {
			const auto context = contextFor(sourceKey("/admission/single-initial.jpg", 309),
				jpegview_linux::SourceWorkPriority::Speculative);
			if (point == jpegview_linux::detail::SourceWorkTestHookPoint::InitialSourceQueueRegistration) {
				(void)failurePools.Acquire(context);
			} else {
				(void)failurePools.AcquireCpu(context);
			}
		} catch (const std::bad_alloc&) {
			registrationFailed = true;
		}
		failurePools.SetTestHookForTesting(nullptr, nullptr);
		const bool rolledBack = coordinatorEmpty(failurePools.Snapshot());
		const auto retryContext = contextFor(sourceKey("/admission/single-initial-retry.jpg", 310),
			jpegview_linux::SourceWorkPriority::Speculative);
		bool laterAdmissionWorked = false;
		if (point == jpegview_linux::detail::SourceWorkTestHookPoint::InitialSourceQueueRegistration) {
			auto lease = failurePools.Acquire(retryContext);
			laterAdmissionWorked = static_cast<bool>(lease);
		} else {
			auto lease = failurePools.AcquireCpu(retryContext);
			laterAdmissionWorked = static_cast<bool>(lease);
		}
		Expect(injection.fired.load() && registrationFailed && rolledBack &&
			laterAdmissionWorked && coordinatorEmpty(failurePools.Snapshot()),
			"single-resource initial registration leaked its queue record or blocked a retry");
	}

	{
		jpegview_linux::SourceWorkCoordinator failurePools;
		auto blocker = failurePools.Acquire(contextFor(sourceKey(
			"/admission/promotion-blocker.jpg", 302),
			jpegview_linux::SourceWorkPriority::Speculative));
		std::atomic<bool> promote{false};
		auto context = contextFor(sourceKey("/admission/promoted.jpg", 303),
			jpegview_linux::SourceWorkPriority::Speculative);
		context.currentPriority = [&promote] {
			return promote.load() ? jpegview_linux::SourceWorkPriority::Foreground :
				jpegview_linux::SourceWorkPriority::Speculative;
		};
		SourceWorkFailureInjection injection{
			jpegview_linux::detail::SourceWorkTestHookPoint::PromotedCpuQueueRegistration};
		failurePools.SetTestHookForTesting(ThrowAtSourceWorkHook, &injection);
		std::atomic<bool> promotionFailed{false};
		std::thread worker([&] {
			try {
				(void)failurePools.AcquireSourceAndCpu(context);
			} catch (const std::bad_alloc&) {
				promotionFailed.store(true);
			}
		});
		const bool queuedTogether = failurePools.WaitForSnapshot(
			[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
				return snapshot.activeSpeculative == 1 && snapshot.waitingSpeculative == 1 &&
					snapshot.waitingCpu == 1;
			}, std::chrono::seconds(2));
		promote.store(true);
		failurePools.NotifyWaiters();
		worker.join();
		failurePools.SetTestHookForTesting(nullptr, nullptr);
		const auto afterFailure = failurePools.Snapshot();
		const bool candidateRemoved = afterFailure.activeSpeculative == 1 &&
			afterFailure.activeForeground == 0 && afterFailure.waitingSpeculative == 0 &&
			afterFailure.waitingForeground == 0 && afterFailure.activeCpu == 0 &&
			afterFailure.waitingCpu == 0;
		blocker.Reset();
		auto validAdmission = failurePools.AcquireSourceAndCpu(contextFor(
			sourceKey("/admission/promotion-retry.jpg", 304),
			jpegview_linux::SourceWorkPriority::Foreground));
		const bool laterAdmissionWorked = static_cast<bool>(validAdmission);
		validAdmission.Reset();
		Expect(queuedTogether && injection.fired.load() && promotionFailed.load() &&
			candidateRemoved && laterAdmissionWorked && coordinatorEmpty(failurePools.Snapshot()),
			"paired queue promotion left a request split across its source and CPU lanes");
	}

	{
		jpegview_linux::SourceWorkCoordinator failurePools;
		auto blocker = failurePools.Acquire(contextFor(sourceKey(
			"/admission/single-promotion-blocker.jpg", 311),
			jpegview_linux::SourceWorkPriority::Speculative));
		std::atomic<bool> promote{false};
		auto context = contextFor(sourceKey("/admission/single-promoted.jpg", 312),
			jpegview_linux::SourceWorkPriority::Speculative);
		context.currentPriority = [&promote] {
			return promote.load() ? jpegview_linux::SourceWorkPriority::Foreground :
				jpegview_linux::SourceWorkPriority::Speculative;
		};
		SourceWorkFailureInjection injection{
			jpegview_linux::detail::SourceWorkTestHookPoint::PromotedSourceQueueRegistration};
		failurePools.SetTestHookForTesting(ThrowAtSourceWorkHook, &injection);
		std::atomic<bool> promotionFailed{false};
		std::thread worker([&] {
			try {
				(void)failurePools.Acquire(context);
			} catch (const std::bad_alloc&) {
				promotionFailed.store(true);
			}
		});
		const bool queued = failurePools.WaitForSnapshot(
			[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
				return snapshot.activeSpeculative == 1 && snapshot.waitingSpeculative == 1;
			}, std::chrono::seconds(2));
		promote.store(true);
		failurePools.NotifyWaiters();
		worker.join();
		failurePools.SetTestHookForTesting(nullptr, nullptr);
		const auto afterFailure = failurePools.Snapshot();
		const bool rolledBack = afterFailure.activeSpeculative == 1 &&
			afterFailure.activeForeground == 0 && afterFailure.waitingSpeculative == 0 &&
			afterFailure.waitingForeground == 0;
		blocker.Reset();
		auto retry = failurePools.Acquire(contextFor(sourceKey(
			"/admission/single-promotion-retry.jpg", 313),
			jpegview_linux::SourceWorkPriority::Foreground));
		const bool laterAdmissionWorked = static_cast<bool>(retry);
		retry.Reset();
		Expect(queued && injection.fired.load() && promotionFailed.load() && rolledBack &&
			laterAdmissionWorked && coordinatorEmpty(failurePools.Snapshot()),
			"source-only promotion left split queue state or blocked a later request");
	}

	{
		jpegview_linux::SourceWorkCoordinator failurePools;
		const std::size_t failureCpuLimit = jpegview_linux::HardwareAwareCpuWorkerCount();
		std::vector<jpegview_linux::CpuWorkLease> blockers;
		for (std::size_t index = 0; index < failureCpuLimit; ++index) {
			auto lease = failurePools.AcquireCpu(contextFor(sourceKey(
				"/admission/single-cpu-blocker-" + std::to_string(index), 320 + index),
				jpegview_linux::SourceWorkPriority::Foreground));
			Expect(static_cast<bool>(lease), "could not saturate single CPU promotion admission");
			blockers.push_back(std::move(lease));
		}
		std::atomic<bool> promote{false};
		auto context = contextFor(sourceKey("/admission/single-cpu-promoted.jpg", 321),
			jpegview_linux::SourceWorkPriority::Speculative);
		context.currentPriority = [&promote] {
			return promote.load() ? jpegview_linux::SourceWorkPriority::Foreground :
				jpegview_linux::SourceWorkPriority::Speculative;
		};
		SourceWorkFailureInjection injection{
			jpegview_linux::detail::SourceWorkTestHookPoint::PromotedCpuQueueRegistration};
		failurePools.SetTestHookForTesting(ThrowAtSourceWorkHook, &injection);
		std::atomic<bool> promotionFailed{false};
		std::thread worker([&] {
			try {
				(void)failurePools.AcquireCpu(context);
			} catch (const std::bad_alloc&) {
				promotionFailed.store(true);
			}
		});
		const bool queued = failurePools.WaitForSnapshot(
			[failureCpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
				return snapshot.activeCpu == failureCpuLimit && snapshot.waitingCpu == 1;
			}, std::chrono::seconds(2));
		promote.store(true);
		failurePools.NotifyWaiters();
		worker.join();
		failurePools.SetTestHookForTesting(nullptr, nullptr);
		const auto afterFailure = failurePools.Snapshot();
		const bool rolledBack = afterFailure.activeCpu == failureCpuLimit &&
			afterFailure.waitingCpu == 0;
		for (auto& blocker : blockers) blocker.Reset();
		auto retry = failurePools.AcquireCpu(contextFor(sourceKey(
			"/admission/single-cpu-promotion-retry.jpg", 322),
			jpegview_linux::SourceWorkPriority::Foreground));
		const bool laterAdmissionWorked = static_cast<bool>(retry);
		retry.Reset();
		Expect(queued && injection.fired.load() && promotionFailed.load() && rolledBack &&
			laterAdmissionWorked && coordinatorEmpty(failurePools.Snapshot()),
			"CPU-only promotion left a stale queue entry or blocked a later request");
	}

	for (const auto point : {
		jpegview_linux::detail::SourceWorkTestHookPoint::ActiveSourceRegistration,
		jpegview_linux::detail::SourceWorkTestHookPoint::ActiveCpuRegistration}) {
		jpegview_linux::SourceWorkCoordinator failurePools;
		SourceWorkFailureInjection injection{point};
		failurePools.SetTestHookForTesting(ThrowAtSourceWorkHook, &injection);
		bool activeRegistrationFailed = false;
		try {
			(void)failurePools.AcquireSourceAndCpu(contextFor(
				sourceKey("/admission/active-registration.jpg", 305),
				jpegview_linux::SourceWorkPriority::Foreground));
		} catch (const std::bad_alloc&) {
			activeRegistrationFailed = true;
		}
		failurePools.SetTestHookForTesting(nullptr, nullptr);
		const bool rolledBack = coordinatorEmpty(failurePools.Snapshot());
		auto validAdmission = failurePools.AcquireSourceAndCpu(contextFor(
			sourceKey("/admission/active-retry.jpg", 306),
			jpegview_linux::SourceWorkPriority::Foreground));
		const bool laterAdmissionWorked = static_cast<bool>(validAdmission);
		validAdmission.Reset();
		Expect(injection.fired.load() && activeRegistrationFailed && rolledBack &&
			laterAdmissionWorked && coordinatorEmpty(failurePools.Snapshot()),
			"paired active registration leaked a source or CPU permit after allocation failure");
	}

	for (const auto point : {
		jpegview_linux::detail::SourceWorkTestHookPoint::ActiveSourceRegistration,
		jpegview_linux::detail::SourceWorkTestHookPoint::ActiveCpuRegistration}) {
		jpegview_linux::SourceWorkCoordinator failurePools;
		SourceWorkFailureInjection injection{point};
		failurePools.SetTestHookForTesting(ThrowAtSourceWorkHook, &injection);
		bool registrationFailed = false;
		try {
			if (point == jpegview_linux::detail::SourceWorkTestHookPoint::ActiveSourceRegistration) {
				(void)failurePools.Acquire(contextFor(sourceKey(
					"/admission/source-only-active.jpg", 307),
					jpegview_linux::SourceWorkPriority::Foreground));
			} else {
				(void)failurePools.AcquireCpu(contextFor(sourceKey(
					"/admission/cpu-only-active.jpg", 308),
					jpegview_linux::SourceWorkPriority::Foreground));
			}
		} catch (const std::bad_alloc&) {
			registrationFailed = true;
		}
		failurePools.SetTestHookForTesting(nullptr, nullptr);
		const bool rolledBack = coordinatorEmpty(failurePools.Snapshot());
		bool laterAdmissionWorked = false;
		if (point == jpegview_linux::detail::SourceWorkTestHookPoint::ActiveSourceRegistration) {
			auto lease = failurePools.Acquire(contextFor(sourceKey(
				"/admission/source-only-active-retry.jpg", 323),
				jpegview_linux::SourceWorkPriority::Foreground));
			laterAdmissionWorked = static_cast<bool>(lease);
		} else {
			auto lease = failurePools.AcquireCpu(contextFor(sourceKey(
				"/admission/cpu-only-active-retry.jpg", 324),
				jpegview_linux::SourceWorkPriority::Foreground));
			laterAdmissionWorked = static_cast<bool>(lease);
		}
		Expect(injection.fired.load() && registrationFailed && rolledBack &&
			laterAdmissionWorked && coordinatorEmpty(failurePools.Snapshot()),
			"single-resource active registration left a ghost queue entry or permit");
	}
}

void TestWorkContextPathAdmissionKeysAvoidMetadataIo() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "path-key-source.jpg";
	const fs::path alias = temporary.path() / "path-key-alias.jpg";
	WriteText(source, "path key identity fixture");
	std::error_code linkError;
	fs::create_hard_link(source, alias, linkError);

	const jpegview_linux::WorkContext pathContext =
		jpegview_linux::MakePathWorkContext(source,
			jpegview_linux::SourceWorkPriority::Metadata);
	const bool pathContextAvoidedIdentityCapture =
		!pathContext.source.backingIdentity.valid &&
		pathContext.source.logicalPath == source.string();

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const bool coordinatorIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(2));
	const auto pathOnlyContext = [](const fs::path& path,
		jpegview_linux::SourceWorkPriority priority) {
		jpegview_linux::WorkContext context;
		context.source.logicalPath = path.string();
		context.sourcePriority = priority;
		return context;
	};
	jpegview_linux::SourceWorkLease sourceLease = coordinator.Acquire(
		pathOnlyContext(source, jpegview_linux::SourceWorkPriority::Metadata), source);
	const bool sourceLeaseAcquired = static_cast<bool>(sourceLease);
	std::mutex aliasMutex;
	std::condition_variable aliasChanged;
	bool releaseAlias = false;
	std::atomic<bool> aliasAdmitted{false};
	std::thread aliasWorker([&] {
		auto lease = coordinator.Acquire(pathOnlyContext(alias,
			jpegview_linux::SourceWorkPriority::Foreground), alias);
		aliasAdmitted.store(static_cast<bool>(lease));
		aliasChanged.notify_all();
		std::unique_lock<std::mutex> lock(aliasMutex);
		aliasChanged.wait(lock, [&] { return releaseAlias; });
	});
	const bool hardlinkPathsAdmittedIndependently = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 1 && snapshot.activeSpeculative == 1;
		}, std::chrono::milliseconds(250));
	sourceLease.Reset();
	{
		std::unique_lock<std::mutex> lock(aliasMutex);
		(void)aliasChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return aliasAdmitted.load(); });
		releaseAlias = true;
	}
	aliasChanged.notify_all();
	aliasWorker.join();

	const fs::path archive = temporary.path() / "path-key-archive.zip";
	WriteText(archive, "archive backing path fixture");
	const fs::path firstMember = archive / "first.jpg";
	const fs::path secondMember = archive / "second.jpg";
	const jpegview_linux::WorkContext archiveMemberContext =
		jpegview_linux::MakePathWorkContext(firstMember,
			jpegview_linux::SourceWorkPriority::Metadata);
	const bool archiveContextAvoidedIdentityCapture =
		!archiveMemberContext.source.backingIdentity.valid;
	const bool archiveCandidateMatchesBackingPath =
		jpegview_linux::ArchiveBackingFileForAdmission(firstMember) == archive;
	jpegview_linux::SourceWorkLease firstMemberLease = coordinator.Acquire(
		pathOnlyContext(firstMember, jpegview_linux::SourceWorkPriority::Metadata),
		firstMember);
	const bool firstMemberLeaseAcquired = static_cast<bool>(firstMemberLease);
	std::atomic<bool> secondMemberAdmitted{false};
	std::thread memberWorker([&] {
		auto lease = coordinator.Acquire(pathOnlyContext(secondMember,
			jpegview_linux::SourceWorkPriority::Foreground), secondMember);
		secondMemberAdmitted.store(static_cast<bool>(lease));
	});
	const bool archiveMembersSharedBackingAdmission = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 1 &&
				snapshot.waitingForeground == 1 && snapshot.activeForeground == 0;
		}, std::chrono::seconds(2));
	const jpegview_linux::SourceWorkSnapshot archiveAdmissionSnapshot = coordinator.Snapshot();
	firstMemberLease.Reset();
	memberWorker.join();
	const bool allAdmissionsReturnedToIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(2));
	std::ostringstream failure;
	failure << "path admission keys performed metadata I/O before admission or failed to use archive backing paths"
		<< " (linkError=" << linkError.value() << ", coordinatorIdle=" << coordinatorIdle
		<< ", pathIdentityInvalid=" << pathContextAvoidedIdentityCapture
		<< ", sourceLease=" << sourceLeaseAcquired
		<< ", independentHardlinks=" << hardlinkPathsAdmittedIndependently
		<< ", aliasAdmitted=" << aliasAdmitted.load()
		<< ", firstMemberLease=" << firstMemberLeaseAcquired
		<< ", archiveIdentityInvalid=" << archiveContextAvoidedIdentityCapture
		<< ", backingCandidate=" << archiveCandidateMatchesBackingPath
		<< ", sharedArchiveWait=" << archiveMembersSharedBackingAdmission
		<< ", archiveSnapshot=" << archiveAdmissionSnapshot.activeSpeculative << '/'
		<< archiveAdmissionSnapshot.waitingForeground << '/'
		<< archiveAdmissionSnapshot.activeForeground
		<< ", secondMemberAdmitted=" << secondMemberAdmitted.load()
		<< ", finalIdle=" << allAdmissionsReturnedToIdle << ')';
	Expect(!linkError && coordinatorIdle && pathContextAvoidedIdentityCapture &&
		sourceLeaseAcquired && firstMemberLeaseAcquired &&
		hardlinkPathsAdmittedIndependently && aliasAdmitted.load() &&
		archiveContextAvoidedIdentityCapture && archiveCandidateMatchesBackingPath &&
		archiveMembersSharedBackingAdmission &&
		secondMemberAdmitted.load() &&
		allAdmissionsReturnedToIdle,
		failure.str());

	// A path-only worker may enter first, then a foreground request may arrive
	// with an already captured descriptor for that same archive member. The
	// descriptor's identity must not erase its normalized fallback path.
	{
		jpegview_linux::SourceWorkCoordinator mixedIdentityCoordinator;
		jpegview_linux::SourceIdentity knownIdentity;
		knownIdentity.device = 91;
		knownIdentity.inode = 92;
		knownIdentity.size = 93;
		knownIdentity.modifiedSeconds = 94;
		knownIdentity.valid = true;
		const fs::path member = temporary.path() / "mixed-identity.zip" / "image.jpg";
		const jpegview_linux::SourceDescriptor descriptor(member, knownIdentity, {});
		const jpegview_linux::WorkContext pathOnly =
			jpegview_linux::MakePathWorkContext(member,
				jpegview_linux::SourceWorkPriority::Metadata);
		jpegview_linux::SourceWorkLease pathOnlyLease =
			mixedIdentityCoordinator.Acquire(pathOnly, member);
		auto descriptorAdmission = std::async(std::launch::async,
			[&mixedIdentityCoordinator, descriptor] {
				return mixedIdentityCoordinator.Acquire(
					jpegview_linux::MakeWorkContext(descriptor,
						jpegview_linux::SourceWorkPriority::Foreground),
					descriptor.LogicalPath());
			});
		const bool crossIdentityRequestRegistered =
			mixedIdentityCoordinator.WaitForSnapshot(
				[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
					return snapshot.activeForeground == 1 || snapshot.waitingForeground == 1;
				}, std::chrono::seconds(2));
		const jpegview_linux::SourceWorkSnapshot mixedSnapshot =
			mixedIdentityCoordinator.Snapshot();
		const bool descriptorWaitedForPathOnlyLease = crossIdentityRequestRegistered &&
			mixedSnapshot.activeSpeculative == 1 &&
			mixedSnapshot.activeForeground == 0 &&
			mixedSnapshot.waitingForeground == 1;
		pathOnlyLease.Reset();
		jpegview_linux::SourceWorkLease descriptorLease = descriptorAdmission.get();
		descriptorLease.Reset();
		Expect(descriptorWaitedForPathOnlyLease,
			"a descriptor-backed foreground request bypassed a path-only lease for the same archive");
	}
	{
		jpegview_linux::SourceWorkCoordinator mixedIdentityCoordinator;
		jpegview_linux::SourceIdentity identity;
		identity.device = 93;
		identity.inode = 94;
		identity.valid = true;
		const fs::path member = temporary.path() / "mixed-paired-identity.zip" / "image.jpg";
		const jpegview_linux::SourceDescriptor descriptor(member, identity, {});
		const auto pathOnly = jpegview_linux::MakePathWorkContext(member,
			jpegview_linux::SourceWorkPriority::Speculative);
		auto pathLease = mixedIdentityCoordinator.AcquireSourceAndCpu(pathOnly, member);
		auto descriptorAdmission = std::async(std::launch::async,
			[&mixedIdentityCoordinator, descriptor] {
				return mixedIdentityCoordinator.AcquireSourceAndCpu(
					jpegview_linux::MakeWorkContext(descriptor,
						jpegview_linux::SourceWorkPriority::Foreground),
					descriptor.LogicalPath());
			});
		const bool descriptorWaitedForPathLease = mixedIdentityCoordinator.WaitForSnapshot(
			[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
				return snapshot.activeSpeculative == 1 && snapshot.activeForeground == 0 &&
					snapshot.waitingForeground == 1 && snapshot.activeCpu == 1 &&
					snapshot.waitingCpu == 1;
			}, std::chrono::seconds(2));
		pathLease.Reset();
		auto descriptorLease = descriptorAdmission.get();
		const bool descriptorAdmittedAfterPathRelease = static_cast<bool>(descriptorLease);
		descriptorLease.Reset();
		Expect(descriptorWaitedForPathLease && descriptorAdmittedAfterPathRelease,
			"paired descriptor admission bypassed a path-only lease for the same backing path");
	}

	// Distinct captured identities continue to represent distinct backing
	// sources even when a stale logical path happens to match.
	{
		jpegview_linux::SourceWorkCoordinator mismatchCoordinator;
		jpegview_linux::SourceIdentity firstIdentity;
		firstIdentity.device = 101;
		firstIdentity.inode = 102;
		firstIdentity.size = 103;
		firstIdentity.modifiedSeconds = 104;
		firstIdentity.valid = true;
		jpegview_linux::SourceIdentity secondIdentity = firstIdentity;
		++secondIdentity.inode;
		const fs::path logicalPath = temporary.path() / "replaced-container.zip" / "image.jpg";
		const jpegview_linux::SourceDescriptor firstDescriptor(logicalPath,
			firstIdentity, {});
		const jpegview_linux::SourceDescriptor secondDescriptor(logicalPath,
			secondIdentity, {});
		auto firstLease = mismatchCoordinator.Acquire(
			jpegview_linux::MakeWorkContext(firstDescriptor,
				jpegview_linux::SourceWorkPriority::Metadata), logicalPath);
		auto secondLease = mismatchCoordinator.Acquire(
			jpegview_linux::MakeWorkContext(secondDescriptor,
				jpegview_linux::SourceWorkPriority::Foreground), logicalPath);
		Expect(static_cast<bool>(firstLease) && static_cast<bool>(secondLease) &&
			mismatchCoordinator.Snapshot().activeSpeculative == 1 &&
			mismatchCoordinator.Snapshot().activeForeground == 1,
			"known, nonmatching backing identities were serialized by their fallback path");
	}
}

void TestArchiveSourceProbesRunUnderAdmission() {
	TemporaryDirectory temporary;
	const fs::path payload = temporary.path() / "probe-image.jpg";
	const fs::path archive = temporary.path() / "probe-archive.zip";
	WriteTinyImage(payload);
	WriteZipArchive(archive, {{"image.jpg", payload}});
	const fs::path member = archive / "image.jpg";
	ArchiveSourceProbeObservation observation;
	jpegview_linux::SetArchiveSourceProbeHookForTesting(
		ObserveArchiveSourceProbe, &observation);
	ArchiveSourceProbeHookReset resetHook;

	std::vector<jpegview_linux::ArchiveEntryInfo> entries;
	std::string error;
	jpegview_linux::ArchiveErrorKind errorKind = jpegview_linux::ArchiveErrorKind::None;
	Expect(jpegview_linux::ListArchiveDirectoryCancellable(archive, entries,
		[] { return true; }, error, &errorKind) && entries.size() == 1,
		"archive listing failed during source-probe admission coverage: " + error);
	jpegview_linux::ArchiveMemberInfo memberInfo;
	Expect(jpegview_linux::GetArchiveMemberInfo(member, memberInfo, error) &&
		memberInfo.size == fs::file_size(payload),
		"archive member metadata failed during source-probe admission coverage: " + error);
	Expect(jpegview_linux::ValidateArchivePassword(archive, "unused", error, &errorKind),
		"unencrypted archive password validation failed during admission coverage: " + error);
	const jpegview_linux::WorkContext passwordContext =
		jpegview_linux::MakePathWorkContext(member,
			jpegview_linux::SourceWorkPriority::Metadata);
	(void)jpegview_linux::HasSessionArchivePassword(member, passwordContext);

	jpegview_linux::DirectorySummaryLoader summaries;
	summaries.Request({archive}, 111);
	std::vector<jpegview_linux::DirectorySummaryResult> summaryResults;
	const auto summaryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (summaryResults.empty() && std::chrono::steady_clock::now() < summaryDeadline) {
		summaryResults = summaries.TakeReady();
		if (summaryResults.empty()) std::this_thread::yield();
	}

	jpegview_linux::FileDialogPreviewLoader previews;
	const std::uint64_t previewGeneration = previews.Request(archive, true,
		jpegview_linux::FileDialogSortMode::Name, 32, 32);
	std::vector<jpegview_linux::FileDialogPreviewResult> previewResults;
	const auto previewDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (previewResults.empty() && std::chrono::steady_clock::now() < previewDeadline) {
		previewResults = previews.TakeReady();
		if (previewResults.empty()) std::this_thread::yield();
	}

	FileListScanWorker scanner;
	const std::uint64_t scanGeneration = scanner.Request(FileList::InitialScanRequest(
		{archive.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	std::vector<FileListScanResult> scanResults;
	const auto scanDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (scanResults.empty() && std::chrono::steady_clock::now() < scanDeadline) {
		scanResults = scanner.TakeReady();
		if (scanResults.empty()) std::this_thread::yield();
	}

	const int classifications = observation.locationClassification.load();
	const int unadmittedClassifications =
		observation.unadmittedLocationClassification.load();
	const int passwordIdentityProbes = observation.passwordIdentity.load();
	const int unadmittedPasswordIdentityProbes =
		observation.unadmittedPasswordIdentity.load();
	std::ostringstream failure;
	failure << "archive source probes escaped admission (classification=" << classifications
		<< ", unadmittedClassification=" << unadmittedClassifications
		<< ", passwordIdentity=" << passwordIdentityProbes
		<< ", unadmittedPasswordIdentity=" << unadmittedPasswordIdentityProbes
		<< ", summaryResults=" << summaryResults.size()
		<< ", previewResults=" << previewResults.size()
		<< ", scanResults=" << scanResults.size() << ')';
	Expect(classifications > 0 && unadmittedClassifications == 0 &&
		passwordIdentityProbes > 0 && unadmittedPasswordIdentityProbes == 0 &&
		summaryResults.size() == 1 && summaryResults.front().generation == 111 &&
		previewResults.size() == 1 && previewResults.front().generation == previewGeneration &&
		scanResults.size() == 1 && scanResults.front().generation == scanGeneration,
		failure.str());
}

void TestArchivePasswordValidationYieldsForForegroundWork() {
	TemporaryDirectory temporary;
	const fs::path fixture = fs::path(__FILE__).parent_path() / "fixtures" /
		"encrypted-zip.zip";
	const fs::path archive = temporary.path() / "yield-password-validation.zip";
	Expect(fs::copy_file(fixture, archive),
		"could not copy the encrypted ZIP password-yield fixture");
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const bool coordinatorInitiallyIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingCpu == 0 &&
				!snapshot.foregroundPending;
		}, std::chrono::seconds(2));
	ArchiveSourceProbeBarrier barrier;
	jpegview_linux::SetArchiveSourceProbeHookForTesting(
		BlockArchiveClassificationProbe, &barrier);
	ArchiveSourceProbeHookReset resetHook;
	jpegview_linux::ArchiveDirectoryLoader loader;
	constexpr std::uint64_t validationGeneration = 541;
	loader.RequestPasswordValidation(archive, "jpegview-test-password",
		validationGeneration);
	bool validationReachedProbe = false;
	{
		std::unique_lock<std::mutex> lock(barrier.mutex);
		validationReachedProbe = barrier.changed.wait_for(lock, std::chrono::seconds(3), [&] {
			return barrier.entered;
		});
	}
	const auto validationSnapshot = coordinator.Snapshot();
	const bool validationHeldPairedMetadataAdmission = validationReachedProbe &&
		barrier.sourceAdmitted && barrier.cpuAdmitted &&
		validationSnapshot.activeSpeculative == 1 && validationSnapshot.activeCpu == 1;
	coordinator.SetForegroundPending(true);
	const fs::path visibleSource = temporary.path() / "visible-foreground-source.jpg";
	const jpegview_linux::WorkContext foregroundContext =
		jpegview_linux::MakePathWorkContext(visibleSource,
			jpegview_linux::SourceWorkPriority::Foreground);
	auto foregroundAdmission = std::async(std::launch::async,
		[&coordinator, foregroundContext, visibleSource] {
			return coordinator.AcquireSourceAndCpu(foregroundContext, visibleSource);
		});
	{
		std::lock_guard<std::mutex> lock(barrier.mutex);
		barrier.release = true;
	}
	barrier.changed.notify_all();
	const bool foregroundEnteredAfterValidationYield = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 1 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 1 && snapshot.foregroundPending;
		}, std::chrono::seconds(3));
	auto foregroundLease = foregroundAdmission.get();
	auto earlyResults = loader.TakeReady();
	const bool validationResultWithheld = earlyResults.empty();
	const bool foregroundLeaseHeld = static_cast<bool>(foregroundLease);
	foregroundLease.Reset();
	coordinator.SetForegroundPending(false);
	std::vector<jpegview_linux::ArchiveDirectoryResult> validationResults =
		std::move(earlyResults);
	const auto validationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (validationResults.empty() && std::chrono::steady_clock::now() < validationDeadline) {
		validationResults = loader.TakeReady();
		if (validationResults.empty()) std::this_thread::yield();
	}
	const bool validationRetriedWithSamePassword = validationResults.size() == 1 &&
		validationResults.front().generation == validationGeneration &&
		validationResults.front().passwordValidation && validationResults.front().error.empty();

	{
		std::lock_guard<std::mutex> lock(barrier.mutex);
		barrier.entered = false;
		barrier.release = false;
		barrier.sourceAdmitted = false;
		barrier.cpuAdmitted = false;
	}
	coordinator.SetForegroundPending(false);
	constexpr std::uint64_t canceledGeneration = validationGeneration + 1;
	loader.RequestPasswordValidation(archive, "jpegview-test-password", canceledGeneration);
	bool canceledValidationReachedProbe = false;
	{
		std::unique_lock<std::mutex> lock(barrier.mutex);
		canceledValidationReachedProbe = barrier.changed.wait_for(lock,
			std::chrono::seconds(3), [&] { return barrier.entered; });
	}
	coordinator.SetForegroundPending(true);
	{
		std::lock_guard<std::mutex> lock(barrier.mutex);
		barrier.release = true;
	}
	barrier.changed.notify_all();
	const bool canceledValidationYielded = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.foregroundPending;
		}, std::chrono::seconds(3));
	const auto staleResultsBeforeClear = loader.TakeReady();
	loader.Clear(canceledGeneration + 1);
	constexpr std::uint64_t replacementGeneration = canceledGeneration + 2;
	loader.RequestPasswordValidation(archive, "jpegview-test-password",
		replacementGeneration);
	coordinator.SetForegroundPending(false);
	std::vector<jpegview_linux::ArchiveDirectoryResult> replacementResults;
	const auto replacementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (replacementResults.empty() &&
		std::chrono::steady_clock::now() < replacementDeadline) {
		replacementResults = loader.TakeReady();
		if (replacementResults.empty()) std::this_thread::yield();
	}
	const bool cancellationDiscardedAndReplacementWorked = canceledValidationReachedProbe &&
		canceledValidationYielded && staleResultsBeforeClear.empty() &&
		replacementResults.size() == 1 &&
		replacementResults.front().generation == replacementGeneration &&
		replacementResults.front().error.empty();

	{
		std::lock_guard<std::mutex> lock(barrier.mutex);
		barrier.entered = false;
		barrier.release = false;
		barrier.sourceAdmitted = false;
		barrier.cpuAdmitted = false;
	}
	coordinator.SetForegroundPending(false);
	auto shutdownLoader = std::make_unique<jpegview_linux::ArchiveDirectoryLoader>();
	shutdownLoader->RequestPasswordValidation(archive, "jpegview-test-password",
		replacementGeneration + 1);
	bool shutdownValidationReachedProbe = false;
	{
		std::unique_lock<std::mutex> lock(barrier.mutex);
		shutdownValidationReachedProbe = barrier.changed.wait_for(lock,
			std::chrono::seconds(3), [&] { return barrier.entered; });
	}
	coordinator.SetForegroundPending(true);
	{
		std::lock_guard<std::mutex> lock(barrier.mutex);
		barrier.release = true;
	}
	barrier.changed.notify_all();
	const bool shutdownValidationYielded = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.foregroundPending;
		}, std::chrono::seconds(3));
	std::promise<void> shutdownComplete;
	auto shutdownFinished = shutdownComplete.get_future();
	std::thread shutdownThread([owned = std::move(shutdownLoader),
		&shutdownComplete]() mutable {
		owned->Shutdown();
		owned.reset();
		shutdownComplete.set_value();
	});
	const bool shutdownWokeForegroundWait =
		shutdownFinished.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	if (!shutdownWokeForegroundWait) {
		coordinator.SetForegroundPending(false);
		coordinator.NotifyWaiters();
	}
	shutdownThread.join();
	coordinator.SetForegroundPending(false);
	std::ostringstream failure;
	failure << "password validation failed to yield and retry around foreground image work"
		<< " (coordinatorIdle=" << coordinatorInitiallyIdle
		<< ", reachedProbe=" << validationReachedProbe
		<< ", pairedAdmission=" << validationHeldPairedMetadataAdmission
		<< ", foregroundEntered=" << foregroundEnteredAfterValidationYield
		<< ", foregroundLease=" << foregroundLeaseHeld
		<< ", resultWithheld=" << validationResultWithheld
		<< ", retriedPassword=" << validationRetriedWithSamePassword
		<< ", cancelProbe=" << canceledValidationReachedProbe
		<< ", cancelYield=" << canceledValidationYielded
		<< ", replacement=" << cancellationDiscardedAndReplacementWorked
		<< ", shutdownProbe=" << shutdownValidationReachedProbe
		<< ", shutdownYield=" << shutdownValidationYielded
		<< ", shutdownWoke=" << shutdownWokeForegroundWait << ')';
	Expect(coordinatorInitiallyIdle && validationReachedProbe &&
		validationHeldPairedMetadataAdmission && foregroundEnteredAfterValidationYield &&
		foregroundLeaseHeld && validationResultWithheld && validationRetriedWithSamePassword &&
		cancellationDiscardedAndReplacementWorked && shutdownValidationReachedProbe &&
		shutdownValidationYielded && shutdownWokeForegroundWait,
		failure.str());
}

void TestFileListAsynchronousScanning() {
	TemporaryDirectory temporary;
	const fs::path root = temporary.path() / "root";
	const fs::path child = root / "01-child";
	const fs::path latest = temporary.path() / "latest";
	fs::create_directories(child);
	fs::create_directories(latest);
	WriteTinyImage(root / "root.png");
	WriteTinyImage(root / "root2.png");
	WriteTinyImage(child / "child.png");
	WriteTinyImage(child / "z-child.png");
	WriteTinyImage(latest / "latest.png");

	FileList list({root.string()}, FileList::SortMode::FileName, true, false);
	list.SetNavigationMode(FileList::NavigationMode::LoopSubDirectories);
	list.First();
	Expect(list.NextLoaded() == FileList::LoadedNavigationResult::Moved &&
		list.Current().filename() == "root2.png",
		"moving within the loaded directory did not use the immediate index-only navigation path");
	list.Last();
	Expect(list.NextLoaded() == FileList::LoadedNavigationResult::NeedsDirectoryScan,
		"the loaded-list hot path did not defer recursive folder entry to the scanner");
	const FileList::ScanRequest boundaryRequest = list.MakeScanRequest(
		FileList::ScanOperation::ForwardBoundary, 1);
	FileListPreparedScan boundary = FileList::PrepareScan(boundaryRequest, [] { return true; });
	Expect(boundary.completed && boundary.targetFound &&
		boundary.replacement.Current().filename() == "child.png",
		"the worker did not prepare the first populated child directory");
	const std::size_t oldIndex = list.CurrentIndex();
	Expect(list.ApplyPreparedScan(std::move(boundary), list.Current()) &&
		list.Current().filename() == "child.png",
		"the prepared boundary result did not select its first image in the new folder");
	const fs::path secondChild = root / "02-child";
	fs::create_directories(secondChild);
	WriteTinyImage(secondChild / "second-child.png");
	const FileList::ScanRequest nextBoundaryRequest = list.MakeScanRequest(
		FileList::ScanOperation::ForwardBoundary, 1);
	FileListPreparedScan nextBoundary = FileList::PrepareScan(nextBoundaryRequest, [] { return true; });
	Expect(nextBoundary.completed && nextBoundary.targetFound &&
		nextBoundary.replacement.Current().filename() == "second-child.png",
		"recursive navigation lost its original root after entering the first child");
	Expect(list.ApplyPreparedScan(std::move(nextBoundary), list.Current()) &&
		list.Current().filename() == "second-child.png",
		"the second recursive folder transition was not applied");
	Expect(list.PreviousLoaded() == FileList::LoadedNavigationResult::Moved &&
		list.Current().filename() == "child.png",
		"reverse traversal did not restore the immediately previous recursive folder");
	Expect(list.PreviousLoaded() == FileList::LoadedNavigationResult::Moved &&
		list.Current().filename() == "root2.png" && list.CurrentIndex() == oldIndex,
		"returning from an asynchronously entered folder did not restore its active entry and index");

	const fs::path siblingRoot = temporary.path() / "sibling-root";
	const fs::path previousSibling = siblingRoot / "01-previous";
	const fs::path currentSibling = siblingRoot / "02-current";
	fs::create_directories(previousSibling);
	fs::create_directories(currentSibling);
	WriteTinyImage(previousSibling / "a-first.png");
	WriteTinyImage(previousSibling / "z-last.png");
	WriteTinyImage(currentSibling / "current.png");
	FileList siblings({currentSibling.string()}, FileList::SortMode::FileName, true, false);
	const fs::path oldSiblingSelection = siblings.Current();
	FileListPreparedScan previousSiblingScan = FileList::PrepareScan(
		siblings.MakeScanRequest(FileList::ScanOperation::PreviousSibling, -1),
		[] { return true; });
	Expect(previousSiblingScan.completed && previousSiblingScan.targetFound &&
		previousSiblingScan.replacement.Current().filename() == "a-first.png" &&
		siblings.ApplyPreparedScan(std::move(previousSiblingScan), oldSiblingSelection) &&
		siblings.Current() == previousSibling / "a-first.png",
		"an async previous-sibling transition preserved the old path instead of selecting the first image");
	FileListPreparedScan nextSiblingScan = FileList::PrepareScan(
		siblings.MakeScanRequest(FileList::ScanOperation::NextSibling, 1),
		[] { return true; });
	Expect(nextSiblingScan.completed && nextSiblingScan.targetFound &&
		nextSiblingScan.replacement.Current().filename() == "current.png" &&
		siblings.ApplyPreparedScan(std::move(nextSiblingScan), siblings.Current()) &&
		siblings.Current() == currentSibling / "current.png",
		"an async next-sibling transition did not select the first image in its target folder");

	FileList provisional;
	provisional.SetProvisionalInputs({(root / "root.png").string(),
		(secondChild / "second-child.png").string()});
	Expect(provisional.Empty(),
		"multiple explicit startup inputs were provisionally reduced to only the first image");

	FileList configurableStartup;
	configurableStartup.SetProvisionalInputs({root.string()});
	FileListPreparedScan startupScan = FileList::PrepareScan(
		configurableStartup.MakeScanRequest(FileList::ScanOperation::Initialize),
		[] { return true; });
	configurableStartup.SetWrapAroundFolder(false);
	Expect(startupScan.completed && configurableStartup.ApplyPreparedScan(std::move(startupScan)) &&
		!configurableStartup.WrapAroundFolder(),
		"applying an in-flight startup scan overwrote a changed folder-wrap preference");
	configurableStartup.Last();
	Expect(configurableStartup.NextLoaded() == FileList::LoadedNavigationResult::NoMove,
		"startup scan did not apply the current folder-wrap preference to active navigation");

	const FileList::ScanRequest canceledRequest = list.MakeScanRequest(FileList::ScanOperation::Reload);
	std::size_t cancellationChecks = 0;
	FileListPreparedScan canceled = FileList::PrepareScan(canceledRequest, [&cancellationChecks] {
		return ++cancellationChecks < 4;
	});
	Expect(!canceled.completed,
		"a scan canceled during enumeration returned a publishable partial list");

	list.Select(0);
	const FileList::ScanRequest reloadRequest = list.MakeScanRequest(FileList::ScanOperation::Reload);
	FileListPreparedScan reload = FileList::PrepareScan(reloadRequest, [] { return true; });
	list.Select(list.Size() - 1);
	const fs::path selectedAtCompletion = list.Current();
	Expect(list.ApplyPreparedScan(std::move(reload), selectedAtCompletion) &&
		list.Current() == selectedAtCompletion,
		"a reload did not preserve navigation performed while its scan was running");

	const FileList::ScanRequest staleRequest = list.MakeScanRequest(FileList::ScanOperation::Reload);
	FileListPreparedScan stale = FileList::PrepareScan(staleRequest, [] { return true; });
	list.SetSorting(FileList::SortMode::FileName, false);
	Expect(!list.ApplyPreparedScan(std::move(stale), list.Current()),
		"a result prepared for a previous list/sort revision replaced the current list");

	FileListScanWorker worker;
	const std::uint64_t superseded = worker.Request(FileList::InitialScanRequest(
		{root.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	const std::uint64_t current = worker.Request(FileList::InitialScanRequest(
		{latest.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	Expect(current > superseded, "a newer scan did not receive a fresh generation identity");
	std::vector<FileListScanResult> results;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < deadline) {
		results = worker.TakeReady();
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	Expect(results.size() == 1 && results.front().generation == current &&
		results.front().prepared.replacement.Current().filename() == "latest.png",
		"a superseded directory scan published stale files instead of the latest request");

	worker.SetForegroundPending(true);
	const std::uint64_t yieldedGeneration = worker.Request(FileList::InitialScanRequest(
		{root.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	const auto yieldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!worker.IsYieldingForForeground() &&
		std::chrono::steady_clock::now() < yieldDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool yieldedForForeground = worker.IsYieldingForForeground();
	const bool withheldResultWhileForegroundPending = worker.TakeReady().empty();
	worker.SetForegroundPending(false);
	results.clear();
	const auto resumeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < resumeDeadline) {
		results = worker.TakeReady();
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(yieldedForForeground && withheldResultWhileForegroundPending,
		"the directory scan did not yield before publishing while foreground work was pending");
	Expect(results.size() == 1 && results.front().generation == yieldedGeneration &&
		results.front().prepared.completed,
		"the directory scan did not resume after foreground work completed");

	// Hold the metadata lane until the scanner has queued its first entry, then
	// interrupt that source read and clear the gate before scan postchecks run.
	// A canceled DescribeFile can leave the path in a completed-looking list
	// without its captured source metadata unless the worker retries the attempt.
	const fs::path scanAdmissionBlockerPath = temporary.path() / "scan-admission-blocker.png";
	WriteText(scanAdmissionBlockerPath, "admission blocker");
	auto& sourceCoordinator = jpegview_linux::SourceWorkCoordinator::Global();
	sourceCoordinator.SetForegroundPending(false);
	const bool scanCoordinatorIdle = sourceCoordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(2));
	const jpegview_linux::WorkContext scanAdmissionBlockerContext =
		jpegview_linux::MakePathWorkContext(scanAdmissionBlockerPath,
			jpegview_linux::SourceWorkPriority::Metadata);
	jpegview_linux::SourceWorkLease scanAdmissionBlocker = sourceCoordinator.Acquire(
		scanAdmissionBlockerContext, scanAdmissionBlockerPath);
	const bool scanAdmissionBlockerAcquired = static_cast<bool>(scanAdmissionBlocker);
	const bool scanBlockerExclusive = scanAdmissionBlockerAcquired &&
		sourceCoordinator.Snapshot().activeSpeculative == 1 &&
		sourceCoordinator.Snapshot().waitingSpeculative == 0;
	FileListScanWorker interruptedDuringEntry;
	const std::uint64_t interruptedGeneration = interruptedDuringEntry.Request(
		FileList::InitialScanRequest({root.string()}, FileList::SortMode::FileName,
			true, false, FileList::NavigationMode::LoopDirectory));
	const bool scanQueuedForMetadataLane = sourceCoordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 1 && snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	interruptedDuringEntry.SetForegroundPending(true);
	sourceCoordinator.SetForegroundPending(true);
	scanAdmissionBlocker.Reset();
	const auto interruptedYieldDeadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!interruptedDuringEntry.IsYieldingForForeground() &&
		std::chrono::steady_clock::now() < interruptedYieldDeadline) {
		std::this_thread::yield();
	}
	const bool scanInterruptedInsideEntry = interruptedDuringEntry.IsYieldingForForeground();
	interruptedDuringEntry.SetForegroundPending(false);
	sourceCoordinator.SetForegroundPending(false);
	std::vector<FileListScanResult> interruptedResults;
	const auto interruptedResultDeadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (interruptedResults.empty() &&
		std::chrono::steady_clock::now() < interruptedResultDeadline) {
		interruptedResults = interruptedDuringEntry.TakeReady();
		if (interruptedResults.empty()) std::this_thread::yield();
	}
	bool allInterruptedScanDescriptorsCaptured = interruptedResults.size() == 1 &&
		interruptedResults.front().generation == interruptedGeneration &&
		interruptedResults.front().prepared.completed &&
		interruptedResults.front().prepared.replacement.Size() == 2;
	if (allInterruptedScanDescriptorsCaptured) {
		for (std::size_t index = 0;
			index < interruptedResults.front().prepared.replacement.Size(); ++index) {
			const jpegview_linux::SourceDescriptor* descriptor =
				interruptedResults.front().prepared.replacement.DescriptorAt(index);
			allInterruptedScanDescriptorsCaptured = allInterruptedScanDescriptorsCaptured &&
				descriptor != nullptr && descriptor->Valid() &&
				descriptor->Metadata().hasFileSize &&
				descriptor->Metadata().hasModificationTime;
		}
	}
	std::ostringstream scanFailure;
	scanFailure << "a foreground-interrupted directory scan published entries with missing "
		"source metadata (idle=" << scanCoordinatorIdle << ", blocker=" << scanBlockerExclusive
		<< ", queued=" << scanQueuedForMetadataLane << ", yielded=" << scanInterruptedInsideEntry
		<< ", results=" << interruptedResults.size() << ", captured="
		<< allInterruptedScanDescriptorsCaptured;
	if (interruptedResults.size() == 1) {
		scanFailure << ", complete=" << interruptedResults.front().prepared.completed
			<< ", entries=" << interruptedResults.front().prepared.replacement.Size();
		for (std::size_t index = 0;
			index < interruptedResults.front().prepared.replacement.Size(); ++index) {
			const auto* descriptor =
				interruptedResults.front().prepared.replacement.DescriptorAt(index);
			scanFailure << ", descriptor[" << index << "]="
				<< (descriptor != nullptr && descriptor->Valid()) << '/'
				<< (descriptor != nullptr && descriptor->Metadata().hasFileSize) << '/'
				<< (descriptor != nullptr && descriptor->Metadata().hasModificationTime);
		}
	}
	Expect(scanCoordinatorIdle && scanAdmissionBlockerAcquired && scanBlockerExclusive &&
		scanQueuedForMetadataLane && scanInterruptedInsideEntry &&
		allInterruptedScanDescriptorsCaptured, scanFailure.str());

	const fs::path classifiedInput = temporary.path() / "classification-race.png";
	const fs::path absentInput = temporary.path() / "classification-absent.png";
	const fs::path classificationBlockerPath =
		temporary.path() / "classification-admission-blocker.jpg";
	WriteTinyImage(classifiedInput);
	WriteText(classificationBlockerPath, "classification admission blocker");
	const bool classificationCoordinatorIdle = sourceCoordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(2));
	const jpegview_linux::WorkContext classificationBlockerContext =
		jpegview_linux::MakePathWorkContext(classificationBlockerPath,
			jpegview_linux::SourceWorkPriority::Metadata);
	jpegview_linux::SourceWorkLease classificationBlocker = sourceCoordinator.Acquire(
		classificationBlockerContext, classificationBlockerPath);
	FileListScanWorker classificationWorker;
	const std::uint64_t classificationGeneration = classificationWorker.Request(
		FileList::InitialScanRequest({classifiedInput.string(), absentInput.string()},
			FileList::SortMode::FileName, true, false,
			FileList::NavigationMode::LoopDirectory));
	const bool classificationWaitedForAdmission = sourceCoordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 1 && snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	std::error_code removeClassifiedError;
	(void)fs::remove(classifiedInput, removeClassifiedError);
	classificationBlocker.Reset();
	std::vector<FileListScanResult> classificationResults;
	const auto classificationDeadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (classificationResults.empty() &&
		std::chrono::steady_clock::now() < classificationDeadline) {
		classificationResults = classificationWorker.TakeReady();
		if (classificationResults.empty()) std::this_thread::yield();
	}
	Expect(classificationCoordinatorIdle && classificationWaitedForAdmission &&
		!removeClassifiedError && classificationResults.size() == 1 &&
		classificationResults.front().generation == classificationGeneration &&
		classificationResults.front().prepared.completed &&
		classificationResults.front().prepared.replacement.Empty(),
		"initial file-list path classification probed filesystem state before metadata admission");

	const auto waitForYield = [](FileListScanWorker& scanWorker) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!scanWorker.IsYieldingForForeground() &&
			std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return scanWorker.IsYieldingForForeground();
	};
	FileListScanWorker replacementWhileYielding;
	replacementWhileYielding.SetForegroundPending(true);
	replacementWhileYielding.Request(FileList::InitialScanRequest(
		{root.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	const bool oldGenerationYielded = waitForYield(replacementWhileYielding);
	const std::uint64_t replacementGeneration = replacementWhileYielding.Request(
		FileList::InitialScanRequest({latest.string()}, FileList::SortMode::FileName,
			true, false, FileList::NavigationMode::LoopDirectory));
	replacementWhileYielding.SetForegroundPending(false);
	std::vector<FileListScanResult> replacementResults;
	const auto replacementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (replacementResults.empty() &&
		std::chrono::steady_clock::now() < replacementDeadline) {
		replacementResults = replacementWhileYielding.TakeReady();
		if (replacementResults.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(oldGenerationYielded && replacementResults.size() == 1 &&
		replacementResults.front().generation == replacementGeneration &&
		replacementResults.front().prepared.replacement.Current().filename() == "latest.png",
		"a replacement scan did not supersede a generation paused for foreground work");

	FileListScanWorker canceledWhileYielding;
	canceledWhileYielding.SetForegroundPending(true);
	canceledWhileYielding.Request(FileList::InitialScanRequest(
		{root.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	const bool canceledGenerationYielded = waitForYield(canceledWhileYielding);
	canceledWhileYielding.Clear();
	const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (canceledWhileYielding.IsYieldingForForeground() &&
		std::chrono::steady_clock::now() < cancelDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool clearWokeYieldedWorker = !canceledWhileYielding.IsYieldingForForeground();
	canceledWhileYielding.SetForegroundPending(false);
	Expect(canceledGenerationYielded && clearWokeYieldedWorker &&
		canceledWhileYielding.TakeReady().empty(),
		"clearing a foreground-yielded scan did not wake and cancel its generation");

	FileListScanWorker stoppingWhileYielding;
	stoppingWhileYielding.SetForegroundPending(true);
	stoppingWhileYielding.Request(FileList::InitialScanRequest(
		{root.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	const bool shutdownGenerationYielded = waitForYield(stoppingWhileYielding);
	std::mutex shutdownMutex;
	std::condition_variable shutdownChanged;
	bool shutdownReturned = false;
	std::thread shutdownThread([&] {
		stoppingWhileYielding.Stop();
		{
			std::lock_guard<std::mutex> lock(shutdownMutex);
			shutdownReturned = true;
		}
		shutdownChanged.notify_all();
	});
	bool shutdownBeforeCleanup = false;
	{
		std::unique_lock<std::mutex> lock(shutdownMutex);
		shutdownBeforeCleanup = shutdownChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return shutdownReturned; });
	}
	if (!shutdownBeforeCleanup) stoppingWhileYielding.SetForegroundPending(false);
	shutdownThread.join();
	Expect(shutdownGenerationYielded && shutdownBeforeCleanup,
		"shutdown did not release and join a worker paused for foreground activity");
}

void TestArchiveTraversalYieldReleasesAdmission() {
	constexpr std::size_t memberCount = 60000;
	TemporaryDirectory temporary;
	const fs::path causeArchive = temporary.path() / "cause-members.zip";
	const fs::path scanArchive = temporary.path() / "scan-members.zip";
	const fs::path summaryArchive = temporary.path() / "summary-members.zip";
	WriteZipArchiveWithImageMembers(causeArchive, 8);
	WriteZipArchiveWithImageMembers(scanArchive, memberCount);
	WriteZipArchiveWithImageMembers(summaryArchive, memberCount);
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const bool causeCoordinatorIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingForeground == 0 &&
				snapshot.waitingSpeculative == 0 && snapshot.waitingCpu == 0 &&
				!snapshot.foregroundPending;
		}, std::chrono::seconds(3));
	bool injectedForegroundAtArchiveCheck = false;
	bool archiveYieldCauseCaptured = false;
	jpegview_linux::WorkContext causeContext;
	causeContext.sourcePriority = jpegview_linux::SourceWorkPriority::Metadata;
	causeContext.shouldContinue = [&] {
		const jpegview_linux::SourceWorkSnapshot snapshot = coordinator.Snapshot();
		if (!injectedForegroundAtArchiveCheck && snapshot.activeSpeculative == 1 &&
			snapshot.activeCpu == 1) {
			injectedForegroundAtArchiveCheck = true;
			coordinator.SetForegroundPending(true);
		}
		return true;
	};
	causeContext.onForegroundYield = [&] { archiveYieldCauseCaptured = true; };
	std::vector<jpegview_linux::ArchiveEntryInfo> causeEntries;
	std::string causeError;
	bool causeListingCompleted = false;
	{
		jpegview_linux::ScopedWorkContext activeContext(causeContext);
		causeListingCompleted = jpegview_linux::ListArchiveDirectoryCancellable(causeArchive,
			causeEntries, std::function<bool()>{}, causeError);
	}
	const bool causeAdmissionReleased = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 0 && snapshot.activeCpu == 0;
		}, std::chrono::seconds(3));
	coordinator.SetForegroundPending(false);
	Expect(causeCoordinatorIdle && injectedForegroundAtArchiveCheck &&
		archiveYieldCauseCaptured && !causeListingCompleted && causeEntries.empty() &&
		causeAdmissionReleased && causeError.find("cancelled") != std::string::npos,
		"the archive wrapper lost a foreground interruption raised between its caller check and internal gate check");
	const auto waitForYield = [](const auto& worker) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!worker.IsYieldingForForeground() &&
			std::chrono::steady_clock::now() < deadline) {
			std::this_thread::yield();
		}
		return worker.IsYieldingForForeground();
	};
	const auto waitForCompleteScan = [memberCount](FileListScanWorker& worker,
		std::uint64_t generation) {
		std::vector<FileListScanResult> results;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
		while (results.empty() && std::chrono::steady_clock::now() < deadline) {
			results = worker.TakeReady();
			if (results.empty()) std::this_thread::yield();
		}
		if (results.size() != 1 || results.front().generation != generation ||
			!results.front().prepared.completed ||
			results.front().prepared.replacement.Size() != memberCount) return false;
		const auto* first = results.front().prepared.replacement.DescriptorAt(0);
		const auto* last = results.front().prepared.replacement.DescriptorAt(memberCount - 1);
		return first != nullptr && first->Valid() && last != nullptr && last->Valid() &&
			first->LogicalPath().filename() == "image-0.jpg" &&
			last->LogicalPath().filename() == "image-59999.jpg";
	};
	const auto startForegroundAdmission = [&coordinator](const fs::path& archive,
		const std::shared_ptr<std::atomic<bool>>& cancel) {
		jpegview_linux::WorkContext context = jpegview_linux::MakePathWorkContext(archive,
			jpegview_linux::SourceWorkPriority::Foreground,
			[cancel] { return !cancel->load(); });
		return std::async(std::launch::async, [&coordinator, archive, context] {
			return coordinator.Acquire(context, archive);
		});
	};

	const bool scanCoordinatorIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingForeground == 0 &&
				snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(3));
	FileListScanWorker scanner;
	const std::uint64_t scanGeneration = scanner.Request(FileList::InitialScanRequest(
		{scanArchive.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	const bool scanEnteredArchive = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 1 && snapshot.activeCpu == 1;
		}, std::chrono::seconds(3));
	coordinator.SetForegroundPending(true);
	auto scanAdmissionCancelled = std::make_shared<std::atomic<bool>>(false);
	auto scanForeground = startForegroundAdmission(scanArchive, scanAdmissionCancelled);
	const bool scanForegroundQueued = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.waitingForeground == 1 || snapshot.activeForeground == 1;
		}, std::chrono::seconds(3));
	const bool scanYielded = waitForYield(scanner);
	const bool scanForegroundAdmittedBeforeClear = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 1 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0;
		}, std::chrono::seconds(3));
	const auto scanGateDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(3);
	bool scanGateStayedSet = false;
	while (std::chrono::steady_clock::now() < scanGateDeadline) {
		const auto snapshot = coordinator.Snapshot();
		if (snapshot.foregroundPending && scanner.IsYieldingForForeground()) {
			scanGateStayedSet = true;
			break;
		}
		std::this_thread::yield();
	}
	const bool scanResultWithheld = scanner.TakeReady().empty();
	if (!scanForegroundAdmittedBeforeClear) {
		coordinator.SetForegroundPending(false);
		scanAdmissionCancelled->store(true);
		coordinator.NotifyWaiters();
	}
	jpegview_linux::SourceWorkLease scanForegroundLease = scanForeground.get();
	const bool scanForegroundLeaseAcquired = static_cast<bool>(scanForegroundLease);
	scanForegroundLease.Reset();
	coordinator.SetForegroundPending(false);
	const bool scanRepublishedComplete = waitForCompleteScan(scanner, scanGeneration);

	const bool summaryCoordinatorIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingForeground == 0 &&
				snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(3));
	jpegview_linux::DirectorySummaryLoader summaryLoader;
	const std::uint64_t summaryGeneration = 4401;
	summaryLoader.Request({summaryArchive}, summaryGeneration);
	const bool summaryEnteredArchive = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 1 && snapshot.activeCpu == 1;
		}, std::chrono::seconds(3));
	coordinator.SetForegroundPending(true);
	auto summaryAdmissionCancelled = std::make_shared<std::atomic<bool>>(false);
	auto summaryForeground = startForegroundAdmission(summaryArchive,
		summaryAdmissionCancelled);
	const bool summaryForegroundQueued = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.waitingForeground == 1 || snapshot.activeForeground == 1;
		}, std::chrono::seconds(3));
	const bool summaryYielded = waitForYield(summaryLoader);
	const bool summaryForegroundAdmittedBeforeClear = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 1 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0;
		}, std::chrono::seconds(3));
	const auto summaryGateDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(3);
	bool summaryGateStayedSet = false;
	while (std::chrono::steady_clock::now() < summaryGateDeadline) {
		const auto snapshot = coordinator.Snapshot();
		if (snapshot.foregroundPending && summaryLoader.IsYieldingForForeground()) {
			summaryGateStayedSet = true;
			break;
		}
		std::this_thread::yield();
	}
	const bool summaryResultWithheld = summaryLoader.TakeReady().empty();
	if (!summaryForegroundAdmittedBeforeClear) {
		coordinator.SetForegroundPending(false);
		summaryAdmissionCancelled->store(true);
		coordinator.NotifyWaiters();
	}
	jpegview_linux::SourceWorkLease summaryForegroundLease = summaryForeground.get();
	const bool summaryForegroundLeaseAcquired = static_cast<bool>(summaryForegroundLease);
	summaryForegroundLease.Reset();
	coordinator.SetForegroundPending(false);
	std::vector<jpegview_linux::DirectorySummaryResult> summaryResults;
	const auto summaryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	while (summaryResults.empty() && std::chrono::steady_clock::now() < summaryDeadline) {
		summaryResults = summaryLoader.TakeReady();
		if (summaryResults.empty()) std::this_thread::yield();
	}
	const bool summaryRepublishedComplete = summaryResults.size() == 1 &&
		summaryResults.front().generation == summaryGeneration &&
		!summaryResults.front().failure.Failed() &&
		summaryResults.front().summary.imageCount == memberCount;

	std::ostringstream message;
	message << "foreground admission waited for an interrupted archive traversal to release its "
		"paired lease (scan idle/entered/yielded/queued/admitted/gate/withheld/lease/complete=" <<
		scanCoordinatorIdle << '/' << scanEnteredArchive << '/' << scanYielded << '/' <<
		scanForegroundQueued << '/' <<
		scanForegroundAdmittedBeforeClear << '/' << scanGateStayedSet << '/' <<
		scanResultWithheld << '/' << scanForegroundLeaseAcquired << '/' << scanRepublishedComplete <<
		", summary idle/entered/yielded/queued/admitted/gate/withheld/lease/complete=" <<
		summaryCoordinatorIdle << '/' << summaryEnteredArchive << '/' << summaryYielded << '/' <<
		summaryForegroundQueued << '/' << summaryForegroundAdmittedBeforeClear << '/' <<
		summaryGateStayedSet << '/' << summaryResultWithheld << '/' <<
		summaryForegroundLeaseAcquired << '/' << summaryRepublishedComplete;
	Expect(scanCoordinatorIdle && scanEnteredArchive && scanYielded && scanForegroundQueued &&
		scanForegroundAdmittedBeforeClear && scanGateStayedSet && scanResultWithheld &&
		scanForegroundLeaseAcquired && scanRepublishedComplete && summaryCoordinatorIdle &&
		summaryEnteredArchive && summaryYielded && summaryForegroundQueued &&
		summaryForegroundAdmittedBeforeClear && summaryGateStayedSet &&
		summaryResultWithheld && summaryForegroundLeaseAcquired && summaryRepublishedComplete,
		message.str());
}

void TestDirectoryEnumerationAdmissionYieldsAndRetries() {
	TemporaryDirectory temporary;
	const fs::path emptyDirectory = temporary.path() / "empty-directory";
	fs::create_directories(emptyDirectory);
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const bool idleBeforeBlocker = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingForeground == 0 &&
				snapshot.waitingSpeculative == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(3));
	jpegview_linux::SourceIdentity blockerIdentity;
	blockerIdentity.device = 991;
	blockerIdentity.inode = 992;
	blockerIdentity.size = 1;
	blockerIdentity.valid = true;
	jpegview_linux::WorkContext blockerContext;
	blockerContext.source = jpegview_linux::SourceKey("/step09/directory-blocker",
		blockerIdentity);
	blockerContext.sourcePriority = jpegview_linux::SourceWorkPriority::Speculative;
	jpegview_linux::SourceWorkLease blocker = coordinator.Acquire(blockerContext);

	FileListScanWorker scanner;
	const std::uint64_t generation = scanner.Request(FileList::InitialScanRequest(
		{emptyDirectory.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory));
	const bool enumerationQueued = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	coordinator.SetForegroundPending(true);
	const bool enumerationYielded = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.waitingSpeculative == 0 && snapshot.activeSpeculative == 1 &&
				snapshot.foregroundPending;
		}, std::chrono::seconds(2)) && scanner.IsYieldingForForeground();
	const bool noScanPublishedDuringForeground = scanner.TakeReady().empty();
	blocker.Reset();
	coordinator.SetForegroundPending(false);
	std::vector<FileListScanResult> scanResults;
	const auto scanDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (scanResults.empty() && std::chrono::steady_clock::now() < scanDeadline) {
		scanResults = scanner.TakeReady();
		if (scanResults.empty()) std::this_thread::yield();
	}
	const bool scanRetried = scanResults.size() == 1 &&
		scanResults.front().generation == generation &&
		scanResults.front().prepared.completed &&
		scanResults.front().prepared.replacement.Empty();

	const bool idleBeforePreviewBlocker = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingForeground == 0 &&
				snapshot.waitingSpeculative == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(3));
	blocker = coordinator.Acquire(blockerContext);
	jpegview_linux::FileDialogPreviewLoader preview;
	const std::uint64_t previewGeneration = preview.Request(emptyDirectory, true,
		jpegview_linux::FileDialogSortMode::Name, 64, 64);
	const bool previewEnumerationQueued = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	coordinator.SetForegroundPending(true);
	const bool previewYielded = previewEnumerationQueued && coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.waitingSpeculative == 0 && snapshot.activeSpeculative == 1 &&
				snapshot.foregroundPending;
		}, std::chrono::seconds(2));
	const bool noPreviewPublishedDuringForeground = preview.TakeReady().empty();
	blocker.Reset();
	coordinator.SetForegroundPending(false);
	std::vector<jpegview_linux::FileDialogPreviewResult> previewResults;
	const auto previewDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (previewResults.empty() && std::chrono::steady_clock::now() < previewDeadline) {
		previewResults = preview.TakeReady();
		if (previewResults.empty()) std::this_thread::yield();
	}
	const bool previewRetried = previewResults.size() == 1 &&
		previewResults.front().generation == previewGeneration &&
		previewResults.front().error == "No images in this folder";

	coordinator.SetForegroundPending(false);
	std::ostringstream diagnostic;
	diagnostic << "directory admission/yield checks failed (idle/scan-queued/scan-yielded/"
		"scan-withheld/scan-retried/preview-idle/preview-queued/preview-yielded/"
		"preview-withheld/preview-retried=" << idleBeforeBlocker << '/' <<
		enumerationQueued << '/' << enumerationYielded << '/' <<
		noScanPublishedDuringForeground << '/' << scanRetried << '/' <<
		idleBeforePreviewBlocker << '/' << previewEnumerationQueued << '/' <<
		previewYielded << '/' << noPreviewPublishedDuringForeground << '/' << previewRetried;
	Expect(idleBeforeBlocker && enumerationQueued && enumerationYielded &&
		noScanPublishedDuringForeground && scanRetried && idleBeforePreviewBlocker &&
		previewEnumerationQueued && previewYielded && noPreviewPublishedDuringForeground &&
		previewRetried, diagnostic.str());
}

void TestArchivePrefetchPlannerKeepsAdmittedContext() {
	TemporaryDirectory temporary;
	const fs::path archive = temporary.path() / "planner-context.zip";
	WriteZipArchiveWithImageMembers(archive, 1);
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(archive / "image-0.jpg");
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const bool idleBeforeTest = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingForeground == 0 &&
				snapshot.waitingSpeculative == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(3));
	SourceWorkRegistrationCounter registrations;
	coordinator.SetTestHookForTesting(ThrowOnNestedSourceRegistration, &registrations);
	jpegview_linux::DisplayPrefetchPlannerWorker planner(
		[](const jpegview_linux::SourceDescriptor&, int& width, int& height,
			std::string&, const jpegview_linux::DisplayPrefetchPlannerWorker::Continue& keepGoing) {
			if (!keepGoing()) return false;
			width = 640;
			height = 480;
			return keepGoing();
		});
	jpegview_linux::DisplayPrefetchPlannerRequest request;
	request.catalogRevision = 1;
	request.descriptorRevision = 1;
	request.viewportRevision = 1;
	request.pageCount = 1;
	request.imageAreaWidth = 640;
	request.imageAreaHeight = 480;
	request.maximumCount = 1;
	request.neighbors.push_back({source.LogicalPath(), source, 0, 1, {}, false, true});
	const std::uint64_t generation = planner.Request(request);
	const bool idle = planner.WaitUntilIdle(std::chrono::seconds(3));
	coordinator.SetTestHookForTesting(nullptr, nullptr);
	const std::vector<jpegview_linux::DisplayPrefetchPlannerResult> results =
		planner.TakeReady();
	const bool planned = results.size() == 1 && results.front().generation == generation &&
		!results.front().failure.Failed() && results.front().dimensions.size() == 1 &&
		results.front().dimensions.front().width == 640 &&
		results.front().dimensions.front().height == 480;
	Expect(idleBeforeTest && idle && source.Valid() && planned &&
		registrations.sourceRegistrations.load() == 1 &&
		!registrations.threwOnNestedRegistration.load(),
		"archive JPEG dimension planning attempted a second admission while its paired "
		"source and CPU lease was still held");
}

void TestArchiveDecodedCachePromotionCompletesDuringForegroundGate() {
	TemporaryDirectory temporary;
	const fs::path payload = temporary.path() / "image-payload.bin";
	const fs::path archive = temporary.path() / "foreground-validation.zip";
	WriteBytes(payload, {1, 2, 3, 4});
	WriteZipArchive(archive, {{"inside.jpg", payload}});
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(archive / "inside.jpg");
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	std::mutex mutex;
	std::condition_variable changed;
	bool decoderStarted = false;
	bool releaseDecoder = false;
	bool completionReceived = false;
	bool foregroundWaitReturned = false;
	bool foregroundGotImage = false;
	jpegview_linux::DecodedImageCache cache(16,
		[&](const fs::path&, DecodedImage& decoded, std::string&) {
			std::unique_lock<std::mutex> lock(mutex);
			decoderStarted = true;
			changed.notify_all();
			if (!changed.wait_for(lock, std::chrono::seconds(3),
				[&] { return releaseDecoder; })) return false;
			jpegview_linux::DecodedFrame frame;
			frame.width = frame.height = 1;
			frame.bgra = {1, 2, 3, 255};
			decoded.frames.push_back(std::move(frame));
			return true;
		});
	cache.RequestBackground(source,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr& image) {
			std::lock_guard<std::mutex> lock(mutex);
			completionReceived = static_cast<bool>(image);
			changed.notify_all();
		}, jpegview_linux::PerfWorkClass::DistantSpeculation);
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return decoderStarted; }),
			"archive decode did not reach its foreground-validation barrier");
	}
	coordinator.SetForegroundPending(true);
	std::thread foregroundWaiter([&] {
		const auto image = cache.FindOrWait(source);
		{
			std::lock_guard<std::mutex> lock(mutex);
			foregroundGotImage = static_cast<bool>(image);
			foregroundWaitReturned = true;
		}
		changed.notify_all();
	});
	const auto promotionDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	bool promoted = false;
	while (std::chrono::steady_clock::now() < promotionDeadline) {
		if (cache.GetDiagnostics().foregroundActive == 1) {
			promoted = true;
			break;
		}
		std::this_thread::yield();
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseDecoder = true;
	}
	changed.notify_all();
	bool completedBeforeGateClear = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		completedBeforeGateClear = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return foregroundWaitReturned && completionReceived;
		});
	}
	if (!completedBeforeGateClear) coordinator.SetForegroundPending(false);
	foregroundWaiter.join();
	const bool idle = cache.WaitUntilIdle(std::chrono::seconds(2));
	coordinator.SetForegroundPending(false);
	Expect(source.Valid() && promoted && completedBeforeGateClear && idle &&
		foregroundGotImage && completionReceived,
		"a promoted archive decode lost its admitted context and blocked publication "
		"behind the foreground gate");
}

void TestThumbnailValidationWaitHonorsCancellationAndShutdown() {
	TemporaryDirectory temporary;
	const fs::path payload = temporary.path() / "thumb-payload.bin";
	const fs::path archive = temporary.path() / "thumbnail-validation.zip";
	WriteBytes(payload, {1, 2, 3, 4});
	WriteZipArchive(archive, {{"inside.jpg", payload}});
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(archive / "inside.jpg");
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	const auto runCase = [&](bool shutdown) {
		coordinator.SetForegroundPending(false);
		std::mutex mutex;
		std::condition_variable changed;
		bool processorStarted = false;
		bool releaseProcessor = false;
		jpegview_linux::ThumbnailPreparationWorker::Processor processor =
			[&](const jpegview_linux::ThumbnailPreparationRequest& request)
				-> jpegview_linux::ThumbnailPreparationWorker::ImagePtr {
				std::unique_lock<std::mutex> lock(mutex);
				processorStarted = true;
				changed.notify_all();
				if (!changed.wait_for(lock, std::chrono::seconds(3),
					[&] { return releaseProcessor; })) return
						jpegview_linux::ThumbnailPreparationWorker::ImagePtr{};
				auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
				image->key = request.key;
				image->sourceDescriptor = request.sourceDescriptor;
				image->width = image->height = 1;
				image->bgra = {1, 2, 3, 255};
				return image;
			};
		auto worker = std::make_unique<jpegview_linux::ThumbnailPreparationWorker>(
			std::move(processor));
		auto cancellation = std::make_shared<std::atomic<bool>>(false);
		jpegview_linux::ThumbnailPreparationRequest request;
		request.key = source.Key();
		request.logicalSource = source.LogicalPath();
		request.sourceDescriptor = source;
		request.maximumWidth = 1;
		request.maximumHeight = 1;
		request.workClass = jpegview_linux::PerfWorkClass::VisibleThumbnail;
		request.cancellation = cancellation;
		Expect(worker->Request(request), "thumbnail validation fixture was not admitted");
		{
			std::unique_lock<std::mutex> lock(mutex);
			Expect(changed.wait_for(lock, std::chrono::seconds(2),
				[&] { return processorStarted; }),
				"thumbnail processor did not reach its validation barrier");
		}
		coordinator.SetForegroundPending(true);
		{
			std::lock_guard<std::mutex> lock(mutex);
			releaseProcessor = true;
		}
		changed.notify_all();
		const bool validationQueued = coordinator.WaitForSnapshot(
			[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
				return snapshot.waitingSpeculative == 1 && snapshot.foregroundPending;
			}, std::chrono::seconds(2));
		bool completedBeforeGateClear = false;
		if (shutdown) {
			std::mutex shutdownMutex;
			std::condition_variable shutdownChanged;
			bool destroyed = false;
			std::thread destroyer([&] {
				worker.reset();
				{
					std::lock_guard<std::mutex> lock(shutdownMutex);
					destroyed = true;
				}
				shutdownChanged.notify_all();
			});
			{
				std::unique_lock<std::mutex> lock(shutdownMutex);
				completedBeforeGateClear = shutdownChanged.wait_for(lock,
					std::chrono::seconds(2), [&] { return destroyed; });
			}
			if (!completedBeforeGateClear) coordinator.SetForegroundPending(false);
			destroyer.join();
		} else {
			cancellation->store(true);
			coordinator.NotifyWaiters();
			completedBeforeGateClear = worker->WaitUntilIdle(std::chrono::seconds(2));
			if (!completedBeforeGateClear) coordinator.SetForegroundPending(false);
			(void)worker->WaitUntilIdle(std::chrono::seconds(2));
		}
		coordinator.SetForegroundPending(false);
		return validationQueued && completedBeforeGateClear;
	};
	const bool cancellationReleasedWait = runCase(false);
	const bool shutdownReleasedWait = runCase(true);
	Expect(source.Valid() && cancellationReleasedWait && shutdownReleasedWait,
		"archive thumbnail source validation ignored cancellation or shutdown while "
		"waiting for paired admission");
}

void TestPreparedScansRejectDescriptorRefreshRace() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "images";
	fs::create_directories(directory);
	const fs::path selected = directory / "a-selected.png";
	WriteBytes(selected, {1, 2, 3, 4});
	SetModificationTimeNanoseconds(selected, 1700000000, 123456000);
	WriteBytes(directory / "z-other.png", {4, 3, 2, 1});

	FileList files({directory.string()}, FileList::SortMode::FileName, true, false);
	const auto selectedIndex = files.IndexOf(selected);
	Expect(selectedIndex.has_value() && files.Current() == selected,
		"scan freshness fixture did not select the first filename-sorted entry");
	jpegview_linux::SourceKey currentKey = files.DescriptorAt(*selectedIndex)->Key();
	const std::vector<fs::path> originalOrder = files.Files();

	const FileList::ScanRequest reloadRequest = files.MakeScanRequest(
		FileList::ScanOperation::Reload);
	FileListPreparedScan staleReload = FileList::PrepareScan(reloadRequest, [] { return true; });
	Expect(staleReload.completed && staleReload.replacement.DescriptorAt(*selectedIndex)->Key() == currentKey,
		"reload fixture did not capture the selected file's old source identity");

	const auto refreshSelected = [&](std::uint8_t firstByte, std::int64_t modificationNanoseconds) {
		WriteBytes(selected, {firstByte, 2, 3, 4});
		SetModificationTimeNanoseconds(selected, 1700000000, modificationNanoseconds);
		const jpegview_linux::SourceDescriptor observed =
			jpegview_linux::DescribeImageSource(selected);
		Expect(observed.Key() != currentKey,
			"source refresh fixture did not produce a different file identity");
		const std::uint64_t mutationRevision = files.MutationRevision();
		Expect(files.RefreshSourceDescriptor(currentKey, observed) &&
			files.MutationRevision() == mutationRevision && files.Files() == originalOrder,
			"filename-sorted source refresh unexpectedly changed list membership or order");
		currentKey = observed.Key();
	};
	refreshSelected(9, 123457000);
	const fs::path selectedAtApply = files.Current();
	Expect(!files.ApplyPreparedScan(std::move(staleReload), selectedAtApply) &&
		files.Current() == selectedAtApply && files.DescriptorAt(*selectedIndex)->Key() == currentKey,
		"a reload captured before an exact descriptor refresh replaced the refreshed identity");

	FileList::ScanRequest droppedRequest = FileList::InitialScanRequest(
		{selected.string()}, FileList::SortMode::FileName, true, false,
		FileList::NavigationMode::LoopDirectory);
	droppedRequest.expectedRevision = files.MutationRevision();
	droppedRequest.expectedDescriptorRevision = files.DescriptorRevision();
	FileListPreparedScan staleDroppedInputs = FileList::PrepareScan(droppedRequest,
		[] { return true; });
	Expect(staleDroppedInputs.completed &&
		staleDroppedInputs.replacement.DescriptorAt(0)->Key() == currentKey,
		"dropped-input fixture did not capture its source identity before refresh");
	refreshSelected(8, 123458000);
	Expect(!files.ApplyPreparedScan(std::move(staleDroppedInputs)) &&
		files.Size() == originalOrder.size() && files.Current() == selectedAtApply &&
		files.DescriptorAt(*selectedIndex)->Key() == currentKey,
		"a stale dropped-input replacement bypassed descriptor freshness validation");
}
const TestCase kTests[] = {
	{"interaction-work-policy-idle-deadline-and-capture", &TestInteractionWorkPolicyIdleDeadlineAndCapture},
	{"resolve-work-context-composes-inherited-callbacks", &TestResolveWorkContextComposesInheritedCallbacks},
	{"source-work-admission-across-worker-pools", &TestSourceWorkAdmissionAcrossWorkerPools},
	{"work-context-path-admission-keys-avoid-metadata-io", &TestWorkContextPathAdmissionKeysAvoidMetadataIo},
	{"archive-source-probes-run-under-admission", &TestArchiveSourceProbesRunUnderAdmission},
	{"archive-password-validation-yields-for-foreground-work", &TestArchivePasswordValidationYieldsForForegroundWork},
	{"display-upload-scheduler-permitted-foreground-priority", &TestDisplayUploadSchedulerPrioritizesPermittedForeground},
	{"display-texture-budget-and-banded-upload-rollback", &TestDisplayTextureBudgetAndBandedUploadRollback},
	{"display-texture-retirement-under-sustained-interaction", &TestDisplayTextureRetirementMakesProgressUnderSustainedInteraction},
	{"file-list-date-sorting-and-selection", &TestFileListDateSortingAndSelectionPreservation},
	{"file-list-natural-name-sorting-and-marked-selection", &TestFileListNaturalNameSortingAndMarkedSelection},
	{"file-list-asynchronous-sorting-keeps-latest-selection", &TestFileListAsynchronousSortingKeepsLatestSelection},
	{"file-list-defers-metadata-resort-to-prepared-sort", &TestFileListDefersMetadataResortToPreparedSort},
	{"file-list-asynchronous-sort-rejects-stale-descriptors", &TestFileListAsynchronousSortRejectsStaleDescriptors},
	{"file-list-creation-time-archive-fallback", &TestFileListCreationTimeArchiveFallback},
	{"file-list-size-and-random-sorting", &TestFileListSizeAndRandomSorting},
	{"file-list-navigation-modes-and-reload", &TestFileListNavigationModesAndReload},
	{"file-list-multiple-inputs", &TestFileListMultipleInputs},
	{"file-list-asynchronous-scanning", &TestFileListAsynchronousScanning},
	{"archive-traversal-yield-releases-admission", &TestArchiveTraversalYieldReleasesAdmission},
	{"directory-enumeration-admission-yields-and-retries", &TestDirectoryEnumerationAdmissionYieldsAndRetries},
	{"archive-prefetch-planner-keeps-admitted-context", &TestArchivePrefetchPlannerKeepsAdmittedContext},
	{"archive-decoded-cache-promotion-completes-during-foreground-gate", &TestArchiveDecodedCachePromotionCompletesDuringForegroundGate},
	{"prepared-scans-reject-descriptor-refresh-race", &TestPreparedScansRejectDescriptorRefreshRace},
	{"thumbnail-preparation-retry-preserves-identity", &TestThumbnailPreparationRetryPreservesIdentityAcrossRepeatedFailuresAndRecovery},
	{"thumbnail-validation-wait-honors-cancellation-and-shutdown", &TestThumbnailValidationWaitHonorsCancellationAndShutdown},
};

} // namespace

const TestSuite& GetWorkAdmissionSuite() {
	static const TestSuite suite{"work_admission", kTests, sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
