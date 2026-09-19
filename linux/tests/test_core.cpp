#include "exif_reader.h"
#include "file_list.h"
#include "image_decoder.h"
#include "image_writer.h"
#include "settings.h"
#include "sort_mode.h"
#include "desktop_applications.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using jpegview_linux::DecodedImage;
using jpegview_linux::FileList;
using jpegview_linux::ImageWriteOptions;

namespace {

class TestFailure : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

void Expect(bool condition, const std::string& message) {
	if (!condition) throw TestFailure(message);
}

void ExpectNear(double actual, double expected, double tolerance, const std::string& message) {
	if (std::abs(actual - expected) > tolerance) {
		std::ostringstream details;
		details << message << " (actual=" << actual << ", expected=" << expected << ')';
		throw TestFailure(details.str());
	}
}

class TemporaryDirectory {
public:
	TemporaryDirectory() {
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path_ = fs::temp_directory_path() /
			("jpegview-linux-tests-" + std::to_string(static_cast<long long>(::getpid())) +
				"-" + std::to_string(stamp));
		std::error_code error;
		fs::create_directories(path_, error);
		if (error) throw TestFailure("cannot create temporary test directory: " + error.message());
	}

	~TemporaryDirectory() {
		std::error_code error;
		fs::remove_all(path_, error);
	}

	const fs::path& path() const { return path_; }

private:
	fs::path path_;
};

void WriteBytes(const fs::path& filename, const std::vector<std::uint8_t>& bytes) {
	std::ofstream output(filename, std::ios::binary);
	if (!output) throw TestFailure("cannot create " + filename.string());
	output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!output) throw TestFailure("cannot write " + filename.string());
}

void WriteText(const fs::path& filename, const std::string& text) {
	std::ofstream output(filename);
	if (!output) throw TestFailure("cannot create " + filename.string());
	output << text;
	if (!output) throw TestFailure("cannot write " + filename.string());
}

std::vector<std::uint8_t> ReadBytes(const fs::path& filename) {
	std::ifstream input(filename, std::ios::binary);
	if (!input) throw TestFailure("cannot read " + filename.string());
	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	input.seekg(0, std::ios::beg);
	if (size < 0) throw TestFailure("cannot determine size of " + filename.string());
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	input.read(reinterpret_cast<char*>(bytes.data()), size);
	if (!input && size != 0) throw TestFailure("cannot read " + filename.string());
	return bytes;
}

std::vector<std::uint8_t> TestPixels() {
	// Top-to-bottom BGRA: opaque red, translucent green, transparent blue, white.
	return {
		0, 0, 255, 255,
		0, 255, 0, 128,
		255, 0, 0, 0,
		255, 255, 255, 255,
	};
}

void WriteTinyImage(const fs::path& filename) {
	const std::vector<std::uint8_t> pixel = {20, 40, 60, 255};
	ImageWriteOptions options;
	std::string error;
	Expect(jpegview_linux::WriteImage(filename, pixel.data(), 1, 1, options, error),
		"cannot create test image " + filename.string() + ": " + error);
}

std::vector<std::string> FileNames(const FileList& files) {
	std::vector<std::string> names;
	for (const fs::path& path : files.Files()) names.push_back(path.filename().string());
	return names;
}

void SetModificationTime(const fs::path& filename, int secondsAfterEpoch) {
	const auto base = fs::file_time_type::clock::now();
	std::error_code error;
	fs::last_write_time(filename, base + std::chrono::seconds(secondsAfterEpoch), error);
	if (error) throw TestFailure("cannot set test timestamp: " + error.message());
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
	Expect(files.Next(), "next image should advance");
	Expect(files.Current().filename() == "photo2.png", "next image selected the wrong file");
	Expect(files.Previous(), "previous image should advance backwards");
	Expect(files.Current().filename() == "photo1.png", "previous image selected the wrong file");
	Expect(!files.Previous(), "non-wrapping file list should stop before the first image");

	FileList descending({directory.string()}, FileList::SortMode::FileName, false, false);
	Expect(FileNames(descending) == std::vector<std::string>({"photo10.png", "photo2.png", "photo1.png"}),
		"descending logical filename ordering is incorrect");
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
	const fs::path siblingB = siblings / "b";
	fs::create_directories(siblingA);
	fs::create_directories(siblingB);
	WriteTinyImage(siblingA / "a.png");
	WriteTinyImage(siblingB / "b.png");
	FileList siblingList({siblingA.string()}, FileList::SortMode::FileName, true, false);
	siblingList.SetNavigationMode(FileList::NavigationMode::LoopSameDirectoryLevel);
	Expect(siblingList.Next(), "sibling navigation did not enter the next populated sibling");
	Expect(siblingList.Current().parent_path().filename() == "b", "sibling navigation entered the wrong folder");
	Expect(siblingList.Previous(), "sibling navigation did not restore the previous sibling");
	Expect(siblingList.Current().parent_path().filename() == "a", "sibling previous navigation restored the wrong folder");

	FileList wrapping({root.string()}, FileList::SortMode::FileName, true, true);
	wrapping.Last();
	Expect(wrapping.Next(), "wrapping navigation should advance from the last image");
	Expect(wrapping.Current().filename() == "root.png", "wrapping navigation selected the wrong image");
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
}

