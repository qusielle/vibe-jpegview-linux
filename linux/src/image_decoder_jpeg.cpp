#include "image_decoder_internal.h"

#include "archive_source.h"
#include "exif_orientation.h"
#include "perf_diagnostics.h"
#include "tiff_metadata_reader.h"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <jpeglib.h>
}

// JCS_EXT_BGRA is an enum value, so detect it through libjpeg's alpha
// extension macro instead of testing the color-space name with #ifdef.
#if defined(JCS_ALPHA_EXTENSIONS)
#define JPEGVIEW_LIBJPEG_HAS_BGRA 1
#else
#define JPEGVIEW_LIBJPEG_HAS_BGRA 0
#endif

namespace jpegview_linux::decoder_detail {

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
namespace {
JpegInputOpenedTestHook jpegInputOpenedTestHook = nullptr;
void* jpegInputOpenedTestContext = nullptr;
}

void SetJpegInputOpenedTestHookForTesting(JpegInputOpenedTestHook hook, void* context) {
	jpegInputOpenedTestHook = hook;
	jpegInputOpenedTestContext = context;
}
#endif

struct FileInput {
	std::FILE* file = nullptr;

	FileInput() = default;
	FileInput(const FileInput&) = delete;
	FileInput& operator=(const FileInput&) = delete;
	~FileInput() { Reset(); }

	void Reset() noexcept {
		if (file != nullptr) (void)std::fclose(file);
		file = nullptr;
	}
};

bool OpenInput(const std::filesystem::path& filename, FileInput& input,
	std::string& errorMessage) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::SourceOpen);
	const int descriptor = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		errorMessage = "cannot open file";
		return false;
	}
	struct stat status{};
	if (::fstat(descriptor, &status) != 0) {
		::close(descriptor);
		errorMessage = "cannot read file";
		return false;
	}
	if (status.st_size <= 0) {
		::close(descriptor);
		errorMessage = "empty file";
		return false;
	}
	input.file = ::fdopen(descriptor, "rb");
	if (input.file == nullptr) {
		::close(descriptor);
		errorMessage = "cannot open file stream";
		return false;
	}
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	if (jpegInputOpenedTestHook != nullptr) {
		jpegInputOpenedTestHook(filename, jpegInputOpenedTestContext);
	}
#endif
	return true;
}

void CloseInput(FileInput& input) {
	input.Reset();
}

struct JpegErrorManager {
	jpeg_error_mgr base{};
	jmp_buf jump{};
	char message[JMSG_LENGTH_MAX]{};
};

void JpegErrorExit(j_common_ptr common) {
	JpegErrorManager* error = reinterpret_cast<JpegErrorManager*>(common->err);
	(*common->err->format_message)(common, error->message);
	longjmp(error->jump, 1);
}

void SuppressJpegMessage(j_common_ptr) {}

void ConvertCmykSampleToBgra(std::uint8_t* sample, bool adobeInverted) {
	const unsigned cyan = sample[0];
	const unsigned magenta = sample[1];
	const unsigned yellow = sample[2];
	const unsigned black = sample[3];
	const auto combine = [adobeInverted, black](unsigned colorant) {
		const unsigned ink = adobeInverted ? colorant : 255u - colorant;
		const unsigned blackInk = adobeInverted ? black : 255u - black;
		return static_cast<std::uint8_t>((ink * blackInk + 127u) / 255u);
	};
	sample[0] = combine(yellow);
	sample[1] = combine(magenta);
	sample[2] = combine(cyan);
	sample[3] = 255;
}

int ReadJpegExifOrientation(const jpeg_decompress_struct& decoder,
	const WorkContext& context) {
	for (jpeg_saved_marker_ptr marker = decoder.marker_list;
		marker != nullptr; marker = marker->next) {
		if (marker->marker != JPEG_APP0 + 1 || marker->data_length < 6 ||
			std::memcmp(marker->data, "Exif\0\0", 6) != 0) continue;
		try {
			ExifInfo metadata;
			if (ReadTiffMetadata(marker->data, marker->data_length, 6,
				metadata, context) && metadata.hasExif &&
				IsValidExifOrientation(metadata.imageOrientation)) {
				return metadata.imageOrientation;
			}
		} catch (...) {
			// Invalid optional metadata must not make an otherwise valid JPEG fail.
		}
		if (!context.Continue()) return 1;
	}
	return 1;
}

