#include "exif_reader.h"
#include "file_list.h"
#include "file_list_scan_worker.h"
#include "image_decoder.h"
#include "image_cache.h"
#include "display_image_cache.h"
#include "cache_budget.h"
#include "image.h"
#include "image_processing.h"
#include "image_processing_store.h"
#include "image_writer.h"
#include "settings.h"
#include "advanced_configuration_model.h"
#include "sort_mode.h"
#include "desktop_applications.h"
#include "desktop_association.h"
#include "external_commands.h"
#include "batch_copy.h"
#include "crop_selection_model.h"
#include "crop_size_dialog_model.h"
#include "zoom_navigator_model.h"
#include "magnifying_glass_model.h"
#include "image_formats.h"
#include "input_commands.h"
#include "viewport.h"
#include "resize_model.h"
#include "context_menu_model.h"
#include "overlay_layout.h"
#include "viewer_chrome.h"
#include "thumbnail_panel_model.h"
#include "thumbnail_resampler.h"
#include "app_icon.h"
#include "image_info_model.h"
#include "spectrum_model.h"
#include "file_dialog_model.h"
#include "recent_files.h"
#include "system_font.h"
#include "bitmap_font.h"
#include "playback_scheduler.h"
#include "double_page_model.h"
#include "archive_source.h"
#include "rar_backend.h"
#include "seven_zip_backend.h"
#include "archive_password_dialog_model.h"
#include "rar_test_fixtures.h"

#include "../../src/JPEGView/resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <zip.h>

namespace fs = std::filesystem;
using jpegview_linux::DecodedImage;
using jpegview_linux::FileList;
using jpegview_linux::FileListPreparedScan;
using jpegview_linux::FileListScanResult;
using jpegview_linux::FileListScanWorker;
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

