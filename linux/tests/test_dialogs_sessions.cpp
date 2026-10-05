#include "test_harness.h"
#include "test_support.h"

namespace {

void TestFileDialogFiltering() {
	const std::vector<jpegview_linux::FileDialogEntry> entries = {
		{fs::path("/pictures"), true, true},
		{fs::path("/pictures/Photos"), true, false},
		{fs::path("/pictures/Alpha.JPG"), false, false},
		{fs::path("/pictures/holiday.png"), false, false},
	};
	Expect(jpegview_linux::FilterFileDialogEntries(entries, {}).size() == entries.size(),
		"empty open-dialog filter removed entries");
	const std::vector<jpegview_linux::FileDialogEntry> filtered =
		jpegview_linux::FilterFileDialogEntries(entries, "PH");
	Expect(filtered.size() == 3 && filtered[0].parent &&
		filtered[1].path.filename() == "Photos" && filtered[2].path.filename() == "Alpha.JPG",
		"open-dialog filter was not a case-insensitive filename substring match");
	const std::vector<jpegview_linux::FileDialogEntry> unmatched =
		jpegview_linux::FilterFileDialogEntries(entries, "missing");
	Expect(unmatched.size() == 1 && unmatched[0].parent,
		"open-dialog filter did not retain only the parent entry when nothing matched");
	const std::vector<jpegview_linux::FileDialogEntry> pathMatch =
		jpegview_linux::FilterFileDialogEntries({
			{fs::path("/archive/summer-trip/photo.jpg"), false, false}}, "summer", true);
	Expect(pathMatch.size() == 1,
		"open-dialog filter did not match a directory in the full file path");
	const std::vector<jpegview_linux::FileDialogEntry> browsePathDoesNotMatch =
		jpegview_linux::FilterFileDialogEntries({
			{fs::path("/archive/summer-trip/photo.jpg"), false, false}}, "summer");
	Expect(browsePathDoesNotMatch.empty(),
		"Browse filename filtering unexpectedly matched a folder path");
}

void TestRecentFilesMruUniquenessPersistenceAndViewportSnapshots() {
	TemporaryDirectory temporary;
	const fs::path folderA = temporary.path() / "album-a";
	const fs::path folderB = temporary.path() / "album-b";
	const fs::path firstA = folderA / "first.jpg";
	const fs::path secondA = folderA / "second.jpg";
	const fs::path onlyB = folderB / "only.jpg";
	jpegview_linux::RecentFiles recent;
	recent.Add(firstA);
	recent.Add(onlyB);
	recent.Add(folderA / "nested" / ".." / "second.jpg");
	const fs::path normalizedSecondA = fs::absolute(secondA).lexically_normal();
	const fs::path normalizedOnlyB = fs::absolute(onlyB).lexically_normal();
	Expect(recent.Files().size() == 2 && recent.Files()[0] == normalizedSecondA &&
		recent.Files()[1] == normalizedOnlyB,
		"recent files did not retain one normalized MRU image per parent folder");
	recent.Add(secondA);
	Expect(recent.Files().size() == 2 && recent.Files()[0] == normalizedSecondA,
		"reopening a recent image duplicated its folder row instead of moving it to the front");

	jpegview_linux::ViewportSnapshot snapshot{false, false, false, 2.75, 3.5};
	recent.RememberViewport(secondA, snapshot);
	recent.RememberDoublePageMode(secondA, {true, true});
	const std::vector<fs::path> orderBeforeRemoval = recent.Files();
	const auto removedRecent = recent.Remove(secondA);
	Expect(removedRecent.has_value() && removedRecent->path == normalizedSecondA &&
		removedRecent->index == 0 && recent.Files().size() == 1 &&
		recent.Files().front() == normalizedOnlyB,
		"removing a recent image did not return its original MRU position");
	Expect(recent.FindViewport(secondA).has_value() && recent.FindDoublePageMode(secondA).has_value(),
		"removing a recent row discarded independent per-image viewing snapshots");
	Expect(recent.Restore(*removedRecent) && recent.Files() == orderBeforeRemoval,
		"undoing a recent removal did not restore its exact MRU position");
	const auto removedTail = recent.Remove(onlyB);
	Expect(removedTail.has_value() && removedTail->index == 1 &&
		recent.Restore(*removedTail) && recent.Files() == orderBeforeRemoval,
		"undoing a non-leading recent removal changed the order");
	Expect(!recent.Remove(folderB / "not-recent.jpg").has_value(),
		"removing an absent image unexpectedly modified recent history");
	const auto restored = recent.FindViewport(normalizedSecondA);
	Expect(restored.has_value() && !restored->fitToWindow &&
		!restored->fillWithCrop && !restored->noEnlarge,
		"recent viewport snapshot did not preserve its manual view mode");
	ExpectNear(restored->zoom, 2.75, 0.0000001,
		"recent viewport snapshot did not preserve its manual zoom");
	ExpectNear(restored->relativeZoom, 3.5, 0.0000001,
		"recent viewport snapshot did not preserve its fit-relative zoom");
	recent.RememberViewport(firstA, {true, true, false, 1000.0});
	const auto clamped = recent.FindViewport(firstA);
	Expect(clamped.has_value() && clamped->fitToWindow && clamped->fillWithCrop &&
		!clamped->noEnlarge && clamped->zoom == jpegview_linux::kMaximumZoom,
		"recent viewport snapshot did not clamp a finite zoom to the supported range");
	recent.RememberViewport(folderB / "invalid.jpg",
		{false, false, false, std::numeric_limits<double>::infinity()});
	Expect(!recent.FindViewport(folderB / "invalid.jpg").has_value(),
		"recent viewport snapshot accepted a non-finite zoom");

	const fs::path unusualPath = folderB / "line\nbreak.jpg";
	recent.Add(unusualPath);
	recent.RememberViewport(unusualPath, {false, false, false, 0.125, 0.25});
	recent.RememberDoublePageMode(unusualPath, {true, false});
	const fs::path database = temporary.path() / "state" / "recent-files.db";
	Expect(jpegview_linux::SaveRecentFiles(database, recent),
		"recent-file database could not be saved atomically");
	Expect(!fs::exists(database.string() + ".tmp." + std::to_string(static_cast<long long>(::getpid()))),
		"recent-file database left its temporary file behind");
	jpegview_linux::RecentFiles loaded;
	Expect(jpegview_linux::LoadRecentFiles(database, loaded),
		"recent-file database could not be loaded");
	Expect(loaded.Files() == recent.Files(),
		"recent-file rows did not round-trip in MRU order, including a newline in a filename");
	const auto unusualViewport = loaded.FindViewport(unusualPath);
	Expect(unusualViewport.has_value() && !unusualViewport->fitToWindow,
		"per-file viewport snapshot did not round-trip independently from the folder list");
	ExpectNear(unusualViewport->zoom, 0.125, 0.0000001,
		"persisted per-file zoom did not round-trip");
	ExpectNear(unusualViewport->relativeZoom, 0.25, 0.0000001,
		"persisted per-file relative zoom did not round-trip");
	const auto unusualDisplayMode = loaded.FindDoublePageMode(unusualPath);
	Expect(unusualDisplayMode.has_value() && unusualDisplayMode->enabled &&
		!unusualDisplayMode->mangaReadingOrder,
		"per-image double-page mode did not round-trip independently of viewport zoom");

	std::string duplicateRecentRow;
	{
		std::ifstream input(database);
		std::string line;
		while (std::getline(input, line)) {
			if (line.rfind("R ", 0) == 0) {
				duplicateRecentRow = line;
				break;
			}
		}
	}
	Expect(!duplicateRecentRow.empty(), "saved recent-file database had no valid recent row");
	const fs::path aliasPath = folderB / "nested" / ".." / "line\nbreak.jpg";
	std::string encodedAliasPath;
	constexpr char hexDigits[] = "0123456789abcdef";
	for (const unsigned char byte : aliasPath.string()) {
		encodedAliasPath.push_back(hexDigits[byte >> 4]);
		encodedAliasPath.push_back(hexDigits[byte & 0x0f]);
	}
	{
		std::ofstream malformed(database, std::ios::app);
		malformed << duplicateRecentRow << "\nR " << encodedAliasPath <<
			"\nR not-hex\nV 616263 1 0 1 nan\n"
			"V 2f616263 1 0 1 999\nD 2f616263 1 1\nD 2f616264 2 0\n";
	}
	jpegview_linux::RecentFiles tolerant;
	Expect(jpegview_linux::LoadRecentFiles(database, tolerant),
		"recent-file loader rejected a database containing malformed records");
	Expect(tolerant.Files() == recent.Files(),
		"malformed recent rows changed valid MRU entries");
	const auto clampedPersisted = tolerant.FindViewport(fs::path("/abc"));
	Expect(clampedPersisted.has_value() &&
		clampedPersisted->zoom == jpegview_linux::kMaximumZoom,
		"finite out-of-range zoom in a malformed-tolerant database was not clamped");
	ExpectNear(clampedPersisted->relativeZoom, 1.0, 1e-12,
		"legacy viewport records without a relative zoom ratio were not accepted");
	const auto tolerantMode = tolerant.FindDoublePageMode(fs::path("/abc"));
	Expect(tolerantMode.has_value() && tolerantMode->enabled && tolerantMode->mangaReadingOrder,
		"valid recent display-mode row was not retained alongside malformed rows");

	const fs::path missingDatabase = temporary.path() / "first-run" / "recent-files.db";
	jpegview_linux::RecentFiles firstRun;
	Expect(jpegview_linux::LoadRecentFiles(missingDatabase, firstRun) && firstRun.Files().empty() &&
		!firstRun.FindDoublePageMode(onlyB).has_value(),
		"a missing first-run recent database was not treated as an empty successful load");
	firstRun.Add(onlyB);
	Expect(jpegview_linux::SaveRecentFiles(missingDatabase, firstRun) &&
		fs::exists(missingDatabase),
		"history loaded from a missing first-run database could not be saved later");
	jpegview_linux::RecentFiles firstRunReloaded;
	Expect(jpegview_linux::LoadRecentFiles(missingDatabase, firstRunReloaded) &&
		firstRunReloaded.Files() == firstRun.Files(),
		"first-run history did not persist after a missing database was loaded");

	jpegview_linux::RecentFiles unchangedOnError;
	unchangedOnError.Add(onlyB);
	const fs::path directoryInsteadOfDatabase = temporary.path() / "not-a-database";
	fs::create_directories(directoryInsteadOfDatabase);
	Expect(!jpegview_linux::LoadRecentFiles(directoryInsteadOfDatabase, unchangedOnError) &&
		unchangedOnError.Files() == firstRun.Files(),
		"an unreadable database path was accepted or cleared existing in-memory history");

	jpegview_linux::RecentFiles capped;
	const std::size_t capCount = std::max(jpegview_linux::kMaximumRecentFolders,
		jpegview_linux::kMaximumRecentViewportSnapshots) + 3;
	for (std::size_t index = 0; index < capCount; ++index) {
		const fs::path filename = temporary.path() / ("folder-" + std::to_string(index)) / "image.jpg";
		capped.Add(filename);
		capped.RememberViewport(filename, {false, false, false, 1.0 + index});
		capped.RememberDoublePageMode(filename, {true, (index & 1u) != 0});
	}
	Expect(capped.Files().size() == jpegview_linux::kMaximumRecentFolders &&
		capped.ViewportSnapshotCount() == jpegview_linux::kMaximumRecentViewportSnapshots &&
		capped.DisplayModeSnapshotCount() == jpegview_linux::kMaximumRecentDisplayModeSnapshots,
		"recent folder rows or per-image view snapshots exceeded their independent bounds");
	Expect(!capped.FindViewport(temporary.path() / "folder-0" / "image.jpg").has_value() &&
		capped.FindViewport(temporary.path() /
			("folder-" + std::to_string(capCount - 1)) / "image.jpg").has_value() &&
		!capped.FindDoublePageMode(temporary.path() / "folder-0" / "image.jpg").has_value() &&
		capped.FindDoublePageMode(temporary.path() /
			("folder-" + std::to_string(capCount - 1)) / "image.jpg").has_value(),
		"per-image snapshot retention did not evict the least recently used entry");

	ScopedEnvironment stateHome("XDG_STATE_HOME", (temporary.path() / "xdg-state").string());
	Expect(jpegview_linux::RecentFilesDatabasePath() ==
		temporary.path() / "xdg-state" / "jpegview-linux" / "recent-files.db",
		"recent-file database path did not follow XDG_STATE_HOME");
}

void TestImageSessionControllerSnapshotsNavigationAndClipboardState() {
	TemporaryDirectory temporary;
	const fs::path first = temporary.path() / "loaded" / "first.png";
	const fs::path second = temporary.path() / "recent" / "second.png";
	const fs::path clipboard = temporary.path() / "clipboard.png";
	fs::create_directories(first.parent_path());
	fs::create_directories(second.parent_path());
	WriteTinyImage(first);
	WriteTinyImage(second);
	WriteTinyImage(clipboard);
	const jpegview_linux::SourceDescriptor firstSource =
		jpegview_linux::DescribeImageSource(first);
	const jpegview_linux::SourceDescriptor secondSource =
		jpegview_linux::DescribeImageSource(second);
	const jpegview_linux::SourceDescriptor clipboardSource =
		jpegview_linux::DescribeImageSource(clipboard);
	const jpegview_linux::ViewportSnapshot firstView{false, false, false, 1.5, 1.0};
	const jpegview_linux::ViewportSnapshot clipboardReturn{false, true, false, 2.75, 1.25};
	const jpegview_linux::ViewportSnapshot navigation{true, false, true, 0.75, 1.0};
	const jpegview_linux::ViewportSnapshot savedSecond{false, false, false, 3.0, 2.0};
	jpegview_linux::ImageProcessingPreset firstProcessing;
	firstProcessing.processing.contrast = 0.2;
	firstProcessing.autoContrast = true;
	jpegview_linux::RecentFiles recentFiles;
	recentFiles.RememberViewport(second, savedSecond);
	recentFiles.RememberDoublePageMode(second, {true, true});
	jpegview_linux::ImageSessionController session;

	const jpegview_linux::ImageSessionStart firstStart = session.BeginSelection(
		firstSource, first, firstView, firstProcessing, true);
	Expect(firstStart.selection.generation == session.Generation() &&
		firstStart.selection.source.Key() == firstSource.Key() &&
		firstStart.selection.viewport.zoom == firstView.zoom &&
		firstStart.selection.processing.processing.contrast == 0.2 &&
		firstStart.selection.processing.autoContrast &&
		firstStart.selection.tracksRecentHistory &&
		firstStart.effects.clearPreviousPresentation && firstStart.effects.restoreViewport &&
		firstStart.effects.requestSelectedSourcePreparation &&
		session.MatchesSelection(firstStart.selection.generation, firstSource.Key()),
		"image-session start did not capture its source, generation, viewport, processing, and effects");
	Expect(session.MarkDocumentChanged() == 1 && session.CommitLoad(first, recentFiles) &&
		session.DocumentRevision() == 1 && session.LoadedPath() ==
		fs::absolute(first).lexically_normal(),
		"successful image-session commit did not retain document revision and Recents ownership");

	session.SetClipboardReturnViewport(clipboardReturn);
	const jpegview_linux::ViewportSnapshot ordinaryNavigation =
		session.ResolveViewportForSelection(second, false, navigation, navigation,
			recentFiles);
	const jpegview_linux::ViewportSnapshot recentOpen =
		session.ResolveViewportForSelection(second, false, navigation, navigation,
			recentFiles, true);
	Expect(session.ResolveViewportForSelection(first, false, navigation, navigation,
		recentFiles).zoom == clipboardReturn.zoom &&
		session.ResolveViewportForSelection(clipboard, true, clipboardReturn, navigation,
			recentFiles).zoom == navigation.zoom &&
		ordinaryNavigation.fitToWindow == navigation.fitToWindow &&
		ordinaryNavigation.fillWithCrop == navigation.fillWithCrop &&
		ordinaryNavigation.noEnlarge == navigation.noEnlarge &&
		ordinaryNavigation.zoom == navigation.zoom &&
		ordinaryNavigation.relativeZoom == navigation.relativeZoom &&
		recentOpen.fitToWindow == savedSecond.fitToWindow &&
		recentOpen.fillWithCrop == savedSecond.fillWithCrop &&
		recentOpen.noEnlarge == savedSecond.noEnlarge &&
		recentOpen.zoom == savedSecond.zoom &&
		recentOpen.relativeZoom == savedSecond.relativeZoom &&
		recentFiles.FindDoublePageMode(second).has_value() &&
		recentFiles.FindDoublePageMode(second)->mangaReadingOrder,
		"ordinary navigation or explicit Recents open applied the wrong viewport policy");

	const jpegview_linux::ImageSessionStart clipboardStart = session.BeginSelection(
		clipboardSource, clipboard, navigation, {}, false);
	Expect(clipboardStart.selection.generation > firstStart.selection.generation &&
		!session.MatchesSelection(firstStart.selection.generation, firstSource.Key()) &&
		session.MatchesSelection(clipboardStart.selection.generation, clipboardSource.Key()) &&
		session.DocumentRevision() == 0 && session.LoadedPath() ==
		fs::absolute(first).lexically_normal() && session.OwnsLoadedPath(first) &&
		!session.CommitLoad(clipboard, recentFiles),
		"clipboard selection replaced committed history or retained the previous document revision");
	const jpegview_linux::ImageProcessingPreset updatedProcessing{{}, false};
	session.UpdateProcessing(updatedProcessing);
	Expect(session.Processing().autoContrast == false &&
		session.Selection()->processing.autoContrast == false,
		"current processing state was not updated in the selected session snapshot");
	session.ClearClipboardReturnViewport();
	Expect(!session.ClipboardReturnViewport().has_value(),
		"completed return from clipboard mode retained its temporary viewport snapshot");
}

void TestImageSessionSelectedLoadCommitsOnlyReadyCurrentOwner() {
	TemporaryDirectory temporary;
	const fs::path first = temporary.path() / "first" / "image.png";
	const fs::path failed = temporary.path() / "failed" / "image.png";
	fs::create_directories(first.parent_path());
	fs::create_directories(failed.parent_path());
	WriteTinyImage(first);
	WriteTinyImage(failed);
	const auto firstSource = jpegview_linux::DescribeImageSource(first);
	const auto failedSource = jpegview_linux::DescribeImageSource(failed);
	jpegview_linux::ImageSessionController session;
	jpegview_linux::RecentFiles recent;
	const auto start = session.BeginSelection(firstSource, first, {}, {}, true);
	Expect(session.Stage() == jpegview_linux::ImageSessionStage::Selected &&
		session.SetStage(start.selection.generation, firstSource.Key(),
			jpegview_linux::ImageSessionStage::AwaitingDisplayFrame) &&
		!session.MarkDisplayFrameReady(start.selection.generation + 1,
			firstSource.Key()) &&
		session.MarkDisplayFrameReady(start.selection.generation, firstSource.Key()) &&
		session.CommitSelectedLoad(start.selection.generation, firstSource.Key(), first, recent) &&
		session.Stage() == jpegview_linux::ImageSessionStage::Committed &&
		session.LoadedPath() == fs::absolute(first).lexically_normal(),
		"selected-load stage did not require a matching renderer-ready owner before Recents commit");
	const auto sameSourceStart = session.BeginSelection(firstSource, first, {}, {}, true);
	Expect(!sameSourceStart.effects.clearPreviousPresentation &&
		sameSourceStart.selection.generation > start.selection.generation,
		"same-source replacement did not preserve the current presentation while advancing its owner");
	const auto failedStart = session.BeginSelection(failedSource, failed, {}, {}, true);
	Expect(failedStart.effects.clearPreviousPresentation &&
		session.SetStage(failedStart.selection.generation, failedSource.Key(),
		jpegview_linux::ImageSessionStage::AwaitingDecodedSource) &&
		session.FailSelectedLoad(failedStart.selection.generation,
			failedSource.Key(), failed) &&
		session.Stage() == jpegview_linux::ImageSessionStage::Failed &&
		session.LoadedPath() == fs::absolute(first).lexically_normal() &&
		!session.CommitSelectedLoad(failedStart.selection.generation,
			failedSource.Key(), failed, recent),
		"failed selected work replaced the last committed image history");
}

void TestSelectedSourceDecodeChannelRejectsStaleOwnerCompletions() {
	TemporaryDirectory temporary;
	const fs::path first = temporary.path() / "first.png";
	const fs::path second = temporary.path() / "second.png";
	WriteTinyImage(first);
	WriteTinyImage(second);
	const auto firstSource = jpegview_linux::DescribeImageSource(first);
	const auto secondSource = jpegview_linux::DescribeImageSource(second);
	jpegview_linux::SelectedSourceDecodeChannel channel;
	channel.Activate(1, firstSource.Key());
	Expect(channel.Publish({1, firstSource, {}, {}}),
		"current selected decode could not publish its result");
	channel.Activate(2, secondSource.Key());
	channel.Activate(3, firstSource.Key());
	Expect(!channel.Publish({1, firstSource, {}, {}}) &&
		!channel.Take(1, firstSource.Key()).has_value() &&
		channel.Publish({3, firstSource, {}, {}}),
		"A-to-B-to-A selection accepted a stale first-A completion");
	const auto newest = channel.Take(3, firstSource.Key());
	Expect(newest.has_value() && newest->generation == 3 &&
		newest->source.Key() == firstSource.Key() &&
		!channel.Take(3, firstSource.Key()).has_value(),
		"selected decode mailbox did not consume only the latest matching result");
	channel.Shutdown();
	Expect(!channel.Publish({3, firstSource, {}, {}}),
		"selected decode mailbox accepted work after shutdown");
}

void TestImageSessionControllerPlansSelectedSourcePreparation() {
	using jpegview_linux::ImageSessionController;
	using jpegview_linux::SelectedSourcePreparationAction;
	using jpegview_linux::SelectedSourcePreparationSnapshot;
	ImageSessionController session;
	SelectedSourcePreparationSnapshot snapshot;
	snapshot.jpegSource = true;
	snapshot.displayCacheEnabled = true;
	snapshot.sourceValid = true;
	Expect(session.PlanSelectedSourcePreparation(snapshot) ==
		SelectedSourcePreparationAction::RequestJpegDimensions,
		"cold valid JPEG did not choose asynchronous dimension preparation");
	snapshot.cachedJpegDimensions = true;
	Expect(session.PlanSelectedSourcePreparation(snapshot) ==
		SelectedSourcePreparationAction::UseCachedJpegDimensions,
		"cached JPEG dimensions did not bypass the header request");
	snapshot.cachedJpegDimensions = false;
	snapshot.jpegDimensionProbeFailed = true;
	Expect(session.PlanSelectedSourcePreparation(snapshot) ==
		SelectedSourcePreparationAction::DecodeSelectedSource,
		"failed JPEG header did not choose the full decode fallback");
	snapshot.jpegDimensionProbeFailed = false;
	snapshot.sourceValid = false;
	Expect(session.PlanSelectedSourcePreparation(snapshot) ==
		SelectedSourcePreparationAction::DecodeSelectedSource,
		"invalid JPEG descriptor did not avoid another header request");
	snapshot.sourceValid = true;
	snapshot.preparationRequested = false;
	Expect(session.PlanSelectedSourcePreparation(snapshot) ==
		SelectedSourcePreparationAction::Skip,
		"disabled source preparation still admitted image work");
	snapshot.preparationRequested = true;
	snapshot.jpegSource = false;
	Expect(session.PlanSelectedSourcePreparation(snapshot) ==
		SelectedSourcePreparationAction::DecodeSelectedSource,
		"non-JPEG source did not use the decoded-image path");
	snapshot.jpegSource = true;
	snapshot.displayCacheEnabled = false;
	Expect(session.PlanSelectedSourcePreparation(snapshot) ==
		SelectedSourcePreparationAction::DecodeSelectedSource,
		"zero cache budget requested JPEG header preparation");

	jpegview_linux::DecodedSourcePreparationSnapshot decoded;
	Expect(session.ShouldPrepareDecodedSource(decoded),
		"ready source with no display fallback did not prepare decoded pixels");
	decoded.cachedDisplayReady = true;
	Expect(!session.ShouldPrepareDecodedSource(decoded),
		"cached display frame caused an unnecessary decoded-source load");
	decoded.cachedDisplayReady = false;
	decoded.deferredForPossibleSpread = true;
	Expect(!session.ShouldPrepareDecodedSource(decoded),
		"spread deferral did not suppress ordinary decoded-source preparation");
	decoded.deferredForPossibleSpread = false;
	decoded.waitingForJpegDimensions = true;
	Expect(!session.ShouldPrepareDecodedSource(decoded),
		"pending JPEG dimensions allowed a competing decoded-source load");
}

void TestRecentImageLoadHistoryCommitsOnlyAfterSuccess() {
	TemporaryDirectory temporary;
	const fs::path firstFolder = temporary.path() / "first-folder";
	const fs::path secondFolder = temporary.path() / "second-folder";
	const fs::path thirdFolder = temporary.path() / "third-folder";
	const fs::path good = firstFolder / "good.jpg";
	const fs::path malformed = firstFolder / "malformed.jpg";
	const fs::path loaded = secondFolder / "loaded.jpg";
	const fs::path pending = thirdFolder / "pending.jpg";
	const fs::path replacement = temporary.path() / "replacement" / "replacement.jpg";
	const fs::path normalizedGood = fs::absolute(good).lexically_normal();
	const fs::path normalizedLoaded = fs::absolute(loaded).lexically_normal();

	jpegview_linux::RecentFiles recents;
	recents.Add(good);
	jpegview_linux::RecentImageLoadState state;
	const jpegview_linux::ViewportSnapshot defaultView{true, false, true, 1.0, 1.0};
	state.BeginLoad(malformed, defaultView);
	Expect(state.FailLoad(malformed),
		"a failed cold-image continuation was not retired from pending load state");
	Expect(state.LoadedPath().empty() && recents.Files().size() == 1 &&
		recents.Files().front() == normalizedGood,
		"a failed cold JPEG replaced the existing same-folder recent image");

	const jpegview_linux::ViewportSnapshot loadedView{false, false, false, 1.75, 1.0};
	state.BeginLoad(loaded, loadedView);
	Expect(state.CommitLoad(loaded, recents) && state.LoadedPath() == normalizedLoaded &&
		recents.Files().size() == 2 && recents.Files().front() == normalizedLoaded,
		"a successful image load did not commit its recent row and history owner");
	state.BeginLoad(pending, defaultView);
	Expect(state.FailLoad(pending) && state.LoadedPath() == normalizedLoaded,
		"a failed replacement cleared the previous committed image history owner");
	Expect(state.OwnsLoadedPath(loaded) && !state.OwnsLoadedPath(pending),
		"the committed path was not the only image recognized as the loaded history owner");
	const jpegview_linux::DoublePageModeState modes{false, false};
	state.SaveCurrentBeforeLoad(pending, loadedView, modes, recents);
	state.BeginLoad(pending, defaultView);
	Expect(state.FailLoad(pending) && state.LoadedPath() == normalizedLoaded &&
		!state.OwnsLoadedPath(loaded),
		"a failed replacement discarded or falsely retained the previous presentation owner");
	const jpegview_linux::ViewportSnapshot transientView{false, false, false, 0.75, 0.5};
	const jpegview_linux::ViewportSnapshot restoredAfterFailure = state.ViewportForSelection(
		loaded, transientView, defaultView, recents);
	const jpegview_linux::ViewportSnapshot recentAfterFailure = state.ViewportForSelection(
		loaded, transientView, defaultView, recents, true);
	Expect(restoredAfterFailure.zoom == defaultView.zoom &&
		restoredAfterFailure.relativeZoom == defaultView.relativeZoom &&
		restoredAfterFailure.fitToWindow == defaultView.fitToWindow &&
		recentAfterFailure.zoom == loadedView.zoom &&
		recentAfterFailure.relativeZoom == loadedView.relativeZoom &&
		recentAfterFailure.fitToWindow == loadedView.fitToWindow,
		"ordinary navigation or explicit Recents open applied the wrong failure-reversal viewport");

	const fs::path followup = temporary.path() / "followup" / "followup.jpg";
	const jpegview_linux::ViewportSnapshot pendingView{false, false, false, 3.25, 2.5};
	const jpegview_linux::ViewportSnapshot outgoingView{false, true, false, 1.5, 1.25};
	const jpegview_linux::DoublePageModeState outgoingModes{true, true};
	const jpegview_linux::ViewportSnapshot pendingCurrentView{true, false, false, 2.75, 1.6};
	const jpegview_linux::DoublePageModeState pendingCurrentModes{false, false};
	recents.RememberViewport(loaded, outgoingView);
	recents.RememberDoublePageMode(loaded, outgoingModes);
	recents.RememberViewport(pending, pendingView);
	state.SaveCurrentBeforeLoad(pending, outgoingView, outgoingModes, recents);
	state.BeginLoad(pending, *recents.FindViewport(pending));
	Expect(state.LoadedPath() == normalizedLoaded &&
		!state.OwnsLoadedPath(loaded) && !state.OwnsLoadedPath(pending),
		"a pending image became the loaded history owner before completion");

	const auto continuation = state.TakePendingLoad(pending);
	Expect(continuation.has_value() &&
		continuation->viewportSnapshot.zoom == pendingView.zoom,
		"a cold JPEG continuation lost the selected image's saved viewport");
	state.SaveCurrentBeforeLoad(pending, pendingCurrentView, pendingCurrentModes, recents);
	state.BeginLoad(pending, continuation->viewportSnapshot);
	state.CancelPendingLoad();
	state.SaveCurrentBeforeLoad(replacement, pendingCurrentView, pendingCurrentModes, recents);
	state.BeginLoad(replacement, defaultView);
	const auto preservedPendingView = recents.FindViewport(pending);
	const auto savedOutgoingView = recents.FindViewport(loaded);
	const auto savedOutgoingModes = recents.FindDoublePageMode(loaded);
	Expect(state.LoadedPath() == normalizedLoaded &&
		!state.OwnsLoadedPath(loaded) && !state.OwnsLoadedPath(replacement) &&
		preservedPendingView.has_value() &&
		preservedPendingView->zoom == pendingView.zoom &&
		preservedPendingView->relativeZoom == pendingView.relativeZoom &&
		savedOutgoingView.has_value() && savedOutgoingView->fitToWindow == outgoingView.fitToWindow &&
		savedOutgoingView->fillWithCrop == outgoingView.fillWithCrop &&
		savedOutgoingView->noEnlarge == outgoingView.noEnlarge &&
		savedOutgoingView->zoom == outgoingView.zoom &&
		savedOutgoingView->relativeZoom == outgoingView.relativeZoom &&
		savedOutgoingModes.has_value() &&
		savedOutgoingModes->enabled == outgoingModes.enabled &&
		savedOutgoingModes->mangaReadingOrder == outgoingModes.mangaReadingOrder,
		"cold-image continuation or cancellation replacement overwrote the committed image's viewport or D/J modes");

	const jpegview_linux::ViewportSnapshot replacementView{false, false, true, 4.0, 3.0};
	const jpegview_linux::DoublePageModeState replacementModes{false, true};
	Expect(state.CommitLoad(replacement, recents) && state.LoadedPath() ==
		fs::absolute(replacement).lexically_normal(),
		"the replacement image did not take history ownership after loading");
	state.SaveCurrentBeforeLoad(followup, replacementView, replacementModes, recents);
	const auto savedReplacementView = recents.FindViewport(replacement);
	const auto savedReplacementModes = recents.FindDoublePageMode(replacement);
	Expect(savedReplacementView.has_value() && savedReplacementView->zoom == replacementView.zoom &&
		savedReplacementView->relativeZoom == replacementView.relativeZoom &&
		savedReplacementModes.has_value() &&
		savedReplacementModes->enabled == replacementModes.enabled &&
		savedReplacementModes->mangaReadingOrder == replacementModes.mangaReadingOrder,
		"a newly committed history owner did not save its own viewport and D/J modes");
}

void TestViewportSnapshotFollowsSelectedIdentityDuringCancellation() {
	TemporaryDirectory temporary;
	const fs::path committed = temporary.path() / "committed.jpg";
	const fs::path pending = temporary.path() / "pending.jpg";
	const jpegview_linux::ViewportSnapshot committedView{
		false, false, false, 2.0, 1.0};
	const jpegview_linux::ViewportSnapshot pendingView{
		false, true, true, 3.0, 1.25};
	const jpegview_linux::ViewportSnapshot navigationView{
		true, false, true, 0.75, 1.0};

	jpegview_linux::RecentFiles recentFiles;
	recentFiles.RememberViewport(committed, committedView);
	recentFiles.RememberViewport(pending, pendingView);
	jpegview_linux::RecentImageLoadState state;
	state.BeginLoad(committed, committedView);
	Expect(state.CommitLoad(committed, recentFiles),
		"the committed selection fixture did not acquire history ownership");
	state.SaveCurrentBeforeLoad(pending, committedView, {}, recentFiles);
	state.BeginLoad(pending, pendingView);

	const jpegview_linux::ViewportSnapshot continuingPending =
		state.ViewportForSelection(pending, pendingView, navigationView, recentFiles);
	Expect(continuingPending.fitToWindow == pendingView.fitToWindow &&
		continuingPending.fillWithCrop == pendingView.fillWithCrop &&
		continuingPending.noEnlarge == pendingView.noEnlarge &&
		continuingPending.zoom == 3.0 &&
		continuingPending.relativeZoom == pendingView.relativeZoom,
		"continuing the same pending selection lost its 3x viewport");

	const jpegview_linux::ViewportSnapshot reversedSelection =
		state.ViewportForSelection(committed, pendingView, navigationView, recentFiles);
	const jpegview_linux::ViewportSnapshot restoredRecentSelection =
		state.ViewportForSelection(committed, pendingView, navigationView, recentFiles, true);
	Expect(!state.OwnsLoadedPath(committed) && state.LoadedPath() ==
		fs::absolute(committed).lexically_normal() &&
		reversedSelection.fitToWindow == navigationView.fitToWindow &&
		reversedSelection.fillWithCrop == navigationView.fillWithCrop &&
		reversedSelection.noEnlarge == navigationView.noEnlarge &&
		reversedSelection.zoom == navigationView.zoom &&
		reversedSelection.relativeZoom == navigationView.relativeZoom &&
		restoredRecentSelection.zoom == committedView.zoom &&
		restoredRecentSelection.relativeZoom == committedView.relativeZoom,
		"normal reversal or explicit Recents opening applied the wrong viewport policy");

	state.CancelPendingLoad();
	Expect(!state.TakePendingLoad(pending).has_value() &&
		!state.CommitLoad(pending, recentFiles),
		"canceled pending state remained available to continue or commit");
	state.BeginLoad(committed, reversedSelection);
	const auto restoredSelection = state.TakePendingLoad(committed);
	Expect(restoredSelection.has_value() &&
		restoredSelection->viewportSnapshot.zoom == navigationView.zoom &&
		restoredSelection->viewportSnapshot.relativeZoom ==
		navigationView.relativeZoom &&
		!state.TakePendingLoad(pending).has_value(),
		"the replacement selection inherited state from the canceled identity");
}

void TestPendingRecentViewportTracksUserModeChanges() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "saved-two-times.jpg";
	jpegview_linux::RecentFiles recents;
	recents.RememberViewport(filename, {false, false, false, 2.0, 1.0});
	const auto saved = recents.FindViewport(filename);
	Expect(saved.has_value() && saved->zoom == 2.0,
		"the pending viewport fixture did not begin from its saved 2x scale");

