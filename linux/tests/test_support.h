#pragma once
#include "exif_reader.h"
#include "file_list.h"
#include "file_list_scan_worker.h"
#include "file_list_sort_worker.h"
#include "display_prefetch_planner.h"
#include "display_preparation_controller.h"
#include "exif_metadata_worker.h"
#include "pending_image_intents.h"
#include "image_decoder.h"
#include "image_cache.h"
#include "display_image_cache.h"
#include "display_texture_pins.h"
#include "cache_budget.h"
#include "cache_policy.h"
#include "display_upload_scheduler.h"
#include "image_spectrum_worker.h"
#include "image_document.h"
#include "image_operation_worker.h"
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
#include "external_process.h"
#include "file_operation_service.h"
#include "batch_copy.h"
#include "crop_selection_model.h"
#include "crop_size_dialog_model.h"
#include "go_to_image_number_model.h"
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
#include "thumbnail_repository.h"
#include "thumbnail_resampler.h"
#include "interaction_work_policy.h"
#include "work_batch_gate.h"
#include "source_work_coordinator.h"
#include "perf_diagnostics.h"
#include "event_loop_model.h"
#include "modal_event_router.h"
#include "renderer_thread_resource.h"
#include "renderer_texture_owner.h"
#include "app_icon.h"
#include "image_info_model.h"
#include "spectrum_model.h"
#include "file_dialog_model.h"
#include "recent_files.h"
#include "system_font.h"
#include "bitmap_font.h"
#include "playback_scheduler.h"
#include "double_page_model.h"
#include "presentation_controller.h"
#include "image_session_controller.h"
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
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <zip.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