std::vector<std::uint8_t> MakeTransparentColorKeyPng() {
	std::vector<std::uint8_t> result = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
	AppendPngChunk(result, "IHDR", {0, 0, 0, 2, 0, 0, 0, 2, 8, 2, 0, 0, 0});
	AppendPngChunk(result, "tRNS", {0, 255, 0, 0, 0, 0});
	const std::vector<std::uint8_t> scanline = {
		0, 255, 0, 0, 0, 255, 0,
		0, 0, 255, 0, 255, 0, 0};
	uLongf compressedSize = compressBound(scanline.size());
	std::vector<std::uint8_t> compressed(compressedSize);
	Expect(compress2(compressed.data(), &compressedSize, scanline.data(), scanline.size(),
		Z_BEST_COMPRESSION) == Z_OK, "could not compress test PNG scanline");
	compressed.resize(compressedSize);
	AppendPngChunk(result, "IDAT", compressed);
	AppendPngChunk(result, "IEND", {});
	return result;
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

void WriteZipArchive(const fs::path& archivePath,
	const std::vector<std::pair<std::string, fs::path>>& members) {
	int errorCode = 0;
	zip_t* archive = zip_open(archivePath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
	if (archive == nullptr) throw TestFailure("cannot create ZIP archive: " + std::to_string(errorCode));
	for (const auto& member : members) {
		zip_source_t* source = zip_source_file(archive, member.second.c_str(), 0, -1);
		if (source == nullptr) {
			const std::string message = zip_strerror(archive);
			zip_discard(archive);
			throw TestFailure("cannot add ZIP fixture source: " + message);
		}
		if (zip_file_add(archive, member.first.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
			const std::string message = zip_strerror(archive);
			zip_source_free(source);
			zip_discard(archive);
			throw TestFailure("cannot add ZIP fixture member: " + message);
		}
	}
	if (zip_close(archive) != 0) {
		const std::string message = zip_strerror(archive);
		zip_discard(archive);
		throw TestFailure("cannot finish ZIP fixture: " + message);
	}
}

void WriteTarArchive(const fs::path& archivePath,
	const std::vector<std::pair<std::string, fs::path>>& members, bool gzip,
	const std::vector<std::pair<std::string, std::string>>& symbolicLinks = {},
	const std::vector<std::pair<std::string, std::string>>& hardLinks = {}) {
	struct archive* writer = archive_write_new();
	if (writer == nullptr) throw TestFailure("cannot allocate TAR fixture writer");
	if (archive_write_set_format_pax_restricted(writer) != ARCHIVE_OK ||
		(gzip && archive_write_add_filter_gzip(writer) != ARCHIVE_OK) ||
		archive_write_open_filename(writer, archivePath.c_str()) != ARCHIVE_OK) {
		const std::string message = archive_error_string(writer) == nullptr ?
			"cannot open TAR fixture" : archive_error_string(writer);
		archive_write_free(writer);
		throw TestFailure(message);
	}
	for (const auto& member : members) {
		const std::vector<std::uint8_t> bytes = ReadBytes(member.second);
		struct archive_entry* entry = archive_entry_new();
		if (entry == nullptr) {
			archive_write_free(writer);
			throw TestFailure("cannot allocate TAR fixture entry");
		}
		archive_entry_set_pathname(entry, member.first.c_str());
		archive_entry_set_filetype(entry, AE_IFREG);
		archive_entry_set_perm(entry, 0644);
		archive_entry_set_size(entry, static_cast<la_int64_t>(bytes.size()));
		archive_entry_set_mtime(entry, 1700000000, 0);
		const int headerResult = archive_write_header(writer, entry);
		archive_entry_free(entry);
		if (headerResult != ARCHIVE_OK) {
			const std::string message = archive_error_string(writer) == nullptr ?
				"cannot write TAR fixture header" : archive_error_string(writer);
			archive_write_free(writer);
			throw TestFailure(message);
		}
		if (!bytes.empty() && archive_write_data(writer, bytes.data(), bytes.size()) !=
			static_cast<la_ssize_t>(bytes.size())) {
			const std::string message = archive_error_string(writer) == nullptr ?
				"cannot write TAR fixture data" : archive_error_string(writer);
			archive_write_free(writer);
			throw TestFailure(message);
		}
		if (archive_write_finish_entry(writer) != ARCHIVE_OK) {
			const std::string message = archive_error_string(writer) == nullptr ?
				"cannot finish TAR fixture entry" : archive_error_string(writer);
			archive_write_free(writer);
			throw TestFailure(message);
		}
	}
	const auto writeLink = [writer](const std::pair<std::string, std::string>& link,
		bool hardLink) {
		struct archive_entry* entry = archive_entry_new();
		if (entry == nullptr) throw TestFailure("cannot allocate TAR link fixture entry");
		archive_entry_set_pathname(entry, link.first.c_str());
		archive_entry_set_filetype(entry, AE_IFREG);
		archive_entry_set_perm(entry, 0644);
		archive_entry_set_size(entry, 0);
		if (hardLink) archive_entry_set_hardlink(entry, link.second.c_str());
		else {
			archive_entry_set_filetype(entry, AE_IFLNK);
			archive_entry_set_symlink(entry, link.second.c_str());
		}
		const int headerResult = archive_write_header(writer, entry);
		archive_entry_free(entry);
		if (headerResult != ARCHIVE_OK || archive_write_finish_entry(writer) != ARCHIVE_OK) {
			const std::string message = archive_error_string(writer) == nullptr ?
				"cannot write TAR link fixture" : archive_error_string(writer);
			archive_write_free(writer);
			throw TestFailure(message);
		}
	};
	for (const auto& link : symbolicLinks) writeLink(link, false);
	for (const auto& link : hardLinks) writeLink(link, true);
	if (archive_write_close(writer) != ARCHIVE_OK) {
		const std::string message = archive_error_string(writer) == nullptr ?
			"cannot finish TAR fixture" : archive_error_string(writer);
		archive_write_free(writer);
		throw TestFailure(message);
	}
	archive_write_free(writer);
}

void Write7zArchive(const fs::path& archivePath,
	const std::vector<std::pair<std::string, fs::path>>& members) {
	struct archive* writer = archive_write_new();
	if (writer == nullptr) throw TestFailure("cannot allocate 7z fixture writer");
	if (archive_write_set_format_7zip(writer) != ARCHIVE_OK ||
		archive_write_open_filename(writer, archivePath.c_str()) != ARCHIVE_OK) {
		const std::string message = archive_error_string(writer) == nullptr ?
			"cannot open 7z fixture" : archive_error_string(writer);
		archive_write_free(writer);
		throw TestFailure(message);
	}
	for (const auto& member : members) {
		const std::vector<std::uint8_t> bytes = ReadBytes(member.second);
		struct archive_entry* entry = archive_entry_new();
		if (entry == nullptr) {
			archive_write_free(writer);
			throw TestFailure("cannot allocate 7z fixture entry");
		}
		archive_entry_set_pathname(entry, member.first.c_str());
		archive_entry_set_filetype(entry, AE_IFREG);
		archive_entry_set_perm(entry, 0644);
		archive_entry_set_size(entry, static_cast<la_int64_t>(bytes.size()));
		archive_entry_set_mtime(entry, 1700000000, 0);
		const int headerResult = archive_write_header(writer, entry);
		archive_entry_free(entry);
		if (headerResult != ARCHIVE_OK) {
			const std::string message = archive_error_string(writer) == nullptr ?
				"cannot write 7z fixture header" : archive_error_string(writer);
			archive_write_free(writer);
			throw TestFailure(message);
		}
		if (!bytes.empty() && archive_write_data(writer, bytes.data(), bytes.size()) !=
			static_cast<la_ssize_t>(bytes.size())) {
			const std::string message = archive_error_string(writer) == nullptr ?
				"cannot write 7z fixture data" : archive_error_string(writer);
			archive_write_free(writer);
			throw TestFailure(message);
		}
		if (archive_write_finish_entry(writer) != ARCHIVE_OK) {
			const std::string message = archive_error_string(writer) == nullptr ?
				"cannot finish 7z fixture entry" : archive_error_string(writer);
			archive_write_free(writer);
			throw TestFailure(message);
		}
	}
	if (archive_write_close(writer) != ARCHIVE_OK) {
		const std::string message = archive_error_string(writer) == nullptr ?
			"cannot finish 7z fixture" : archive_error_string(writer);
		archive_write_free(writer);
		throw TestFailure(message);
	}
	archive_write_free(writer);
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
	jpegview_linux::FileDialogFileSizeLoader archiveSizeLoader;
	archiveSizeLoader.Request({rootImage}, 1);
	std::vector<jpegview_linux::FileDialogFileSizeResult> archiveSizes;
	const auto archiveSizeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (archiveSizes.empty() && std::chrono::steady_clock::now() < archiveSizeDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		archiveSizes = archiveSizeLoader.TakeReady();
	}
	Expect(archiveSizes.size() == 1 && archiveSizes.front().path == rootImage &&
		archiveSizes.front().size == fs::file_size(image),
		"background file-size lookup used the ZIP container size instead of the uncompressed member size");

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

		FileList files({archive.string()}, FileList::SortMode::FileName, true, false);
		Expect(files.Size() == 1 && files.Current() == rootImage && files.IsArchiveMember(0),
			"opening a TAR container did not initialize its root image list");
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
	const fs::path rootImage = archive / "01-root.png";
	const fs::path nestedDirectory = archive / "nested";
	const fs::path nestedImage = nestedDirectory / "02-child.png";

	Expect(jpegview_linux::IsArchiveContainerName(archive) &&
		jpegview_linux::IsArchiveContainerFile(archive) &&
		jpegview_linux::IsArchiveLocation(archive) &&
		jpegview_linux::IsArchiveLocation(nestedDirectory) &&
		jpegview_linux::IsArchiveMemberLocation(rootImage) &&
		jpegview_linux::ArchiveFormatName(nestedImage) == ".7Z" &&
		jpegview_linux::ArchiveBackingFile(nestedImage) == archive,
		"7z path recognition did not preserve the virtual archive-member contract");

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
	modes.mangaReadingOrder = true;
	const auto mangaSpread = jpegview_linux::BuildDoublePageSpread(1, 4, pageOne, pageTwo, modes);
	Expect(mangaSpread.has_value() && mangaSpread->currentPage.x == 600 &&
		mangaSpread->nextPage.x == 0,
		"manga reading order did not swap the spread pages");
	Expect(!jpegview_linux::BuildDoublePageSpread(1, 4, landscape, pageTwo, modes).has_value() &&
		!jpegview_linux::BuildDoublePageSpread(1, 4, pageOne, landscape, modes).has_value(),
		"double-page mode paired a non-portrait page");
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

void TestHeldNavigationCoalescesKeyRepeats() {
	jpegview_linux::HeldNavigationController navigation;
	Expect(navigation.KeyDown(1, 79, false) == 1 && navigation.Scancode() == 79,
		"physical right-arrow press did not request one immediate navigation step");
	Expect(navigation.AfterImageShown(true) == 0,
		"held navigation skipped an image before the initial repeat threshold");
	Expect(navigation.KeyDown(-1, 80, true) == 0 && navigation.Scancode() == 79,
		"stale key-repeat changed the active navigation direction");
	Expect(navigation.AfterImageShown(true) == 0,
		"stale key-repeat enabled continuous navigation");
	for (int repeat = 0; repeat < 100; ++repeat) {
		Expect(navigation.KeyDown(1, 79, true) == 0 && navigation.Scancode() == 79,
			"OS key-repeat queued another navigation step");
	}
	Expect(navigation.AfterImageShown(true) == 1 && navigation.AfterImageShown(true) == 1,
		"held right-arrow did not request one step after each displayed image");
	Expect(navigation.AfterImageShown(false) == 0 && navigation.Scancode() == -1,
		"released right-arrow continued navigating after the displayed image");
	Expect(navigation.KeyDown(-1, 44, false, true) == -1 &&
		navigation.ShiftModifierAllowed(),
		"Shift+Space did not retain its allowed modifier while held");
	Expect(navigation.KeyDown(-1, 44, true, false) == 0 &&
		navigation.AfterImageShown(true) == 0,
		"stale unshifted Space repeat activated held Shift+Space navigation");
	Expect(navigation.KeyDown(-1, 44, true, true) == 0 &&
		navigation.AfterImageShown(true) == -1,
		"held Shift+Space did not repeat one previous-image step");
	Expect(navigation.AfterImageShown(false) == 0 && !navigation.ShiftModifierAllowed(),
		"released Shift+Space kept its modifier allowance");
	Expect(navigation.KeyDown(-1, 80, false) == -1 && navigation.AfterImageShown(true) == 0,
		"new left-arrow press bypassed the initial repeat threshold");
	Expect(navigation.KeyDown(-1, 80, true) == 0 && navigation.AfterImageShown(true) == -1,
		"held left-arrow did not continue after its repeat threshold");
	navigation.Reset();
	Expect(navigation.AfterImageShown(true) == 0 && navigation.Scancode() == -1,
		"reset navigation state retained a pending repeat");
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
}

void ExpectDecoded(const fs::path& filename, const std::vector<std::uint8_t>& expected,
	bool exactRgb, bool exactAlpha, bool expectedTransparency = false) {
	DecodedImage decoded;
	std::string error;
	Expect(jpegview_linux::DecodeImage(filename, decoded, error),
		"cannot decode " + filename.extension().string() + ": " + error);
	Expect(decoded.frames.size() == 1, "static output did not decode to one frame");
	Expect(!decoded.animation, "static output was incorrectly marked animated");
	Expect(decoded.frames.front().width == 2 && decoded.frames.front().height == 2,
		"decoded dimensions are incorrect for " + filename.extension().string());
	Expect(decoded.frames.front().hasTransparency == expectedTransparency,
		"transparency metadata is incorrect for " + filename.extension().string());
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
		bool hasTransparency;
	};
	const std::vector<FormatCase> formats = {
		{".png", true, true, true},
		{".bmp", true, false, true},
		{".tga", true, true, true},
		{".ppm", true, false, false},
		{".qoi", true, true, true},
		{".psd", true, false, false},
		{".jpg", false, false, false},
	};
	for (const FormatCase& format : formats) {
		const fs::path filename = temporary.path() / ("roundtrip" + std::string(format.extension));
		std::string error;
		Expect(jpegview_linux::WriteImage(filename, pixels.data(), 2, 2, options, error),
			"cannot write " + filename.extension().string() + ": " + error);
		ExpectDecoded(filename, pixels, format.exactRgb, format.exactAlpha,
			format.hasTransparency);
		if (std::string(format.extension) == ".jpg") {
			int mcuWidth = 0;
			int mcuHeight = 0;
			Expect(jpegview_linux::ReadJpegMcuSize(filename, mcuWidth, mcuHeight, error) &&
				mcuWidth >= 8 && mcuHeight >= 8 && mcuWidth % 8 == 0 && mcuHeight % 8 == 0,
				"JPEG MCU dimensions were not read from the sampling factors");
			Expect(!jpegview_linux::ReadJpegMcuSize(temporary.path() / "missing.jpg",
				mcuWidth, mcuHeight, error), "JPEG MCU reader accepted a missing file");
			const fs::path malformed = temporary.path() / "malformed.jpg";
			WriteText(malformed, "not a JPEG image");
			Expect(!jpegview_linux::ReadJpegMcuSize(malformed, mcuWidth, mcuHeight, error),
				"JPEG MCU reader accepted a malformed file");
		}
	}
	std::vector<std::uint8_t> opaquePixels = pixels;
	for (std::size_t alpha = 3; alpha < opaquePixels.size(); alpha += 4) opaquePixels[alpha] = 255;
	const fs::path opaquePng = temporary.path() / "opaque.png";
	std::string opaqueError;
	Expect(jpegview_linux::WriteImage(opaquePng, opaquePixels.data(), 2, 2, options, opaqueError),
		"cannot write opaque PNG: " + opaqueError);
	ExpectDecoded(opaquePng, opaquePixels, true, true, false);
	const fs::path colorKeyPng = temporary.path() / "color-key.png";
	WriteBytes(colorKeyPng, MakeTransparentColorKeyPng());
	ExpectDecoded(colorKeyPng,
		{0, 0, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 0},
		true, true, true);

	const fs::path uppercase = temporary.path() / "uppercase.PNG";
	std::string error;
	Expect(jpegview_linux::WriteImage(uppercase, pixels.data(), 2, 2, options, error),
		"uppercase extension was not accepted by the writer: " + error);
	ExpectDecoded(uppercase, pixels, true, true, true);

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

	const fs::path truncatedJpeg = temporary.path() / "truncated.jpg";
	WriteBytes(truncatedJpeg, {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F'});
	error.clear();
	Expect(!jpegview_linux::DecodeImage(truncatedJpeg, decoded, error) && !error.empty(),
		"truncated native JPEG decode did not fail cleanly");
}

void TestJpegDisplayDecodeScaling() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "large-preview.jpg";
	constexpr int width = 64;
	constexpr int height = 48;
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
			pixels[offset] = static_cast<std::uint8_t>(x * 3);
			pixels[offset + 1] = static_cast<std::uint8_t>(y * 4);
			pixels[offset + 2] = static_cast<std::uint8_t>(x + y);
			pixels[offset + 3] = 255;
		}
	}
	ImageWriteOptions options;
	options.jpegQuality = 95;
	std::string error;
	Expect(jpegview_linux::WriteImage(filename, pixels.data(), width, height, options, error),
		"cannot create scaled JPEG fixture: " + error);
	Expect(jpegview_linux::IsJpegPath(filename) &&
		jpegview_linux::IsJpegPath(temporary.path() / "PHOTO.JPEG") &&
		!jpegview_linux::IsJpegPath(temporary.path() / "photo.png"),
		"JPEG display-path extension policy is incorrect");

	int headerWidth = 0;
	int headerHeight = 0;
	Expect(jpegview_linux::ReadJpegDimensions(filename, headerWidth, headerHeight, error) &&
		headerWidth == width && headerHeight == height,
		"JPEG header probe returned incorrect source dimensions");
	DecodedImage scaled;
	int sourceWidth = 0;
	int sourceHeight = 0;
	Expect(jpegview_linux::DecodeJpegForDisplay(filename, 9, 7, scaled,
		sourceWidth, sourceHeight, error),
		"scaled JPEG display decode failed: " + error);
	Expect(sourceWidth == width && sourceHeight == height && scaled.frames.size() == 1,
		"scaled JPEG decode lost its full source dimensions");
	Expect(scaled.frames[0].width >= 9 && scaled.frames[0].height >= 7 &&
		scaled.frames[0].width < width && scaled.frames[0].height < height,
		"JPEG display decode did not select a reduced DCT size above the target");

	const jpegview_linux::DisplayImageRequest request =
		jpegview_linux::MakeJpegDisplayImageRequest(filename, width, height,
			10, 8, false, 1);
	Expect(request.Valid() && !request.decoded,
		"file-backed JPEG display request was not valid without full decoded pixels");
	jpegview_linux::DisplayImageCache display(4096, 1);
	const jpegview_linux::DisplayImageCache::ImagePtr prepared =
		display.RequestAndWait(request);
	Expect(prepared && prepared->filename == filename &&
		prepared->width == 10 && prepared->height == 8 &&
		prepared->bgra.size() == 10u * 8u * 4u,
		"file-backed JPEG preparation did not produce exact display-size pixels");
}

std::shared_ptr<DecodedImage> CachedTestImage(std::size_t bytes) {
	auto image = std::make_shared<DecodedImage>();
	jpegview_linux::DecodedFrame frame;
	frame.width = static_cast<int>(bytes / 4);
	frame.height = 1;
	frame.bgra.assign(bytes, 127);
	image->frames.push_back(std::move(frame));
	return image;
}

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

std::shared_ptr<DecodedImage> DisplayCacheTestImage(int width, int height) {
	auto image = std::make_shared<DecodedImage>();
	jpegview_linux::DecodedFrame frame;
	frame.width = width;
	frame.height = height;
	frame.bgra.resize(static_cast<std::size_t>(width) * height * 4);
	for (std::size_t offset = 0; offset < frame.bgra.size(); offset += 4) {
		frame.bgra[offset] = static_cast<std::uint8_t>(offset / 4);
		frame.bgra[offset + 1] = 70;
		frame.bgra[offset + 2] = 180;
		frame.bgra[offset + 3] = 255;
	}
	image->frames.push_back(std::move(frame));
	return image;
}

void TestDisplayImageCacheBackgroundPreparation() {
	Expect(jpegview_linux::DisplayPrefetchCount(1024, 10, 10, 100) == 1,
		"display prefetch sizing did not reserve one texture slot for the current image");
	Expect(jpegview_linux::DisplayPrefetchCount(40000, 10, 10, 100) == 99,
		"display prefetch sizing did not use the available texture budget");
	Expect(jpegview_linux::DisplayPrefetchCount(1024 * 1024, 1, 1, 10000) == 512,
		"display prefetch sizing did not enforce its speculative work cap");
	Expect(jpegview_linux::DisplayPrefetchCount(399, 10, 10, 5) == 0 &&
		jpegview_linux::DisplayPrefetchCount(1024, 0, 10, 5) == 0,
		"display prefetch sizing accepted an unusable cache or viewport");

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
	const auto secondScaled = jpegview_linux::MakeDisplayImageRequest(
		secondFile, decoded, 0, 2, 2, false, 1);
	const auto thirdScaled = jpegview_linux::MakeDisplayImageRequest(
		thirdFile, decoded, 0, 2, 2, false, 2);
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
	Expect(pairedWorkersStartedTogether &&
		pairedPreparation.WaitUntilIdle(std::chrono::seconds(2)) &&
		pairedPreparation.TakeCompleted(3).size() == 3 &&
		pairedPreparation.HasPendingOrCached(scaled.key),
		"paired display requests were not made available to workers together with later neighbors");
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
	inactiveLevels.unsharpAmount = 1.0;
	const auto activeUnsharpRequest = jpegview_linux::MakeDisplayImageRequest(
		firstFile, decoded, 0, 2, 2, false, 0, inactiveLevels);
	Expect(activeUnsharpRequest.key != scaled.key,
		"display cache key omitted an enabled unsharp-mask adjustment");

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
	ordered.TakeCompleted(10);

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
	Expect(ordered.Find(requests[0]) == nullptr,
		"display cache served prepared pixels after the source file changed");
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

void TestSharedCacheBudgetAccounting() {
	jpegview_linux::SharedCacheBudget budget(32);
	Expect(budget.Capacity() == 32 && budget.Used() == 0 && budget.Available() == 32,
		"shared cache budget did not expose its initial capacity");
	Expect(budget.TryReserve(20) && budget.Used() == 20 && budget.Available() == 12,
		"shared cache budget did not account for a reservation");
	Expect(!budget.TryReserve(13) && budget.Used() == 20,
		"shared cache budget exceeded its total capacity");
	Expect(budget.TryReserve(12) && budget.Used() == 32 && budget.Available() == 0,
		"shared cache budget rejected its exact remaining capacity");
	budget.Release(7);
	Expect(budget.Used() == 25 && budget.Available() == 7,
		"shared cache budget did not release retained bytes");
	budget.SetCapacity(16);
	Expect(budget.Capacity() == 16 && budget.Used() == 25 && budget.Available() == 0 &&
		!budget.TryReserve(1),
		"lowered shared cache capacity incorrectly discarded or admitted reservations");
	budget.Release(100);
	Expect(budget.Used() == 0 && budget.Available() == 16,
		"shared cache budget underflowed while releasing bytes");
	Expect(jpegview_linux::CacheBytesFromMiB(2) == 2u * 1024u * 1024u,
		"cache MiB conversion returned the wrong byte count");

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
	displayCache.Release(displayRequest.key);
	Expect(shared->Used() == 16, "display eviction did not return bytes to the shared cache budget");
	Expect(shared->TryReserve(16) && shared->Used() == 32,
		"display staging bytes could not be transferred to a texture reservation");
	shared->Release(16);
	decodedCache.Clear();
	Expect(shared->Used() == 0, "decoded eviction did not return bytes to the shared cache budget");
}

jpegview_linux::Image MakeIndexedImage(int width, int height) {
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
	for (int index = 0; index < width * height; ++index) {
		const std::size_t offset = static_cast<std::size_t>(index) * 4;
		const std::uint8_t value = static_cast<std::uint8_t>(index + 1);
		pixels[offset] = value;
		pixels[offset + 1] = static_cast<std::uint8_t>(value + 20);
		pixels[offset + 2] = static_cast<std::uint8_t>(value + 40);
		pixels[offset + 3] = static_cast<std::uint8_t>(value + 80);
	}
	jpegview_linux::Image image;
	Expect(image.StoreBGRA(pixels.data(), width, height), "could not create indexed image fixture");
	return image;
}

std::vector<std::uint8_t> ImageBlueChannel(const jpegview_linux::Image& image) {
	std::vector<std::uint8_t> values;
	for (std::size_t offset = 0; offset < image.bgra.size(); offset += 4) {
		values.push_back(image.bgra[offset]);
	}
	return values;
}

void TestImageStorageTransformsAndValidation() {
	jpegview_linux::Image empty;
	Expect(!empty.StoreBGRA(nullptr, 1, 1) && !empty.StoreBGRA(nullptr, 0, 0),
		"image storage accepted null or empty pixel input");
	const std::uint8_t onePixel[4] = {1, 2, 3, 4};
	Expect(!empty.StoreBGRA(onePixel, 65535, 65535),
		"image storage accepted dimensions above the pixel safety limit");
	jpegview_linux::Image transparent;
	Expect(transparent.StoreBGRA(onePixel, 1, 1, true) && transparent.hasTransparency,
		"image storage discarded its transparency metadata");
	jpegview_linux::Image transparentCrop;
	Expect(transparent.CopyCrop(0, 0, 1, 1, transparentCrop) && transparentCrop.hasTransparency,
		"cropping discarded its transparency metadata");

	const jpegview_linux::Image source = MakeIndexedImage(2, 3);
	jpegview_linux::Image transformed = source;
	Expect(transformed.Rotate(true) && transformed.width == 3 && transformed.height == 2 &&
		ImageBlueChannel(transformed) == std::vector<std::uint8_t>({5, 3, 1, 6, 4, 2}),
		"clockwise image rotation changed pixel orientation");
	Expect(transformed.originalWidth == 2 && transformed.originalHeight == 3,
		"image rotation changed source dimensions");

	transformed = source;
	Expect(transformed.Rotate(false) &&
		ImageBlueChannel(transformed) == std::vector<std::uint8_t>({2, 4, 6, 1, 3, 5}),
		"counter-clockwise image rotation changed pixel orientation");
	transformed = source;
	Expect(transformed.Mirror(true) &&
		ImageBlueChannel(transformed) == std::vector<std::uint8_t>({2, 1, 4, 3, 6, 5}),
		"horizontal image mirror changed pixel orientation");
	transformed = source;
	Expect(transformed.Mirror(false) &&
		ImageBlueChannel(transformed) == std::vector<std::uint8_t>({5, 6, 3, 4, 1, 2}),
		"vertical image mirror changed pixel orientation");

	jpegview_linux::Image malformed;
	malformed.width = 2;
	malformed.height = 2;
	malformed.bgra = {1, 2, 3, 4};
	Expect(!malformed.Rotate(true) && !malformed.Mirror(true) &&
		!malformed.Resize(1, 1) && !malformed.Crop(0, 0, 1, 1) && !malformed.AutoContrast(),
		"image operations accepted a truncated pixel buffer");
}

void TestImageCropCopiesHalfOpenRectangle() {
	jpegview_linux::Image image = MakeIndexedImage(3, 2);
	const std::vector<std::uint8_t> originalPixels = image.bgra;
	jpegview_linux::Image copied;
	Expect(image.CopyCrop(1, 0, 3, 2, copied) && copied.width == 2 && copied.height == 2 &&
		copied.originalWidth == 3 && copied.originalHeight == 2 &&
		ImageBlueChannel(copied) == std::vector<std::uint8_t>({2, 3, 5, 6}) &&
		image.bgra == originalPixels,
		"copy-crop did not extract the half-open rectangle while retaining its source");
	jpegview_linux::Image preserved = MakeIndexedImage(1, 1);
	const std::vector<std::uint8_t> preservedPixels = preserved.bgra;
	Expect(!image.CopyCrop(1, 0, 4, 2, preserved) && preserved.width == 1 &&
		preserved.height == 1 && preserved.bgra == preservedPixels,
		"invalid copy-crop changed its output image");
	Expect(image.CopyCrop(1, 0, 3, 2, image) && image.width == 2 && image.height == 2 &&
		image.originalWidth == 3 && image.originalHeight == 2 &&
		ImageBlueChannel(image) == std::vector<std::uint8_t>({2, 3, 5, 6}),
		"copy-crop could not safely replace its source image");
	image = MakeIndexedImage(3, 2);
	Expect(image.Crop(1, 0, 3, 2), "valid image crop was rejected");
	Expect(image.width == 2 && image.height == 2 &&
		ImageBlueChannel(image) == std::vector<std::uint8_t>({2, 3, 5, 6}),
		"crop did not copy the requested half-open pixel rectangle");
	Expect(image.originalWidth == 3 && image.originalHeight == 2,
		"crop unexpectedly replaced the dimensions retained from the source image");
	const std::vector<std::uint8_t> validPixels = image.bgra;
	for (const auto& bounds : std::vector<std::array<int, 4>>{
		{{-1, 0, 1, 1}}, {{0, -1, 1, 1}}, {{0, 0, 4, 1}},
		{{0, 0, 1, 3}}, {{2, 0, 1, 1}}, {{0, 1, 1, 1}}}) {
		Expect(!image.Crop(bounds[0], bounds[1], bounds[2], bounds[3]) && image.bgra == validPixels,
			"invalid image crop was accepted or partially changed pixels");
	}
	Expect(image.Crop(0, 0, 2, 2) && image.width == 2 && image.height == 2 &&
		ImageBlueChannel(image) == std::vector<std::uint8_t>({2, 3, 5, 6}),
		"crop of the full current image changed its pixel content");
}

void TestCropSelectionModelGeometryAndManipulation() {
	using jpegview_linux::CropSelectionHandle;
	using jpegview_linux::CropSelectionMode;
	using jpegview_linux::CropSelectionModel;
	using jpegview_linux::SelectionRect;
	Expect(!jpegview_linux::ShouldStartNewCropSelection(false, false, false) &&
		!jpegview_linux::ShouldStartNewCropSelection(false, false, true) &&
		jpegview_linux::ShouldStartNewCropSelection(true, false, false) &&
		!jpegview_linux::ShouldStartNewCropSelection(true, false, true) &&
		jpegview_linux::ShouldStartNewCropSelection(false, true, true),
		"crop-selection drag gating did not keep selection off by default or preserve modifier override");
	CropSelectionModel selection;
	selection.SetImageSize(100, 80);
	Expect(selection.StartNew(2, 1), "selection could not start on a valid image");
	Expect(selection.Update(5, 3), "drag did not update its selection rectangle");
	selection.End();
	Expect(selection.HasSelection() && selection.Rect().left == 2 && selection.Rect().top == 1 &&
		selection.Rect().right == 6 && selection.Rect().bottom == 4,
		"forward selection drag did not produce half-open, inclusive-pixel bounds");

	selection.StartNew(5, 3);
	selection.Update(2, 1);
	selection.End();
	Expect(selection.Rect().left == 2 && selection.Rect().top == 1 &&
		selection.Rect().right == 6 && selection.Rect().bottom == 4,
		"reverse-direction selection drag changed normalized bounds");

	selection.StartNew(99, 79);
	selection.Update(1000, 1000);
	selection.End();
	Expect(selection.Rect().left == 99 && selection.Rect().top == 79 &&
		selection.Rect().right == 100 && selection.Rect().bottom == 80,
		"selection at the bottom-right image boundary exceeded the source dimensions");

	selection.SetImageSize(200, 120);
	selection.SetAspectRatio(16, 9);
	selection.StartNew(10, 10);
	selection.Update(90, 60);
	selection.End();
	const SelectionRect widescreen = selection.Rect();
	ExpectNear(static_cast<double>(widescreen.Width()) / widescreen.Height(), 16.0 / 9.0,
		0.03, "fixed-aspect creation did not preserve the requested ratio");
	Expect(widescreen.left >= 0 && widescreen.top >= 0 && widescreen.right <= 200 &&
		widescreen.bottom <= 120, "fixed-aspect selection escaped image bounds");

	selection.SetImageSize(400, 200);
	selection.SetMode(CropSelectionMode::ImageAspect);
	selection.StartNew(10, 10);
	selection.Update(70, 45);
	selection.End();
	ExpectNear(static_cast<double>(selection.Rect().Width()) / selection.Rect().Height(), 2.0,
		0.04, "same-as-image mode did not use the current image aspect ratio");

	selection.SetFixedSize(320, 200, true);
	selection.StartNew(4, 7);
	selection.Update(20, 20, 2.0);
	selection.End();
	Expect(selection.Rect().left == 20 && selection.Rect().top == 20 &&
		selection.Rect().Width() == 160 && selection.Rect().Height() == 100,
		"screen-pixel fixed size did not scale back to source-image pixels");
	selection.SetFixedSize(80, 45, false);
	selection.StartNew(4, 7);
	selection.Update(20, 20, 3.0);
	selection.End();
	Expect(selection.Rect().left == 20 && selection.Rect().top == 20 &&
		selection.Rect().Width() == 80 && selection.Rect().Height() == 45,
		"image-pixel fixed size incorrectly depended on viewport zoom");
	selection.SetFixedSize(60, 30, true);
	selection.StartNew(15, 15);
	selection.Update(25, 30, 2.0);
	selection.End();
	Expect(selection.Rect().left == 25 && selection.Rect().top == 30 &&
		selection.Rect().Width() == 30 && selection.Rect().Height() == 15,
		"fixed screen-pixel selection did not follow the pointer at the current zoom");

	selection.SetImageSize(50, 40);
	selection.SetMode(CropSelectionMode::Free);
	selection.StartNew(10, 10);
	selection.Update(29, 29);
	selection.End();
	Expect(selection.Rect().Width() == 20 && selection.Rect().Height() == 20,
		"selection setup for manipulation failed");
	Expect(selection.StartManipulation(15, 15, CropSelectionHandle::Move),
		"selection move could not start");
	selection.Update(20, 22);
	selection.End();
	Expect(selection.Rect().left == 15 && selection.Rect().top == 17 &&
		selection.Rect().right == 35 && selection.Rect().bottom == 37,
		"moving an existing selection did not preserve its size and pointer offset");
	selection.StartManipulation(20, 27, CropSelectionHandle::Move);
	selection.Update(1000, 1000);
	selection.End();
	Expect(selection.Rect().right == 50 && selection.Rect().bottom == 40,
		"moving a selection past the image edge did not clamp it inside the image");

	selection.SetImageSize(50, 50);
	selection.SetMode(CropSelectionMode::Free);
	selection.Clear();
	selection.StartNew(10, 10);
	selection.Update(29, 29);
	selection.End();
	selection.StartManipulation(10, 20, CropSelectionHandle::Left);
	selection.Update(5, 20);
	selection.End();
	Expect(selection.Rect().left == 5 && selection.Rect().right == 30,
		"left-edge resize did not preserve the opposite edge");
	selection.SetAspectRatio(1, 1);
	selection.StartManipulation(selection.Rect().right - 1, selection.Rect().bottom - 1,
		CropSelectionHandle::BottomRight);
	selection.Update(selection.Rect().right + 5, selection.Rect().bottom + 2);
	selection.End();
	ExpectNear(static_cast<double>(selection.Rect().Width()) / selection.Rect().Height(), 1.0,
		0.03, "fixed-aspect corner resize did not retain its ratio");
	selection.SetImageSize(200, 200);
	selection.SetMode(CropSelectionMode::Free);
	selection.StartNew(20, 30);
	selection.Update(49, 49);
	selection.End();
	const SelectionRect beforeAspectChange = selection.Rect();
	selection.SetAspectRatio(16, 9);
	Expect(selection.ReapplyMode() && selection.Rect().left == beforeAspectChange.left &&
		selection.Rect().top == beforeAspectChange.top,
		"changing aspect mode did not retain the selection's top-left anchor");
	ExpectNear(static_cast<double>(selection.Rect().Width()) / selection.Rect().Height(), 16.0 / 9.0,
		0.03, "changing aspect mode did not immediately resize the selection");
	selection.SetFixedSize(12, 8, false);
	Expect(selection.ReapplyMode() && selection.Rect().left == beforeAspectChange.left &&
		selection.Rect().top == beforeAspectChange.top && selection.Rect().Width() == 12 &&
		selection.Rect().Height() == 8,
		"applying fixed-size mode did not immediately resize the selection");
	selection.SetMode(CropSelectionMode::Free);
	const SelectionRect beforeFreeMode = selection.Rect();
	Expect(!selection.ReapplyMode() && selection.Rect().left == beforeFreeMode.left &&
		selection.Rect().top == beforeFreeMode.top && selection.Rect().right == beforeFreeMode.right &&
		selection.Rect().bottom == beforeFreeMode.bottom,
		"free mode unexpectedly changed the selection bounds");
	selection.SetImageSize(65535, 65535);
	selection.SetAspectRatio(65535, 1);
	selection.StartNew(0, 0);
	selection.Update(65534, 65534);
	selection.End();
	Expect(selection.Rect().Width() == 65535 && selection.Rect().Height() == 1 &&
		selection.Rect().right <= selection.ImageWidth() &&
		selection.Rect().bottom <= selection.ImageHeight(),
		"extreme aspect ratio overflowed or failed to fit within image boundaries");

	selection.SetImageSize(0, 0);
	Expect(!selection.StartNew(0, 0) && !selection.HasSelection(),
		"selection accepted an empty source image");
}

void TestCropSelectionViewMappingAndHitTesting() {
	using jpegview_linux::CropSelectionHandle;
	using jpegview_linux::CropSelectionModel;
	using jpegview_linux::SelectionRect;
	using jpegview_linux::SelectionScreenRect;
	const SelectionScreenRect imageDestination{100, 50, 200, 100};
	const SelectionScreenRect mapped = CropSelectionModel::ToScreen(
		SelectionRect{20, 10, 100, 60}, imageDestination, 400, 200);
	Expect(mapped.x == 110 && mapped.y == 55 && mapped.width == 40 && mapped.height == 25,
		"selection screen rectangle did not follow the image scale and offset");
	const auto point = CropSelectionModel::ScreenToImage(150, 75, imageDestination, 400, 200);
	Expect(point.x == 100 && point.y == 50,
		"screen-to-image mapping did not invert the destination scale");
	const auto clipped = CropSelectionModel::ScreenToImage(-500, 900,
		imageDestination, 400, 200);
	Expect(clipped.x == 0 && clipped.y == 199,
		"screen-to-image mapping did not clamp points outside the rendered image");

	const SelectionScreenRect selected{10, 20, 100, 60};
	Expect(CropSelectionModel::HitTest(10, 20, selected, false) == CropSelectionHandle::TopLeft &&
		CropSelectionModel::HitTest(60, 20, selected, false) == CropSelectionHandle::Top &&
		CropSelectionModel::HitTest(50, 45, selected, false) == CropSelectionHandle::Move &&
		CropSelectionModel::HitTest(150, 100, selected, false) == CropSelectionHandle::None,
		"selection handle and interior hit-testing returned incorrect actions");
	Expect(CropSelectionModel::HitTest(10, 20, selected, true) == CropSelectionHandle::Move,
		"fixed-size mode exposed a resize handle instead of move-only behavior");
	const SelectionRect aligned = CropSelectionModel::AlignToMcu(
		SelectionRect{10, 10, 61, 51}, 100, 80, 16, 16);
	Expect(aligned.left == 0 && aligned.top == 0 && aligned.right == 64 && aligned.bottom == 64,
		"lossless crop did not expand bounds to MCU edges");
	const SelectionRect edgeAligned = CropSelectionModel::AlignToMcu(
		SelectionRect{80, 64, 100, 80}, 100, 80, 16, 16);
	Expect(edgeAligned.left == 80 && edgeAligned.top == 64 &&
		edgeAligned.right == 96 && edgeAligned.bottom == 80,
		"lossless crop did not trim a partial right-edge MCU like the Windows implementation");
	Expect(!CropSelectionModel::AlignToMcu(SelectionRect{96, 0, 100, 8},
		100, 80, 16, 8).Valid(), "lossless crop accepted a rectangle trimmed to zero width");
}

void TestImageResizeFiltersAndLimits() {
	jpegview_linux::Image source = MakeIndexedImage(4, 1);
	jpegview_linux::Image point = source;
	Expect(point.Resize(2, 1, 0) && ImageBlueChannel(point) == std::vector<std::uint8_t>({1, 3}),
		"point downsampling did not map destination pixels to expected source pixels");
	jpegview_linux::Image enlarged = MakeIndexedImage(2, 1);
	Expect(enlarged.Resize(3, 1, 0) &&
		ImageBlueChannel(enlarged) == std::vector<std::uint8_t>({1, 1, 2}),
		"point enlargement did not preserve source endpoints");
	jpegview_linux::Image clampedFilter = source;
	Expect(clampedFilter.Resize(2, 1, -50) && clampedFilter.bgra == point.bgra,
		"resize did not clamp a low filter index to point sampling");
	jpegview_linux::Image filteredEnlargement = MakeIndexedImage(2, 2);
	const jpegview_linux::Image enlargementSource = filteredEnlargement;
	Expect(filteredEnlargement.Resize(5, 4, 1) &&
		std::equal(filteredEnlargement.bgra.begin(), filteredEnlargement.bgra.begin() + 4,
			enlargementSource.bgra.begin()) &&
		std::equal(filteredEnlargement.bgra.end() - 4, filteredEnlargement.bgra.end(),
			enlargementSource.bgra.end() - 4),
		"bicubic enlargement did not preserve the first and last source pixels");
	jpegview_linux::Image singlePixel = MakeIndexedImage(1, 1);
	const std::vector<std::uint8_t> singleColor = singlePixel.bgra;
	Expect(singlePixel.Resize(4, 3, 3), "filtered enlargement rejected a one-pixel source");
	for (std::size_t offset = 0; offset < singlePixel.bgra.size(); offset += 4) {
		Expect(std::equal(singlePixel.bgra.begin() + static_cast<std::ptrdiff_t>(offset),
			singlePixel.bgra.begin() + static_cast<std::ptrdiff_t>(offset + 4), singleColor.begin()),
			"one-pixel enlargement did not retain its constant BGRA value");
	}
	jpegview_linux::Image unchanged = source;
	Expect(unchanged.Resize(4, 1, 3) && unchanged.bgra == source.bgra,
		"same-size resize modified source pixels");
	jpegview_linux::Image highFilter = source;
	jpegview_linux::Image defaultFilter = source;
	Expect(highFilter.Resize(3, 1, 999) && defaultFilter.Resize(3, 1, 3) &&
		highFilter.bgra == defaultFilter.bgra,
		"resize did not clamp a high filter index to sharpen-medium");

	std::vector<std::uint8_t> constantPixels(8u * 8u * 4u);
	for (std::size_t offset = 0; offset < constantPixels.size(); offset += 4) {
		constantPixels[offset] = 25;
		constantPixels[offset + 1] = 75;
		constantPixels[offset + 2] = 125;
		constantPixels[offset + 3] = 175;
	}
	jpegview_linux::Image constant;
	Expect(constant.StoreBGRA(constantPixels.data(), 8, 8), "could not create constant resize fixture");
	for (int filter = 1; filter < 4; ++filter) {
		jpegview_linux::Image resized = constant;
		Expect(resized.Resize(3, 3, filter) && resized.width == 3 && resized.height == 3,
			"filtered resize rejected valid dimensions");
		for (std::size_t offset = 0; offset < resized.bgra.size(); offset += 4) {
			Expect(resized.bgra[offset] == 25 && resized.bgra[offset + 1] == 75 &&
				resized.bgra[offset + 2] == 125 && resized.bgra[offset + 3] == 175,
				"normalized resize kernel changed a constant color or alpha value");
		}
	}

	jpegview_linux::Image multipass = MakeIndexedImage(30, 2);
	Expect(multipass.Resize(3, 1, 3) && multipass.width == 3 && multipass.height == 1 &&
		multipass.originalWidth == 30 && multipass.originalHeight == 2,
		"large reduction did not complete through the multi-pass resize path");
	const jpegview_linux::Image beforeFailure = multipass;
	Expect(!multipass.Resize(0, 1) && !multipass.Resize(65535, 65535) &&
		multipass.width == beforeFailure.width && multipass.height == beforeFailure.height &&
		multipass.bgra == beforeFailure.bgra,
		"invalid resize dimensions modified the image");
}

void TestImageAutoContrastInvariants() {
	const std::vector<std::uint8_t> pixels = {
		20, 40, 60, 17, 80, 100, 120, 18,
		140, 160, 180, 19, 200, 220, 240, 20,
	};
	jpegview_linux::Image image;
	Expect(image.StoreBGRA(pixels.data(), 2, 2), "could not create auto-contrast fixture");
	Expect(image.AutoContrast(), "auto contrast rejected a valid image");
	Expect(image.width == 2 && image.height == 2 && image.originalWidth == 2 && image.originalHeight == 2,
		"auto contrast changed image dimensions");
	bool colorChanged = false;
	for (std::size_t offset = 0; offset < image.bgra.size(); offset += 4) {
		colorChanged = colorChanged || !std::equal(image.bgra.begin() + static_cast<std::ptrdiff_t>(offset),
			image.bgra.begin() + static_cast<std::ptrdiff_t>(offset + 3),
			pixels.begin() + static_cast<std::ptrdiff_t>(offset));
		Expect(image.bgra[offset + 3] == pixels[offset + 3],
			"auto contrast modified straight-alpha values");
	}
	Expect(colorChanged, "auto contrast left a non-uniform low-range fixture unchanged");
}

void TestPictureLevelsModelAndProcessing() {
	using jpegview_linux::ImageProcessingParams;
	using jpegview_linux::LevelControl;
	ImageProcessingParams params;
	Expect(jpegview_linux::IsDefaultImageProcessing(params), "picture-level defaults are not identity values");
	Expect(static_cast<std::size_t>(LevelControl::Count) == 12,
		"not all Windows picture-level sliders are represented");
	Expect(jpegview_linux::GetLevelControlInfo(LevelControl::ColorCorrection).enabledByAutoContrast &&
		jpegview_linux::GetLevelControlInfo(LevelControl::ContrastCorrection).enabledByAutoContrast,
		"automatic-correction refinement sliders were not tied to auto-contrast state");
	for (std::size_t index = 0; index < static_cast<std::size_t>(LevelControl::Count); ++index) {
		const LevelControl control = static_cast<LevelControl>(index);
		const auto& info = jpegview_linux::GetLevelControlInfo(control);
		jpegview_linux::SetLevelControlValue(params, control, info.minimum - 10.0);
		ExpectNear(jpegview_linux::GetLevelControlValue(params, control), info.minimum, 1e-12,
			"picture-level minimum was not clamped");
		jpegview_linux::SetLevelControlValue(params, control, info.maximum + 10.0);
		ExpectNear(jpegview_linux::GetLevelControlValue(params, control), info.maximum, 1e-12,
			"picture-level maximum was not clamped");
		jpegview_linux::SetLevelControlValue(params, control, info.defaultValue);
	}
	Expect(jpegview_linux::IsDefaultImageProcessing(params),
		"range-boundary testing failed to restore slider identity values");
	ExpectNear(jpegview_linux::LevelControlValueAtPosition(LevelControl::Brightness, 0.0),
		2.0, 1e-12, "brightness slider left endpoint is not logarithmic maximum");
	ExpectNear(jpegview_linux::LevelControlValueAtPosition(LevelControl::Brightness, 1.0),
		0.5, 1e-12, "brightness slider right endpoint is not logarithmic minimum");
	jpegview_linux::SetLevelControlValue(params, LevelControl::Brightness, 1.0);
	ExpectNear(jpegview_linux::LevelControlPosition(params, LevelControl::Brightness),
		0.5, 1e-12, "brightness slider logarithmic position did not round-trip");
	jpegview_linux::SetLevelControlValue(params, LevelControl::Contrast, 1.0);
	ExpectNear(params.contrast, 0.5, 1e-12, "contrast slider was not clamped to its Windows range");
	jpegview_linux::SetLevelControlValue(params, LevelControl::Brightness,
		std::numeric_limits<double>::quiet_NaN());
	ExpectNear(params.gamma, 1.0, 1e-12, "malformed slider input did not restore its default");
	jpegview_linux::SetLevelControlValue(params, LevelControl::Contrast, 0.0);
	Expect(jpegview_linux::IsDefaultImageProcessing(params), "reset slider values did not restore defaults");
	jpegview_linux::ImageProcessingPreset current;
	jpegview_linux::ImageProcessingPreset saved;
	jpegview_linux::SetLevelControlValue(current.processing, LevelControl::Contrast, 0.25);
	current.autoContrast = true;
	jpegview_linux::SetLevelControlValue(saved.processing, LevelControl::Contrast, -0.25);
	ImageProcessingParams defaultProcessing;
	jpegview_linux::SetLevelControlValue(defaultProcessing, LevelControl::Saturation, 1.4);
	const auto kept = jpegview_linux::ResolveImageProcessingForFile(current, &saved, true, false,
		defaultProcessing);
	const auto restored = jpegview_linux::ResolveImageProcessingForFile(current, &saved, false, false,
		defaultProcessing);
	const auto defaults = jpegview_linux::ResolveImageProcessingForFile(current, nullptr, false, true,
		defaultProcessing);
	ExpectNear(kept.processing.contrast, 0.25, 1e-12,
		"keep-between-images did not override the saved per-file levels");
	Expect(kept.autoContrast, "keep-between-images did not preserve auto correction state");
	ExpectNear(restored.processing.contrast, -0.25, 1e-12,
		"per-file levels were not restored when keep was disabled");
	Expect(!restored.autoContrast, "per-file auto-correction state was not restored");
	ExpectNear(defaults.processing.saturation, 1.4, 1e-12,
		"image without saved levels did not receive the configured default preset");
	Expect(defaults.autoContrast, "image without saved levels did not receive default auto correction");

	const std::vector<std::uint8_t> pixels = {
		32, 64, 96, 17, 64, 96, 128, 18,
		96, 128, 160, 19, 128, 160, 192, 20,
	};
	jpegview_linux::Image original;
	Expect(original.StoreBGRA(pixels.data(), 2, 2), "could not create levels fixture");
	ImageProcessingParams color;
	jpegview_linux::SetLevelControlValue(color, LevelControl::Saturation, 0.0);
	jpegview_linux::Image grayscale = original;
	Expect(grayscale.ApplyProcessing(color, false), "saturation processing rejected a valid image");
	for (std::size_t offset = 0; offset < grayscale.bgra.size(); offset += 4) {
		Expect(std::abs(static_cast<int>(grayscale.bgra[offset]) - grayscale.bgra[offset + 1]) <= 1 &&
			std::abs(static_cast<int>(grayscale.bgra[offset + 1]) - grayscale.bgra[offset + 2]) <= 1,
			"zero saturation did not produce grayscale");
		Expect(grayscale.bgra[offset + 3] == pixels[offset + 3], "levels processing changed alpha");
	}
	color = {};
	jpegview_linux::SetLevelControlValue(color, LevelControl::CyanRed, 1.0);
	jpegview_linux::Image redTint = original;
	Expect(redTint.ApplyProcessing(color, false) && redTint.bgra[2] > original.bgra[2],
		"cyan-red level did not tint toward red");
	color = {};
	jpegview_linux::SetLevelControlValue(color, LevelControl::Brightness, 0.5);
	jpegview_linux::Image brighter = original;
	Expect(brighter.ApplyProcessing(color, false) && brighter.bgra[0] > original.bgra[0],
		"brightness/gamma level did not brighten the image");
	color = {};
	color.localDensityEnabled = true;
	color.lightenShadows = 1.0;
	color.deepShadows = 0.8;
	jpegview_linux::Image locallyCorrected = original;
	Expect(locallyCorrected.ApplyProcessing(color, false) &&
		locallyCorrected.bgra.size() == original.bgra.size(),
		"local density correction failed on a small image");
	color = {};
	color.unsharpRadius = 1.0;
	color.unsharpAmount = 2.0;
	color.unsharpThreshold = 0.0;
	jpegview_linux::Image unsharp = original;
	Expect(unsharp.ApplyProcessing(color, false) && unsharp.bgra != original.bgra,
		"unsharp mask did not alter detail when enabled");
	color = {};
	color.colorCorrection = 0.3;
	color.contrastCorrection = 0.6;
	color.deepShadows = 0.8;
	color.unsharpRadius = 4.0;
	color.unsharpThreshold = 10.0;
	jpegview_linux::Image inactive = original;
	Expect(inactive.ApplyProcessing(color, false) && inactive.bgra == original.bgra,
		"inactive correction controls needlessly changed image pixels");
}

void TestPictureLevelsStoreRoundTrip() {
	TemporaryDirectory temporary;
	const fs::path database = temporary.path() / "picture-levels.db";
	jpegview_linux::ImageProcessingStore expected;
	jpegview_linux::ImageProcessingPreset preset;
	preset.processing.contrast = 0.23;
	preset.processing.gamma = 1.2;
	preset.processing.cyanRed = -0.4;
	preset.processing.localDensityEnabled = true;
	preset.autoContrast = true;
	expected["/images/a \"quoted\" photo.jpg"] = preset;
	Expect(jpegview_linux::SaveImageProcessingStore(database, expected),
		"picture-level store could not be written");
	jpegview_linux::ImageProcessingStore loaded;
	Expect(jpegview_linux::LoadImageProcessingStore(database, loaded),
		"picture-level store could not be read");
	Expect(loaded.size() == 1 && jpegview_linux::EqualImageProcessing(
		loaded.begin()->second.processing, preset.processing) && loaded.begin()->second.autoContrast,
		"picture-level store did not round-trip parameters and correction state");
	Expect(loaded.begin()->first == expected.begin()->first,
		"picture-level store did not preserve quoted path characters");
	preset.processing.contrast = 0.36;
	preset.autoContrast = false;
	expected.begin()->second = preset;
	Expect(jpegview_linux::SaveImageProcessingStore(database, expected),
		"picture-level store could not replace an existing database atomically");
	loaded.clear();
	Expect(jpegview_linux::LoadImageProcessingStore(database, loaded) && loaded.size() == 1 &&
		std::abs(loaded.begin()->second.processing.contrast - 0.36) < 1e-12 &&
		!loaded.begin()->second.autoContrast,
		"replaced picture-level store did not contain the new backup contents");
	std::ofstream malformed(database, std::ios::trunc);
	malformed << "\"/broken\" 1 no-number\n";
	malformed.close();
	Expect(!jpegview_linux::LoadImageProcessingStore(database, loaded) && loaded.size() == 1 &&
		std::abs(loaded.begin()->second.processing.contrast - 0.36) < 1e-12,
		"malformed picture-level database was accepted or replaced the current store");
	std::ofstream outOfRange(database, std::ios::trunc);
	outOfRange << "\"/invalid\" 0 0 0 -1 0 0 0 0 0 0 0 0 0 0 0\n";
	outOfRange.close();
	Expect(!jpegview_linux::LoadImageProcessingStore(database, loaded) && loaded.size() == 1 &&
		std::abs(loaded.begin()->second.processing.contrast - 0.36) < 1e-12,
		"out-of-range picture-level database was accepted or replaced the current store");
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
	expected.showZoomNavigator = false;
	expected.doublePageModeEnabled = true;
	expected.mangaReadingOrderEnabled = true;
	expected.mangaModeInvertsLeftRight = false;
	expected.spacebarNavigatesImages = true;
	expected.folderWrapAround = false;
	expected.transparencyPattern = jpegview_linux::TransparencyPattern::Checkerboard;
	expected.thumbnailPanelWidth = 287;
	expected.fileDialogWidth = 1040;
	expected.fileDialogHeight = 735;
	expected.fileDialogPreviewRatio = 0.375;
	expected.magnifyingGlassWidth = 425;
	expected.magnifyingGlassHeight = 215;
	expected.magnifyingGlassZoomLevel = 0.725;
	expected.fixedCropWidth = 512;
	expected.fixedCropHeight = 288;
	expected.fixedCropScreenPixels = false;
	expected.userCropAspectWidth = 13;
	expected.userCropAspectHeight = 7;
	expected.selectionModeEnabled = true;
	expected.infoVisible = true;
	expected.showHistogram = true;
	expected.showFilename = true;
	expected.autoContrast = true;
	expected.keepPictureLevels = true;
	expected.defaultImageProcessing.contrast = 0.18;
	expected.defaultImageProcessing.gamma = 1.15;
	expected.defaultImageProcessing.saturation = 1.25;
	expected.defaultImageProcessing.cyanRed = 0.2;
	expected.defaultImageProcessing.magentaGreen = -0.2;
	expected.defaultImageProcessing.yellowBlue = 0.3;
	expected.defaultImageProcessing.lightenShadows = 0.4;
	expected.defaultImageProcessing.darkenHighlights = 0.2;
	expected.defaultImageProcessing.deepShadows = 0.6;
	expected.defaultImageProcessing.colorCorrection = 0.1;
	expected.defaultImageProcessing.contrastCorrection = 0.55;
	expected.defaultImageProcessing.sharpen = 0.15;
	expected.defaultImageProcessing.localDensityEnabled = true;
	expected.unsharpMaskRadius = 2.25;
	expected.unsharpMaskAmount = 3.5;
	expected.unsharpMaskThreshold = 7.0;
	expected.cacheSizeMiB = 1536;
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
	Expect(loaded.showZoomNavigator == expected.showZoomNavigator,
		"zoom navigator visibility did not round-trip");
	Expect(loaded.doublePageModeEnabled == expected.doublePageModeEnabled &&
		loaded.mangaReadingOrderEnabled == expected.mangaReadingOrderEnabled &&
		loaded.mangaModeInvertsLeftRight == expected.mangaModeInvertsLeftRight &&
		loaded.spacebarNavigatesImages == expected.spacebarNavigatesImages &&
		loaded.folderWrapAround == expected.folderWrapAround,
		"navigation and display-mode settings did not round-trip");
	Expect(loaded.transparencyPattern == expected.transparencyPattern,
		"transparent-image pattern did not round-trip");
	Expect(loaded.thumbnailPanelWidth == expected.thumbnailPanelWidth,
		"thumbnail panel width did not round-trip");
	Expect(loaded.fileDialogWidth == expected.fileDialogWidth &&
		loaded.fileDialogHeight == expected.fileDialogHeight,
		"file-dialog dimensions did not round-trip");
	ExpectNear(loaded.fileDialogPreviewRatio, expected.fileDialogPreviewRatio, 0.0000001,
		"file-dialog preview proportion did not round-trip");
	Expect(loaded.magnifyingGlassWidth == expected.magnifyingGlassWidth &&
		loaded.magnifyingGlassHeight == expected.magnifyingGlassHeight,
		"magnifying-glass size did not round-trip");
	ExpectNear(loaded.magnifyingGlassZoomLevel, expected.magnifyingGlassZoomLevel, 0.0000001,
		"magnifying-glass zoom level did not round-trip");
	Expect(loaded.fixedCropWidth == expected.fixedCropWidth &&
		loaded.fixedCropHeight == expected.fixedCropHeight &&
		loaded.fixedCropScreenPixels == expected.fixedCropScreenPixels,
		"fixed crop size and pixel units did not round-trip");
	Expect(loaded.userCropAspectWidth == expected.userCropAspectWidth &&
		loaded.userCropAspectHeight == expected.userCropAspectHeight &&
		loaded.selectionModeEnabled == expected.selectionModeEnabled,
		"user crop ratio or selection mode did not round-trip");
	Expect(loaded.infoVisible == expected.infoVisible && loaded.showHistogram == expected.showHistogram &&
		loaded.showFilename == expected.showFilename &&
		loaded.autoContrast == expected.autoContrast,
		"overlay/correction settings did not round-trip");
	Expect(loaded.keepPictureLevels == expected.keepPictureLevels,
		"keep picture levels setting did not round-trip");
	Expect(jpegview_linux::EqualImageProcessing(loaded.defaultImageProcessing,
		expected.defaultImageProcessing), "default picture levels did not round-trip");
	ExpectNear(loaded.unsharpMaskRadius, expected.unsharpMaskRadius, 0.0000001,
		"unsharp radius setting did not round-trip");
	ExpectNear(loaded.unsharpMaskAmount, expected.unsharpMaskAmount, 0.0000001,
		"unsharp amount setting did not round-trip");
	ExpectNear(loaded.unsharpMaskThreshold, expected.unsharpMaskThreshold, 0.0000001,
		"unsharp threshold setting did not round-trip");
	Expect(loaded.copyRenamePattern == expected.copyRenamePattern, "batch pattern did not round-trip");
	Expect(loaded.cacheSizeMiB == expected.cacheSizeMiB, "cache size did not round-trip");
	Expect(loaded.manualZoomSet, "saved manual zoom was not marked present");
	ExpectNear(loaded.manualZoom, expected.manualZoom, 0.0000001, "manual zoom did not round-trip");

	const fs::path malformed = temporary.path() / "malformed.conf";
	std::ofstream malformedOutput(malformed);
	malformedOutput << "  scale_mode = manual\nmanual_zoom=not-a-number\ndefault_gamma=not-a-number\n"
		"thumbnail_panel_width=not-a-number\nfile_dialog_width=not-a-number\n"
		"file_dialog_height=not-a-number\nfile_dialog_preview_ratio=nan\n"
		"magnifying_glass_width=not-a-number\nmagnifying_glass_height=not-a-number\n"
		"magnifying_glass_zoom_level=nan\n"
		"fixed_crop_width=not-a-number\nfixed_crop_height=0\n"
		"user_crop_aspect_width=0\nuser_crop_aspect_height=nan\n"
		"fixed_crop_screen_pixels=maybe\ndefault_selection_mode=1\n"
		"selection_mode_enabled=maybe\n"
		"show_zoom_navigator=maybe\nmanga_mode_inverts_left_right=maybe\n"
		"spacebar_navigates_images=maybe\nfolder_wrap_around=maybe\n"
		"transparency_pattern=diagonal\n"
		"cache_size_mb=not-a-number\nunknown_key=value\n";
	malformedOutput.close();
	loaded = {};
	Expect(jpegview_linux::LoadViewerSettings(malformed, loaded), "malformed settings file was rejected entirely");
	Expect(loaded.scaleMode == "manual", "whitespace around a setting was not trimmed");
	Expect(!loaded.manualZoomSet && loaded.manualZoom == 1.0,
		"malformed manual zoom did not retain its default");
	Expect(!loaded.thumbnailPanelVisible,
		"settings without thumbnail visibility did not retain the hidden default");
	Expect(!loaded.showHistogram,
		"settings without a histogram choice did not retain the hidden default");
	Expect(loaded.showZoomNavigator,
		"malformed zoom navigator visibility did not retain its enabled default");
	Expect(!loaded.doublePageModeEnabled && !loaded.mangaReadingOrderEnabled,
		"missing double-page settings did not retain their disabled defaults");
	Expect(loaded.mangaModeInvertsLeftRight,
		"missing or malformed manga inversion setting did not preserve its enabled default");
	Expect(!loaded.spacebarNavigatesImages,
		"missing or malformed Space navigation setting did not preserve its disabled default");
	Expect(loaded.folderWrapAround,
		"missing or malformed folder-wrap setting did not preserve the wrapping default");
	Expect(loaded.transparencyPattern == jpegview_linux::TransparencyPattern::Black,
		"malformed transparency pattern did not retain the default black background");
	Expect(loaded.thumbnailPanelWidth == jpegview_linux::kDefaultThumbnailPanelWidth,
		"malformed thumbnail width did not retain its default");
	Expect(loaded.fileDialogWidth == jpegview_linux::kDefaultFileDialogWidth &&
		loaded.fileDialogHeight == jpegview_linux::kDefaultFileDialogHeight &&
		loaded.fileDialogPreviewRatio == 0.0,
		"malformed file-dialog geometry did not retain its defaults");
	Expect(loaded.magnifyingGlassWidth == jpegview_linux::MagnifyingGlassModel::kDefaultWidth &&
		loaded.magnifyingGlassHeight == jpegview_linux::MagnifyingGlassModel::kDefaultHeight &&
		loaded.magnifyingGlassZoomLevel ==
			jpegview_linux::MagnifyingGlassModel::kDefaultZoomLevel,
		"malformed magnifying-glass settings did not retain their defaults");
	Expect(loaded.fixedCropWidth == jpegview_linux::kDefaultFixedCropWidth &&
		loaded.fixedCropHeight == jpegview_linux::kMinimumFixedCropDimension &&
		loaded.fixedCropScreenPixels &&
		loaded.userCropAspectWidth == jpegview_linux::kDefaultUserCropAspectWidth &&
		loaded.userCropAspectHeight == jpegview_linux::kDefaultUserCropAspectHeight &&
		!loaded.selectionModeEnabled,
		"malformed or legacy selection-mode settings did not retain the disabled default");
	Expect(loaded.cacheSizeMiB == jpegview_linux::kDefaultCacheSizeMiB,
		"malformed cache size did not retain its default");

	const fs::path clamped = temporary.path() / "clamped.conf";
	WriteText(clamped, "manual_zoom=1000\nthumbnail_panel_width=2\n"
		"file_dialog_width=1\nfile_dialog_height=999999\nfile_dialog_preview_ratio=4\n"
		"magnifying_glass_width=1\nmagnifying_glass_height=999999\n"
		"magnifying_glass_zoom_level=4\n"
		"transparency_pattern=white\n"
		"fixed_crop_width=999999\nfixed_crop_height=-10\n"
		"user_crop_aspect_width=999999\nuser_crop_aspect_height=5\n"
		"cache_size_mb=999999999\n");
	loaded = {};
	Expect(jpegview_linux::LoadViewerSettings(clamped, loaded) && loaded.manualZoomSet &&
		loaded.manualZoom == jpegview_linux::kMaximumZoom &&
		loaded.thumbnailPanelWidth == jpegview_linux::kMinimumThumbnailPanelWidth &&
		loaded.fileDialogWidth == jpegview_linux::kMinimumFileDialogWidth &&
		loaded.fileDialogHeight == jpegview_linux::kMaximumFileDialogDimension &&
		loaded.fileDialogPreviewRatio == 0.8 &&
		loaded.magnifyingGlassWidth == jpegview_linux::MagnifyingGlassModel::kMinimumWidth &&
		loaded.magnifyingGlassHeight == jpegview_linux::MagnifyingGlassModel::kMaximumDimension &&
		loaded.magnifyingGlassZoomLevel ==
			jpegview_linux::MagnifyingGlassModel::kMaximumZoomLevel &&
		loaded.transparencyPattern == jpegview_linux::TransparencyPattern::White &&
		loaded.fixedCropWidth == jpegview_linux::kMaximumFixedCropDimension &&
		loaded.fixedCropHeight == jpegview_linux::kMinimumFixedCropDimension &&
		loaded.userCropAspectWidth == jpegview_linux::kMaximumFixedCropDimension &&
		loaded.userCropAspectHeight == 5 &&
		loaded.cacheSizeMiB == jpegview_linux::kMaximumCacheSizeMiB,
		"out-of-range settings were not clamped to their public limits");

	jpegview_linux::ViewerSettings unchanged;
	unchanged.scaleMode = "sentinel";
	Expect(!jpegview_linux::LoadViewerSettings(temporary.path() / "missing.conf", unchanged) &&
		unchanged.scaleMode == "sentinel",
		"missing settings file modified the caller's existing settings");
}

void TestAdvancedConfigurationModelCategoriesAndRoundTrips() {
	using Category = jpegview_linux::AdvancedConfigurationCategory;
	using FieldKind = jpegview_linux::AdvancedConfigurationFieldKind;
	jpegview_linux::ViewerSettings source;
	source.scaleMode = "manual";
	source.maximized = true;
	source.selectionModeEnabled = true;
	source.autoContrast = true;
	source.mangaModeInvertsLeftRight = false;
	source.spacebarNavigatesImages = true;
	source.folderWrapAround = false;
	source.transparencyPattern = jpegview_linux::TransparencyPattern::White;
	source.showHistogram = true;
	source.thumbnailPanelWidth = 287;
	source.fileDialogWidth = 1040;
	source.fileDialogHeight = 735;
	source.fileDialogPreviewRatio = 0.375;
	source.magnifyingGlassWidth = 425;
	source.magnifyingGlassHeight = 215;
	source.magnifyingGlassZoomLevel = 0.725;
	source.userCropAspectWidth = 13;
	source.userCropAspectHeight = 7;
	source.defaultImageProcessing.contrast = 0.18;
	source.defaultImageProcessing.gamma = 1.15;
	source.defaultImageProcessing.saturation = 1.25;
	source.defaultImageProcessing.cyanRed = 0.2;
	source.defaultImageProcessing.magentaGreen = -0.2;
	source.defaultImageProcessing.yellowBlue = 0.3;
	source.defaultImageProcessing.lightenShadows = 0.4;
	source.defaultImageProcessing.darkenHighlights = 0.2;
	source.defaultImageProcessing.deepShadows = 0.6;
	source.defaultImageProcessing.colorCorrection = 0.1;
	source.defaultImageProcessing.contrastCorrection = 0.55;
	source.defaultImageProcessing.sharpen = 0.15;
	source.defaultImageProcessing.localDensityEnabled = true;
	source.unsharpMaskRadius = 2.25;
	source.unsharpMaskAmount = 3.5;
	source.unsharpMaskThreshold = 7.0;
	source.cacheSizeMiB = 1536;
	source.copyRenamePattern = u8"%F=旅行-%n";

	const std::array<std::vector<std::string>, 7> expectedKeys = {{
		{"manga_mode_inverts_left_right", "spacebar_navigates_images", "folder_wrap_around"},
		{"transparency_pattern", "show_histogram"},
		{"thumbnail_panel_width", "file_dialog_width", "file_dialog_height",
			"file_dialog_preview_ratio"},
		{"magnifying_glass_width", "magnifying_glass_height", "magnifying_glass_zoom_level"},
		{"user_crop_aspect_width", "user_crop_aspect_height"},
		{"default_local_density", "default_contrast", "default_gamma", "default_saturation",
			"default_cyan_red", "default_magenta_green", "default_yellow_blue",
			"default_lighten_shadows", "default_darken_highlights", "default_deep_shadows",
			"default_color_correction", "default_contrast_correction", "default_sharpen",
			"unsharp_mask_radius", "unsharp_mask_amount", "unsharp_mask_threshold"},
		{"cache_size_mb", "copy_rename_pattern"},
	}};
	const char* const categoryNames[] = {
		"Behavior", "Appearance", "Panels & dialogs", "Magnifying glass", "Crop",
		"Image defaults", "Performance & batch",
	};

	jpegview_linux::AdvancedConfigurationModel model;
	model.Open(source);
	Expect(model.IsOpen() && model.CategoryCount() == 7 && model.ActiveCategory() == 0,
		"advanced configuration did not open on its first category");
	std::size_t fieldTotal = 0;
	for (int category = 0; category < model.CategoryCount(); ++category) {
		Expect(std::string(model.CategoryName(category)) == categoryNames[category],
			"advanced configuration category label changed");
		Expect(model.SelectCategory(category) && model.ActiveCategory() == category,
			"advanced configuration could not select a category");
		Expect(model.RowCount() == static_cast<int>(expectedKeys[category].size()),
			"advanced configuration category has an unexpected row count");
		for (int row = 0; row < model.RowCount(); ++row) {
			const jpegview_linux::AdvancedConfigurationField* field = model.FieldAt(row);
			Expect(field != nullptr && model.FieldKey(row) == expectedKeys[category][row] &&
				model.FieldLabel(row)[0] != '\0',
				"advanced configuration field metadata is missing or out of order");
			if (field->kind == FieldKind::Integer || field->kind == FieldKind::Decimal) {
				Expect(field->minimum < field->maximum && field->step > 0.0,
					"numeric advanced setting has invalid bounds or adjustment step");
			} else if (field->kind == FieldKind::Choice) {
				Expect(field->choices != nullptr && field->choiceCount >= 2,
					"choice advanced setting has no choice labels");
			}
			++fieldTotal;
		}
	}
	Expect(fieldTotal == 32, "advanced configuration did not expose every requested setting");
	Expect(model.SelectCategory(static_cast<int>(Category::Behavior)) &&
		model.FieldValue(2) == "Off",
		"folder-wrap setting did not display its disabled draft value");
	Expect(model.SelectCategory(static_cast<int>(Category::Appearance)) &&
		model.FieldValue(0) == "White" && model.FieldValue(1) == "On",
		"choice or boolean setting values were not formatted for display");
	Expect(model.SelectCategory(static_cast<int>(Category::PanelsAndDialogs)) &&
		model.FieldValue(3) == "37.5%",
		"file-dialog preview ratio did not use its percentage display format");
	Expect(model.SelectCategory(static_cast<int>(Category::MagnifyingGlass)) &&
		model.FieldValue(2) == "0.725",
		"magnifying-glass zoom level did not retain its source-scale meaning");
	Expect(jpegview_linux::EqualImageProcessing(model.Draft().defaultImageProcessing,
		source.defaultImageProcessing) && model.Draft().copyRenamePattern == source.copyRenamePattern &&
		model.Draft().magnifyingGlassWidth == source.magnifyingGlassWidth &&
		model.Draft().fileDialogPreviewRatio == source.fileDialogPreviewRatio,
		"opening and visiting categories changed the persisted settings draft");
	Expect(model.Draft().scaleMode == source.scaleMode && model.Draft().maximized == source.maximized &&
		model.Draft().selectionModeEnabled == source.selectionModeEnabled &&
		model.Draft().autoContrast == source.autoContrast,
		"advanced configuration altered excluded automatic or session settings");

	auto edit = [&model](int row, const std::string& text) {
		model.SelectRow(row);
		Expect(model.ActivateSelected() && model.IsEditing(),
			"activating a typed advanced setting did not begin editing");
		Expect(model.AppendText(text), "advanced configuration rejected valid edit text");
		Expect(model.CommitEdit() && !model.IsEditing(),
			"valid advanced setting edit did not commit");
	};

	model.SelectCategory(static_cast<int>(Category::Behavior));
	model.SelectRow(0);
	Expect(model.ActivateSelected() && model.Draft().mangaModeInvertsLeftRight,
		"behavior boolean did not toggle on activation");
	Expect(model.AdjustSelected(1) && !model.Draft().mangaModeInvertsLeftRight,
		"directional adjustment did not toggle a boolean setting");
	model.SelectRow(1);
	Expect(model.AdjustSelected(-1) && !model.Draft().spacebarNavigatesImages,
		"behavior boolean did not toggle through directional adjustment");
	model.SelectRow(2);
	Expect(model.ActivateSelected() && model.Draft().folderWrapAround &&
		model.AdjustSelected(-1) && !model.Draft().folderWrapAround,
		"folder-wrap setting did not toggle in both advanced-configuration controls");

	model.SelectCategory(static_cast<int>(Category::Appearance));
	model.SelectRow(0);
	Expect(model.AdjustSelected(1) &&
		model.Draft().transparencyPattern == jpegview_linux::TransparencyPattern::Checkerboard,
		"appearance choice did not advance to the next transparency pattern");
	Expect(model.AdjustSelected(1) &&
		model.Draft().transparencyPattern == jpegview_linux::TransparencyPattern::Black,
		"appearance choice did not wrap after its final value");
	model.SelectRow(1);
	Expect(model.ActivateSelected() && !model.Draft().showHistogram,
		"appearance boolean did not toggle on activation");

	model.SelectCategory(static_cast<int>(Category::PanelsAndDialogs));
	model.SelectRow(0);
	Expect(model.AdjustSelected(1) && model.Draft().thumbnailPanelWidth == 295,
		"thumbnail panel width did not use its configured adjustment step");
	edit(1, "1400");
	edit(2, "850");
	edit(3, "0.5");
	Expect(model.Draft().fileDialogWidth == 1400 && model.Draft().fileDialogHeight == 850 &&
		std::abs(model.Draft().fileDialogPreviewRatio - 0.5) < 1e-12,
		"panels and dialogs fields did not round-trip edits to the ViewerSettings draft");

	model.SelectCategory(static_cast<int>(Category::MagnifyingGlass));
	edit(0, "560");
	edit(1, "280");
	model.SelectRow(2);
	Expect(model.AdjustSelected(1) &&
		std::abs(model.Draft().magnifyingGlassZoomLevel - 0.75) < 1e-12,
		"magnifying-glass zoom level did not adjust in its source-scale units");
	Expect(model.FieldValue(2) == "0.750", "magnifying-glass zoom formatting lost precision");
	model.SelectCategory(static_cast<int>(Category::Crop));
	edit(0, "21");
	edit(1, "9");
	Expect(model.Draft().userCropAspectWidth == 21 && model.Draft().userCropAspectHeight == 9,
		"crop aspect fields did not round-trip typed values");

	model.SelectCategory(static_cast<int>(Category::ImageDefaults));
	model.SelectRow(0);
	Expect(model.ActivateSelected() && !model.Draft().defaultImageProcessing.localDensityEnabled,
		"default local-density boolean did not toggle");
	for (std::size_t index = 0; index < static_cast<std::size_t>(jpegview_linux::LevelControl::Count); ++index) {
		const int row = static_cast<int>(index) + 1;
		const auto* field = model.FieldAt(row);
		const auto control = static_cast<jpegview_linux::LevelControl>(index);
		const double target = (field->minimum + field->maximum) / 2.0;
		edit(row, std::to_string(target));
		ExpectNear(jpegview_linux::GetLevelControlValue(model.Draft().defaultImageProcessing,
			control), target, 0.00001,
			"one of the twelve default picture-level controls did not round-trip");
	}
	edit(13, "3.25");
	edit(14, "8.75");
	edit(15, "19.5");
	Expect(model.Draft().unsharpMaskRadius == 3.25 && model.Draft().unsharpMaskAmount == 8.75 &&
		model.Draft().unsharpMaskThreshold == 19.5,
		"unsharp-mask defaults did not round-trip through typed edits");

	model.SelectCategory(static_cast<int>(Category::PerformanceAndBatch));
	model.SelectRow(0);
	Expect(model.AdjustSelected(1) && model.Draft().cacheSizeMiB == 1600,
		"cache budget did not adjust by the declared MB step");
	edit(1, u8"copy-旅行-%F_%n");
	Expect(model.Draft().copyRenamePattern == u8"copy-旅行-%F_%n",
		"copy/rename pattern did not preserve Unicode and template text");

	model.SelectCategory(static_cast<int>(Category::ImageDefaults));
	model.SetVisibleRows(3);
	Expect(model.VisibleRows() == 3 && model.Scroll() == 0,
		"advanced configuration did not retain the visible row count");
	model.MoveSelection(8);
	Expect(model.SelectedRow() == 8 && model.Scroll() == 6,
		"row selection did not scroll to keep the selected field visible");
	model.ScrollBy(100);
	Expect(model.Scroll() == static_cast<std::size_t>(model.RowCount() - model.VisibleRows()) &&
		model.SelectedRow() == 8,
		"independent field scrolling did not clamp at the end or changed selection");
	model.ScrollBy(-100);
	Expect(model.Scroll() == 0, "field scrolling did not clamp at the beginning");
	model.MoveSelection(-100);
	Expect(model.SelectedRow() == 0 && model.Scroll() == 0,
		"row selection did not clamp to the first field");
	model.SelectCategory(static_cast<int>(Category::Behavior));
	model.MoveCategory(-1);
	Expect(model.ActiveCategory() == static_cast<int>(Category::PerformanceAndBatch),
		"category cycling did not wrap backward");
	model.MoveCategory(1);
	Expect(model.ActiveCategory() == static_cast<int>(Category::Behavior),
		"category cycling did not wrap forward");

	model.Close();
	Expect(!model.IsOpen() && !model.IsEditing(), "advanced configuration did not close cleanly");
}

void TestAdvancedConfigurationModelValidationAndCancellation() {
	using Category = jpegview_linux::AdvancedConfigurationCategory;
	jpegview_linux::ViewerSettings original;
	original.fileDialogWidth = 900;
	original.fileDialogPreviewRatio = 0.375;
	original.unsharpMaskRadius = 1.0;
	original.copyRenamePattern = u8"old-写像-%n";
	jpegview_linux::AdvancedConfigurationModel model;
	model.Open(original);

	model.SelectCategory(static_cast<int>(Category::PanelsAndDialogs));
	model.SelectRow(1);
	model.BeginEdit();
	Expect(model.IsEditing() && model.EditingText() == "900",
		"numeric edit did not start with the current setting");
	Expect(model.AppendText("12px"), "numeric editor rejected input before strict validation");
	Expect(!model.CommitEdit() && model.IsEditing() && !model.Message().empty() &&
		model.Draft().fileDialogWidth == 900,
		"malformed integer input changed the draft or failed without a validation message");
	model.CancelEdit();
	Expect(!model.IsEditing() && model.Draft().fileDialogWidth == 900,
		"cancelling an invalid numeric edit changed the draft");

	model.BeginEdit();
	model.AppendText("999999");
	Expect(model.CommitEdit() && model.Draft().fileDialogWidth == jpegview_linux::kMaximumFileDialogDimension &&
		!model.Message().empty(),
		"integer setting did not clamp to its documented maximum");
	model.SelectRow(1);
	model.BeginEdit();
	model.AppendText("-5");
	Expect(model.CommitEdit() && model.Draft().fileDialogWidth == jpegview_linux::kMinimumFileDialogWidth,
		"integer setting did not clamp to its documented minimum");

	model.SelectRow(3);
	model.BeginEdit();
	model.AppendText("nan");
	Expect(!model.CommitEdit() && model.Draft().fileDialogPreviewRatio == 0.375,
		"non-finite decimal input was accepted or changed the draft");
	model.SelectAll();
	model.AppendText("0x1p-1");
	Expect(!model.CommitEdit() && model.Draft().fileDialogPreviewRatio == 0.375,
		"non-decimal hexadecimal notation passed strict numeric validation");
	model.SelectAll();
	model.AppendText("1");
	Expect(model.CommitEdit() && model.Draft().fileDialogPreviewRatio == 0.8,
		"decimal setting did not clamp to its documented maximum");

	model.SelectCategory(static_cast<int>(Category::MagnifyingGlass));
	model.SelectRow(2);
	model.BeginEdit();
	model.AppendText("-1");
	Expect(model.CommitEdit() &&
		model.Draft().magnifyingGlassZoomLevel == jpegview_linux::MagnifyingGlassModel::kMinimumZoomLevel,
		"magnifying-glass zoom did not clamp to its documented minimum");
	model.SelectCategory(static_cast<int>(Category::ImageDefaults));
	model.SelectRow(13);
	model.BeginEdit();
	model.AppendText("100");
	Expect(model.CommitEdit() && model.Draft().unsharpMaskRadius == 5.0,
		"unsharp radius did not clamp to its documented maximum");
	model.BeginEdit();
	model.AppendText("-1");
	Expect(model.CommitEdit() && model.Draft().unsharpMaskRadius == 0.0,
		"unsharp radius did not clamp to its documented minimum");

	model.SelectCategory(static_cast<int>(Category::Crop));
	model.SelectRow(0);
	model.BeginEdit();
	model.AppendText("0");
	Expect(model.CommitEdit() && model.Draft().userCropAspectWidth == 1,
		"crop aspect integer did not clamp to its documented minimum");
	model.SelectRow(1);
	model.BeginEdit();
	model.AppendText("999999");
	Expect(model.CommitEdit() && model.Draft().userCropAspectHeight ==
		jpegview_linux::kMaximumFixedCropDimension,
		"crop aspect integer did not clamp to its documented maximum");

	model.SelectCategory(static_cast<int>(Category::PerformanceAndBatch));
	model.SelectRow(1);
	model.BeginEdit();
	model.SelectAll();
	model.AppendText(u8"copy-写像");
	model.Backspace();
	Expect(model.EditingText() == u8"copy-写",
		"copy/rename pattern backspace split a UTF-8 code point");
	model.AppendText(u8"像");
	Expect(model.CommitEdit() && model.Draft().copyRenamePattern == u8"copy-写像",
		"copy/rename text edit did not commit the edited string");

	model.BeginEdit();
	model.SelectAll();
	model.AppendText("temporary");
	model.CancelEdit();
	Expect(model.Draft().copyRenamePattern == u8"copy-写像",
		"cancelling a text edit changed the active draft");
	model.BeginEdit();
	model.SelectAll();
	Expect(!model.AppendText("bad\npattern") && model.EditingText().empty(),
		"copy/rename editor accepted a line break that would corrupt settings storage");
	model.CancelEdit();

	model.Open(original);
	model.SelectCategory(static_cast<int>(Category::Behavior));
	model.SelectRow(0);
	Expect(model.ActivateSelected() && model.Draft().mangaModeInvertsLeftRight !=
		original.mangaModeInvertsLeftRight,
		"draft mutation could not be staged before cancelling the dialog");
	Expect(original.mangaModeInvertsLeftRight && original.fileDialogWidth == 900 &&
		original.copyRenamePattern == u8"old-写像-%n",
		"editing an advanced configuration draft mutated its source settings");
	model.Close();
	model.Open(original);
	Expect(model.Draft().mangaModeInvertsLeftRight == original.mangaModeInvertsLeftRight &&
		model.Draft().fileDialogWidth == original.fileDialogWidth &&
		model.Draft().copyRenamePattern == original.copyRenamePattern,
		"reopening after cancellation did not restore the caller-provided settings");
	model.SetMessage("adapter message");
	Expect(model.Message() == "adapter message", "advanced configuration message was not retained");
}

void TestTransparencyPatternValuesAndTileColors() {
	using jpegview_linux::TransparencyPattern;
	TransparencyPattern pattern = TransparencyPattern::Black;
	Expect(jpegview_linux::ParseTransparencyPattern("black", pattern) &&
		pattern == TransparencyPattern::Black,
		"black transparency pattern could not be parsed");
	Expect(jpegview_linux::ParseTransparencyPattern("white", pattern) &&
		pattern == TransparencyPattern::White,
		"white transparency pattern could not be parsed");
	Expect(jpegview_linux::ParseTransparencyPattern("checkerboard", pattern) &&
		pattern == TransparencyPattern::Checkerboard,
		"checkerboard transparency pattern could not be parsed");
	Expect(!jpegview_linux::ParseTransparencyPattern("stripe", pattern) &&
		pattern == TransparencyPattern::Checkerboard,
		"invalid transparency pattern changed the active value");
	const auto black = jpegview_linux::TransparencyPatternTileColor(TransparencyPattern::Black, 1, 0);
	const auto white = jpegview_linux::TransparencyPatternTileColor(TransparencyPattern::White, 1, 0);
	const auto light = jpegview_linux::TransparencyPatternTileColor(TransparencyPattern::Checkerboard, 0, 0);
	const auto dark = jpegview_linux::TransparencyPatternTileColor(TransparencyPattern::Checkerboard, 1, 0);
	Expect(black.red == 0 && black.green == 0 && black.blue == 0 &&
		white.red == 255 && white.green == 255 && white.blue == 255,
		"solid transparency patterns returned incorrect colors");
	Expect(light.red == 208 && light.green == 208 && light.blue == 208 &&
		dark.red == 144 && dark.green == 144 && dark.blue == 144 &&
		jpegview_linux::kTransparencyCheckerCellSize > 0,
		"checkerboard pattern colors or tile geometry were invalid");
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
	homeEnvironment.Clear();
	Expect(jpegview_linux::ViewerSettingsPath().empty(),
		"settings path was invented when neither XDG_CONFIG_HOME nor HOME was available");
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

void TestBatchCopyDialogController() {
	std::vector<jpegview_linux::BatchCopyItem> items;
	for (int index = 0; index < 8; ++index) {
		jpegview_linux::BatchCopyItem item;
		item.source = fs::path("/tmp/photos/image" + std::to_string(index) + ".jpg");
		items.push_back(std::move(item));
	}
	jpegview_linux::BatchCopyDialogController dialog;
	dialog.Open(items, 5, "%F-copy.%e", 3);
	Expect(dialog.IsOpen() && dialog.PatternFocused() && dialog.Cursor() == 5 && dialog.Scroll() == 3,
		"batch dialog did not initialize focus, cursor, and visible scroll state");
	dialog.MoveCursor(1, 3);
	Expect(dialog.Cursor() == 5, "batch dialog moved the list cursor while its pattern had focus");
	dialog.TogglePatternFocus();
	dialog.MoveCursor(1, 3);
	Expect(dialog.Cursor() == 6 && dialog.Scroll() == 4,
		"batch dialog did not reveal a keyboard-moved cursor");
	dialog.ToggleItem(6);
	Expect(dialog.Items()[6].selected && dialog.Items()[6].destinationText == "image6-copy.jpg" &&
		dialog.Message().find("1 selected") != std::string::npos,
		"batch dialog did not update selection, preview, and summary together");
	dialog.SelectAll(true);
	Expect(std::all_of(dialog.Items().begin(), dialog.Items().end(),
		[](const jpegview_linux::BatchCopyItem& item) { return item.selected; }) &&
		dialog.Message().find("8 selected") != std::string::npos,
		"batch dialog select-all did not refresh preview state");
	dialog.ScrollBy(100, 3);
	Expect(dialog.Scroll() == 5, "batch dialog scrolling exceeded its final full page");
	dialog.ScrollBy(-100, 3);
	Expect(dialog.Scroll() == 0, "batch dialog scrolling exceeded its first page");
	dialog.FocusItem(2);
	Expect(dialog.Cursor() == 2, "batch dialog hover focus did not select a valid row");
	dialog.FocusItem(99);
	Expect(dialog.Cursor() == 2, "batch dialog hover focus accepted an invalid row");

	dialog.Open({}, 0, u8"copy-写真", 4);
	dialog.BackspacePattern();
	Expect(dialog.Pattern() == u8"copy-写", "batch dialog Backspace split a UTF-8 code point");
	dialog.AppendPattern(u8"像");
	Expect(dialog.Pattern() == u8"copy-写像", "batch dialog did not append Unicode pattern text");
	dialog.TogglePatternFocus();
	dialog.AppendPattern("ignored");
	Expect(dialog.Pattern() == u8"copy-写像", "batch dialog edited the pattern without pattern focus");
	dialog.Close();
	Expect(!dialog.IsOpen() && !dialog.PatternFocused(), "batch dialog did not clear open/focus state");
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

void TestDefaultViewerRegistration() {
	TemporaryDirectory temporary;
	const fs::path dataHome = temporary.path() / "data";
	const fs::path configHome = temporary.path() / "config";
	const fs::path executable = temporary.path() / "JPEG View%\".AppImage";
	const fs::path mimeApps = configHome / "mimeapps.list";
	fs::create_directories(configHome);
	WriteText(mimeApps,
		"[Added Associations]\n"
		"image/jpeg=existing-viewer.desktop;\n"
		"\n[Default Applications]\n"
		"image/jpeg=existing-viewer.desktop;\n"
		"text/plain=editor.desktop;\n");
	std::string errorMessage;
	Expect(jpegview_linux::RegisterDefaultViewer(executable, dataHome, configHome, errorMessage),
		"default viewer could not be registered: " + errorMessage);
	const fs::path desktopFile = dataHome / "applications" / "jpegview-linux-user.desktop";
	const std::string desktop = [&desktopFile]() {
		std::ifstream input(desktopFile);
		std::ostringstream contents;
		contents << input.rdbuf();
		return contents.str();
	}();
	std::string escapedExecutable;
	for (const char character : executable.string()) {
		if (character == '\\' || character == '"') escapedExecutable.push_back('\\');
		if (character == '%') escapedExecutable.push_back('%');
		escapedExecutable.push_back(character);
	}
	Expect(desktop.find("Exec=\"" + escapedExecutable + "\" %F") != std::string::npos &&
		desktop.find("%%") != std::string::npos && desktop.find("NoDisplay=true") != std::string::npos,
		"default viewer desktop entry did not safely quote its executable or hide its launcher duplicate");
	Expect(desktop.find("MimeType=image/jpeg;") != std::string::npos &&
		desktop.find("image/tiff;") != std::string::npos &&
		desktop.find("image/jxl;") != std::string::npos,
		"default viewer desktop entry omitted supported MIME types");
	jpegview_linux::OpenWithApplication registeredEntry;
	Expect(!jpegview_linux::ReadDesktopApplication(desktopFile, "image/jpeg", registeredEntry),
		"the NoDisplay default-viewer entry leaked into Open With discovery");
	std::ifstream associationInput(mimeApps);
	std::ostringstream associationContents;
	associationContents << associationInput.rdbuf();
	const std::string associations = associationContents.str();
	Expect(associations.find("[Added Associations]\nimage/jpeg=existing-viewer.desktop;\n") !=
		std::string::npos &&
		associations.find("image/jpeg=jpegview-linux-user.desktop;") != std::string::npos &&
		associations.find("text/plain=editor.desktop;") != std::string::npos &&
		associations.find("image/jxl=jpegview-linux-user.desktop;") != std::string::npos,
		"default viewer registration did not preserve unrelated MIME associations");
	Expect(jpegview_linux::RegisterDefaultViewer(executable, dataHome, configHome, errorMessage),
		"repeated default viewer registration failed: " + errorMessage);
	std::ifstream repeatedInput(mimeApps);
	std::ostringstream repeatedContents;
	repeatedContents << repeatedInput.rdbuf();
	const std::string repeated = repeatedContents.str();
	const std::string mimeDefault = "image/jpeg=jpegview-linux-user.desktop;";
	const std::size_t first = repeated.find(mimeDefault);
	Expect(first != std::string::npos && repeated.find(mimeDefault, first + mimeDefault.size()) == std::string::npos,
		"repeated default viewer registration duplicated a MIME association");
}

void TestExternalCommandPlanning() {
	using jpegview_linux::ClipboardBackend;
	using jpegview_linux::ExternalCommand;
	using jpegview_linux::LosslessJpegOperation;
	const fs::path source = fs::path("/tmp/source photo.jpg");
	const fs::path output = fs::path("/tmp/output.jpg");
	ExternalCommand command = jpegview_linux::LosslessJpegCommand(
		LosslessJpegOperation::Rotate90, source, output);
	Expect(command.executable == "jpegtran" && command.arguments ==
		std::vector<std::string>({"-copy", "all", "-rotate", "90", "-outfile",
			output.string(), source.string()}),
		"lossless JPEG rotation plan has incorrect arguments");
	command = jpegview_linux::LosslessJpegCommand(
		LosslessJpegOperation::FlipVertical, source, output);
	Expect(command.arguments[2] == "-flip" && command.arguments[3] == "vertical",
		"lossless JPEG mirror plan has incorrect arguments");
	command = jpegview_linux::LosslessJpegCropCommand(source, output, 16, 32, 640, 480);
	Expect(command.executable == "jpegtran" && command.arguments ==
		std::vector<std::string>({"-copy", "all", "-crop", "640x480+16+32",
			"-outfile", output.string(), source.string()}),
		"lossless JPEG crop plan has incorrect arguments");
	Expect(!jpegview_linux::LosslessJpegCropCommand(source, output, -1, 0, 1, 1).Valid() &&
		!jpegview_linux::LosslessJpegCropCommand(source, output, 0, 0, 0, 1).Valid(),
		"lossless JPEG crop plan accepted invalid geometry");
	Expect(jpegview_linux::PrintCommand(output).executable == "lp" &&
		jpegview_linux::PrintCommand(output).arguments == std::vector<std::string>({output.string()}),
		"print command plan is incorrect");
	const std::string projectUrl = "https://github.com/qusielle/vibe-jpegview-linux?ref=about&source=app";
	const auto openUrl = jpegview_linux::OpenUrlCommands(projectUrl);
	Expect(openUrl.size() == 2 && openUrl[0].executable == "xdg-open" &&
		openUrl[0].arguments == std::vector<std::string>({projectUrl}) &&
		openUrl[1].executable == "gio" &&
		openUrl[1].arguments == std::vector<std::string>({"open", projectUrl}) &&
		jpegview_linux::OpenUrlCommands("").empty(),
		"URL opener fallback plans are incorrect");

	const auto openFolder = jpegview_linux::OpenContainingFolderCommands(fs::path("/tmp/my folder"));
	Expect(openFolder.size() == 2 && openFolder[0].executable == "xdg-open" &&
		openFolder[1].executable == "gio" && openFolder[1].arguments ==
			std::vector<std::string>({"open", "/tmp/my folder"}),
		"open-folder fallback plans are incorrect");
	jpegview_linux::OpenWithApplication application;
	application.name = "Editor";
	application.exec = "photo-editor --new-window %f";
	application.terminal = true;
	auto openWith = jpegview_linux::OpenWithCommand(application, source, true);
	Expect(openWith.has_value() && openWith->executable == "x-terminal-emulator" &&
		openWith->arguments == std::vector<std::string>({"-e", "photo-editor", "--new-window", source.string()}),
		"terminal Open with plan did not wrap the expanded desktop command");
	openWith = jpegview_linux::OpenWithCommand(application, source, false);
	Expect(openWith.has_value() && openWith->executable == "photo-editor" &&
		openWith->arguments == std::vector<std::string>({"--new-window", source.string()}),
		"direct Open with plan did not split executable and arguments");
	application.exec.clear();
	Expect(!jpegview_linux::OpenWithCommand(application, source, true).has_value(),
		"invalid Open with command produced an execution plan");

	const auto wallpaper = jpegview_linux::WallpaperCommandSequences(source);
	Expect(wallpaper.size() == 3 && wallpaper[0].required.executable == "gsettings" &&
		wallpaper[0].required.arguments[2] == "picture-uri" &&
		wallpaper[0].required.arguments.back() == "file:///tmp/source%20photo.jpg" &&
		wallpaper[0].afterSuccess.size() == 1 &&
		wallpaper[0].afterSuccess[0].arguments[2] == "picture-uri-dark" &&
		wallpaper[1].required.executable == "feh" &&
		wallpaper[2].required.executable == "nitrogen",
		"wallpaper backend plan order or GNOME follow-up is incorrect");
	const auto trash = jpegview_linux::TrashCommands(source);
	Expect(trash.size() == 2 && trash[0].arguments ==
		std::vector<std::string>({"trash", source.string()}) &&
		trash[1].executable == "trash-put",
		"trash helper plans are incorrect");

	Expect(jpegview_linux::SelectClipboardBackend(true, true, true) == ClipboardBackend::Wayland &&
		jpegview_linux::SelectClipboardBackend(false, true, true) == ClipboardBackend::Xclip &&
		jpegview_linux::SelectClipboardBackend(false, true, false) == ClipboardBackend::Wayland &&
		jpegview_linux::SelectClipboardBackend(true, false, false) == ClipboardBackend::None,
		"clipboard backend preference/fallback policy is incorrect");
	const ExternalCommand waylandWrite = jpegview_linux::ClipboardWriteCommand(ClipboardBackend::Wayland);
	const ExternalCommand xclipRead = jpegview_linux::ClipboardReadCommand(ClipboardBackend::Xclip);
	Expect(waylandWrite.executable == "wl-copy" && waylandWrite.arguments ==
		std::vector<std::string>({"--type", "image/png"}) &&
		xclipRead.executable == "xclip" && xclipRead.arguments.back() == "-o" &&
		!jpegview_linux::ClipboardReadCommand(ClipboardBackend::None).Valid(),
		"clipboard helper arguments are incorrect");
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

	const fs::path streaming = temporary.path() / "streaming.jpg";
	Expect(::mkfifo(streaming.c_str(), 0600) == 0, "cannot create streaming JPEG fixture");
	const int stream = ::open(streaming.c_str(), O_RDWR | O_CLOEXEC);
	Expect(stream >= 0, "cannot open streaming JPEG fixture");
	auto streamingRead = std::async(std::launch::async, [&streaming] {
		jpegview_linux::ExifInfo streamedInfo;
		std::string streamedComment;
		return std::make_pair(
			jpegview_linux::ReadJpegMetadata(streaming, streamedInfo, streamedComment),
			streamedComment);
	});
	const std::array<std::uint8_t, 12> header = {
		0xff, 0xd8, 0xff, 0xfe, 0x00, 0x06, 't', 'e', 's', 't', 0xff, 0xda
	};
	Expect(::write(stream, header.data(), header.size()) ==
		static_cast<ssize_t>(header.size()), "cannot write streaming JPEG fixture");
	const bool stoppedAtScan = streamingRead.wait_for(std::chrono::seconds(2)) ==
		std::future_status::ready;
	::close(stream);
	const auto streamed = streamingRead.get();
	Expect(stoppedAtScan && streamed.first && streamed.second == "test",
		"JPEG metadata reader consumed compressed scan data instead of stopping at its header");

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
	viewport.ActualSize();
	viewport.Pan(10000.0, 10000.0);
	viewport.ClampToView(1000, 600, 500, 300);
	ExpectRect(viewport.Destination(1000, 600, 500, 300), 0, 0, 1000, 600,
		"viewport clamping did not retain the top-left image edge");
	viewport.Pan(-20000.0, -20000.0);
	viewport.ClampToView(1000, 600, 500, 300);
	ExpectRect(viewport.Destination(1000, 600, 500, 300), -500, -300, 1000, 600,
		"viewport clamping did not retain the bottom-right image edge");
	viewport.Pan(100.0, 100.0);
	viewport.ClampToView(100, 50, 500, 300);
	ExpectRect(viewport.Destination(100, 50, 500, 300), 200, 125, 100, 50,
		"viewport clamping did not center image axes smaller than the view");

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

void TestZoomNavigatorGeometryAndPanning() {
	using namespace jpegview_linux;
	const ZoomNavigatorLayout layout = CalculateZoomNavigatorLayout(1600, 1200,
		0, 0, 1280, 800);
	Expect(layout.hotArea.x == 976 && layout.hotArea.y == 8 &&
		layout.hotArea.width == 296 && layout.hotArea.height == 222 &&
		layout.image.x == layout.hotArea.x && layout.image.y == layout.hotArea.y &&
		layout.image.width == 296 && layout.image.height == 222,
		"zoom navigator did not use its responsive corner layout and image aspect ratio");
	const ZoomNavigatorLayout portrait = CalculateZoomNavigatorLayout(900, 1600,
		10, 5, 1280, 800);
	Expect(portrait.image.height == portrait.hotArea.height &&
		portrait.image.width < portrait.hotArea.width &&
		portrait.image.x > portrait.hotArea.x,
		"portrait overview was not fitted and centered inside its hot area");
	Expect(ImageNeedsZoomNavigator(1600, 1200, 1280, 800) &&
		!ImageNeedsZoomNavigator(800, 600, 1280, 800) &&
		!ImageNeedsZoomNavigator(0, 600, 1280, 800),
		"navigator visibility did not follow image overflow");

	const NormalizedImageRect visible = CalculateVisibleImageRect(
		-160, -200, 1600, 1200, 0, 0, 1280, 800);
	Expect(visible.valid, "zoomed image viewport was not mapped onto the source image");
	ExpectNear(visible.left, 0.1, 0.0001, "navigator viewport left edge is incorrect");
	ExpectNear(visible.top, 1.0 / 6.0, 0.0001, "navigator viewport top edge is incorrect");
	ExpectNear(visible.right, 0.9, 0.0001, "navigator viewport right edge is incorrect");
	ExpectNear(visible.bottom, 5.0 / 6.0, 0.0001, "navigator viewport bottom edge is incorrect");
	const ZoomNavigatorRect mapped = MapVisibleRectToNavigator(visible, layout.image);
	Expect(mapped.x == 1006 && mapped.y == 45 && mapped.width == 236 && mapped.height == 148,
		"navigator visible rectangle was mapped to the wrong thumbnail pixels");
	const ZoomNavigatorPoint mappedPoint = NavigatorPointToImage(1035, 119, layout.image);
	ExpectNear(mappedPoint.x, 59.0 / 296.0, 0.0001,
		"navigator click did not map to the source-image horizontal fraction");
	ExpectNear(mappedPoint.y, 0.5, 0.0001,
		"navigator click did not map to the source-image vertical fraction");

	const ZoomNavigatorPan centering = CalculateNavigatorCenterPan(
		0.5, 0.5, 0.1, 0.5, 0.8, 2.0 / 3.0, 1600, 1200);
	ExpectNear(centering.x, 160.0, 0.0001,
		"navigator click did not clamp the requested center to the visible image bounds");
	ExpectNear(centering.y, 0.0, 0.0001,
		"navigator click moved an axis whose complete image was already visible");
	const ZoomNavigatorPan drag = CalculateNavigatorDragPan(90, -20,
		layout.image, 1600, 1200);
	ExpectNear(drag.x, -90.0 * 1600.0 / 296.0, 0.0001,
		"navigator drag did not scale pan distance by the image overview");
	ExpectNear(drag.y, 20.0 * 1200.0 / 222.0, 0.0001,
		"navigator drag did not scale vertical pan distance by the overview");
	Expect(!CalculateVisibleImageRect(3000, 0, 1600, 1200, 0, 0, 1280, 800).valid &&
		MapVisibleRectToNavigator({}, layout.image).width == 0 &&
		NavigatorPointToImage(0, 0, {}).x == 0.0 &&
		CalculateNavigatorDragPan(10, 10, {}, 1600, 1200).x == 0.0,
		"navigator geometry accepted invalid or fully off-screen input");
}

void TestMagnifyingGlassModelDefaultsBoundsAndWheelDirections() {
	using namespace jpegview_linux;
	MagnifyingGlassModel model;
	Expect(!model.Enabled() && model.Width() == MagnifyingGlassModel::kDefaultWidth &&
		model.Height() == MagnifyingGlassModel::kDefaultHeight &&
		model.ZoomLevel() == MagnifyingGlassModel::kDefaultZoomLevel,
		"magnifying glass did not start with its transient defaults");
	MagnifyingGlassModel restored;
	restored.SetParameters(425, 215, 0.725, 1200, 800);
	Expect(!restored.Enabled() && restored.Width() == 425 && restored.Height() == 215 &&
		std::abs(restored.ZoomLevel() - 0.725) < 1e-12,
		"magnifying-glass parameters did not restore independently of enabled state");
	restored.SetParameters(1, 99999, std::numeric_limits<double>::quiet_NaN(), 400, 200);
	Expect(restored.Width() == MagnifyingGlassModel::kMinimumWidth && restored.Height() == 180 &&
		restored.ZoomLevel() == MagnifyingGlassModel::kDefaultZoomLevel,
		"restored magnifying-glass parameters were not validated and constrained to the view");
	model.Toggle();
	Expect(model.Enabled(), "magnifying glass toggle did not enable the lens");
	model.SetEnabled(false);
	Expect(!model.Enabled(), "magnifying glass enabled state could not be cleared");

	const MagnifyingGlassWheelModifiers plain;
	model.HandleWheel(MagnifyingGlassWheelDirection::Down, plain, 1200, 800);
	Expect(model.Width() == 380 && model.Height() == 190,
		"plain wheel down did not grow both lens dimensions");
	model.HandleWheel(MagnifyingGlassWheelDirection::Up, plain, 1200, 800);
	Expect(model.Width() == 350 && model.Height() == 175,
		"plain wheel up did not shrink both lens dimensions");

	MagnifyingGlassWheelModifiers control;
	control.control = true;
	model.HandleWheel(MagnifyingGlassWheelDirection::Down, control, 1200, 800);
	Expect(model.Width() == 350 && model.Height() == 190,
		"Ctrl+wheel changed a dimension other than lens height");
	model.HandleWheel(MagnifyingGlassWheelDirection::Up, control, 1200, 800);
	MagnifyingGlassWheelModifiers alt;
	alt.alt = true;
	model.HandleWheel(MagnifyingGlassWheelDirection::Down, alt, 1200, 800);
	Expect(model.Width() == 380 && model.Height() == 175,
		"Alt+wheel changed a dimension other than lens width");
	model.HandleWheel(MagnifyingGlassWheelDirection::Up, alt, 1200, 800);

	MagnifyingGlassWheelModifiers shift;
	shift.shift = true;
	model.HandleWheel(MagnifyingGlassWheelDirection::Down, shift, 1200, 800);
	ExpectNear(model.ZoomLevel(), 0.525, 0.000001,
		"Shift+wheel down did not increase the zoom-level value");
	model.HandleWheel(MagnifyingGlassWheelDirection::Up, shift, 1200, 800);
	ExpectNear(model.ZoomLevel(), MagnifyingGlassModel::kDefaultZoomLevel, 0.000001,
		"Shift+wheel up did not decrease the zoom-level value");
	for (int i = 0; i < 40; ++i) {
		model.HandleWheel(MagnifyingGlassWheelDirection::Down, shift, 1200, 800);
	}
	ExpectNear(model.ZoomLevel(), MagnifyingGlassModel::kMaximumZoomLevel, 0.000001,
		"zoom level exceeded its upper bound");
	for (int i = 0; i < 40; ++i) {
		model.HandleWheel(MagnifyingGlassWheelDirection::Up, shift, 1200, 800);
	}
	ExpectNear(model.ZoomLevel(), MagnifyingGlassModel::kMinimumZoomLevel, 0.000001,
		"zoom level exceeded its lower bound");

	MagnifyingGlassModel bounded;
	for (int i = 0; i < 40; ++i) {
		bounded.HandleWheel(MagnifyingGlassWheelDirection::Down, plain, 400, 200);
	}
	Expect(bounded.Width() == 360 && bounded.Height() == 180,
		"lens dimensions exceeded 90 percent of feasible parent dimensions");
	for (int i = 0; i < 40; ++i) {
		bounded.HandleWheel(MagnifyingGlassWheelDirection::Up, plain, 400, 200);
	}
	Expect(bounded.Width() == MagnifyingGlassModel::kMinimumWidth &&
		bounded.Height() == MagnifyingGlassModel::kMinimumHeight,
		"lens dimensions fell below their minimums");
	for (int i = 0; i < 40; ++i) {
		bounded.HandleWheel(MagnifyingGlassWheelDirection::Down, plain, 100, 50);
	}
	Expect(bounded.Width() == MagnifyingGlassModel::kMinimumWidth &&
		bounded.Height() == MagnifyingGlassModel::kMinimumHeight,
		"small parent dimensions incorrectly forced the lens below its minimum size");
}

void TestMagnifyingGlassGeometryMapsNativeTextureAndPadsEdges() {
	using namespace jpegview_linux;
	const MagnifyingGlassRect displayedImage{100, 50, 500, 250};
	const MagnifyingGlassGeometry centered = CalculateMagnifyingGlassGeometry(
		350.0, 175.0, displayedImage, 1000, 500, 350, 175,
		MagnifyingGlassModel::kDefaultZoomLevel);
	Expect(centered.valid, "magnifying glass rejected a pointer inside the displayed image");
	Expect(centered.lensRect.x == 175 && centered.lensRect.y == 88 &&
		centered.lensRect.width == 350 && centered.lensRect.height == 175,
		"magnifying glass lens was not centered on the pointer");
	Expect(centered.sourceRect.x == 325 && centered.sourceRect.y == 162 &&
		centered.sourceRect.width == 350 && centered.sourceRect.height == 176,
		"2x default lens crop did not map through the displayed-image scale");
	Expect(centered.contentDestinationRect.x == centered.lensRect.x &&
		centered.contentDestinationRect.y == centered.lensRect.y &&
		centered.contentDestinationRect.width == centered.lensRect.width &&
		centered.contentDestinationRect.height == centered.lensRect.height,
		"unclipped source crop did not fill the lens content area");

	const MagnifyingGlassGeometry scaledTexture = CalculateMagnifyingGlassGeometry(
		350.0, 175.0, displayedImage, 2000, 1000, 350, 175,
		MagnifyingGlassModel::kDefaultZoomLevel);
	Expect(scaledTexture.valid && scaledTexture.sourceRect.x == 650 &&
		scaledTexture.sourceRect.y == 325 && scaledTexture.sourceRect.width == 700 &&
		scaledTexture.sourceRect.height == 350,
		"native source crop did not account for texture-to-destination scaling");

	const MagnifyingGlassRect fullImage{0, 0, 1000, 500};
	const MagnifyingGlassGeometry left = CalculateMagnifyingGlassGeometry(
		0.0, 250.0, fullImage, 1000, 500, 350, 175, 0.5);
	const MagnifyingGlassGeometry right = CalculateMagnifyingGlassGeometry(
		999.0, 250.0, fullImage, 1000, 500, 350, 175, 0.5);
	const MagnifyingGlassGeometry top = CalculateMagnifyingGlassGeometry(
		500.0, 0.0, fullImage, 1000, 500, 350, 175, 0.5);
	const MagnifyingGlassGeometry bottom = CalculateMagnifyingGlassGeometry(
		500.0, 499.0, fullImage, 1000, 500, 350, 175, 0.5);
	Expect(left.valid && left.sourceRect.x == 0 && left.sourceRect.width < 350 &&
		left.contentDestinationRect.x > left.lensRect.x &&
		left.contentDestinationRect.x + left.contentDestinationRect.width <=
		left.lensRect.x + left.lensRect.width,
		"left source edge was not clipped and padded inside the lens");
	Expect(right.valid && right.sourceRect.x + right.sourceRect.width == 1000 &&
		right.contentDestinationRect.x == right.lensRect.x &&
		right.contentDestinationRect.x + right.contentDestinationRect.width <
		right.lensRect.x + right.lensRect.width,
		"right source edge was not clipped and padded inside the lens");
	Expect(top.valid && top.sourceRect.y == 0 && top.sourceRect.height < 176 &&
		top.contentDestinationRect.y > top.lensRect.y &&
		top.contentDestinationRect.y + top.contentDestinationRect.height <=
		top.lensRect.y + top.lensRect.height,
		"top source edge was not clipped and padded inside the lens");
	Expect(bottom.valid && bottom.sourceRect.y + bottom.sourceRect.height == 500 &&
		bottom.contentDestinationRect.y == bottom.lensRect.y &&
		bottom.contentDestinationRect.y + bottom.contentDestinationRect.height <
		bottom.lensRect.y + bottom.lensRect.height,
		"bottom source edge was not clipped and padded inside the lens");
	Expect(!CalculateMagnifyingGlassGeometry(1000.0, 100.0, fullImage, 1000, 500,
		350, 175, 0.5).valid &&
		!CalculateMagnifyingGlassGeometry(0.0, 250.0, fullImage, 1000, 500,
		350, 175, 0.1).valid,
		"magnifying glass geometry accepted an outside pointer or invalid zoom level");
}

void TestViewportNavigationResetsTransientZoom() {
	jpegview_linux::Viewport viewport;
	viewport.Fit(1000, 600, 500, 300, false, true);
	viewport.ZoomAt(2.0, 250, 150, 1000, 600, 500, 300);
	Expect(std::string(viewport.ScaleMode()) == "manual", "zoom did not affect the current image");
	const jpegview_linux::ViewportSnapshot fitNavigation = viewport.NavigationSnapshot();
	Expect(fitNavigation.fitToWindow && !fitNavigation.fillWithCrop && fitNavigation.noEnlarge,
		"transient zoom replaced the fit navigation mode");
	Expect(std::string(viewport.NavigationScaleMode()) == "fit_no_enlarge",
		"navigation scale-mode serialization followed transient zoom state");
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

void TestResizeDialogController() {
	jpegview_linux::ResizeDialogController dialog;
	dialog.Open(400, 200);
	Expect(dialog.IsOpen() && dialog.FocusedField() == jpegview_linux::ResizeModel::kPercentField,
		"resize dialog did not open on its percentage field");
	dialog.AppendText("50abc.5");
	Expect(dialog.Model().FieldText(jpegview_linux::ResizeModel::kPercentField) == "51" &&
		dialog.Model().FieldText(jpegview_linux::ResizeModel::kWidthField) == "202" &&
		dialog.Model().FieldText(jpegview_linux::ResizeModel::kHeightField) == "101",
		"resize dialog did not filter text or update coupled dimensions");
	dialog.MoveFocus(1);
	Expect(dialog.FocusedField() == jpegview_linux::ResizeModel::kWidthField,
		"resize dialog did not move focus forward");
	dialog.AppendText("100");
	int width = 0;
	int height = 0;
	Expect(dialog.Target(width, height) && width == 100 && height == 50,
		"resize dialog did not replace a primed field and expose its target");
	dialog.SelectAll();
	Expect(dialog.Model().FieldText(jpegview_linux::ResizeModel::kWidthField).empty(),
		"resize dialog select-all did not clear the focused numeric field");
	dialog.AppendText("80");
	dialog.Backspace();
	Expect(dialog.Target(width, height) && width == 8 && height == 4,
		"resize dialog Backspace did not recalculate its target");
	dialog.MoveFocus(-1);
	Expect(dialog.FocusedField() == jpegview_linux::ResizeModel::kPercentField,
		"resize dialog did not move focus backward");
	dialog.SelectField(jpegview_linux::ResizeModel::kFilterField);
	const int previousFilter = dialog.Model().Filter();
	dialog.CycleFilter(1);
	Expect(dialog.FocusedField() == jpegview_linux::ResizeModel::kFilterField &&
		dialog.Model().Filter() == (previousFilter + 1) % jpegview_linux::ResizeModel::kFilterCount,
		"resize dialog did not cycle its filter in place");
	dialog.SetMessage("external failure");
	Expect(dialog.Message() == "external failure", "resize dialog did not retain adapter failure feedback");
	dialog.Close();
	Expect(!dialog.IsOpen() && dialog.Message().empty(),
		"resize dialog did not clear transient state when closed");
}

void TestCropSizeDialogController() {
	using jpegview_linux::CropSizeDialogController;
	CropSizeDialogController dialog;
	dialog.Open(320, 200, true);
	Expect(dialog.IsOpen() && dialog.FocusedField() == CropSizeDialogController::kWidthField &&
		dialog.WidthText() == "320" && dialog.HeightText() == "200" && dialog.UsesScreenPixels(),
		"fixed crop-size dialog did not open with its persisted values and units");
	dialog.AppendText("64px");
	dialog.MoveFocus(-1);
	Expect(dialog.FocusedField() == CropSizeDialogController::kHeightField,
		"fixed crop-size dialog did not cycle focus backward");
	dialog.AppendText("48");
	int width = 0;
	int height = 0;
	bool screenPixels = false;
	Expect(dialog.Apply(width, height, screenPixels) && width == 64 && height == 48 && screenPixels,
		"fixed crop-size dialog did not filter and apply valid dimensions");
	dialog.ToggleUnits();
	Expect(dialog.Apply(width, height, screenPixels) && !screenPixels,
		"fixed crop-size dialog did not preserve the selected pixel unit");
	dialog.SelectField(CropSizeDialogController::kWidthField);
	dialog.SelectAll();
	dialog.AppendText("65536");
	dialog.SelectField(CropSizeDialogController::kHeightField);
	dialog.SelectAll();
	dialog.AppendText("65535");
	Expect(!dialog.Apply(width, height, screenPixels) && !dialog.Message().empty(),
		"fixed crop-size dialog accepted an out-of-range dimension");
	dialog.SelectField(CropSizeDialogController::kWidthField);
	dialog.SelectAll();
	dialog.AppendText("65535");
	Expect(dialog.Apply(width, height, screenPixels) && width == 65535 && height == 65535,
		"fixed crop-size dialog rejected its maximum dimension");
	dialog.SelectField(CropSizeDialogController::kWidthField);
	dialog.Backspace();
	Expect(dialog.WidthText().empty(), "backspace did not clear a newly focused fixed crop field");
	dialog.Close();
	Expect(!dialog.IsOpen() && dialog.Message().empty(),
		"fixed crop-size dialog did not clear transient state on close");
}

void TestArchivePasswordDialogModel() {
	using jpegview_linux::ArchivePasswordDialogModel;
	ArchivePasswordDialogModel dialog;
	Expect(!dialog.IsOpen() && dialog.ArchivePath().empty() && dialog.DisplayText().empty(),
		"archive password dialog did not start closed and empty");

	dialog.Begin("/images/private.zip");
	Expect(dialog.IsOpen() && dialog.ArchivePath() == "/images/private.zip" &&
		dialog.Error().empty(), "archive password dialog did not begin for its archive");
	Expect(dialog.AppendText("p"), "archive password dialog rejected ASCII text");
	Expect(dialog.AppendText("\xc3\xa9"), "archive password dialog rejected valid multibyte UTF-8");
	Expect(dialog.AppendText("\xf0\x9f\x94\x90"), "archive password dialog rejected a four-byte UTF-8 character");
	const std::string bullet = "\xe2\x80\xa2";
	Expect(dialog.DisplayText() == bullet + bullet + bullet,
		"archive password display did not mask one bullet per UTF-8 code point");
	Expect(dialog.DisplayText().find('p') == std::string::npos &&
		dialog.DisplayText().find("\xc3\xa9") == std::string::npos,
		"archive password display exposed entered characters");

	const std::string incompleteUtf8("\xc3", 1);
	Expect(!dialog.AppendText(incompleteUtf8), "archive password dialog accepted incomplete UTF-8");
	Expect(dialog.DisplayText() == bullet + bullet + bullet,
		"malformed UTF-8 partially changed the entered password");
	const std::string embeddedNul("x\0y", 3);
	Expect(!dialog.AppendText(embeddedNul), "archive password dialog accepted embedded NUL bytes");
	Expect(dialog.DisplayText() == bullet + bullet + bullet,
		"embedded NUL input partially changed the entered password");

	Expect(dialog.Backspace() && dialog.DisplayText() == bullet + bullet,
		"archive password backspace did not remove a full multibyte code point");
	Expect(dialog.Backspace() && dialog.DisplayText() == bullet,
		"archive password backspace did not remove the preceding multibyte code point");
	Expect(dialog.Backspace() && dialog.DisplayText().empty(),
		"archive password backspace did not remove the final ASCII character");
	Expect(!dialog.Backspace(), "archive password backspace reported a change for an empty field");

	Expect(dialog.AppendText("first"), "archive password dialog rejected retry text");
	std::optional<std::string> submitted = dialog.Submit();
	Expect(submitted.has_value() && *submitted == "first",
		"archive password submit did not transfer the entered password");
	Expect(!dialog.IsOpen() && dialog.ArchivePath().empty() && dialog.Error().empty() &&
		dialog.DisplayText().empty(), "archive password submit did not close and reset dialog state");
	Expect(!dialog.Submit().has_value(), "closed archive password dialog submitted a second time");
	Expect(!dialog.AppendText("late"), "closed archive password dialog accepted text");

	dialog.Begin("/images/private.7z", "The password was incorrect");
	Expect(dialog.IsOpen() && dialog.ArchivePath() == "/images/private.7z" &&
		dialog.Error() == "The password was incorrect" && dialog.DisplayText().empty(),
		"archive password retry did not show its error with a fresh empty entry");
	Expect(dialog.AppendText("retry") && dialog.Error().empty(),
		"archive password retry did not clear stale error feedback when edited");
	submitted = dialog.Submit();
	Expect(submitted.has_value() && *submitted == "retry",
		"archive password retry did not submit the newly entered password");

	dialog.Begin("/images/private.rar");
	Expect(dialog.AppendText("cancel-me"), "archive password dialog rejected cancel-test text");
	dialog.Cancel();
	Expect(!dialog.IsOpen() && dialog.ArchivePath().empty() && dialog.Error().empty() &&
		dialog.DisplayText().empty() && !dialog.Submit().has_value(),
		"archive password cancel did not clear and close the dialog");

	dialog.Begin("/images/empty-password.zip");
	submitted = dialog.Submit();
	Expect(submitted.has_value() && submitted->empty() && !dialog.IsOpen(),
		"archive password dialog could not submit an intentionally empty password");
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

void TestContextMenuCatalogAndState() {
	using jpegview_linux::ContextMenuState;
	using jpegview_linux::MenuItem;
	const auto findCommand = [](const std::vector<MenuItem>& items, int command) -> const MenuItem* {
		const auto found = std::find_if(items.begin(), items.end(), [command](const MenuItem& item) {
			return item.command == command;
		});
		return found == items.end() ? nullptr : &*found;
	};
	const auto findLabel = [](const std::vector<MenuItem>& items, const std::string& label) -> const MenuItem* {
		const auto found = std::find_if(items.begin(), items.end(), [&label](const MenuItem& item) {
			return item.label == label;
		});
		return found == items.end() ? nullptr : &*found;
	};

	ContextMenuState state;
	const std::vector<MenuItem> compact = jpegview_linux::BuildContextMenu(state, false);
	Expect(findCommand(compact, jpegview_linux::kContextMenuShowAdvanced) != nullptr,
		"compact context menu omitted the one-off advanced-options command");
	Expect(findCommand(compact, IDM_PRINT) == nullptr && findCommand(compact, IDM_LOOP_FOLDER) == nullptr &&
		findCommand(compact, IDM_ZOOM_400) == nullptr && findCommand(compact, IDM_SLIDESHOW_START) == nullptr,
		"compact context menu exposed advanced catalog entries");
	Expect(findCommand(compact, IDM_OPEN) != nullptr && findCommand(compact, IDM_NEXT) != nullptr &&
		findCommand(compact, IDM_ZOOM_100) != nullptr && findCommand(compact, IDM_EXIT) != nullptr,
		"compact context menu lost a primary command");
	Expect(findCommand(compact, IDM_HELP) != nullptr && findCommand(compact, IDM_HELP)->enabled &&
		findCommand(compact, IDM_HELP)->shortcut == "F1",
		"compact context menu omitted the built-in help command");
	Expect(findCommand(compact, jpegview_linux::kCommandEditPictureLevels) != nullptr,
		"compact context menu omitted the picture-level editor");
	Expect(compact.back().command == jpegview_linux::kCommandAdvancedConfiguration &&
		compact.back().label == "Advanced configuration..." && compact.back().enabled,
		"advanced configuration was not the final enabled compact-menu command");
	Expect(jpegview_linux::kCommandAdvancedConfiguration !=
		jpegview_linux::kCommandToggleMagnifyingGlass,
		"advanced configuration reused the magnifying-glass keyboard command ID");
	const MenuItem* compactSelectionMode = findCommand(compact,
		jpegview_linux::kCommandToggleSelectionMode);
	Expect(compactSelectionMode != nullptr && compactSelectionMode->label == "Crop selection mode" &&
		!compactSelectionMode->checked && compactSelectionMode->shortcut == "Ctrl+E",
		"compact context menu did not expose the disabled-by-default crop selection mode");
	const MenuItem* magnifyingGlass = findCommand(compact,
		jpegview_linux::kCommandToggleMagnifyingGlass);
	Expect(magnifyingGlass != nullptr && magnifyingGlass->label == "Magnifying glass" &&
		magnifyingGlass->shortcut == "Z" && !magnifyingGlass->checked &&
		!magnifyingGlass->enabled,
		"context menu did not expose the image-dependent magnifying-glass toggle");
	const MenuItem* doublePageMode = findCommand(compact,
		jpegview_linux::kCommandToggleDoublePageMode);
	const MenuItem* mangaReadingOrder = findCommand(compact,
		jpegview_linux::kCommandToggleMangaReadingOrder);
	Expect(doublePageMode != nullptr && doublePageMode->label == "Double page mode" &&
		doublePageMode->shortcut == "D" && !doublePageMode->checked && !doublePageMode->enabled &&
		mangaReadingOrder != nullptr && mangaReadingOrder->label == "Double page manga mode" &&
		mangaReadingOrder->shortcut == "J" && !mangaReadingOrder->checked &&
		!mangaReadingOrder->enabled,
		"context menu did not expose the image-dependent double-page modes");
	Expect(findCommand(compact, jpegview_linux::kCommandPreviousSiblingFolder) == nullptr &&
		findCommand(compact, jpegview_linux::kCommandNextSiblingFolder) == nullptr,
		"compact context menu exposed advanced sibling-folder navigation commands");

	state.playbackMode = jpegview_linux::PlaybackMode::Movie;
	state.animationAvailable = true;
	state.movieFramesPerSecond = 50.0;
	state.infoVisible = true;
	state.filenameVisible = true;
	state.navigationPanelEnabled = false;
	state.navigationPanelAutoReveal = false;
	state.thumbnailPanelVisible = true;
	state.navigationMode = FileList::NavigationMode::LoopSubDirectories;
	state.sortMode = FileList::SortMode::LastModificationTime;
	state.sortAscending = false;
	state.imageAvailable = true;
	state.magnifyingGlassEnabled = true;
	state.doublePageModeEnabled = true;
	state.mangaReadingOrderEnabled = true;
	state.losslessJpegAvailable = true;
	state.autoCorrectionEnabled = true;
	state.pictureLevelsAvailable = true;
	state.selectionModeEnabled = true;
	state.localDensityEnabled = true;
	state.keepPictureLevels = true;
	state.pictureLevelsSaved = true;
	state.fitToWindow = false;
	state.zoom = 1.0;
	state.fullscreen = true;
	state.borderless = true;
	state.alwaysOnTop = true;
	state.transitionEffect = IDM_EFFECT_BLEND;
	state.transitionDurationMs = 1000;
	state.openWithApplicationNames = {"Photo Editor", u8"写真工具"};
	const std::vector<MenuItem> advanced = jpegview_linux::BuildContextMenu(state, true);
	Expect(advanced.back().command == jpegview_linux::kCommandAdvancedConfiguration &&
		advanced.back().label == "Advanced configuration..." && advanced.back().enabled,
		"advanced configuration was not the final enabled expanded-menu command");
	Expect(findCommand(advanced, jpegview_linux::kCommandToggleDoublePageMode)->checked &&
		findCommand(advanced, jpegview_linux::kCommandToggleMangaReadingOrder)->checked,
		"context menu did not reflect active double-page and manga states");
	Expect(findCommand(advanced, IDM_NEXT)->shortcut == "Left/PgDn" &&
		findCommand(advanced, IDM_PREV)->shortcut == "Right/PgUp",
		"manga context-menu navigation shortcuts did not reflect reversed reading order");

	Expect(findCommand(advanced, jpegview_linux::kContextMenuShowAdvanced) == nullptr,
		"expanded context menu retained the advanced-options command");
	Expect(findCommand(advanced, IDM_PRINT) != nullptr && findCommand(advanced, IDM_LOOP_FOLDER) != nullptr &&
		findCommand(advanced, IDM_ZOOM_400) != nullptr && findCommand(advanced, IDM_SLIDESHOW_START) != nullptr,
		"expanded context menu omitted an advanced catalog section");
	const MenuItem* previousSibling = findCommand(advanced,
		jpegview_linux::kCommandPreviousSiblingFolder);
	const MenuItem* nextSibling = findCommand(advanced,
		jpegview_linux::kCommandNextSiblingFolder);
	Expect(previousSibling != nullptr && previousSibling->label == "  Previous sibling folder" &&
		previousSibling->shortcut == "Alt+Left" && nextSibling != nullptr &&
		nextSibling->label == "  Next sibling folder" && nextSibling->shortcut == "Alt+Right",
		"expanded context menu omitted sibling-folder navigation items or their shortcuts");
	Expect(findCommand(advanced, IDM_SHOW_FILEINFO)->checked &&
		findCommand(advanced, IDM_SHOW_FILENAME)->checked &&
		!findCommand(advanced, IDM_SHOW_NAVPANEL)->checked &&
		findCommand(advanced, jpegview_linux::kCommandToggleThumbnailPanel)->checked &&
		findCommand(advanced, jpegview_linux::kCommandToggleZoomNavigator)->checked,
		"context menu did not reflect panel visibility state");
	Expect(findCommand(advanced, jpegview_linux::kCommandToggleMagnifyingGlass)->enabled &&
		findCommand(advanced, jpegview_linux::kCommandToggleMagnifyingGlass)->checked,
		"context menu did not reflect the active magnifying-glass state");
	Expect(findCommand(advanced, jpegview_linux::kCommandToggleSelectionMode)->checked,
		"context menu did not reflect enabled crop selection mode");
	state.showZoomNavigator = false;
	const std::vector<MenuItem> navigatorHidden = jpegview_linux::BuildContextMenu(state, true);
	Expect(findCommand(navigatorHidden, jpegview_linux::kCommandToggleZoomNavigator) != nullptr &&
		!findCommand(navigatorHidden, jpegview_linux::kCommandToggleZoomNavigator)->checked,
		"context menu did not reflect the disabled zoom navigator setting");
	Expect(findCommand(advanced, IDM_LOOP_RECURSIVELY)->checked &&
		findCommand(advanced, IDM_SORT_MOD_DATE)->checked &&
		findCommand(advanced, IDM_SORT_DESCENDING)->checked &&
		findLabel(advanced, "Current order: D (modification date)") != nullptr,
		"context menu did not reflect navigation and ordering state");
	Expect(findCommand(advanced, IDM_CHANGESIZE)->enabled &&
		findCommand(advanced, IDM_ROTATE_90_LOSSLESS)->enabled &&
		findCommand(advanced, IDM_AUTO_CORRECTION)->checked &&
		findCommand(advanced, IDM_SAVE_PARAMETERS)->enabled &&
		findCommand(advanced, IDM_BACKUP_PARAMDB)->enabled &&
		findCommand(advanced, IDM_RESTORE_PARAMDB)->enabled &&
		findCommand(advanced, IDM_SET_AS_DEFAULT_VIEWER)->enabled &&
		findCommand(advanced, jpegview_linux::kCommandEditPictureLevels)->enabled &&
		findCommand(advanced, IDM_LDC)->checked && findCommand(advanced, IDM_KEEP_PARAMETERS)->checked &&
		!findCommand(advanced, IDM_SAVE_PARAM_DB)->enabled &&
		!findCommand(advanced, IDM_CLEAR_PARAM_DB)->enabled,
		"context menu did not enable image-dependent commands");
	state.keepPictureLevels = false;
	const std::vector<MenuItem> editableDatabase = jpegview_linux::BuildContextMenu(state, true);
	Expect(findCommand(editableDatabase, IDM_SAVE_PARAM_DB)->enabled &&
		findCommand(editableDatabase, IDM_CLEAR_PARAM_DB)->enabled,
		"parameter database actions stayed disabled after keep-between-images was turned off");
	Expect(findCommand(advanced, IDM_ZOOM_100)->checked &&
		findCommand(advanced, IDM_FULL_SCREEN_MODE)->checked &&
		findCommand(advanced, IDM_HIDE_TITLE_BAR)->checked &&
		findCommand(advanced, IDM_ALWAYS_ON_TOP)->checked,
		"context menu did not reflect viewport and window state");
	Expect(findCommand(advanced, IDM_EFFECT_BLEND)->checked &&
		findCommand(advanced, IDM_EFFECTTIME_SLOW)->checked &&
		findCommand(advanced, IDM_MOVIE_50_FPS)->checked,
		"context menu did not reflect playback settings");
	Expect(findLabel(advanced, "  Photo Editor") != nullptr &&
		findCommand(advanced, IDM_FIRST_OPENWITH_CMD)->label == "  Photo Editor" &&
		findCommand(advanced, IDM_FIRST_OPENWITH_CMD + 1)->label == u8"  写真工具",
		"context menu did not own or number dynamic Open with labels");
	Expect(!findCommand(advanced, IDM_EDIT_GLOBAL_CONFIG)->enabled,
		"unsupported settings command unexpectedly became actionable");

	ContextMenuState unavailable;
	unavailable.parameterDatabaseAvailable = false;
	const std::vector<MenuItem> disabled = jpegview_linux::BuildContextMenu(unavailable, true);
	Expect(!findCommand(disabled, IDM_CHANGESIZE)->enabled &&
		!findCommand(disabled, IDM_ROTATE_90_LOSSLESS)->enabled &&
		!findCommand(disabled, IDM_AUTO_CORRECTION)->enabled &&
		!findCommand(disabled, IDM_SAVE_PARAMETERS)->enabled &&
		!findCommand(disabled, IDM_BACKUP_PARAMDB)->enabled &&
		!findCommand(disabled, IDM_RESTORE_PARAMDB)->enabled,
		"image-dependent commands were enabled without an image");
	Expect(findLabel(disabled, "  (no configured applications)") != nullptr,
		"empty Open with state omitted its disabled placeholder");
	ContextMenuState archiveImage;
	archiveImage.archiveMember = true;
	archiveImage.imageAvailable = true;
	archiveImage.openWithApplicationNames.clear();
	const std::vector<MenuItem> archiveMenu = jpegview_linux::BuildContextMenu(archiveImage, true);
	Expect(!findCommand(archiveMenu, IDM_PRINT)->enabled &&
		!findCommand(archiveMenu, IDM_BATCH_COPY)->enabled &&
		!findCommand(archiveMenu, IDM_TOUCH_IMAGE)->enabled &&
		!findCommand(archiveMenu, IDM_TOUCH_IMAGE_EXIF_FOLDER)->enabled &&
		!findCommand(archiveMenu, IDM_SET_WALLPAPER_ORIG)->enabled &&
		findCommand(archiveMenu, IDM_SET_WALLPAPER_DISPLAY)->enabled,
		"filesystem-only actions were not disabled for an archive image");
}

void TestContextMenuMnemonics() {
	using jpegview_linux::ContextMenuState;
	using jpegview_linux::MenuItem;
	ContextMenuState state;
	const std::vector<MenuItem> compact = jpegview_linux::BuildContextMenu(state, false);
	const auto open = std::find_if(compact.begin(), compact.end(), [](const MenuItem& item) {
		return item.command == IDM_OPEN;
	});
	Expect(open != compact.end() && open->mnemonic == 'o' && open->mnemonicOffset == 0,
		"Open image did not receive the expected O mnemonic");
	const int openIndex = static_cast<int>(std::distance(compact.begin(), open));
	Expect(jpegview_linux::MenuMnemonicMatches(compact, 'O') == std::vector<int>({openIndex}),
		"mnemonic matching was not case-insensitive or included ambiguous compact entries");
	for (const MenuItem& item : compact) {
		const bool actionable = !item.separator && item.command != 0 && item.enabled;
		if (!actionable) {
			Expect(item.mnemonic == '\0' && item.mnemonicOffset == -1,
				"separator, heading, or disabled item received a mnemonic");
			continue;
		}
		Expect(item.mnemonic >= 'a' && item.mnemonic <= 'z' && item.mnemonicOffset >= 0 &&
			static_cast<std::size_t>(item.mnemonicOffset) < item.label.size() &&
			std::tolower(static_cast<unsigned char>(item.label[
				static_cast<std::size_t>(item.mnemonicOffset)])) == item.mnemonic,
			"actionable item did not retain the exact character position used by its underline");
	}

	std::vector<MenuItem> repeated;
	for (int command = 1; command <= 6; ++command) {
		repeated.emplace_back("Item", command);
	}
	jpegview_linux::AssignMenuMnemonics(repeated);
	const char repeatedLetter = repeated.front().mnemonic;
	const std::vector<int> matches = jpegview_linux::MenuMnemonicMatches(repeated, repeatedLetter);
	Expect(matches.size() == 2 && matches.front() == 0,
		"mnemonic assignment did not share a letter after the candidate alphabet was exhausted");
	Expect(jpegview_linux::NextMenuMnemonicSelection(repeated, repeatedLetter, -1) == matches[0] &&
		jpegview_linux::NextMenuMnemonicSelection(repeated, repeatedLetter, matches[0]) == matches[1] &&
		jpegview_linux::NextMenuMnemonicSelection(repeated, repeatedLetter, matches[1]) == matches[0],
		"ambiguous mnemonic presses did not cycle matching commands in menu order");
	Expect(jpegview_linux::MenuMnemonicMatches(repeated, '!').empty() &&
		jpegview_linux::NextMenuMnemonicSelection(repeated, '!', -1) == -1,
		"non-letter mnemonic returned a selectable menu item");
}

void TestCropContextMenuCommandsAndModes() {
	using jpegview_linux::ContextMenuState;
	using jpegview_linux::CropSelectionMode;
	using jpegview_linux::MenuItem;
	ContextMenuState state;
	state.cropContextMenu = true;
	state.cropSelectionAvailable = true;
	state.losslessJpegCropAvailable = true;
	state.cropMode = CropSelectionMode::FixedAspect;
	state.cropAspectWidth = 16;
	state.cropAspectHeight = 9;
	state.userCropAspectWidth = 14;
	state.userCropAspectHeight = 11;
	const std::vector<MenuItem> items = jpegview_linux::BuildContextMenu(state, false);
	const auto find = [&items](int command) -> const MenuItem* {
		const auto found = std::find_if(items.begin(), items.end(), [command](const MenuItem& item) {
			return item.command == command;
		});
		return found == items.end() ? nullptr : &*found;
	};
	Expect(find(IDM_CROP_SEL) != nullptr && find(IDM_CROP_SEL)->enabled &&
		find(IDM_LOSSLESS_CROP_SEL) != nullptr && find(IDM_LOSSLESS_CROP_SEL)->enabled &&
		find(IDM_COPY_SEL) != nullptr && find(IDM_COPY_SEL)->enabled &&
		find(IDM_ZOOM_SEL) != nullptr && find(IDM_ZOOM_SEL)->enabled,
		"crop menu omitted an enabled selection action");
	Expect(find(IDM_CROPMODE_FREE) != nullptr &&
		find(IDM_CROPMODE_16_9) != nullptr && find(IDM_CROPMODE_16_9)->checked &&
		find(IDM_CROPMODE_USER) != nullptr &&
		find(IDM_CROPMODE_USER)->label == "  User aspect (14 : 11)",
		"crop menu omitted a mode, checked aspect, or configured user ratio");
	const MenuItem* cropSelectionMode = find(jpegview_linux::kCommandToggleSelectionMode);
	Expect(cropSelectionMode != nullptr && !cropSelectionMode->checked &&
		cropSelectionMode->shortcut == "Ctrl+E" && items.front().command ==
		jpegview_linux::kCommandToggleSelectionMode,
		"selection context menu did not put the crop-mode toggle first and unchecked");
	state.selectionModeEnabled = true;
	const std::vector<MenuItem> enabledModeItems = jpegview_linux::BuildContextMenu(state, false);
	const auto enabledMode = std::find_if(enabledModeItems.begin(), enabledModeItems.end(),
		[](const MenuItem& item) {
			return item.command == jpegview_linux::kCommandToggleSelectionMode;
		});
	Expect(enabledMode != enabledModeItems.end() && enabledMode->checked,
		"selection context menu did not check the active crop mode");
	state.losslessJpegCropAvailable = false;
	state.cropSelectionAvailable = false;
	const std::vector<MenuItem> unavailable = jpegview_linux::BuildContextMenu(state, true);
	const auto findUnavailable = [&unavailable](int command) -> const MenuItem* {
		const auto found = std::find_if(unavailable.begin(), unavailable.end(), [command](const MenuItem& item) {
			return item.command == command;
		});
		return found == unavailable.end() ? nullptr : &*found;
	};
	Expect(findUnavailable(IDM_CROP_SEL) != nullptr && !findUnavailable(IDM_CROP_SEL)->enabled &&
		findUnavailable(IDM_LOSSLESS_CROP_SEL) != nullptr &&
		!findUnavailable(IDM_LOSSLESS_CROP_SEL)->enabled &&
		findUnavailable(IDM_ZOOM_SEL) != nullptr && !findUnavailable(IDM_ZOOM_SEL)->enabled,
		"crop menu left selection actions enabled after the selection became unavailable");
	Expect(findUnavailable(jpegview_linux::kContextMenuShowAdvanced) == nullptr,
		"crop-only menu was incorrectly compacted into advanced options");
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
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[2], -1, -1) == 7,
		"Up with no current item did not enter at the bottom of its column");
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
	Expect(info.x == 4 && info.y == 4 && info.width == 136 && info.height == 66,
		"EXIF overlay was not content-sized");
	Expect(info.textWidth == 100 && info.visibleLines == 3 && info.contentHeight == 66,
		"EXIF overlay margins, toggle room, or visible line count are incorrect");
	info = jpegview_linux::InformationOverlayLayout(100, 3, 500, 300, false,
		4, 6, 18, 20, true);
	Expect(info.width == 268 && info.height == 126 && info.textWidth == 100 &&
		info.visibleLines == 3 && info.contentHeight == 66,
		"expanded EXIF overlay did not reserve the Windows-sized histogram area");

	info = jpegview_linux::InformationOverlayLayout(100, 20, 500, 40, true);
	Expect(info.y == 28, "EXIF overlay did not move below visible filename overlay");
	Expect(info.height == 32 && info.visibleLines == 1,
		"EXIF overlay did not clamp vertically to a small window");

	filename = jpegview_linux::FilenameOverlayLayout(20, 4);
	Expect(filename.width == 1 && filename.textWidth == 1,
		"overlay layout did not remain valid for an extremely narrow window");
}

void TestViewerChromePaintPlans() {
	const jpegview_linux::OverlayLayout filenameLayout{4, 4, 112, 20, 100, 1};
	jpegview_linux::OverlayPaintPlan overlay = jpegview_linux::FilenameOverlayPaint(
		filenameLayout, "[1/2] image.jpg", 11, 6);
	Expect(overlay.panel.x == 4 && overlay.panel.y == 4 && overlay.panel.width == 112 &&
		overlay.panel.height == 20 && overlay.background.alpha == 205 &&
		overlay.text.size() == 1 && overlay.text[0].x == 10 && overlay.text[0].y == 8 &&
		overlay.text[0].color.red == 255,
		"filename overlay paint plan changed panel style or text alignment");
	const jpegview_linux::OverlayLayout infoLayout{4, 28, 160, 48, 100, 2, 48};
	jpegview_linux::InformationOverlayPaintPlan infoPaint = jpegview_linux::InformationOverlayPaint(infoLayout,
		{"heading", "details", "hidden"}, 18, 6);
	Expect(infoPaint.overlay.text.size() == 2 && infoPaint.overlay.text[0].y == 34 &&
		infoPaint.overlay.text[1].y == 52 && infoPaint.overlay.text[0].color.red == 255 &&
		infoPaint.overlay.text[1].color.red == 243 && infoPaint.overlay.text[1].color.green == 242 &&
		infoPaint.overlay.text[1].color.blue == 231 && infoPaint.spectrumButton.width == 18 &&
		infoPaint.spectrumLines.size() == 2 &&
		infoPaint.spectrumLines[0].y1 < infoPaint.spectrumLines[0].y2 &&
		jpegview_linux::Contains(infoPaint.spectrumButton, infoPaint.spectrumButton.x,
			infoPaint.spectrumButton.y) &&
		!jpegview_linux::Contains(infoPaint.spectrumButton,
			infoPaint.spectrumButton.x + infoPaint.spectrumButton.width,
			infoPaint.spectrumButton.y),
		"information overlay paint plan ignored visible lines or emphasis colors");
	jpegview_linux::GrayscaleSpectrum spectrum{};
	spectrum[64] = 16;
	spectrum[192] = 4;
	const jpegview_linux::OverlayLayout spectrumLayout = jpegview_linux::InformationOverlayLayout(
		100, 3, 500, 300, false, 4, 6, 18, 20, true);
	infoPaint = jpegview_linux::InformationOverlayPaint(spectrumLayout,
		{"heading", "details", "hidden"}, 18, 6, true, &spectrum, true);
	Expect(infoPaint.spectrumLines.size() == 5 && infoPaint.spectrumButton.x == 248 &&
		infoPaint.spectrumButton.y == 46 && infoPaint.spectrumLines[2].x1 == 10 &&
		infoPaint.spectrumLines[2].y1 == 120 && infoPaint.spectrumLines[2].x2 == 266 &&
		infoPaint.spectrumLines[2].color.red == 255 &&
		infoPaint.spectrumLines[3].x1 == 74 && infoPaint.spectrumLines[3].y1 == 70 &&
		infoPaint.spectrumLines[3].y2 == 120 &&
		infoPaint.spectrumLines[4].x1 == 202 && infoPaint.spectrumLines[4].y1 == 95 &&
		infoPaint.spectrumLines[4].y2 == 120 &&
		infoPaint.spectrumLines[0].y1 > infoPaint.spectrumLines[0].y2 &&
		infoPaint.spectrumLines[0].color.red == 255,
		"expanded EXIF spectrum or collapse button was laid out incorrectly");

	jpegview_linux::NavigationPanelPaint navigation = jpegview_linux::BuildNavigationPanelPaint(
		800, 600, 341, 585, true, FileList::SortMode::LastModificationTime, 7, 18, 11);
	Expect(navigation.panel.x == 198 && navigation.panel.y == 568 &&
		navigation.panel.width == 403 && navigation.panel.height == 32 &&
		navigation.opacity == 255 && navigation.buttons.size() == 12,
		"navigation paint plan did not keep compact geometry or include both display-mode controls");
	Expect(navigation.buttons[0].rect.x == 204 && navigation.buttons[3].rect.x == 297 &&
		navigation.buttons[4].rect.x == 328 && navigation.buttons[5].rect.x == 367 &&
		navigation.buttons[7].rect.x == 437 && navigation.buttons[9].rect.x == 499 &&
		navigation.buttons[10].rect.x == 538 && navigation.buttons[11].rect.x == 569,
		"navigation paint plan lost section spacing");
	Expect(navigation.buttons[0].command == IDM_FIRST && navigation.buttons[0].lines.size() == 4 &&
		navigation.buttons[0].lines[0].x1 == 211 && navigation.buttons[0].lines[0].y1 == 578 &&
		navigation.buttons[0].lines[0].y2 == 590 &&
		navigation.buttons[0].lines[2].x1 == 223 &&
		navigation.buttons[0].lines[2].x2 == 218 &&
		navigation.buttons[0].foreground.red == 243 &&
		navigation.buttons[0].foreground.green == 242 &&
		navigation.buttons[0].foreground.blue == 231,
		"first-image navigation icon geometry is incorrect");
	Expect(navigation.buttons[4].command == jpegview_linux::kNavigationSortModeCommand &&
		navigation.buttons[4].hovered && navigation.buttons[4].text.size() == 1 &&
		navigation.buttons[4].text[0].text == "D" &&
		navigation.buttons[4].foreground.red == 255 &&
		navigation.buttons[4].foreground.green == 205 &&
		navigation.buttons[4].foreground.blue == 0,
		"navigation sort button did not expose state and hover in its paint plan");
	Expect(navigation.buttons[5].lines.empty() && navigation.buttons[5].text.size() == 1 &&
		navigation.buttons[5].text[0].text == "1:1" &&
		navigation.buttons[6].outlines.size() == 1 &&
		navigation.buttons[6].outlines[0].width == 14 &&
		navigation.buttons[7].lines.size() == 6 &&
		navigation.buttons[7].lines[0].x1 == 442 &&
		navigation.buttons[7].lines[0].x2 == 451 &&
		navigation.buttons[8].lines.size() == 6 &&
		navigation.buttons[9].command == jpegview_linux::kCommandToggleSelectionMode &&
		navigation.buttons[9].lines.size() == 8 &&
		navigation.buttons[10].command == jpegview_linux::kCommandToggleDoublePageMode &&
		navigation.buttons[10].outlines.size() == 2 &&
		navigation.buttons[11].command == jpegview_linux::kCommandToggleMangaReadingOrder &&
		navigation.buttons[11].outlines.size() == 2 && navigation.buttons[11].lines.size() == 7,
		"fit or rotation controls did not use the original Windows action glyphs");
	navigation = jpegview_linux::BuildNavigationPanelPaint(
		800, 600, -1, -1, false, FileList::SortMode::FileName, 7, 18, 11, true, true, true);
	Expect(navigation.opacity == 128 && navigation.buttons[0].foreground.alpha == 128 &&
		navigation.buttons[5].lines.size() == 12 && navigation.buttons[5].text.empty() &&
		navigation.buttons[4].text[0].text == "N" &&
		navigation.buttons[9].foreground.red == 255 &&
		navigation.buttons[9].foreground.green == 205 &&
		navigation.buttons[9].foreground.blue == 0 &&
		navigation.buttons[10].foreground.red == 255 &&
		navigation.buttons[11].foreground.red == 255,
		"fit-action navigation icon or name-order label is incorrect");

	Expect(jpegview_linux::NavigationTooltip(IDM_FULL_SCREEN_MODE, true, false,
		FileList::SortMode::FileName) == "Full screen mode (F11)" &&
		jpegview_linux::NavigationTooltip(IDM_FULL_SCREEN_MODE, true, true,
			FileList::SortMode::FileName) == "Window mode (F11)" &&
		jpegview_linux::NavigationTooltip(IDM_NEXT, true, false,
			FileList::SortMode::FileName, false, false, true) == "Show next image (Left)" &&
		jpegview_linux::NavigationTooltip(IDM_PREV, true, false,
			FileList::SortMode::FileName, false, false, true) == "Show previous image (Right)" &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kNavigationSortModeCommand, true, false,
			FileList::SortMode::LastModificationTime).find("click for file name") != std::string::npos &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kCommandToggleSelectionMode, true, false,
			FileList::SortMode::FileName, false) == "Enable crop selection mode (Ctrl+E)" &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kCommandToggleSelectionMode, true, false,
			FileList::SortMode::FileName, true) == "Disable crop selection mode (Ctrl+E)" &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kCommandToggleDoublePageMode, true, false,
			FileList::SortMode::FileName, false, false) == "Enable double page mode (D)" &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kCommandToggleMangaReadingOrder,
			true, false, FileList::SortMode::FileName, false, true, true) ==
			"Disable double page manga mode (J)",
		"navigation tooltip did not reflect fullscreen or sort state");
	const jpegview_linux::UiRect anchor{2, 5, 40, 40};
	overlay = jpegview_linux::NavigationTooltipPaint(anchor, "tip", 21, 11, 100, 60);
	Expect(overlay.panel.x == 4 && overlay.panel.y == 34 && overlay.panel.width == 37 &&
		overlay.panel.height == 22 && overlay.background.alpha == 215 &&
		overlay.text[0].x == 12,
		"navigation tooltip paint plan did not clamp or flip around its anchor");
	Expect(jpegview_linux::Contains(navigation.buttons[0].rect,
		navigation.buttons[0].rect.x, navigation.buttons[0].rect.y) &&
		!jpegview_linux::Contains(navigation.buttons[0].rect,
			navigation.buttons[0].rect.x + navigation.buttons[0].rect.width,
			navigation.buttons[0].rect.y),
		"viewer chrome hit testing did not use half-open rectangle bounds");
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
	const std::vector<jpegview_linux::ThumbnailSlot> spreadSlots =
		jpegview_linux::ThumbnailPanelSlots(10, 5, 500, 100, std::nullopt, 6);
	Expect(spreadSlots.size() == 5, "spread thumbnail layout omitted visible rows");
	Expect(spreadSlots[2].current && !spreadSlots[2].doublePagePartner &&
		spreadSlots[3].fileIndex == 6 && !spreadSlots[3].current &&
		spreadSlots[3].doublePagePartner && !spreadSlots[4].doublePagePartner,
		"thumbnail panel did not distinguish the active double-page partner");
	Expect(std::count_if(spreadSlots.begin(), spreadSlots.end(), [](const auto& slot) {
		return slot.current || slot.doublePagePartner;
	}) == 2, "thumbnail panel did not highlight exactly both pages in the spread");
	const std::vector<jpegview_linux::ThumbnailSlot> markedSlots =
		jpegview_linux::ThumbnailPanelSlots(10, 5, 500, 100, 7);
	Expect(markedSlots[2].current && !markedSlots[2].marked && markedSlots[4].fileIndex == 7 &&
		markedSlots[4].marked && !markedSlots[4].current,
		"thumbnail panel did not distinguish the marked image from the current image");
	const std::vector<jpegview_linux::ThumbnailSlot> unmarkedVisibleSlots =
		jpegview_linux::ThumbnailPanelSlots(10, 5, 100, 100, 0);
	Expect(std::none_of(unmarkedVisibleSlots.begin(), unmarkedVisibleSlots.end(),
		[](const jpegview_linux::ThumbnailSlot& slot) { return slot.marked; }),
		"thumbnail panel marked an image outside the visible rows");

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
	Expect(jpegview_linux::ThumbnailCacheCapacity(164, 112, 1,
		16u * 1024u * 1024u, 64) == 64,
		"thumbnail cache did not honor its entry limit");
	Expect(jpegview_linux::ThumbnailCacheCapacity(800, 536, 1,
		16u * 1024u * 1024u, 64) == 39,
		"thumbnail cache did not scale down for wide thumbnails");
	Expect(jpegview_linux::ThumbnailCacheCapacity(100, 103, 1, 1, 64) == 1,
		"thumbnail cache did not retain one entry below its pixel budget");
	Expect(jpegview_linux::ThumbnailCacheCapacity(0, 100, 1, 10000, 64) == 0 &&
		jpegview_linux::ThumbnailCacheCapacity(100, 2, 1, 10000, 64) == 0 &&
		jpegview_linux::ThumbnailCacheCapacity(100, 100, 1, 0, 64) == 0 &&
		jpegview_linux::ThumbnailCacheCapacity(100, 100, 1, 10000, 0) == 0,
		"thumbnail cache accepted invalid dimensions or limits");

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

void TestThumbnailCacheSchedulingAndEviction() {
	jpegview_linux::ThumbnailCacheScheduler scheduler;
	const std::vector<std::string> keys = {"a", "b", "c", "d", "e"};
	jpegview_linux::ThumbnailCacheScheduler retained;
	Expect(retained.Prepare(keys, 2, keys.size()).empty(),
		"full-list thumbnail retention unexpectedly evicted an entry");
	for (std::size_t completed = 0; completed < keys.size(); ++completed) {
		const auto retainedRequest = retained.Next(0);
		Expect(retainedRequest.has_value(),
			"full-list thumbnail retention stopped before every file was cached");
		Expect(retained.Complete(*retainedRequest, 0, 0).empty(),
			"full-list thumbnail retention evicted a completed thumbnail");
	}
	Expect(retained.CacheSize() == keys.size() &&
		retained.Prepare(keys, 4, keys.size()).empty() &&
		retained.CacheSize() == keys.size(),
		"navigation dropped thumbnails retained for the active file list");

	Expect(scheduler.Prepare(keys, 2, 3).empty() && scheduler.PendingCount() == 3,
		"thumbnail scheduler did not prepare a capacity-limited queue");
	auto request = scheduler.Next(100);
	Expect(request.has_value() && request->fileIndex == 2 && request->key == "c",
		"thumbnail scheduler did not start with the current image");
	Expect(scheduler.Complete(*request, 100).empty() && scheduler.IsCached("c"),
		"thumbnail scheduler did not record completed work");
	Expect(!scheduler.Next(124).has_value(), "thumbnail scheduler ignored its decode pacing deadline");
	request = scheduler.Next(125);
	Expect(request.has_value() && request->key == "b",
		"thumbnail scheduler did not prefer the preceding equidistant image");
	scheduler.Complete(*request, 125);
	request = scheduler.Next(150);
	Expect(request.has_value() && request->key == "d",
		"thumbnail scheduler did not continue in nearest-first order");
	scheduler.Complete(*request, 150);
	Expect(scheduler.CacheSize() == 3 && scheduler.PendingCount() == 0,
		"thumbnail scheduler cache/work accounting is incorrect");

	const std::vector<std::string> evicted = scheduler.Prepare(keys, 2, 2);
	Expect(evicted == std::vector<std::string>({"b"}) && scheduler.IsCached("c") &&
		scheduler.IsCached("d") && !scheduler.IsCached("b"),
		"thumbnail scheduler did not evict the least-recently-used non-current image");
	request = scheduler.Next(151);
	Expect(request.has_value() && request->key == "b",
		"thumbnail scheduler did not skip retained cache entries and reload an evicted neighbor");
	const jpegview_linux::ThumbnailLoadRequest stale = *request;
	scheduler.Prepare(keys, 4, 2);
	Expect(scheduler.Complete(stale, 151).empty() && !scheduler.IsCached("b"),
		"thumbnail scheduler accepted work from a cancelled generation");

	scheduler.Clear();
	Expect(scheduler.CacheSize() == 0 && scheduler.PendingCount() == 0,
		"thumbnail scheduler clear retained cache or work state");
	scheduler.Prepare({"wrap-a", "wrap-b"}, 0, 2);
	request = scheduler.Next(0xfffffffau);
	Expect(request.has_value() && request->key == "wrap-a", "wraparound pacing fixture did not start");
	scheduler.Complete(*request, 0xfffffffau, 10);
	Expect(!scheduler.Next(3).has_value(), "thumbnail pacing deadline fired early across tick wraparound");
	request = scheduler.Next(4);
	Expect(request.has_value() && request->key == "wrap-b",
		"thumbnail pacing deadline did not fire at tick wraparound");
	scheduler.Complete(*request, 4);
	const std::vector<std::string> zeroCapacityEvictions = scheduler.Prepare({"wrap-a"}, 0, 0);
	Expect(zeroCapacityEvictions.size() == 2 && scheduler.CacheSize() == 0,
		"zero-capacity thumbnail cache retained its protected entry");

	jpegview_linux::ThumbnailCacheScheduler external;
	external.Prepare(keys, 2, 2);
	Expect(external.Store("c").empty() && external.Store("b").empty() &&
		external.Store("e") == std::vector<std::string>({"b"}) && external.IsCached("c") &&
		external.IsCached("e"),
		"thumbnail scheduler did not account for externally prepared pixels");
}

void TestThumbnailBackgroundPreparation() {
	Expect(jpegview_linux::CanReuseDisplayPixelsForThumbnail(1920, 1080, 4u * 1024u * 1024u) &&
		!jpegview_linux::CanReuseDisplayPixelsForThumbnail(8000, 6000, 4u * 1024u * 1024u) &&
		!jpegview_linux::CanReuseDisplayPixelsForThumbnail(0, 1080, 4u * 1024u * 1024u),
		"thumbnail display-source bound accepted an invalid or oversized frame");
	auto source = std::make_shared<jpegview_linux::PreparedDisplayImage>();
	source->width = 4;
	source->height = 4;
	source->bgra = MakeIndexedImage(4, 4).bgra;
	source->bgra[3] = 0;
	source->hasTransparency = true;
	jpegview_linux::ThumbnailPreparationWorker realWorker;
	Expect(realWorker.Request({"scaled", source, 2, 2, 0}),
		"thumbnail worker rejected valid display-ready pixels");
	Expect(realWorker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker did not finish source-area downsampling");
	const auto scaled = realWorker.TakeCompleted(1);
	Expect(scaled.size() == 1 && scaled[0]->key == "scaled" &&
		scaled[0]->width == 2 && scaled[0]->height == 2 && scaled[0]->bgra.size() == 16 &&
		scaled[0]->hasTransparency,
		"thumbnail worker returned incorrect derived pixels");

	std::mutex orderMutex;
	std::condition_variable orderChanged;
	bool blockerStarted = false;
	bool releaseBlocker = false;
	std::vector<std::string> order;
	jpegview_linux::ThumbnailPreparationWorker prioritized(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			{
				std::unique_lock<std::mutex> lock(orderMutex);
				order.push_back(request.key);
				if (request.key == "blocker") {
					blockerStarted = true;
					orderChanged.notify_all();
					orderChanged.wait(lock, [&] { return releaseBlocker; });
				}
			}
			auto result = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			result->key = request.key;
			result->width = result->height = 1;
			result->bgra.assign(4, 255);
			return result;
		});
	Expect(prioritized.Request({"blocker", source, 2, 2, 9}),
		"thumbnail worker rejected its blocking request");
	bool blockerStartedInTime = false;
	{
		std::unique_lock<std::mutex> lock(orderMutex);
		blockerStartedInTime = orderChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return blockerStarted; });
	}
	const bool queuedNeighbors = blockerStartedInTime &&
		prioritized.Request({"far", source, 2, 2, 5}) &&
		prioritized.Request({"near", source, 2, 2, 1});
	{
		std::lock_guard<std::mutex> lock(orderMutex);
		releaseBlocker = true;
	}
	orderChanged.notify_all();
	Expect(blockerStartedInTime, "thumbnail worker did not start in the background");
	Expect(queuedNeighbors, "thumbnail worker rejected queued neighbors");
	Expect(prioritized.WaitUntilIdle(std::chrono::seconds(2)),
		"prioritized thumbnail work did not finish");
	{
		std::lock_guard<std::mutex> lock(orderMutex);
		Expect(order == std::vector<std::string>({"blocker", "near", "far"}),
			"thumbnail worker did not prefer the closest queued neighbor");
	}
	Expect(prioritized.TakeCompleted(3).size() == 3,
		"thumbnail worker did not publish every prepared neighbor");

	std::mutex staleMutex;
	std::condition_variable staleChanged;
	bool staleStarted = false;
	bool releaseStale = false;
	jpegview_linux::ThumbnailPreparationWorker cancellable(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			std::unique_lock<std::mutex> lock(staleMutex);
			staleStarted = true;
			staleChanged.notify_all();
			staleChanged.wait(lock, [&] { return releaseStale; });
			auto result = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			result->key = request.key;
			return result;
		});
	Expect(cancellable.Request({"stale", source, 2, 2, 0}),
		"thumbnail worker rejected cancellation fixture");
	bool staleStartedInTime = false;
	{
		std::unique_lock<std::mutex> lock(staleMutex);
		staleStartedInTime = staleChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return staleStarted; });
	}
	if (staleStartedInTime) cancellable.Clear();
	{
		std::lock_guard<std::mutex> lock(staleMutex);
		releaseStale = true;
	}
	staleChanged.notify_all();
	Expect(staleStartedInTime, "stale thumbnail work did not start");
	Expect(cancellable.WaitUntilIdle(std::chrono::seconds(2)) &&
		cancellable.TakeCompleted(1).empty(),
		"cleared thumbnail work published a stale result");
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
	Expect(!jpegview_linux::DownsampleThumbnailBgra({1, 2, 3, 4}, 2, 2, 1, 1, filtered),
		"thumbnail downsampler accepted a truncated source buffer");
	Expect(!jpegview_linux::DownsampleThumbnailBgra({}, 0, 0, 0, 0, filtered),
		"thumbnail downsampler accepted empty dimensions");
}