void ExpectDecoded(const fs::path& filename, const std::vector<std::uint8_t>& expected,
	bool exactRgb, bool exactAlpha) {
	DecodedImage decoded;
	std::string error;
	Expect(jpegview_linux::DecodeImage(filename, decoded, error),
		"cannot decode " + filename.extension().string() + ": " + error);
	Expect(decoded.frames.size() == 1, "static output did not decode to one frame");
	Expect(!decoded.animation, "static output was incorrectly marked animated");
	Expect(decoded.frames.front().width == 2 && decoded.frames.front().height == 2,
		"decoded dimensions are incorrect for " + filename.extension().string());
	const std::vector<std::uint8_t>& actual = decoded.frames.front().bgra;
	Expect(actual.size() == expected.size(), "decoded pixel buffer has the wrong size");
	for (std::size_t pixel = 0; pixel < expected.size() / 4; ++pixel) {
		if (exactRgb) {
			for (int channel = 0; channel < 3; ++channel) {
				Expect(actual[pixel * 4 + channel] == expected[pixel * 4 + channel],
					"decoded RGB channel differs for " + filename.extension().string());
			}
		}
		if (exactAlpha) {
			Expect(actual[pixel * 4 + 3] == expected[pixel * 4 + 3],
				"decoded alpha channel differs for " + filename.extension().string());
		}
	}
}

void TestImageWriterDecoderRoundTrips() {
	TemporaryDirectory temporary;
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	options.jpegQuality = 100;
	options.webpQuality = 100;

	struct FormatCase {
		const char* extension;
		bool exactRgb;
		bool exactAlpha;
	};
	const std::vector<FormatCase> formats = {
		{".png", true, true},
		{".bmp", true, false},
		{".tga", true, true},
		{".ppm", true, false},
		{".qoi", true, true},
		{".psd", true, false},
		{".jpg", false, false},
	};
	for (const FormatCase& format : formats) {
		const fs::path filename = temporary.path() / ("roundtrip" + std::string(format.extension));
		std::string error;
		Expect(jpegview_linux::WriteImage(filename, pixels.data(), 2, 2, options, error),
			"cannot write " + filename.extension().string() + ": " + error);
		ExpectDecoded(filename, pixels, format.exactRgb, format.exactAlpha);
	}

	const fs::path uppercase = temporary.path() / "uppercase.PNG";
	std::string error;
	Expect(jpegview_linux::WriteImage(uppercase, pixels.data(), 2, 2, options, error),
		"uppercase extension was not accepted by the writer: " + error);
	ExpectDecoded(uppercase, pixels, true, true);

	const std::vector<std::pair<const char*, bool>> optionalFormats = {
#if JPEGVIEW_HAVE_GIF
		{".gif", false},
#endif
#if JPEGVIEW_HAVE_TIFF
		{".tiff", true},
#endif
#if JPEGVIEW_HAVE_WEBP
		{".webp", false},
#endif
#if JPEGVIEW_HAVE_HEIF
		{".heic", false},
#endif
#if JPEGVIEW_HAVE_AVIF
		{".avif", false},
#endif
#if JPEGVIEW_HAVE_JXL
		{".jxl", false},
#endif
	};
	for (const auto& format : optionalFormats) {
		const fs::path filename = temporary.path() / ("optional" + std::string(format.first));
		error.clear();
		Expect(jpegview_linux::WriteImage(filename, pixels.data(), 2, 2, options, error),
			"cannot write optional " + filename.extension().string() + ": " + error);
		DecodedImage decoded;
		error.clear();
		Expect(jpegview_linux::DecodeImage(filename, decoded, error),
			"cannot decode optional " + filename.extension().string() + ": " + error);
		Expect(decoded.frames.size() == 1 && decoded.frames.front().width == 2 && decoded.frames.front().height == 2,
			"optional format returned incorrect decoded dimensions");
	}

	const fs::path unknown = temporary.path() / "image.unknown";
	error.clear();
	Expect(!jpegview_linux::WriteImage(unknown, pixels.data(), 2, 2, options, error),
		"unknown output extension was accepted");
	Expect(error.find("unsupported output format") != std::string::npos,
		"unknown output extension returned an unhelpful error");
	error.clear();
	Expect(!jpegview_linux::WriteImage(temporary.path() / "invalid.png", pixels.data(), 0, 2, options, error),
		"invalid dimensions were accepted by the writer");
	Expect(!error.empty(), "invalid dimensions did not produce an error message");
}