	jpegview_linux::RecentImageLoadState state;
	state.BeginLoad(filename, *saved);
	const jpegview_linux::ViewportSnapshot actualSize{false, false, false, 1.0, 1.0};
	Expect(state.UpdatePendingViewport(filename, actualSize),
		"Actual Size did not update the selected pending image's snapshot");
	Expect(!state.UpdatePendingViewport(temporary.path() / "another.jpg", actualSize),
		"a viewport update changed the snapshot for a different pending path");
	const auto actualContinuation = state.TakePendingLoad(filename);
	Expect(actualContinuation.has_value() &&
		actualContinuation->viewportSnapshot.zoom == 1.0 &&
		!actualContinuation->viewportSnapshot.fitToWindow,
		"the cold-image continuation ignored Actual Size after selecting a saved 2x view");

	state.BeginLoad(filename, actualContinuation->viewportSnapshot);
	const jpegview_linux::ViewportSnapshot fitToWindow{true, true, false, 0.75, 1.25};
	Expect(state.UpdatePendingViewport(filename, fitToWindow),
		"Fit to Window did not update the selected pending image's snapshot");
	const auto fitContinuation = state.TakePendingLoad(filename);
	Expect(fitContinuation.has_value() &&
		fitContinuation->viewportSnapshot.fitToWindow &&
		fitContinuation->viewportSnapshot.fillWithCrop &&
		!fitContinuation->viewportSnapshot.noEnlarge &&
		fitContinuation->viewportSnapshot.zoom == fitToWindow.zoom &&
		fitContinuation->viewportSnapshot.relativeZoom == fitToWindow.relativeZoom,
		"the cold-image continuation did not consume the latest fit mode and zoom snapshot");
	Expect(!state.UpdatePendingViewport(filename, actualSize),
		"a viewport update succeeded after the pending continuation had been consumed");
}

