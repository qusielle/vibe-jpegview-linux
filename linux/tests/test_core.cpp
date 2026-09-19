#include "exif_reader.h"
#include "file_list.h"
#include "image_decoder.h"
#include "image_writer.h"
#include "settings.h"
#include "sort_mode.h"
#include "desktop_applications.h"
#include "batch_copy.h"
#include "image_formats.h"
#include "input_commands.h"
#include "viewport.h"
#include "resize_model.h"
#include "context_menu_model.h"
#include "overlay_layout.h"
#include "thumbnail_panel_model.h"
#include "thumbnail_resampler.h"
#include "app_icon.h"
#include "image_info_model.h"
#include "file_dialog_model.h"
#include "system_font.h"

#include "../../src/JPEGView/resource.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>
#include <zlib.h>

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

void ExpectRect(const jpegview_linux::ViewportRect& actual, int x, int y, int width, int height,
	const std::string& message) {
	if (actual.x != x || actual.y != y || actual.width != width || actual.height != height) {
		std::ostringstream details;
		details << message << " (actual=" << actual.x << ',' << actual.y << ' ' << actual.width << 'x'
			<< actual.height << ", expected=" << x << ',' << y << ' ' << width << 'x' << height << ')';
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

class ScopedEnvironment {
public:
	ScopedEnvironment(const char* name, const std::string& value) : name_(name) {
		const char* previous = std::getenv(name);
		if (previous != nullptr) previous_ = previous;
		if (setenv(name, value.c_str(), 1) != 0) throw TestFailure("cannot set test environment");
	}

	~ScopedEnvironment() {
		if (previous_.has_value()) setenv(name_.c_str(), previous_->c_str(), 1);
		else unsetenv(name_.c_str());
	}

	void Clear() {
		if (unsetenv(name_.c_str()) != 0) throw TestFailure("cannot clear test environment");
	}

private:
	std::string name_;
	std::optional<std::string> previous_;
};

void WriteBytes(const fs::path& filename, const std::vector<std::uint8_t>& bytes) {
	std::ofstream output(filename, std::ios::binary);
	if (!output) throw TestFailure("cannot create " + filename.string());
	output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!output) throw TestFailure("cannot write " + filename.string());
}

std::uint32_t ReadBigEndian32(const std::vector<std::uint8_t>& bytes, std::size_t position) {
	Expect(position + 4 <= bytes.size(), "truncated PNG integer in test fixture");
	return (static_cast<std::uint32_t>(bytes[position]) << 24) |
		(static_cast<std::uint32_t>(bytes[position + 1]) << 16) |
		(static_cast<std::uint32_t>(bytes[position + 2]) << 8) | bytes[position + 3];
}

void AppendBigEndian16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
	bytes.push_back(static_cast<std::uint8_t>(value));
}

void AppendBigEndian32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
	bytes.push_back(static_cast<std::uint8_t>(value >> 24));
	bytes.push_back(static_cast<std::uint8_t>(value >> 16));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
	bytes.push_back(static_cast<std::uint8_t>(value));
}

void AppendPngChunk(std::vector<std::uint8_t>& output, const std::string& type,
	const std::vector<std::uint8_t>& data) {
	Expect(type.size() == 4, "test PNG chunk type is not four bytes");
	AppendBigEndian32(output, static_cast<std::uint32_t>(data.size()));
	output.insert(output.end(), type.begin(), type.end());
	output.insert(output.end(), data.begin(), data.end());
	uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type.data()), type.size());
	crc = crc32(crc, data.data(), data.size());
	AppendBigEndian32(output, static_cast<std::uint32_t>(crc));
}

std::vector<std::vector<std::uint8_t>> PngChunks(const std::vector<std::uint8_t>& png,
	const std::string& requestedType) {
	static constexpr std::array<std::uint8_t, 8> signature = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
	Expect(png.size() >= signature.size() && std::equal(signature.begin(), signature.end(), png.begin()),
		"test PNG has an invalid signature");
	std::vector<std::vector<std::uint8_t>> result;
	std::size_t position = signature.size();
	while (position + 12 <= png.size()) {
		const std::uint32_t length = ReadBigEndian32(png, position);
		position += 4;
		Expect(length <= png.size() - position - 8, "test PNG chunk is truncated");
		const std::string type(reinterpret_cast<const char*>(png.data() + position), 4);
		position += 4;
		if (type == requestedType) {
			result.emplace_back(png.begin() + static_cast<std::ptrdiff_t>(position),
				png.begin() + static_cast<std::ptrdiff_t>(position + length));
		}
		position += length + 4;
		if (type == "IEND") break;
	}
	return result;
}