void TestGrayscaleSpectrumCalculationAndScaling() {
	const std::vector<std::uint8_t> pixels = {
		0, 0, 0, 255,
		255, 255, 255, 255,
		0, 0, 255, 255,
		0, 255, 0, 255,
		255, 0, 0, 255,
	};
	const jpegview_linux::GrayscaleSpectrum spectrum =
		jpegview_linux::BuildGrayscaleSpectrum(pixels, 5, 1);
	Expect(spectrum[0] == 1 && spectrum[31] == 1 && spectrum[63] == 1 &&
		spectrum[159] == 1 && spectrum[255] == 1,
		"weighted grayscale histogram did not match Windows JPEGView channel weights");
	Expect(jpegview_linux::BuildGrayscaleSpectrum({1, 2, 3}, 1, 1) ==
		jpegview_linux::GrayscaleSpectrum{} &&
		jpegview_linux::BuildGrayscaleSpectrum(pixels, 0, 1) ==
		jpegview_linux::GrayscaleSpectrum{},
		"grayscale histogram accepted invalid image storage or dimensions");

	std::vector<std::uint8_t> largeImage(1000 * 1000 * 4, 127);
	for (std::size_t offset = 3; offset < largeImage.size(); offset += 4) {
		largeImage[offset] = 255;
	}
	const jpegview_linux::GrayscaleSpectrum sampled =
		jpegview_linux::BuildGrayscaleSpectrum(largeImage, 1000, 1000);
	Expect(sampled[127] == 40000 &&
		std::accumulate(sampled.begin(), sampled.end(), std::uint64_t{0}) == 40000,
		"large-image histogram did not use Windows JPEGView's bounded grid sampling");

	jpegview_linux::GrayscaleSpectrum distribution{};
	distribution[80] = 16;
	distribution[200] = 4;
	const auto heights = jpegview_linux::GrayscaleSpectrumBarHeights(distribution, 50);
	Expect(heights[80] == 50 && heights[200] == 25 && heights[0] == 0 &&
		jpegview_linux::GrayscaleSpectrumBarHeights(distribution, 0) ==
		std::array<int, jpegview_linux::kSpectrumBinCount>{},
		"grayscale spectrum bars did not use square-root scaling or handle empty height");
}