void TestPendingImageIntentsRespectSourceGeneration() {
	TemporaryDirectory temporary;
	const fs::path firstPath = temporary.path() / "pending-first.jpg";
	const fs::path secondPath = temporary.path() / "pending-second.jpg";
	WriteText(firstPath, "first image");
	WriteText(secondPath, "second image");
	const jpegview_linux::SourceKey first =
		jpegview_linux::DescribeImageSource(firstPath).Key();
	const jpegview_linux::SourceKey second =
		jpegview_linux::DescribeImageSource(secondPath).Key();

	jpegview_linux::PendingImageIntents pending;
	jpegview_linux::RecentImageLoadState startupLoad;
	startupLoad.BeginLoad(firstPath, {true, false, true, 1.0, 1.0});
	pending.Begin(firstPath, first, 40);
	jpegview_linux::ViewportIntent startupPan;
	startupPan.type = jpegview_linux::ViewportIntentType::Pan;
	startupPan.deltaY = 48.0;
	Expect(pending.QueueViewport(firstPath, first, 40, startupPan),
		"startup scan-completion fixture could not queue a pre-catalog viewport intent");
	const fs::path provisionalSelection = fs::absolute(firstPath).lexically_normal();
	const bool startupPathChanged = startupLoad.LoadedPath() != provisionalSelection;
	const bool samePendingStartupLoad = pending.MatchesStartupLoad(true, true, true,
		provisionalSelection, first, 40);
	const bool currentPathChanged =
		!samePendingStartupLoad && startupPathChanged;
	Expect(startupPathChanged && samePendingStartupLoad &&
		!currentPathChanged &&
		!pending.MatchesStartupLoad(false, true, true,
			provisionalSelection, first, 40) &&
		!pending.MatchesStartupLoad(true, false, true,
			provisionalSelection, first, 40) &&
		!pending.MatchesStartupLoad(true, true, false,
			provisionalSelection, first, 40) &&
		!pending.MatchesStartupLoad(true, true, true,
			provisionalSelection, second, 40) &&
		!pending.MatchesStartupLoad(true, true, true,
			provisionalSelection, first, 41),
		"startup catalog completion did not distinguish the matching provisional cold load from a replacement");
	const auto startupContinuation = startupLoad.TakePendingLoad(firstPath);
	const auto startupActions = pending.Take(first, 40);
	Expect(startupContinuation.has_value() && startupActions.has_value() &&
		startupActions->actions.size() == 1 &&
		startupActions->actions.front().type == jpegview_linux::PendingImageIntentType::Viewport &&
		startupActions->actions.front().viewport.deltaY == 48.0,
		"startup catalog completion lost the provisional load state or its pre-scan input");
	pending.Begin(firstPath, first, 41);
	Expect(pending.MatchesSelection(firstPath, first, 41) &&
		!pending.MatchesSelection(firstPath, first, 42) &&
		!pending.MatchesSelection(secondPath, first, 41) &&
		pending.QueueTransform(firstPath, first, 41, IDM_ROTATE_90) &&
		pending.QueueTransform(firstPath, first, 41, IDM_MIRROR_H) &&
		pending.RequestTransition(first, 41),
		"pending rotate, mirror, or transition intent was rejected");
	Expect(!pending.Take(first, 42).has_value() &&
		!pending.Take(second, 41).has_value(),
		"a different load generation or source could consume pending image intents");
	const auto completed = pending.Take(first, 41);
	Expect(completed.has_value() && completed->actions.size() == 2 &&
		completed->actions[0].type == jpegview_linux::PendingImageIntentType::Transform &&
		completed->actions[0].transform == IDM_ROTATE_90 &&
		completed->actions[1].transform == IDM_MIRROR_H && completed->startTransition &&
		!pending.Take(first, 41).has_value(),
		"matching header completion did not drain transforms in order with its transition");
	pending.Begin(firstPath, first, 42);
	Expect(pending.QueueViewport(firstPath, first, 42, startupPan),
		"async-stage fixture could not queue its first viewport action");
	const auto drained = pending.Drain(first, 42);
	Expect(drained.has_value() && drained->actions.size() == 1 &&
		pending.MatchesSelection(firstPath, first, 42) && pending.ActionCount() == 0 &&
		pending.QueueTransform(firstPath, first, 42, IDM_ROTATE_90),
		"draining one async stage deactivated the selection or discarded later input");
	const auto laterStage = pending.Take(first, 42);
	Expect(laterStage.has_value() && laterStage->actions.size() == 1 &&
		laterStage->actions.front().type == jpegview_linux::PendingImageIntentType::Transform,
		"the next async stage did not receive input queued after a drain");

	pending.Begin(firstPath, first, 43);
	Expect(pending.QueueTransform(firstPath, first, 43, IDM_ROTATE_270),
		"replacement fixture could not queue its rotation");
	pending.Begin(secondPath, second, 44);
	Expect(!pending.Take(first, 43).has_value(),
		"replacement source retained a stale queued rotation");

	const jpegview_linux::ViewportSnapshot base{false, false, false, 2.0, 2.0};
	pending.Begin(firstPath, first, 45);
	jpegview_linux::ViewportIntent actualSize;
	actualSize.type = jpegview_linux::ViewportIntentType::ActualSize;
	jpegview_linux::ViewportIntent pan;
	pan.type = jpegview_linux::ViewportIntentType::Pan;
	pan.deltaX = 0.0;
	pan.deltaY = 80.0;
	Expect(pending.QueueTransform(firstPath, first, 45, IDM_ROTATE_90) &&
		pending.QueueViewport(firstPath, first, 45, actualSize) &&
		pending.QueueViewport(firstPath, first, 45, pan),
		"interleaved rotate, Actual Size, and pan actions were rejected");
	const auto interleaved = pending.Take(first, 45);
	Expect(interleaved.has_value() && interleaved->actions.size() == 3 &&
		interleaved->actions[0].type == jpegview_linux::PendingImageIntentType::Transform &&
		interleaved->actions[1].type == jpegview_linux::PendingImageIntentType::Viewport &&
		interleaved->actions[1].viewport.type == jpegview_linux::ViewportIntentType::ActualSize &&
		interleaved->actions[2].type == jpegview_linux::PendingImageIntentType::Viewport &&
		interleaved->actions[2].viewport.type == jpegview_linux::ViewportIntentType::Pan,
		"cold-image actions lost their original cross-category order");

	const auto replay = [&](const std::vector<jpegview_linux::PendingImageIntent>& actions,
		bool groupViewportActionsFirst, jpegview_linux::Viewport& viewport) {
		viewport.Restore(base, 1600, 400, 1000, 800);
		int width = 1600;
		int height = 400;
		const auto rotate = [&] {
			const jpegview_linux::ViewportSnapshot snapshot = viewport.Snapshot();
			std::swap(width, height);
			viewport.Restore(snapshot, width, height, 1000, 800);
		};
		const auto apply = [&](const jpegview_linux::PendingImageIntent& action) {
			if (action.type == jpegview_linux::PendingImageIntentType::Transform) {
				if (action.transform == IDM_ROTATE_90 || action.transform == IDM_ROTATE_270) rotate();
				return;
			}
			jpegview_linux::ApplyViewportIntent(viewport, action.viewport,
				width, height, 1000, 800);
			if (action.viewport.type == jpegview_linux::ViewportIntentType::ZoomByFactor ||
				action.viewport.type == jpegview_linux::ViewportIntentType::ZoomPreset ||
				action.viewport.type == jpegview_linux::ViewportIntentType::Pan) {
				viewport.ClampToView(width, height, 1000, 800);
			}
		};
		if (groupViewportActionsFirst) {
			for (const auto& action : actions) {
				if (action.type == jpegview_linux::PendingImageIntentType::Viewport) apply(action);
			}
			for (const auto& action : actions) {
				if (action.type == jpegview_linux::PendingImageIntentType::Transform) apply(action);
			}
		} else {
			for (const auto& action : actions) apply(action);
		}
	};
	jpegview_linux::Viewport orderedView;
	jpegview_linux::Viewport groupedView;
	replay(interleaved->actions, false, orderedView);
	replay(interleaved->actions, true, groupedView);
	ExpectNear(orderedView.Zoom(), 1.0, 1e-12,
		"Actual Size after rotation did not set the final source-pixel scale");
	ExpectNear(orderedView.OffsetY(), 80.0, 1e-12,
		"vertical pan after rotation and Actual Size was lost");
	ExpectNear(groupedView.OffsetY(), 0.0, 1e-12,
		"grouped viewport replay no longer reproduces the lost-pan failure");

	pending.Begin(firstPath, first, 47);
	Expect(pending.QueueCopySelection(firstPath, first, 47, 12, 18, 76, 64),
		"deferred copy-selection action was rejected");
	Expect(pending.QueueCopyImage(firstPath, first, 47, false) &&
		pending.QueueCropSelection(firstPath, first, 47, 4, 7, 90, 101),
		"deferred copy-image or crop-selection action was rejected");
	const auto copySelection = pending.Take(first, 47);
	Expect(copySelection.has_value() && copySelection->actions.size() == 3 &&
		copySelection->actions.front().type ==
			jpegview_linux::PendingImageIntentType::CopySelection &&
		copySelection->actions.front().selectionLeft == 12 &&
		copySelection->actions.front().selectionTop == 18 &&
		copySelection->actions.front().selectionRight == 76 &&
		copySelection->actions.front().selectionBottom == 64 &&
		copySelection->actions[1].type ==
			jpegview_linux::PendingImageIntentType::CopyImage &&
		!copySelection->actions[1].fullSize &&
		copySelection->actions[2].type ==
			jpegview_linux::PendingImageIntentType::CropSelection &&
		copySelection->actions[2].selectionLeft == 4 &&
		copySelection->actions[2].selectionTop == 7 &&
		copySelection->actions[2].selectionRight == 90 &&
		copySelection->actions[2].selectionBottom == 101,
		"deferred pixel actions lost their captured arguments or accepted order");

	pending.Begin(firstPath, first, 46);
	for (std::size_t index = 0; index < jpegview_linux::kMaximumPendingImageIntents; ++index) {
		const bool accepted = index % 2 == 0 ?
			pending.QueueViewport(firstPath, first, 46, pan) :
			pending.QueueTransform(firstPath, first, 46, IDM_ROTATE_90);
		Expect(accepted, "pending intent stress fixture hit the bound too early");
	}
	Expect(pending.ActionCount() == jpegview_linux::kMaximumPendingImageIntents &&
		!pending.CanQueue(firstPath, first, 46) &&
		!pending.QueueTransform(firstPath, first, 46, IDM_ROTATE_270) &&
		!pending.QueueViewport(firstPath, first, 46, actualSize) &&
		pending.ActionCount() == jpegview_linux::kMaximumPendingImageIntents,
		"blocked-load intent admission grew past its documented bound");
	const auto bounded = pending.Take(first, 46);
	Expect(bounded.has_value() &&
		bounded->actions.size() == jpegview_linux::kMaximumPendingImageIntents &&
		bounded->actions.back().type == jpegview_linux::PendingImageIntentType::Transform &&
		bounded->actions.back().transform == IDM_ROTATE_90,
		"capacity rejection changed the final accepted action or replay order");
	jpegview_linux::Viewport boundedReplay;
	jpegview_linux::Viewport boundedReplayAgain;
	replay(bounded->actions, false, boundedReplay);
	replay(bounded->actions, false, boundedReplayAgain);
	ExpectNear(boundedReplay.Zoom(), boundedReplayAgain.Zoom(), 1e-12,
		"bounded blocked-I/O replay was not deterministic");
	ExpectNear(boundedReplay.OffsetY(), boundedReplayAgain.OffsetY(), 1e-12,
		"capacity rejection changed the terminal viewport replay");

	jpegview_linux::DeferredExifDateAction deferredDate;
	deferredDate.Begin(firstPath, first, 9);
	Expect(deferredDate.MustDeferFor(firstPath, first) &&
		deferredDate.Defer(firstPath, first),
		"EXIF action was not recorded while metadata was pending");
	jpegview_linux::ExifMetadataResult result;
	result.generation = 8;
	result.source = first;
	result.metadata.dateTime = "2024:05:06 07:08:09";
	Expect(!deferredDate.Complete(result, firstPath, first).matchedPendingRead &&
		deferredDate.MustDeferFor(firstPath, first),
		"stale EXIF metadata consumed the current deferred date action");
	result.generation = 9;
	const auto metadataBeforeCommit = deferredDate.Complete(result, firstPath, first);
	Expect(metadataBeforeCommit.matchedPendingRead &&
		!metadataBeforeCommit.runDeferredAction &&
		deferredDate.MarkImageCommitted(firstPath, first).runDeferredAction,
		"metadata-first completion did not wait for image commit before touching the source");

	deferredDate.Begin(firstPath, first, 10);
	Expect(deferredDate.Defer(firstPath, first) &&
		!deferredDate.MarkImageCommitted(firstPath, first).runDeferredAction,
		"image-first completion ran the EXIF action before metadata arrived");
	result.generation = 10;
	const auto commitBeforeMetadata = deferredDate.Complete(result, firstPath, first);
	Expect(commitBeforeMetadata.matchedPendingRead &&
		commitBeforeMetadata.runDeferredAction,
		"image-first completion did not run the action when valid metadata arrived");

	deferredDate.Begin(firstPath, first, 11);
	Expect(deferredDate.Defer(firstPath, first) &&
		!deferredDate.MarkImageCommitted(secondPath, second).runDeferredAction &&
		!deferredDate.Complete(result, secondPath, second).matchedPendingRead,
		"a replaced owner inherited the deferred EXIF-date action");
}

