#include "test_harness.h"
#include "test_support.h"

namespace {

void TestOrdinarySourceMetadataUsesOneStatxRequest() {
	TemporaryDirectory temporary;
	const fs::path sourcePath = temporary.path() / "metadata.jpg";
	WriteTinyImage(sourcePath);
	SetModificationTimeNanoseconds(sourcePath, 1700000000, 123456000);
	SourceIdentityStatObservation observation;
	ArchiveSourceProbeHookReset resetHook;
	jpegview_linux::SetArchiveSourceProbeHookForTesting(
		ObserveSourceIdentityStat, &observation);
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(sourcePath);
	jpegview_linux::SetArchiveSourceProbeHookForTesting(nullptr, nullptr);
	const bool completedWithStatx = observation.statxAttempts.load() == 1 &&
		observation.completedStatx.load() == 1 && observation.fallbackStats.load() == 0;
	const bool usedFallback = observation.statxAttempts.load() <= 1 &&
		observation.completedStatx.load() == 0 && observation.fallbackStats.load() == 1;
	Expect(source.Valid() && source.Metadata().hasFileSize &&
		source.Metadata().hasModificationTime && source.Metadata().hasCreationTime &&
		(completedWithStatx || usedFallback),
		"ordinary source metadata required redundant identity and timestamp probes");
	if (usedFallback) {
		Expect(source.Metadata().creationTimeNanoseconds ==
			source.Metadata().modificationTimeNanoseconds,
			"stat fallback did not preserve modification-time creation fallback");
	}

	SourceIdentityStatObservation fallbackObservation;
	jpegview_linux::SetArchiveSourceProbeHookForTesting(
		ObserveSourceIdentityStat, &fallbackObservation);
	jpegview_linux::SetStatxUnavailableForTesting(true);
	const jpegview_linux::SourceDescriptor fallbackSource =
		jpegview_linux::DescribeImageSource(sourcePath);
	jpegview_linux::SetStatxUnavailableForTesting(false);
	jpegview_linux::SetArchiveSourceProbeHookForTesting(nullptr, nullptr);
	Expect(fallbackSource.Valid() && fallbackSource.BackingIdentity() == source.BackingIdentity() &&
		fallbackSource.Metadata().hasFileSize &&
		fallbackSource.Metadata().hasModificationTime &&
		fallbackSource.Metadata().creationTimeNanoseconds ==
			fallbackSource.Metadata().modificationTimeNanoseconds &&
		fallbackObservation.statxAttempts.load() == 0 &&
		fallbackObservation.completedStatx.load() == 0 &&
		fallbackObservation.fallbackStats.load() == 1,
		"forced stat fallback did not preserve source identity and creation-time semantics");
}

void TestSourceDescriptorIdentityAndUnusualPaths() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "identities";
	fs::create_directories(directory);
	const fs::path source = directory / "same-size.png";
	const fs::path replacement = directory / "replacement.tmp";
	WriteBytes(source, {1, 2, 3, 4});
	constexpr std::int64_t timestampSeconds = 1700000000;
	SetModificationTimeNanoseconds(source, timestampSeconds, 123456000);
	const jpegview_linux::SourceDescriptor original =
		jpegview_linux::DescribeImageSource(source);
	Expect(original.Valid() && original.Metadata().hasFileSize &&
		original.Metadata().fileSize == 4 && original.Metadata().hasModificationTime,
		"regular-file descriptor omitted its backing identity or captured metadata");
	const jpegview_linux::SourceKey originalKey = original.Key();
	const jpegview_linux::SourceKey missingKeyA{source.string(),
		jpegview_linux::SourceIdentity{}};
	const jpegview_linux::SourceKey missingKeyB{source.string(),
		jpegview_linux::SourceIdentity{}};
	jpegview_linux::SourceIdentity otherInvalidIdentity;
	otherInvalidIdentity.device = 1;
	const jpegview_linux::SourceKey missingKeyWithFields{source.string(),
		otherInvalidIdentity};
	const jpegview_linux::SourceKey duplicateOriginalKey{originalKey.logicalPath,
		originalKey.backingIdentity};
	Expect(originalKey == duplicateOriginalKey &&
		jpegview_linux::SourceKeyHash{}(originalKey) ==
			jpegview_linux::SourceKeyHash{}(duplicateOriginalKey),
		"equal valid source keys did not produce matching hashes");
	Expect(!missingKeyA.Valid() && missingKeyA == missingKeyA &&
		missingKeyA == missingKeyB && missingKeyA != missingKeyWithFields &&
		jpegview_linux::SourceKeyHash{}(missingKeyA) ==
			jpegview_linux::SourceKeyHash{}(missingKeyB),
		"invalid source keys did not compare all identity fields or retain matching hashes");
	FileList files({directory.string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 1 && files.DescriptorAt(0) != nullptr &&
		files.DescriptorAt(0)->Key() == originalKey,
		"file-list scan did not retain the source descriptor captured for its entry");

	WriteBytes(source, {4, 3, 2, 1});
	SetModificationTimeNanoseconds(source, timestampSeconds, 123457000);
	const jpegview_linux::SourceDescriptor edited =
		jpegview_linux::DescribeImageSource(source);
	Expect(edited.Valid() && edited.BackingIdentity().device == original.BackingIdentity().device &&
		edited.BackingIdentity().inode == original.BackingIdentity().inode &&
		edited.BackingIdentity().size == original.BackingIdentity().size &&
		edited.BackingIdentity().modifiedSeconds == original.BackingIdentity().modifiedSeconds &&
		edited.BackingIdentity().modifiedNanoseconds !=
			original.BackingIdentity().modifiedNanoseconds && edited.Key() != originalKey,
		"same-inode, same-size edit with a nanosecond-only timestamp change reused its source key");
	Expect(files.RefreshSourceDescriptor(originalKey, edited) &&
		files.DescriptorAt(0)->Key() == edited.Key() &&
		!files.RefreshSourceDescriptor(originalKey, original),
		"file list did not accept a matching source refresh or reject a delayed stale descriptor");

	WriteBytes(replacement, {9, 8, 7, 6});
	SetModificationTimeNanoseconds(replacement, timestampSeconds, 123457000);
	std::error_code renameError;
	fs::rename(replacement, source, renameError);
	Expect(!renameError && fs::file_size(source) == 4,
		"same-size inode replacement fixture failed: " + renameError.message());
	const jpegview_linux::SourceDescriptor replaced =
		jpegview_linux::DescribeImageSource(source);
	Expect(replaced.Valid() && replaced.BackingIdentity().size == edited.BackingIdentity().size &&
		replaced.BackingIdentity().modifiedSeconds == edited.BackingIdentity().modifiedSeconds &&
		replaced.BackingIdentity().modifiedNanoseconds == edited.BackingIdentity().modifiedNanoseconds &&
		replaced.BackingIdentity().inode != edited.BackingIdentity().inode &&
		replaced.Key() != edited.Key(),
		"same-size rename-over replacement was not distinguished by inode identity");
	Expect(files.RefreshSourceDescriptor(edited.Key(), replaced) &&
		files.DescriptorAt(0)->Key() == replaced.Key(),
		"file list did not refresh after same-path inode replacement");

	std::error_code removeError;
	Expect(fs::remove(source, removeError) && !removeError,
		"could not remove source for missing-descriptor coverage");
	const jpegview_linux::SourceDescriptor missing =
		jpegview_linux::DescribeImageSource(source);
	Expect(!missing.Valid() && !missing.BackingIdentity().valid &&
		!jpegview_linux::IsImageSourceCurrent(replaced),
		"missing source did not produce an invalid descriptor that fails freshness validation");
	Expect(files.RefreshSourceDescriptor(replaced.Key(), missing) &&
		files.DescriptorAt(0)->Key() == missing.Key() &&
		!files.DescriptorAt(0)->Key().Valid(),
		"missing-source refresh did not replace the prior cache identity with an invalid key");

	const std::string newlineName = "legal-line\nbreak.png";
	const std::string nonUtf8Name = std::string("legal-") + static_cast<char>(0xff) + "-name.png";
	const fs::path newlinePath = directory / newlineName;
	const fs::path nonUtf8Path = directory / nonUtf8Name;
	WriteBytes(newlinePath, {1});
	WriteBytes(nonUtf8Path, {2});
	const jpegview_linux::SourceDescriptor newlineDescriptor =
		jpegview_linux::DescribeImageSource(newlinePath);
	const jpegview_linux::SourceDescriptor nonUtf8Descriptor =
		jpegview_linux::DescribeImageSource(nonUtf8Path);
	Expect(newlineDescriptor.Valid() && nonUtf8Descriptor.Valid() &&
		newlineDescriptor.Key().logicalPath.find('\n') != std::string::npos &&
		nonUtf8Descriptor.Key().logicalPath.find(static_cast<char>(0xff)) != std::string::npos &&
		newlineDescriptor.Key() != nonUtf8Descriptor.Key(),
		"legal newline or non-UTF-8 filename bytes were lost or conflated in source keys");
	FileList unusualFiles({directory.string()}, FileList::SortMode::FileName, true, false);
	Expect(unusualFiles.ContainsPath(newlinePath) && unusualFiles.ContainsPath(nonUtf8Path) &&
		unusualFiles.DescriptorAt(*unusualFiles.IndexOf(newlinePath))->Key() ==
			newlineDescriptor.Key() &&
		unusualFiles.DescriptorAt(*unusualFiles.IndexOf(nonUtf8Path))->Key() ==
			nonUtf8Descriptor.Key(),
		"background enumeration did not preserve legal unusual filename bytes in descriptors");
}

void TestProvisionalSourceDescriptorSurvivesStartupReplacement() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "startup.jpg";
	const fs::path replacement = temporary.path() / "startup-next.tmp";
	WriteBytes(source, {0xff, 0xd8, 0xff, 0xd9});
	FileList files;
	files.SetProvisionalInputs({source.string()});
	Expect(files.Size() == 1 && files.DescriptorAt(0) != nullptr &&
		files.DescriptorAt(0)->Valid(),
		"provisional direct-image startup did not capture a valid backing identity");
	const jpegview_linux::SourceDescriptor original = *files.DescriptorAt(0);
	const auto directRequest = jpegview_linux::MakeJpegDisplayImageRequest(
		original, 100, 50, 50, 25, false);
	Expect(directRequest.Valid(),
		"provisional JPEG descriptor did not admit the startup reduced-display request");

	WriteBytes(replacement, {0xff, 0xd8, 0, 0, 0xff, 0xd9});
	SetModificationTimeNanoseconds(replacement,
		original.BackingIdentity().modifiedSeconds,
		static_cast<long>(original.BackingIdentity().modifiedNanoseconds));
	std::ifstream keepOriginalInode(source, std::ios::binary);
	Expect(static_cast<bool>(keepOriginalInode),
		"could not keep the original startup source inode alive during replacement");
	std::error_code renameError;
	fs::rename(replacement, source, renameError);
	Expect(!renameError, "could not stage same-path startup replacement: " +
		renameError.message());
	FileListPreparedScan scan = FileList::PrepareScan(
		files.MakeScanRequest(FileList::ScanOperation::Initialize), [] { return true; });
	Expect(scan.completed && scan.replacement.Size() == 1 &&
		scan.replacement.DescriptorAt(0)->Valid() &&
		scan.replacement.DescriptorAt(0)->Key() != original.Key(),
		"startup scan did not retain the replacement source identity for the same logical path");
	const jpegview_linux::SourceKey replacementKey =
		scan.replacement.DescriptorAt(0)->Key();
	Expect(files.ApplyPreparedScan(std::move(scan), source) &&
		files.Current() == source && files.DescriptorAt(0)->Key() == replacementKey &&
		directRequest.source.Key() != files.DescriptorAt(0)->Key(),
		"same-path startup scan relabeled the old direct-decode key with the replacement descriptor");
}

void TestCurrentJpegDimensionsAcceptStableSourceAcrossListRevisions() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "header.jpg";
	WriteBytes(source, {0xff, 0xd8, 0xff, 0xd9});
	FileList files;
	files.SetProvisionalInputs({source.string()});
	const jpegview_linux::SourceKey requestSource = files.DescriptorAt(0)->Key();
	const std::uint64_t requestGeneration = 12;
	const std::uint64_t initialCatalogRevision = files.MutationRevision();
	const std::uint64_t initialDescriptorRevision = files.DescriptorRevision();

	FileListPreparedScan scan = FileList::PrepareScan(
		files.MakeScanRequest(FileList::ScanOperation::Initialize), [] { return true; });
	Expect(scan.completed && files.ApplyPreparedScan(std::move(scan), source) &&
		files.MutationRevision() != initialCatalogRevision &&
		files.DescriptorAt(0)->Key() == requestSource,
		"startup catalog publication did not preserve the pending JPEG source identity");
	Expect(jpegview_linux::IsCurrentJpegDimensionsResult(requestGeneration,
		requestSource, requestGeneration, files.DescriptorAt(0)->Key()),
		"catalog revision alone rejected the current JPEG header result");

	const jpegview_linux::SourceDescriptor withDimensions =
		files.DescriptorAt(0)->WithImageProperties(64, 48, false);
	Expect(files.RefreshSourceDescriptor(withDimensions) &&
		files.DescriptorRevision() != initialDescriptorRevision &&
		files.DescriptorAt(0)->Key() == requestSource &&
		jpegview_linux::IsCurrentJpegDimensionsResult(requestGeneration,
			requestSource, requestGeneration, files.DescriptorAt(0)->Key()),
		"descriptor revision alone rejected a JPEG header for the same backing source");

	jpegview_linux::SourceIdentity replacementIdentity = requestSource.backingIdentity;
	++replacementIdentity.modifiedNanoseconds;
	const jpegview_linux::SourceKey replacementSource(
		requestSource.logicalPath, replacementIdentity);
	Expect(!jpegview_linux::IsCurrentJpegDimensionsResult(requestGeneration,
		requestSource, requestGeneration, replacementSource) &&
		!jpegview_linux::IsCurrentJpegDimensionsResult(requestGeneration,
			requestSource, requestGeneration + 1, requestSource),
		"JPEG header gate accepted a replaced source or superseded load generation");
}

void TestNonCurrentSourceRefreshPreservesSelection() {
	TemporaryDirectory temporary;
	const fs::path anchor = temporary.path() / "a-anchor.png";
	const fs::path middle = temporary.path() / "m-middle.png";
	const fs::path target = temporary.path() / "z-target.png";
	WriteBytes(anchor, {1, 2, 3, 4});
	WriteBytes(middle, {2, 3, 4, 5});
	WriteBytes(target, {5, 6, 7, 8});
	constexpr std::int64_t timestampSeconds = 1700000100;
	SetModificationTimeNanoseconds(target, timestampSeconds, 100000000);
	SetModificationTimeNanoseconds(middle, timestampSeconds, 200000000);
	SetModificationTimeNanoseconds(anchor, timestampSeconds, 300000000);
	FileList files({temporary.path().string()},
		FileList::SortMode::LastModificationTime, true, false);
	Expect(files.Current() == target && files.Size() == 3,
		"noncurrent source refresh fixture did not select its first image");
	const std::size_t targetIndex = *files.IndexOf(target);
	const jpegview_linux::SourceKey previous = files.DescriptorAt(targetIndex)->Key();
	Expect(files.Select(*files.IndexOf(anchor)),
		"noncurrent source refresh fixture could not select its stable anchor");
	WriteBytes(target, {8, 7, 6, 5});
	SetModificationTimeNanoseconds(target, timestampSeconds, 400000000);
	const jpegview_linux::SourceDescriptor observed =
		jpegview_linux::DescribeImageSource(target);
	const jpegview_linux::SourceRefreshOutcome outcome =
		jpegview_linux::RefreshFileListSource(files, previous, observed);
	Expect(observed.Valid() && observed.Key() != previous && outcome.applied &&
		outcome.orderChanged && !outcome.selectedSourceChanged &&
		outcome.previousIndex == 0 && outcome.currentIndex == 2 &&
		files.Current() == anchor && files.DescriptorAt(*files.IndexOf(target))->Key() == observed.Key(),
		"noncurrent refresh sorting changed the selected anchor or retained stale source metadata");
}

void TestCurrentProcessedSavePreservesMaterializedPixels() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "processed-save.png";
	WriteBytes(source, {1, 2, 3, 4});
	FileList files({source.string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 1 && files.Current() == source,
		"processed-save refresh fixture did not select its source");
	const jpegview_linux::SourceKey previous = files.DescriptorAt(0)->Key();
	WriteBytes(source, {5, 6, 7, 8, 9});
	const jpegview_linux::SourceDescriptor observed =
		jpegview_linux::DescribeImageSource(source);
	const jpegview_linux::SourceRefreshOutcome refresh =
		jpegview_linux::RefreshFileListSource(files, previous, observed);
	std::size_t processingApplications = 1;
	const jpegview_linux::SourceRefreshDisplayAction action =
		jpegview_linux::ResolveSourceRefreshDisplayAction(refresh, true);
	if (action == jpegview_linux::SourceRefreshDisplayAction::ReloadCurrent) {
		++processingApplications;
	}
	Expect(refresh.applied && refresh.selectedSourceChanged && observed.Valid() &&
		observed.Key() != previous && files.DescriptorAt(0)->Key() == observed.Key() &&
		action == jpegview_linux::SourceRefreshDisplayAction::PreserveCurrentPixels &&
		processingApplications == 1,
		"processed save refreshed the source descriptor but reapplied processing instead of preserving its materialized pixels");
	Expect(jpegview_linux::ResolveSourceRefreshDisplayAction(refresh, false) ==
		jpegview_linux::SourceRefreshDisplayAction::ReloadCurrent,
		"ordinary current-source changes stopped selecting the reload path");
}