bool ReadJpegSize(const std::filesystem::path& filename, int& width, int& height,
	std::string& errorMessage, const WorkContext& context) {
	if (!context.Continue()) {
		errorMessage = "JPEG metadata read was cancelled";
		return false;
	}
	FileInput input;
	if (!OpenInput(filename, input, errorMessage)) return false;
	JpegErrorManager error{};
	jpeg_decompress_struct decoder{};
	volatile bool created = false;
	decoder.err = jpeg_std_error(&error.base);
	error.base.error_exit = JpegErrorExit;
	error.base.output_message = SuppressJpegMessage;
	// libjpeg reports failures through longjmp, so this branch must explicitly release its C resources.
	if (setjmp(error.jump) != 0) {
		if (created) jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		errorMessage = error.message[0] == '\0' ? "invalid JPEG header" : error.message;
		return false;
	}
	jpeg_create_decompress(&decoder);
	created = true;
	jpeg_stdio_src(&decoder, input.file);
	jpeg_save_markers(&decoder, JPEG_APP0 + 1, 0xFFFFu);
	const bool headerValid = jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK;
	const int orientation = headerValid ? ReadJpegExifOrientation(decoder, context) : 1;
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		errorMessage = "JPEG metadata read was cancelled";
		return false;
	}
	const bool valid = headerValid &&
		ValidDimensions(static_cast<int>(decoder.image_width),
			static_cast<int>(decoder.image_height), errorMessage);
	if (valid) {
		const int storedWidth = static_cast<int>(decoder.image_width);
		const int storedHeight = static_cast<int>(decoder.image_height);
		width = ExifOrientationSwapsAxes(orientation) ? storedHeight : storedWidth;
		height = ExifOrientationSwapsAxes(orientation) ? storedWidth : storedHeight;
	}
	jpeg_destroy_decompress(&decoder);
	CloseInput(input);
	if (!valid && errorMessage.empty()) errorMessage = "invalid JPEG header";
	return valid;
}

bool DecodeJpeg(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage, int minimumWidth, int minimumHeight,
	int* sourceWidth, int* sourceHeight, const WorkContext& context) {
	if (!context.Continue()) {
		errorMessage = "JPEG decode was cancelled";
		return false;
	}
	FileInput input;
	if (!OpenInput(filename, input, errorMessage)) return false;
	JpegErrorManager error{};
	jpeg_decompress_struct decoder{};
	volatile bool created = false;
#if JPEGVIEW_LIBJPEG_HAS_BGRA
	DecodedFrame* volatile directFrame = nullptr;
#else
	volatile std::uint8_t* pixels = nullptr;
#endif
	decoder.err = jpeg_std_error(&error.base);
	error.base.error_exit = JpegErrorExit;
	error.base.output_message = SuppressJpegMessage;
	// Keep libjpeg-owned state and the volatile pixel owner explicit across its longjmp error path.
	if (setjmp(error.jump) != 0) {
		if (created) jpeg_destroy_decompress(&decoder);
#if JPEGVIEW_LIBJPEG_HAS_BGRA
		delete const_cast<DecodedFrame*>(directFrame);
#else
		std::free(const_cast<std::uint8_t*>(pixels));
#endif
		CloseInput(input);
		errorMessage = error.message[0] == '\0' ? "JPEG decoder failed" : error.message;
		return false;
	}

	jpeg_create_decompress(&decoder);
	created = true;
	jpeg_stdio_src(&decoder, input.file);
	jpeg_save_markers(&decoder, JPEG_APP0 + 1, 0xFFFFu);
	const bool headerValid = jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK;
	const int orientation = headerValid ? ReadJpegExifOrientation(decoder, context) : 1;
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		errorMessage = "JPEG decode was cancelled";
		return false;
	}
	if (!headerValid ||
		!ValidDimensions(static_cast<int>(decoder.image_width),
			static_cast<int>(decoder.image_height), errorMessage)) {
		jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		if (errorMessage.empty()) errorMessage = "invalid JPEG header";
		return false;
	}
	const int storedWidth = static_cast<int>(decoder.image_width);
	const int storedHeight = static_cast<int>(decoder.image_height);
	const bool cmykSource = decoder.jpeg_color_space == JCS_CMYK ||
		decoder.jpeg_color_space == JCS_YCCK;
	const bool adobeInvertedCmyk = decoder.saw_Adobe_marker != FALSE;
	if (sourceWidth != nullptr) {
		*sourceWidth = ExifOrientationSwapsAxes(orientation) ? storedHeight : storedWidth;
	}
	if (sourceHeight != nullptr) {
		*sourceHeight = ExifOrientationSwapsAxes(orientation) ? storedWidth : storedHeight;
	}
	if (minimumWidth > 0 && minimumHeight > 0) {
		const unsigned int rawMinimumWidth = static_cast<unsigned int>(
			ExifOrientationSwapsAxes(orientation) ? minimumHeight : minimumWidth);
		const unsigned int rawMinimumHeight = static_cast<unsigned int>(
			ExifOrientationSwapsAxes(orientation) ? minimumWidth : minimumHeight);
		for (const unsigned int denominator : {8u, 4u, 2u}) {
			const unsigned int scaledWidth =
				(decoder.image_width + denominator - 1) / denominator;
			const unsigned int scaledHeight =
				(decoder.image_height + denominator - 1) / denominator;
			if (scaledWidth >= rawMinimumWidth && scaledHeight >= rawMinimumHeight) {
				decoder.scale_num = 1;
				decoder.scale_denom = denominator;
				break;
			}
		}
	}