namespace fs = std::filesystem;
extern std::string gTestExecutablePath;
using jpegview_linux::DecodedImage;
using jpegview_linux::FileList;
using jpegview_linux::FileListPreparedScan;
using jpegview_linux::FileListPreparedSort;
using jpegview_linux::FileListScanResult;
using jpegview_linux::FileListScanWorker;
using jpegview_linux::FileListSortResult;
using jpegview_linux::FileListSortWorker;
using jpegview_linux::ImageWriteOptions;

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace {

class TestFailure : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

struct RendererResourceTestWindow {};
struct RendererResourceTestRenderer {};
std::vector<int> gRendererResourceDestructionOrder;

void DestroyRendererResourceTestWindow(RendererResourceTestWindow* resource) {
	gRendererResourceDestructionOrder.push_back(1);
	delete resource;
}

void DestroyRendererResourceTestRenderer(RendererResourceTestRenderer* resource) {
	gRendererResourceDestructionOrder.push_back(2);
	delete resource;
}

void Expect(bool condition, const std::string& message) {
	if (!condition) throw TestFailure(message);
}

struct SourceWorkFailureInjection {
	jpegview_linux::detail::SourceWorkTestHookPoint point;
	std::atomic<bool> fired{false};
};

void ThrowAtSourceWorkHook(jpegview_linux::detail::SourceWorkTestHookPoint point,
	void* context) {
	auto* injection = static_cast<SourceWorkFailureInjection*>(context);
	if (injection != nullptr && point == injection->point &&
		!injection->fired.exchange(true)) {
		throw std::bad_alloc();
	}
}

struct SourceWorkRegistrationCounter {
	std::atomic<int> sourceRegistrations{0};
	std::atomic<bool> threwOnNestedRegistration{false};
};

void ThrowOnNestedSourceRegistration(
	jpegview_linux::detail::SourceWorkTestHookPoint point, void* context) {
	auto* counter = static_cast<SourceWorkRegistrationCounter*>(context);
	if (counter != nullptr && point ==
		jpegview_linux::detail::SourceWorkTestHookPoint::InitialSourceQueueRegistration &&
		counter->sourceRegistrations.fetch_add(1) == 1) {
		counter->threwOnNestedRegistration.store(true);
		throw std::bad_alloc();
	}
}

struct ArchiveSourceProbeObservation {
	std::atomic<int> locationClassification{0};
	std::atomic<int> unadmittedLocationClassification{0};
	std::atomic<int> passwordIdentity{0};
	std::atomic<int> unadmittedPasswordIdentity{0};
};

struct ArchiveSourceProbeBarrier {
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false;
	bool release = false;
	bool sourceAdmitted = false;
	bool cpuAdmitted = false;
};

void BlockArchiveClassificationProbe(jpegview_linux::detail::ArchiveSourceProbePoint point,
	const fs::path&, bool sourceAdmitted, bool cpuAdmitted, void* context) {
	if (point != jpegview_linux::detail::ArchiveSourceProbePoint::LocationClassificationStat) {
		return;
	}
	auto* barrier = static_cast<ArchiveSourceProbeBarrier*>(context);
	if (barrier == nullptr) return;
	std::unique_lock<std::mutex> lock(barrier->mutex);
	if (barrier->entered) return;
	barrier->entered = true;
	barrier->sourceAdmitted = sourceAdmitted;
	barrier->cpuAdmitted = cpuAdmitted;
	barrier->changed.notify_all();
	(void)barrier->changed.wait_for(lock, std::chrono::seconds(5), [&] {
		return barrier->release;
	});
}

void ObserveArchiveSourceProbe(jpegview_linux::detail::ArchiveSourceProbePoint point,
	const fs::path&, bool sourceAdmitted, bool cpuAdmitted, void* context) {
	auto* observation = static_cast<ArchiveSourceProbeObservation*>(context);
	if (observation == nullptr) return;
	if (point == jpegview_linux::detail::ArchiveSourceProbePoint::LocationClassificationStat) {
		observation->locationClassification.fetch_add(1);
		if (!sourceAdmitted) observation->unadmittedLocationClassification.fetch_add(1);
	} else if (point == jpegview_linux::detail::ArchiveSourceProbePoint::PasswordCacheIdentityStat) {
		observation->passwordIdentity.fetch_add(1);
		if (!sourceAdmitted || !cpuAdmitted) {
			observation->unadmittedPasswordIdentity.fetch_add(1);
		}
	}
}

struct SourceIdentityStatObservation {
	std::atomic<int> statxAttempts{0};
	std::atomic<int> completedStatx{0};
	std::atomic<int> fallbackStats{0};
};

void ObserveSourceIdentityStat(jpegview_linux::detail::ArchiveSourceProbePoint point,
	const fs::path&, bool, bool, void* context) {
	auto* observation = static_cast<SourceIdentityStatObservation*>(context);
	if (observation == nullptr) return;
	if (point == jpegview_linux::detail::ArchiveSourceProbePoint::SourceIdentityStatxAttempt) {
		observation->statxAttempts.fetch_add(1);
	} else if (point == jpegview_linux::detail::ArchiveSourceProbePoint::SourceIdentityStatxComplete) {
		observation->completedStatx.fetch_add(1);
	} else if (point == jpegview_linux::detail::ArchiveSourceProbePoint::SourceIdentityStatFallback) {
		observation->fallbackStats.fetch_add(1);
	}
}

struct ArchiveSourceProbeHookReset {
	~ArchiveSourceProbeHookReset() {
		jpegview_linux::SetArchiveSourceProbeHookForTesting(nullptr, nullptr);
	}
};

struct ArchiveCatalogFailureInjection {
	std::atomic<bool> fired{false};
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false;
	bool release = false;
};

void ThrowAtArchiveCatalogHook(jpegview_linux::detail::ArchiveCatalogTestHookPoint,
	void* context) {
	auto* injection = static_cast<ArchiveCatalogFailureInjection*>(context);
	if (injection != nullptr && !injection->fired.exchange(true)) {
		{
			std::unique_lock<std::mutex> lock(injection->mutex);
			injection->entered = true;
			injection->changed.notify_all();
			(void)injection->changed.wait_for(lock, std::chrono::seconds(5), [&] {
				return injection->release;
			});
		}
		throw std::bad_alloc();
	}
}

template <typename Mutex, typename Condition, typename Flag>
class ScopedConditionRelease {
public:
	ScopedConditionRelease(Mutex& mutex, Condition& condition, Flag& flag)
		: mutex_(mutex), condition_(condition), flag_(flag) {}
	~ScopedConditionRelease() {
		{
			std::lock_guard<Mutex> lock(mutex_);
			flag_ = true;
		}
		condition_.notify_all();
	}
	ScopedConditionRelease(const ScopedConditionRelease&) = delete;
	ScopedConditionRelease& operator=(const ScopedConditionRelease&) = delete;

private:
	Mutex& mutex_;
	Condition& condition_;
	Flag& flag_;
};

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

bool HasCodecSourceReadTrace(const char* codec, std::uint64_t maximumBytes) {
	const char* tracePath = std::getenv("JPEGVIEW_PERF_TRACE");
	if (tracePath == nullptr || *tracePath == '\0') return true;
	const std::string expectedDetail = std::string("\"") + codec + "\"";
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	do {
		std::ifstream trace(tracePath);
		std::string line;
		while (std::getline(trace, line)) {
			std::array<std::string, 13> fields{};
			std::size_t start = 0;
			bool complete = true;
			for (std::size_t index = 0; index < fields.size(); ++index) {
				const std::size_t comma = line.find(',', start);
				if (index + 1 == fields.size()) {
					fields[index] = line.substr(start);
				} else if (comma != std::string::npos) {
					fields[index] = line.substr(start, comma - start);
					start = comma + 1;
				} else {
					complete = false;
					break;
				}
			}
			if (!complete || fields[1] != "source_read" || fields[3] != "worker_thread" ||
				fields[5] != "focused_preview" || fields[12] != expectedDetail) continue;
			try {
				const std::uint64_t durationUs = std::stoull(fields[2]);
				const std::uint64_t bytes = std::stoull(fields[6]);
				const std::uint64_t calls = std::stoull(fields[7]);
				if (durationUs > 0 && bytes > 0 && bytes <= maximumBytes && calls > 0 &&
					fields[4] != "0") return true;
			} catch (const std::exception&) {
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
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

void WriteZipArchiveWithImageMembers(const fs::path& archivePath, std::size_t count) {
	int errorCode = 0;
	zip_t* archive = zip_open(archivePath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
	if (archive == nullptr) throw TestFailure("cannot create large ZIP archive: " +
		std::to_string(errorCode));
	static constexpr char imageBytes[] = "x";
	for (std::size_t index = 0; index < count; ++index) {
		const std::string name = "image-" + std::to_string(index) + ".jpg";
		zip_source_t* source = zip_source_buffer(archive, imageBytes,
			sizeof(imageBytes) - 1, 0);
		if (source == nullptr) {
			const std::string message = zip_strerror(archive);
			zip_discard(archive);
			throw TestFailure("cannot add large ZIP fixture source: " + message);
		}
		if (zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
			const std::string message = zip_strerror(archive);
			zip_source_free(source);
			zip_discard(archive);
			throw TestFailure("cannot add large ZIP fixture member: " + message);
		}
	}
	if (zip_close(archive) != 0) {
		const std::string message = zip_strerror(archive);
		zip_discard(archive);
		throw TestFailure("cannot finish large ZIP fixture: " + message);
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

void SetModificationTimeNanoseconds(const fs::path& filename, std::int64_t seconds,
	long nanoseconds) {
	timespec times[2]{};
	times[0].tv_nsec = UTIME_OMIT;
	times[1].tv_sec = static_cast<time_t>(seconds);
	times[1].tv_nsec = nanoseconds;
	Expect(::utimensat(AT_FDCWD, filename.c_str(), times, 0) == 0,
		"cannot set precise test timestamp for " + filename.string());
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

std::shared_ptr<DecodedImage> CachedTestImage(std::size_t bytes) {
	auto image = std::make_shared<DecodedImage>();
	jpegview_linux::DecodedFrame frame;
	frame.width = static_cast<int>(bytes / 4);
	frame.height = 1;
	frame.bgra.assign(bytes, 127);
	image->frames.push_back(std::move(frame));
	return image;
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

std::shared_ptr<const jpegview_linux::PreparedDisplayImage>
DisplayCachePreparedTestImage(const jpegview_linux::DisplayImageRequest& request) {
	auto image = std::make_shared<jpegview_linux::PreparedDisplayImage>();
	image->filename = request.filename;
	image->source = request.source;
	image->cacheKey = request.cacheKey;
	image->key = request.key;
	image->width = request.targetWidth;
	image->height = request.targetHeight;
	const std::size_t bytes = static_cast<std::size_t>(image->width) *
		static_cast<std::size_t>(image->height) * 4;
	image->bgra.assign(bytes, 255);
	return image;
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

} // namespace

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