std::vector<std::uint8_t> MakeApng(const std::vector<std::uint8_t>& firstPng,
	const std::vector<std::uint8_t>& secondPng) {
	const auto headers = PngChunks(firstPng, "IHDR");
	const auto firstData = PngChunks(firstPng, "IDAT");
	const auto secondData = PngChunks(secondPng, "IDAT");
	Expect(headers.size() == 1 && firstData.size() == 1 && secondData.size() == 1,
		"test PNG did not have the expected simple chunk layout");
	std::vector<std::uint8_t> result = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
	AppendPngChunk(result, "IHDR", headers.front());
	AppendPngChunk(result, "acTL", {0, 0, 0, 2, 0, 0, 0, 1});
	std::vector<std::uint8_t> frameControl;
	AppendBigEndian32(frameControl, 0);
	AppendBigEndian32(frameControl, 2);
	AppendBigEndian32(frameControl, 2);
	AppendBigEndian32(frameControl, 0);
	AppendBigEndian32(frameControl, 0);
	AppendBigEndian16(frameControl, 7);
	AppendBigEndian16(frameControl, 100);
	frameControl.push_back(0);
	frameControl.push_back(0);
	AppendPngChunk(result, "fcTL", frameControl);
	AppendPngChunk(result, "IDAT", firstData.front());
	frameControl[3] = 1;
	AppendPngChunk(result, "fcTL", frameControl);
	std::vector<std::uint8_t> frameData{0, 0, 0, 2};
	frameData.insert(frameData.end(), secondData.front().begin(), secondData.front().end());
	AppendPngChunk(result, "fdAT", frameData);
	AppendPngChunk(result, "IEND", {});
	return result;
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
	Expect(files.Select(2) && files.Current().filename() == "photo10.png",
		"direct file selection for the thumbnail panel selected the wrong image");
	Expect(!files.Select(3) && files.Current().filename() == "photo10.png",
		"out-of-range direct file selection changed the current image");

	FileList descending({directory.string()}, FileList::SortMode::FileName, false, false);
	Expect(FileNames(descending) == std::vector<std::string>({"photo10.png", "photo2.png", "photo1.png"}),
		"descending logical filename ordering is incorrect");
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
		{'e', 0x00C3u, IDM_TOUCH_IMAGE_EXIF},
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
		{'z', 0, IDM_SORT_RANDOM},
		{SDLK_F7, 0, IDM_LOOP_FOLDER},
		{SDLK_F8, 0, IDM_LOOP_RECURSIVELY},
		{SDLK_F9, 0, IDM_LOOP_SIBLINGS},
		{SDLK_DELETE, 0, IDM_MOVE_TO_RECYCLE_BIN_CONFIRM},
		{'w', 0, IDM_EXPLORE},
		{SDLK_RIGHT, 0, IDM_NEXT},
		{SDLK_PAGEDOWN, 0, IDM_NEXT},
		{SDLK_LEFT, 0, IDM_PREV},
		{SDLK_PAGEUP, 0, IDM_PREV},
		{SDLK_HOME, 0, IDM_FIRST},
		{SDLK_END, 0, IDM_LAST},
		{SDLK_SPACE, 0, IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS},
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

	struct OptionalFormatCase {
		const char* extension;
		int dimension;
	};
	const std::vector<OptionalFormatCase> optionalFormats = {
#if JPEGVIEW_HAVE_GIF
		{".gif", 2},
#endif
#if JPEGVIEW_HAVE_TIFF
		{".tiff", 2},
#endif
#if JPEGVIEW_HAVE_WEBP
		{".webp", 2},
#endif
#if JPEGVIEW_HAVE_HEIF
		// Ubuntu 20.04's libheif/x265 encoder cannot encode a 2x2 image.
		// Exercise the codec with a realistic size while keeping the other
		// optional format tests sensitive to tiny-image regressions.
		{".heic", 64},
#endif
#if JPEGVIEW_HAVE_AVIF
		{".avif", 2},
#endif
#if JPEGVIEW_HAVE_JXL
		{".jxl", 2},
#endif
	};
	for (const OptionalFormatCase& format : optionalFormats) {
		const fs::path filename = temporary.path() / ("optional" + std::string(format.extension));
		std::vector<std::uint8_t> optionalPixels(
			static_cast<std::size_t>(format.dimension) * format.dimension * 4);
		for (int y = 0; y < format.dimension; ++y) {
			for (int x = 0; x < format.dimension; ++x) {
				const std::size_t target = (static_cast<std::size_t>(y) * format.dimension + x) * 4;
				const std::size_t source = (static_cast<std::size_t>(y % 2) * 2 + x % 2) * 4;
				std::copy_n(pixels.data() + source, 4, optionalPixels.data() + target);
			}
		}
		error.clear();
		Expect(jpegview_linux::WriteImage(filename, optionalPixels.data(), format.dimension,
			format.dimension, options, error),
			"cannot write optional " + filename.extension().string() + ": " + error);
		DecodedImage decoded;
		error.clear();
		Expect(jpegview_linux::DecodeImage(filename, decoded, error),
			"cannot decode optional " + filename.extension().string() + ": " + error);
		Expect(decoded.frames.size() == 1 && decoded.frames.front().width == format.dimension &&
			decoded.frames.front().height == format.dimension,
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

void ExpectStaticFrame(const fs::path& filename, int width, int height,
	const std::vector<std::uint8_t>& expectedPixels) {
	DecodedImage decoded;
	std::string error;
	Expect(jpegview_linux::DecodeImage(filename, decoded, error),
		"cannot decode PNM fixture " + filename.filename().string() + ": " + error);
	Expect(!decoded.animation && decoded.frames.size() == 1, "PNM fixture was not decoded as a static image");
	Expect(decoded.frames.front().width == width && decoded.frames.front().height == height,
		"PNM fixture dimensions are incorrect for " + filename.filename().string());
	if (decoded.frames.front().bgra != expectedPixels) {
		std::ostringstream actual;
		for (std::uint8_t value : decoded.frames.front().bgra) actual << static_cast<int>(value) << ',';
		throw TestFailure("PNM fixture pixels are incorrect for " + filename.filename().string() +
			" (actual=" + actual.str() + ")");
	}
}

void TestPnmVariants() {
	TemporaryDirectory temporary;
	WriteText(temporary.path() / "ascii.pgm", "P2\n# grayscale comment\n2 1\n255\n0 255\n");
	ExpectStaticFrame(temporary.path() / "ascii.pgm", 2, 1,
		{0, 0, 0, 255, 255, 255, 255, 255});
	WriteText(temporary.path() / "ascii.pbm", "P1\n2 1\n0 1\n");
	ExpectStaticFrame(temporary.path() / "ascii.pbm", 2, 1,
		{255, 255, 255, 255, 0, 0, 0, 255});

	WriteBytes(temporary.path() / "binary.pgm", {
		'P', '5', '\n', '2', ' ', '1', '\n', '6', '5', '5', '3', '5', '\n',
		0x00, 0x00, 0xff, 0xff});
	ExpectStaticFrame(temporary.path() / "binary.pgm", 2, 1,
		{0, 0, 0, 255, 255, 255, 255, 255});

	WriteBytes(temporary.path() / "bitmap.pbm", {
		'P', '4', '\n', '8', ' ', '1', '\n', 0xaa});
	std::vector<std::uint8_t> pbmExpected;
	for (int bit = 0; bit < 8; ++bit) {
		const std::uint8_t value = (bit % 2 == 0) ? 0 : 255;
		pbmExpected.insert(pbmExpected.end(), {value, value, value, 255});
	}
	ExpectStaticFrame(temporary.path() / "bitmap.pbm", 8, 1, pbmExpected);

	WriteText(temporary.path() / "ascii.ppm", "P3\n2 1\n255\n255 0 0   0 255 0\n");
	ExpectStaticFrame(temporary.path() / "ascii.ppm", 2, 1,
		{0, 0, 255, 255, 0, 255, 0, 255});

	WriteText(temporary.path() / "alpha.pam",
		"P7\nWIDTH 2\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n");
	std::ofstream alpha(temporary.path() / "alpha.pam", std::ios::binary | std::ios::app);
	alpha.write("\xff\x00\x00\xff\x00\xff\x00\x80", 8);
	alpha.close();
	ExpectStaticFrame(temporary.path() / "alpha.pam", 2, 1,
		{0, 0, 255, 255, 0, 255, 0, 128});
}

void TestAnimatedImageDecoders() {
	TemporaryDirectory temporary;
	ImageWriteOptions options;
	const std::vector<std::uint8_t> red = {
		0, 0, 255, 255, 0, 0, 255, 255,
		0, 0, 255, 255, 0, 0, 255, 255};
	const std::vector<std::uint8_t> blue = {
		255, 0, 0, 255, 255, 0, 0, 255,
		255, 0, 0, 255, 255, 0, 0, 255};
	std::string error;
	const fs::path firstPng = temporary.path() / "first.png";
	const fs::path secondPng = temporary.path() / "second.png";
	Expect(jpegview_linux::WriteImage(firstPng, red.data(), 2, 2, options, error), "cannot write APNG first frame");
	Expect(jpegview_linux::WriteImage(secondPng, blue.data(), 2, 2, options, error), "cannot write APNG second frame");
	const fs::path apng = temporary.path() / "animated.apng";
	WriteBytes(apng, MakeApng(ReadBytes(firstPng), ReadBytes(secondPng)));
	DecodedImage decoded;
	Expect(jpegview_linux::DecodeImage(apng, decoded, error), "cannot decode APNG: " + error);
	Expect(decoded.animation && decoded.frames.size() == 2 && decoded.loopCount == 1,
		"APNG animation metadata is incorrect");
	Expect(decoded.frames[0].delayMs == 70 && decoded.frames[1].delayMs == 70,
		"APNG frame delay is incorrect");
	Expect(decoded.frames[0].bgra != decoded.frames[1].bgra, "APNG frames were not composited independently");

	const std::vector<std::uint8_t> gif = {
		0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x02, 0x00, 0xf0, 0x00,
		0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x21, 0xff, 0x0b, 0x4e, 0x45,
		0x54, 0x53, 0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x01,
		0x00, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x21, 0xff,
		0x0b, 0x49, 0x6d, 0x61, 0x67, 0x65, 0x4d, 0x61, 0x67, 0x69, 0x63, 0x6b,
		0x0e, 0x67, 0x61, 0x6d, 0x6d, 0x61, 0x3d, 0x30, 0x2e, 0x34, 0x35, 0x34,
		0x35, 0x34, 0x35, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02,
		0x00, 0x00, 0x02, 0x02, 0x84, 0x51, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x07,
		0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x49, 0x6d, 0x61, 0x67, 0x65, 0x4d,
		0x61, 0x67, 0x69, 0x63, 0x6b, 0x0e, 0x67, 0x61, 0x6d, 0x6d, 0x61, 0x3d,
		0x30, 0x2e, 0x34, 0x35, 0x34, 0x35, 0x34, 0x35, 0x00, 0x2c, 0x00, 0x00,
		0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x80, 0x00, 0x00, 0xff, 0xff, 0xff,
		0xff, 0x02, 0x02, 0x84, 0x51, 0x00, 0x3b};
	const fs::path gifFile = temporary.path() / "animated.gif";
	WriteBytes(gifFile, gif);
	error.clear();
	decoded = {};
	Expect(jpegview_linux::DecodeImage(gifFile, decoded, error), "cannot decode GIF: " + error);
	Expect(decoded.animation && decoded.frames.size() == 2 && decoded.loopCount == 1,
		"GIF animation metadata is incorrect");
	Expect(decoded.frames[0].delayMs == 100 && decoded.frames[1].delayMs == 100,
		"GIF frame delay was not clamped to the viewer minimum");
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
	expected.thumbnailPanelVisible = true;
	expected.thumbnailPanelWidth = 287;
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
	Expect(loaded.thumbnailPanelVisible == expected.thumbnailPanelVisible,
		"thumbnail panel visibility did not round-trip");
	Expect(loaded.thumbnailPanelWidth == expected.thumbnailPanelWidth,
		"thumbnail panel width did not round-trip");
	Expect(loaded.infoVisible == expected.infoVisible && loaded.showFilename == expected.showFilename &&
		loaded.autoContrast == expected.autoContrast,
		"overlay/correction settings did not round-trip");
	Expect(loaded.copyRenamePattern == expected.copyRenamePattern, "batch pattern did not round-trip");
	Expect(loaded.manualZoomSet, "saved manual zoom was not marked present");
	ExpectNear(loaded.manualZoom, expected.manualZoom, 0.0000001, "manual zoom did not round-trip");

	const fs::path malformed = temporary.path() / "malformed.conf";
	std::ofstream malformedOutput(malformed);
	malformedOutput << "  scale_mode = manual\nmanual_zoom=not-a-number\n"
		"thumbnail_panel_width=not-a-number\nunknown_key=value\n";
	malformedOutput.close();
	loaded = {};
	Expect(jpegview_linux::LoadViewerSettings(malformed, loaded), "malformed settings file was rejected entirely");
	Expect(loaded.scaleMode == "manual", "whitespace around a setting was not trimmed");
	Expect(!loaded.manualZoomSet && loaded.manualZoom == 1.0,
		"malformed manual zoom did not retain its default");
	Expect(!loaded.thumbnailPanelVisible,
		"settings without thumbnail visibility did not retain the hidden default");
	Expect(loaded.thumbnailPanelWidth == jpegview_linux::kDefaultThumbnailPanelWidth,
		"malformed thumbnail width did not retain its default");
}

void TestSettingsPathSelection() {
	TemporaryDirectory temporary;
	const fs::path xdgHome = temporary.path() / "xdg";
	const fs::path home = temporary.path() / "home";
	ScopedEnvironment xdg("XDG_CONFIG_HOME", xdgHome.string());
	ScopedEnvironment homeEnvironment("HOME", home.string());
	Expect(jpegview_linux::ViewerSettingsPath() == xdgHome / "jpegview-linux" / "settings.conf",
		"XDG_CONFIG_HOME settings path is incorrect");
	xdg.Clear();
	Expect(jpegview_linux::ViewerSettingsPath() == home / ".config" / "jpegview-linux" / "settings.conf",
		"HOME settings path fallback is incorrect");
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

std::string FormatLocalTime(std::time_t timestamp, const char* format) {
	std::tm localTime{};
	Expect(localtime_r(&timestamp, &localTime) != nullptr, "cannot convert test timestamp to local time");
	char output[64]{};
	Expect(std::strftime(output, sizeof(output), format, &localTime) != 0,
		"cannot format test timestamp");
	return output;
}

void TestBatchCopyPatternExpansionAndPreview() {
	const fs::path source = fs::path("/tmp/photos/holiday12.JPG");
	const std::time_t timestamp = 1706933106; // 2024-02-03 04:05:06 UTC.
	const fs::path pictures = fs::path("/tmp/Pictures");
	const std::string pattern = R"(folder\%x-%n-%2x-%3x-%9x-%f-%F-%e-%h-%min-%d-%m-%y-%2y-%3M-%M-%pictures%)";
	const std::string expanded = jpegview_linux::ExpandBatchPattern(pattern, 4, source, timestamp, pictures);
	const std::string expected = "folder/5-12-05-005-000000005-holiday12.JPG-holiday12-JPG-" +
		FormatLocalTime(timestamp, "%H") + "-" + FormatLocalTime(timestamp, "%M") + "-" +
		FormatLocalTime(timestamp, "%d") + "-" + FormatLocalTime(timestamp, "%m") + "-" +
		FormatLocalTime(timestamp, "%Y") + "-" + FormatLocalTime(timestamp, "%y") + "-" +
		FormatLocalTime(timestamp, "%b") + "-" + FormatLocalTime(timestamp, "%B") + "-/tmp/Pictures";
	Expect(expanded == expected, "batch copy pattern expansion is incorrect: " + expanded);
	Expect(jpegview_linux::ExpandBatchPattern("%f", 0, fs::path("no-extension"), 0) == "no-extension",
		"batch copy expansion mishandled a filename without an extension");

	jpegview_linux::BatchCopyItem first;
	first.source = fs::path("/tmp/photos/a12.jpg");
	first.modificationTime = timestamp;
	first.selected = true;
	jpegview_linux::BatchCopyItem skipped;
	skipped.source = fs::path("/tmp/photos/b13.jpg");
	jpegview_linux::BatchCopyItem renamed;
	renamed.source = fs::path("/tmp/photos/c14.jpg");
	renamed.selected = true;
	std::vector<jpegview_linux::BatchCopyItem> items{first, skipped, renamed};
	jpegview_linux::UpdateBatchCopyPreview("out/%2x-%f", items);
	Expect(items[0].destinationText == "out/01-a12.jpg", "batch copy preview text is incorrect");
	Expect(items[0].destination == fs::path("/tmp/photos/out/01-a12.jpg"),
		"batch copy relative destination was not resolved against the source directory");
	Expect(items[0].copy, "batch copy preview did not classify a different directory as a copy");
	Expect(items[1].destination.empty() && items[1].destinationText.empty() && !items[1].copy,
		"unselected batch copy item was included in the preview");
	Expect(items[2].destinationText == "out/02-c14.jpg" && items[2].copy,
		"batch copy selected index did not ignore unselected items");
	Expect(jpegview_linux::BatchCopyDestination("renamed-%f", items[2], 0) ==
		fs::path("/tmp/photos/renamed-c14.jpg"), "same-directory rename target is incorrect");
	items[0].selected = false;
	items[2].selected = false;
	items[0].destination = fs::path("stale");
	items[0].destinationText = "stale";
	items[0].copy = true;
	jpegview_linux::UpdateBatchCopyPreview("", items);
	Expect(items[0].destination.empty() && items[0].destinationText.empty() && !items[0].copy,
		"empty batch pattern did not clear stale preview state");
	Expect(jpegview_linux::FormatBatchDate(0).empty(), "zero batch timestamp should format as empty");
	Expect(jpegview_linux::FormatBatchDate(timestamp) ==
		FormatLocalTime(timestamp, "%Y-%m-%d %H:%M:%S"), "batch date formatting is incorrect");
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

void TestViewportModesAndGeometry() {
	jpegview_linux::Viewport viewport;
	Expect(std::string(viewport.ScaleMode()) == "fit_no_enlarge", "viewport default scale mode changed");

	viewport.Fit(100, 50, 500, 300);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "fit-no-enlarge scaled a small image up");
	ExpectRect(viewport.Destination(100, 50, 500, 300), 200, 125, 100, 50,
		"small image was not centered at original size");

	viewport.Fit(1000, 500, 500, 300);
	ExpectNear(viewport.Zoom(), 0.5, 0.0001, "large image fit used the wrong scale");
	ExpectRect(viewport.Destination(1000, 500, 500, 300), 0, 25, 500, 250,
		"fitted image geometry is incorrect");

	viewport.Fit(1000, 500, 500, 300, true, false);
	Expect(std::string(viewport.ScaleMode()) == "fill", "fill mode was not recorded");
	ExpectNear(viewport.Zoom(), 0.6, 0.0001, "fill mode used the wrong scale");
	ExpectRect(viewport.Destination(1000, 500, 500, 300), -50, 0, 600, 300,
		"fill-and-crop geometry is incorrect");

	viewport.Fit(1000, 600, 500, 300);
	ExpectRect(viewport.Destination(1000, 600, 500, 300), 0, 0, 500, 300,
		"matching-aspect oversized image retained an artificial border");

	viewport.LoadScaleMode("fit", false, 1.0);
	Expect(viewport.IsFitToWindow() && !viewport.NoEnlarge(), "fit mode incorrectly prevents enlargement");
	Expect(std::string(viewport.ScaleMode()) == "fit", "fit mode did not round-trip");
	viewport.LoadScaleMode("fill_no_enlarge", false, 1.0);
	Expect(viewport.FillWithCrop() && viewport.NoEnlarge(), "fill-no-enlarge mode did not load");
	viewport.LoadScaleMode("unknown", false, 1.0);
	Expect(std::string(viewport.ScaleMode()) == "fit_no_enlarge", "unknown scale mode did not use safe default");
}

void TestViewportManualZoomPanAndRestore() {
	jpegview_linux::Viewport viewport;
	viewport.LoadScaleMode("manual", true, 100.0);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "persisted transient zoom was restored instead of actual size");
	viewport.LoadScaleMode("manual", true, 0.0001);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "persisted transient zoom replaced actual size");

	viewport.ActualSize();
	Expect(viewport.IsActualSize(), "actual-size viewport was not identified as actual size");
	viewport.ZoomAt(2.0, 150, 75, 200, 100, 400, 200);
	Expect(!viewport.IsActualSize(), "zoomed viewport was incorrectly identified as actual size");
	ExpectNear(viewport.Zoom(), 2.0, 0.0001, "anchored zoom did not update scale");
	ExpectRect(viewport.Destination(200, 100, 400, 200), 50, 25, 400, 200,
		"anchored zoom did not preserve the point beneath the pointer");
	viewport.Pan(10.0, -5.0);
	ExpectRect(viewport.Destination(200, 100, 400, 200), 60, 20, 400, 200,
		"viewport pan did not update the destination");
	Expect(std::string(viewport.ScaleMode()) == "manual", "zoomed or panned viewport was not manual");
	const jpegview_linux::ViewportSnapshot manual = viewport.Snapshot();
	viewport.ActualSize();
	viewport.Pan(48.0, -48.0);
	Expect(viewport.IsActualSize() && viewport.OffsetX() == 48.0 && viewport.OffsetY() == -48.0,
		"actual-size keyboard-style pan did not preserve scale or update offsets");

	viewport.Fit(800, 600, 400, 300);
	viewport.Restore(manual, 320, 200, 640, 480);
	ExpectNear(viewport.Zoom(), 2.0, 0.0001, "manual snapshot did not restore zoom");
	ExpectNear(viewport.OffsetX(), 0.0, 0.0001, "manual restore did not reset horizontal pan");
	ExpectNear(viewport.OffsetY(), 0.0, 0.0001, "manual restore did not reset vertical pan");

	viewport.Fit(800, 600, 400, 300, false, true);
	const jpegview_linux::ViewportSnapshot fitted = viewport.Snapshot();
	viewport.ActualSize();
	viewport.Restore(fitted, 1000, 500, 500, 300);
	ExpectNear(viewport.Zoom(), 0.5, 0.0001, "fit snapshot did not recompute for new geometry");
	Expect(viewport.IsFitToWindow() && viewport.NoEnlarge(), "fit snapshot flags were not restored");

	const double zoomBeforeInvalidInput = viewport.Zoom();
	viewport.ZoomAt(2.0, 0, 0, 0, 100, 500, 300);
	viewport.ZoomAt(-1.0, 0, 0, 100, 100, 500, 300);
	ExpectNear(viewport.Zoom(), zoomBeforeInvalidInput, 0.0001, "invalid zoom input changed viewport state");
}

void TestViewportNavigationResetsTransientZoom() {
	jpegview_linux::Viewport viewport;
	viewport.Fit(1000, 600, 500, 300, false, true);
	viewport.ZoomAt(2.0, 250, 150, 1000, 600, 500, 300);
	Expect(std::string(viewport.ScaleMode()) == "manual", "zoom did not affect the current image");
	const jpegview_linux::ViewportSnapshot fitNavigation = viewport.NavigationSnapshot();
	Expect(fitNavigation.fitToWindow && !fitNavigation.fillWithCrop && fitNavigation.noEnlarge,
		"transient zoom replaced the fit navigation mode");
	viewport.Restore(fitNavigation, 2000, 1000, 500, 300);
	ExpectNear(viewport.Zoom(), 0.25, 0.0001, "next image retained transient zoom instead of fitting");

	viewport.Fit(1000, 500, 500, 300, true, false);
	viewport.Pan(30.0, 20.0);
	const jpegview_linux::ViewportSnapshot fillNavigation = viewport.NavigationSnapshot();
	Expect(fillNavigation.fitToWindow && fillNavigation.fillWithCrop && !fillNavigation.noEnlarge,
		"transient pan replaced the fill navigation mode");

	viewport.ActualSize();
	viewport.ZoomAt(3.0, 250, 150, 200, 100, 500, 300);
	const jpegview_linux::ViewportSnapshot actualSizeNavigation = viewport.NavigationSnapshot();
	Expect(!actualSizeNavigation.fitToWindow,
		"actual-size navigation was incorrectly converted to a fit mode");
	ExpectNear(actualSizeNavigation.zoom, 1.0, 0.0001,
		"zooming replaced actual size as the navigation zoom");
	viewport.Restore(actualSizeNavigation, 400, 200, 500, 300);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "next image retained zoom instead of actual size");

	viewport.LoadScaleMode("manual", true, 2.5);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "legacy manual setting was restored instead of actual size");
	ExpectNear(viewport.NavigationZoom(), 1.0, 0.0001,
		"legacy manual setting was allowed to propagate to subsequent images");
}