void TestImageInfoFormatting() {
	Expect(jpegview_linux::FormatImagePosition(0, 123) == "1/123",
		"single-page position formatting changed");
	Expect(jpegview_linux::FormatImagePosition(0, 123, 1) == "1-2/123",
		"double-page position did not include both visible image numbers");
	Expect(jpegview_linux::FormatImagePosition(8, 123, 7) == "8-9/123",
		"double-page position did not present the pair in file-list order");
	Expect(jpegview_linux::FormatImagePosition(0, 1, 0) == "1/1" &&
		jpegview_linux::FormatImagePosition(0, 1, 1) == "1/1",
		"single-image position formatting added an invalid spread partner");
	Expect(jpegview_linux::FormatImagePosition(0, 0).empty() &&
		jpegview_linux::FormatImagePosition(3, 3).empty(),
		"invalid image-list positions should not produce a visible count");
	Expect(jpegview_linux::FormatImageDimensionsAndSize(1920, 1080, "2.5 MB") ==
		"1920 X 1080, 2.5 MB",
		"image dimensions and file size were not compacted into one line");
	Expect(jpegview_linux::FormatImageDimensionsAndSize(640, 480, {}) == "640 X 480",
		"missing file size left punctuation in the dimensions line");
	Expect(jpegview_linux::FormatModificationDateLine("2026-09-19 12:34:56") ==
		"2026-09-19 12:34:56",
		"modification date popup text still includes a label");
	Expect(jpegview_linux::FormatFileSize(1536) == "1.5 KB",
		"file-size formatting changed while moving it into the information model");
	Expect(jpegview_linux::FormatFileSize(1023) == "1023 B" &&
		jpegview_linux::FormatFileSize(10 * 1024) == "10 KB" &&
		jpegview_linux::FormatFileSize(3ull * 1024 * 1024 * 1024) == "3.0 GB",
		"file-size formatting changed at a unit or precision boundary");
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
	fs::remove(home / ".gtkrc-2.0");
	fs::create_directories(config / "xsettingsd");
	WriteText(config / "xsettingsd/xsettingsd.conf", "Gtk/FontName 'Xsettings Choice 10'\n");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Xsettings Choice 10",
		"xsettingsd system font was not parsed");
	fs::remove(config / "xsettingsd/xsettingsd.conf");
	WriteText(config / "kdeglobals", "[General]\nfont=KDE Choice,11,-1,5,50,0,0,0,0,0\n");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "KDE Choice 11",
		"KDE system font family and size were not parsed");
	fs::remove(config / "kdeglobals");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Sans 10",
		"missing desktop font settings did not use the documented fallback");
	{
		ScopedEnvironment explicitOverride("JPEGVIEW_FONT", "  Override Choice 13  ");
		Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Override Choice 13",
			"JPEGVIEW_FONT did not override desktop settings or was not trimmed");
	}

	jpegview_linux::SystemFont font("Sans 10");
	Expect(font.LineHeight() == jpegview_linux::Terminus9LineHeight() &&
		font.TextWidth("iiii") == font.TextWidth("WWWW") &&
		font.TextWidth("Terminus 9") == jpegview_linux::Terminus9TextWidth("Terminus 9"),
		"printable ASCII did not use the fixed-width 9-point bitmap metrics");
	Expect(jpegview_linux::Terminus9CanRender("Mixed Case 123") &&
		!jpegview_linux::Terminus9CanRender(u8"café"),
		"bitmap-font coverage did not distinguish printable ASCII from Unicode");
	const jpegview_linux::BitmapFontGlyph& uppercase = jpegview_linux::Terminus9Glyph('A');
	const jpegview_linux::BitmapFontGlyph& lowercase = jpegview_linux::Terminus9Glyph('a');
	const jpegview_linux::BitmapGlyphInkBounds narrowGlyph =
		jpegview_linux::Terminus9GlyphInkBounds('i');
	Expect(jpegview_linux::Terminus9LineHeight() == 13 && uppercase.advance == 6 &&
		uppercase.width == 6 && uppercase.height == 12,
		"9-point font did not use the embedded 12-pixel monochrome bitmap strike");
	Expect(narrowGlyph.width > 0 && narrowGlyph.width <
		jpegview_linux::Terminus9Glyph('i').advance && narrowGlyph.left >= 0 &&
		narrowGlyph.left + narrowGlyph.width <= jpegview_linux::Terminus9Glyph('i').width,
		"bitmap glyph ink bounds included blank advance-cell pixels");
	Expect(uppercase.pixelOffset != lowercase.pixelOffset,
		"9-point bitmap font mapped lowercase letters to uppercase glyphs");
	const std::uint8_t expectedAdvance = uppercase.advance;
	for (unsigned int character = 32; character <= 126; ++character) {
		const jpegview_linux::BitmapFontGlyph& glyph = jpegview_linux::Terminus9Glyph(
			static_cast<unsigned char>(character));
		Expect(glyph.advance == expectedAdvance,
			"9-point Terminus bitmap glyphs do not use a fixed-width advance");
		const std::uint8_t* pixels = jpegview_linux::Terminus9GlyphPixels(glyph);
		const std::size_t pixelCount = static_cast<std::size_t>(glyph.width) * glyph.height;
		Expect(std::all_of(pixels, pixels + pixelCount, [](std::uint8_t value) {
			return value == 0 || value == 255;
		}), "embedded Terminus glyph contains antialiased pixel values");
	}
	const jpegview_linux::RasterizedText asciiRaster = font.Rasterize("Mixed Case");
	Expect(asciiRaster.width > 0 && asciiRaster.height == jpegview_linux::Terminus9LineHeight() &&
		!asciiRaster.argb.empty() &&
		std::any_of(asciiRaster.argb.begin(), asciiRaster.argb.end(), [](std::uint32_t pixel) {
			return (pixel >> 24) == 255;
		}) &&
		std::all_of(asciiRaster.argb.begin(), asciiRaster.argb.end(), [](std::uint32_t pixel) {
			return (pixel >> 24) == 0 || (pixel >> 24) == 255;
		}), "9-point ASCII bitmap was not rasterized with crisp one-bit coverage");
	const jpegview_linux::RasterizedText raster = font.Rasterize(u8"Привет — 日本語");
	// Unicode text uses the independently sized system font, not the embedded
	// bitmap font whose fixed line height is returned by SystemFont::LineHeight.
	Expect(raster.width > 0 && raster.height > 0 && !raster.argb.empty(),
		"system font did not rasterize non-Latin UTF-8 text");
	Expect(std::any_of(raster.argb.begin(), raster.argb.end(), [](std::uint32_t pixel) {
		return (pixel >> 24) != 0;
	}), "non-Latin system-font text rasterized as an empty image");
	const std::string invalidUtf8 = std::string("valid") + static_cast<char>(0xff);
	Expect(font.TextWidth(invalidUtf8) > 0 && !font.Rasterize(invalidUtf8).argb.empty(),
		"system font did not replace malformed UTF-8 safely");
}

