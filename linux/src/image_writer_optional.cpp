#include "image_writer_internal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

#if JPEGVIEW_HAVE_GIF
extern "C" {
#include <gif_lib.h>
}
#endif
#if JPEGVIEW_HAVE_TIFF
extern "C" {
#include <tiffio.h>
}
#endif
#if JPEGVIEW_HAVE_HEIF
extern "C" {
#include <libheif/heif.h>
}
#endif
#if JPEGVIEW_HAVE_AVIF
extern "C" {
#include <avif/avif.h>
}
#endif
#if JPEGVIEW_HAVE_JXL
extern "C" {
#include <jxl/encode.h>
#include <jxl/resizable_parallel_runner.h>
}
#endif

namespace jpegview_linux::detail {

struct DynamicLibraryCloser {
	void operator()(void* library) const noexcept {
		if (library != nullptr) (void)dlclose(library);
	}
};

bool WriteWebP(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, const ImageWriteOptions& options, std::string& errorMessage) {
	using EncodeBGRA = std::size_t (*)(const std::uint8_t*, int, int, int, float, unsigned char**);
	using EncodeLosslessBGRA = std::size_t (*)(const std::uint8_t*, int, int, int, unsigned char**);
	using FreeMemory = void (*)(void*);
	const char* libraryNames[] = {"libwebp.so.7", "libwebp.so.6", "libwebp.so"};
	void* library = nullptr;
	for (const char* libraryName : libraryNames) {
		library = dlopen(libraryName, RTLD_NOW | RTLD_LOCAL);
		if (library != nullptr) break;
	}
	if (library == nullptr) {
		errorMessage = "WebP encoder library not available";
		return false;
	}
	std::unique_ptr<void, DynamicLibraryCloser> libraryOwner(library);
	auto encodeBGRA = reinterpret_cast<EncodeBGRA>(dlsym(libraryOwner.get(), "WebPEncodeBGRA"));
	auto encodeLosslessBGRA = reinterpret_cast<EncodeLosslessBGRA>(dlsym(libraryOwner.get(), "WebPEncodeLosslessBGRA"));
	auto freeMemory = reinterpret_cast<FreeMemory>(dlsym(libraryOwner.get(), "WebPFree"));
	if (freeMemory == nullptr || (options.webpLossless ? encodeLosslessBGRA == nullptr : encodeBGRA == nullptr)) {
		errorMessage = "incompatible WebP encoder library";
		return false;
	}
	std::size_t outputSize = 0;
	unsigned char* encoded = nullptr;
	outputSize = options.webpLossless ?
		encodeLosslessBGRA(bgra, width, height, width * 4, &encoded) :
		encodeBGRA(bgra, width, height, width * 4, static_cast<float>(std::clamp(options.webpQuality, 0, 100)), &encoded);
	std::unique_ptr<unsigned char, FreeMemory> encodedOwner(encoded, freeMemory);
	if (encoded == nullptr || outputSize == 0) {
		errorMessage = "WebP encoder failed";
		return false;
	}
	std::ofstream output(filename, std::ios::binary);
	if (output) output.write(reinterpret_cast<const char*>(encoded), static_cast<std::streamsize>(outputSize));
	const bool success = static_cast<bool>(output);
	if (!success) errorMessage = "error writing output file";
	return success;
}
#if JPEGVIEW_HAVE_GIF
bool WriteGif(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	std::vector<GifByteType> red(static_cast<std::size_t>(width) * height);
	std::vector<GifByteType> green(red.size());
	std::vector<GifByteType> blue(red.size());
	std::vector<GifByteType> indexed(red.size());
	for (std::size_t pixelIndex = 0; pixelIndex < red.size(); ++pixelIndex) {
		const std::uint8_t* source = bgra + pixelIndex * 4;
		const unsigned int alpha = source[3];
		// GIF has one transparent index and no per-pixel alpha. Flatten partial
		// alpha against white, matching the visible result of the viewer.
		red[pixelIndex] = static_cast<GifByteType>((source[2] * alpha + 255u * (255u - alpha)) / 255u);
		green[pixelIndex] = static_cast<GifByteType>((source[1] * alpha + 255u * (255u - alpha)) / 255u);
		blue[pixelIndex] = static_cast<GifByteType>((source[0] * alpha + 255u * (255u - alpha)) / 255u);
	}
	int colorMapSize = 256;
	std::array<GifColorType, 256> colorMap{};
	if (GifQuantizeBuffer(static_cast<unsigned int>(width), static_cast<unsigned int>(height),
		&colorMapSize, red.data(), green.data(), blue.data(), indexed.data(), colorMap.data()) != GIF_OK) {
		errorMessage = "GIF color quantization failed";
		return false;
	}
	ColorMapObject* map = GifMakeMapObject(256, colorMap.data());
	if (map == nullptr) {
		errorMessage = "GIF color map allocation failed";
		return false;
	}
	int gifError = 0;
	GifFileType* gif = EGifOpenFileName(filename.string().c_str(), false, &gifError);
	if (gif == nullptr) {
		GifFreeMapObject(map);
		errorMessage = GifErrorString(gifError) == nullptr ? "cannot open GIF output" : GifErrorString(gifError);
		return false;
	}
	bool success = EGifPutScreenDesc(gif, width, height, 8, 0, map) == GIF_OK &&
		EGifPutImageDesc(gif, 0, 0, width, height, false, nullptr) == GIF_OK;
	for (int y = 0; success && y < height; ++y) {
		success = EGifPutLine(gif, indexed.data() + static_cast<std::size_t>(y) * width, width) == GIF_OK;
	}
	const int closeResult = EGifCloseFile(gif, &gifError);
	GifFreeMapObject(map);
	if (!success || closeResult != GIF_OK) {
		errorMessage = GifErrorString(gifError) == nullptr ? "GIF encoder failed" : GifErrorString(gifError);
		return false;
	}
	return true;
}
#endif

#if JPEGVIEW_HAVE_TIFF
bool WriteTiff(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	std::unique_ptr<TIFF, decltype(&TIFFClose)> tiffOwner(
		TIFFOpen(filename.string().c_str(), "w"), &TIFFClose);
	TIFF* tiff = tiffOwner.get();
	if (tiff == nullptr) {
		errorMessage = "cannot open TIFF output";
		return false;
	}
	const std::uint16_t extraSamples = EXTRASAMPLE_UNASSALPHA;
	TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(width));
	TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(height));
	TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 4);
	TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
	TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
	TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
	TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
	TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, &extraSamples);
	std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4);
	bool success = true;
	for (int y = 0; success && y < height; ++y) {
		const std::uint8_t* source = bgra + static_cast<std::size_t>(y) * width * 4;
		for (int x = 0; x < width; ++x) {
			row[x * 4] = source[x * 4 + 2];
			row[x * 4 + 1] = source[x * 4 + 1];
			row[x * 4 + 2] = source[x * 4];
			row[x * 4 + 3] = source[x * 4 + 3];
		}
		success = TIFFWriteScanline(tiff, row.data(), static_cast<uint32_t>(y), 0) >= 0;
	}
	if (!success) errorMessage = "TIFF encoder failed";
	return success;
}
#endif