void TestDecoderFailures() {
	TemporaryDirectory temporary;
	const fs::path invalid = temporary.path() / "invalid.png";
	WriteBytes(invalid, {0x89, 0x50, 0x4e, 0x47, 0x00});
	DecodedImage decoded;
	decoded.animation = true;
	decoded.frames.resize(1);
	std::string error;
	Expect(!jpegview_linux::DecodeImage(invalid, decoded, error), "truncated PNG was accepted");
	Expect(decoded.frames.empty() && !decoded.animation, "decoder did not reset output on failure");
	Expect(!error.empty(), "truncated PNG did not produce an error message");

	error.clear();
	Expect(!jpegview_linux::DecodeImage(temporary.path() / "missing.jpg", decoded, error),
		"missing image was accepted");
	Expect(error == "cannot open file" || !error.empty(), "missing image did not produce an error message");
}

void TestSettingsRoundTripAndMalformedValues() {
	TemporaryDirectory temporary;
	const fs::path settingsPath = temporary.path() / "config" / "settings.conf";
	jpegview_linux::ViewerSettings expected;
	expected.scaleMode = "manual";
	expected.sortMode = "file_name";
	expected.sortAscending = false;
	expected.manualZoom = 2.375;
	expected.maximized = true;
	expected.navigationPanelEnabled = false;
	expected.navigationPanelAutoReveal = false;
	expected.infoVisible = true;
	expected.showFilename = true;
	expected.autoContrast = true;
	expected.copyRenamePattern = "%F=%n";
	Expect(jpegview_linux::SaveViewerSettings(settingsPath, expected), "settings could not be saved");
	Expect(fs::exists(settingsPath), "settings file was not created");
	Expect(!fs::exists(settingsPath.string() + ".tmp"), "temporary settings file was left behind");

	jpegview_linux::ViewerSettings loaded;
	Expect(jpegview_linux::LoadViewerSettings(settingsPath, loaded), "settings could not be loaded");
	Expect(loaded.scaleMode == expected.scaleMode && loaded.sortMode == expected.sortMode,
		"settings string values did not round-trip");
	Expect(loaded.sortAscending == expected.sortAscending && loaded.maximized == expected.maximized,
		"settings boolean values did not round-trip");
	Expect(loaded.navigationPanelEnabled == expected.navigationPanelEnabled &&
		loaded.navigationPanelAutoReveal == expected.navigationPanelAutoReveal,
		"navigation panel settings did not round-trip");
	Expect(loaded.infoVisible == expected.infoVisible && loaded.showFilename == expected.showFilename &&
		loaded.autoContrast == expected.autoContrast,
		"overlay/correction settings did not round-trip");
	Expect(loaded.copyRenamePattern == expected.copyRenamePattern, "batch pattern did not round-trip");
	Expect(loaded.manualZoomSet, "saved manual zoom was not marked present");
	ExpectNear(loaded.manualZoom, expected.manualZoom, 0.0000001, "manual zoom did not round-trip");

	const fs::path malformed = temporary.path() / "malformed.conf";
	std::ofstream malformedOutput(malformed);
	malformedOutput << "  scale_mode = manual\nmanual_zoom=not-a-number\nunknown_key=value\n";
	malformedOutput.close();
	loaded = {};
	Expect(jpegview_linux::LoadViewerSettings(malformed, loaded), "malformed settings file was rejected entirely");
	Expect(loaded.scaleMode == "manual", "whitespace around a setting was not trimmed");
	Expect(!loaded.manualZoomSet && loaded.manualZoom == 1.0,
		"malformed manual zoom did not retain its default");
}