void TestFileDialogSourceRefreshRequiresExactDescriptor() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "dialog-image.png";
	WriteBytes(source, {1, 2, 3, 4});
	const jpegview_linux::SourceDescriptor placeholder(source, {}, {});
	const jpegview_linux::SourceDescriptor first =
		jpegview_linux::DescribeImageSource(source);
	jpegview_linux::FileDialogEntry entry{source};
	entry.sourceDescriptor = placeholder;
	jpegview_linux::FileDialogModel model;
	model.SetEntries({entry});
	Expect(first.Valid() && model.RefreshSourceDescriptor(placeholder.Key(), first),
		"file-dialog model did not replace its path placeholder with captured metadata");

	WriteBytes(source, {4, 3, 2, 1, 0});
	const jpegview_linux::SourceDescriptor replacement =
		jpegview_linux::DescribeImageSource(source);
	Expect(replacement.Valid() && replacement.Key() != first.Key() &&
		model.RefreshSourceDescriptor(first.Key(), replacement),
		"file-dialog model did not accept a fresh descriptor for the expected source");
	Expect(!model.RefreshSourceDescriptor(placeholder.Key(), first) &&
		model.AllEntries().front().sourceDescriptor.Key() == replacement.Key(),
		"late placeholder metadata relabeled a newer file-dialog source descriptor");
}

void TestFileDialogPreviewRefreshPolicy() {
	const fs::path source = "/missing/recent-image.png";
	const jpegview_linux::SourceDescriptor missing(source, {}, {});
	jpegview_linux::SourceIdentity firstIdentity;
	firstIdentity.device = 7;
	firstIdentity.inode = 11;
	firstIdentity.size = 128;
	firstIdentity.modifiedSeconds = 1700000000;
	firstIdentity.modifiedNanoseconds = 123000000;
	firstIdentity.valid = true;
	const jpegview_linux::SourceDescriptor present(source, firstIdentity, {});
	jpegview_linux::SourceIdentity replacementIdentity = firstIdentity;
	++replacementIdentity.inode;
	const jpegview_linux::SourceDescriptor replacement(source, replacementIdentity, {});

	Expect(!jpegview_linux::ShouldRefreshFileDialogPreviewSource(missing.Key(), missing),
		"an unchanged invalid preview source requested another load or invalidation");
	Expect(jpegview_linux::ShouldRefreshFileDialogPreviewSource(missing.Key(), present),
		"a recreated preview source did not request descriptor refresh and invalidation");
	Expect(jpegview_linux::ShouldRefreshFileDialogPreviewSource(present.Key(), missing),
		"a deleted preview source did not request descriptor refresh and invalidation");
	Expect(jpegview_linux::ShouldRefreshFileDialogPreviewSource(present.Key(), replacement),
		"a replaced preview source did not request descriptor refresh and invalidation");
}

void TestArchiveDescriptorIdentityAndReplacement() {
	TemporaryDirectory temporary;
	const fs::path image = temporary.path() / "payload.bin";
	const fs::path archive = temporary.path() / "members.zip";
	const fs::path replacementArchive = temporary.path() / "members-next.zip";
	WriteBytes(image, {1, 3, 5, 7});
	WriteZipArchive(archive, {{"root.png", image}, {"nested/child.png", image}});
	const fs::path rootMember = archive / "root.png";
	const fs::path childMember = archive / "nested" / "child.png";
	FileList files({archive.string()}, FileList::SortMode::FileName, true, false);
	const auto rootIndex = files.IndexOf(rootMember);
	Expect(rootIndex.has_value(),
		"archive descriptor fixture did not enumerate its root image member");
	const jpegview_linux::SourceDescriptor originalRoot = *files.DescriptorAt(*rootIndex);
	const jpegview_linux::SourceDescriptor originalChild =
		jpegview_linux::DescribeImageSource(childMember);
	Expect(originalRoot.Valid() && originalChild.Valid() &&
		originalRoot.Metadata().archiveMember && originalChild.Metadata().archiveMember &&
		originalRoot.BackingIdentity() == originalChild.BackingIdentity() &&
		originalRoot.Key().logicalPath != originalChild.Key().logicalPath &&
		originalRoot.Key() != originalChild.Key() &&
		originalRoot.Metadata().fileSize == originalChild.Metadata().fileSize,
		"archive member descriptors did not share container identity while retaining distinct member keys and sizes");

	const jpegview_linux::SourceIdentity originalContainer =
		jpegview_linux::DescribeImageSource(archive).BackingIdentity();
	std::vector<jpegview_linux::ArchiveEntryInfo> originalListing;
	std::string listingError;
	Expect(jpegview_linux::ListArchiveDirectoryCancellable(archive, originalListing,
		[] { return true; }, listingError),
		"could not list the original archive catalog: " + listingError);
	const auto originalListingRoot = std::find_if(originalListing.begin(),
		originalListing.end(), [&](const auto& entry) { return entry.path == rootMember; });
	Expect(originalListingRoot != originalListing.end() &&
		originalListingRoot->backingIdentity == originalContainer,
		"archive entry metadata did not retain the identity used to build its catalog");
	SetModificationTimeNanoseconds(archive, originalContainer.modifiedSeconds,
		static_cast<long>(originalContainer.modifiedNanoseconds));
	const jpegview_linux::SourceDescriptor normalizedContainer =
		jpegview_linux::DescribeImageSource(archive);
	std::error_code copyError;
	fs::copy_file(archive, replacementArchive, fs::copy_options::overwrite_existing,
		copyError);
	Expect(!copyError && fs::file_size(replacementArchive) ==
		normalizedContainer.BackingIdentity().size,
		"archive replacement fixture could not retain the original bytes and size");
	SetModificationTimeNanoseconds(replacementArchive,
		normalizedContainer.BackingIdentity().modifiedSeconds,
		static_cast<long>(normalizedContainer.BackingIdentity().modifiedNanoseconds));
	std::error_code renameError;
	fs::rename(replacementArchive, archive, renameError);
	Expect(!renameError,
		"could not atomically replace archive fixture: " + renameError.message());
	const jpegview_linux::SourceDescriptor replacementRoot =
		jpegview_linux::DescribeImageSource(rootMember);
	Expect(replacementRoot.Valid() &&
		replacementRoot.BackingIdentity().size == originalRoot.BackingIdentity().size &&
		replacementRoot.BackingIdentity().modifiedSeconds ==
			originalRoot.BackingIdentity().modifiedSeconds &&
		replacementRoot.BackingIdentity().modifiedNanoseconds ==
			originalRoot.BackingIdentity().modifiedNanoseconds &&
		replacementRoot.BackingIdentity().inode != originalRoot.BackingIdentity().inode &&
		replacementRoot.Key() != originalRoot.Key() &&
		!jpegview_linux::IsImageSourceCurrent(originalRoot),
		"same-size, same-time archive replacement did not invalidate its old member descriptor by inode");
	Expect(files.Reload(rootMember), "file list could not refresh its replaced archive catalog");
	const auto refreshedRootIndex = files.IndexOf(rootMember);
	std::vector<jpegview_linux::ArchiveEntryInfo> replacementListing;
	Expect(jpegview_linux::ListArchiveDirectoryCancellable(archive, replacementListing,
		[] { return true; }, listingError),
		"could not list the replacement archive catalog: " + listingError);
	const auto replacementListingRoot = std::find_if(replacementListing.begin(),
		replacementListing.end(), [&](const auto& entry) { return entry.path == rootMember; });
	Expect(refreshedRootIndex.has_value() &&
		files.DescriptorAt(*refreshedRootIndex)->Key() == replacementRoot.Key() &&
		files.DescriptorAt(*refreshedRootIndex)->Metadata().fileSize ==
			replacementRoot.Metadata().fileSize &&
		replacementListingRoot != replacementListing.end() &&
		replacementListingRoot->backingIdentity == replacementRoot.BackingIdentity() &&
		replacementListingRoot->backingIdentity != originalListingRoot->backingIdentity &&
		originalListingRoot->backingIdentity == originalContainer,
		"archive reload did not publish replacement identity and uncompressed member metadata");
}

void TestFileListFilteringAndLogicalSorting() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "images";
	fs::create_directories(directory);
	WriteTinyImage(directory / "photo10.png");
	WriteTinyImage(directory / "photo2.png");
	WriteTinyImage(directory / "photo1.png");
	WriteBytes(directory / "not-an-image.txt", {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'});

	FileList files({directory.string()}, FileList::SortMode::FileName, true, false);
	Expect(FileNames(files) == std::vector<std::string>({"photo1.png", "photo2.png", "photo10.png"}),
		"logical filename ordering or extension filtering is incorrect");
	Expect(files.Current().filename() == "photo1.png", "file list did not start at the first sorted image");
	Expect(files.IndexOf(directory / "photo1.png") == std::optional<std::size_t>(0) &&
		files.IndexOf(directory / "photo10.png") == std::optional<std::size_t>(2) &&
		!files.IndexOf(directory / "missing.png").has_value() &&
		!files.IndexOf({}).has_value(),
		"file-list exact path lookup did not distinguish present and missing paths");
	Expect(files.Next(), "next image should advance");
	Expect(files.Current().filename() == "photo2.png", "next image selected the wrong file");
	Expect(files.Previous(), "previous image should advance backwards");
	Expect(files.Current().filename() == "photo1.png", "previous image selected the wrong file");
	Expect(!files.Previous(), "non-wrapping file list should stop before the first image");
	Expect(files.Select(2) && files.Current().filename() == "photo10.png",
		"direct file selection for the thumbnail panel selected the wrong image");
	Expect(!files.Select(3) && files.Current().filename() == "photo10.png",
		"out-of-range direct file selection changed the current image");

	FileList descending({directory.string()}, FileList::SortMode::FileName, false, false);
	Expect(FileNames(descending) == std::vector<std::string>({"photo10.png", "photo2.png", "photo1.png"}),
		"descending logical filename ordering is incorrect");
	Expect(descending.IndexOf(directory / "photo1.png") == std::optional<std::size_t>(2),
		"file-list exact path lookup did not follow the active sort order");
}

void TestArchiveBrowsingDecodingAndRecentPreview() {
	TemporaryDirectory temporary;
	const fs::path imageDirectory = temporary.path() / "source";
	fs::create_directories(imageDirectory);
	const fs::path image = imageDirectory / "fixture.png";
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	std::string error;
	Expect(jpegview_linux::WriteImage(image, pixels.data(), 2, 2, options, error),
		"cannot create archive image fixture: " + error);
	const fs::path notes = imageDirectory / "notes.txt";
	WriteText(notes, "not an image");
	const fs::path archive = temporary.path() / "album.ZIP";
	WriteZipArchive(archive, {
		{"01-root.png", image},
		{"nested/02-child.png", image},
		{"../escape.png", image},
		{"notes.txt", notes},
	});
	const fs::path rootImage = archive / "01-root.png";
	const fs::path nestedDirectory = archive / "nested";
	const fs::path nestedImage = nestedDirectory / "02-child.png";

	Expect(jpegview_linux::IsArchiveContainerName(archive) &&
		jpegview_linux::IsArchiveContainerFile(archive) &&
		jpegview_linux::IsArchiveLocation(archive) &&
		jpegview_linux::IsArchiveLocation(nestedDirectory) &&
		jpegview_linux::IsArchiveMemberLocation(rootImage) &&
		!jpegview_linux::IsArchiveMemberLocation(archive),
		"archive path recognition did not distinguish a container, virtual directory, and member");
	Expect(jpegview_linux::ArchiveFormatName(nestedImage) == "ZIP" &&
		jpegview_linux::ArchiveBackingFile(nestedImage) == archive &&
		jpegview_linux::ArchiveLocationDisplayName(nestedDirectory) == archive.string() + "!/nested",
		"archive display or physical-container mapping is incorrect");
	ArchiveSourceProbeObservation presentationProbe;
	jpegview_linux::SetArchiveSourceProbeHookForTesting(
		ObserveArchiveSourceProbe, &presentationProbe);
	const jpegview_linux::ArchiveLocationPresentation presentation =
		jpegview_linux::PrepareArchiveLocationPresentation(nestedDirectory);
	const int classificationStatsAfterPresentation =
		presentationProbe.locationClassification.load();
	const std::string lexicalContainerFormat =
		jpegview_linux::ArchiveContainerFormatName(archive);
	const int classificationStatsAfterLexicalFormat =
		presentationProbe.locationClassification.load();
	jpegview_linux::SetArchiveSourceProbeHookForTesting(nullptr, nullptr);
	Expect(presentation.archiveLocation &&
		presentation.displayName == archive.string() + "!/nested" &&
		presentation.formatName == "ZIP" &&
		classificationStatsAfterPresentation == 1 &&
		lexicalContainerFormat == "ZIP" &&
		classificationStatsAfterLexicalFormat == classificationStatsAfterPresentation,
		"prepared archive presentation repeated source classification or changed its label");
	Expect(jpegview_linux::ArchiveTimestampNanoseconds(std::numeric_limits<std::int64_t>::max()) ==
			std::numeric_limits<std::int64_t>::max() &&
		jpegview_linux::ArchiveTimestampNanoseconds(std::numeric_limits<std::int64_t>::min()) ==
			std::numeric_limits<std::int64_t>::min(),
		"archive timestamp conversion overflowed at extreme values");
	std::error_code archiveTimeError;
	const fs::file_time_type memberModificationTime =
		jpegview_linux::ImageSourceModificationTime(rootImage, archiveTimeError);
	const fs::file_time_type sourceModificationTime = fs::last_write_time(image);
	Expect(!archiveTimeError && std::chrono::duration_cast<std::chrono::seconds>(
		memberModificationTime - sourceModificationTime).count() >= -2 &&
		std::chrono::duration_cast<std::chrono::seconds>(
			memberModificationTime - sourceModificationTime).count() <= 2,
		"archive member timestamps were not converted to the filesystem clock domain");

	std::vector<jpegview_linux::ArchiveEntryInfo> entries;
	Expect(jpegview_linux::ListArchiveDirectory(archive, entries, error),
		"cannot list ZIP central-directory entries: " + error);
	Expect(entries.size() == 3 &&
		std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
			return entry.directory && entry.path.filename() == "nested";
		}) &&
		std::none_of(entries.begin(), entries.end(), [](const auto& entry) {
			return entry.path.filename() == "escape.png";
		}),
		"archive listing omitted an implicit directory or accepted a traversal member");
	jpegview_linux::ArchiveMemberInfo memberInfo;
	Expect(jpegview_linux::GetArchiveMemberInfo(rootImage, memberInfo, error) &&
		memberInfo.size == fs::file_size(image),
		"archive member metadata did not preserve the uncompressed size");

	FileList files({archive.string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 1 && files.Current() == rootImage && files.IsArchiveMember(0),
		"opening a ZIP container did not initialize its image list or archive identity");
	FileListPreparedScan asyncArchiveScan = FileList::PrepareScan(
		FileList::InitialScanRequest({archive.string()}, FileList::SortMode::FileName,
			true, false, FileList::NavigationMode::LoopDirectory),
		[] { return true; });
	Expect(asyncArchiveScan.completed && asyncArchiveScan.targetFound &&
		asyncArchiveScan.replacement.Current() == rootImage &&
		asyncArchiveScan.replacement.IsArchiveMember(0),
		"an asynchronous initial scan did not prepare an archive catalog and image list");
	Expect(files.MarkCurrentForToggle(), "archive image could not be marked for A/B toggling");
	files.SetNavigationMode(FileList::NavigationMode::LoopSubDirectories);
	Expect(files.Next() && files.Current() == nestedImage && files.IsArchiveMember(0),
		"recursive file-list navigation did not enter an archive subdirectory");
	Expect(files.Previous() && files.Current() == rootImage && files.Next() &&
		files.Current() == nestedImage && files.ToggleBetweenMarkedAndCurrent() &&
		files.Current() == rootImage && files.ToggleBetweenMarkedAndCurrent() &&
		files.Current() == nestedImage,
		"archive file-list direction changes or marked-image toggling selected the wrong member");
	FileList directMember({nestedImage.string()}, FileList::SortMode::FileName, true, false);
	Expect(directMember.Size() == 1 && directMember.Current() == nestedImage,
		"opening an archive member directly did not select it in its virtual parent directory");

	DecodedImage decoded;
	Expect(jpegview_linux::DecodeImage(rootImage, decoded, error),
		"image decoder could not read an archive member: " + error);
	Expect(decoded.frames.size() == 1 && decoded.frames.front().width == 2 &&
		decoded.frames.front().height == 2 && decoded.frames.front().bgra == pixels,
		"archive member decoding changed dimensions or pixels");

	const jpegview_linux::DirectorySummary summary =
		jpegview_linux::CountImmediateDirectoryContents(archive);
	Expect(summary.imageCount == 1 && summary.subdirectoryCount == 1 &&
		jpegview_linux::FirstImageInDirectory(archive,
			jpegview_linux::FileDialogSortMode::Name) == rootImage &&
		jpegview_linux::FirstImageInDirectory(nestedDirectory,
			jpegview_linux::FileDialogSortMode::Name) == nestedImage,
		"open-dialog summaries or folder preview resolution did not browse archive contents");
	jpegview_linux::FileDialogPreviewLoader previewLoader;
	const std::uint64_t generation = previewLoader.Request(rootImage, false,
		jpegview_linux::FileDialogSortMode::Name, 16, 16);
	std::vector<jpegview_linux::FileDialogPreviewResult> previews;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (previews.empty() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		previews = previewLoader.TakeReady();
	}
	Expect(previews.size() == 1 && previews.front().generation == generation &&
		previews.front().source == rootImage && previews.front().error.empty() &&
		previews.front().width == 2 && previews.front().height == 2 &&
		previews.front().sourceWidth == 2 && previews.front().sourceHeight == 2 &&
		previews.front().fileSizeKnown && previews.front().fileSize == memberInfo.size,
		"recent-image preview did not decode a virtual archive member");
	const jpegview_linux::SourceDescriptor archivePlaceholder(rootImage, {}, {});
	jpegview_linux::FileDialogEntry recentArchiveEntry{rootImage, false, false, {}, false, true};
	recentArchiveEntry.sourceDescriptor = archivePlaceholder;
	jpegview_linux::FileDialogModel recentArchiveModel;
	recentArchiveModel.SetEntries({recentArchiveEntry});
	jpegview_linux::FileDialogFileSizeLoader archiveSizeLoader;
	archiveSizeLoader.RequestSources({archivePlaceholder}, 1);
	std::vector<jpegview_linux::FileDialogFileSizeResult> archiveSizes;
	const auto archiveSizeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (archiveSizes.empty() && std::chrono::steady_clock::now() < archiveSizeDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		archiveSizes = archiveSizeLoader.TakeReady();
	}
	Expect(archiveSizes.size() == 1 && archiveSizes.front().path == rootImage &&
		archiveSizes.front().size == fs::file_size(image) &&
		archiveSizes.front().requestedSource.Key() == archivePlaceholder.Key() &&
		archiveSizes.front().observedSource.Valid() &&
		archiveSizes.front().observedSource.Metadata().archiveMember &&
		archiveSizes.front().parentDisplayName == archive.string() + " (ZIP)" &&
		recentArchiveModel.RefreshSourceDescriptor(archivePlaceholder.Key(),
			archiveSizes.front().observedSource,
			archiveSizes.front().parentDisplayName) &&
		recentArchiveModel.AllEntries().front().fileSizeKnown &&
		recentArchiveModel.AllEntries().front().fileSize == fs::file_size(image) &&
		recentArchiveModel.AllEntries().front().archiveMember &&
		recentArchiveModel.AllEntries().front().parentDisplayName ==
			archive.string() + " (ZIP)",
		"background file-size lookup did not preserve archive-member size and prepared Recents parent label");

	const fs::path comicArchive = temporary.path() / "comic.CBZ";
	Expect(fs::copy_file(archive, comicArchive), "could not copy ZIP fixture as CBZ");
	const fs::path comicImage = comicArchive / "01-root.png";
	Expect(jpegview_linux::IsArchiveContainerName(comicArchive) &&
		jpegview_linux::IsArchiveContainerFile(comicArchive) &&
		jpegview_linux::IsArchiveMemberLocation(comicImage) &&
		jpegview_linux::ArchiveFormatName(comicImage) == "CBZ",
		"CBZ extension was not recognized or given its comic-archive label");
	entries.clear();
	Expect(jpegview_linux::ListArchiveDirectory(comicArchive, entries, error) &&
		std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
			return !entry.directory && entry.path == comicImage;
		}), "CBZ did not list its ZIP image members: " + error);
	FileList comicFiles({comicArchive.string()}, FileList::SortMode::FileName, true, false);
	Expect(comicFiles.Size() == 1 && comicFiles.Current() == comicImage &&
		comicFiles.IsArchiveMember(0), "opening a CBZ did not expose its image as a virtual file");
	DecodedImage comicDecoded;
	Expect(jpegview_linux::DecodeImage(comicImage, comicDecoded, error) &&
		comicDecoded.frames.size() == 1 && comicDecoded.frames.front().bgra == pixels,
		"CBZ member decoding did not reuse the ZIP reader: " + error);

	const fs::path malformed = temporary.path() / "broken.zip";
	WriteText(malformed, "not a ZIP archive");
	entries.clear();
	Expect(!jpegview_linux::ListArchiveDirectory(malformed, entries, error) && !error.empty(),
		"malformed ZIP input did not fail with a useful error");
}