#if JPEGVIEW_LIBJPEG_HAS_BGRA
	decoder.out_color_space = cmykSource ? JCS_CMYK : JCS_EXT_BGRA;
	const int outputComponents = 4;
#else
	decoder.out_color_space = cmykSource ? JCS_CMYK : JCS_RGB;
	const int outputComponents = cmykSource ? 4 : 3;
#endif
	jpeg_start_decompress(&decoder);
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		errorMessage = "JPEG decode was cancelled";
		return false;
	}
	const int width = static_cast<int>(decoder.output_width);
	const int height = static_cast<int>(decoder.output_height);
	const std::size_t rowBytes = static_cast<std::size_t>(width) * outputComponents;
	const std::size_t byteCount = rowBytes * static_cast<std::size_t>(height);
#if JPEGVIEW_LIBJPEG_HAS_BGRA
	try {
		directFrame = new DecodedFrame();
		DecodedFrame* frame = const_cast<DecodedFrame*>(directFrame);
		frame->width = width;
		frame->height = height;
		frame->bgra.resize(byteCount);
	} catch (const std::exception&) {
		delete const_cast<DecodedFrame*>(directFrame);
		directFrame = nullptr;
		jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		errorMessage = "out of memory";
		return false;
	}
#else
	pixels = static_cast<std::uint8_t*>(std::malloc(byteCount));
	if (pixels == nullptr) {
		jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		errorMessage = "out of memory";
		return false;
	}
#endif
	while (decoder.output_scanline < decoder.output_height) {
		if ((decoder.output_scanline & 15u) == 0 && !context.Continue()) {
			jpeg_destroy_decompress(&decoder);
			created = false;
#if JPEGVIEW_LIBJPEG_HAS_BGRA
			delete const_cast<DecodedFrame*>(directFrame);
#else
			std::free(const_cast<std::uint8_t*>(pixels));
#endif
			CloseInput(input);
			errorMessage = "JPEG decode was cancelled";
			return false;
		}
#if JPEGVIEW_LIBJPEG_HAS_BGRA
		DecodedFrame* frame = const_cast<DecodedFrame*>(directFrame);
		JSAMPROW row = frame->bgra.data() +
			static_cast<std::size_t>(decoder.output_scanline) * rowBytes;
#else
		JSAMPROW row = const_cast<std::uint8_t*>(pixels) +
			static_cast<std::size_t>(decoder.output_scanline) * rowBytes;
#endif
		jpeg_read_scanlines(&decoder, &row, 1);
	}
	jpeg_finish_decompress(&decoder);
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		created = false;
#if JPEGVIEW_LIBJPEG_HAS_BGRA
		delete const_cast<DecodedFrame*>(directFrame);
#else
		std::free(const_cast<std::uint8_t*>(pixels));
#endif
		CloseInput(input);
		errorMessage = "JPEG decode was cancelled";
		return false;
	}
	jpeg_destroy_decompress(&decoder);
	created = false;
	CloseInput(input);

#if JPEGVIEW_LIBJPEG_HAS_BGRA
	try {
		DecodedFrame* frame = const_cast<DecodedFrame*>(directFrame);
		if (cmykSource) {
			for (int y = 0; y < frame->height; ++y) {
				if ((y & 15) == 0 && !context.Continue()) {
					delete frame;
				directFrame = nullptr;
					errorMessage = "JPEG decode was cancelled";
					return false;
				}
				std::uint8_t* row = frame->bgra.data() +
					static_cast<std::size_t>(y) * frame->width * 4;
				for (int x = 0; x < frame->width; ++x) {
					ConvertCmykSampleToBgra(row + static_cast<std::size_t>(x) * 4,
						adobeInvertedCmyk);
				}
			}
		}
		if (!context.Continue()) {
			delete frame;
			directFrame = nullptr;
			errorMessage = "JPEG decode was cancelled";
			return false;
		}
		if (!ApplyExifOrientation(orientation, frame->width, frame->height,
			frame->bgra, errorMessage, [&context] { return context.Continue(); })) {
			delete frame;
			directFrame = nullptr;
			return false;
		}
		image.frames.push_back(std::move(*frame));
		delete frame;
		directFrame = nullptr;
		return true;
	} catch (const std::exception&) {
		delete const_cast<DecodedFrame*>(directFrame);
		errorMessage = "out of memory";
		return false;
	}