void TestSortModeMappings() {
	struct SortCase {
		FileList::SortMode mode;
		const char* setting;
		const char* label;
		const char* description;
	};
	const std::vector<SortCase> cases = {
		{FileList::SortMode::LastModificationTime, "modification_date", "D", "modification date"},
		{FileList::SortMode::CreationTime, "creation_date", "C", "creation date"},
		{FileList::SortMode::FileName, "file_name", "N", "file name"},
		{FileList::SortMode::Random, "random", "R", "random"},
		{FileList::SortMode::FileSize, "file_size", "S", "file size"},
	};
	for (const SortCase& testCase : cases) {
		Expect(std::string(jpegview_linux::SortModeSettingName(testCase.mode)) == testCase.setting,
			"sort mode setting mapping is incorrect");
		Expect(std::string(jpegview_linux::SortModeShortLabel(testCase.mode)) == testCase.label,
			"sort mode short label mapping is incorrect");
		Expect(std::string(jpegview_linux::SortModeDescription(testCase.mode)) == testCase.description,
			"sort mode description mapping is incorrect");
		FileList::SortMode parsed = FileList::SortMode::FileSize;
		Expect(jpegview_linux::ParseSortMode(testCase.setting, parsed), "known sort mode was not parsed");
		Expect(parsed == testCase.mode, "sort mode parser returned the wrong mode");
	}
	FileList::SortMode unchanged = FileList::SortMode::FileName;
	Expect(!jpegview_linux::ParseSortMode("not-a-sort-mode", unchanged), "unknown sort mode was accepted");
	Expect(unchanged == FileList::SortMode::FileName, "unknown sort mode changed the output value");
}

void TestDesktopApplicationParsingAndExecExpansion() {
	TemporaryDirectory temporary;
	const fs::path desktopFile = temporary.path() / "viewer.desktop";
	WriteText(desktopFile,
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Test\\sViewer\n"
		"Exec=test-viewer --title \"hello world\" %f\n"
		"MimeType=image/jpeg;image/png;\n"
		"Terminal=true\n"
		"\n"
		"[Desktop Action Open]\n"
		"Name=Ignored\n");

	jpegview_linux::OpenWithApplication application;
	Expect(jpegview_linux::ReadDesktopApplication(desktopFile, "image/jpeg", application),
		"valid desktop entry was rejected");
	Expect(application.name == "Test Viewer" && application.terminal,
		"desktop entry name or terminal flag was parsed incorrectly");
	Expect(application.exec.find("hello world") != std::string::npos, "desktop Exec quoting was lost");

	Expect(!jpegview_linux::ReadDesktopApplication(desktopFile, "image/gif", application),
		"desktop entry with an incompatible MIME type was accepted");
	WriteText(temporary.path() / "hidden.desktop",
		"[Desktop Entry]\nType=Application\nName=Hidden\nExec=viewer %f\nMimeType=image/jpeg;\nHidden=true\n");
	Expect(!jpegview_linux::ReadDesktopApplication(temporary.path() / "hidden.desktop", "image/jpeg", application),
		"hidden desktop entry was accepted");

	Expect(jpegview_linux::UnescapeDesktopValue("a\\sb\\n\\t\\\\") == "a b\n\t\\",
		"desktop value escaping was parsed incorrectly");
	Expect(jpegview_linux::MimeTypeMatches("image/*", "image/png"), "MIME wildcard did not match");
	Expect(jpegview_linux::MimeTypeMatches("image/x-ms-bmp", "image/bmp"), "MIME alias did not match");
	Expect(jpegview_linux::MimeTypeMatches("*/*", "image/png"), "universal MIME wildcard did not match");
	Expect(!jpegview_linux::MimeTypeMatches("image/jpeg", "image/png"), "incompatible MIME type matched");
	Expect(jpegview_linux::MimeTypeForExtension(".jpg") == "image/jpeg", "JPEG MIME mapping is incorrect");

	const fs::path image = temporary.path() / "photos" / "a file.jpg";
	application.name = "Test Viewer";
	application.desktopFile = desktopFile;
	application.exec = "viewer --name \"%n\" %u %% %d";
	const std::vector<std::string> tokens = jpegview_linux::TokenizeDesktopExec(
		"viewer --label \"hello world\" 'single quoted' escaped\\ space");
	Expect(tokens == std::vector<std::string>({"viewer", "--label", "hello world", "single quoted", "escaped space"}),
		"desktop Exec tokenization is incorrect");
	const std::vector<std::string> arguments = jpegview_linux::DesktopExecArguments(application, image);
	Expect(arguments.size() == 6 && arguments[0] == "viewer" && arguments[1] == "--name" &&
		arguments[2] == "a file.jpg" && arguments[3].find("file:///tmp") == 0 &&
		arguments[4] == "%" && arguments[5] == fs::absolute(image.parent_path()).lexically_normal().string(),
		"desktop Exec field expansion is incorrect");

	application.exec = "viewer --flag";
	const std::vector<std::string> implicitFile = jpegview_linux::DesktopExecArguments(application, image);
	Expect(implicitFile.size() == 3 && implicitFile[2] == fs::absolute(image).lexically_normal().string(),
		"desktop Exec did not append an image when no field code was present");
}