void TestArchiveCatalogFailureReleasesLoadingState() {
	TemporaryDirectory temporary;
	const fs::path payload = temporary.path() / "catalog-payload.bin";
	const fs::path archive = temporary.path() / "catalog-exception.zip";
	WriteBytes(payload, {1, 2, 3, 4, 5, 6});
	WriteZipArchive(archive, {{"image.jpg", payload}});
	const fs::path member = archive / "image.jpg";
	ArchiveCatalogFailureInjection injection;
	jpegview_linux::WorkContext preadmittedContext = jpegview_linux::MakePathWorkContext(
		member, jpegview_linux::SourceWorkPriority::Metadata);
	preadmittedContext.sourceAccessAlreadyAdmitted = true;
	preadmittedContext.cpuProcessingAlreadyAdmitted = true;
	bool firstLoadSucceeded = false;
	jpegview_linux::ArchiveErrorKind firstErrorKind = jpegview_linux::ArchiveErrorKind::None;
	std::string firstError;
	std::mutex waiterMutex;
	std::condition_variable waiterChanged;
	bool waiterObservedLoading = false;
	bool waiterFinished = false;
	std::atomic<int> waiterContinueCalls{0};
	std::chrono::steady_clock::time_point waiterDeadline =
		std::chrono::steady_clock::time_point::max();
	std::atomic<bool> waiterCanceled{false};
	jpegview_linux::WorkContext waiterContext = preadmittedContext;
	bool waiterLoadSucceeded = false;
	jpegview_linux::ArchiveErrorKind waiterErrorKind = jpegview_linux::ArchiveErrorKind::None;
	std::string waiterError;
	std::thread firstLoader;
	std::thread waiter;
	jpegview_linux::SetArchiveCatalogTestHookForTesting(
		ThrowAtArchiveCatalogHook, &injection);
	struct CatalogHookReset {
		bool active = true;

		void Reset() {
			if (!active) return;
			jpegview_linux::SetArchiveCatalogTestHookForTesting(nullptr, nullptr);
			active = false;
		}

		~CatalogHookReset() { Reset(); }
	} hookReset;
	struct CatalogFailureThreadCleanup {
		ArchiveCatalogFailureInjection& injection;
		std::atomic<bool>& waiterCanceled;
		std::condition_variable& waiterChanged;
		std::thread& firstLoader;
		std::thread& waiter;

		void ReleaseLoader() {
			{
				std::lock_guard<std::mutex> lock(injection.mutex);
				injection.release = true;
			}
			injection.changed.notify_all();
		}

		void CancelWaiter() {
			waiterCanceled.store(true);
			waiterChanged.notify_all();
		}

		void JoinThreads() {
			if (firstLoader.joinable()) firstLoader.join();
			if (waiter.joinable()) waiter.join();
		}

		~CatalogFailureThreadCleanup() {
			ReleaseLoader();
			CancelWaiter();
			JoinThreads();
		}
	} threadCleanup{injection, waiterCanceled, waiterChanged, firstLoader, waiter};
	waiterContext.shouldContinue = [&] {
		bool notify = false;
		bool keepWaiting = false;
		{
			std::lock_guard<std::mutex> lock(waiterMutex);
			const int call = waiterContinueCalls.fetch_add(1);
			if (call == 0) {
				waiterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			}
			if (call == 1) {
				waiterObservedLoading = true;
				notify = true;
			}
			keepWaiting = !waiterCanceled.load() &&
				std::chrono::steady_clock::now() < waiterDeadline;
		}
		if (notify) waiterChanged.notify_all();
		return keepWaiting;
	};
	firstLoader = std::thread([&] {
		jpegview_linux::ArchiveMemberInfo info;
		firstLoadSucceeded = jpegview_linux::GetArchiveMemberInfo(member, info,
			firstError, &firstErrorKind, preadmittedContext);
	});
	bool loaderHookEntered = false;
	{
		std::unique_lock<std::mutex> lock(injection.mutex);
		loaderHookEntered = injection.changed.wait_for(lock, std::chrono::seconds(3), [&] {
			return injection.entered;
		});
	}
	if (loaderHookEntered) {
		waiter = std::thread([&] {
			jpegview_linux::ArchiveMemberInfo info;
			waiterLoadSucceeded = jpegview_linux::GetArchiveMemberInfo(member, info,
				waiterError, &waiterErrorKind, waiterContext);
			{
				std::lock_guard<std::mutex> lock(waiterMutex);
				waiterFinished = true;
			}
			waiterChanged.notify_all();
		});
	}
	bool waiterReachedLoadingState = false;
	if (loaderHookEntered) {
		std::unique_lock<std::mutex> lock(waiterMutex);
		waiterReachedLoadingState = waiterChanged.wait_for(lock,
			std::chrono::seconds(3), [&] { return waiterObservedLoading; });
	}
	threadCleanup.ReleaseLoader();
	if (firstLoader.joinable()) firstLoader.join();
	bool waiterCompletionObserved = false;
	if (waiter.joinable()) {
		std::unique_lock<std::mutex> lock(waiterMutex);
		waiterCompletionObserved = waiterChanged.wait_for(lock,
			std::chrono::seconds(3), [&] { return waiterFinished; });
		lock.unlock();
		if (!waiterCompletionObserved) threadCleanup.CancelWaiter();
		if (waiter.joinable()) waiter.join();
	}
	hookReset.Reset();
	const bool firstLoadReportedFailure = !firstLoadSucceeded && injection.fired.load() &&
		firstErrorKind == jpegview_linux::ArchiveErrorKind::Other && !firstError.empty();
	const bool waiterReportedFailure = loaderHookEntered && waiterReachedLoadingState &&
		waiterCompletionObserved && !waiterLoadSucceeded &&
		waiterErrorKind == firstErrorKind && waiterError == firstError;
	jpegview_linux::ArchiveMemberInfo retryInfo;
	std::string retryError;
	const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	const jpegview_linux::WorkContext retryContext = jpegview_linux::MakePathWorkContext(
		member, jpegview_linux::SourceWorkPriority::Metadata, [&retryDeadline] {
			return std::chrono::steady_clock::now() < retryDeadline;
		});
	const bool retrySucceeded = jpegview_linux::GetArchiveMemberInfo(member, retryInfo,
		retryError, nullptr, retryContext) &&
		retryInfo.size == fs::file_size(payload);
	const auto snapshot = jpegview_linux::SourceWorkCoordinator::Global().Snapshot();
	Expect(firstLoadReportedFailure && waiterReportedFailure && retrySucceeded &&
		snapshot.activeForeground == 0 &&
		snapshot.activeSpeculative == 0 && snapshot.activeCpu == 0 &&
		snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0 &&
		snapshot.waitingCpu == 0,
		"an exception left a cold archive catalog marked loading or leaked worker admission");
}