void TestResizeModelAspectRatioValidationAndFilters() {
	jpegview_linux::ResizeModel model;
	model.Reset(400, 200);
	Expect(model.OriginalWidth() == 400 && model.OriginalHeight() == 200,
		"resize model did not retain original dimensions");
	Expect(model.FieldText(jpegview_linux::ResizeModel::kPercentField) == "100",
		"resize model did not initialize percentage");
	Expect(model.Filter() == 2 && std::string(model.FilterName()) == "SHARPEN LOW",
		"resize model default filter changed");

	model.FieldText(jpegview_linux::ResizeModel::kPercentField) = "50";
	Expect(model.UpdateFrom(jpegview_linux::ResizeModel::kPercentField), "valid resize percentage was rejected");
	Expect(model.FieldText(jpegview_linux::ResizeModel::kWidthField) == "200" &&
		model.FieldText(jpegview_linux::ResizeModel::kHeightField) == "100",
		"percentage did not update proportional dimensions");

	model.FieldText(jpegview_linux::ResizeModel::kWidthField) = "100";
	Expect(model.UpdateFrom(jpegview_linux::ResizeModel::kWidthField), "valid resize width was rejected");
	Expect(model.FieldText(jpegview_linux::ResizeModel::kPercentField) == "25" &&
		model.FieldText(jpegview_linux::ResizeModel::kHeightField) == "50",
		"width did not update percentage and proportional height");

	model.FieldText(jpegview_linux::ResizeModel::kHeightField) = "100";
	Expect(model.UpdateFrom(jpegview_linux::ResizeModel::kHeightField), "valid resize height was rejected");
	int width = 0;
	int height = 0;
	Expect(model.Target(width, height) && width == 200 && height == 100,
		"resize target did not reflect edited height");

	model.FieldText(jpegview_linux::ResizeModel::kWidthField) = "65535";
	Expect(!model.UpdateFrom(jpegview_linux::ResizeModel::kWidthField), "oversized pixel count was accepted");
	Expect(!model.ValidationMessage().empty(), "oversized resize did not provide validation feedback");
	model.FieldText(jpegview_linux::ResizeModel::kWidthField) = "12px";
	Expect(!model.Target(width, height), "partially numeric resize target was accepted");

	model.Reset(1, 1);
	model.FieldText(jpegview_linux::ResizeModel::kPercentField) = "0.1";
	Expect(!model.UpdateFrom(jpegview_linux::ResizeModel::kPercentField),
		"percentage producing a zero-sized image was accepted");
	model.FieldText(jpegview_linux::ResizeModel::kPercentField) = "nan";
	Expect(!model.UpdateFrom(jpegview_linux::ResizeModel::kPercentField), "non-finite percentage was accepted");

	model.CycleFilter(1);
	Expect(model.Filter() == 3, "resize filter did not advance");
	model.CycleFilter(1);
	Expect(model.Filter() == 0, "resize filter did not wrap forward");
	model.CycleFilter(-1);
	Expect(model.Filter() == 3, "resize filter did not wrap backward");
}