void TestPendingImageOperationAdmissionPreservesIntentOrder() {
	using Admission = jpegview_linux::PendingImageOperationAdmission;
	Expect(jpegview_linux::PlanPendingImageOperationAdmission(
		false, 0, false, false) == Admission::StartNow,
		"a direct operation was rejected with no earlier work pending");
	Expect(jpegview_linux::PlanPendingImageOperationAdmission(
		false, 0, true, false) == Admission::WaitForSelectedCommit,
		"a direct operation did not wait for its selected image's display commit");
	Expect(jpegview_linux::PlanPendingImageOperationAdmission(
		false, 1, true, false) == Admission::Rejected &&
		jpegview_linux::PlanPendingImageOperationAdmission(
			false, 2, false, false) == Admission::Rejected,
		"a later direct operation passed previously accepted deferred intents");
	Expect(jpegview_linux::PlanPendingImageOperationAdmission(
		false, 2, false, true) == Admission::StartNow,
		"replay rejected its own front intent because it remained in the ordered queue");
	Expect(jpegview_linux::PlanPendingImageOperationAdmission(
		true, 0, true, false) == Admission::Rejected &&
		jpegview_linux::PlanPendingImageOperationAdmission(
			true, 1, false, true) == Admission::Rejected,
		"a second image operation was admitted before the active one completed");
}

void TestFileDialogSorting() {
	using SortMode = jpegview_linux::FileDialogSortMode;
	using FileTime = fs::file_time_type;
	using namespace std::chrono_literals;
	const FileTime epoch{};
	std::vector<jpegview_linux::FileDialogEntry> entries = {
		{fs::path("/pictures/z-last.jpg"), false, false, epoch + 40s},
		{fs::path("/pictures/B-folder"), true, false, epoch + 30s},
		{fs::path("/pictures"), true, true, epoch + 50s},
		{fs::path("/pictures/a-first.jpg"), false, false, epoch + 20s},
		{fs::path("/pictures/a-folder"), true, false, epoch + 10s},
	};

	jpegview_linux::SortFileDialogEntries(entries, SortMode::Name);
	Expect(entries[0].parent && entries[1].path.filename() == "a-folder" &&
		entries[2].path.filename() == "B-folder" && entries[3].path.filename() == "a-first.jpg" &&
		entries[4].path.filename() == "z-last.jpg",
		"open-dialog name sorting did not retain parent/directory grouping or ignore case");

	jpegview_linux::SortFileDialogEntries(entries, SortMode::ModificationDate);
	Expect(entries[0].parent && entries[1].path.filename() == "B-folder" &&
		entries[2].path.filename() == "a-folder" && entries[3].path.filename() == "z-last.jpg" &&
		entries[4].path.filename() == "a-first.jpg",
		"open-dialog modification-date sorting did not order each entry group newest first");

	entries[3].modificationTime = entries[4].modificationTime;
	jpegview_linux::SortFileDialogEntries(entries, SortMode::ModificationDate);
	Expect(entries[3].path.filename() == "a-first.jpg" && entries[4].path.filename() == "z-last.jpg",
		"open-dialog modification-date ties did not fall back to deterministic name order");
}

void TestFileDialogModelStateAndNavigation() {
	using Entry = jpegview_linux::FileDialogEntry;
	using FileTime = fs::file_time_type;
	using namespace std::chrono_literals;
	const FileTime epoch{};
	const std::vector<Entry> entries = {
		{fs::path("/pictures"), true, true, epoch + 100s},
		{fs::path("/pictures/b-folder"), true, false, epoch + 50s},
		{fs::path("/pictures/a-folder"), true, false, epoch + 10s},
		{fs::path("/pictures/03-last.jpg"), false, false, epoch + 40s},
		{fs::path(u8"/pictures/02-写真.jpg"), false, false, epoch + 30s},
		{fs::path("/pictures/01-first.jpg"), false, false, epoch + 20s},
	};

	jpegview_linux::FileDialogModel model;
	const jpegview_linux::FileDialogScrollbarGeometry disabledScrollbar =
		jpegview_linux::CalculateFileDialogScrollbarGeometry(5, 5, 2, 20, 100, 24);
	Expect(!disabledScrollbar.scrollable && disabledScrollbar.maximumScroll == 0 &&
		disabledScrollbar.thumbY == 20 && disabledScrollbar.thumbHeight == 100 &&
		jpegview_linux::FileDialogScrollForThumbPosition(disabledScrollbar, 60) == 0,
		"open-dialog scrollbar did not fill and disable its thumb when all rows fit");
	const jpegview_linux::FileDialogScrollbarGeometry scrollbarAtStart =
		jpegview_linux::CalculateFileDialogScrollbarGeometry(100, 10, 0, 20, 200, 24);
	const jpegview_linux::FileDialogScrollbarGeometry scrollbarAtMiddle =
		jpegview_linux::CalculateFileDialogScrollbarGeometry(100, 10, 45, 20, 200, 24);
	const jpegview_linux::FileDialogScrollbarGeometry scrollbarAtEnd =
		jpegview_linux::CalculateFileDialogScrollbarGeometry(100, 10, 90, 20, 200, 24);
	Expect(scrollbarAtStart.scrollable && scrollbarAtStart.thumbHeight == 24 &&
		scrollbarAtStart.thumbY == 20 && scrollbarAtMiddle.thumbY > scrollbarAtStart.thumbY &&
		scrollbarAtEnd.thumbY + scrollbarAtEnd.thumbHeight == 220 &&
		jpegview_linux::FileDialogScrollForThumbPosition(scrollbarAtStart, 20) == 0 &&
		jpegview_linux::FileDialogScrollForThumbPosition(scrollbarAtEnd, 196) == 90 &&
		std::abs(jpegview_linux::FileDialogScrollForThumbPosition(scrollbarAtMiddle,
			scrollbarAtMiddle.thumbY) - 45) <= 1,
		"open-dialog scrollbar thumb was not proportional or could not map to row offsets");
	const jpegview_linux::FileDialogScrollbarGeometry minimumThumbScrollbar =
		jpegview_linux::CalculateFileDialogScrollbarGeometry(10000, 1, 0, 4, 80, 22);
	Expect(minimumThumbScrollbar.thumbHeight == 22 &&
		jpegview_linux::FileDialogScrollForThumbPosition(minimumThumbScrollbar, -100) == 0 &&
		jpegview_linux::FileDialogScrollForThumbPosition(minimumThumbScrollbar, 10000) == 9999,
		"open-dialog scrollbar minimum thumb size or drag clamping was incorrect");
	model.Begin(false);
	model.SetEntries({});
	Expect(model.Entries().empty() && model.SelectedIndex() == -1,
		"empty open-dialog listing retained a selection");
	model.SetEntries(entries);
	Expect(model.Entries().size() == entries.size() && model.Entries()[0].parent &&
		model.Entries()[1].path.filename() == "a-folder" && model.SelectedIndex() == 1,
		"open-dialog model did not sort by name or select the first child");
	const fs::path sizedEntry = "/pictures/03-last.jpg";
	Expect(model.SetFileSize(sizedEntry, 1536) &&
		std::any_of(model.AllEntries().begin(), model.AllEntries().end(),
			[&sizedEntry](const Entry& entry) {
				return entry.path == sizedEntry && entry.fileSizeKnown && entry.fileSize == 1536;
			}) &&
		std::any_of(model.Entries().begin(), model.Entries().end(),
			[&sizedEntry](const Entry& entry) {
				return entry.path == sizedEntry && entry.fileSizeKnown && entry.fileSize == 1536;
			}),
		"file-size updates did not reach the sorted and filtered open-dialog rows");
	model.AppendFilter("03-last.jpg");
	model.ToggleSortMode(2);
	Expect(model.Entries().size() == 2 && model.Entries()[0].parent &&
		model.Entries()[1].path == sizedEntry && model.Entries()[1].fileSizeKnown &&
		model.Entries()[1].fileSize == 1536,
		"filtering or changing sort after metadata arrival detached the indexed row size");
	model.ClearFilter();
	model.ToggleSortMode(2);
	const fs::path encryptedArchivePath = "/pictures/locked.7z";
	jpegview_linux::FileDialogModel encryptedModel;
	encryptedModel.SetEntries({
		{fs::path("/pictures"), true, true},
		{encryptedArchivePath, true, false, {}, true},
	});
	encryptedModel.AppendFilter("locked");
	const int selectionBeforeEncryptedMark = encryptedModel.SelectedIndex();
	Expect(encryptedModel.MarkEncrypted(encryptedArchivePath) &&
		encryptedModel.SelectedIndex() == selectionBeforeEncryptedMark &&
		std::any_of(encryptedModel.Entries().begin(), encryptedModel.Entries().end(),
			[&encryptedArchivePath](const Entry& entry) {
				return entry.path == encryptedArchivePath && entry.encrypted;
			}) &&
		std::any_of(encryptedModel.AllEntries().begin(), encryptedModel.AllEntries().end(),
			[&encryptedArchivePath](const Entry& entry) {
				return entry.path == encryptedArchivePath && entry.encrypted;
			}),
		"encrypted archive marking did not update filtered/catalog rows without moving selection");
	model.SetEntriesInOrder({
		{fs::path("/recent/z/photo.jpg"), false, false},
		{fs::path("/recent/a/other.jpg"), false, false},
	}, true);
	Expect(model.Entries().size() == 2 &&
		model.Entries()[0].path.filename() == "photo.jpg" &&
		model.Entries()[1].path.filename() == "other.jpg" &&
		model.SelectedIndex() == 0,
		"open-dialog model did not preserve caller-provided recent MRU order");
	model.AppendFilter("/recent/a");
	Expect(model.Entries().size() == 1 &&
		model.Entries()[0].path.filename() == "other.jpg",
		"recent-dialog model did not filter against the full path in MRU order");
	model.ClearFilter();
	model.SetEntries(entries);
	model.ScrollBy(2, 2);
	Expect(model.Scroll() == 2 && model.SelectedIndex() == 1,
		"open-dialog wheel scrolling changed selection or ignored its scroll offset");
	model.ScrollBy(100, 2);
	Expect(model.Scroll() == 4,
		"open-dialog wheel scrolling did not clamp at the last complete viewport");
	model.ScrollBy(-100, 2);
	Expect(model.Scroll() == 0,
		"open-dialog wheel scrolling did not clamp at the first row");
	model.ScrollTo(100, 2);
	Expect(model.Scroll() == 4 && model.SelectedIndex() == 1,
		"open-dialog thumb scrolling did not clamp without changing selection");
	model.ScrollTo(-100, 2);
	Expect(model.Scroll() == 0 && model.SelectedIndex() == 1,
		"open-dialog thumb scrolling did not clamp back to the first row");

	model.MoveSelectionByPage(1, 2);
	Expect(model.SelectedIndex() == 3 && model.Scroll() == 2,
		"open-dialog page movement did not update selection and scroll together");
	model.SelectLast(2);
	Expect(model.SelectedIndex() == 5 && model.Scroll() == 4,
		"open-dialog End selection did not reveal the last row");
	model.SelectFirst(2);
	Expect(model.SelectedIndex() == 0 && model.Scroll() == 0,
		"open-dialog Home selection did not reveal the first row");
	model.MoveSelection(-1, 2);
	Expect(model.SelectedIndex() == 0, "open-dialog selection moved before its first row");
	Expect(model.Focus(fs::path("/pictures/03-last.jpg"), 2) &&
		model.SelectedEntry() != nullptr && model.SelectedEntry()->path.filename() == "03-last.jpg",
		"open-dialog could not focus an entry by path");
	Expect(!model.Focus(fs::path("/pictures/missing.jpg"), 2),
		"open-dialog reported focusing a missing path");
	model.SelectFirst(2);
	Expect(model.Focus(fs::path("/pictures/01-first.jpg"), 2) &&
		model.SelectedEntry() != nullptr && model.SelectedEntry()->path.filename() == "01-first.jpg" &&
		model.Scroll() == 2,
		"open-dialog could not focus the current file and scroll it into view");

	model.AppendFilter(u8"写真");
	Expect(model.Filter() == u8"写真" && model.Entries().size() == 2 &&
		model.Entries()[0].parent && model.Entries()[1].path.filename() == u8"02-写真.jpg" &&
		model.SelectedIndex() == 1,
		"open-dialog Unicode filtering did not retain the parent and matching image");
	Expect(model.BackspaceFilter() && model.Filter() == u8"写",
		"open-dialog Backspace removed a byte instead of one UTF-8 character");
	Expect(model.BackspaceFilter() && model.Filter().empty(),
		"open-dialog could not clear the final filter character");
	model.AppendFilter("missing");
	Expect(model.Entries().size() == 1 && model.Entries()[0].parent && model.SelectedIndex() == -1,
		"unmatched open-dialog filter left the parent row selected");
	model.ClearFilter();

	Expect(model.Focus(fs::path("/pictures/a-folder"), 10),
		"could not prepare sort-selection preservation test");
	model.ToggleSortMode(10);
	Expect(model.SortMode() == jpegview_linux::FileDialogSortMode::ModificationDate &&
		model.Entries()[1].path.filename() == "b-folder" &&
		model.SelectedEntry() != nullptr && model.SelectedEntry()->path.filename() == "a-folder",
		"open-dialog date sorting did not reorder entries while preserving selection");

	model.Begin(true);
	model.SetEntries(entries);
	Expect(model.SaveDialog() && model.Entries()[1].path.filename() == "a-folder" &&
		model.SelectedIndex() == 0,
		"save dialog did not retain fixed name sorting and first-row selection");
	model.AppendFilter("first");
	Expect(model.Filter().empty() && model.Entries().size() == entries.size(),
		"save dialog unexpectedly applied an open-dialog filter");
	model.ClearSelection();
	model.MoveSelection(1, 3);
	Expect(model.SelectedIndex() == 0, "save dialog did not move from an empty selection to the first row");
	model.Clear();
	Expect(model.Entries().empty() && model.AllEntries().empty() && model.SelectedEntry() == nullptr,
		"clearing the file-dialog model retained stale state");
	model.ScrollBy(5, 1);
	Expect(model.Scroll() == 0, "scrolling an empty file-dialog listing created a scroll offset");

	std::string malformed = std::string("ok") + static_cast<char>(0x80);
	Expect(jpegview_linux::EraseLastUtf8CodePoint(malformed) && malformed == "ok",
		"UTF-8 erasure damaged valid text before a malformed trailing byte");
}