void TestColdArchiveMemberMetadataYieldAndReplacement() {
	TemporaryDirectory temporary;
	const fs::path payload = temporary.path() / "metadata-payload.bin";
	WriteBytes(payload, {8, 7, 6, 5, 4, 3, 2, 1});
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const auto coordinatorIdle = [&coordinator] {
		return coordinator.WaitForSnapshot([](const auto& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingForeground == 0 &&
				snapshot.waitingSpeculative == 0 && snapshot.waitingCpu == 0 &&
				!snapshot.foregroundPending;
		}, std::chrono::seconds(3));
	};

	const fs::path retryArchive = temporary.path() / "metadata-retry.zip";
	WriteZipArchive(retryArchive, {{"image.jpg", payload}});
	const fs::path retryMember = retryArchive / "image.jpg";
	const jpegview_linux::SourceDescriptor retryPlaceholder(retryMember, {}, {});
	std::atomic<bool> retryGateInjected{false};
	std::atomic<bool> retrySawPairedAdmission{false};
	std::mutex retryMutex;
	std::condition_variable retryChanged;
	const auto retryCapture = [&](const jpegview_linux::SourceDescriptor& requested) {
		if (requested.LogicalPath() == retryMember && !retryGateInjected.load()) {
			const auto snapshot = coordinator.Snapshot();
			if (snapshot.activeSpeculative == 1 && snapshot.activeCpu == 1) {
				retrySawPairedAdmission.store(true);
				coordinator.SetForegroundPending(true);
				{
					std::lock_guard<std::mutex> lock(retryMutex);
					retryGateInjected.store(true);
				}
				retryChanged.notify_all();
			}
		}
		return jpegview_linux::DescribeImageSource(requested.LogicalPath());
	};
	jpegview_linux::FileDialogFileSizeLoader retryLoader(retryCapture);
	const bool retryCoordinatorIdle = coordinatorIdle();
	retryLoader.RequestSources({retryPlaceholder}, 901);
	bool retryGateObserved = false;
	{
		std::unique_lock<std::mutex> lock(retryMutex);
		retryGateObserved = retryChanged.wait_for(lock, std::chrono::seconds(3), [&] {
			return retryGateInjected.load();
		});
	}
	auto retryForegroundCanceled = std::make_shared<std::atomic<bool>>(false);
	const auto retryForegroundContext = jpegview_linux::MakePathWorkContext(retryMember,
		jpegview_linux::SourceWorkPriority::Foreground,
		[retryForegroundCanceled] { return !retryForegroundCanceled->load(); });
	auto retryForegroundFuture = std::async(std::launch::async, [&coordinator,
		retryMember, retryForegroundContext] {
		return coordinator.Acquire(retryForegroundContext, retryMember);
	});
	const bool retryForegroundAdmitted = coordinator.WaitForSnapshot(
		[](const auto& snapshot) {
			return snapshot.activeForeground == 1 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(3));
	const bool retryWithheldWhileForeground = retryLoader.TakeReady().empty();
	if (!retryForegroundAdmitted) {
		retryForegroundCanceled->store(true);
		coordinator.SetForegroundPending(false);
		coordinator.NotifyWaiters();
	}
	jpegview_linux::SourceWorkLease retryForegroundLease = retryForegroundFuture.get();
	coordinator.SetForegroundPending(false);
	retryForegroundLease.Reset();
	std::vector<jpegview_linux::FileDialogFileSizeResult> retryResults;
	const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (retryResults.empty() && std::chrono::steady_clock::now() < retryDeadline) {
		retryResults = retryLoader.TakeReady();
		if (retryResults.empty()) std::this_thread::yield();
	}
	const bool retryPublishedCurrent = retryResults.size() == 1 &&
		retryResults.front().generation == 901 && retryResults.front().path == retryMember &&
		retryResults.front().size == fs::file_size(payload) &&
		retryResults.front().observedSource.Metadata().hasFileSize;
	Expect(retryCoordinatorIdle && retryGateObserved && retrySawPairedAdmission.load() &&
		retryForegroundAdmitted && retryWithheldWhileForeground && retryPublishedCurrent &&
		coordinatorIdle(),
		"cold archive member metadata did not yield paired permits and retry its current row result");

	const fs::path replacedArchive = temporary.path() / "metadata-replaced.zip";
	WriteZipArchive(replacedArchive, {{"old.jpg", payload}});
	const fs::path replacedMember = replacedArchive / "old.jpg";
	const fs::path replacementFile = temporary.path() / "replacement-row.bin";
	WriteText(replacementFile, "replacement generation result");
	const jpegview_linux::SourceDescriptor oldPlaceholder(replacedMember, {}, {});
	const jpegview_linux::SourceDescriptor replacementPlaceholder(replacementFile, {}, {});
	std::atomic<bool> replacementGateInjected{false};
	std::mutex replacementMutex;
	std::condition_variable replacementChanged;
	const auto replacementCapture = [&](const jpegview_linux::SourceDescriptor& requested) {
		if (requested.LogicalPath() == replacedMember && !replacementGateInjected.load()) {
			const auto snapshot = coordinator.Snapshot();
			if (snapshot.activeSpeculative == 1 && snapshot.activeCpu == 1) {
				coordinator.SetForegroundPending(true);
				{
					std::lock_guard<std::mutex> lock(replacementMutex);
					replacementGateInjected.store(true);
				}
				replacementChanged.notify_all();
			}
		}
		return jpegview_linux::DescribeImageSource(requested.LogicalPath());
	};
	jpegview_linux::FileDialogFileSizeLoader replacementLoader(replacementCapture);
	const bool replacementCoordinatorIdle = coordinatorIdle();
	replacementLoader.RequestSources({oldPlaceholder}, 910);
	bool replacementGateObserved = false;
	{
		std::unique_lock<std::mutex> lock(replacementMutex);
		replacementGateObserved = replacementChanged.wait_for(lock,
			std::chrono::seconds(3), [&] { return replacementGateInjected.load(); });
	}
	auto replacementForegroundCanceled = std::make_shared<std::atomic<bool>>(false);
	const auto replacementForegroundContext = jpegview_linux::MakePathWorkContext(replacedMember,
		jpegview_linux::SourceWorkPriority::Foreground,
		[replacementForegroundCanceled] { return !replacementForegroundCanceled->load(); });
	auto replacementForegroundFuture = std::async(std::launch::async,
		[&coordinator, replacedMember, replacementForegroundContext] {
			return coordinator.Acquire(replacementForegroundContext, replacedMember);
		});
	const bool replacementForegroundAdmitted = coordinator.WaitForSnapshot(
		[](const auto& snapshot) {
			return snapshot.activeForeground == 1 && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0;
		}, std::chrono::seconds(3));
	replacementLoader.RequestSources({replacementPlaceholder}, 911);
	const bool replacedResultWithheld = replacementLoader.TakeReady().empty();
	if (!replacementForegroundAdmitted) {
		replacementForegroundCanceled->store(true);
		coordinator.SetForegroundPending(false);
		coordinator.NotifyWaiters();
	}
	jpegview_linux::SourceWorkLease replacementForegroundLease =
		replacementForegroundFuture.get();
	coordinator.SetForegroundPending(false);
	replacementForegroundLease.Reset();
	std::vector<jpegview_linux::FileDialogFileSizeResult> replacementResults;
	const auto replacementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (replacementResults.empty() &&
		std::chrono::steady_clock::now() < replacementDeadline) {
		replacementResults = replacementLoader.TakeReady();
		if (replacementResults.empty()) std::this_thread::yield();
	}
	Expect(replacementCoordinatorIdle && replacementGateObserved &&
		replacementForegroundAdmitted && replacedResultWithheld &&
		replacementResults.size() == 1 && replacementResults.front().generation == 911 &&
		replacementResults.front().path == replacementFile &&
		replacementResults.front().size == fs::file_size(replacementFile) && coordinatorIdle(),
		"a replaced archive metadata request published stale data or held paired admission");
}

void TestEncryptedZipBrowsingAndSessionPasswords() {
	TemporaryDirectory temporary;
	const fs::path fixture = fs::path(__FILE__).parent_path() / "fixtures" / "encrypted-zip.zip";
	const fs::path archive = temporary.path() / "encrypted.zip";
	Expect(fs::copy_file(fixture, archive), "could not copy encrypted ZIP test fixture");
	const fs::path member = archive / "inside-password.ppm";

	std::vector<jpegview_linux::ArchiveEntryInfo> entries;
	bool containsEncryptedEntries = false;
	jpegview_linux::ArchiveErrorKind errorKind = jpegview_linux::ArchiveErrorKind::None;
	std::string error;
	Expect(jpegview_linux::ListArchiveDirectoryCancellable(archive, entries,
		[] { return true; }, error, &errorKind, &containsEncryptedEntries),
		"encrypted ZIP central directory could not be listed: " + error);
	Expect(containsEncryptedEntries && entries.size() == 1 &&
		entries.front().path == member && !entries.front().directory && entries.front().encrypted,
		"encrypted ZIP member was not exposed and marked in the virtual directory listing");
	jpegview_linux::ArchiveMemberInfo info;
	Expect(jpegview_linux::GetArchiveMemberInfo(member, info, error) && info.encrypted,
		"encrypted ZIP member metadata lost its encryption state");

	bool callbackCalled = false;
	Expect(!jpegview_linux::WithArchiveMemberFile(member,
		[&callbackCalled](const fs::path&, std::string&) {
			callbackCalled = true;
			return true;
		}, error, &errorKind) && !callbackCalled &&
		errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired,
		"encrypted ZIP image was read without requesting a password");

	Expect(!jpegview_linux::ValidateArchivePassword(archive, "incorrect", error, &errorKind) &&
		errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword,
		"incorrect ZIP password was not rejected deterministically");
	const fs::path cancellationArchive = temporary.path() / "cancel-validation.zip";
	Expect(fs::copy_file(fixture, cancellationArchive),
		"could not copy the ZIP archive cancellation fixture");
	int validationContinuationChecks = 0;
	const jpegview_linux::WorkContext canceledValidation =
		jpegview_linux::MakePathWorkContext(cancellationArchive,
			jpegview_linux::SourceWorkPriority::Metadata,
			[&validationContinuationChecks] {
				return ++validationContinuationChecks < 9;
			});
	Expect(!jpegview_linux::ValidateArchivePassword(cancellationArchive,
		"jpegview-test-password", error, &errorKind, canceledValidation) &&
		error.find("cancelled") != std::string::npos &&
		errorKind == jpegview_linux::ArchiveErrorKind::Other &&
		validationContinuationChecks == 9,
		"ZIP password probe ignored cancellation after cataloging or failed to release admission");

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	std::vector<jpegview_linux::CpuWorkLease> occupiedCpu;
	const std::size_t cpuLimit = jpegview_linux::HardwareAwareCpuWorkerCount();
	for (std::size_t index = 0; index < cpuLimit; ++index) {
		auto lease = coordinator.AcquireCpu(jpegview_linux::MakePathWorkContext(
			temporary.path() / ("archive-cpu-" + std::to_string(index)),
			jpegview_linux::SourceWorkPriority::Speculative));
		if (lease) occupiedCpu.push_back(std::move(lease));
	}
	jpegview_linux::ArchiveDirectoryLoader admittedValidationLoader;
	constexpr std::uint64_t replacedValidationGeneration = 74;
	constexpr std::uint64_t currentValidationGeneration = 75;
	admittedValidationLoader.RequestPasswordValidation(archive,
		"jpegview-test-password", replacedValidationGeneration);
	const bool validationWaitedForSharedCpu = coordinator.WaitForSnapshot(
		[cpuLimit](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeCpu == cpuLimit && snapshot.waitingCpu >= 1 &&
				snapshot.waitingSpeculative >= 1;
		}, std::chrono::seconds(2));
	const bool validationWithheldBeforeAdmission =
		admittedValidationLoader.TakeReady().empty();
	admittedValidationLoader.RequestPasswordValidation(archive,
		"jpegview-test-password", currentValidationGeneration);
	for (auto& lease : occupiedCpu) lease.Reset();
	std::vector<jpegview_linux::ArchiveDirectoryResult> admittedResults;
	const auto admittedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (admittedResults.empty() && std::chrono::steady_clock::now() < admittedDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		admittedResults = admittedValidationLoader.TakeReady();
	}
	Expect(validationWaitedForSharedCpu && validationWithheldBeforeAdmission &&
		admittedResults.size() == 1 && admittedResults.front().passwordValidation &&
		admittedResults.front().generation == currentValidationGeneration &&
		admittedResults.front().error.empty(),
		"archive password validation bypassed paired admission or published a replaced generation");
	jpegview_linux::ArchiveDirectoryLoader validationLoader;
	constexpr std::uint64_t validationGeneration = 73;
	validationLoader.RequestPasswordValidation(archive, "incorrect", validationGeneration);
	std::vector<jpegview_linux::ArchiveDirectoryResult> validationResults;
	const auto validationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (validationResults.empty() && std::chrono::steady_clock::now() < validationDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		validationResults = validationLoader.TakeReady();
	}
	Expect(validationResults.size() == 1 && validationResults.front().passwordValidation &&
		validationResults.front().generation == validationGeneration &&
		validationResults.front().errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword,
		"background password validation did not publish a bounded, generation-tagged rejection");
	Expect(jpegview_linux::ValidateArchivePassword(archive, "jpegview-test-password", error,
		&errorKind),
		"correct ZIP password failed validation: " + error);
	Expect(jpegview_linux::SetSessionArchivePassword(archive, "jpegview-test-password") &&
		jpegview_linux::HasSessionArchivePassword(member),
		"validated ZIP password was not cached against its backing archive for this run");

	DecodedImage decoded;
	Expect(jpegview_linux::DecodeImage(member, decoded, error),
		"cached ZIP password could not decode the encrypted image: " + error);
	const std::vector<std::uint8_t> expectedPixels = {
		0x90, 0x60, 0x30, 255, 0xa0, 0x70, 0x40, 255,
		0xb0, 0x80, 0x50, 255, 0xc0, 0x90, 0x60, 255};
	Expect(decoded.frames.size() == 1 && decoded.frames.front().bgra == expectedPixels,
		"encrypted ZIP image decoding changed its pixels");
	jpegview_linux::ClearSessionArchivePasswords();
	Expect(!jpegview_linux::HasSessionArchivePassword(archive),
		"clearing the in-memory password cache retained a ZIP password");
	Expect(!jpegview_linux::WithArchiveMemberFile(member,
		[](const fs::path&, std::string&) { return true; }, error, &errorKind) &&
		errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired,
		"ZIP password cache clearing did not require a new password");

	jpegview_linux::FileDialogPreviewLoader previewLoader;
	const std::uint64_t generation = previewLoader.Request(member, false,
		jpegview_linux::FileDialogSortMode::Name, 16, 16);
	std::vector<jpegview_linux::FileDialogPreviewResult> previews;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (previews.empty() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		previews = previewLoader.TakeReady();
	}
	Expect(previews.size() == 1 && previews.front().generation == generation &&
		previews.front().bgra.empty() && previews.front().error.find("password required") !=
		std::string::npos,
		"encrypted preview did not report a locked state without a password prompt");
}

void TestEncryptedSevenZipBrowsingAndSessionPasswords() {
	struct PasswordCacheGuard {
		PasswordCacheGuard() { jpegview_linux::ClearSessionArchivePasswords(); }
		~PasswordCacheGuard() { jpegview_linux::ClearSessionArchivePasswords(); }
	} passwordCacheGuard;
	TemporaryDirectory temporary;
	const fs::path fixtureDirectory = fs::path(__FILE__).parent_path() / "fixtures";
	const fs::path dataArchive = temporary.path() / "data-encrypted.7z";
	const fs::path headerArchive = temporary.path() / "header-encrypted.7z";
	Expect(fs::copy_file(fixtureDirectory / "data-encrypted.7z", dataArchive) &&
		fs::copy_file(fixtureDirectory / "header-encrypted.7z", headerArchive),
		"could not copy encrypted 7z test fixtures");
	std::vector<jpegview_linux::ArchiveEntryInfo> entries;
	std::string error;
	jpegview_linux::ArchiveErrorKind errorKind = jpegview_linux::ArchiveErrorKind::None;
	bool containsEncryptedEntries = false;
	if (!jpegview_linux::SevenZipBackendAvailable()) {
		Expect(!jpegview_linux::ListArchiveDirectoryCancellable(dataArchive, entries,
			[] { return true; }, error, &errorKind, &containsEncryptedEntries) &&
			errorKind == jpegview_linux::ArchiveErrorKind::UnsupportedEncryption,
			"normal-build fallback did not report unavailable encrypted 7z support");
		return;
	}

	const fs::path dataMember = dataArchive / "visible.png";
	const fs::path headerMember = headerArchive / "visible.png";
	// 7z represents a zero-byte regular file without an encrypted data stream.
	// Password validation must not mistake it for a password check and must
	// continue to the non-empty encrypted payload in the same archive.
	const bool listedDataArchive = jpegview_linux::ListArchiveDirectoryCancellable(dataArchive,
		entries, [] { return true; }, error, &errorKind, &containsEncryptedEntries);
	const auto emptyDataMember = std::find_if(entries.begin(), entries.end(),
		[](const auto& entry) { return entry.path.filename() == "empty.dat"; });
	const auto encryptedDataMember = std::find_if(entries.begin(), entries.end(),
		[](const auto& entry) { return entry.path.filename() == "visible.png"; });
	Expect(listedDataArchive && containsEncryptedEntries && entries.size() == 2 &&
		emptyDataMember != entries.end() && emptyDataMember->size == 0 &&
		!emptyDataMember->encrypted && encryptedDataMember != entries.end() &&
		encryptedDataMember->path == dataMember && encryptedDataMember->encrypted,
		"data-encrypted 7z did not retain visible names and encrypted-member metadata: " + error +
		" (listed=" + (listedDataArchive ? "true" : "false") + ", kind=" +
		std::to_string(static_cast<int>(errorKind)) + ", entries=" +
		std::to_string(entries.size()) + ", containsEncrypted=" +
		(containsEncryptedEntries ? "true" : "false") + (entries.empty() ? "" :
		", path=" + entries.front().path.string() + ", encrypted=" +
		(entries.front().encrypted ? "true" : "false")) + ")");
	jpegview_linux::SevenZipEntry emptyEncryptedProbe;
	emptyEncryptedProbe.encrypted = true;
	jpegview_linux::SevenZipEntry encryptedDataProbe;
	encryptedDataProbe.encrypted = true;
	encryptedDataProbe.size = 1;
	Expect(!jpegview_linux::IsSevenZipPasswordVerificationMember(emptyEncryptedProbe) &&
		jpegview_linux::IsSevenZipPasswordVerificationMember(encryptedDataProbe),
		"zero-length encrypted 7z metadata was treated as a password-verification signal");
	entries.clear();
	containsEncryptedEntries = false;
	Expect(!jpegview_linux::ListArchiveDirectoryCancellable(headerArchive, entries,
		[] { return true; }, error, &errorKind, &containsEncryptedEntries) &&
		errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired && entries.empty(),
		"header-encrypted 7z exposed hidden names or did not request a password");

	const auto awaitPreview = [](jpegview_linux::FileDialogPreviewLoader& loader,
		const fs::path& archive) {
		const std::uint64_t generation = loader.Request(archive, true,
			jpegview_linux::FileDialogSortMode::Name, 32, 32);
		std::vector<jpegview_linux::FileDialogPreviewResult> results;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (results.empty() && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			results = loader.TakeReady();
		}
		Expect(results.size() == 1 && results.front().generation == generation,
			"7z preview did not finish before its bounded deadline");
		return results.front();
	};
	jpegview_linux::FileDialogPreviewLoader previewLoader;
	const auto dataPreview = awaitPreview(previewLoader, dataArchive);
	Expect(dataPreview.errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired &&
		dataPreview.encryptedArchive && dataPreview.source == dataArchive &&
		dataPreview.bgra.empty(),
		"data-encrypted 7z preview did not stay locked without prompting");
	const auto headerPreview = awaitPreview(previewLoader, headerArchive);
	Expect(headerPreview.errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired &&
		headerPreview.source == headerArchive && headerPreview.bgra.empty(),
		"header-encrypted 7z preview did not stay locked without prompting");

	Expect(!jpegview_linux::ValidateArchivePassword(dataArchive, "wrong-password", error,
		&errorKind) && errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword,
		"wrong data-encrypted 7z password was not rejected");
	Expect(!jpegview_linux::ValidateArchivePassword(headerArchive, "wrong-password", error,
		&errorKind) && errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword,
		"wrong header-encrypted 7z password was not rejected");
	Expect(jpegview_linux::ValidateArchivePassword(dataArchive, "test-secret", error,
		&errorKind), "correct data-encrypted 7z password failed: " + error);
	Expect(jpegview_linux::ValidateArchivePassword(headerArchive, "test-secret", error,
		&errorKind), "correct header-encrypted 7z password failed: " + error);

	Expect(jpegview_linux::SetSessionArchivePassword(dataArchive, "test-secret") &&
		jpegview_linux::HasSessionArchivePassword(dataMember),
		"data-encrypted 7z password was not cached by backing-file identity");
	DecodedImage decoded;
	Expect(jpegview_linux::DecodeImage(dataMember, decoded, error) &&
		decoded.frames.size() == 1,
		"cached data-encrypted 7z password did not decode the visible-name member: " + error);
	Expect(jpegview_linux::SetSessionArchivePassword(headerArchive, "test-secret") &&
		jpegview_linux::HasSessionArchivePassword(headerMember),
		"header-encrypted 7z password was not reused for member paths");
	entries.clear();
	Expect(jpegview_linux::ListArchiveDirectory(headerArchive, entries, error) &&
		entries.size() == 1 && entries.front().path == headerMember,
		"correct header-encrypted 7z password did not reveal names on a later open: " + error);
	DecodedImage headerDecoded;
	Expect(jpegview_linux::DecodeImage(headerMember, headerDecoded, error) &&
		headerDecoded.frames.size() == 1,
		"cached header-encrypted 7z password did not decode the image: " + error);

	Expect(!jpegview_linux::ValidateArchivePassword(dataArchive, "wrong-password", error,
		&errorKind) && errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword &&
		jpegview_linux::HasSessionArchivePassword(dataArchive),
		"submitted candidate validation was hidden by the existing credential cache");
	jpegview_linux::ClearSessionArchivePasswords();
	Expect(!jpegview_linux::HasSessionArchivePassword(dataArchive) &&
		!jpegview_linux::HasSessionArchivePassword(headerArchive),
		"clearing 7z credentials retained a session password");
	entries.clear();
	Expect(!jpegview_linux::ListArchiveDirectoryCancellable(headerArchive, entries,
		[] { return true; }, error, &errorKind) &&
		errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired,
		"clearing credentials retained a decrypted header-encrypted 7z catalog");
	Expect(!jpegview_linux::WithArchiveMemberFile(dataMember,
		[](const fs::path&, std::string&) { return true; }, error, &errorKind) &&
		errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired,
		"clearing 7z credentials did not relock a data-encrypted member");
}

void TestTarAndTgzBrowsingDecodingAndSafety() {
	TemporaryDirectory temporary;
	const fs::path sourceDirectory = temporary.path() / "source";
	fs::create_directories(sourceDirectory);
	const fs::path image = sourceDirectory / "fixture.png";
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	std::string error;
	Expect(jpegview_linux::WriteImage(image, pixels.data(), 2, 2, options, error),
		"cannot create TAR image fixture: " + error);
	const fs::path notes = sourceDirectory / "notes.txt";
	WriteText(notes, "not an image");
	const std::vector<std::pair<std::string, fs::path>> members = {
		{"01-root.png", image}, {"./nested/02-child.png", image},
		{"../escape.png", image}, {"/absolute.png", image}, {"notes.txt", notes},
	};
	const std::vector<std::pair<std::string, bool>> formats = {
		{"album.TAR", false}, {"album.tar.gz", true}, {"album.TGZ", true},
	};
	for (const auto& [archiveName, gzip] : formats) {
		const fs::path archive = temporary.path() / archiveName;
		WriteTarArchive(archive, members, gzip,
			{{"symbolic-link.png", "01-root.png"}},
			{{"hard-link.png", "01-root.png"}});
		const std::string expectedFormat = gzip ? "TGZ" : "TAR";
		const fs::path rootImage = archive / "01-root.png";
		const fs::path nestedDirectory = archive / "nested";
		const fs::path nestedImage = nestedDirectory / "02-child.png";

		Expect(jpegview_linux::IsArchiveContainerName(archive) &&
			jpegview_linux::IsArchiveContainerFile(archive) &&
			jpegview_linux::IsArchiveLocation(archive) &&
			jpegview_linux::IsArchiveLocation(nestedDirectory) &&
			jpegview_linux::IsArchiveMemberLocation(rootImage) &&
			!jpegview_linux::IsArchiveMemberLocation(archive),
			"TAR path recognition did not distinguish a container, virtual directory, and member");
		Expect(jpegview_linux::ArchiveFormatName(nestedImage) == expectedFormat &&
			jpegview_linux::ArchiveBackingFile(nestedImage) == archive &&
			jpegview_linux::ArchiveLocationDisplayName(nestedDirectory) ==
				archive.string() + "!/nested",
			"TAR display or physical-container mapping is incorrect");

		std::vector<jpegview_linux::ArchiveEntryInfo> entries;
		Expect(jpegview_linux::ListArchiveDirectory(archive, entries, error),
			"cannot list TAR headers: " + error);
		Expect(entries.size() == 3 &&
			std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
				return entry.directory && entry.path.filename() == "nested";
			}) &&
			std::none_of(entries.begin(), entries.end(), [](const auto& entry) {
				const std::string name = entry.path.filename().string();
				return name == "escape.png" || name == "absolute.png" ||
					name == "symbolic-link.png" || name == "hard-link.png";
			}),
			"TAR listing omitted an implied directory or accepted an unsafe/link member");
		entries.clear();
		Expect(jpegview_linux::ListArchiveDirectory(nestedDirectory, entries, error) &&
			entries.size() == 1 && entries.front().path == nestedImage && !entries.front().directory,
			"TAR ./ path normalization did not preserve its nested image");
		jpegview_linux::ArchiveMemberInfo memberInfo;
		Expect(jpegview_linux::GetArchiveMemberInfo(rootImage, memberInfo, error) &&
			memberInfo.size == fs::file_size(image) && memberInfo.modificationTime == 1700000000,
			"TAR member metadata did not preserve size and timestamp");
		const jpegview_linux::SourceDescriptor memberDescriptor =
			jpegview_linux::DescribeImageSource(rootImage);
		std::error_code memberTimeError;
		Expect(memberDescriptor.Valid() &&
			memberDescriptor.Metadata().modificationTimeNanoseconds ==
				1700000000ll * 1000000000ll &&
			jpegview_linux::ArchiveFileModificationTime(memberInfo.modificationTime) ==
				jpegview_linux::ImageSourceModificationTime(rootImage, memberTimeError) &&
			!memberTimeError,
			"archive source metadata mixed Unix epoch nanoseconds with file-clock nanoseconds");

		FileList files({archive.string()}, FileList::SortMode::FileName, true, false);
		Expect(files.Size() == 1 && files.Current() == rootImage && files.IsArchiveMember(0),
			"opening a TAR container did not initialize its root image list");
		Expect(files.DescriptorAt(0)->Metadata().modificationTimeNanoseconds ==
			1700000000ll * 1000000000ll,
			"file-list archive sort metadata did not retain the shared Unix epoch domain");
		files.SetNavigationMode(FileList::NavigationMode::LoopSubDirectories);
		Expect(files.Next() && files.Current() == nestedImage && files.IsArchiveMember(0),
			"recursive file-list navigation did not enter a TAR subdirectory");
		DecodedImage decoded;
		Expect(jpegview_linux::DecodeImage(rootImage, decoded, error),
			"image decoder could not read a TAR member: " + error);
		Expect(decoded.frames.size() == 1 && decoded.frames.front().width == 2 &&
			decoded.frames.front().height == 2 && decoded.frames.front().bgra == pixels,
			"TAR member decoding changed dimensions or pixels");
		DecodedImage nestedDecoded;
		Expect(jpegview_linux::DecodeImage(nestedImage, nestedDecoded, error) &&
			nestedDecoded.frames.size() == 1 && nestedDecoded.frames.front().bgra == pixels,
			"TAR lookup by header ordinal did not find a later nested member: " + error);
		if (archiveName == "album.tar.gz") {
			jpegview_linux::FileDialogPreviewLoader previewLoader;
			const std::uint64_t previewGeneration = previewLoader.Request(rootImage, false,
				jpegview_linux::FileDialogSortMode::Name, 16, 16);
			std::vector<jpegview_linux::FileDialogPreviewResult> previews;
			const auto previewDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (previews.empty() && std::chrono::steady_clock::now() < previewDeadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				previews = previewLoader.TakeReady();
			}
			Expect(previews.size() == 1 && previews.front().generation == previewGeneration &&
				previews.front().source == rootImage && previews.front().error.empty() &&
				previews.front().width == 2 && previews.front().height == 2,
				"open-dialog preview could not decode a TGZ member");

			jpegview_linux::ArchiveDirectoryLoader loader;
			loader.Request(archive, 20);
			loader.Request(nestedDirectory, 21);
			std::vector<jpegview_linux::ArchiveDirectoryResult> results;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (results.empty() && std::chrono::steady_clock::now() < deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				results = loader.TakeReady();
			}
			Expect(results.size() == 1 && results.front().generation == 21 &&
				results.front().directory == nestedDirectory && results.front().error.empty() &&
				results.front().entries.size() == 1 &&
				results.front().entries.front().path == nestedImage,
				"archive directory worker published a stale or incomplete result");
		}
	}

	const fs::path cancelled = temporary.path() / "cancelled.tgz";
	WriteTarArchive(cancelled, members, true);
	std::vector<jpegview_linux::ArchiveEntryInfo> cancelledEntries;
	Expect(!jpegview_linux::ListArchiveDirectoryCancellable(cancelled, cancelledEntries,
		[] { return false; }, error) && error.find("cancel") != std::string::npos,
		"TAR catalog cancellation did not interrupt a cold gzip stream");
	const fs::path malformed = temporary.path() / "broken.tar.gz";
	WriteText(malformed, "not a TAR or gzip archive");
	std::vector<jpegview_linux::ArchiveEntryInfo> entries;
	Expect(!jpegview_linux::ListArchiveDirectory(malformed, entries, error) && !error.empty(),
		"malformed TAR input did not fail with a useful error");
}