void TestContextMenuCompactionAndSelection() {
	using jpegview_linux::MenuItem;
	const std::vector<jpegview_linux::MenuItem> complete = {
		{"Open", 10},
		{nullptr, 0, true},
		{"Advanced heading", 0, false, false, true, nullptr, true},
		{"Advanced command", 20, false, false, true, "Ctrl+A", true},
		{"Disabled", 30, false, false, false},
		{"Close", 40},
	};
	const std::vector<jpegview_linux::MenuItem> compact =
		jpegview_linux::CompactMenuItems(complete, -1, "Show Advanced Options");
	Expect(compact.size() == 5, "compact menu retained advanced entries or lost core entries");
	Expect(std::string(compact[2].label) == "Show Advanced Options" && compact[2].command == -1,
		"compact menu did not insert the one-off advanced command at the first advanced item");
	Expect(std::count_if(compact.begin(), compact.end(), [](const jpegview_linux::MenuItem& item) {
		return item.command == -1;
	}) == 1, "compact menu inserted the advanced command more than once");
	Expect(std::none_of(compact.begin(), compact.end(), [](const jpegview_linux::MenuItem& item) {
		return item.advanced;
	}), "compact menu retained an advanced item");

	Expect(jpegview_linux::NextMenuSelection(compact, -1, 1) == 0,
		"menu selection did not start at first command");
	Expect(jpegview_linux::NextMenuSelection(compact, 0, 1) == 2,
		"menu selection did not skip separator");
	Expect(jpegview_linux::NextMenuSelection(compact, 2, 1) == 4,
		"menu selection did not skip disabled item");
	Expect(jpegview_linux::NextMenuSelection(compact, 4, 1) == 0,
		"menu selection did not wrap forward");
	Expect(jpegview_linux::NextMenuSelection(compact, 0, -1) == 4,
		"menu selection did not wrap backward");
	Expect(jpegview_linux::NextMenuSelection({{nullptr, 0, true}, {"Disabled", 1, false, false, false}},
		-1, 1) == -1, "menu with no actionable items returned a selection");

	const std::vector<MenuItem> separatedSections = {
		{nullptr, 0, true},
		{"Next", 50},
		{nullptr, 0, true},
		{"Navigation", 0, false, false, true, nullptr, true},
		{"Loop recursively", 51, false, false, true, nullptr, true},
		{nullptr, 0, true},
		{"Transform image", 0, false, false, true, nullptr, true},
		{"Unsupported transform", 52, false, false, false, nullptr, true},
		{nullptr, 0, true},
		{"Actual size", 53},
		{nullptr, 0, true},
		{nullptr, 0, true},
	};
	const std::vector<MenuItem> compactSections = jpegview_linux::CompactMenuItems(
		separatedSections, -1, "Show Advanced Options");
	Expect(compactSections.size() == 5 && compactSections[0].command == 50 &&
		compactSections[1].separator && compactSections[2].command == -1 &&
		compactSections[3].separator && compactSections[4].command == 53,
		"compacting advanced sections left empty, repeated, or trailing separators");
}

