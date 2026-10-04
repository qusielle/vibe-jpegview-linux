#include "image_decoder_internal.h"

#include "perf_diagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
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
#if JPEGVIEW_HAVE_WEBP
extern "C" {
#include <webp/decode.h>
#include <webp/demux.h>
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
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/resizable_parallel_runner.h>
}
#endif
#if JPEGVIEW_HAVE_JXR
#include <JXRGlue.h>
#endif
#if JPEGVIEW_HAVE_RAW
#include <libraw/libraw.h>
#include <libraw/libraw_datastream.h>
#include <libraw/libraw_version.h>
#endif
#if JPEGVIEW_HAVE_LCMS2
#include <lcms2.h>
#endif
#if JPEGVIEW_HAVE_JXR
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace jpegview_linux::decoder_detail {

#if JPEGVIEW_HAVE_GIF
int GifLoopCount(const GifFileType* gif) {
	if (gif == nullptr) return 0;
	for (int imageIndex = 0; imageIndex < gif->ImageCount; ++imageIndex) {
		const SavedImage& saved = gif->SavedImages[imageIndex];
		bool netscapeApplication = false;
		for (int blockIndex = 0; blockIndex < saved.ExtensionBlockCount; ++blockIndex) {
			const ExtensionBlock& block = saved.ExtensionBlocks[blockIndex];
			if (block.Function == APPLICATION_EXT_FUNC_CODE && block.ByteCount >= 11 &&
				std::memcmp(block.Bytes, "NETSCAPE2.0", 11) == 0) {
				netscapeApplication = true;
				continue;
			}
			if (netscapeApplication && block.ByteCount >= 3 && block.Bytes[0] == 1) {
				return block.Bytes[1] | (static_cast<int>(block.Bytes[2]) << 8);
			}
		}
	}
	return 0;
}

struct GifInput {
	std::FILE* file = nullptr;
	CodecSourceReadTracker* reads = nullptr;
};

int ReadGif(GifFileType* gif, GifByteType* destination, int requested) {
	GifInput* input = gif == nullptr ? nullptr : static_cast<GifInput*>(gif->UserData);
	if (input == nullptr || requested <= 0) return 0;
	return static_cast<int>(input->reads->Read(input->file, destination,
		static_cast<std::size_t>(requested)));
}

bool DecodeGif(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	CodecSourceReadTracker sourceReads("giflib");
	std::unique_ptr<std::FILE, int (*)(std::FILE*)> file = sourceReads.Enabled() ?
		OpenSourceFile(filename) : std::unique_ptr<std::FILE, int (*)(std::FILE*)>(nullptr, &std::fclose);
	GifInput input{file.get(), &sourceReads};
	int gifError = 0;
	GifFileType* gif = sourceReads.Enabled() ?
		(file == nullptr ? nullptr : DGifOpen(&input, ReadGif, &gifError)) :
		DGifOpenFileName(filename.string().c_str(), &gifError);
	if (gif == nullptr) {
		if (sourceReads.Enabled() && file == nullptr) gifError = D_GIF_ERR_OPEN_FAILED;
		errorMessage = GifErrorString(gifError) == nullptr ? "cannot open GIF" : GifErrorString(gifError);
		return false;
	}
	const auto closeGif = [](GifFileType* file) {
		if (file != nullptr) (void)DGifCloseFile(file, nullptr);
	};
	std::unique_ptr<GifFileType, decltype(closeGif)> gifOwner(gif, closeGif);
	if (DGifSlurp(gif) != GIF_OK || gif->SWidth <= 0 || gif->SHeight <= 0) {
		errorMessage = GifErrorString(gif->Error) == nullptr ? "invalid GIF" : GifErrorString(gif->Error);
		return false;
	}
	if (!ValidDimensions(gif->SWidth, gif->SHeight, errorMessage)) {
		return false;
	}
	const std::size_t canvasBytes = static_cast<std::size_t>(gif->SWidth) * gif->SHeight * 4;
	std::vector<std::uint8_t> canvas(canvasBytes, 0);
	std::vector<std::uint8_t> previous;
	image.loopCount = GifLoopCount(gif);
	for (int imageIndex = 0; imageIndex < gif->ImageCount; ++imageIndex) {
		const SavedImage& saved = gif->SavedImages[imageIndex];
		GraphicsControlBlock control{};
		if (DGifSavedExtensionToGCB(gif, imageIndex, &control) != GIF_OK) {
			control.DisposalMode = DISPOSAL_UNSPECIFIED;
			control.DelayTime = 10;
			control.TransparentColor = NO_TRANSPARENT_COLOR;
		}
		if (control.DisposalMode == DISPOSE_PREVIOUS) previous = canvas;
		const ColorMapObject* colorMap = saved.ImageDesc.ColorMap != nullptr ?
			saved.ImageDesc.ColorMap : gif->SColorMap;
		if (colorMap == nullptr || colorMap->Colors == nullptr) {
			errorMessage = "GIF has no color map";
			return false;
		}
		for (int y = 0; y < saved.ImageDesc.Height; ++y) {
			for (int x = 0; x < saved.ImageDesc.Width; ++x) {
				const int targetX = saved.ImageDesc.Left + x;
				const int targetY = saved.ImageDesc.Top + y;
				if (targetX < 0 || targetY < 0 || targetX >= gif->SWidth || targetY >= gif->SHeight) continue;
				const int colorIndex = saved.RasterBits[y * saved.ImageDesc.Width + x];
				if (colorIndex == control.TransparentColor || colorIndex >= colorMap->ColorCount) continue;
				const GifColorType& color = colorMap->Colors[colorIndex];
				const std::size_t offset = (static_cast<std::size_t>(targetY) * gif->SWidth + targetX) * 4;
				canvas[offset] = color.Blue;
				canvas[offset + 1] = color.Green;
				canvas[offset + 2] = color.Red;
				canvas[offset + 3] = 255;
			}
		}
		if (!AppendBGRA(image, gif->SWidth, gif->SHeight, canvas.data(),
			std::max(100, control.DelayTime * 10), errorMessage)) {
			return false;
		}
		if (control.DisposalMode == DISPOSE_BACKGROUND) {
			for (int y = 0; y < saved.ImageDesc.Height; ++y) {
				for (int x = 0; x < saved.ImageDesc.Width; ++x) {
					const int targetX = saved.ImageDesc.Left + x;
					const int targetY = saved.ImageDesc.Top + y;
					if (targetX < 0 || targetY < 0 || targetX >= gif->SWidth || targetY >= gif->SHeight) continue;
					const std::size_t offset = (static_cast<std::size_t>(targetY) * gif->SWidth + targetX) * 4;
					std::fill_n(canvas.data() + offset, 4, 0);
				}
			}
		} else if (control.DisposalMode == DISPOSE_PREVIOUS && previous.size() == canvas.size()) {
			canvas = previous;
		}
	}
	image.animation = image.frames.size() > 1;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_WEBP
bool DecodeWebP(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> encoded;
	if (!ReadFile(filename, encoded, errorMessage)) return false;
	WebPData data{encoded.data(), encoded.size()};
	WebPAnimDecoderOptions options{};
	if (!WebPAnimDecoderOptionsInit(&options)) {
		errorMessage = "WebP animation decoder unavailable";
		return false;
	}
	std::vector<std::uint8_t> iccProfile;
	std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> demuxer(
		WebPDemux(&data), &WebPDemuxDelete);
	if (demuxer != nullptr) {
		WebPChunkIterator iterator{};
		if (WebPDemuxGetChunk(demuxer.get(), "ICCP", 1, &iterator)) {
			struct ChunkIteratorOwner {
				WebPChunkIterator* iterator;
				explicit ChunkIteratorOwner(WebPChunkIterator* value) : iterator(value) {}
				ChunkIteratorOwner(const ChunkIteratorOwner&) = delete;
				ChunkIteratorOwner& operator=(const ChunkIteratorOwner&) = delete;
				~ChunkIteratorOwner() { WebPDemuxReleaseChunkIterator(iterator); }
			} iteratorOwner{&iterator};
			iccProfile.assign(iterator.chunk.bytes, iterator.chunk.bytes + iterator.chunk.size);
		}
	}
	demuxer.reset();
	options.color_mode = MODE_BGRA;
	options.use_threads = 1;
	std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(
		WebPAnimDecoderNew(&data, &options), &WebPAnimDecoderDelete);
	if (decoder == nullptr) {
		errorMessage = "invalid WebP image";
		return false;
	}
	WebPAnimInfo info{};
	if (!WebPAnimDecoderGetInfo(decoder.get(), &info) || info.canvas_width <= 0 || info.canvas_height <= 0) {
		errorMessage = "invalid WebP animation";
		return false;
	}
	image.loopCount = info.loop_count;
	int previousTimestamp = 0;
	while (WebPAnimDecoderHasMoreFrames(decoder.get())) {
		uint8_t* pixels = nullptr;
		int timestamp = 0;
		if (!WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp) || pixels == nullptr) {
			errorMessage = "WebP frame decode failed";
			return false;
		}
		const int delay = timestamp > previousTimestamp ? timestamp - previousTimestamp : 100;
		std::vector<std::uint8_t> frame(pixels,
			pixels + static_cast<std::size_t>(info.canvas_width) * info.canvas_height * 4);
		ApplyIccProfile(frame, iccProfile);
		if (!AppendBGRA(image, info.canvas_width, info.canvas_height, std::move(frame), std::max(10, delay), errorMessage)) {
			return false;
		}
		previousTimestamp = timestamp;
	}
	image.animation = image.frames.size() > 1;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_TIFF
struct TiffInput {
	std::FILE* file = nullptr;
	CodecSourceReadTracker* reads = nullptr;
};

tmsize_t ReadTiff(thandle_t handle, void* destination, tmsize_t requested) {
	TiffInput* input = static_cast<TiffInput*>(handle);
	if (input == nullptr || requested <= 0) return 0;
	return static_cast<tmsize_t>(input->reads->Read(input->file, destination,
		static_cast<std::size_t>(requested)));
}

tmsize_t WriteTiff(thandle_t, void*, tmsize_t) {
	return 0;
}

toff_t SeekTiff(thandle_t handle, toff_t offset, int origin) {
	TiffInput* input = static_cast<TiffInput*>(handle);
	if (input == nullptr || ::fseeko(input->file, static_cast<off_t>(offset), origin) != 0) {
		return static_cast<toff_t>(-1);
	}
	const off_t position = ::ftello(input->file);
	return position < 0 ? static_cast<toff_t>(-1) : static_cast<toff_t>(position);
}

int CloseTiff(thandle_t) {
	return 0;
}

toff_t SizeTiff(thandle_t handle) {
	const TiffInput* input = static_cast<const TiffInput*>(handle);
	struct stat status{};
	return input != nullptr && ::fstat(::fileno(input->file), &status) == 0 && status.st_size >= 0 ?
		static_cast<toff_t>(status.st_size) : 0;
}

bool DecodeTiff(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	CodecSourceReadTracker sourceReads("libtiff");
	std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(nullptr, &std::fclose);
	TiffInput input;
	TIFF* tiff = nullptr;
	if (sourceReads.Enabled()) {
		file = OpenSourceFile(filename);
		if (file == nullptr) {
			errorMessage = "cannot open TIFF";
			return false;
		}
		input = {file.get(), &sourceReads};
		tiff = TIFFClientOpen(filename.string().c_str(), "r", &input,
			ReadTiff, WriteTiff, SeekTiff, CloseTiff, SizeTiff, nullptr, nullptr);
	} else {
		tiff = TIFFOpen(filename.string().c_str(), "r");
	}
	if (tiff == nullptr) {
		errorMessage = "cannot open TIFF";
		return false;
	}
	std::unique_ptr<TIFF, decltype(&TIFFClose)> tiffOwner(tiff, &TIFFClose);
	for (tdir_t directory = 0; TIFFSetDirectory(tiff, directory) != 0; ++directory) {
		uint32_t width = 0;
		uint32_t height = 0;
		if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) == 0 ||
			TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) == 0 ||
			width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
			errorMessage = "invalid TIFF dimensions";
			return false;
		}
		const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
		std::vector<uint32_t> raster;
		try {
			raster.resize(pixelCount);
		} catch (const std::exception&) {
			errorMessage = "out of memory";
			return false;
		}
		if (!TIFFReadRGBAImageOriented(tiff, width, height, raster.data(), ORIENTATION_TOPLEFT, 0)) {
			errorMessage = "TIFF frame decode failed";
			return false;
		}
		std::vector<std::uint8_t> bgra(pixelCount * 4);
		for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
			const uint32_t rgba = raster[pixel];
			bgra[pixel * 4] = TIFFGetB(rgba);
			bgra[pixel * 4 + 1] = TIFFGetG(rgba);
			bgra[pixel * 4 + 2] = TIFFGetR(rgba);
			bgra[pixel * 4 + 3] = TIFFGetA(rgba);
		}
		if (!AppendBGRA(image, static_cast<int>(width), static_cast<int>(height), std::move(bgra), 0, errorMessage)) {
			return false;
		}
	}
	// TIFF pages are navigable frames but are not timed animations.
	image.animation = false;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_HEIF