void TestSevenZipBrowsingDecodingAndPreview() {
	TemporaryDirectory temporary;
	const fs::path sourceDirectory = temporary.path() / "source";
	fs::create_directories(sourceDirectory);
	const fs::path image = sourceDirectory / "fixture.png";
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	std::string error;
	Expect(jpegview_linux::WriteImage(image, pixels.data(), 2, 2, options, error),
		"cannot create 7z image fixture: " + error);
	const fs::path notes = sourceDirectory / "notes.txt";
	WriteText(notes, "not an image");
	const fs::path archive = temporary.path() / "album.7Z";
	Write7zArchive(archive, {
		{"01-root.png", image}, {"nested/02-child.png", image},
		{"notes.txt", notes}, {"../escape.png", image}, {"/absolute.png", image},
	});
	const fs::path comicArchive = temporary.path() / "comic.CB7";
	Expect(fs::copy_file(archive, comicArchive), "could not copy 7z fixture as CB7");
	const fs::path rootImage = archive / "01-root.png";
	const fs::path nestedDirectory = archive / "nested";
	const fs::path nestedImage = nestedDirectory / "02-child.png";
	const fs::path comicImage = comicArchive / "01-root.png";

	Expect(jpegview_linux::IsArchiveContainerName(archive) &&
		jpegview_linux::IsArchiveContainerFile(archive) &&
		jpegview_linux::IsArchiveLocation(archive) &&
		jpegview_linux::IsArchiveLocation(nestedDirectory) &&
		jpegview_linux::IsArchiveMemberLocation(rootImage) &&
		jpegview_linux::ArchiveFormatName(nestedImage) == ".7Z" &&
		jpegview_linux::IsArchiveContainerName(comicArchive) &&
		jpegview_linux::IsArchiveContainerFile(comicArchive) &&
		jpegview_linux::IsArchiveMemberLocation(comicImage) &&
		jpegview_linux::ArchiveFormatName(comicImage) == "CB7" &&
		jpegview_linux::ArchiveBackingFile(nestedImage) == archive,
		"7z/CB7 path recognition did not preserve the virtual archive-member contract");

	std::vector<jpegview_linux::ArchiveEntryInfo> entries;
	Expect(jpegview_linux::ListArchiveDirectory(archive, entries, error),
		"cannot list 7z archive headers: " + error);
	Expect(entries.size() == 3 &&
		std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
			return entry.directory && entry.path.filename() == "nested";
		}) &&
		std::none_of(entries.begin(), entries.end(), [](const auto& entry) {
			const std::string name = entry.path.filename().string();
			return name == "escape.png" || name == "absolute.png";
		}),
		"7z listing omitted its implied folder or exposed an unsafe member path");
	entries.clear();
	Expect(jpegview_linux::ListArchiveDirectory(nestedDirectory, entries, error) &&
		entries.size() == 1 && entries.front().path == nestedImage && !entries.front().directory,
		"7z virtual-directory listing did not return its nested image");
	jpegview_linux::ArchiveMemberInfo memberInfo;
	Expect(jpegview_linux::GetArchiveMemberInfo(rootImage, memberInfo, error) &&
		memberInfo.size == fs::file_size(image) && memberInfo.modificationTime == 1700000000,
		"7z member metadata did not preserve size and timestamp");

	FileList files({archive.string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 1 && files.Current() == rootImage && files.IsArchiveMember(0),
		"opening a 7z container did not initialize its root image list");
	files.SetNavigationMode(FileList::NavigationMode::LoopSubDirectories);
	Expect(files.Next() && files.Current() == nestedImage && files.IsArchiveMember(0),
		"recursive file-list navigation did not enter a 7z subdirectory");
	DecodedImage decoded;
	Expect(jpegview_linux::DecodeImage(rootImage, decoded, error),
		"image decoder could not read a 7z member: " + error);
	Expect(decoded.frames.size() == 1 && decoded.frames.front().width == 2 &&
		decoded.frames.front().height == 2 && decoded.frames.front().bgra == pixels,
		"7z member decoding changed dimensions or pixels");
	std::vector<jpegview_linux::ArchiveEntryInfo> comicEntries;
	Expect(jpegview_linux::ListArchiveDirectory(comicArchive, comicEntries, error) &&
		std::any_of(comicEntries.begin(), comicEntries.end(), [](const auto& entry) {
			return !entry.directory && entry.path.filename() == "01-root.png";
		}), "CB7 did not list its 7z image members: " + error);
	FileList comicFiles({comicArchive.string()}, FileList::SortMode::FileName, true, false);
	Expect(comicFiles.Size() == 1 && comicFiles.Current() == comicImage && comicFiles.IsArchiveMember(0),
		"opening a CB7 did not expose its image as a virtual file");
	DecodedImage comicDecoded;
	Expect(jpegview_linux::DecodeImage(comicImage, comicDecoded, error),
		"CB7 member decoding did not reuse the 7z reader: " + error);
	Expect(comicDecoded.frames.size() == 1 && comicDecoded.frames.front().width == 2 &&
		comicDecoded.frames.front().height == 2 && comicDecoded.frames.front().bgra == pixels,
		"CB7 member decoding changed dimensions or pixels");
	DecodedImage nestedDecoded;
	Expect(jpegview_linux::DecodeImage(nestedImage, nestedDecoded, error) &&
		nestedDecoded.frames.size() == 1 && nestedDecoded.frames.front().bgra == pixels,
		"seekable 7z reader could not reopen and decode a later member: " + error);

	jpegview_linux::FileDialogPreviewLoader previewLoader;
	const std::uint64_t previewGeneration = previewLoader.Request(rootImage, false,
		jpegview_linux::FileDialogSortMode::Name, 16, 16);
	std::vector<jpegview_linux::FileDialogPreviewResult> previews;
	const auto previewDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (previews.empty() && std::chrono::steady_clock::now() < previewDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		previews = previewLoader.TakeReady();
	}
	Expect(previews.size() == 1 && previews.front().generation == previewGeneration &&
		previews.front().source == rootImage && previews.front().error.empty() &&
		previews.front().width == 2 && previews.front().height == 2,
		"open-dialog preview could not decode a 7z member");

	jpegview_linux::ArchiveDirectoryLoader directoryLoader;
	directoryLoader.Request(archive, 40);
	directoryLoader.Request(nestedDirectory, 41);
	std::vector<jpegview_linux::ArchiveDirectoryResult> results;
	const auto listingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < listingDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		results = directoryLoader.TakeReady();
	}
	Expect(results.size() == 1 && results.front().generation == 41 &&
		results.front().directory == nestedDirectory && results.front().error.empty() &&
		results.front().entries.size() == 1 && results.front().entries.front().path == nestedImage,
		"7z directory worker published a stale or incomplete listing");

	const fs::path cancelled = temporary.path() / "cancelled.7z";
	Write7zArchive(cancelled, {{"image.png", image}});
	std::vector<jpegview_linux::ArchiveEntryInfo> cancelledEntries;
	Expect(!jpegview_linux::ListArchiveDirectoryCancellable(cancelled, cancelledEntries,
		[] { return false; }, error) && error.find("cancel") != std::string::npos,
		"7z catalog cancellation did not stop a cold seekable scan");
	const fs::path malformed = temporary.path() / "broken.7z";
	WriteText(malformed, "not a 7z archive");
	entries.clear();
	Expect(!jpegview_linux::ListArchiveDirectory(malformed, entries, error) && !error.empty(),
		"malformed 7z input did not fail with a useful error");
}

void TestRarBrowsingDecodingAndPreview() {
	TemporaryDirectory temporary;
	std::string error;
	const fs::path rar4 = temporary.path() / "classic.RAR";
	const std::vector<std::uint8_t> rar4Bytes = rar_test_fixtures::Rar4ImageArchive();
	Expect(rar4Bytes.size() > 100, "embedded RAR4 fixture did not decode from base64");
	WriteBytes(rar4, rar4Bytes);
	Expect(jpegview_linux::IsArchiveContainerName(rar4) &&
		jpegview_linux::IsArchiveContainerFile(rar4) &&
		jpegview_linux::ArchiveFormatName(rar4 / "testfile.jpg") == "RAR" &&
		jpegview_linux::ArchiveBackingFile(rar4 / "testfile.jpg") == rar4,
		"case-insensitive RAR recognition did not preserve the virtual-member contract");

	std::vector<jpegview_linux::ArchiveEntryInfo> entries;
	Expect(jpegview_linux::ListArchiveDirectory(rar4, entries, error),
		"cannot list RAR4 image archive: " + error);
	const fs::path rar4Jpeg = rar4 / "testfile.jpg";
	Expect(entries.size() == 2 &&
		std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
			return !entry.directory && entry.path == rar4Jpeg && entry.size > 0;
		}) &&
		std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
			return !entry.directory && entry.path == rar4 / "testfile.png" && entry.size > 0;
		}),
		"RAR4 listing did not expose its JPEG and PNG members with sizes");
	DecodedImage rar4Decoded;
	Expect(jpegview_linux::DecodeImage(rar4Jpeg, rar4Decoded, error) &&
		!rar4Decoded.frames.empty() && rar4Decoded.frames.front().width > 0 &&
		rar4Decoded.frames.front().height > 0,
		"image decoder could not read a RAR4 member: " + error);

	const fs::path rar5 = temporary.path() / "solid.rar";
	const std::vector<std::uint8_t> rar5Bytes = rar_test_fixtures::Rar5SolidImageArchive();
	Expect(rar5Bytes.size() > 100, "embedded solid RAR5 fixture did not decode from base64");
	WriteBytes(rar5, rar5Bytes);
	entries.clear();
	Expect(jpegview_linux::ListArchiveDirectory(rar5, entries, error),
		"cannot list solid RAR5 image archive: " + error);
	const fs::path rar5Png = rar5 / "testfile.png";
	Expect(entries.size() == 2 &&
		std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
			return !entry.directory && entry.path == rar5Png && entry.size > 0;
		}),
		"solid RAR5 listing did not expose the later PNG member");
	FileList files({rar5.string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 2 && files.IsArchiveMember(0) &&
		std::find(files.Files().begin(), files.Files().end(), rar5Png) != files.Files().end(),
		"opening a RAR5 container did not initialize its image-member list");
	DecodedImage rar5Decoded;
	Expect(jpegview_linux::DecodeImage(rar5Png, rar5Decoded, error) &&
		!rar5Decoded.frames.empty() && rar5Decoded.frames.front().width > 0 &&
		rar5Decoded.frames.front().height > 0,
		"image decoder could not read a later member from a solid RAR5 archive: " + error);

	jpegview_linux::FileDialogPreviewLoader previewLoader;
	const std::uint64_t previewGeneration = previewLoader.Request(rar5Png, false,
		jpegview_linux::FileDialogSortMode::Name, 64, 64);
	std::vector<jpegview_linux::FileDialogPreviewResult> previews;
	const auto previewDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (previews.empty() && std::chrono::steady_clock::now() < previewDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		previews = previewLoader.TakeReady();
	}
	Expect(previews.size() == 1 && previews.front().generation == previewGeneration &&
		previews.front().source == rar5Png && previews.front().error.empty() &&
		previews.front().width > 0 && previews.front().height > 0,
		"open-dialog preview could not decode a solid RAR5 image member");

	const fs::path malformed = temporary.path() / "broken.rar";
	WriteText(malformed, "not a RAR archive");
	entries.clear();
	Expect(!jpegview_linux::ListArchiveDirectory(malformed, entries, error) && !error.empty(),
		"malformed RAR input did not fail with a useful error");
}

