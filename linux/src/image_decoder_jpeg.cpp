#include "image_decoder_internal.h"

#include "archive_source.h"
#include "perf_diagnostics.h"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <jpeglib.h>
}

namespace jpegview_linux::decoder_detail {

struct MappedInput {
	void* data = MAP_FAILED;
	std::size_t size = 0;

	MappedInput() = default;
	MappedInput(const MappedInput&) = delete;
	MappedInput& operator=(const MappedInput&) = delete;
	~MappedInput() { Reset(); }

	void Reset() noexcept {
		if (data != MAP_FAILED) (void)::munmap(data, size);
		data = MAP_FAILED;
		size = 0;
	}
};

bool MapInput(const std::filesystem::path& filename, MappedInput& input,
	bool sequentialRead, std::string& errorMessage) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::SourceMap);
	PerfDiagnostics::Instance().Record(PerfMetric::SourceRead);
	const int descriptor = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		errorMessage = "cannot open file";
		return false;
	}
	struct stat status{};
	if (::fstat(descriptor, &status) != 0 || status.st_size <= 0 ||
		static_cast<std::uintmax_t>(status.st_size) >
			static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
		::close(descriptor);
		errorMessage = status.st_size == 0 ? "empty file" : "cannot read file";
		return false;
	}
	input.size = static_cast<std::size_t>(status.st_size);
	input.data = ::mmap(nullptr, input.size, PROT_READ, MAP_PRIVATE, descriptor, 0);
	::close(descriptor);
	if (input.data == MAP_FAILED) {
		input.size = 0;
		errorMessage = "cannot map file";
		return false;
	}
	// A full entropy decode consumes the compressed stream from start to end, while
	// a dimension probe stops near the header. Avoid pulling the bodies of many
	// large neighboring files into the page cache during prefetch planning.
	(void)::madvise(input.data, input.size, sequentialRead ? MADV_SEQUENTIAL : MADV_RANDOM);
	return true;
}

void UnmapInput(MappedInput& input) {
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

bool ReadJpegSize(const std::filesystem::path& filename, int& width, int& height,
	std::string& errorMessage, const WorkContext& context) {
	if (!context.Continue()) {
		errorMessage = "JPEG metadata read was cancelled";
		return false;
	}
	MappedInput input;
	if (!MapInput(filename, input, false, errorMessage)) return false;
	JpegErrorManager error{};
	jpeg_decompress_struct decoder{};
	volatile bool created = false;
	decoder.err = jpeg_std_error(&error.base);
	error.base.error_exit = JpegErrorExit;
	error.base.output_message = SuppressJpegMessage;
	// libjpeg reports failures through longjmp, so this branch must explicitly release its C resources.
	if (setjmp(error.jump) != 0) {
		if (created) jpeg_destroy_decompress(&decoder);
		UnmapInput(input);
		errorMessage = error.message[0] == '\0' ? "invalid JPEG header" : error.message;
		return false;
	}
	jpeg_create_decompress(&decoder);
	created = true;
	jpeg_mem_src(&decoder, static_cast<const unsigned char*>(input.data), input.size);
	const bool headerValid = jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK;
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		UnmapInput(input);
		errorMessage = "JPEG metadata read was cancelled";
		return false;
	}
	const bool valid = headerValid &&
		ValidDimensions(static_cast<int>(decoder.image_width),
			static_cast<int>(decoder.image_height), errorMessage);
	if (valid) {
		width = static_cast<int>(decoder.image_width);
		height = static_cast<int>(decoder.image_height);
	}
	jpeg_destroy_decompress(&decoder);
	UnmapInput(input);
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
	MappedInput input;
	if (!MapInput(filename, input, true, errorMessage)) return false;
	JpegErrorManager error{};
	jpeg_decompress_struct decoder{};
	volatile bool created = false;
#ifdef JCS_EXT_BGRA
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
#ifdef JCS_EXT_BGRA
		delete const_cast<DecodedFrame*>(directFrame);
#else
		std::free(const_cast<std::uint8_t*>(pixels));
#endif
		UnmapInput(input);
		errorMessage = error.message[0] == '\0' ? "JPEG decoder failed" : error.message;
		return false;
	}

	jpeg_create_decompress(&decoder);
	created = true;
	jpeg_mem_src(&decoder, static_cast<const unsigned char*>(input.data), input.size);
	const bool headerValid = jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK;
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		UnmapInput(input);
		errorMessage = "JPEG decode was cancelled";
		return false;
	}
	if (!headerValid ||
		!ValidDimensions(static_cast<int>(decoder.image_width),
			static_cast<int>(decoder.image_height), errorMessage)) {
		jpeg_destroy_decompress(&decoder);
		UnmapInput(input);
		if (errorMessage.empty()) errorMessage = "invalid JPEG header";
		return false;
	}
	if (sourceWidth != nullptr) *sourceWidth = static_cast<int>(decoder.image_width);
	if (sourceHeight != nullptr) *sourceHeight = static_cast<int>(decoder.image_height);
	if (minimumWidth > 0 && minimumHeight > 0) {
		for (const unsigned int denominator : {8u, 4u, 2u}) {
			const unsigned int scaledWidth =
				(decoder.image_width + denominator - 1) / denominator;
			const unsigned int scaledHeight =
				(decoder.image_height + denominator - 1) / denominator;
			if (scaledWidth >= static_cast<unsigned int>(minimumWidth) &&
				scaledHeight >= static_cast<unsigned int>(minimumHeight)) {
				decoder.scale_num = 1;
				decoder.scale_denom = denominator;
				break;
			}
		}
	}