#if JPEGVIEW_HAVE_HEIF
bool WriteHeif(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, int quality, bool avif, std::string& errorMessage) {
	#if defined(LIBHEIF_HAVE_VERSION) && LIBHEIF_HAVE_VERSION(1, 8, 0)
	const heif_compression_format format = avif ? heif_compression_AV1 : heif_compression_HEVC;
	#else
	if (avif) {
		errorMessage = "AVIF encoder is not available in this libheif version";
		return false;
	}
	const heif_compression_format format = heif_compression_HEVC;
	#endif
	if (!heif_have_encoder_for_format(format)) {
		errorMessage = avif ? "AVIF encoder is not available" : "HEIF encoder is not available";
		return false;
	}
	std::unique_ptr<heif_context, decltype(&heif_context_free)> contextOwner(
		heif_context_alloc(), &heif_context_free);
	heif_context* context = contextOwner.get();
	if (context == nullptr) {
		errorMessage = "HEIF encoder unavailable";
		return false;
	}
	std::unique_ptr<heif_encoder, decltype(&heif_encoder_release)> encoderOwner(
		nullptr, &heif_encoder_release);
	std::unique_ptr<heif_image, decltype(&heif_image_release)> imageOwner(
		nullptr, &heif_image_release);
	std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handleOwner(
		nullptr, &heif_image_handle_release);
	heif_image* image = nullptr;
	const auto fail = [&](const heif_error& error, const char* fallback) {
		errorMessage = error.message == nullptr || error.message[0] == '\0' ? fallback : error.message;
	};
	heif_encoder* encoder = nullptr;
	heif_error error = heif_context_get_encoder_for_format(context, format, &encoder);
	encoderOwner.reset(encoder);
	if (error.code == heif_error_Ok) error = heif_encoder_set_lossy_quality(encoderOwner.get(), std::clamp(quality, 0, 100));
	if (error.code == heif_error_Ok) {
		error = heif_image_create(width, height, heif_colorspace_RGB,
			heif_chroma_interleaved_RGBA, &image);
		imageOwner.reset(image);
	}
	if (error.code == heif_error_Ok) error = heif_image_add_plane(imageOwner.get(), heif_channel_interleaved, width, height, 8);
	if (error.code == heif_error_Ok) {
		int stride = 0;
		std::uint8_t* target = heif_image_get_plane(imageOwner.get(), heif_channel_interleaved, &stride);
		if (target == nullptr || stride < width * 4) {
			error.code = heif_error_Encoding_error;
			error.message = "HEIF pixel plane unavailable";
		} else {
			for (int y = 0; y < height; ++y) {
				const std::uint8_t* source = bgra + static_cast<std::size_t>(y) * width * 4;
				std::uint8_t* row = target + static_cast<std::size_t>(y) * stride;
				for (int x = 0; x < width; ++x) {
					row[x * 4] = source[x * 4 + 2];
					row[x * 4 + 1] = source[x * 4 + 1];
					row[x * 4 + 2] = source[x * 4];
					row[x * 4 + 3] = source[x * 4 + 3];
				}
			}
		}
	}
	if (error.code == heif_error_Ok) {
		heif_image_handle* handle = nullptr;
		error = heif_context_encode_image(context, imageOwner.get(), encoderOwner.get(), nullptr, &handle);
		handleOwner.reset(handle);
	}
	if (error.code == heif_error_Ok) error = heif_context_write_to_file(context, filename.string().c_str());
	if (error.code != heif_error_Ok) fail(error, avif ? "AVIF encoder failed" : "HEIF encoder failed");
	return error.code == heif_error_Ok;
}
#endif