struct HeifInput {
	std::FILE* file = nullptr;
	CodecSourceReadTracker* reads = nullptr;
	std::uint64_t fileSize = 0;
};

std::int64_t PositionHeif(void* user) {
	const HeifInput* input = static_cast<const HeifInput*>(user);
	if (input == nullptr) return -1;
	const off_t position = ::ftello(input->file);
	return position < 0 ? -1 : static_cast<std::int64_t>(position);
}

int ReadHeif(void* destination, std::size_t requested, void* user) {
	HeifInput* input = static_cast<HeifInput*>(user);
	if (input == nullptr) return 1;
	return input->reads->Read(input->file, destination, requested) == requested ?
		0 : static_cast<int>(heif_error_Invalid_input);
}

int SeekHeif(std::int64_t position, void* user) {
	HeifInput* input = static_cast<HeifInput*>(user);
	if (input == nullptr || position < 0 ||
		static_cast<std::uint64_t>(position) > input->fileSize ||
		::fseeko(input->file, static_cast<off_t>(position), SEEK_SET) != 0) return 1;
	return 0;
}

enum heif_reader_grow_status WaitForHeifFileSize(std::int64_t targetSize, void* user) {
	const HeifInput* input = static_cast<const HeifInput*>(user);
	return input != nullptr && targetSize >= 0 &&
		static_cast<std::uint64_t>(targetSize) <= input->fileSize ?
		heif_reader_grow_status_size_reached : heif_reader_grow_status_size_beyond_eof;
}