void TestFileDialogModelIndexedLargeCatalogUpdates() {
	using Entry = jpegview_linux::FileDialogEntry;
	constexpr std::size_t entryCount = 15000;
	const fs::path directory = fs::temp_directory_path() / "jpegview-indexed-dialog-catalog";
	std::vector<Entry> entries;
	entries.reserve(entryCount);
	std::vector<fs::path> paths;
	paths.reserve(entryCount);
	for (std::size_t index = 0; index < entryCount; ++index) {
		std::ostringstream name;
		name << "photo-" << std::setw(5) << std::setfill('0') << index << ".jpg";
		paths.push_back(directory / name.str());
		entries.emplace_back(paths.back(), false, false);
	}
	jpegview_linux::FileDialogModel model;
	model.SetEntriesInOrder(std::move(entries));
	bool allUpdated = true;
	for (std::size_t index = 0; index < entryCount; ++index) {
		allUpdated = model.SetFileSize(paths[index], index + 1) && allUpdated;
	}
	std::size_t knownSizeCount = 0;
	for (const Entry& entry : model.AllEntries()) {
		if (entry.fileSizeKnown && entry.fileSize > 0) ++knownSizeCount;
	}
	const std::string finalName = paths.back().filename().string();
	model.AppendFilter(finalName);
	const auto filtered = model.Entries();
	Expect(allUpdated && knownSizeCount == entryCount && filtered.size() == 1 &&
		filtered[0].path == paths.back() && filtered[0].fileSizeKnown &&
		filtered[0].fileSize == entryCount && model.Focus(paths.back(), 10),
		"15,000 exact-index metadata updates lost a size during later filtering or focus");
}

void TestFileDialogDirectorySummaries() {
	TemporaryDirectory temporary;
	const fs::path album = temporary.path() / "album";
	fs::create_directories(album / "first-subdir");
	fs::create_directories(album / "second-subdir");
	WriteText(album / "photo.JPG", "image fixture");
	WriteText(album / "scan.ppm", "image fixture");
	WriteText(album / "notes.txt", "not an image");
	WriteText(album / "first-subdir" / "recursive.png", "must not be counted");
	WriteZipArchive(album / "archive.ZIP", {{"contained-photo.jpg", album / "photo.JPG"}});

	const jpegview_linux::DirectorySummary summary =
		jpegview_linux::CountImmediateDirectoryContents(album);
	Expect(summary.imageCount == 2 && summary.subdirectoryCount == 3,
		"directory summary did not count immediate compatible images, subdirectories, and archive folders");
	Expect(jpegview_linux::FormatDirectorySummary(summary) == "2 images, 3 dirs",
		"directory summary plural formatting is incorrect");
	Expect(jpegview_linux::FormatDirectorySummary({1, 1}) == "1 image, 1 dir",
		"directory summary singular formatting is incorrect");

	jpegview_linux::DirectorySummaryLoader loader;
	loader.Request({album}, 17);
	std::vector<jpegview_linux::DirectorySummaryResult> results;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (results.empty() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		results = loader.TakeReady();
	}
	Expect(results.size() == 1 && results[0].generation == 17 &&
		results[0].directory == album && results[0].summary.imageCount == 2 &&
		results[0].summary.subdirectoryCount == 3,
		"background directory summary loader did not publish the requested result");
	const fs::path empty = temporary.path() / "empty";
	fs::create_directory(empty);
	loader.Request({album, empty, album, empty, album}, 23);
	results.clear();
	bool summaryDrainWasBounded = true;
	std::size_t drainedSummaryCount = 0;
	const auto boundedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (drainedSummaryCount < 5 && std::chrono::steady_clock::now() < boundedDeadline) {
		std::vector<jpegview_linux::DirectorySummaryResult> batch = loader.TakeReady(2);
		if (batch.size() > 2) summaryDrainWasBounded = false;
		drainedSummaryCount += batch.size();
		results.insert(results.end(), std::make_move_iterator(batch.begin()),
			std::make_move_iterator(batch.end()));
		if (results.size() < 5) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(summaryDrainWasBounded && results.size() == 5 &&
		std::all_of(results.begin(), results.end(), [](const auto& result) {
			return result.generation == 23;
		}), "directory summary result draining exceeded its per-update bound");

	loader.Request({album}, 18);
	loader.Request({empty}, 19);
	results.clear();
	const auto replacementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (results.empty() && std::chrono::steady_clock::now() < replacementDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		results = loader.TakeReady();
	}
	Expect(results.size() == 1 && results[0].generation == 19 && results[0].directory == empty,
		"background directory summary loader published stale work after a replacement request");

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	const fs::path summaryAdmissionBlockerPath =
		temporary.path() / "summary-admission-blocker.jpg";
	WriteText(summaryAdmissionBlockerPath, "admission blocker");
	coordinator.SetForegroundPending(false);
	const bool summaryCoordinatorIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(2));
	const jpegview_linux::WorkContext summaryAdmissionBlockerContext =
		jpegview_linux::MakePathWorkContext(summaryAdmissionBlockerPath,
			jpegview_linux::SourceWorkPriority::Metadata);
	jpegview_linux::SourceWorkLease summaryAdmissionBlocker = coordinator.Acquire(
		summaryAdmissionBlockerContext, summaryAdmissionBlockerPath);
	const bool summaryAdmissionBlockerAcquired =
		static_cast<bool>(summaryAdmissionBlocker);
	const bool summaryBlockerExclusive = summaryAdmissionBlockerAcquired &&
		coordinator.Snapshot().activeSpeculative == 1 &&
		coordinator.Snapshot().waitingSpeculative == 0;
	loader.Request({album}, 20);
	const bool summaryQueuedForMetadataLane = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 1 && snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	coordinator.SetForegroundPending(true);
	summaryAdmissionBlocker.Reset();
	const auto yieldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!loader.IsYieldingForForeground() &&
		std::chrono::steady_clock::now() < yieldDeadline) {
		std::this_thread::yield();
	}
	const bool summaryYielded = loader.IsYieldingForForeground();
	coordinator.SetForegroundPending(false);
	results.clear();
	const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (results.empty() && std::chrono::steady_clock::now() < retryDeadline) {
		results = loader.TakeReady();
		if (results.empty()) std::this_thread::yield();
	}
	std::ostringstream summaryFailure;
	summaryFailure << "directory summary did not retry after its foreground interruption cleared "
		"before retry (idle=" << summaryCoordinatorIdle << ", blocker="
		<< summaryBlockerExclusive << ", queued=" << summaryQueuedForMetadataLane
		<< ", yielded=" << summaryYielded << ", results=" << results.size();
	if (!results.empty()) {
		summaryFailure << ", generation=" << results.front().generation
			<< ", images=" << results.front().summary.imageCount
			<< ", subdirectories=" << results.front().summary.subdirectoryCount
			<< ", failure=" << results.front().failure.message;
	}
	Expect(summaryCoordinatorIdle && summaryAdmissionBlockerAcquired && summaryBlockerExclusive &&
		summaryQueuedForMetadataLane && summaryYielded &&
		results.size() == 1 && results.front().generation == 20 &&
		results.front().summary.imageCount == 2 &&
		results.front().summary.subdirectoryCount == 3 && !results.front().failure.Failed(),
		summaryFailure.str());

	const fs::path emptyAdmissionBlockerPath =
		temporary.path() / "empty-summary-admission-blocker.jpg";
	WriteText(emptyAdmissionBlockerPath, "empty directory admission blocker");
	const bool emptyCoordinatorIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0;
		}, std::chrono::seconds(2));
	const jpegview_linux::WorkContext emptyBlockerContext =
		jpegview_linux::MakePathWorkContext(emptyAdmissionBlockerPath,
			jpegview_linux::SourceWorkPriority::Metadata);
	jpegview_linux::SourceWorkLease emptyAdmissionBlocker = coordinator.Acquire(
		emptyBlockerContext, emptyAdmissionBlockerPath);
	const bool emptyBlockerAcquired = static_cast<bool>(emptyAdmissionBlocker);
	loader.Request({empty}, 22);
	const bool emptySummaryQueuedBeforeOpen = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeSpeculative == 1 && snapshot.waitingSpeculative == 1;
		}, std::chrono::seconds(2));
	coordinator.SetForegroundPending(true);
	emptyAdmissionBlocker.Reset();
	const auto emptyYieldDeadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!loader.IsYieldingForForeground() &&
		std::chrono::steady_clock::now() < emptyYieldDeadline) {
		std::this_thread::yield();
	}
	const bool emptySummaryYielded = loader.IsYieldingForForeground();
	const bool noEmptySummaryWhileForegroundPending = loader.TakeReady().empty();
	coordinator.SetForegroundPending(false);
	results.clear();
	const auto emptyRetryDeadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (results.empty() && std::chrono::steady_clock::now() < emptyRetryDeadline) {
		results = loader.TakeReady();
		if (results.empty()) std::this_thread::yield();
	}
	Expect(emptyCoordinatorIdle && emptyBlockerAcquired && emptySummaryQueuedBeforeOpen &&
		emptySummaryYielded && noEmptySummaryWhileForegroundPending &&
		results.size() == 1 && results.front().generation == 22 &&
		results.front().directory == empty &&
		results.front().summary.imageCount == 0 &&
		results.front().summary.subdirectoryCount == 0 && !results.front().failure.Failed(),
		"empty directory summary opened outside source admission or published during foreground suppression");

	coordinator.SetForegroundPending(true);
	auto stoppingLoader = std::make_unique<jpegview_linux::DirectorySummaryLoader>();
	stoppingLoader->Request({album}, 21);
	const auto shutdownYieldDeadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!stoppingLoader->IsYieldingForForeground() &&
		std::chrono::steady_clock::now() < shutdownYieldDeadline) {
		std::this_thread::yield();
	}
	const bool shutdownYielded = stoppingLoader->IsYieldingForForeground();
	std::mutex shutdownMutex;
	std::condition_variable shutdownChanged;
	bool shutdownReturned = false;
	std::thread shutdownThread([&] {
		stoppingLoader->Shutdown();
		stoppingLoader.reset();
		{
			std::lock_guard<std::mutex> lock(shutdownMutex);
			shutdownReturned = true;
		}
		shutdownChanged.notify_all();
	});
	bool shutdownFinishedWithoutGateRelease = false;
	{
		std::unique_lock<std::mutex> lock(shutdownMutex);
		shutdownFinishedWithoutGateRelease = shutdownChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return shutdownReturned; });
	}
	if (!shutdownFinishedWithoutGateRelease) coordinator.SetForegroundPending(false);
	shutdownThread.join();
	coordinator.SetForegroundPending(false);
	Expect(shutdownYielded && shutdownFinishedWithoutGateRelease,
		"directory summary destruction waited for foreground work or renderer progress");
}