#if JPEGVIEW_HAVE_AVIF
bool WriteAvif(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, int quality, std::string& errorMessage) {
	std::unique_ptr<avifImage, decltype(&avifImageDestroy)> image(
		avifImageCreate(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
			8, AVIF_PIXEL_FORMAT_YUV444), &avifImageDestroy);
	std::unique_ptr<avifEncoder, decltype(&avifEncoderDestroy)> encoder(
		avifEncoderCreate(), &avifEncoderDestroy);
	if (image == nullptr || encoder == nullptr) {
		errorMessage = "AVIF encoder unavailable";
		return false;
	}
	avifRGBImage rgb{};
	avifRGBImageSetDefaults(&rgb, image.get());
	rgb.format = AVIF_RGB_FORMAT_BGRA;
	rgb.pixels = const_cast<std::uint8_t*>(bgra);
	rgb.rowBytes = static_cast<uint32_t>(width) * 4;
	avifResult result = avifImageRGBToYUV(image.get(), &rgb);
	if (result == AVIF_RESULT_OK) {
		const int clampedQuality = std::clamp(quality, 0, 100);
		#if AVIF_VERSION >= 1000000
		encoder->quality = clampedQuality;
		encoder->qualityAlpha = clampedQuality;
		#else
		const int quantizer = (AVIF_QUANTIZER_WORST_QUALITY * (100 - clampedQuality) + 50) / 100;
		encoder->minQuantizer = quantizer;
		encoder->maxQuantizer = quantizer;
		encoder->minQuantizerAlpha = quantizer;
		encoder->maxQuantizerAlpha = quantizer;
		#endif
	}
	avifRWData output = AVIF_DATA_EMPTY;
	struct AvifDataOwner {
		avifRWData& data;
		~AvifDataOwner() { avifRWDataFree(&data); }
	} outputOwner{output};
	if (result == AVIF_RESULT_OK) result = avifEncoderWrite(encoder.get(), image.get(), &output);
	if (result == AVIF_RESULT_OK) {
		std::ofstream file(filename, std::ios::binary);
		if (file) file.write(reinterpret_cast<const char*>(output.data),
			static_cast<std::streamsize>(output.size));
		if (!file) result = AVIF_RESULT_IO_ERROR;
	}
	if (result != AVIF_RESULT_OK) {
		errorMessage = "AVIF encoder failed";
		return false;
	}
	return true;
}
#endif