bool DecodeHeif(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	CodecSourceReadTracker sourceReads("libheif");
	std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(nullptr, &std::fclose);
	HeifInput input;
	const heif_reader reader = {
		1, PositionHeif, ReadHeif, SeekHeif, WaitForHeifFileSize};
	std::unique_ptr<heif_context, decltype(&heif_context_free)> context(
		heif_context_alloc(), &heif_context_free);
	if (context == nullptr) {
		errorMessage = "HEIF decoder unavailable";
		return false;
	}
	heif_error readError{};
	if (sourceReads.Enabled()) {
		file = OpenSourceFile(filename);
		struct stat status{};
		if (file == nullptr || ::fstat(::fileno(file.get()), &status) != 0 || status.st_size < 0) {
			errorMessage = "HEIF read failed";
			return false;
		}
		input = {file.get(), &sourceReads, static_cast<std::uint64_t>(status.st_size)};
		readError = heif_context_read_from_reader(context.get(), &reader, &input, nullptr);
	} else {
		readError = heif_context_read_from_file(context.get(), filename.string().c_str(), nullptr);
	}
	if (readError.code != heif_error_Ok) {
		errorMessage = readError.message == nullptr ? "HEIF read failed" : readError.message;
		return false;
	}
	const int count = heif_context_get_number_of_top_level_images(context.get());
	if (count <= 0) {
		errorMessage = "HEIF contains no images";
		return false;
	}
	std::vector<heif_item_id> ids(static_cast<std::size_t>(count));
	const int filled = heif_context_get_list_of_top_level_image_IDs(context.get(), ids.data(), count);
	for (int index = 0; index < filled; ++index) {
		heif_image_handle* handle = nullptr;
		heif_error handleError = heif_context_get_image_handle(context.get(), ids[static_cast<std::size_t>(index)], &handle);
		if (handleError.code != heif_error_Ok || handle == nullptr) {
			errorMessage = handleError.message == nullptr ? "HEIF image handle failed" : handleError.message;
			return false;
		}
		std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handleOwner(
			handle, &heif_image_handle_release);
		std::vector<std::uint8_t> iccProfile;
		const size_t iccSize = heif_image_handle_get_raw_color_profile_size(handleOwner.get());
		if (iccSize > 0 && iccSize <= kMaxAnimationBytes) {
			iccProfile.resize(iccSize);
			const heif_error profileError = heif_image_handle_get_raw_color_profile(handleOwner.get(), iccProfile.data());
			if (profileError.code != heif_error_Ok) iccProfile.clear();
		}
		heif_image* decoded = nullptr;
		const heif_error decodeError = heif_decode_image(handleOwner.get(), &decoded, heif_colorspace_RGB,
			heif_chroma_interleaved_RGBA, nullptr);
		handleOwner.reset();
		if (decodeError.code != heif_error_Ok || decoded == nullptr) {
			errorMessage = decodeError.message == nullptr ? "HEIF frame decode failed" : decodeError.message;
			return false;
		}
		std::unique_ptr<heif_image, decltype(&heif_image_release)> decodedOwner(
			decoded, &heif_image_release);
		const int width = heif_image_get_width(decoded, heif_channel_interleaved);
		const int height = heif_image_get_height(decoded, heif_channel_interleaved);
		int stride = 0;
		const std::uint8_t* pixels = heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
		if (pixels == nullptr || stride < width * 4 || !ValidDimensions(width, height, errorMessage)) {
			errorMessage = pixels == nullptr ? "HEIF pixel plane unavailable" : errorMessage;
			return false;
		}
		std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* source = pixels + static_cast<std::size_t>(y) * stride;
			std::uint8_t* target = bgra.data() + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) {
				target[x * 4] = source[x * 4 + 2];
				target[x * 4 + 1] = source[x * 4 + 1];
				target[x * 4 + 2] = source[x * 4];
				target[x * 4 + 3] = source[x * 4 + 3];
			}
		}
		ApplyIccProfile(bgra, iccProfile);
		decodedOwner.reset();
		if (!AppendBGRA(image, width, height, std::move(bgra), 100, errorMessage)) {
			return false;
		}
	}
	image.animation = image.frames.size() > 1;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_AVIF