void TestEncryptedRarBrowsingAndSessionPasswords() {
	struct PasswordCacheGuard {
		PasswordCacheGuard() { jpegview_linux::ClearSessionArchivePasswords(); }
		~PasswordCacheGuard() { jpegview_linux::ClearSessionArchivePasswords(); }
	} passwordCacheGuard;
	struct Fixture {
		const char* filename;
		bool headerEncrypted;
	};
	constexpr Fixture fixtures[] = {
		{"rar4-data-encrypted.rar", false},
		{"rar4-header-encrypted.rar", true},
		{"rar5-data-encrypted.rar", false},
		{"rar5-header-encrypted.rar", true},
	};
	TemporaryDirectory temporary;
	const fs::path fixtureDirectory = fs::path(__FILE__).parent_path() /
		"fixtures" / "encrypted_rar";
	const std::vector<std::uint8_t> expectedPng = ReadBytes(fixtureDirectory / "private.png");
	Expect(expectedPng.size() > 32 && expectedPng[0] == 0x89 &&
		expectedPng[1] == 'P' && expectedPng[2] == 'N' && expectedPng[3] == 'G',
		"encrypted RAR test image fixture is not a PNG");
	std::string error;
	jpegview_linux::ArchiveErrorKind errorKind = jpegview_linux::ArchiveErrorKind::None;
	if (!jpegview_linux::RarBackendAvailable()) {
		const fs::path archive = temporary.path() / fixtures[0].filename;
		Expect(fs::copy_file(fixtureDirectory / fixtures[0].filename, archive),
			"could not copy encrypted RAR fallback fixture");
		Expect(!jpegview_linux::ValidateArchivePassword(archive, "test-secret", error,
			&errorKind) && errorKind == jpegview_linux::ArchiveErrorKind::UnsupportedEncryption,
			"local unavailable RAR backend did not report encrypted RAR as unsupported");
		return;
	}

	const auto awaitPreview = [](jpegview_linux::FileDialogPreviewLoader& loader,
		const fs::path& archive) {
		const std::uint64_t generation = loader.Request(archive, true,
			jpegview_linux::FileDialogSortMode::Name, 32, 32);
		std::vector<jpegview_linux::FileDialogPreviewResult> results;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
		while (results.empty() && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			results = loader.TakeReady();
		}
		Expect(results.size() == 1 && results.front().generation == generation,
			"encrypted RAR preview did not finish before its bounded deadline");
		return results.front();
	};

	std::vector<fs::path> archivePaths;
	for (const Fixture& fixture : fixtures) {
		const fs::path archive = temporary.path() / fixture.filename;
		Expect(fs::copy_file(fixtureDirectory / fixture.filename, archive),
			std::string("could not copy encrypted RAR fixture ") + fixture.filename);
		archivePaths.push_back(archive);
		const fs::path member = archive / "private.png";
		std::vector<jpegview_linux::ArchiveEntryInfo> entries;
		bool containsEncryptedEntries = false;
		if (fixture.headerEncrypted) {
			Expect(!jpegview_linux::ListArchiveDirectoryCancellable(archive, entries,
				[] { return true; }, error, &errorKind, &containsEncryptedEntries) &&
				errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired && entries.empty(),
				std::string(fixture.filename) + " exposed hidden member names without a password");
		} else {
			Expect(jpegview_linux::ListArchiveDirectoryCancellable(archive, entries,
				[] { return true; }, error, &errorKind, &containsEncryptedEntries) &&
				containsEncryptedEntries && entries.size() == 1 &&
				entries.front().path == member && entries.front().encrypted,
				std::string(fixture.filename) +
				" did not keep encrypted-data member names visible: " + error);
		}

		jpegview_linux::FileDialogPreviewLoader previewLoader;
		const auto lockedPreview = awaitPreview(previewLoader, archive);
		Expect(lockedPreview.errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired &&
			lockedPreview.bgra.empty(),
			std::string(fixture.filename) + " preview did not stay locked without prompting (kind=" +
			std::to_string(static_cast<int>(lockedPreview.errorKind)) + ", error=" +
			lockedPreview.error + ", source=" + lockedPreview.source.string() + ", encrypted=" +
			(lockedPreview.encryptedArchive ? "true" : "false") + ")");

		const bool wrongPasswordAccepted = jpegview_linux::ValidateArchivePassword(archive,
			"wrong-password", error, &errorKind);
		Expect(!wrongPasswordAccepted &&
			errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword,
			std::string(fixture.filename) + " accepted or misclassified an incorrect password (accepted=" +
			(wrongPasswordAccepted ? "true" : "false") + ", kind=" +
			std::to_string(static_cast<int>(errorKind)) + ", error=" + error + ")");
		Expect(!jpegview_linux::HasSessionArchivePassword(archive),
			"failed encrypted RAR password validation cached a credential");
		Expect(jpegview_linux::ValidateArchivePassword(archive, "test-secret", error,
			&errorKind), std::string(fixture.filename) +
			" rejected its correct password: " + error);
		Expect(jpegview_linux::SetSessionArchivePassword(archive, "test-secret") &&
			jpegview_linux::HasSessionArchivePassword(member),
			std::string(fixture.filename) + " password was not reused by archive member paths");

		entries.clear();
		containsEncryptedEntries = false;
		Expect(jpegview_linux::ListArchiveDirectoryCancellable(archive, entries,
			[] { return true; }, error, &errorKind, &containsEncryptedEntries) &&
			containsEncryptedEntries && entries.size() == 1 &&
			entries.front().path == member && entries.front().encrypted,
			std::string(fixture.filename) + " did not list encrypted member metadata after unlock: " +
			error);

		bool extracted = false;
		Expect(jpegview_linux::WithArchiveMemberFile(member,
			[&](const fs::path& temporaryImage, std::string&) {
				extracted = ReadBytes(temporaryImage) == expectedPng;
				return extracted;
			}, error, &errorKind) && extracted,
			std::string(fixture.filename) + " did not extract the expected image bytes: " + error);
		jpegview_linux::DecodedImage decoded;
		Expect(jpegview_linux::DecodeImage(member, decoded, error) &&
			decoded.frames.size() == 1 && decoded.frames.front().width == 1 &&
			decoded.frames.front().height == 1,
			std::string(fixture.filename) + " did not decode its encrypted image: " + error);
		const auto unlockedPreview = awaitPreview(previewLoader, archive);
		Expect(unlockedPreview.error.empty() && !unlockedPreview.bgra.empty() &&
			unlockedPreview.width == 1 && unlockedPreview.height == 1,
			std::string(fixture.filename) + " did not reuse the cached password in its preview");
	}

	const fs::path cancelArchive = temporary.path() / "cancelled.rar";
	Expect(fs::copy_file(fixtureDirectory / fixtures[0].filename, cancelArchive),
		"could not copy cancellation fixture");
	std::vector<jpegview_linux::ArchiveEntryInfo> cancelledEntries;
	Expect(!jpegview_linux::ListArchiveDirectoryCancellable(cancelArchive, cancelledEntries,
		[] { return false; }, error, &errorKind) && error.find("cancel") != std::string::npos &&
		cancelledEntries.empty(), "encrypted RAR listing did not stop on cancellation");
	const jpegview_linux::RarDataWriter rejectingWriter = [](
		const std::uint8_t*, std::size_t, std::string& sinkError) {
		sinkError = "fixture sink rejected output";
		return false;
	};
	Expect(!jpegview_linux::ExtractRarMember(archivePaths.front(), 0, "private.png",
		std::optional<std::string>("test-secret"), [] { return true; }, rejectingWriter,
		error, &errorKind) && error.find("fixture sink rejected output") != std::string::npos,
		"encrypted RAR output callback failure was lost or treated as successful extraction");
	Expect(!jpegview_linux::ExtractRarMember(archivePaths.front(), 0, "private.png",
		std::optional<std::string>("test-secret"), [] { return false; },
		[](const std::uint8_t*, std::size_t, std::string&) { return true; },
		error, &errorKind) && error.find("cancel") != std::string::npos,
		"encrypted RAR extraction did not report cancellation safely");

	jpegview_linux::ClearSessionArchivePasswords();
	for (std::size_t index = 0; index < std::size(fixtures); ++index) {
		std::vector<jpegview_linux::ArchiveEntryInfo> entries;
		if (fixtures[index].headerEncrypted) {
			Expect(!jpegview_linux::ListArchiveDirectoryCancellable(archivePaths[index], entries,
				[] { return true; }, error, &errorKind) &&
				errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired && entries.empty(),
				"clearing RAR credentials retained a decrypted header catalog");
		} else {
			Expect(jpegview_linux::ListArchiveDirectory(archivePaths[index], entries, error) &&
				entries.size() == 1 && entries.front().encrypted,
				"clearing RAR credentials hid visible data-encrypted names");
			Expect(!jpegview_linux::WithArchiveMemberFile(archivePaths[index] / "private.png",
				[](const fs::path&, std::string&) { return true; }, error, &errorKind) &&
				errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired,
				"clearing RAR credentials did not relock an encrypted image");
		}
	}
}

void TestFileListMarkedImageToggle() {
	TemporaryDirectory temporary;
	const fs::path root = temporary.path() / "root";
	const fs::path first = root / "01-first.png";
	const fs::path second = root / "02-second.png";
	fs::create_directories(root);
	WriteTinyImage(first);
	WriteTinyImage(second);

	FileList files({root.string()}, FileList::SortMode::FileName, true, false);
	Expect(!files.HasMarkedFile() && !files.ToggleBetweenMarkedAndCurrent(),
		"toggle succeeded before a file was marked");
	Expect(files.MarkCurrentForToggle() && files.HasMarkedFile(),
		"current image could not be marked for toggling");
	Expect(files.MarkedIndex() && *files.MarkedIndex() == 0,
		"marked image index was not exposed in the active file list");
	Expect(files.Next() && files.Current() == second,
		"file-list setup did not navigate away from the marked image");
	Expect(files.ToggleBetweenMarkedAndCurrent() && files.Current() == first,
		"first toggle did not return to the marked image");
	Expect(files.ToggleBetweenMarkedAndCurrent() && files.Current() == second,
		"second toggle did not return to the image viewed before the first toggle");
	Expect(files.ToggleBetweenMarkedAndCurrent() && files.Current() == first,
		"repeated toggling did not alternate between the same image pair");

	Expect(files.Select(1) && files.MarkCurrentForToggle(),
		"marking a replacement image failed");
	Expect(files.MarkedIndex() && *files.MarkedIndex() == 1,
		"replacement mark did not update its active file-list index");
	Expect(files.Select(0) && files.ToggleBetweenMarkedAndCurrent() && files.Current() == second,
		"a new mark did not replace the previous marked image and reset the toggle pair");
	Expect(files.ToggleBetweenMarkedAndCurrent() && files.Current() == first,
		"replacement mark did not toggle back to the newly captured image");
	files.SetSorting(FileList::SortMode::FileName, false);
	Expect(files.MarkedIndex() && *files.MarkedIndex() == 0 && files.Current() == first,
		"marked-image index was not refreshed after file-list reordering");

	const fs::path nestedRoot = temporary.path() / "nested-root";
	const fs::path nestedImage = nestedRoot / "child" / "03-nested.png";
	fs::create_directories(nestedImage.parent_path());
	const fs::path topImage = nestedRoot / "01-top.png";
	WriteTinyImage(topImage);
	WriteTinyImage(nestedImage);
	FileList nestedNavigation({nestedRoot.string()}, FileList::SortMode::FileName, true, false);
	Expect(nestedNavigation.MarkCurrentForToggle(),
		"nested-navigation fixture could not mark its root image");
	Expect(nestedNavigation.MarkedIndex() && *nestedNavigation.MarkedIndex() == 0,
		"marked root image was not visible in the initial file list");
	nestedNavigation.SetNavigationMode(FileList::NavigationMode::LoopSubDirectories);
	Expect(nestedNavigation.Next() && nestedNavigation.Current() == nestedImage,
		"nested-navigation fixture did not enter its child directory");
	Expect(!nestedNavigation.MarkedIndex(),
		"marked image incorrectly appeared in a different active directory's list");
	Expect(nestedNavigation.ToggleBetweenMarkedAndCurrent() && nestedNavigation.Current() == topImage &&
		nestedNavigation.Size() == 1,
		"toggle did not load the marked image's directory when it was outside the active list");
	Expect(nestedNavigation.ToggleBetweenMarkedAndCurrent() && nestedNavigation.Current() == nestedImage &&
		nestedNavigation.Size() == 1,
		"toggle did not restore the captured image and its directory");
	const fs::path markedReturnPath = nestedNavigation.Current();
	const fs::path asynchronousMarkedTarget = nestedNavigation.MarkedToggleTarget();
	Expect(!asynchronousMarkedTarget.empty() && !nestedNavigation.ContainsPath(asynchronousMarkedTarget),
		"cross-directory marked toggle was not identified as requiring a prepared folder list");
	FileList::ScanRequest markedScanRequest = nestedNavigation.MakeScanRequest(
		FileList::ScanOperation::MarkedToggleTarget);
	markedScanRequest.selectedPath = asynchronousMarkedTarget;
	FileListPreparedScan markedScan = FileList::PrepareScan(markedScanRequest,
		[] { return true; });
	Expect(markedScan.completed && markedScan.targetFound &&
		markedScan.replacement.Current() == topImage,
		"the worker did not prepare the marked image's directory and selected path");
	Expect(nestedNavigation.ApplyPreparedScan(std::move(markedScan)) &&
		nestedNavigation.CompleteMarkedToggle(markedReturnPath) &&
		nestedNavigation.Current() == topImage,
		"an asynchronously prepared marked-image toggle did not preserve its return target");
	const fs::path markedToggleBackPath = nestedNavigation.Current();
	const fs::path asynchronousReturnTarget = nestedNavigation.MarkedToggleTarget();
	FileList::ScanRequest returnScanRequest = nestedNavigation.MakeScanRequest(
		FileList::ScanOperation::MarkedToggleTarget);
	returnScanRequest.selectedPath = asynchronousReturnTarget;
	FileListPreparedScan returnScan = FileList::PrepareScan(returnScanRequest,
		[] { return true; });
	Expect(returnScan.completed && returnScan.targetFound &&
		nestedNavigation.ApplyPreparedScan(std::move(returnScan)) &&
		nestedNavigation.CompleteMarkedToggle(markedToggleBackPath) &&
		nestedNavigation.Current() == nestedImage,
		"the asynchronously prepared marked-image toggle did not return to the prior folder");

	FileList missingMarked({root.string()}, FileList::SortMode::FileName, true, false);
	Expect(missingMarked.MarkCurrentForToggle() && missingMarked.Next(),
		"missing-mark fixture did not initialize");
	const fs::path stillCurrent = missingMarked.Current();
	std::error_code removeError;
	fs::remove(first, removeError);
	Expect(!removeError, "could not remove the marked-image fixture");
	Expect(!missingMarked.ToggleBetweenMarkedAndCurrent() && missingMarked.Current() == stillCurrent,
		"toggle to a removed marked image changed the current selection");
}

void TestSupportedImageExtensionPolicy() {
	const std::vector<std::string> supported = {
		"photo.JPG", "photo.apng", "photo.PAM", "camera.CR3", "camera.rwl"};
	for (const std::string& filename : supported) {
		Expect(jpegview_linux::IsSupportedImagePath(filename),
			"supported image extension was rejected: " + filename);
	}
	for (const char* filename : {"notes.txt", "photo", "image.jpeg.bak"}) {
		Expect(!jpegview_linux::IsSupportedImagePath(filename),
			std::string("unsupported image extension was accepted: ") + filename);
	}
}