void TestContextMenuColumnLayoutAndNavigation() {
	using jpegview_linux::MenuColumn;
	using jpegview_linux::MenuItem;
	const std::vector<MenuItem> items = {
		{"Top", 10},
		{nullptr, 0, true},
		{"Heading", 0},
		{"Alpha", 20},
		{"Disabled", 30, false, false, false},
		{"Beta", 40},
		{nullptr, 0, true},
		{"Gamma", 50},
		{"Delta", 60},
		{"Last", 70},
	};
	const std::vector<MenuColumn> columns =
		jpegview_linux::LayoutMenuColumns(items, 43, 18, 7);
	Expect(columns.size() == 4, "long menu did not split into height-limited columns");
	const std::vector<MenuColumn> expected = {
		{0, 3, 43}, {3, 5, 36}, {5, 8, 43}, {8, 10, 36},
	};
	std::size_t nextItem = 0;
	for (std::size_t index = 0; index < columns.size(); ++index) {
		Expect(columns[index].begin == expected[index].begin &&
			columns[index].end == expected[index].end &&
			columns[index].height == expected[index].height,
			"column layout did not preserve sequential item ranges and separator heights");
		Expect(columns[index].begin == nextItem && columns[index].height <= 43,
			"column layout left a gap/overlap or exceeded the available content height");
		nextItem = columns[index].end;
	}
	Expect(nextItem == items.size(), "column layout omitted trailing menu items");

	const std::vector<MenuItem> shortMenu = {{"One", 1}, {nullptr, 0, true}, {"Two", 2}};
	const std::vector<MenuColumn> exactFit =
		jpegview_linux::LayoutMenuColumns(shortMenu, 43, 18, 7);
	Expect(exactFit.size() == 1 && exactFit[0].height == 43,
		"menu that exactly fits the available height was unnecessarily split");
	Expect(jpegview_linux::LayoutMenuColumns(shortMenu, 42, 18, 7).size() == 2,
		"menu overflowing by one pixel did not continue in a second column");
	const std::vector<MenuColumn> emptyLayout =
		jpegview_linux::LayoutMenuColumns({}, 1, 18, 7);
	Expect(emptyLayout.size() == 1 && emptyLayout[0].begin == 0 &&
		emptyLayout[0].end == 0 && emptyLayout[0].height == 0,
		"empty menu did not return a valid empty column");

	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[2], 5, 1) == 7,
		"Down did not skip a separator within its column");
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[2], 7, 1) == 5,
		"Down did not wrap within its column");
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[2], 5, -1) == 7,
		"Up did not wrap backward within its column");
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[1], 3, 1) == 3,
		"vertical navigation did not remain on the only enabled item in a column");
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, {4, 5, 18}, 4, 1) == -1,
		"column containing only a disabled item returned a selection");

	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, 3, 1, 18, 7) == 5,
		"Right did not enter the adjacent column at the closest row");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, 7, -1, 18, 7) == 3,
		"Left did not choose the nearest actionable item in the previous column");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, 0, -1, 18, 7) == -1,
		"Left wrapped past the first column");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, 9, 1, 18, 7) == -1,
		"Right wrapped past the last column");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, -1, 1, 18, 7) == 0,
		"Right with no selection did not start at the first column");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, -1, -1, 18, 7) == 8,
		"Left with no selection did not start at the last column");

	const std::vector<MenuItem> withEmptyColumn = {
		{"Enabled before", 1}, {"Disabled only", 2, false, false, false}, {"Enabled after", 3},
	};
	const std::vector<MenuColumn> sparseColumns = {{0, 1, 18}, {1, 2, 18}, {2, 3, 18}};
	Expect(jpegview_linux::AdjacentMenuSelection(withEmptyColumn, sparseColumns, 0, 1, 18, 7) == 2,
		"horizontal navigation did not skip a column with no actionable items");
}