struct AvifInput {
	avifIO io{};
	std::FILE* file = nullptr;
	CodecSourceReadTracker* reads = nullptr;
	std::uint64_t fileSize = 0;
	std::vector<std::uint8_t> buffer;
};

avifResult ReadAvif(avifIO* io, std::uint32_t, std::uint64_t offset,
	std::size_t requested, avifROData* output) {
	if (output == nullptr) return AVIF_RESULT_INVALID_ARGUMENT;
	output->data = nullptr;
	output->size = 0;
	AvifInput* input = io == nullptr ? nullptr : static_cast<AvifInput*>(io->data);
	if (input == nullptr || offset > input->fileSize) return AVIF_RESULT_IO_ERROR;
	const std::uint64_t remaining = input->fileSize - offset;
	const std::size_t bytesRequested = static_cast<std::size_t>(std::min<std::uint64_t>(
		requested, remaining));
	if (bytesRequested == 0) return AVIF_RESULT_OK;
	if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
		::fseeko(input->file, static_cast<off_t>(offset), SEEK_SET) != 0) {
		return AVIF_RESULT_IO_ERROR;
	}
	try {
		input->buffer.resize(bytesRequested);
	} catch (const std::exception&) {
		return AVIF_RESULT_UNKNOWN_ERROR;
	}
	const std::size_t bytesRead = input->reads->Read(
		input->file, input->buffer.data(), bytesRequested);
	if (bytesRead != bytesRequested) {
		input->buffer.resize(bytesRead);
		output->data = input->buffer.data();
		output->size = bytesRead;
		return AVIF_RESULT_IO_ERROR;
	}
	output->data = input->buffer.data();
	output->size = bytesRead;
	return AVIF_RESULT_OK;
}