#ifdef JCS_EXT_BGRA
	decoder.out_color_space = JCS_EXT_BGRA;
	constexpr int outputComponents = 4;
#else
	decoder.out_color_space = JCS_RGB;
	constexpr int outputComponents = 3;
#endif
	jpeg_start_decompress(&decoder);
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		UnmapInput(input);
		errorMessage = "JPEG decode was cancelled";
		return false;
	}
	const int width = static_cast<int>(decoder.output_width);
	const int height = static_cast<int>(decoder.output_height);
	const std::size_t rowBytes = static_cast<std::size_t>(width) * outputComponents;
	const std::size_t byteCount = rowBytes * static_cast<std::size_t>(height);
#ifdef JCS_EXT_BGRA
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
		UnmapInput(input);
		errorMessage = "out of memory";
		return false;
	}
#else
	pixels = static_cast<std::uint8_t*>(std::malloc(byteCount));
	if (pixels == nullptr) {
		jpeg_destroy_decompress(&decoder);
		UnmapInput(input);
		errorMessage = "out of memory";
		return false;
	}
#endif
	while (decoder.output_scanline < decoder.output_height) {
		if ((decoder.output_scanline & 15u) == 0 && !context.Continue()) {
			jpeg_destroy_decompress(&decoder);
			created = false;
#ifdef JCS_EXT_BGRA
			delete const_cast<DecodedFrame*>(directFrame);
#else
			std::free(const_cast<std::uint8_t*>(pixels));
#endif
			UnmapInput(input);
			errorMessage = "JPEG decode was cancelled";
			return false;
		}
#ifdef JCS_EXT_BGRA
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
#ifdef JCS_EXT_BGRA
		delete const_cast<DecodedFrame*>(directFrame);
#else
		std::free(const_cast<std::uint8_t*>(pixels));
#endif
		UnmapInput(input);
		errorMessage = "JPEG decode was cancelled";
		return false;
	}
	jpeg_destroy_decompress(&decoder);
	created = false;
	UnmapInput(input);

#ifdef JCS_EXT_BGRA
	try {
		DecodedFrame* frame = const_cast<DecodedFrame*>(directFrame);
		if (!context.Continue()) {
			delete frame;
			directFrame = nullptr;
			errorMessage = "JPEG decode was cancelled";
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
			bgra[pixel * 4] = pixels[pixel * 3 + 2];
			bgra[pixel * 4 + 1] = pixels[pixel * 3 + 1];
			bgra[pixel * 4 + 2] = pixels[pixel * 3];
			bgra[pixel * 4 + 3] = 255;
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


bool ReadJpegMcuSizeCore(const std::filesystem::path& filename,
	int& width, int& height, std::string& errorMessage,
	const WorkContext& context) {
	if (!context.Continue()) {
		errorMessage = "JPEG metadata read was cancelled";
		return false;
	}
	if (IsArchiveMemberLocation(filename)) {
		return WithArchiveMemberFile(filename,
			[&width, &height, context](const std::filesystem::path& temporary,
				std::string& decodeError) {
				return ReadJpegMcuSizeCore(temporary, width, height,
					decodeError, context);
			}, errorMessage, nullptr, [&context] { return context.Continue(); });
	}
	MappedInput input;
	if (!MapInput(filename, input, false, errorMessage)) return false;
	JpegErrorManager error{};
	jpeg_decompress_struct decoder{};
	volatile bool created = false;
	decoder.err = jpeg_std_error(&error.base);
	error.base.error_exit = JpegErrorExit;
	error.base.output_message = SuppressJpegMessage;
	// libjpeg reports malformed headers by longjmp; clean up the decoder and mapping on that path.
	if (setjmp(error.jump) != 0) {
		if (created) jpeg_destroy_decompress(&decoder);
		UnmapInput(input);
		errorMessage = error.message[0] == '\0' ? "invalid JPEG header" : error.message;
		return false;
	}
	jpeg_create_decompress(&decoder);
	created = true;
	jpeg_mem_src(&decoder, static_cast<const unsigned char*>(input.data), input.size);
	const bool headerValid = jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK;
	if (!context.Continue()) {
		jpeg_destroy_decompress(&decoder);
		UnmapInput(input);
		errorMessage = "JPEG metadata read was cancelled";
		return false;
	}
	const bool valid = headerValid && decoder.max_h_samp_factor > 0 &&
		decoder.max_v_samp_factor > 0;
	if (valid) {
		width = decoder.max_h_samp_factor * DCTSIZE;
		height = decoder.max_v_samp_factor * DCTSIZE;
	}
	jpeg_destroy_decompress(&decoder);
	UnmapInput(input);
	if (!valid) errorMessage = "invalid JPEG sampling factors";
	return valid;
}

} // namespace jpegview_linux::decoder_detail