void TestFileDialogDirectoryLoader() {
	using namespace std::chrono_literals;
	using Entry = jpegview_linux::FileDialogEntry;
	TemporaryDirectory temporary;
	const fs::path album = temporary.path() / "album";
	const fs::path subdirectory = album / "subdirectory";
	const fs::path ordinaryZipNamedDirectory = album / "ordinary.zip";
	fs::create_directories(subdirectory);
	fs::create_directories(ordinaryZipNamedDirectory);
	const fs::path image = album / "photo.jpg";
	WriteText(image, "directory listing size fixture");
	WriteText(album / "notes.txt", "not an image");
	const fs::path archive = album / "bundle.zip";
	WriteZipArchive(archive, {{"inside.jpg", image}, {"nested/child.jpg", image}});
	const fs::path empty = temporary.path() / "empty";
	fs::create_directory(empty);
	const fs::path missing = temporary.path() / "missing";
	const jpegview_linux::FileDialogListingPolicy policy{};
	std::vector<Entry> cancellableSortEntries;
	for (int index = 511; index >= 0; --index) {
		cancellableSortEntries.emplace_back(temporary.path() /
			("sort-" + std::to_string(index) + ".jpg"), false, false);
	}
	int sortContinuationChecks = 0;
	const jpegview_linux::FileDialogEntrySortOrders interruptedSort =
		jpegview_linux::BuildFileDialogEntrySortOrders(cancellableSortEntries,
			[&sortContinuationChecks] { return ++sortContinuationChecks < 6; });
	Expect(!interruptedSort.completed && sortContinuationChecks == 6,
		"directory row-order preparation did not stop from its comparison-batch cancellation check");
	jpegview_linux::FileDialogDirectoryLoader loader;
	const auto waitForResult = [](jpegview_linux::FileDialogDirectoryLoader& source) {
		std::vector<jpegview_linux::FileDialogDirectoryResult> results;
		const auto deadline = std::chrono::steady_clock::now() + 4s;
		while (results.empty() && std::chrono::steady_clock::now() < deadline) {
			results = source.TakeReady();
			if (results.empty()) std::this_thread::sleep_for(1ms);
		}
		return results;
	};

	loader.Request(album, 71, policy);
	std::vector<jpegview_linux::FileDialogDirectoryResult> results = waitForResult(loader);
	Expect(results.size() == 1 && results.front().generation == 71 &&
		results.front().directory == album && !results.front().archiveLocation &&
		results.front().locationDisplayName == album.string() &&
		results.front().archiveFormatName.empty() &&
		results.front().error.empty(),
		"filesystem Browse listing did not publish a completed generation");
	if (!results.empty()) {
		jpegview_linux::FileDialogModel model;
		model.SetEntriesWithPreparedOrder(std::move(results.front().entries),
			std::move(results.front().sortOrders));
		const auto findEntry = [&model](const fs::path& path) -> const Entry* {
			for (const Entry& entry : model.AllEntries()) {
				if (entry.path == path) return &entry;
			}
			return nullptr;
		};
		const Entry* imageEntry = findEntry(image);
		const Entry* archiveEntry = findEntry(archive);
		const Entry* ordinaryZipDirectory = findEntry(ordinaryZipNamedDirectory);
		Expect(model.AllEntries().size() == 5 && model.AllEntries().front().parent &&
			imageEntry != nullptr && imageEntry->fileSizeKnown &&
			imageEntry->fileSize == fs::file_size(image) &&
			archiveEntry != nullptr && archiveEntry->directory &&
			archiveEntry->archiveContainer && archiveEntry->archiveFormatName == "ZIP" &&
			ordinaryZipDirectory != nullptr && ordinaryZipDirectory->directory &&
			!ordinaryZipDirectory->archiveContainer &&
			ordinaryZipDirectory->archiveFormatName.empty(),
			"filesystem listing lost its parent, immediate folders, archive rows, or file sizes");
	}

	loader.Request(archive, 72, policy);
	results = waitForResult(loader);
	const auto member = results.empty() ?
		std::vector<jpegview_linux::FileDialogEntry>::const_iterator{} :
		std::find_if(results.front().entries.begin(), results.front().entries.end(),
			[](const Entry& entry) { return entry.path.filename() == "inside.jpg"; });
	Expect(results.size() == 1 && results.front().generation == 72 &&
		results.front().archiveLocation && results.front().error.empty() &&
		results.front().locationDisplayName == archive.string() + " (ZIP)" &&
		results.front().archiveFormatName == "ZIP" &&
		member != results.front().entries.end() && member->archiveMember &&
		member->fileSizeKnown && member->fileSize == fs::file_size(image) &&
		member->sourceDescriptor.Valid(),
		"generic Browse directory loader did not preserve archive member identity and size");
	const fs::path nestedArchiveDirectory = archive / "nested";
	loader.Request(nestedArchiveDirectory, 79, policy);
	results = waitForResult(loader);
	Expect(results.size() == 1 && results.front().generation == 79 &&
		results.front().archiveLocation && results.front().error.empty() &&
		results.front().locationDisplayName == archive.string() + "!/nested" &&
		results.front().archiveFormatName == "ZIP" &&
		std::any_of(results.front().entries.begin(), results.front().entries.end(),
			[](const Entry& entry) { return entry.path.filename() == "child.jpg"; }),
		"archive member directory did not carry its prepared location and format labels");

	loader.Request(empty, 73, policy);
	results = waitForResult(loader);
	Expect(results.size() == 1 && results.front().generation == 73 &&
		results.front().error.empty() && results.front().entries.size() == 1 &&
		results.front().entries.front().parent,
		"empty filesystem listing did not return only its parent row");
	loader.Request(missing, 74, policy);
	results = waitForResult(loader);
	Expect(results.size() == 1 && results.front().generation == 74 &&
		!results.front().error.empty() && results.front().entries.size() == 1 &&
		results.front().entries.front().parent,
		"unreadable or missing filesystem listing did not publish a useful failure");
	jpegview_linux::FileDialogListingPolicy savePolicy;
	savePolicy.saveDialog = true;
	savePolicy.includeArchives = false;
	loader.Request(album, 75, savePolicy);
	results = waitForResult(loader);
	Expect(results.size() == 1 && results.front().policy.saveDialog &&
		std::none_of(results.front().entries.begin(), results.front().entries.end(),
			[&archive](const Entry& entry) { return entry.path == archive; }) &&
		std::none_of(results.front().entries.begin(), results.front().entries.end(),
			[](const Entry& entry) { return entry.path.filename() == "notes.txt"; }),
		"save-dialog listing policy exposed archives or non-image files");
	jpegview_linux::FileDialogListingPolicy restorePolicy;
	restorePolicy.includeNonImageFiles = true;
	restorePolicy.includeArchives = false;
	loader.Request(album, 76, restorePolicy);
	results = waitForResult(loader);
	Expect(results.size() == 1 && results.front().policy.includeNonImageFiles &&
		std::any_of(results.front().entries.begin(), results.front().entries.end(),
			[](const Entry& entry) { return entry.path.filename() == "notes.txt"; }) &&
		std::none_of(results.front().entries.begin(), results.front().entries.end(),
			[&archive](const Entry& entry) { return entry.path == archive; }),
		"parameter-restore listing did not include files while hiding archives");
	const fs::path throwingPath = temporary.path() / "throwing-listing";
	const fs::path recoveredPath = temporary.path() / "recovered-listing";
	const auto exceptionEnumerator = [&throwingPath](const fs::path& directory,
		jpegview_linux::FileDialogListingPolicy, const std::function<bool()>&) {
		if (directory == throwingPath) throw std::runtime_error("synthetic listing failure");
		jpegview_linux::FileDialogDirectoryResult result;
		result.directory = directory;
		result.entries.emplace_back(directory / "recovered.jpg", false, false);
		return result;
	};
	jpegview_linux::FileDialogDirectoryLoader exceptionLoader(exceptionEnumerator);
	exceptionLoader.Request(throwingPath, 80, policy);
	results = waitForResult(exceptionLoader);
	Expect(results.size() == 1 && results.front().generation == 80 &&
		results.front().directory == throwingPath &&
		results.front().error == "synthetic listing failure",
		"directory listing callback exception was not published as a current failure");
	exceptionLoader.Request(recoveredPath, 81, policy);
	results = waitForResult(exceptionLoader);
	Expect(results.size() == 1 && results.front().generation == 81 &&
		results.front().directory == recoveredPath && results.front().error.empty() &&
		results.front().entries.size() == 1,
		"directory listing worker did not service a valid request after callback failure");

	const fs::path slow = temporary.path() / "slow";
	const fs::path replacement = temporary.path() / "replacement";
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false;
	bool release = false;
	const auto enumerator = [&mutex, &changed, &entered, &release, &slow](
		const fs::path& directory, jpegview_linux::FileDialogListingPolicy,
		const std::function<bool()>&) {
		if (directory == slow) {
			std::unique_lock<std::mutex> lock(mutex);
			entered = true;
			changed.notify_all();
			changed.wait(lock, [&release] { return release; });
		}
		jpegview_linux::FileDialogDirectoryResult result;
		result.directory = directory;
		result.entries.emplace_back(directory / "listed.jpg", false, false);
		return result;
	};
	jpegview_linux::FileDialogDirectoryLoader replacing(enumerator);
	auto slowRequest = std::async(std::launch::async, [&replacing, &slow, policy] {
		replacing.Request(slow, 77, policy);
	});
	const bool requestReturned =
		slowRequest.wait_for(2s) == std::future_status::ready;
	if (requestReturned) slowRequest.get();
	bool slowEnumerationEntered = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		slowEnumerationEntered = changed.wait_for(lock, 2s, [&entered] { return entered; });
	}
	replacing.Request(replacement, 78, policy);
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	changed.notify_all();
	results = waitForResult(replacing);
	Expect(requestReturned && slowEnumerationEntered && results.size() == 1 &&
		results.front().generation == 78 && results.front().directory == replacement,
		"slow listing blocked request submission or published a replaced directory result");

	std::mutex cancelMutex;
	std::condition_variable cancelChanged;
	bool cancelEntered = false;
	bool cancelRelease = false;
	const fs::path cancelPath = temporary.path() / "cancel";
	const fs::path afterClear = temporary.path() / "after-clear";
	const auto cancelEnumerator = [&cancelMutex, &cancelChanged, &cancelEntered,
		&cancelRelease, &cancelPath](const fs::path& directory,
		jpegview_linux::FileDialogListingPolicy, const std::function<bool()>&) {
		if (directory == cancelPath) {
			std::unique_lock<std::mutex> lock(cancelMutex);
			cancelEntered = true;
			cancelChanged.notify_all();
			cancelChanged.wait(lock, [&cancelRelease] { return cancelRelease; });
		}
		jpegview_linux::FileDialogDirectoryResult result;
		result.directory = directory;
		result.entries.emplace_back(directory / "listed.jpg", false, false);
		return result;
	};
	jpegview_linux::FileDialogDirectoryLoader cleared(cancelEnumerator);
	cleared.Request(cancelPath, 77, policy);
	bool cancellationEntered = false;
	{
		std::unique_lock<std::mutex> lock(cancelMutex);
		cancellationEntered = cancelChanged.wait_for(lock, 2s,
			[&cancelEntered] { return cancelEntered; });
	}
	cleared.Clear(78);
	cleared.Request(afterClear, 79, policy);
	{
		std::lock_guard<std::mutex> lock(cancelMutex);
		cancelRelease = true;
	}
	cancelChanged.notify_all();
	results = waitForResult(cleared);
	Expect(cancellationEntered && results.size() == 1 &&
		results.front().generation == 79 && results.front().directory == afterClear,
		"clearing a blocked listing failed to cancel it before the next directory completed");

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	std::vector<jpegview_linux::CpuWorkLease> occupiedCpu;
	const std::size_t cpuLimit = jpegview_linux::HardwareAwareCpuWorkerCount();
	for (std::size_t index = 0; index < cpuLimit; ++index) {
		jpegview_linux::WorkContext foregroundCpu;
		foregroundCpu.sourcePriority = jpegview_linux::SourceWorkPriority::Foreground;
		auto lease = coordinator.AcquireCpu(foregroundCpu);
		if (lease) occupiedCpu.push_back(std::move(lease));
	}
	auto catalog = std::make_shared<std::vector<Entry>>();
	for (int index = 1023; index >= 0; --index) {
		catalog->emplace_back(temporary.path() /
			("admitted-sort-" + std::to_string(index) + ".jpg"), false, false);
	}
	const auto sortEnumerator = [catalog](const fs::path& directory,
		jpegview_linux::FileDialogListingPolicy, const std::function<bool()>&) {
		jpegview_linux::FileDialogDirectoryResult result;
		result.directory = directory;
		result.entries = *catalog;
		return result;
	};
	const fs::path supersededSort = temporary.path() / "superseded-sort";
	const fs::path currentSort = temporary.path() / "current-sort";
	jpegview_linux::FileDialogDirectoryLoader cpuAdmitted(sortEnumerator);
	cpuAdmitted.Request(supersededSort, 91, policy);
	const bool sortingWaitedForCpu = coordinator.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.waitingCpu >= 1;
		}, 2s);
	const bool sortWithheldWithoutCpu = cpuAdmitted.TakeReady().empty();
	cpuAdmitted.Request(currentSort, 92, policy);
	for (jpegview_linux::CpuWorkLease& lease : occupiedCpu) lease.Reset();
	results = waitForResult(cpuAdmitted);
	Expect(sortingWaitedForCpu && sortWithheldWithoutCpu && results.size() == 1 &&
		results.front().generation == 92 && results.front().directory == currentSort &&
		results.front().sortOrders.completed,
		"directory sorting bypassed shared CPU admission or published a superseded generation");

	const auto idleEnumerator = [](const fs::path& directory,
		jpegview_linux::FileDialogListingPolicy,
		const std::function<bool()>&) {
		jpegview_linux::FileDialogDirectoryResult result;
		result.directory = directory;
		result.entries.emplace_back(directory / "idle.jpg", false, false);
		return result;
	};
	auto idleLoader = std::make_unique<jpegview_linux::FileDialogDirectoryLoader>(idleEnumerator);
	idleLoader->Request(temporary.path() / "idle-shutdown", 90, policy);
	results = waitForResult(*idleLoader);
	Expect(results.size() == 1 && results.front().generation == 90,
		"idle-shutdown listing did not finish before testing worker teardown");
	std::promise<void> shutdownFinished;
	std::future<void> shutdownResult = shutdownFinished.get_future();
	std::thread shutdownThread([loader = std::move(idleLoader),
		finished = std::move(shutdownFinished), policy]() mutable {
		loader->Shutdown();
		loader->Shutdown();
		loader->Request("ignored-after-shutdown", 91, policy);
		Expect(loader->TakeReady().empty(),
			"shut down directory loader accepted new requests");
		loader.reset();
		finished.set_value();
	});
	const bool idleShutdownCompleted = shutdownResult.wait_for(2s) == std::future_status::ready;
	if (idleShutdownCompleted) shutdownThread.join();
	else shutdownThread.detach();
	Expect(idleShutdownCompleted,
		"destroying an idle directory loader did not wake and join its worker promptly");
}