void TestOverlayLayoutUsesContentWidthAndComfortableMargins() {
	jpegview_linux::OverlayLayout filename = jpegview_linux::FilenameOverlayLayout(60, 800);
	Expect(filename.x == 4 && filename.y == 4 && filename.width == 72 && filename.height == 20,
		"short filename overlay was not content-sized with six-pixel margins");
	Expect(filename.textWidth == 60, "filename overlay text area did not match content width");

	filename = jpegview_linux::FilenameOverlayLayout(900, 800);
	Expect(filename.width == 792 && filename.textWidth == 780,
		"long filename overlay did not clamp to the window insets");

	jpegview_linux::OverlayLayout info =
		jpegview_linux::InformationOverlayLayout(100, 3, 500, 300, false);
	Expect(info.x == 4 && info.y == 4 && info.width == 112 && info.height == 66,
		"EXIF overlay was not content-sized");
	Expect(info.textWidth == 100 && info.visibleLines == 3,
		"EXIF overlay margins or visible line count are incorrect");

	info = jpegview_linux::InformationOverlayLayout(100, 20, 500, 40, true);
	Expect(info.y == 28, "EXIF overlay did not move below visible filename overlay");
	Expect(info.height == 32 && info.visibleLines == 1,
		"EXIF overlay did not clamp vertically to a small window");

	filename = jpegview_linux::FilenameOverlayLayout(20, 4);
	Expect(filename.width == 1 && filename.textWidth == 1,
		"overlay layout did not remain valid for an extremely narrow window");
}