#if JPEGVIEW_HAVE_JXL
struct JxlEncoderResources {
	JxlEncoder* encoder = nullptr;
	void* runner = nullptr;
	JxlEncoderResources(JxlEncoder* encoderValue, void* runnerValue)
		: encoder(encoderValue), runner(runnerValue) {}
	JxlEncoderResources(const JxlEncoderResources&) = delete;
	JxlEncoderResources& operator=(const JxlEncoderResources&) = delete;

	void Reset() noexcept {
		if (encoder != nullptr) JxlEncoderDestroy(encoder);
		if (runner != nullptr) JxlResizableParallelRunnerDestroy(runner);
		encoder = nullptr;
		runner = nullptr;
	}
	~JxlEncoderResources() { Reset(); }
};

bool WriteJxl(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, int quality, std::string& errorMessage) {
	JxlEncoder* encoder = JxlEncoderCreate(nullptr);
	void* runner = JxlResizableParallelRunnerCreate(nullptr);
	JxlEncoderResources resources{encoder, runner};
	if (encoder == nullptr || runner == nullptr) {
		errorMessage = "JPEG XL encoder unavailable";
		return false;
	}
	const auto cleanup = [&]() { resources.Reset(); };
	if (JxlEncoderSetParallelRunner(encoder, JxlResizableParallelRunner, runner) != JXL_ENC_SUCCESS) {
		cleanup();
		errorMessage = "JPEG XL encoder initialization failed";
		return false;
	}
	JxlResizableParallelRunnerSetThreads(runner,
		JxlResizableParallelRunnerSuggestThreads(static_cast<uint64_t>(width), static_cast<uint64_t>(height)));
	JxlBasicInfo basicInfo{};
	JxlEncoderInitBasicInfo(&basicInfo);
	basicInfo.xsize = static_cast<uint32_t>(width);
	basicInfo.ysize = static_cast<uint32_t>(height);
	basicInfo.bits_per_sample = 8;
	basicInfo.num_color_channels = 3;
	basicInfo.num_extra_channels = 1;
	basicInfo.alpha_bits = 8;
	JxlColorEncoding colorEncoding{};
	JxlColorEncodingSetToSRGB(&colorEncoding, JXL_FALSE);
	JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
	std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
	for (std::size_t pixel = 0; pixel < rgba.size() / 4; ++pixel) {
		rgba[pixel * 4] = bgra[pixel * 4 + 2];
		rgba[pixel * 4 + 1] = bgra[pixel * 4 + 1];
		rgba[pixel * 4 + 2] = bgra[pixel * 4];
		rgba[pixel * 4 + 3] = bgra[pixel * 4 + 3];
	}
	JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(encoder, nullptr);
	JxlEncoderStatus status = settings == nullptr ? JXL_ENC_ERROR : JxlEncoderSetBasicInfo(encoder, &basicInfo);
	if (status == JXL_ENC_SUCCESS) status = JxlEncoderSetColorEncoding(encoder, &colorEncoding);
	if (status == JXL_ENC_SUCCESS) {
		// A lower distance is higher quality; map the existing 0..100 quality
		// control onto a useful visually-lossy range.
		status = JxlEncoderSetFrameDistance(settings, static_cast<float>((100 - std::clamp(quality, 0, 100)) / 12.0));
	}
	if (status == JXL_ENC_SUCCESS) {
		status = JxlEncoderAddImageFrame(settings, &format, rgba.data(), rgba.size());
	}
	if (status == JXL_ENC_SUCCESS) JxlEncoderCloseInput(encoder);
	std::vector<std::uint8_t> encoded;
	if (status == JXL_ENC_SUCCESS) {
		for (;;) {
			std::array<std::uint8_t, 65536> buffer{};
			std::uint8_t* next = buffer.data();
			size_t available = buffer.size();
			status = JxlEncoderProcessOutput(encoder, &next, &available);
			encoded.insert(encoded.end(), buffer.data(), next);
			if (status == JXL_ENC_SUCCESS) break;
			if (status != JXL_ENC_NEED_MORE_OUTPUT) break;
		}
	}
	if (status == JXL_ENC_SUCCESS) {
		std::ofstream output(filename, std::ios::binary);
		if (output) output.write(reinterpret_cast<const char*>(encoded.data()),
			static_cast<std::streamsize>(encoded.size()));
		if (!output) status = JXL_ENC_ERROR;
	}
	cleanup();
	if (status != JXL_ENC_SUCCESS) {
		errorMessage = "JPEG XL encoder failed";
		return false;
	}
	return true;
}
#endif

} // namespace jpegview_linux::detail