void TestFileDialogListingCompletionPreservesUserState() {
	using Entry = jpegview_linux::FileDialogEntry;
	std::string message = jpegview_linux::FileDialogListingLoadingMessage();
	jpegview_linux::UpdateFileDialogListingMessage(message, {});
	Expect(message.empty(), "successful listing left its loading status visible");
	message = "File exists; press ENTER to overwrite or ESC to cancel";
	jpegview_linux::UpdateFileDialogListingMessage(message, {});
	jpegview_linux::UpdateFileDialogListingMessage(message, "Cannot read folder: permission denied");
	Expect(message == "File exists; press ENTER to overwrite or ESC to cancel",
		"late listing completion replaced an operation-owned overwrite confirmation");
	message = jpegview_linux::FileDialogListingLoadingMessage();
	jpegview_linux::UpdateFileDialogListingMessage(message,
		"Cannot read folder: permission denied");
	Expect(message == "Cannot read folder: permission denied",
		"directory listing failure did not replace its loading status");

	jpegview_linux::FileDialogModel restoreModel;
	restoreModel.Begin(false);
	const fs::path root = fs::temp_directory_path() / "restore-filter-state";
	restoreModel.SetEntriesInOrder({Entry{root / "..", true, true}});
	restoreModel.AppendFilter("matching-backup");
	std::vector<Entry> entries = {
		Entry{root / "..", true, true},
		Entry{root / "matching-backup.jvdb", false, false},
		Entry{root / "other-backup.jvdb", false, false},
	};
	jpegview_linux::FileDialogEntrySortOrders orders =
		jpegview_linux::BuildFileDialogEntrySortOrders(entries);
	restoreModel.SetEntriesWithPreparedOrder(std::move(entries), std::move(orders));
	if (jpegview_linux::FileDialogShouldClearSelectionAfterListing(false, true, true)) {
		restoreModel.ClearSelection();
	}
	const Entry* selected = restoreModel.SelectedEntry();
	Expect(selected != nullptr && selected->path.filename() == "matching-backup.jvdb",
		"queued restore activation lost the row selected by its filter when listing completed");
	Expect(jpegview_linux::FileDialogShouldClearSelectionAfterListing(true, false, true) &&
		jpegview_linux::FileDialogShouldClearSelectionAfterListing(false, true, false) &&
		!jpegview_linux::FileDialogShouldClearSelectionAfterListing(false, true, true) &&
		!jpegview_linux::FileDialogShouldClearSelectionAfterListing(false, false, true),
		"listing selection policy did not preserve only pending filtered restore activation");
}

void TestColdArchiveRowDescriptorsAreCapturedOffThread() {
	TemporaryDirectory temporary;
	const fs::path browseFile = temporary.path() / "browse-row.png";
	const fs::path archivePayload = temporary.path() / "payload.bin";
	const fs::path recentArchive = temporary.path() / "recent-cold.zip";
	WriteBytes(browseFile, {1, 2, 3, 4});
	WriteBytes(archivePayload, {9, 8, 7, 6, 5});
	WriteZipArchive(recentArchive, {{"recent.png", archivePayload}});
	const fs::path recentMember = recentArchive / "recent.png";
	const jpegview_linux::SourceDescriptor browsePlaceholder(browseFile, {}, {});
	const jpegview_linux::SourceDescriptor recentPlaceholder(recentMember, {}, {});
	Expect(!browsePlaceholder.Valid() && !recentPlaceholder.Valid(),
		"file-dialog rows were not left as uncaptured placeholders on the event thread");

	std::mutex mutex;
	std::condition_variable changed;
	bool archiveCaptureStarted = false;
	bool releaseArchiveCapture = false;
	bool requestReturned = false;
	std::thread::id requestThread;
	std::thread::id archiveCaptureThread;
	const auto capture = [&](const jpegview_linux::SourceDescriptor& requested) {
		if (requested.LogicalPath() == recentMember) {
			std::unique_lock<std::mutex> lock(mutex);
			archiveCaptureThread = std::this_thread::get_id();
			archiveCaptureStarted = true;
			changed.notify_all();
			changed.wait(lock, [&] { return releaseArchiveCapture; });
		}
		return jpegview_linux::DescribeImageSource(requested.LogicalPath());
	};
	jpegview_linux::FileDialogFileSizeLoader loader(capture);
	auto requestCall = std::async(std::launch::async, [&] {
		{
			std::lock_guard<std::mutex> lock(mutex);
			requestThread = std::this_thread::get_id();
		}
		loader.RequestSources({browsePlaceholder, recentPlaceholder}, 82);
		{
			std::lock_guard<std::mutex> lock(mutex);
			requestReturned = true;
		}
		changed.notify_all();
	});
	bool requestReturnedWhileArchiveCaptureBlocked = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		requestReturnedWhileArchiveCaptureBlocked =
			changed.wait_for(lock, std::chrono::seconds(2), [&] {
				return archiveCaptureStarted && requestReturned;
		});
	}
	bool archiveCaptureReached = false;
	{
		std::lock_guard<std::mutex> lock(mutex);
		archiveCaptureReached = archiveCaptureStarted;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseArchiveCapture = true;
	}
	changed.notify_all();
	const bool requestCallCompleted =
		requestCall.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	if (requestCallCompleted) requestCall.get();
	Expect(archiveCaptureReached && requestReturnedWhileArchiveCaptureBlocked &&
		requestCallCompleted && archiveCaptureThread != requestThread,
		"cold archive catalog capture blocked or ran on the file-dialog request thread");
	std::vector<jpegview_linux::FileDialogFileSizeResult> results;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.size() < 2 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		auto ready = loader.TakeReady();
		results.insert(results.end(), std::make_move_iterator(ready.begin()),
			std::make_move_iterator(ready.end()));
	}
	const auto browseResult = std::find_if(results.begin(), results.end(),
		[&](const auto& result) { return result.path == browseFile; });
	const auto recentResult = std::find_if(results.begin(), results.end(),
		[&](const auto& result) { return result.path == recentMember; });
	Expect(results.size() == 2 && browseResult != results.end() &&
		recentResult != results.end() && browseResult->observedSource.Valid() &&
		recentResult->observedSource.Valid() &&
		recentResult->observedSource.Metadata().archiveMember &&
		recentResult->size == fs::file_size(archivePayload),
		"Browse/Recents row placeholders did not receive worker-captured source metadata");

	std::vector<fs::path> boundedPaths;
	std::vector<jpegview_linux::SourceDescriptor> boundedSources;
	for (int index = 0; index < 5; ++index) {
		const fs::path path = temporary.path() / ("bounded-" + std::to_string(index) + ".jpg");
		WriteBytes(path, {static_cast<std::uint8_t>(index)});
		boundedPaths.push_back(path);
		boundedSources.emplace_back(path, jpegview_linux::SourceIdentity{},
			jpegview_linux::SourceMetadata{});
	}
	jpegview_linux::FileDialogFileSizeLoader boundedLoader;
	boundedLoader.RequestSources(boundedSources, 83);
	std::vector<jpegview_linux::FileDialogFileSizeResult> boundedResults;
	bool sizeDrainWasBounded = true;
	const auto boundedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (boundedResults.size() < boundedPaths.size() &&
		std::chrono::steady_clock::now() < boundedDeadline) {
		std::vector<jpegview_linux::FileDialogFileSizeResult> batch =
			boundedLoader.TakeReady(2);
		if (batch.size() > 2) sizeDrainWasBounded = false;
		boundedResults.insert(boundedResults.end(), std::make_move_iterator(batch.begin()),
			std::make_move_iterator(batch.end()));
		if (boundedResults.size() < boundedPaths.size()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	Expect(sizeDrainWasBounded && boundedResults.size() == boundedPaths.size() &&
		std::all_of(boundedResults.begin(), boundedResults.end(), [](const auto& result) {
			return result.generation == 83 && result.observedSource.Valid();
		}), "file-size result draining exceeded its per-update bound or lost a row");
	boundedLoader.Shutdown();
	boundedLoader.Request({boundedPaths.front()}, 84);
	Expect(boundedLoader.TakeReady().empty(),
		"shut down file-size loader accepted a new request");
}

void TestFileDialogPreviewSelectionAndBackgroundLoading() {
	TemporaryDirectory temporary;
	const fs::path album = temporary.path() / "album";
	fs::create_directories(album);
	const fs::path alpha = album / "a-photo.png";
	const fs::path recent = album / "z-photo.jpg";
	WriteText(alpha, "supported extension used for order resolution");
	WriteText(recent, "supported extension used for order resolution");
	SetModificationTime(alpha, 10);
	SetModificationTime(recent, 20);
	Expect(jpegview_linux::FirstImageInDirectory(album,
		jpegview_linux::FileDialogSortMode::Name) == alpha,
		"directory preview did not resolve the first image in name order");
	Expect(jpegview_linux::FirstImageInDirectory(album,
		jpegview_linux::FileDialogSortMode::ModificationDate) == recent,
		"directory preview did not resolve the first image in modification-date order");
	const jpegview_linux::FileDialogPreviewSize defaultPreviewSize =
		jpegview_linux::FileDialogPreviewImageSize(260, 468);
	const jpegview_linux::FileDialogPreviewSize widerPreviewSize =
		jpegview_linux::FileDialogPreviewImageSize(320, 468);
	const jpegview_linux::FileDialogPreviewSize tinyPreviewSize =
		jpegview_linux::FileDialogPreviewImageSize(8, 10);
	Expect(defaultPreviewSize.width == 244 && defaultPreviewSize.height == 404 &&
		widerPreviewSize.width == 304 && widerPreviewSize.height == 404 &&
		tinyPreviewSize.width == 1 && tinyPreviewSize.height == 1,
		"preview image target did not follow the pane size and its content insets");
	const jpegview_linux::FileDialogPreviewFooterLayout normalFooter =
		jpegview_linux::CalculateFileDialogPreviewFooterLayout(304, 80, 120);
	const jpegview_linux::FileDialogPreviewFooterLayout narrowFooter =
		jpegview_linux::CalculateFileDialogPreviewFooterLayout(184, 120, 140);
	const jpegview_linux::FileDialogPreviewFooterLayout filenameOnlyFooter =
		jpegview_linux::CalculateFileDialogPreviewFooterLayout(184, 80, 0);
	Expect(normalFooter.filenameWidth == 176 && normalFooter.detailsWidth == 120 &&
		normalFooter.detailsOffsetX == 184 &&
		normalFooter.detailsOffsetX + normalFooter.detailsWidth == 304,
		"preview footer did not left-align the name and right-align image details");
	Expect(narrowFooter.filenameWidth == 88 && narrowFooter.detailsWidth == 88 &&
		narrowFooter.detailsOffsetX == 96 &&
		narrowFooter.filenameWidth + 8 + narrowFooter.detailsWidth == 184,
		"narrow preview footer did not split and clip its two labels safely");
	Expect(filenameOnlyFooter.filenameWidth == 184 && filenameOnlyFooter.detailsWidth == 0,
		"preview footer did not give the full row to a filename without image details");

	const fs::path imageDirectory = temporary.path() / "images";
	fs::create_directories(imageDirectory);
	const fs::path ppm = imageDirectory / "wide.ppm";
	std::vector<std::uint8_t> ppmBytes = {
		'P', '6', '\n', '8', '0', '0', ' ', '6', '0', '0', '\n', '2', '5', '5', '\n',
	};
	for (int y = 0; y < 600; ++y) {
		for (int x = 0; x < 800; ++x) {
			const std::uint8_t value = (x + y) % 2 == 0 ? 0 : 255;
			ppmBytes.insert(ppmBytes.end(), {value, value, value});
		}
	}
	WriteBytes(ppm, ppmBytes);

	std::mutex controlledMutex;
	std::condition_variable controlledChanged;
	bool activePreviewStarted = false;
	bool releaseActivePreview = false;
	std::vector<fs::path> controlledPaths;
	std::vector<jpegview_linux::PerfContext> controlledContexts;
	jpegview_linux::FileDialogPreviewLoader controlledLoader(
		[&](const fs::path& path, bool, jpegview_linux::FileDialogSortMode, int, int,
			const std::function<bool()>&) {
			const jpegview_linux::PerfContext context = jpegview_linux::CurrentPerfContext();
			std::unique_lock<std::mutex> lock(controlledMutex);
			controlledPaths.push_back(path);
			controlledContexts.push_back(context);
			if (path.filename() == "active.png") {
				activePreviewStarted = true;
				controlledChanged.notify_all();
				controlledChanged.wait(lock, [&] { return releaseActivePreview; });
			}
			jpegview_linux::FileDialogPreviewResult result;
			result.source = path;
			result.width = result.height = 1;
			result.bgra.assign(4, 255);
			return result;
		});
	for (int index = 0; index < 512; ++index) {
		(void)controlledLoader.Request({}, false,
			jpegview_linux::FileDialogSortMode::Name, 0, 0);
	}
	const std::uint64_t activeGeneration = controlledLoader.Request(
		"active.png", false, jpegview_linux::FileDialogSortMode::Name, 2, 2);
	bool activePreviewReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(controlledMutex);
		activePreviewReachedBarrier = controlledChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return activePreviewStarted; });
	}
	const std::uint64_t pendingGeneration = controlledLoader.Request(
		"pending.png", false, jpegview_linux::FileDialogSortMode::Name, 2, 2);
	const std::uint64_t barrierCurrentGeneration = controlledLoader.Request(
		"current.png", false, jpegview_linux::FileDialogSortMode::Name, 2, 2);
	{
		std::lock_guard<std::mutex> lock(controlledMutex);
		releaseActivePreview = true;
	}
	controlledChanged.notify_all();
	std::vector<jpegview_linux::FileDialogPreviewResult> controlledResults;
	const auto controlledDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (controlledResults.empty() && std::chrono::steady_clock::now() < controlledDeadline) {
		controlledResults = controlledLoader.TakeReady();
		if (controlledResults.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Expect(activePreviewReachedBarrier && activeGeneration < pendingGeneration &&
		pendingGeneration < barrierCurrentGeneration && controlledResults.size() == 1 &&
		controlledResults.front().generation == barrierCurrentGeneration &&
		controlledResults.front().source == "current.png",
		"preview replacement published a pending or active stale result instead of the latest request");
	{
		std::lock_guard<std::mutex> lock(controlledMutex);
		Expect(controlledPaths == std::vector<fs::path>({"active.png", "current.png"}),
			"preview replacement ran a displaced pending task or omitted the latest task");
		Expect(std::all_of(controlledContexts.begin(), controlledContexts.end(), [](const auto& context) {
			return context.workClass == jpegview_linux::PerfWorkClass::FocusedPreview &&
				context.execution == jpegview_linux::PerfExecution::WorkerThread;
		}), "preview processor did not keep focused-preview worker attribution");
	}
	const char* tracePath = std::getenv("JPEGVIEW_PERF_TRACE");
	if (tracePath != nullptr && *tracePath != '\0') {
		const auto countCancellationRows = [&](std::uint64_t generation,
			std::uint64_t reason, const char* execution) {
			std::ifstream trace(tracePath);
			std::string line;
			std::size_t count = 0;
			while (std::getline(trace, line)) {
				std::array<std::string, 13> fields{};
				std::size_t start = 0;
				for (std::size_t index = 0; index < fields.size(); ++index) {
					const std::size_t comma = line.find(',', start);
					if (index + 1 == fields.size()) {
						fields[index] = line.substr(start);
					} else if (comma != std::string::npos) {
						fields[index] = line.substr(start, comma - start);
						start = comma + 1;
					} else {
						break;
					}
				}
				if (fields[1] == "cancellation" && fields[3] == execution &&
					fields[5] == "focused_preview" &&
					fields[6] == std::to_string(generation) &&
					fields[7] == std::to_string(reason) &&
					fields[12] == "\"file_dialog_preview\"") ++count;
			}
			return count;
		};
		std::size_t pendingCancellationRows = 0;
		std::size_t staleCancellationRows = 0;
		const auto traceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		do {
			pendingCancellationRows = countCancellationRows(pendingGeneration, 1, "event_thread");
			staleCancellationRows = countCancellationRows(activeGeneration, 3, "worker_thread");
			if (pendingCancellationRows == 1 && staleCancellationRows == 1) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		} while (std::chrono::steady_clock::now() < traceDeadline);
		Expect(pendingCancellationRows == 1 && staleCancellationRows == 1,
			"preview pending replacement or stale active result was not traced exactly once in its owning thread");
	}

	jpegview_linux::FileDialogPreviewLoader loader;
	const std::uint64_t staleGeneration = loader.Request(ppm, false,
		jpegview_linux::FileDialogSortMode::Name, 2, 2);
	const std::uint64_t currentGeneration = loader.Request(imageDirectory, true,
		jpegview_linux::FileDialogSortMode::Name, 2, 2);
	Expect(currentGeneration > staleGeneration,
		"file-dialog preview requests did not advance their generation");
	std::vector<jpegview_linux::FileDialogPreviewResult> results;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		results = loader.TakeReady();
	}
	Expect(results.size() == 1 && results[0].generation == currentGeneration &&
		results[0].source == ppm && results[0].error.empty(),
		"background preview loading published stale or failed directory work");
	Expect(results[0].width == 2 && results[0].height == 2 &&
		results[0].sourceWidth == 800 && results[0].sourceHeight == 600 &&
		results[0].fileSizeKnown && results[0].fileSize == fs::file_size(ppm) &&
		results[0].bgra == std::vector<std::uint8_t>({
			128, 128, 128, 255, 128, 128, 128, 255,
			128, 128, 128, 255, 128, 128, 128, 255}),
		"background image preview did not area-filter high-frequency detail");
	const fs::path sizeOld = imageDirectory / "old-size.bin";
	const fs::path sizeCurrent = imageDirectory / "current-size.bin";
	WriteText(sizeOld, "stale size request");
	WriteText(sizeCurrent, "current file size");
	jpegview_linux::FileDialogFileSizeLoader sizeLoader;
	sizeLoader.Request({sizeOld}, 50);
	sizeLoader.Request({sizeCurrent}, 51);
	std::vector<jpegview_linux::FileDialogFileSizeResult> sizeResults;
	const auto sizeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (sizeResults.empty() && std::chrono::steady_clock::now() < sizeDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		sizeResults = sizeLoader.TakeReady();
	}
	Expect(sizeResults.size() == 1 && sizeResults.front().generation == 51 &&
		sizeResults.front().path == sizeCurrent &&
		sizeResults.front().size == fs::file_size(sizeCurrent),
		"file-size worker published stale results after its request was replaced");
	const fs::path transparentPng = imageDirectory / "transparent.png";
	const std::vector<std::uint8_t> transparentPixels = TestPixels();
	ImageWriteOptions pngOptions;
	std::string pngError;
	Expect(jpegview_linux::WriteImage(transparentPng, transparentPixels.data(), 2, 2,
		pngOptions, pngError), "could not create transparent preview PNG: " + pngError);
	const std::uint64_t transparentGeneration = loader.Request(transparentPng, false,
		jpegview_linux::FileDialogSortMode::Name, 2, 2);
	results.clear();
	const auto transparentDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < transparentDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		results = loader.TakeReady();
	}
	Expect(results.size() == 1 && results[0].generation == transparentGeneration &&
		results[0].hasTransparency && results[0].error.empty(),
		"file-dialog preview lost transparent PNG metadata");

	const jpegview_linux::SourceDescriptor previewPlaceholder(ppm, {}, {});
	const std::uint64_t resizedGeneration = loader.Request(ppm, false,
		jpegview_linux::FileDialogSortMode::Name, widerPreviewSize.width,
		widerPreviewSize.height, previewPlaceholder);
	Expect(resizedGeneration > currentGeneration,
		"changing the preview target size did not replace its request");
	results.clear();
	const auto resizedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < resizedDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		results = loader.TakeReady();
	}
	Expect(results.size() == 1 && results[0].generation == resizedGeneration &&
		results[0].width == 304 && results[0].height == 228 && results[0].error.empty() &&
		results[0].requestedSourceDescriptor.Key() == previewPlaceholder.Key() &&
		results[0].sourceDescriptor.Valid(),
		"resized preview request did not return the source image at its new target size");

	loader.Clear();
	Expect(loader.TakeReady().empty(), "clearing the preview loader retained a completed image");
	loader.Shutdown();
	loader.Shutdown();
	(void)loader.Request(ppm, false, jpegview_linux::FileDialogSortMode::Name, 2, 2);
	Expect(loader.TakeReady().empty(), "shut down preview loader accepted a new request");
}