void TestThumbnailPanelLayoutPreloadAndSizing() {
	Expect(jpegview_linux::ThumbnailRowHeight(164, 1) == 112,
		"default thumbnail row height changed");
	Expect(jpegview_linux::ThumbnailRowHeight(48, 1) == 35,
		"narrow thumbnail panel did not create compact rows");
	Expect(jpegview_linux::ThumbnailRowHeight(240, 1) == 163,
		"wide thumbnail panel did not scale its rows with panel width");

	jpegview_linux::ThumbnailPanelLayout layout =
		jpegview_linux::CalculateThumbnailPanelLayout(800, 600, true, 164);
	Expect(layout.panelWidth == 164 && layout.imageX == 164 &&
		layout.imageWidth == 636 && layout.imageHeight == 600,
		"visible thumbnail panel did not reserve image viewport space");
	layout = jpegview_linux::CalculateThumbnailPanelLayout(800, 600, false, 164);
	Expect(layout.panelWidth == 0 && layout.imageX == 0 &&
		layout.imageWidth == 800 && layout.imageHeight == 600,
		"hidden thumbnail panel reduced the image viewport");
	layout = jpegview_linux::CalculateThumbnailPanelLayout(80, 40, true, 164);
	Expect(layout.panelWidth == 79 && layout.imageX == 79 &&
		layout.imageWidth == 1 && layout.imageHeight == 40,
		"thumbnail panel consumed the entire narrow image viewport");

	const std::vector<jpegview_linux::ThumbnailSlot> slots =
		jpegview_linux::ThumbnailPanelSlots(10, 5, 500, 100);
	Expect(slots.size() == 5, "thumbnail panel did not create one row per visible neighbor");
	for (std::size_t index = 0; index < slots.size(); ++index) {
		Expect(slots[index].fileIndex == index + 3 && slots[index].y == static_cast<int>(index) * 100,
			"thumbnail rows do not follow the active file-list order");
	}
	Expect(slots[2].current && slots[2].fileIndex == 5 && slots[2].y == 200,
		"current thumbnail was not centered in the panel");
	Expect(std::count_if(slots.begin(), slots.end(), [](const jpegview_linux::ThumbnailSlot& slot) {
		return slot.current;
	}) == 1, "thumbnail panel marked more than one current image");

	const std::vector<jpegview_linux::ThumbnailSlot> firstSlots =
		jpegview_linux::ThumbnailPanelSlots(4, 0, 300, 100);
	Expect(firstSlots.size() == 2 && firstSlots[0].fileIndex == 0 && firstSlots[0].y == 100 &&
		firstSlots[0].current && firstSlots[1].fileIndex == 1 && firstSlots[1].y == 200,
		"thumbnail panel did not keep the first image centered without wrapping");
	Expect(jpegview_linux::ThumbnailPanelSlots(0, 0, 300, 100).empty(),
		"empty file list produced thumbnail rows");
	Expect(jpegview_linux::ThumbnailPanelSlots(4, 4, 300, 100).empty(),
		"invalid current index produced thumbnail rows");

	const std::vector<std::size_t> preload = jpegview_linux::ThumbnailPreloadOrder(7, 3, 7);
	Expect(preload == std::vector<std::size_t>({3, 2, 4, 1, 5, 0, 6}),
		"thumbnail preload order is not nearest-current-first");
	const std::vector<std::size_t> limited = jpegview_linux::ThumbnailPreloadOrder(20, 10, 4);
	Expect(limited == std::vector<std::size_t>({10, 9, 11, 8}),
		"thumbnail preload order did not honor the memory-cache limit");
	Expect(jpegview_linux::ThumbnailPreloadOrder(4, 0, 8) ==
		std::vector<std::size_t>({0, 1, 2, 3}),
		"thumbnail preload order failed at the first file");

	jpegview_linux::ThumbnailSize size = jpegview_linux::FitThumbnailSize(400, 200, 100, 80);
	Expect(size.width == 100 && size.height == 50,
		"wide thumbnail did not preserve its aspect ratio");
	size = jpegview_linux::FitThumbnailSize(100, 400, 80, 100);
	Expect(size.width == 25 && size.height == 100,
		"tall thumbnail did not preserve its aspect ratio");
	size = jpegview_linux::FitThumbnailSize(40, 20, 100, 80);
	Expect(size.width == 40 && size.height == 20,
		"small thumbnail was enlarged");
	Expect(jpegview_linux::FitThumbnailSize(0, 20, 100, 80).width == 0,
		"invalid image dimensions produced a thumbnail size");

	const jpegview_linux::ThumbnailRect thumbnail =
		jpegview_linux::ThumbnailImageRect(164, 109, 164, 200, 112, 1);
	Expect(thumbnail.x == 0 && thumbnail.y == 201 &&
		thumbnail.width == 164 && thumbnail.height == 109,
		"thumbnail row added horizontal margins or incorrect vertical margins");
}

void TestThumbnailDownsamplingAntialiasing() {
	std::vector<std::uint8_t> checkerboard(8u * 8u * 4u, 255);
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			const std::uint8_t value = (x + y) % 2 == 0 ? 0 : 255;
			const std::size_t offset = (static_cast<std::size_t>(y) * 8 + x) * 4;
			checkerboard[offset] = value;
			checkerboard[offset + 1] = value;
			checkerboard[offset + 2] = value;
		}
	}
	std::vector<std::uint8_t> filtered;
	Expect(jpegview_linux::DownsampleThumbnailBgra(checkerboard, 8, 8, 2, 2, filtered),
		"thumbnail antialiasing rejected valid downsampling dimensions");
	Expect(filtered.size() == 16, "thumbnail antialiasing returned an incorrectly sized image");
	for (std::size_t offset = 0; offset < filtered.size(); offset += 4) {
		Expect(filtered[offset] == 128 && filtered[offset + 1] == 128 &&
			filtered[offset + 2] == 128 && filtered[offset + 3] == 255,
			"high-frequency thumbnail detail was sampled instead of area-filtered");
	}

	const std::vector<std::uint8_t> transparentEdge = {
		0, 0, 255, 255,
		255, 0, 0, 0,
	};
	Expect(jpegview_linux::DownsampleThumbnailBgra(transparentEdge, 2, 1, 1, 1, filtered),
		"thumbnail antialiasing rejected a transparent edge");
	Expect(filtered == std::vector<std::uint8_t>({0, 0, 255, 128}),
		"thumbnail antialiasing introduced a color fringe at a transparent edge");

	Expect(jpegview_linux::DownsampleThumbnailBgra(transparentEdge, 2, 1, 2, 1, filtered) &&
		filtered == transparentEdge, "same-size thumbnails were modified");
	Expect(!jpegview_linux::DownsampleThumbnailBgra(transparentEdge, 2, 1, 3, 1, filtered),
		"thumbnail-only downsampler accepted enlargement");
}