bool AllocateAvifRgbPixels(avifRGBImage& rgb) {
	// libavif 1.0 changed this helper from void to avifResult.
#if AVIF_VERSION >= 1000000
	return avifRGBImageAllocatePixels(&rgb) == AVIF_RESULT_OK && rgb.pixels != nullptr;
#else
	avifRGBImageAllocatePixels(&rgb);
	return rgb.pixels != nullptr;
#endif
}

struct AvifRgbPixelsOwner {
	avifRGBImage& image;
	explicit AvifRgbPixelsOwner(avifRGBImage& value) : image(value) {}
	AvifRgbPixelsOwner(const AvifRgbPixelsOwner&) = delete;
	AvifRgbPixelsOwner& operator=(const AvifRgbPixelsOwner&) = delete;
	~AvifRgbPixelsOwner() { avifRGBImageFreePixels(&image); }
};

bool DecodeAvif(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	CodecSourceReadTracker sourceReads("libavif");
	std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(nullptr, &std::fclose);
	AvifInput input{};
	if (sourceReads.Enabled()) {
		file = OpenSourceFile(filename);
		struct stat status{};
		if (file == nullptr || ::fstat(::fileno(file.get()), &status) != 0 || status.st_size < 0) {
			errorMessage = "invalid AVIF image";
			return false;
		}
		input.file = file.get();
		input.reads = &sourceReads;
		input.fileSize = static_cast<std::uint64_t>(status.st_size);
		input.io.read = ReadAvif;
		input.io.sizeHint = input.fileSize;
		input.io.persistent = AVIF_FALSE;
		input.io.data = &input;
	}
	std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder(
		avifDecoderCreate(), &avifDecoderDestroy);
	if (decoder == nullptr) {
		errorMessage = "AVIF decoder unavailable";
		return false;
	}
	const bool ioReady = sourceReads.Enabled() ?
		(avifDecoderSetIO(decoder.get(), &input.io), true) :
		avifDecoderSetIOFile(decoder.get(), filename.string().c_str()) == AVIF_RESULT_OK;
	if (!ioReady || avifDecoderParse(decoder.get()) != AVIF_RESULT_OK || decoder->imageCount == 0) {
		errorMessage = "invalid AVIF image";
		return false;
	}
	for (uint32_t index = 0; index < static_cast<uint32_t>(decoder->imageCount); ++index) {
		if (avifDecoderNthImage(decoder.get(), index) != AVIF_RESULT_OK || decoder->image == nullptr) {
			errorMessage = "AVIF frame decode failed";
			return false;
		}
		avifRGBImage rgb{};
		avifRGBImageSetDefaults(&rgb, decoder->image);
		rgb.depth = 8;
		rgb.format = AVIF_RGB_FORMAT_BGRA;
		if (!AllocateAvifRgbPixels(rgb)) {
			errorMessage = "AVIF pixel allocation failed";
			avifRGBImageFreePixels(&rgb);
			return false;
		}
		AvifRgbPixelsOwner rgbOwner{rgb};
		if (avifImageYUVToRGB(decoder->image, &rgb) != AVIF_RESULT_OK) {
			errorMessage = "AVIF color conversion failed";
			return false;
		}
		std::vector<std::uint8_t> iccProfile;
		if (decoder->image->icc.data != nullptr && decoder->image->icc.size > 0) {
			iccProfile.assign(decoder->image->icc.data,
				decoder->image->icc.data + decoder->image->icc.size);
		}
		std::vector<std::uint8_t> bgra(static_cast<std::size_t>(rgb.width) * rgb.height * 4);
		for (uint32_t y = 0; y < rgb.height; ++y) {
			std::copy_n(rgb.pixels + static_cast<std::size_t>(y) * rgb.rowBytes,
				static_cast<std::size_t>(rgb.width) * 4, bgra.data() + static_cast<std::size_t>(y) * rgb.width * 4);
		}
		ApplyIccProfile(bgra, iccProfile);
		const int delay = static_cast<int>(std::lround(decoder->imageTiming.duration * 1000.0));
		const bool appended = AppendBGRA(image, static_cast<int>(rgb.width), static_cast<int>(rgb.height),
			std::move(bgra), std::max(10, delay), errorMessage);
		if (!appended) return false;
	}
	image.animation = image.frames.size() > 1;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_JXL
JxlDecoderStatus GetJxlOriginalIccProfileSize(const JxlDecoder* decoder, size_t* profileSize) {
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
	return JxlDecoderGetICCProfileSize(decoder, nullptr,
		JXL_COLOR_PROFILE_TARGET_ORIGINAL, profileSize);
#else
	return JxlDecoderGetICCProfileSize(decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
		profileSize);
#endif
}

JxlDecoderStatus GetJxlOriginalIccProfile(const JxlDecoder* decoder,
	uint8_t* profile, size_t profileSize) {
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
	return JxlDecoderGetColorAsICCProfile(decoder, nullptr,
		JXL_COLOR_PROFILE_TARGET_ORIGINAL, profile, profileSize);
#else
	return JxlDecoderGetColorAsICCProfile(decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
		profile, profileSize);
#endif
}

struct JxlDecoderResources {
	JxlDecoder* decoder = nullptr;
	void* runner = nullptr;
	JxlDecoderResources(JxlDecoder* decoderValue, void* runnerValue)
		: decoder(decoderValue), runner(runnerValue) {}
	JxlDecoderResources(const JxlDecoderResources&) = delete;
	JxlDecoderResources& operator=(const JxlDecoderResources&) = delete;

	void Reset() noexcept {
		if (decoder != nullptr) JxlDecoderDestroy(decoder);
		if (runner != nullptr) JxlResizableParallelRunnerDestroy(runner);
		decoder = nullptr;
		runner = nullptr;
	}
	~JxlDecoderResources() { Reset(); }
};

bool DecodeJxl(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> encoded;
	if (!ReadFile(filename, encoded, errorMessage)) return false;
	JxlDecoder* decoder = JxlDecoderCreate(nullptr);
	void* runner = JxlResizableParallelRunnerCreate(nullptr);
	JxlDecoderResources resources{decoder, runner};
	if (decoder == nullptr || runner == nullptr) {
		errorMessage = "JPEG XL decoder unavailable";
		return false;
	}
	const auto cleanup = [&]() { resources.Reset(); };
	if (JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
		JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
		JxlDecoderSetParallelRunner(decoder, JxlResizableParallelRunner, runner) != JXL_DEC_SUCCESS ||
		JxlDecoderSetCoalescing(decoder, JXL_TRUE) != JXL_DEC_SUCCESS) {
		cleanup();
		errorMessage = "JPEG XL decoder initialization failed";
		return false;
	}
	JxlDecoderSetInput(decoder, encoded.data(), encoded.size());
	JxlDecoderCloseInput(decoder);
	JxlBasicInfo basicInfo{};
	std::vector<std::uint8_t> pixels;
	std::vector<std::uint8_t> iccProfile;
	int frameDelay = 100;
	for (;;) {
		const JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
		if (status == JXL_DEC_BASIC_INFO) {
			if (JxlDecoderGetBasicInfo(decoder, &basicInfo) != JXL_DEC_SUCCESS ||
				basicInfo.xsize > std::numeric_limits<int>::max() || basicInfo.ysize > std::numeric_limits<int>::max() ||
				!ValidDimensions(static_cast<int>(basicInfo.xsize), static_cast<int>(basicInfo.ysize), errorMessage)) {
				cleanup();
				return false;
			}
			JxlResizableParallelRunnerSetThreads(runner,
				JxlResizableParallelRunnerSuggestThreads(basicInfo.xsize, basicInfo.ysize));
		} else if (status == JXL_DEC_COLOR_ENCODING) {
			size_t profileSize = 0;
			if (GetJxlOriginalIccProfileSize(decoder, &profileSize) == JXL_DEC_SUCCESS &&
				profileSize > 0 && profileSize <= kMaxAnimationBytes) {
				iccProfile.resize(profileSize);
				if (GetJxlOriginalIccProfile(decoder, iccProfile.data(), iccProfile.size()) !=
					JXL_DEC_SUCCESS) iccProfile.clear();
			}
			if (iccProfile.empty()) {
				JxlColorEncoding preferredProfile{};
				JxlColorEncodingSetToSRGB(&preferredProfile, JXL_FALSE);
				JxlDecoderSetPreferredColorProfile(decoder, &preferredProfile);
			}
		} else if (status == JXL_DEC_FRAME) {
			JxlFrameHeader header{};
			if (JxlDecoderGetFrameHeader(decoder, &header) != JXL_DEC_SUCCESS) {
				cleanup();
				errorMessage = "JPEG XL frame header failed";
				return false;
			}
			if (basicInfo.animation.tps_numerator != 0) {
				frameDelay = std::max(10, static_cast<int>(std::lround(
					1000.0 * header.duration * basicInfo.animation.tps_denominator /
					basicInfo.animation.tps_numerator)));
			} else {
				frameDelay = 100;
			}
		} else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
			const JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
			size_t bufferSize = 0;
			if (JxlDecoderImageOutBufferSize(decoder, &format, &bufferSize) != JXL_DEC_SUCCESS) {
				cleanup();
				errorMessage = "JPEG XL output buffer query failed";
				return false;
			}
			try {
				pixels.resize(bufferSize);
			} catch (const std::exception&) {
				cleanup();
				errorMessage = "out of memory";
				return false;
			}
			if (JxlDecoderSetImageOutBuffer(decoder, &format, pixels.data(), pixels.size()) != JXL_DEC_SUCCESS) {
				cleanup();
				errorMessage = "JPEG XL output buffer setup failed";
				return false;
			}
		} else if (status == JXL_DEC_FULL_IMAGE) {
			std::vector<std::uint8_t> bgra(pixels.size());
			for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
				bgra[pixel * 4] = pixels[pixel * 4 + 2];
				bgra[pixel * 4 + 1] = pixels[pixel * 4 + 1];
				bgra[pixel * 4 + 2] = pixels[pixel * 4];
				bgra[pixel * 4 + 3] = pixels[pixel * 4 + 3];
			}
			ApplyIccProfile(bgra, iccProfile);
			if (!AppendBGRA(image, static_cast<int>(basicInfo.xsize), static_cast<int>(basicInfo.ysize),
				std::move(bgra), frameDelay, errorMessage)) {
				cleanup();
				return false;
			}
		} else if (status == JXL_DEC_SUCCESS) {
			break;
		} else {
			cleanup();
			errorMessage = status == JXL_DEC_NEED_MORE_INPUT ? "truncated JPEG XL image" :
				"JPEG XL frame decode failed";
			return false;
		}
	}
	cleanup();
	image.animation = basicInfo.have_animation && image.frames.size() > 1;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_JXR