void TestPlaybackSchedulerTimingAndModes() {
	using jpegview_linux::PlaybackActionType;
	using jpegview_linux::PlaybackMode;
	jpegview_linux::PlaybackScheduler scheduler;
	scheduler.ConfigureImage({20, 30}, 2, true, 100);
	Expect(scheduler.AnimationPlaying() && scheduler.NextTick() == 120,
		"animated image did not schedule its first frame delay");
	Expect(scheduler.Tick(119).type == PlaybackActionType::None,
		"animation advanced before its frame deadline");
	Expect(scheduler.Tick(120).type == PlaybackActionType::ShowFrame &&
		scheduler.FrameIndex() == 1 && scheduler.NextTick() == 150,
		"animation did not advance or schedule the second frame");
	Expect(scheduler.Tick(150).type == PlaybackActionType::ShowFrame &&
		scheduler.FrameIndex() == 0 && scheduler.CompletedLoops() == 1,
		"animation did not wrap after its first loop");
	scheduler.Tick(170);
	Expect(scheduler.Tick(200).type == PlaybackActionType::None &&
		!scheduler.AnimationPlaying() && scheduler.CompletedLoops() == 2,
		"finite animation did not stop after its declared loop count");
	const jpegview_linux::PlaybackAction resumed = scheduler.Resume(300);
	Expect(resumed.type == PlaybackActionType::ShowFrame && resumed.frameIndex == 0 &&
		scheduler.AnimationPlaying() && scheduler.NextTick() == 320,
		"animation resume did not rewind a completed sequence");
	scheduler.FrameDisplayFailed();
	Expect(!scheduler.AnimationPlaying() && scheduler.NextTick() == 0,
		"failed animation frame did not stop scheduling");

	scheduler.ConfigureImage({}, 0, false, 400);
	scheduler.StartMovie(25.0, 400);
	Expect(scheduler.Mode() == PlaybackMode::Movie && scheduler.NextTick() == 440 &&
		scheduler.Tick(439).type == PlaybackActionType::None &&
		scheduler.Tick(440).type == PlaybackActionType::NextImage,
		"movie mode did not advance a static image at its frame interval");
	scheduler.StartMovie(1000.0, 500);
	Expect(scheduler.MovieFramesPerSecond() == 100.0 && scheduler.NextTick() == 510,
		"movie speed or minimum interval was not clamped");

	scheduler.StartSlideshow(0.05, 1000);
	Expect(scheduler.Mode() == PlaybackMode::Slideshow &&
		scheduler.SlideshowSeconds() == 0.1 &&
		scheduler.Tick(1099).type == PlaybackActionType::None &&
		scheduler.Tick(1100).type == PlaybackActionType::NextImage,
		"slideshow delay was not clamped or honored");
	scheduler.NotifyInteraction(1200);
	Expect(scheduler.Tick(1250).type == PlaybackActionType::None,
		"interaction did not postpone slideshow advancement");
	scheduler.Stop(1300);
	Expect(scheduler.Mode() == PlaybackMode::None && scheduler.SlideshowSeconds() == 0.0,
		"stopping playback did not clear active mode state");

	jpegview_linux::PlaybackScheduler wrapping;
	wrapping.ConfigureImage({20, 20}, 0, true, 0xfffffff5u);
	Expect(wrapping.NextTick() == 9 &&
		wrapping.Tick(8).type == PlaybackActionType::None &&
		wrapping.Tick(9).type == PlaybackActionType::ShowFrame,
		"animation deadline comparison failed across tick wraparound");
	wrapping.StartSlideshow(0.1, 0xfffffff0u);
	Expect(wrapping.Tick(83).type == PlaybackActionType::None &&
		wrapping.Tick(84).type == PlaybackActionType::NextImage,
		"slideshow elapsed time failed across tick wraparound");
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

	jpegview_linux::ViewportSnapshot snapshot{false, false, false, 2.75};
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
	recent.RememberViewport(unusualPath, {false, false, false, 0.125});
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
	Expect(defaultPreviewSize.width == 244 && defaultPreviewSize.height == 386 &&
		widerPreviewSize.width == 304 && widerPreviewSize.height == 386 &&
		tinyPreviewSize.width == 1 && tinyPreviewSize.height == 1,
		"preview image target did not follow the pane size and its content insets");

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

	const std::uint64_t resizedGeneration = loader.Request(ppm, false,
		jpegview_linux::FileDialogSortMode::Name, widerPreviewSize.width, widerPreviewSize.height);
	Expect(resizedGeneration > currentGeneration,
		"changing the preview target size did not replace its request");
	results.clear();
	const auto resizedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (results.empty() && std::chrono::steady_clock::now() < resizedDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		results = loader.TakeReady();
	}
	Expect(results.size() == 1 && results[0].generation == resizedGeneration &&
		results[0].width == 304 && results[0].height == 228 && results[0].error.empty(),
		"resized preview request did not return the source image at its new target size");

	loader.Clear();
	Expect(loader.TakeReady().empty(), "clearing the preview loader retained a completed image");
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
	RunTest("archive-browsing-decoding-and-recent-preview",
		TestArchiveBrowsingDecodingAndRecentPreview, failures);
	RunTest("encrypted-zip-browsing-and-session-passwords",
		TestEncryptedZipBrowsingAndSessionPasswords, failures);
	RunTest("encrypted-7z-browsing-and-session-passwords",
		TestEncryptedSevenZipBrowsingAndSessionPasswords, failures);
	RunTest("tar-and-tgz-browsing-decoding-and-safety",
		TestTarAndTgzBrowsingDecodingAndSafety, failures);
	RunTest("7z-browsing-decoding-and-preview", TestSevenZipBrowsingDecodingAndPreview, failures);
	RunTest("rar-browsing-decoding-and-preview", TestRarBrowsingDecodingAndPreview, failures);
	RunTest("encrypted-rar-browsing-and-session-passwords",
		TestEncryptedRarBrowsingAndSessionPasswords, failures);
	RunTest("file-list-marked-image-toggle", TestFileListMarkedImageToggle, failures);
	RunTest("supported-image-extension-policy", TestSupportedImageExtensionPolicy, failures);
	RunTest("keyboard-command-mappings", TestKeyboardCommandMappings, failures);
	RunTest("double-page-pairing-navigation-and-reading-order",
		TestDoublePagePairingNavigationAndReadingOrder, failures);
	RunTest("double-page-presentation-atomic-commit",
		TestDoublePagePresentationCommitsSpreadAtomically, failures);
	RunTest("held-navigation-repeat-coalescing", TestHeldNavigationCoalescesKeyRepeats, failures);
	RunTest("file-list-date-sorting-and-selection", TestFileListDateSortingAndSelectionPreservation, failures);
	RunTest("file-list-size-and-random-sorting", TestFileListSizeAndRandomSorting, failures);
	RunTest("file-list-navigation-modes-and-reload", TestFileListNavigationModesAndReload, failures);
	RunTest("file-list-multiple-inputs", TestFileListMultipleInputs, failures);
	RunTest("file-list-asynchronous-scanning", TestFileListAsynchronousScanning, failures);
	RunTest("image-writer-decoder-round-trips", TestImageWriterDecoderRoundTrips, failures);
	RunTest("pnm-variants", TestPnmVariants, failures);
	RunTest("animated-image-decoders", TestAnimatedImageDecoders, failures);
	RunTest("decoder-failures", TestDecoderFailures, failures);
	RunTest("jpeg-display-decode-scaling", TestJpegDisplayDecodeScaling, failures);
	RunTest("decoded-image-cache-and-background-prefetch", TestDecodedImageCacheAndBackgroundPrefetch, failures);
	RunTest("display-image-cache-background-preparation", TestDisplayImageCacheBackgroundPreparation, failures);
	RunTest("shared-cache-budget-accounting", TestSharedCacheBudgetAccounting, failures);
	RunTest("image-storage-transforms-and-validation", TestImageStorageTransformsAndValidation, failures);
	RunTest("image-crop-copies-half-open-rectangle", TestImageCropCopiesHalfOpenRectangle, failures);
	RunTest("crop-selection-model-geometry-and-manipulation",
		TestCropSelectionModelGeometryAndManipulation, failures);
	RunTest("crop-selection-view-mapping-and-hit-testing",
		TestCropSelectionViewMappingAndHitTesting, failures);
	RunTest("image-resize-filters-and-limits", TestImageResizeFiltersAndLimits, failures);
	RunTest("image-auto-contrast-invariants", TestImageAutoContrastInvariants, failures);
	RunTest("picture-levels-model-and-processing", TestPictureLevelsModelAndProcessing, failures);
	RunTest("picture-levels-store-round-trip", TestPictureLevelsStoreRoundTrip, failures);
	RunTest("settings-round-trip-and-malformed-values", TestSettingsRoundTripAndMalformedValues, failures);
	RunTest("advanced-configuration-model-categories-and-round-trips",
		TestAdvancedConfigurationModelCategoriesAndRoundTrips, failures);
	RunTest("advanced-configuration-model-validation-and-cancellation",
		TestAdvancedConfigurationModelValidationAndCancellation, failures);
	RunTest("transparency-pattern-values-and-tile-colors",
		TestTransparencyPatternValuesAndTileColors, failures);
	RunTest("settings-path-selection", TestSettingsPathSelection, failures);
	RunTest("sort-mode-mappings", TestSortModeMappings, failures);
	RunTest("batch-copy-pattern-expansion-and-preview", TestBatchCopyPatternExpansionAndPreview, failures);
	RunTest("batch-copy-dialog-controller", TestBatchCopyDialogController, failures);
	RunTest("desktop-application-parsing-and-expansion", TestDesktopApplicationParsingAndExecExpansion, failures);
	RunTest("default-viewer-registration", TestDefaultViewerRegistration, failures);
	RunTest("external-command-planning", TestExternalCommandPlanning, failures);
	RunTest("exif-and-jpeg-comment-parsing", TestExifAndJpegCommentParsing, failures);
	RunTest("viewport-modes-and-geometry", TestViewportModesAndGeometry, failures);
	RunTest("viewport-manual-zoom-pan-and-restore", TestViewportManualZoomPanAndRestore, failures);
	RunTest("zoom-navigator-geometry-and-panning", TestZoomNavigatorGeometryAndPanning, failures);
	RunTest("magnifying-glass-model-defaults-bounds-and-wheel-directions",
		TestMagnifyingGlassModelDefaultsBoundsAndWheelDirections, failures);
	RunTest("magnifying-glass-geometry-native-scaling-and-edge-padding",
		TestMagnifyingGlassGeometryMapsNativeTextureAndPadsEdges, failures);
	RunTest("viewport-navigation-resets-transient-zoom", TestViewportNavigationResetsTransientZoom, failures);
	RunTest("resize-model-aspect-ratio-validation-and-filters", TestResizeModelAspectRatioValidationAndFilters, failures);
	RunTest("resize-dialog-controller", TestResizeDialogController, failures);
	RunTest("crop-size-dialog-controller", TestCropSizeDialogController, failures);
	RunTest("archive-password-dialog-model", TestArchivePasswordDialogModel, failures);
	RunTest("context-menu-compaction-and-selection", TestContextMenuCompactionAndSelection, failures);
	RunTest("context-menu-catalog-and-state", TestContextMenuCatalogAndState, failures);
	RunTest("context-menu-letter-mnemonics", TestContextMenuMnemonics, failures);
	RunTest("crop-context-menu-commands-and-modes", TestCropContextMenuCommandsAndModes, failures);
	RunTest("context-menu-column-layout-and-navigation", TestContextMenuColumnLayoutAndNavigation, failures);
	RunTest("overlay-layout-content-width-and-margins", TestOverlayLayoutUsesContentWidthAndComfortableMargins, failures);
	RunTest("viewer-chrome-paint-plans", TestViewerChromePaintPlans, failures);
	RunTest("thumbnail-panel-layout-preload-and-sizing", TestThumbnailPanelLayoutPreloadAndSizing, failures);
	RunTest("thumbnail-cache-scheduling-and-eviction", TestThumbnailCacheSchedulingAndEviction, failures);
	RunTest("thumbnail-background-preparation", TestThumbnailBackgroundPreparation, failures);
	RunTest("thumbnail-downsampling-antialiasing", TestThumbnailDownsamplingAntialiasing, failures);
	RunTest("grayscale-spectrum-calculation-and-scaling", TestGrayscaleSpectrumCalculationAndScaling, failures);
	RunTest("image-info-formatting", TestImageInfoFormatting, failures);
	RunTest("system-font-resolution-and-unicode-rendering", TestSystemFontResolutionAndUnicodeRendering, failures);
	RunTest("playback-scheduler-timing-and-modes", TestPlaybackSchedulerTimingAndModes, failures);
	RunTest("recent-files-mru-uniqueness-persistence-and-viewports",
		TestRecentFilesMruUniquenessPersistenceAndViewportSnapshots, failures);
	RunTest("file-dialog-filtering", TestFileDialogFiltering, failures);
	RunTest("file-dialog-sorting", TestFileDialogSorting, failures);
	RunTest("file-dialog-model-state-and-navigation", TestFileDialogModelStateAndNavigation, failures);
	RunTest("file-dialog-directory-summaries", TestFileDialogDirectorySummaries, failures);
	RunTest("file-dialog-preview-selection-and-background-loading",
		TestFileDialogPreviewSelectionAndBackgroundLoading, failures);
	RunTest("embedded-application-icon", TestEmbeddedApplicationIcon, failures);
	if (failures != 0) {
		std::cerr << failures << " test group(s) failed\n";
		return 1;
	}
	std::cout << "All core tests passed\n";
	return 0;
}