#else
	std::vector<std::uint8_t> bgra;
	try {
		bgra.resize(static_cast<std::size_t>(width) * height * 4);
	} catch (const std::exception&) {
		std::free(const_cast<std::uint8_t*>(pixels));
		errorMessage = "out of memory";
		return false;
	}
	if (!context.Continue()) {
		std::free(const_cast<std::uint8_t*>(pixels));
		errorMessage = "JPEG decode was cancelled";
		return false;
	}
	for (int y = 0; y < height; ++y) {
		if ((y & 15) == 0 && !context.Continue()) {
			std::free(const_cast<std::uint8_t*>(pixels));
			errorMessage = "JPEG decode was cancelled";
			return false;
		}
		for (int x = 0; x < width; ++x) {
			const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
			if (cmykSource) {
				std::copy_n(const_cast<const std::uint8_t*>(pixels) + pixel * 4,
					4, bgra.data() + pixel * 4);
				ConvertCmykSampleToBgra(bgra.data() + pixel * 4, adobeInvertedCmyk);
			} else {
				bgra[pixel * 4] = pixels[pixel * 3 + 2];
				bgra[pixel * 4 + 1] = pixels[pixel * 3 + 1];
				bgra[pixel * 4 + 2] = pixels[pixel * 3];
				bgra[pixel * 4 + 3] = 255;
			}
		}
	}
	if (!context.Continue()) {
		std::free(const_cast<std::uint8_t*>(pixels));
		errorMessage = "JPEG decode was cancelled";
		return false;
	}
	try {
		DecodedFrame frame;
		frame.width = width;
		frame.height = height;
		frame.bgra = std::move(bgra);
		if (!ApplyExifOrientation(orientation, frame.width, frame.height,
			frame.bgra, errorMessage, [&context] { return context.Continue(); })) {
			std::free(const_cast<std::uint8_t*>(pixels));
			return false;
		}
		image.frames.push_back(std::move(frame));
	} catch (const std::exception&) {
		std::free(const_cast<std::uint8_t*>(pixels));
		errorMessage = "out of memory";
		return false;
	}
	std::free(const_cast<std::uint8_t*>(pixels));
	return true;
#endif
}


bool ReadJpegCropMetadataCore(const std::filesystem::path& filename,
	JpegCropMetadata& metadata, std::string& errorMessage,
	const WorkContext& context) {
	if (!context.Continue()) {
		errorMessage = "JPEG metadata read was cancelled";
		return false;
	}
	if (IsArchiveMemberLocation(filename)) {
		return WithArchiveMemberFile(filename,
			[&metadata, context](const std::filesystem::path& temporary,
				std::string& decodeError) {
				return ReadJpegCropMetadataCore(temporary, metadata,
					decodeError, context);
			}, errorMessage, nullptr, [&context] { return context.Continue(); });
	}
	FileInput input;
	if (!OpenInput(filename, input, errorMessage)) return false;
	JpegErrorManager error{};
	jpeg_decompress_struct decoder{};
	volatile bool created = false;
	decoder.err = jpeg_std_error(&error.base);
	error.base.error_exit = JpegErrorExit;
	error.base.output_message = SuppressJpegMessage;
	// libjpeg reports malformed headers by longjmp; clean up the decoder and stream on that path.
	if (setjmp(error.jump) != 0) {
		if (created) jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		errorMessage = error.message[0] == '\0' ? "invalid JPEG header" : error.message;
		return false;
	}
	jpeg_create_decompress(&decoder);
	created = true;
	jpeg_save_markers(&decoder, JPEG_APP0 + 1, 0xFFFFu);
	jpeg_stdio_src(&decoder, input.file);
	const bool headerValid = jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK;
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		errorMessage = "JPEG metadata read was cancelled";
		return false;
	}
	const bool valid = headerValid && decoder.max_h_samp_factor > 0 &&
		decoder.max_v_samp_factor > 0 && decoder.image_width > 0 &&
		decoder.image_height > 0 &&
		decoder.image_width <= static_cast<JDIMENSION>(std::numeric_limits<int>::max()) &&
		decoder.image_height <= static_cast<JDIMENSION>(std::numeric_limits<int>::max());
	if (valid) {
		metadata.storedWidth = static_cast<int>(decoder.image_width);
		metadata.storedHeight = static_cast<int>(decoder.image_height);
		metadata.mcuWidth = decoder.max_h_samp_factor * DCTSIZE;
		metadata.mcuHeight = decoder.max_v_samp_factor * DCTSIZE;
		metadata.exifOrientation = ReadJpegExifOrientation(decoder, context);
	}
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		CloseInput(input);
		metadata = {};
		errorMessage = "JPEG metadata read was cancelled";
		return false;
	}
	jpeg_destroy_decompress(&decoder);
	CloseInput(input);
	if (!valid) errorMessage = "invalid JPEG crop metadata";
	return valid;
}

} // namespace jpegview_linux::decoder_detail