struct JxrCodecResources {
	PKImageDecode* decoder = nullptr;
	PKFormatConverter* converter = nullptr;
	JxrCodecResources() = default;
	JxrCodecResources(const JxrCodecResources&) = delete;
	JxrCodecResources& operator=(const JxrCodecResources&) = delete;

	void Release() noexcept {
		if (converter != nullptr) PKFormatConverter_Release(&converter);
		if (decoder != nullptr) PKImageDecode_Release(&decoder);
	}
	~JxrCodecResources() { Release(); }
};

bool DecodeJxr(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	CodecSourceReadTracker sourceReads("jxr");
	JxrCodecResources resources;
	if (PKCodecFactory_CreateDecoderFromFile(filename.string().c_str(), &resources.decoder) != WMP_errSuccess ||
		resources.decoder == nullptr) {
		errorMessage = "JPEG XR decoder unavailable or invalid image";
		return false;
	}
	if (sourceReads.Enabled()) {
		PerfDiagnostics::Instance().RecordText(PerfMetric::SourceRead,
			0, 0, 0, 0, 0, 0, 0, "jxr_unmeasured_read");
	}
	I32 width = 0;
	I32 height = 0;
	const bool initialized = PKImageDecode_GetSize(resources.decoder, &width, &height) == WMP_errSuccess &&
		ValidDimensions(width, height, errorMessage) &&
		PKCodecFactory_CreateFormatConverter(&resources.converter) == WMP_errSuccess &&
		resources.converter != nullptr &&
		PKFormatConverter_Initialize(resources.converter, resources.decoder, nullptr,
			GUID_PKPixelFormat24bppBGR) == WMP_errSuccess;
	if (!initialized) {
		if (errorMessage.empty()) errorMessage = "JPEG XR decoder initialization failed";
		return false;
	}
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
	const PKRect rect{0, 0, width, height};
	const bool copied = resources.converter->Convert(resources.converter, &rect,
		pixels.data(), static_cast<U32>(width * 3)) == WMP_errSuccess;
	resources.Release();
	if (!copied) {
		errorMessage = "JPEG XR frame decode failed";
		return false;
	}
	std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
	for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel) {
		bgra[pixel * 4] = pixels[pixel * 3];
		bgra[pixel * 4 + 1] = pixels[pixel * 3 + 1];
		bgra[pixel * 4 + 2] = pixels[pixel * 3 + 2];
		bgra[pixel * 4 + 3] = 255;
	}
	return AppendBGRA(image, width, height, std::move(bgra), 0, errorMessage, false);
}
#endif