class ExifFixture {
public:
	ExifFixture() : bytes_({'E', 'x', 'i', 'f', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) {
		Write16(6, 0x4949);
		Write16(8, 42);
		Write32(10, 8);
	}

	std::vector<std::uint8_t> Build() {
		const std::uint32_t ifd0 = AddDirectory(7);
		const std::uint32_t exif = AddDirectory(8);
		const std::uint32_t gps = AddDirectory(6);
		SetEntry(ifd0, 0, 0x010F, 2, 5, AddString("Acme"));
		SetEntry(ifd0, 1, 0x0110, 2, 6, AddString("Model"));
		SetEntry(ifd0, 2, 0x010E, 2, 12, AddString("A test image"));
		SetEntry(ifd0, 3, 0x0131, 2, 6, AddString("Tester"));
		SetEntry(ifd0, 4, 0x0132, 2, 20, AddString("2024:01:02 03:04:05"));
		SetEntry(ifd0, 5, 0x8769, 4, 1, exif);
		SetEntry(ifd0, 6, 0x8825, 4, 1, gps);

		SetEntry(exif, 0, 0x9003, 2, 20, AddString("2024:02:03 04:05:06"));
		SetEntry(exif, 1, 0x829A, 5, 1, AddRational(1, 125));
		SetEntry(exif, 2, 0x9204, 10, 1, AddRational(-1, 3));
		SetEntryInline16(exif, 3, 0x9209, 3, 1, 1);
		SetEntry(exif, 4, 0x920A, 5, 1, AddRational(50, 1));
		SetEntry(exif, 5, 0x829D, 5, 1, AddRational(28, 10));
		SetEntryInline16(exif, 6, 0x8827, 3, 1, 200);
		std::vector<std::uint8_t> comment = {'A', 'S', 'C', 'I', 'I', 0, 0, 0, 'h', 'e', 'l', 'l', 'o', 0};
		SetEntry(exif, 7, 0x9286, 7, static_cast<std::uint32_t>(comment.size()), AddBytes(comment));

		SetEntryInlineBytes(gps, 0, 0x0001, 2, 2, {'S', 0});
		SetEntry(gps, 1, 0x0002, 5, 3, AddRationals({{12, 1}, {34, 1}, {56, 1}}));
		SetEntryInlineBytes(gps, 2, 0x0003, 2, 2, {'W', 0});
		SetEntry(gps, 3, 0x0004, 5, 3, AddRationals({{98, 1}, {7, 1}, {6, 1}}));
		SetEntryInlineBytes(gps, 4, 0x0005, 1, 1, {1});
		SetEntry(gps, 5, 0x0006, 5, 1, AddRational(30, 1));
		return bytes_;
	}

private:
	void Write16(std::size_t position, std::uint16_t value) {
		Ensure(position + 2);
		bytes_[position] = static_cast<std::uint8_t>(value & 0xff);
		bytes_[position + 1] = static_cast<std::uint8_t>(value >> 8);
	}

	void Write32(std::size_t position, std::uint32_t value) {
		Ensure(position + 4);
		for (int byte = 0; byte < 4; ++byte) bytes_[position + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
	}

	void Ensure(std::size_t size) {
	if (bytes_.size() < size) bytes_.resize(size, 0);
	}

	std::uint32_t AddDirectory(std::size_t count) {
		const std::uint32_t relative = static_cast<std::uint32_t>(bytes_.size() - 6);
		const std::size_t start = bytes_.size();
		bytes_.resize(bytes_.size() + 2 + count * 12 + 4, 0);
		Write16(start, static_cast<std::uint16_t>(count));
		return relative;
	}

	std::uint32_t AddBytes(const std::vector<std::uint8_t>& bytes) {
		const std::uint32_t relative = static_cast<std::uint32_t>(bytes_.size() - 6);
		bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
		return relative;
	}

	std::uint32_t AddString(const std::string& text) {
		std::vector<std::uint8_t> bytes(text.begin(), text.end());
		bytes.push_back(0);
		return AddBytes(bytes);
	}

	std::uint32_t AddRational(std::int32_t numerator, std::int32_t denominator) {
		const std::uint32_t relative = static_cast<std::uint32_t>(bytes_.size() - 6);
		const std::size_t start = bytes_.size();
		bytes_.resize(bytes_.size() + 8, 0);
		Write32(start, static_cast<std::uint32_t>(numerator));
		Write32(start + 4, static_cast<std::uint32_t>(denominator));
		return relative;
	}

	std::uint32_t AddRationals(const std::vector<std::pair<std::int32_t, std::int32_t>>& values) {
		const std::uint32_t relative = static_cast<std::uint32_t>(bytes_.size() - 6);
		for (const auto& value : values) AddRational(value.first, value.second);
		return relative;
	}

	std::size_t EntryPosition(std::uint32_t directory, std::size_t index) const {
		return 6 + directory + 2 + index * 12;
	}

	void SetEntry(std::uint32_t directory, std::size_t index, std::uint16_t tag,
		std::uint16_t type, std::uint32_t count, std::uint32_t value) {
		const std::size_t position = EntryPosition(directory, index);
		Write16(position, tag);
		Write16(position + 2, type);
		Write32(position + 4, count);
		Write32(position + 8, value);
	}

	void SetEntryInline16(std::uint32_t directory, std::size_t index, std::uint16_t tag,
		std::uint16_t type, std::uint32_t count, std::uint16_t value) {
		SetEntry(directory, index, tag, type, count, value);
	}

	void SetEntryInlineBytes(std::uint32_t directory, std::size_t index, std::uint16_t tag,
		std::uint16_t type, std::uint32_t count, std::initializer_list<std::uint8_t> values) {
		const std::size_t position = EntryPosition(directory, index);
		Write16(position, tag);
		Write16(position + 2, type);
		Write32(position + 4, count);
		std::size_t offset = position + 8;
		for (std::uint8_t value : values) bytes_[offset++] = value;
	}

	std::vector<std::uint8_t> bytes_;
};

std::vector<std::uint8_t> InsertJpegSegment(const std::vector<std::uint8_t>& jpeg,
	std::uint8_t marker, const std::vector<std::uint8_t>& payload) {
	Expect(jpeg.size() >= 2 && jpeg[0] == 0xff && jpeg[1] == 0xd8, "test JPEG has no SOI marker");
	Expect(payload.size() + 2 <= 65535, "test JPEG segment is too large");
	std::vector<std::uint8_t> result{0xff, 0xd8, 0xff, marker,
		static_cast<std::uint8_t>((payload.size() + 2) >> 8),
		static_cast<std::uint8_t>((payload.size() + 2) & 0xff)};
	result.insert(result.end(), payload.begin(), payload.end());
	result.insert(result.end(), jpeg.begin() + 2, jpeg.end());
	return result;
}

void TestExifAndJpegCommentParsing() {
	TemporaryDirectory temporary;
	const fs::path jpeg = temporary.path() / "metadata.jpg";
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	options.jpegQuality = 100;
	std::string error;
	Expect(jpegview_linux::WriteImage(jpeg, pixels.data(), 2, 2, options, error),
		"cannot create EXIF fixture JPEG: " + error);
	const std::vector<std::uint8_t> withExif = InsertJpegSegment(ReadBytes(jpeg), 0xe1, ExifFixture().Build());
	const std::vector<std::uint8_t> withComment = InsertJpegSegment(withExif, 0xfe,
		std::vector<std::uint8_t>{'t', 'e', 's', 't', ' ', 'c', 'o', 'm', 'm', 'e', 'n', 't'});
	WriteBytes(jpeg, withComment);

	jpegview_linux::ExifInfo info;
	std::string comment;
	Expect(jpegview_linux::ReadJpegMetadata(jpeg, info, comment), "EXIF fixture was not recognized as JPEG");
	Expect(info.hasExif, "valid EXIF directory was not detected");
	Expect(info.cameraModel == "Acme Model", "camera make/model was parsed incorrectly");
	Expect(info.imageDescription == "A test image", "image description was parsed incorrectly");
	Expect(info.software == "Tester", "software metadata was parsed incorrectly");
	Expect(info.dateTime == "2024:01:02 03:04:05", "IFD0 date was parsed incorrectly");
	Expect(info.acquisitionDate == "2024:02:03 04:05:06", "EXIF acquisition date was parsed incorrectly");
	Expect(info.exposureTime == "1/125", "exposure time was parsed incorrectly");
	Expect(info.hasExposureBias, "exposure bias was not detected");
	ExpectNear(info.exposureBias, -1.0 / 3.0, 0.0001, "exposure bias was parsed incorrectly");
	Expect(info.hasFlash && info.flashFired, "flash metadata was parsed incorrectly");
	Expect(info.hasFocalLength, "focal length was not detected");
	ExpectNear(info.focalLength, 50.0, 0.0001, "focal length was parsed incorrectly");
	Expect(info.hasFNumber, "f-number was not detected");
	ExpectNear(info.fNumber, 2.8, 0.0001, "f-number was parsed incorrectly");
	Expect(info.isoSpeed == 200, "ISO metadata was parsed incorrectly");
	Expect(info.userComment == "hello", "user comment was parsed incorrectly");
	Expect(info.hasGps && info.gpsLocation == "-12.58222, -98.11833", "GPS metadata was parsed incorrectly");
	Expect(info.hasAltitude, "GPS altitude was not detected");
	ExpectNear(info.altitude, -30.0, 0.0001, "GPS altitude was parsed incorrectly");
	Expect(comment == "test comment", "JPEG comment was parsed incorrectly");

	const fs::path plain = temporary.path() / "plain.jpg";
	Expect(jpegview_linux::WriteImage(plain, pixels.data(), 2, 2, options, error), "cannot create plain JPEG");
	info = {};
	comment.clear();
	Expect(!jpegview_linux::ReadJpegMetadata(plain, info, comment), "plain JPEG unexpectedly reported metadata");
	Expect(!info.hasExif && comment.empty(), "plain JPEG unexpectedly contained metadata");

	const fs::path malformed = temporary.path() / "malformed.jpg";
	WriteBytes(malformed, {0xff, 0xd8, 0xff, 0xe1, 0x00, 0x0a, 'E', 'x', 'i', 'f', 0, 0});
	info = {};
	comment.clear();
	Expect(!jpegview_linux::ReadJpegMetadata(malformed, info, comment), "malformed metadata should be treated as absent");
	Expect(!info.hasExif, "malformed metadata was incorrectly accepted as EXIF");
}

void RunTest(const char* name, void (*test)(), int& failures) {
	try {
		test();
		std::cout << "PASS " << name << '\n';
	} catch (const std::exception& error) {
		++failures;
		std::cerr << "FAIL " << name << ": " << error.what() << '\n';
	}
}

} // namespace

int main() {
	int failures = 0;
	RunTest("file-list-filtering-and-logical-sorting", TestFileListFilteringAndLogicalSorting, failures);
	RunTest("file-list-date-sorting-and-selection", TestFileListDateSortingAndSelectionPreservation, failures);
	RunTest("file-list-navigation-modes-and-reload", TestFileListNavigationModesAndReload, failures);
	RunTest("file-list-multiple-inputs", TestFileListMultipleInputs, failures);
	RunTest("image-writer-decoder-round-trips", TestImageWriterDecoderRoundTrips, failures);
	RunTest("decoder-failures", TestDecoderFailures, failures);
	RunTest("settings-round-trip-and-malformed-values", TestSettingsRoundTripAndMalformedValues, failures);
	RunTest("sort-mode-mappings", TestSortModeMappings, failures);
	RunTest("desktop-application-parsing-and-expansion", TestDesktopApplicationParsingAndExecExpansion, failures);
	RunTest("exif-and-jpeg-comment-parsing", TestExifAndJpegCommentParsing, failures);
	if (failures != 0) {
		std::cerr << failures << " test group(s) failed\n";
		return 1;
	}
	std::cout << "All core tests passed\n";
	return 0;
}