void TestFileDialogPreviewRecapturesRecreatedSource() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "recreated-preview.png";
	const jpegview_linux::SourceDescriptor missing(source, {}, {});
	Expect(!missing.Valid(), "preview recreation fixture unexpectedly exists before its worker request");
	std::mutex mutex;
	std::condition_variable changed;
	bool captureStarted = false;
	bool releaseCapture = false;
	std::thread::id captureThread;
	const std::thread::id requestThread = std::this_thread::get_id();
	const auto capture = [&](const jpegview_linux::SourceDescriptor& requested) {
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (!captureStarted) {
				captureThread = std::this_thread::get_id();
				captureStarted = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseCapture; });
			}
		}
		return jpegview_linux::DescribeImageSource(requested.LogicalPath());
	};
	jpegview_linux::FileDialogPreviewLoader loader({}, capture);
	const std::uint64_t generation = loader.Request(source, false,
		jpegview_linux::FileDialogSortMode::Name, 2, 2, missing);
	ScopedConditionRelease release(mutex, changed, releaseCapture);
	bool recaptureReached = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		recaptureReached = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return captureStarted;
		});
	}
	if (recaptureReached) WriteTinyImage(source);
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseCapture = true;
	}
	changed.notify_all();
	std::vector<jpegview_linux::FileDialogPreviewResult> results;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < deadline) {
		results = loader.TakeReady();
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	Expect(recaptureReached && captureThread != requestThread && results.size() == 1 &&
		results.front().generation == generation &&
		results.front().requestedSourceDescriptor.Key() == missing.Key() &&
		results.front().sourceDescriptor.Valid() &&
		results.front().sourceDescriptor.Key() != missing.Key() &&
		results.front().observedSource.Key() == results.front().sourceDescriptor.Key() &&
		results.front().error.empty() && results.front().width == 1 &&
		results.front().height == 1 && !results.front().bgra.empty(),
		"preview did not recapture a missing source after recreation before decoding its pixels");

	const fs::path stillMissingPath = temporary.path() / "still-missing.png";
	const jpegview_linux::SourceDescriptor stillMissing(stillMissingPath, {}, {});
	std::atomic<int> customProcessorCalls{0};
	jpegview_linux::FileDialogPreviewLoader customLoader(
		[&](const fs::path& path, bool, jpegview_linux::FileDialogSortMode,
			int, int, const std::function<bool()>&) {
			++customProcessorCalls;
			jpegview_linux::FileDialogPreviewResult result;
			result.source = path;
			result.width = result.height = 1;
			result.bgra.assign(4, 255);
			return result;
		});
	const std::uint64_t missingGeneration = customLoader.Request(stillMissingPath,
		false, jpegview_linux::FileDialogSortMode::Name, 1, 1, stillMissing);
	results.clear();
	const auto missingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < missingDeadline) {
		results = customLoader.TakeReady();
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	Expect(results.size() == 1 && results.front().generation == missingGeneration &&
		customProcessorCalls == 0 && results.front().sourceDescriptor.Key() == stillMissing.Key() &&
		!results.front().sourceDescriptor.Valid() && results.front().bgra.empty() &&
		results.front().error == "Image changed while preparing preview",
		"custom preview processor decoded a nonempty path without a valid recaptured source identity");
}

void TestEmbeddedApplicationIcon() {
	jpegview_linux::ApplicationIcon icon;
	std::string error;
	Expect(jpegview_linux::DecodeApplicationIcon(icon, error),
		"cannot decode embedded JPEGView.ico: " + error);
	Expect(icon.width == 64 && icon.height == 64,
		"embedded application icon did not select the largest ICO frame");
	Expect(icon.bgra.size() == 64u * 64u * 4u,
		"embedded application icon has an invalid pixel buffer");
	bool hasDifferentPixels = false;
	for (std::size_t offset = 4; offset < icon.bgra.size(); offset += 4) {
		if (!std::equal(icon.bgra.begin(), icon.bgra.begin() + 4, icon.bgra.begin() + offset)) {
			hasDifferentPixels = true;
			break;
		}
	}
	Expect(hasDifferentPixels, "embedded application icon decoded as a single color");
	for (std::size_t offset = 3; offset < icon.bgra.size(); offset += 4) {
		Expect(icon.bgra[offset] == 255, "opaque JPEGView.ico frame acquired unexpected transparency");
	}
}

void TestRendererTextureOwnerLifecycle() {
	Expect(SDL_Init(0) == 0, "SDL initialization failed: " + std::string(SDL_GetError()));
	struct SdlTestResources {
		SDL_Surface* surface = nullptr;
		SDL_Renderer* renderer = nullptr;
		~SdlTestResources() {
			if (renderer != nullptr) SDL_DestroyRenderer(renderer);
			if (surface != nullptr) SDL_FreeSurface(surface);
			SDL_Quit();
		}
	} resources;
	resources.surface = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32,
		SDL_PIXELFORMAT_ARGB8888);
	Expect(resources.surface != nullptr,
		"could not create software-renderer test surface: " + std::string(SDL_GetError()));
	resources.renderer = SDL_CreateSoftwareRenderer(resources.surface);
	Expect(resources.renderer != nullptr,
		"could not create software test renderer: " + std::string(SDL_GetError()));

	jpegview_linux::RendererTextureOwner owner;
	owner.SetRenderer(resources.renderer);
	Expect(owner.CreateEmpty(0, 2) == nullptr && owner.LiveTextureCount() == 0,
		"empty image texture accepted invalid dimensions");
	Expect(owner.CreateAndUpload({0, 1, 2}, 1, 1, false) == nullptr &&
		owner.LiveTextureCount() == 0,
		"image texture upload accepted a buffer with the wrong size");

	const std::vector<std::uint8_t> pixels{
		0, 0, 255, 255, 0, 255, 0, 255,
		255, 0, 0, 255, 255, 255, 255, 128};
	SDL_Texture* uploaded = owner.CreateAndUpload(pixels, 2, 2, true);
	Expect(uploaded != nullptr && owner.LiveTextureCount() == 1,
		"uploaded texture was not adopted by the image owner");
	int blendMode = SDL_BLENDMODE_NONE;
	Expect(SDL_GetTextureBlendMode(uploaded, &blendMode) == 0 &&
		blendMode == SDL_BLENDMODE_BLEND,
		"uploaded texture did not preserve the transparency blend mode");
	Expect(owner.Destroy(uploaded, pixels.size()) && owner.LiveTextureCount() == 0,
		"image owner did not release an explicitly destroyed texture");
	Expect(!owner.Destroy(uploaded) && owner.LiveTextureCount() == 0,
		"image owner accepted a duplicate texture destruction");

	Expect(owner.CreateEmpty(3, 2) != nullptr && owner.LiveTextureCount() == 1,
		"incomplete display texture was not adopted by the image owner");
	Expect(owner.DestroyAll() == 1 && owner.LiveTextureCount() == 0,
		"shutdown did not release all remaining image textures");
	Expect(owner.DestroyAll() == 0,
		"repeated image texture shutdown released unexpected resources");
	Expect(owner.CreateEmpty(2, 2) != nullptr && owner.LiveTextureCount() == 1,
		"could not retain a texture before testing metadata allocation failure");
	for (int attempt = 0; attempt < 2; ++attempt) {
		owner.FailNextAdoptionForTesting();
		Expect(owner.CreateEmpty(2, 2) == nullptr && owner.LiveTextureCount() == 1,
			"texture metadata allocation failure did not return null and preserve existing textures");
	}
	owner.FailNextAdoptionForTesting();
	Expect(owner.CreateAndUpload(pixels, 2, 2, true) == nullptr &&
		owner.LiveTextureCount() == 1,
		"uploaded texture metadata allocation failure did not preserve the retry contract");
	Expect(owner.CreateAndUpload(pixels, 2, 2, true) != nullptr &&
		owner.LiveTextureCount() == 2,
		"texture creation did not recover after metadata allocation failures");
	Expect(owner.DestroyAll() == 2 && owner.LiveTextureCount() == 0,
		"texture shutdown did not release the existing and retried textures");
	owner.SetRenderer(nullptr);
}
const TestCase kTests[] = {
	{"recent-files-mru-uniqueness-persistence-and-viewports", &TestRecentFilesMruUniquenessPersistenceAndViewportSnapshots},
	{"image-session-controller-navigation-and-clipboard-state", &TestImageSessionControllerSnapshotsNavigationAndClipboardState},
	{"image-session-selected-load-commits-only-ready-current-owner", &TestImageSessionSelectedLoadCommitsOnlyReadyCurrentOwner},
	{"selected-source-decode-channel-rejects-stale-owner-completions", &TestSelectedSourceDecodeChannelRejectsStaleOwnerCompletions},
	{"image-session-controller-selected-source-preparation", &TestImageSessionControllerPlansSelectedSourcePreparation},
	{"recent-image-load-history-commits-only-after-success", &TestRecentImageLoadHistoryCommitsOnlyAfterSuccess},
	{"viewport-snapshot-follows-selected-identity-during-cancellation", &TestViewportSnapshotFollowsSelectedIdentityDuringCancellation},
	{"pending-recent-viewport-tracks-user-mode-changes", &TestPendingRecentViewportTracksUserModeChanges},
	{"pending-image-intents-respect-source-generation", &TestPendingImageIntentsRespectSourceGeneration},
	{"pending-image-operation-admission-preserves-intent-order", &TestPendingImageOperationAdmissionPreservesIntentOrder},
	{"file-dialog-filtering", &TestFileDialogFiltering},
	{"file-dialog-sorting", &TestFileDialogSorting},
	{"file-dialog-model-state-and-navigation", &TestFileDialogModelStateAndNavigation},
	{"file-dialog-model-indexed-large-catalog-updates", &TestFileDialogModelIndexedLargeCatalogUpdates},
	{"file-dialog-directory-summaries", &TestFileDialogDirectorySummaries},
	{"file-dialog-directory-loader", &TestFileDialogDirectoryLoader},
	{"file-dialog-listing-completion-preserves-user-state", &TestFileDialogListingCompletionPreservesUserState},
	{"cold-archive-row-descriptors-are-captured-off-thread", &TestColdArchiveRowDescriptorsAreCapturedOffThread},
	{"file-dialog-preview-selection-and-background-loading", &TestFileDialogPreviewSelectionAndBackgroundLoading},
	{"file-dialog-preview-recaptures-recreated-source", &TestFileDialogPreviewRecapturesRecreatedSource},
	{"embedded-application-icon", &TestEmbeddedApplicationIcon},
	{"renderer-texture-owner-lifecycle", &TestRendererTextureOwnerLifecycle},
};

} // namespace

const TestSuite& GetDialogsSessionsSuite() {
	static const TestSuite suite{"dialogs_sessions", kTests, sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