void TestKeyboardCommandMappings() {
	struct KeyCase {
		Sint32 key;
		Uint16 modifiers;
		int command;
	};
	const std::vector<KeyCase> cases = {
		{SDLK_F1, 0, IDM_HELP},
		{SDLK_q, 0, IDM_EXIT},
		{SDLK_o, 0x00C0u, IDM_OPEN},
		{SDLK_F2, 0x00C0u, IDM_SHOW_FILENAME},
		{'c', 0x00C0u, IDM_COPY_FULL},
		{'c', 0x00C3u, IDM_COPY_PATH},
		{'v', 0x00C0u, IDM_PASTE},
		{'p', 0x00C0u, IDM_PRINT},
		{'s', 0x00C0u, IDM_SAVE_ALLOW_NO_PROMPT},
		{'s', 0x00C3u, IDM_SAVE_SCREEN},
		{SDLK_r, 0x00C0u, IDM_RELOAD},
		{SDLK_r, 0x00C3u, IDM_CHANGESIZE},
		{'m', 0x00C3u, IDM_TOUCH_IMAGE},
		{'m', 0x00C0u, IDM_MARK_FOR_TOGGLE},
		{'e', 0x00C3u, IDM_TOUCH_IMAGE_EXIF},
		{'e', 0x00C0u, jpegview_linux::kCommandToggleSelectionMode},
		{'n', 0x00C0u, IDM_SHOW_NAVPANEL},
		{'t', 0x00C0u, jpegview_linux::kCommandToggleThumbnailPanel},
		{'n', 0x0003u, IDM_SHOW_FILENAME},
		{SDLK_F2, 0, IDM_SHOW_FILEINFO},
		{SDLK_F3, 0, IDM_TOGGLE_RESAMPLING_QUALITY},
		{SDLK_F4, 0, IDM_KEEP_PARAMETERS},
		{SDLK_F5, 0, IDM_AUTO_CORRECTION},
		{SDLK_F6, 0, IDM_LDC},
		{'c', 0, IDM_SORT_CREATION_DATE},
		{'n', 0, IDM_SORT_NAME},
		{'m', 0, IDM_SORT_MOD_DATE},
		{'z', 0, jpegview_linux::kCommandToggleMagnifyingGlass},
		{'d', 0, jpegview_linux::kCommandToggleDoublePageMode},
		{'j', 0, jpegview_linux::kCommandToggleMangaReadingOrder},
		{SDLK_F7, 0, IDM_LOOP_FOLDER},
		{SDLK_F8, 0, IDM_LOOP_RECURSIVELY},
		{SDLK_F9, 0, IDM_LOOP_SIBLINGS},
		{SDLK_LEFT, 0x0100u, jpegview_linux::kCommandPreviousSiblingFolder},
		{SDLK_RIGHT, 0x0200u, jpegview_linux::kCommandNextSiblingFolder},
		{SDLK_LEFT, 0x00C0u, IDM_TOGGLE},
		{SDLK_RIGHT, 0x00C0u, IDM_TOGGLE},
		{SDLK_LEFT, 0x0140u, 0},
		{SDLK_RIGHT, 0x0101u, 0},
		{SDLK_DELETE, 0, IDM_MOVE_TO_RECYCLE_BIN_CONFIRM},
		{'w', 0, IDM_EXPLORE},
		{SDLK_RIGHT, 0, IDM_NEXT},
		{SDLK_PAGEDOWN, 0, IDM_NEXT},
		{SDLK_LEFT, 0, IDM_PREV},
		{SDLK_PAGEUP, 0, IDM_PREV},
		{SDLK_HOME, 0, IDM_FIRST},
		{SDLK_END, 0, IDM_LAST},
		{SDLK_SPACE, 0, IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS},
		{SDLK_SPACE, 0x0003u, 0},
		{SDLK_RETURN, 0, IDM_FIT_TO_SCREEN},
		{SDLK_DOWN, 0, IDM_ROTATE_90},
		{SDLK_UP, 0, IDM_ROTATE_270},
		{SDLK_UP, 0x0003u, IDM_PAN_UP},
		{SDLK_DOWN, 0x0003u, IDM_PAN_DOWN},
		{SDLK_RIGHT, 0x0003u, IDM_PAN_RIGHT},
		{SDLK_LEFT, 0x0003u, IDM_PAN_LEFT},
		{SDLK_DOWN, 0x00C0u, IDM_ZOOM_DEC},
		{SDLK_UP, 0x00C0u, IDM_ZOOM_INC},
		{SDLK_F11, 0, IDM_FULL_SCREEN_MODE},
		{SDLK_F11, 0x0003u, IDM_HIDE_TITLE_BAR},
		{SDLK_F11, 0x00C0u, IDM_FIT_WINDOW_TO_IMAGE},
		{SDLK_F12, 0, IDM_SPAN_SCREENS},
		{SDLK_F12, 0x0003u, IDM_ALWAYS_ON_TOP},
		{SDLK_RETURN, 0x00C0u, IDM_FILL_WITH_CROP},
		{'r', 0, IDM_ROTATE_90_LOSSLESS_CONFIRM},
		{'t', 0, IDM_ROTATE_270_LOSSLESS_CONFIRM},
		{SDLK_0, 0, IDM_FIT_TO_SCREEN},
		{'f', 0, IDM_FULL_SCREEN_MODE},
		{SDLK_EQUALS, 0, IDM_ZOOM_INC},
		{SDLK_KP_PLUS, 0x0003u, IDM_ZOOM_INC},
		{SDLK_MINUS, 0, IDM_ZOOM_DEC},
		{SDLK_KP_MINUS, 0x0003u, IDM_ZOOM_DEC},
	};
	for (const KeyCase& testCase : cases) {
		SDL_KeyboardEvent event{};
		event.keysym.sym = testCase.key;
		event.keysym.mod = testCase.modifiers;
		Expect(jpegview_linux::CommandForKey(event, false) == testCase.command,
			"keyboard command mapping is incorrect");
	}
	SDL_KeyboardEvent space{};
	space.keysym.sym = SDLK_SPACE;
	Expect(jpegview_linux::SpacebarNavigationDirection(space, false) == 0 &&
		jpegview_linux::CommandForKey(space, false) == IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS,
		"disabled Space navigation did not preserve the existing scale shortcut");
	Expect(jpegview_linux::SpacebarNavigationDirection(space, true) == 1,
		"enabled Space navigation did not select the next image");
	space.keysym.mod = 0x0003u;
	Expect(jpegview_linux::SpacebarNavigationDirection(space, true) == -1,
		"Shift+Space did not select the previous image");
	space.keysym.mod = 0x0040u;
	Expect(jpegview_linux::SpacebarNavigationDirection(space, true) == 0,
		"Ctrl+Space unexpectedly entered image navigation");
	space.keysym.mod = 0x0100u;
	Expect(jpegview_linux::SpacebarNavigationDirection(space, true) == 0,
		"Alt+Space unexpectedly entered image navigation");
	space.keysym.mod = 0;
	space.keysym.sym = SDLK_RETURN;
	Expect(jpegview_linux::SpacebarNavigationDirection(space, true) == 0,
		"non-Space key unexpectedly entered Space navigation");
	SDL_KeyboardEvent escape{};
	escape.keysym.sym = SDLK_ESCAPE;
	Expect(jpegview_linux::CommandForKey(escape, false) == IDM_EXIT,
		"Escape did not exit when playback was inactive");
	Expect(jpegview_linux::CommandForKey(escape, true) == IDM_DEFAULT_ESC,
		"Escape did not stop playback before exiting");
	SDL_KeyboardEvent resume{};
	resume.keysym.sym = SDLK_r;
	resume.keysym.mod = 0x0300u;
	Expect(jpegview_linux::CommandForKey(resume, false) == IDM_SLIDESHOW_RESUME,
		"Alt+R did not resume playback");
	resume.keysym.sym = 'x';
	Expect(jpegview_linux::CommandForKey(resume, false) == 0, "unsupported Alt command was accepted");
	SDL_KeyboardEvent unknown{};
	unknown.keysym.sym = 'x';
	Expect(jpegview_linux::CommandForKey(unknown, false) == 0, "unknown key was accepted");
}

void TestDoublePagePairingNavigationAndReadingOrder() {
	using jpegview_linux::DoublePageModeState;
	using jpegview_linux::PageDimensions;
	const PageDimensions cover{600, 900};
	const PageDimensions pageOne{600, 900};
	const PageDimensions pageTwo{600, 1200};
	const PageDimensions landscape{1200, 800};
	DoublePageModeState modes{true, false};

	Expect(!jpegview_linux::BuildDoublePageSpread(0, 4, cover, pageOne, modes).has_value(),
		"double-page mode paired the cover with the first interior page");
	const auto spread = jpegview_linux::BuildDoublePageSpread(1, 4, pageOne, pageTwo, modes);
	Expect(spread.has_value() && spread->firstIndex == 1 && spread->secondIndex == 2 &&
		spread->canvasHeight == 1200 && spread->canvasWidth == 1400 &&
		spread->currentPage.x == 0 && spread->currentPage.width == 800 &&
		spread->nextPage.x == 800 && spread->nextPage.width == 600,
		"portrait pages did not form a common-height, aspect-preserving spread");
	const auto clockwiseSpread = jpegview_linux::BuildDoublePageSpread(
		1, 4, pageOne, pageTwo, modes, true, 1);
	Expect(clockwiseSpread.has_value() && clockwiseSpread->canvasWidth == 1200 &&
		clockwiseSpread->canvasHeight == 1400 &&
		clockwiseSpread->currentPage.x == 0 && clockwiseSpread->currentPage.y == 0 &&
		clockwiseSpread->currentPage.width == 1200 &&
		clockwiseSpread->currentPage.height == 800 &&
		clockwiseSpread->nextPage.x == 0 && clockwiseSpread->nextPage.y == 800 &&
		clockwiseSpread->nextPage.width == 1200 && clockwiseSpread->nextPage.height == 600,
		"clockwise rotation did not turn the full spread into a correctly stacked book");
	const auto counterClockwiseSpread = jpegview_linux::BuildDoublePageSpread(
		1, 4, pageOne, pageTwo, modes, true, 3);
	Expect(counterClockwiseSpread.has_value() && counterClockwiseSpread->canvasWidth == 1200 &&
		counterClockwiseSpread->canvasHeight == 1400 &&
		counterClockwiseSpread->currentPage.y == 600 &&
		counterClockwiseSpread->nextPage.y == 0,
		"counterclockwise rotation did not retain both pages in the rotated spread");
	const auto halfTurnSpread = jpegview_linux::BuildDoublePageSpread(
		1, 4, pageOne, pageTwo, modes, true, 2);
	Expect(halfTurnSpread.has_value() && halfTurnSpread->canvasWidth == 1400 &&
		halfTurnSpread->canvasHeight == 1200 && halfTurnSpread->currentPage.x == 600 &&
		halfTurnSpread->nextPage.x == 0,
		"half-turn rotation did not turn both pages together");
	modes.mangaReadingOrder = true;
	const auto mangaSpread = jpegview_linux::BuildDoublePageSpread(1, 4, pageOne, pageTwo, modes);
	Expect(mangaSpread.has_value() && mangaSpread->currentPage.x == 600 &&
		mangaSpread->nextPage.x == 0,
		"manga reading order did not swap the spread pages");
	Expect(!jpegview_linux::BuildDoublePageSpread(1, 4, landscape, pageTwo, modes).has_value() &&
		!jpegview_linux::BuildDoublePageSpread(1, 4, pageOne, landscape, modes).has_value(),
		"double-page mode paired a non-portrait page");
	Expect(!jpegview_linux::CanAnchorDoublePageSpread(1, 4, landscape) &&
		jpegview_linux::CanAnchorDoublePageSpread(1, 4, pageOne),
		"double-page anchor eligibility did not resolve known landscape pages before partner lookup");
	jpegview_linux::DoublePagePresentationModel landscapePresentation;
	landscapePresentation.AwaitDimensions(1, 2);
	if (!jpegview_linux::CanAnchorDoublePageSpread(1, 4, landscape)) {
		landscapePresentation.UseSinglePage(1);
	}
	Expect(landscapePresentation.Phase() ==
			jpegview_linux::DoublePagePresentationPhase::SinglePage &&
		!landscapePresentation.SuppressSinglePage(1),
		"cold partner dimensions suppressed a known landscape anchor instead of presenting it alone");
	Expect(!jpegview_linux::BuildDoublePageSpread(1, 4, pageOne, std::nullopt, modes).has_value(),
		"double-page mode paired before neighbor dimensions were available");

	const std::optional<PageDimensions> current = pageOne;
	const std::optional<PageDimensions> next = pageTwo;
	const std::optional<PageDimensions> previousStart = pageOne;
	const std::optional<PageDimensions> previousPartner = pageTwo;
	Expect(jpegview_linux::DoublePageNavigationStep(1, 1, 4, true,
		current, next, std::nullopt, std::nullopt) == 2,
		"forward navigation did not skip the visible partner page");
	Expect(jpegview_linux::DoublePageNavigationStep(-1, 3, 5, true,
		std::nullopt, std::nullopt, previousStart, previousPartner) == 2,
		"backward navigation did not skip a preceding spread");
	Expect(jpegview_linux::DoublePageNavigationStep(-1, 2, 4, true,
		std::nullopt, std::nullopt, cover, pageOne) == 1 &&
		jpegview_linux::DoublePageNavigationStep(1, 1, 4, false,
			current, next, std::nullopt, std::nullopt) == 1,
		"navigation skipped pages when a cover or single-page mode requires a one-page step");
	Expect(jpegview_linux::LogicalDirectionForPhysicalKey(-1, false, true) == -1 &&
		jpegview_linux::LogicalDirectionForPhysicalKey(-1, true, true) == 1 &&
		jpegview_linux::LogicalDirectionForPhysicalKey(1, true, true) == -1 &&
		jpegview_linux::LogicalDirectionForPhysicalKey(-1, true, false) == -1 &&
		jpegview_linux::LogicalDirectionForPhysicalKey(1, true, false) == 1 &&
		jpegview_linux::LogicalDirectionForPhysicalKey(0, true, false) == 0,
		"physical left/right navigation did not honor the manga inversion preference");
}

void TestDoublePagePresentationCommitsSpreadAtomically() {
	using jpegview_linux::DoublePagePresentationModel;
	using jpegview_linux::DoublePagePresentationPhase;
	DoublePagePresentationModel presentation;
	presentation.AwaitDimensions(3, 4);
	const std::uint64_t dimensionGeneration = presentation.Generation();
	Expect(presentation.Phase() == DoublePagePresentationPhase::AwaitingDimensions &&
		presentation.SuppressSinglePage(3) && !presentation.SpreadReady(3),
		"unresolved partner dimensions allowed the anchor to appear alone");

	Expect(presentation.BeginSpread(3, 4, "anchor-final-size", "partner-final-size"),
		"spread preparation did not start after partner dimensions resolved");
	const std::uint64_t spreadGeneration = presentation.Generation();
	Expect(spreadGeneration > dimensionGeneration && presentation.SuppressSinglePage(3),
		"new spread generation did not keep the single-page fallback hidden");
	Expect(!presentation.MarkTextureReady("stale-anchor") &&
		presentation.MarkTextureReady("anchor-final-size") &&
		!presentation.SpreadReady(3) && presentation.SuppressSinglePage(3),
		"a single ready page incorrectly committed the spread");
	Expect(!presentation.BeginSpread(3, 4, "anchor-final-size", "partner-final-size") &&
		presentation.MarkTextureReady("partner-final-size") &&
		presentation.SpreadReady(3) && !presentation.SpreadPresented(3) &&
		presentation.MarkSpreadPresented(3) && presentation.SpreadPresented(3) &&
		!presentation.SuppressSinglePage(3),
		"refreshing a pending spread reset readiness or published before both pages were ready");

	presentation.AwaitDimensions(5, 6);
	Expect(presentation.Generation() > spreadGeneration &&
		!presentation.MarkTextureReady("anchor-final-size") &&
		presentation.SuppressSinglePage(5) && !presentation.SpreadPresented(5),
		"late completion from an old spread changed the new navigation generation");
	presentation.UseSinglePage(5);
	Expect(!presentation.SuppressSinglePage(5) && !presentation.SpreadReady(5),
		"resolved non-pair remained hidden as a pending spread");

	presentation.BeginSpread(5, 6, "failed-anchor", "failed-partner");
	Expect(presentation.MarkTextureFailed("failed-partner") &&
		presentation.Phase() == DoublePagePresentationPhase::FailedSpread &&
		!presentation.SuppressSinglePage(5),
		"failed spread texture did not release the single-page fallback");
}

void TestPresentationControllerGatesHeldNavigationOnFirstFrame() {
	using jpegview_linux::PresentationController;
	using jpegview_linux::PresentationNavigationState;
	PresentationController presentation;
	Expect(presentation.BeginSpread(3, 4, "anchor", "partner"),
		"presentation controller did not begin the selected spread");
	PresentationNavigationState state{3, true, false, true, false, false};
	Expect(!presentation.CanRepeatNavigation(state) &&
		presentation.MarkTextureReady("anchor") &&
		!presentation.CanRepeatNavigation(state),
		"held navigation advanced before both pages had renderer textures");
	Expect(presentation.MarkTextureReady("partner") &&
		!presentation.CanRepeatNavigation(state),
		"held navigation advanced before the first complete spread frame was presented");
	Expect(presentation.AcknowledgeFramePresented(3, true) &&
		presentation.CanRepeatNavigation({3, true, false, true, true, true}),
		"held navigation did not resume after the first complete spread frame was presented");

	Expect(presentation.BeginSpread(5, 6, "failed-anchor", "failed-partner") &&
		presentation.MarkTextureFailed("failed-partner") &&
		presentation.CanRepeatNavigation({5, true, false, false, false, false}),
		"a failed spread did not return navigation to its single-page fallback");
	Expect(!presentation.CanRepeatNavigation({5, false, false, false, false, false}) &&
		!presentation.CanRepeatNavigation({5, true, true, false, false, false}),
		"pending selection or JPEG header completion allowed repeat navigation");
}

void TestPresentationControllerPlansSpreadPreparationWithoutRenderer() {
	using jpegview_linux::PresentationController;
	using jpegview_linux::SpreadPreparationAction;
	using jpegview_linux::SpreadPreparationSnapshot;
	PresentationController presentation;
	SpreadPreparationSnapshot snapshot;
	Expect(presentation.PlanSpreadPreparation(snapshot).action ==
		SpreadPreparationAction::UseSinglePage,
		"empty image list did not choose the single-page presentation");

	snapshot.selectedIndex = 1;
	snapshot.pageCount = 4;
	snapshot.modes.enabled = true;
	snapshot.cacheAvailable = true;
	Expect(presentation.PlanSpreadPreparation(snapshot).action ==
		SpreadPreparationAction::AwaitDimensions,
		"pending anchor selection did not wait for its spread dimensions");
	snapshot.cacheAvailable = false;
	Expect(presentation.PlanSpreadPreparation(snapshot).action ==
		SpreadPreparationAction::UseSinglePage,
		"disabled cache admitted pending spread work");

	snapshot.selectedImageCommitted = true;
	snapshot.cacheAvailable = true;
	snapshot.anchorPixelsAvailable = true;
	Expect(presentation.PlanSpreadPreparation(snapshot).action ==
		SpreadPreparationAction::CapturePageDimensions,
		"committed image did not request its page-dimension snapshot");
	snapshot.pageDimensionsCaptured = true;
	snapshot.anchorDimensions = jpegview_linux::PageDimensions{600, 900};
	Expect(presentation.PlanSpreadPreparation(snapshot).action ==
		SpreadPreparationAction::AwaitDimensions,
		"unknown partner dimensions did not keep spread preparation pending");
	snapshot.partnerDecodeFailed = true;
	const auto failedPartner = presentation.PlanSpreadPreparation(snapshot);
	Expect(failedPartner.action == SpreadPreparationAction::UseSinglePageAfterPartnerFailure &&
		failedPartner.preserveDeferredDisplay,
		"failed partner decode did not preserve the anchor's deferred display fallback");

	snapshot.partnerDecodeFailed = false;
	snapshot.partnerDimensions = jpegview_linux::PageDimensions{720, 1080};
	snapshot.anchorRotationQuarterTurns = 1;
	const auto rotated = presentation.PlanSpreadPreparation(snapshot);
	Expect(rotated.action == SpreadPreparationAction::PrepareSpread &&
		rotated.layout.has_value() && rotated.layout->firstIndex == 1 &&
		rotated.layout->secondIndex == 2 &&
		rotated.layout->clockwiseQuarterTurns == 1 &&
		rotated.layout->canvasWidth == 1080 && rotated.layout->canvasHeight == 1440,
		"spread planning lost rotated geometry or adjacent source identity");

	snapshot.anchorRotationQuarterTurns = 0;
	snapshot.partnerDimensions = jpegview_linux::PageDimensions{1080, 720};
	const auto landscapePartner = presentation.PlanSpreadPreparation(snapshot);
	Expect(landscapePartner.action == SpreadPreparationAction::UseSinglePageAfterPartnerFailure &&
		landscapePartner.preserveDeferredDisplay,
		"ineligible partner geometry did not select the deferred single-page fallback");
	snapshot.anchorDimensions = jpegview_linux::PageDimensions{900, 600};
	Expect(presentation.PlanSpreadPreparation(snapshot).action ==
		SpreadPreparationAction::UseSinglePage,
		"landscape anchor incorrectly entered spread request planning");
}