void TestImageInfoFormatting() {
	Expect(jpegview_linux::FormatImageDimensionsAndSize(1920, 1080, "2.5 MB") ==
		"1920 X 1080, 2.5 MB",
		"image dimensions and file size were not compacted into one line");
	Expect(jpegview_linux::FormatImageDimensionsAndSize(640, 480, {}) == "640 X 480",
		"missing file size left punctuation in the dimensions line");
	Expect(jpegview_linux::FormatModificationDateLine("2026-09-19 12:34:56") ==
		"Mod.date: 2026-09-19 12:34:56",
		"modification date label was not shortened");
	Expect(jpegview_linux::FormatFileSize(1536) == "1.5 KB",
		"file-size formatting changed while moving it into the information model");
}

void TestSystemFontResolutionAndUnicodeRendering() {
	TemporaryDirectory temporary;
	const fs::path home = temporary.path() / "home";
	const fs::path config = temporary.path() / "config";
	fs::create_directories(config / "xfce4/xfconf/xfce-perchannel-xml");
	fs::create_directories(config / "gtk-3.0");
	WriteText(config / "gtk-3.0/settings.ini",
		"[Settings]\ngtk-font-name=GTK Choice 11\n");
	WriteText(config / "xfce4/xfconf/xfce-perchannel-xml/xsettings.xml",
		"<channel><property value=\"Tahoma &amp; Friends 12\" type=\"string\" name=\"FontName\"/></channel>\n");
	ScopedEnvironment overrideFont("JPEGVIEW_FONT", "");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Tahoma & Friends 12",
		"XFCE system font was not preferred or its XML value was not decoded");

	fs::remove(config / "xfce4/xfconf/xfce-perchannel-xml/xsettings.xml");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "GTK Choice 11",
		"GTK system font was not used when XFCE settings were absent");
	fs::remove(config / "gtk-3.0/settings.ini");
	fs::create_directories(home);
	WriteText(home / ".gtkrc-2.0", "gtk-font-name = \"Legacy Choice 9\"\n");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Legacy Choice 9",
		"GTK 2 system font was not parsed");

	jpegview_linux::SystemFont font("Sans 10");
	Expect(font.LineHeight() > 7 && font.TextWidth("iiii") < font.TextWidth("WWWW"),
		"system font metrics are missing or not proportional");
	const jpegview_linux::RasterizedText raster = font.Rasterize(u8"Привет — 日本語");
	Expect(raster.width > 0 && raster.height >= font.LineHeight() && !raster.argb.empty(),
		"system font did not rasterize non-Latin UTF-8 text");
	Expect(std::any_of(raster.argb.begin(), raster.argb.end(), [](std::uint32_t pixel) {
		return (pixel >> 24) != 0;
	}), "non-Latin system-font text rasterized as an empty image");
}

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

	const jpegview_linux::DirectorySummary summary =
		jpegview_linux::CountImmediateDirectoryContents(album);
	Expect(summary.imageCount == 2 && summary.subdirectoryCount == 2,
		"directory summary did not count only immediate compatible images and subdirectories");
	Expect(jpegview_linux::FormatDirectorySummary(summary) == "2 images, 2 dirs",
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
		results[0].summary.subdirectoryCount == 2,
		"background directory summary loader did not publish the requested result");

	const fs::path empty = temporary.path() / "empty";
	fs::create_directory(empty);
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
	RunTest("supported-image-extension-policy", TestSupportedImageExtensionPolicy, failures);
	RunTest("keyboard-command-mappings", TestKeyboardCommandMappings, failures);
	RunTest("file-list-date-sorting-and-selection", TestFileListDateSortingAndSelectionPreservation, failures);
	RunTest("file-list-size-and-random-sorting", TestFileListSizeAndRandomSorting, failures);
	RunTest("file-list-navigation-modes-and-reload", TestFileListNavigationModesAndReload, failures);
	RunTest("file-list-multiple-inputs", TestFileListMultipleInputs, failures);
	RunTest("image-writer-decoder-round-trips", TestImageWriterDecoderRoundTrips, failures);
	RunTest("pnm-variants", TestPnmVariants, failures);
	RunTest("animated-image-decoders", TestAnimatedImageDecoders, failures);
	RunTest("decoder-failures", TestDecoderFailures, failures);
	RunTest("settings-round-trip-and-malformed-values", TestSettingsRoundTripAndMalformedValues, failures);
	RunTest("settings-path-selection", TestSettingsPathSelection, failures);
	RunTest("sort-mode-mappings", TestSortModeMappings, failures);
	RunTest("batch-copy-pattern-expansion-and-preview", TestBatchCopyPatternExpansionAndPreview, failures);
	RunTest("desktop-application-parsing-and-expansion", TestDesktopApplicationParsingAndExecExpansion, failures);
	RunTest("exif-and-jpeg-comment-parsing", TestExifAndJpegCommentParsing, failures);
	RunTest("viewport-modes-and-geometry", TestViewportModesAndGeometry, failures);
	RunTest("viewport-manual-zoom-pan-and-restore", TestViewportManualZoomPanAndRestore, failures);
	RunTest("viewport-navigation-resets-transient-zoom", TestViewportNavigationResetsTransientZoom, failures);
	RunTest("resize-model-aspect-ratio-validation-and-filters", TestResizeModelAspectRatioValidationAndFilters, failures);
	RunTest("context-menu-compaction-and-selection", TestContextMenuCompactionAndSelection, failures);
	RunTest("context-menu-column-layout-and-navigation", TestContextMenuColumnLayoutAndNavigation, failures);
	RunTest("overlay-layout-content-width-and-margins", TestOverlayLayoutUsesContentWidthAndComfortableMargins, failures);
	RunTest("thumbnail-panel-layout-preload-and-sizing", TestThumbnailPanelLayoutPreloadAndSizing, failures);
	RunTest("thumbnail-downsampling-antialiasing", TestThumbnailDownsamplingAntialiasing, failures);
	RunTest("image-info-formatting", TestImageInfoFormatting, failures);
	RunTest("system-font-resolution-and-unicode-rendering", TestSystemFontResolutionAndUnicodeRendering, failures);
	RunTest("file-dialog-filtering", TestFileDialogFiltering, failures);
	RunTest("file-dialog-directory-summaries", TestFileDialogDirectorySummaries, failures);
	RunTest("embedded-application-icon", TestEmbeddedApplicationIcon, failures);
	if (failures != 0) {
		std::cerr << failures << " test group(s) failed\n";
		return 1;
	}
	std::cout << "All core tests passed\n";
	return 0;
}