#if JPEGVIEW_HAVE_RAW
class TrackedRawFileDatastream : public LibRaw_bigfile_datastream {
public:
	TrackedRawFileDatastream(const char* filename, CodecSourceReadTracker& reads)
		: LibRaw_bigfile_datastream(filename), reads_(reads) {}

	int read(void* destination, size_t size, size_t count) override {
		#if LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 20, 0)
		if (substream != nullptr) return LibRaw_bigfile_datastream::read(destination, size, count);
		#endif
		const INT64 before = LibRaw_bigfile_datastream::tell();
		const CodecSourceReadTracker::TimePoint start = reads_.BeginRead();
		const int result = LibRaw_bigfile_datastream::read(destination, size, count);
		const CodecSourceReadTracker::TimePoint finish = CodecSourceReadTracker::Clock::now();
		const INT64 after = LibRaw_bigfile_datastream::tell();
		reads_.CompleteRead(start, before >= 0 && after > before ?
			static_cast<std::uint64_t>(after - before) : 0, finish);
		return result;
	}

	int get_char() override {
		#if LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 20, 0)
		if (substream != nullptr) return LibRaw_bigfile_datastream::get_char();
		#endif
		const INT64 before = LibRaw_bigfile_datastream::tell();
		const CodecSourceReadTracker::TimePoint start = reads_.BeginRead();
		const int result = LibRaw_bigfile_datastream::get_char();
		const CodecSourceReadTracker::TimePoint finish = CodecSourceReadTracker::Clock::now();
		const INT64 after = LibRaw_bigfile_datastream::tell();
		reads_.CompleteRead(start, before >= 0 && after > before ?
			static_cast<std::uint64_t>(after - before) : 0, finish);
		return result;
	}