void TestPresentationControllerPlansSpreadRequestsAndFallbacks() {
	using jpegview_linux::PresentationController;
	using jpegview_linux::SpreadRequestAction;
	using jpegview_linux::SpreadRequestSnapshot;
	PresentationController presentation;
	const auto missingPartner = presentation.PlanSpreadRequests({false, 100, 100, 1000, false});
	Expect(missingPartner.action == SpreadRequestAction::ReturnToSinglePage &&
		missingPartner.preserveDeferredDisplay,
		"unavailable partner request did not retain the anchor fallback");
	const auto overBudget = presentation.PlanSpreadRequests({true, 600, 500, 1000, false});
	Expect(overBudget.action == SpreadRequestAction::ReturnToSinglePage &&
		!overBudget.preserveDeferredDisplay,
		"over-budget spread requests did not return to ordinary single-page preparation");
	SpreadRequestSnapshot failedPair{true, 400, 500, 1000, true};
	const auto deferredFailure = presentation.PlanSpreadRequests(failedPair);
	Expect(deferredFailure.action == SpreadRequestAction::KeepFailedSpreadDeferred &&
		deferredFailure.preserveDeferredDisplay,
		"previously failed spread did not keep its deferred fallback while rejecting retries");
	failedPair.pairPreviouslyFailed = false;
	const auto admittedPair = presentation.PlanSpreadRequests(failedPair);
	Expect(admittedPair.action == SpreadRequestAction::StartSpreadRequests &&
		!admittedPair.preserveDeferredDisplay,
		"eligible spread pair did not start its two texture requests");
	const std::size_t maximum = std::numeric_limits<std::size_t>::max();
	const auto overflowPair = presentation.PlanSpreadRequests({true, maximum, 1, maximum, false});
	Expect(overflowPair.action == SpreadRequestAction::ReturnToSinglePage,
		"overflowed spread texture accounting was admitted");
}

void TestDoublePageRefreshInvalidatesVisiblePartnerGeometry() {
	const jpegview_linux::PageDimensions anchor{600, 900};
	const jpegview_linux::PageDimensions oldPartner{600, 1200};
	const jpegview_linux::PageDimensions replacementPartner{900, 1800};
	const auto oldSpread = jpegview_linux::BuildDoublePageSpread(1, 4,
		anchor, oldPartner, {true, false});
	const auto replacementSpread = jpegview_linux::BuildDoublePageSpread(1, 4,
		anchor, replacementPartner, {true, false});
	Expect(oldSpread.has_value() && replacementSpread.has_value() &&
		oldSpread->secondIndex == replacementSpread->secondIndex &&
		oldSpread->canvasWidth != replacementSpread->canvasWidth &&
		oldSpread->canvasHeight != replacementSpread->canvasHeight &&
		jpegview_linux::DoublePageSpreadAffectedBySourceRefresh(*oldSpread, 2, false) &&
		!jpegview_linux::DoublePageSpreadAffectedBySourceRefresh(*oldSpread, 3, false) &&
		jpegview_linux::DoublePageSpreadAffectedBySourceRefresh(*oldSpread, 3, true),
		"visible partner identity or list reorder did not invalidate its captured spread geometry");
}

void TestFullListReplacementRebuildsSpreadFromNewNeighbor() {
	TemporaryDirectory temporary;
	const fs::path coverPath = temporary.path() / "00-cover.png";
	const fs::path anchorPath = temporary.path() / "10-anchor.png";
	const fs::path partnerPath = temporary.path() / "20-partner.png";
	const fs::path reorderedPath = temporary.path() / "30-reordered.png";
	const auto writeSizedImage = [](const fs::path& path, int width, int height) {
		std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 255);
		ImageWriteOptions options;
		std::string error;
		Expect(jpegview_linux::WriteImage(path, pixels.data(), width, height,
			options, error), "could not create spread rescan fixture: " + error);
	};
	const auto readDimensions = [](const fs::path& path) {
		DecodedImage decoded;
		std::string error;
		Expect(jpegview_linux::DecodeImage(path, decoded, error) &&
			!decoded.frames.empty(), "could not read spread rescan fixture: " + error);
		return jpegview_linux::PageDimensions{
			decoded.frames.front().width, decoded.frames.front().height};
	};
	writeSizedImage(coverPath, 4, 3);
	writeSizedImage(anchorPath, 2, 4);
	writeSizedImage(partnerPath, 3, 6);
	writeSizedImage(reorderedPath, 4, 8);
	constexpr std::int64_t timestamp = 1700000400;
	SetModificationTimeNanoseconds(coverPath, timestamp, 100000000);
	SetModificationTimeNanoseconds(anchorPath, timestamp, 200000000);
	SetModificationTimeNanoseconds(partnerPath, timestamp, 300000000);
	SetModificationTimeNanoseconds(reorderedPath, timestamp, 400000000);
	FileList files({temporary.path().string()},
		FileList::SortMode::LastModificationTime, true, false);
	const auto originalAnchorIndex = files.IndexOf(anchorPath);
	const auto originalPartnerIndex = files.IndexOf(partnerPath);
	Expect(originalAnchorIndex == std::optional<std::size_t>(1) &&
		originalPartnerIndex == std::optional<std::size_t>(2) &&
		files.Select(*originalAnchorIndex),
		"spread rescan fixture did not establish its selected anchor and visible partner");
	const jpegview_linux::SourceKey anchorKey = files.DescriptorAt(*originalAnchorIndex)->Key();
	const jpegview_linux::SourceKey oldPartnerKey =
		files.DescriptorAt(*originalPartnerIndex)->Key();
	const jpegview_linux::PageDimensions anchorDimensions = readDimensions(anchorPath);
	const jpegview_linux::PageDimensions oldPartnerDimensions = readDimensions(partnerPath);
	constexpr int rotationQuarterTurns = 1;
	const auto oldSpread = jpegview_linux::BuildDoublePageSpread(*originalAnchorIndex,
		files.Size(), anchorDimensions, oldPartnerDimensions, {true, false}, true,
		rotationQuarterTurns);
	Expect(oldSpread.has_value(), "spread rescan fixture did not form its original spread");
	jpegview_linux::DoublePagePresentationModel presentation;
	Expect(presentation.BeginSpread(*originalAnchorIndex, *originalAnchorIndex + 1,
		"original-anchor", "original-partner") &&
		presentation.MarkTextureReady("original-anchor") &&
		presentation.MarkTextureReady("original-partner") &&
		presentation.MarkSpreadPresented(*originalAnchorIndex),
		"spread rescan fixture did not commit its original presentation generation");
	const std::uint64_t oldGeneration = presentation.Generation();
	jpegview_linux::Viewport viewport;
	viewport.ActualSize();
	viewport.ZoomAt(1.25, 40, 30, oldSpread->canvasWidth,
		oldSpread->canvasHeight, 100, 80);
	viewport.Pan(7.0, -3.0);
	const jpegview_linux::ViewportSnapshot viewportBefore = viewport.Snapshot();
	const double offsetXBefore = viewport.OffsetX();
	const double offsetYBefore = viewport.OffsetY();

	writeSizedImage(partnerPath, 5, 10);
	SetModificationTimeNanoseconds(partnerPath, timestamp, 500000000);
	const fs::path selectedPath = files.Current();
	const FileList::ScanRequest scanRequest =
		files.MakeScanRequest(FileList::ScanOperation::Reload);
	jpegview_linux::FileListPreparedScan scan = jpegview_linux::FileList::PrepareScan(
		scanRequest, [] { return true; });
	Expect(scan.completed && files.ApplyPreparedScan(std::move(scan), selectedPath),
		"accepted full-list scan did not publish its replacement descriptors");
	const auto refreshedAnchorIndex = files.IndexOf(anchorPath);
	const auto refreshedPartnerIndex = files.IndexOf(partnerPath);
	const auto newNeighborIndex = files.IndexOf(reorderedPath);
	Expect(refreshedAnchorIndex.has_value() && refreshedPartnerIndex.has_value() &&
		newNeighborIndex == std::optional<std::size_t>(*refreshedAnchorIndex + 1) &&
		refreshedPartnerIndex != originalPartnerIndex && files.Current() == selectedPath &&
		files.DescriptorAt(*refreshedAnchorIndex)->Key() == anchorKey &&
		files.DescriptorAt(*refreshedPartnerIndex)->Key() !=
			oldPartnerKey,
		"full-list replacement did not reorder the replaced partner while retaining the selected anchor identity");

	presentation.InvalidateForFileListReplacement(*refreshedAnchorIndex);
	const jpegview_linux::PageDimensions newNeighborDimensions = readDimensions(reorderedPath);
	const auto rebuiltSpread = jpegview_linux::BuildDoublePageSpread(*refreshedAnchorIndex,
		files.Size(), anchorDimensions, newNeighborDimensions, {true, false}, true,
		rotationQuarterTurns);
	Expect(presentation.Generation() > oldGeneration &&
		presentation.Phase() == jpegview_linux::DoublePagePresentationPhase::SinglePage &&
		!presentation.SpreadReady(*refreshedAnchorIndex) &&
		!presentation.MarkTextureReady("original-partner") && rebuiltSpread.has_value() &&
		rebuiltSpread->secondIndex == *newNeighborIndex &&
		rebuiltSpread->nextPage.width != oldSpread->nextPage.width &&
		rebuiltSpread->canvasHeight != oldSpread->canvasHeight &&
		rebuiltSpread->clockwiseQuarterTurns == rotationQuarterTurns &&
		viewport.Snapshot().zoom == viewportBefore.zoom &&
		viewport.OffsetX() == offsetXBefore && viewport.OffsetY() == offsetYBefore,
		"whole-list replacement retained stale spread geometry or changed the anchor viewport/rotation state");
}

void TestHeldNavigationCoalescesKeyRepeats() {
	jpegview_linux::HeldNavigationController navigation;
	Expect(navigation.KeyDown(1, 79, false) == 1 && navigation.Scancode() == 79,
		"physical right-arrow press did not request one immediate navigation step");
	Expect(navigation.AfterImageShown(true, true) == 0,
		"held navigation skipped an image before the initial repeat threshold");
	Expect(navigation.KeyDown(-1, 80, true) == 0 && navigation.Scancode() == 79,
		"stale key-repeat changed the active navigation direction");
	Expect(navigation.AfterImageShown(true, true) == 0,
		"stale key-repeat enabled continuous navigation");
	for (int repeat = 0; repeat < 100; ++repeat) {
		Expect(navigation.KeyDown(1, 79, true) == 0 && navigation.Scancode() == 79,
			"OS key-repeat queued another navigation step");
	}
	Expect(navigation.AfterImageShown(true, true) == 1 && navigation.AfterImageShown(true, true) == 1,
		"held right-arrow did not request one step after each displayed image");
	Expect(navigation.AfterImageShown(false, true) == 0 && navigation.Scancode() == -1,
		"released right-arrow continued navigating after the displayed image");
	Expect(navigation.KeyDown(-1, 44, false, true) == -1 &&
		navigation.ShiftModifierAllowed(),
		"Shift+Space did not retain its allowed modifier while held");
	Expect(navigation.KeyDown(-1, 44, true, false) == 0 &&
		navigation.AfterImageShown(true, true) == 0,
		"stale unshifted Space repeat activated held Shift+Space navigation");
	Expect(navigation.KeyDown(-1, 44, true, true) == 0 &&
		navigation.AfterImageShown(true, true) == -1,
		"held Shift+Space did not repeat one previous-image step");
	Expect(navigation.AfterImageShown(false, true) == 0 && !navigation.ShiftModifierAllowed(),
		"released Shift+Space kept its modifier allowance");
	Expect(navigation.KeyDown(-1, 80, false) == -1 && navigation.AfterImageShown(true, true) == 0,
		"new left-arrow press bypassed the initial repeat threshold");
	Expect(navigation.KeyDown(-1, 80, true) == 0 && navigation.AfterImageShown(true, true) == -1,
		"held left-arrow did not continue after its repeat threshold");
	navigation.Reset();
	Expect(navigation.AfterImageShown(true, true) == 0 && navigation.Scancode() == -1,
		"reset navigation state retained a pending repeat");
}

void TestHeldNavigationWaitsForCurrentImageContinuation() {
	jpegview_linux::HeldNavigationController navigation;
	Expect(navigation.KeyDown(1, 79, false) == 1,
		"initial Right press waited for the current image continuation");
	Expect(navigation.KeyDown(1, 79, true) == 0,
		"OS key-repeat queued another image change while its continuation was pending");
	Expect(navigation.AfterImageShown(true, false) == 0,
		"held Right advanced while the current JPEG header continuation was pending");
	Expect(navigation.AfterImageShown(true, true) == 1,
		"held Right did not resume after the current image was ready to present");
	navigation.KeyDown(1, 79, true);
	Expect(navigation.AfterImageShown(false, false) == 0 && navigation.Scancode() == -1,
		"releasing Right during a pending continuation retained held-navigation state");
	Expect(navigation.AfterImageShown(true, true) == 0,
		"released Right advanced after the current image continuation completed");
}
const TestCase kTests[] = {
	{"file-list-filtering-and-logical-sorting", &TestFileListFilteringAndLogicalSorting},
	{"ordinary-source-metadata-uses-one-statx-request", &TestOrdinarySourceMetadataUsesOneStatxRequest},
	{"source-descriptor-identity-and-unusual-paths", &TestSourceDescriptorIdentityAndUnusualPaths},
	{"provisional-source-descriptor-survives-startup-replacement", &TestProvisionalSourceDescriptorSurvivesStartupReplacement},
	{"current-jpeg-dimensions-stable-source-across-list-revisions", &TestCurrentJpegDimensionsAcceptStableSourceAcrossListRevisions},
	{"noncurrent-source-refresh-preserves-selection", &TestNonCurrentSourceRefreshPreservesSelection},
	{"current-processed-save-preserves-materialized-pixels", &TestCurrentProcessedSavePreservesMaterializedPixels},
	{"file-dialog-source-refresh-requires-exact-descriptor", &TestFileDialogSourceRefreshRequiresExactDescriptor},
	{"file-dialog-preview-refresh-policy", &TestFileDialogPreviewRefreshPolicy},
	{"archive-browsing-decoding-and-recent-preview", &TestArchiveBrowsingDecodingAndRecentPreview},
	{"archive-descriptor-identity-and-replacement", &TestArchiveDescriptorIdentityAndReplacement},
	{"encrypted-zip-browsing-and-session-passwords", &TestEncryptedZipBrowsingAndSessionPasswords},
	{"encrypted-7z-browsing-and-session-passwords", &TestEncryptedSevenZipBrowsingAndSessionPasswords},
	{"tar-and-tgz-browsing-decoding-and-safety", &TestTarAndTgzBrowsingDecodingAndSafety},
	{"7z-browsing-decoding-and-preview", &TestSevenZipBrowsingDecodingAndPreview},
	{"rar-browsing-decoding-and-preview", &TestRarBrowsingDecodingAndPreview},
	{"encrypted-rar-browsing-and-session-passwords", &TestEncryptedRarBrowsingAndSessionPasswords},
	{"file-list-marked-image-toggle", &TestFileListMarkedImageToggle},
	{"supported-image-extension-policy", &TestSupportedImageExtensionPolicy},
	{"keyboard-command-mappings", &TestKeyboardCommandMappings},
	{"double-page-pairing-navigation-and-reading-order", &TestDoublePagePairingNavigationAndReadingOrder},
	{"double-page-presentation-atomic-commit", &TestDoublePagePresentationCommitsSpreadAtomically},
	{"presentation-controller-first-frame-navigation-gate", &TestPresentationControllerGatesHeldNavigationOnFirstFrame},
	{"presentation-controller-plans-spread-preparation-without-renderer", &TestPresentationControllerPlansSpreadPreparationWithoutRenderer},
	{"presentation-controller-plans-spread-requests-and-fallbacks", &TestPresentationControllerPlansSpreadRequestsAndFallbacks},
	{"double-page-refresh-invalidates-visible-partner-geometry", &TestDoublePageRefreshInvalidatesVisiblePartnerGeometry},
	{"full-list-replacement-rebuilds-spread-from-new-neighbor", &TestFullListReplacementRebuildsSpreadFromNewNeighbor},
	{"held-navigation-repeat-coalescing", &TestHeldNavigationCoalescesKeyRepeats},
	{"held-navigation-waits-for-current-image-continuation", &TestHeldNavigationWaitsForCurrentImageContinuation},
	{"archive-catalog-failure-releases-loading-state", &TestArchiveCatalogFailureReleasesLoadingState},
	{"cold-archive-member-metadata-yield-and-replacement", &TestColdArchiveMemberMetadataYieldAndReplacement},
};

} // namespace

const TestSuite& GetSourceNavigationSuite() {
	static const TestSuite suite{"source_navigation", kTests, sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