	char* gets(char* destination, int capacity) override {
		#if LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 20, 0)
		if (substream != nullptr) return LibRaw_bigfile_datastream::gets(destination, capacity);
		#endif
		const INT64 before = LibRaw_bigfile_datastream::tell();
		const CodecSourceReadTracker::TimePoint start = reads_.BeginRead();
		char* result = LibRaw_bigfile_datastream::gets(destination, capacity);
		const CodecSourceReadTracker::TimePoint finish = CodecSourceReadTracker::Clock::now();
		const INT64 after = LibRaw_bigfile_datastream::tell();
		reads_.CompleteRead(start, before >= 0 && after > before ?
			static_cast<std::uint64_t>(after - before) : 0, finish);
		return result;
	}

	int scanf_one(const char* format, void* value) override {
		#if LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 20, 0)
		if (substream != nullptr) return LibRaw_bigfile_datastream::scanf_one(format, value);
		#endif
		const INT64 before = LibRaw_bigfile_datastream::tell();
		const CodecSourceReadTracker::TimePoint start = reads_.BeginRead();
		const int result = LibRaw_bigfile_datastream::scanf_one(format, value);
		const CodecSourceReadTracker::TimePoint finish = CodecSourceReadTracker::Clock::now();
		const INT64 after = LibRaw_bigfile_datastream::tell();
		reads_.CompleteRead(start, before >= 0 && after > before ?
			static_cast<std::uint64_t>(after - before) : 0, finish);
		return result;
	}

	int jpeg_src(void* jpegdata) override {
		const int result = LibRaw_bigfile_datastream::jpeg_src(jpegdata);
#if LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 20, 0)
		if (result == 0 && reads_.Enabled()) {
			PerfDiagnostics::Instance().RecordText(PerfMetric::SourceRead,
				0, 0, 0, 0, 0, 0, 0, "libraw_jpeg_handoff_unmeasured");
		}
#endif
		return result;
	}

#if LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 21, 0) || defined(LIBRAW_OLD_VIDEO_SUPPORT)
	void* make_jas_stream() override {
		void* stream = LibRaw_bigfile_datastream::make_jas_stream();
		if (stream != nullptr && reads_.Enabled()) {
			PerfDiagnostics::Instance().RecordText(PerfMetric::SourceRead,
				0, 0, 0, 0, 0, 0, 0, "libraw_jasper_handoff_unmeasured");
		}
		return stream;
	}
#endif

private:
	CodecSourceReadTracker& reads_;
};

bool DecodeRaw(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	CodecSourceReadTracker sourceReads("libraw");
	// LibRaw uses this caller-owned stream during decoding; declare it before raw
	// so LibRaw is destroyed first and the stream stays alive through cleanup.
	std::unique_ptr<TrackedRawFileDatastream> input;
	LibRaw raw;
	const int openResult = sourceReads.Enabled() ?
		([&]() {
			input = std::make_unique<TrackedRawFileDatastream>(filename.string().c_str(), sourceReads);
			return raw.open_datastream(input.get());
		})() : raw.open_file(filename.string().c_str());
	if (openResult != LIBRAW_SUCCESS) {
		errorMessage = "unsupported or invalid RAW image";
		return false;
	}
	raw.imgdata.params.output_bps = 8;
	int width = 0;
	int height = 0;
	int colors = 0;
	int bits = 0;
	raw.get_mem_image_format(&width, &height, &colors, &bits);
	if (!ValidDimensions(width, height, errorMessage) || colors < 1 || colors > 4) return false;
	if (raw.unpack() != LIBRAW_SUCCESS || raw.dcraw_process() != LIBRAW_SUCCESS) {
		errorMessage = "RAW development failed";
		return false;
	}
	const int stride = width * colors;
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
	if (raw.copy_mem_image(pixels.data(), stride, 1) != LIBRAW_SUCCESS) {
		errorMessage = "RAW pixel extraction failed";
		return false;
	}
	std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::uint8_t* source = pixels.data() + static_cast<std::size_t>(y) * stride + x * colors;
			std::uint8_t* target = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4;
			if (colors == 1) target[0] = target[1] = target[2] = source[0];
			else {
				target[0] = source[0];
				target[1] = source[1];
				target[2] = source[2];
			}
			target[3] = colors == 4 ? source[3] : 255;
		}
	}
	return AppendBGRA(image, width, height, std::move(bgra), 0, errorMessage, colors == 4);
}
#endif

} // namespace jpegview_linux::decoder_detail
