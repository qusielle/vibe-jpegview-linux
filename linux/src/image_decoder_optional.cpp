#include "image_decoder_internal.h"

#include "exif_orientation.h"
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
struct GifInput {
	std::FILE* file = nullptr;
	CodecSourceReadTracker* reads = nullptr;
	const WorkContext* context = nullptr;
};

int ReadGif(GifFileType* gif, GifByteType* destination, int requested) {
	GifInput* input = gif == nullptr ? nullptr : static_cast<GifInput*>(gif->UserData);
	if (input == nullptr || requested <= 0 || !input->context->Continue()) return 0;
	return static_cast<int>(input->reads->Read(input->file, destination,
		static_cast<std::size_t>(requested)));
}

bool DecodeGif(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage, const WorkContext& supplied) {
	const WorkContext context = ResolveWorkContext(filename,
		SourceWorkPriority::Foreground, supplied);
	const auto continueWork = [&] {
		if (context.Continue()) return true;
		errorMessage = "source work was cancelled";
		return false;
	};
	if (!continueWork()) return false;
	CodecSourceReadTracker sourceReads("giflib");
	std::unique_ptr<std::FILE, int (*)(std::FILE*)> file = OpenSourceFile(filename);
	if (file == nullptr) {
		errorMessage = "cannot open GIF";
		return false;
	}
	GifInput input{file.get(), &sourceReads, &context};
	int gifError = 0;
	GifFileType* gif = DGifOpen(&input, ReadGif, &gifError);
	if (gif == nullptr) {
		if (!continueWork()) return false;
		errorMessage = GifErrorString(gifError) == nullptr ? "cannot open GIF" : GifErrorString(gifError);
		return false;
	}
	const auto closeGif = [](GifFileType* file) {
		if (file != nullptr) (void)DGifCloseFile(file, nullptr);
	};
	std::unique_ptr<GifFileType, decltype(closeGif)> gifOwner(gif, closeGif);
	// Reject the screen before decoding any frame; image rasters are then consumed
	// one line at a time rather than retained as a full raster for every frame.
	if (!ValidDimensions(gif->SWidth, gif->SHeight, errorMessage)) return false;
	const std::size_t canvasBytes = static_cast<std::size_t>(gif->SWidth) * gif->SHeight * 4;
	if (canvasBytes > kMaxAnimationBytes) {
		errorMessage = "animation is too large";
		return false;
	}
	std::vector<std::uint8_t> canvas;
	std::vector<GifPixelType> scanline;
	std::vector<std::uint8_t> previous;
	DecodedImage decoded;
	int loopCount = 1;
	std::uint64_t decodedFrameBytes = 0;
	GraphicsControlBlock nextControl{};
	nextControl.DisposalMode = DISPOSAL_UNSPECIFIED;
	nextControl.DelayTime = 0;
	nextControl.TransparentColor = NO_TRANSPARENT_COLOR;
	try {
		canvas.assign(canvasBytes, 0);
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	const auto gifFailure = [&] {
		if (!continueWork()) return false;
		const char* message = GifErrorString(gif->Error);
		errorMessage = message == nullptr ? "invalid GIF" : message;
		return false;
	};
	for (;;) {
		if (!continueWork()) return false;
		GifRecordType recordType = UNDEFINED_RECORD_TYPE;
		if (DGifGetRecordType(gif, &recordType) != GIF_OK) return gifFailure();
		if (recordType == TERMINATE_RECORD_TYPE) break;
		if (recordType == EXTENSION_RECORD_TYPE) {
			int extensionCode = 0;
			GifByteType* extension = nullptr;
			if (DGifGetExtension(gif, &extensionCode, &extension) != GIF_OK) return gifFailure();
			bool firstBlock = true;
			bool netscapeApplication = false;
			while (extension != nullptr) {
				const std::size_t blockBytes = extension[0];
				const GifByteType* payload = extension + 1;
				if (extensionCode == GRAPHICS_EXT_FUNC_CODE && blockBytes >= 4) {
					GraphicsControlBlock control{};
					if (DGifExtensionToGCB(blockBytes, payload, &control) == GIF_OK) {
						nextControl = control;
					}
				} else if (extensionCode == APPLICATION_EXT_FUNC_CODE) {
					if (firstBlock) {
						netscapeApplication = blockBytes >= 11 &&
							std::memcmp(payload, "NETSCAPE2.0", 11) == 0;
					} else if (netscapeApplication && blockBytes >= 3 && payload[0] == 1) {
						const int repeats = payload[1] |
							(static_cast<int>(payload[2]) << 8);
						// The extension counts repeats after the initial pass.
						loopCount = repeats == 0 ? 0 : repeats + 1;
					}
				}
				if (DGifGetExtensionNext(gif, &extension) != GIF_OK) return gifFailure();
				firstBlock = false;
			}
			continue;
		}
		if (recordType != IMAGE_DESC_RECORD_TYPE) {
			errorMessage = "invalid GIF record";
			return false;
		}
		if (decoded.frames.size() >= kMaxAnimationFrames) {
			errorMessage = "GIF has too many frames";
			return false;
		}
		if (DGifGetImageDesc(gif) != GIF_OK) return gifFailure();
		const GifImageDesc& descriptor = gif->Image;
		if (!ValidDimensions(descriptor.Width, descriptor.Height, errorMessage)) return false;
		const ColorMapObject* colorMap = descriptor.ColorMap != nullptr ?
			descriptor.ColorMap : gif->SColorMap;
		if (colorMap == nullptr || colorMap->Colors == nullptr) {
			errorMessage = "GIF has no color map";
			return false;
		}
		if (decodedFrameBytes > kMaxAnimationBytes - canvasBytes) {
			errorMessage = "animation is too large";
			return false;
		}
		try {
			scanline.resize(static_cast<std::size_t>(descriptor.Width));
			if (nextControl.DisposalMode == DISPOSE_PREVIOUS) previous = canvas;
		} catch (const std::exception&) {
			errorMessage = "out of memory";
			return false;
		}
		int frameDelayMs = nextControl.DelayTime * 10;
		if (frameDelayMs == 0) frameDelayMs = 100;
		else if (frameDelayMs < 20) frameDelayMs = 20;
		const std::array<int, 4> interlaceOffsets = {0, 4, 2, 1};
		const std::array<int, 4> interlaceSteps = {8, 8, 4, 2};
		for (int pass = 0; pass < (descriptor.Interlace ? 4 : 1); ++pass) {
			const int startRow = descriptor.Interlace ? interlaceOffsets[pass] : 0;
			const int stepRow = descriptor.Interlace ? interlaceSteps[pass] : 1;
			for (int y = startRow; y < descriptor.Height; y += stepRow) {
				if (!continueWork()) return false;
				if (DGifGetLine(gif, scanline.data(), descriptor.Width) != GIF_OK) {
					return gifFailure();
				}
				const int targetY = descriptor.Top + y;
				if (targetY < 0 || targetY >= gif->SHeight) continue;
				const int firstX = std::max(0, -descriptor.Left);
				const int lastX = std::min(descriptor.Width, gif->SWidth - descriptor.Left);
				for (int x = firstX; x < lastX; ++x) {
					const int colorIndex = scanline[static_cast<std::size_t>(x)];
					if (colorIndex == nextControl.TransparentColor ||
						colorIndex >= colorMap->ColorCount) continue;
					const GifColorType& color = colorMap->Colors[colorIndex];
					const std::size_t offset =
						(static_cast<std::size_t>(targetY) * gif->SWidth +
						 descriptor.Left + x) * 4;
					canvas[offset] = color.Blue;
					canvas[offset + 1] = color.Green;
					canvas[offset + 2] = color.Red;
					canvas[offset + 3] = 255;
				}
			}
		}
		try {
			DecodedFrame frame;
			frame.width = gif->SWidth;
			frame.height = gif->SHeight;
			frame.delayMs = frameDelayMs;
			frame.bgra = canvas;
			for (std::size_t alpha = 3; alpha < frame.bgra.size(); alpha += 4) {
				if (frame.bgra[alpha] != 255) {
					frame.hasTransparency = true;
					break;
				}
			}
			decoded.frames.push_back(std::move(frame));
		} catch (const std::exception&) {
			errorMessage = "out of memory";
			return false;
		}
		decodedFrameBytes += canvasBytes;
		if (nextControl.DisposalMode == DISPOSE_BACKGROUND) {
			const int firstX = std::max(0, -descriptor.Left);
			const int lastX = std::min(descriptor.Width, gif->SWidth - descriptor.Left);
			for (int y = 0; y < descriptor.Height; ++y) {
				if (!continueWork()) return false;
				const int targetY = descriptor.Top + y;
				if (targetY < 0 || targetY >= gif->SHeight) continue;
				for (int x = firstX; x < lastX; ++x) {
					const std::size_t offset =
						(static_cast<std::size_t>(targetY) * gif->SWidth +
						 descriptor.Left + x) * 4;
					std::fill_n(canvas.data() + offset, 4, 0);
				}
			}
		} else if (nextControl.DisposalMode == DISPOSE_PREVIOUS && previous.size() == canvas.size()) {
			canvas.swap(previous);
			previous.clear();
		}
		nextControl = {};
		nextControl.DisposalMode = DISPOSAL_UNSPECIFIED;
		nextControl.DelayTime = 0;
		nextControl.TransparentColor = NO_TRANSPARENT_COLOR;
	}
	if (decoded.frames.empty()) {
		errorMessage = "GIF has no image frames";
		return false;
	}
	decoded.animation = decoded.frames.size() > 1;
	decoded.isGif = true;
	decoded.loopCount = loopCount;
	image = std::move(decoded);
	return true;
}
#endif

#if JPEGVIEW_HAVE_WEBP
bool DecodeWebP(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage, const WorkContext& supplied) {
	const WorkContext context = ResolveWorkContext(filename,
		SourceWorkPriority::Foreground, supplied);
	const auto cancelled = [&] {
		if (CheckDecoderCancellation(context, errorMessage)) return false;
		image = {};
		return true;
	};
	std::vector<std::uint8_t> encoded;
	if (!ReadFile(filename, encoded, errorMessage, context)) return false;
	if (cancelled()) return false;
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
	if (!WebPAnimDecoderGetInfo(decoder.get(), &info) || info.canvas_width == 0 ||
		info.canvas_height == 0 ||
		info.canvas_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
		info.canvas_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
		!ValidDimensions(static_cast<int>(info.canvas_width),
			static_cast<int>(info.canvas_height), errorMessage)) {
		errorMessage = "invalid WebP animation";
		return false;
	}
	image.loopCount = info.loop_count;
	int previousTimestamp = 0;
	AnimationFrameBudget frameBudget(image);
	while (WebPAnimDecoderHasMoreFrames(decoder.get())) {
		if (cancelled()) return false;
		uint8_t* pixels = nullptr;
		int timestamp = 0;
		if (!WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp) || pixels == nullptr) {
			errorMessage = "WebP frame decode failed";
			return false;
		}
		if (cancelled()) return false;
		const int delay = timestamp > previousTimestamp ? timestamp - previousTimestamp : 100;
		std::vector<std::uint8_t> frame(static_cast<std::size_t>(info.canvas_width) *
			info.canvas_height * 4);
		for (uint32_t y = 0; y < info.canvas_height; ++y) {
			if (cancelled()) return false;
			std::copy_n(pixels + static_cast<std::size_t>(y) * info.canvas_width * 4,
				static_cast<std::size_t>(info.canvas_width) * 4,
				frame.data() + static_cast<std::size_t>(y) * info.canvas_width * 4);
		}
		if (cancelled()) return false;
		ApplyIccProfile(frame, iccProfile);
		if (cancelled()) return false;
		if (!AppendBGRA(image, info.canvas_width, info.canvas_height,
			std::move(frame), std::max(10, delay), errorMessage, true, context, frameBudget)) {
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
	std::string& errorMessage, const WorkContext& context) {
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
	AnimationFrameBudget frameBudget(image);
	for (tdir_t directory = 0; TIFFSetDirectory(tiff, directory) != 0; ++directory) {
		if (!context.Continue()) {
			errorMessage = "TIFF decode was cancelled";
			return false;
		}
		uint32_t width = 0;
		uint32_t height = 0;
		if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) == 0 ||
			TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) == 0 ||
			width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() ||
			!ValidDimensions(static_cast<int>(width), static_cast<int>(height), errorMessage)) {
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
		struct RgbaImageOwner {
			TIFFRGBAImage image{};
			bool active = false;
			~RgbaImageOwner() {
				if (active) TIFFRGBAImageEnd(&image);
			}
		} rgbaOwner;
		char rgbaError[1024]{};
		if (!TIFFRGBAImageBegin(&rgbaOwner.image, tiff, 1, rgbaError)) {
			errorMessage = "TIFF frame decode failed";
			return false;
		}
		rgbaOwner.active = true;
		int orientation = rgbaOwner.image.orientation;
		if (!IsValidExifOrientation(orientation)) orientation = ORIENTATION_TOPLEFT;
		// Ask libtiff for the stored top-left raster; the shared EXIF transform below
		// handles all eight orientations, including the axis-swapping cases it cannot rotate.
		rgbaOwner.image.orientation = ORIENTATION_TOPLEFT;
		rgbaOwner.image.req_orientation = ORIENTATION_TOPLEFT;
		if (!TIFFRGBAImageGet(&rgbaOwner.image, raster.data(), width, height)) {
			errorMessage = "TIFF frame decode failed";
			return false;
		}
		TIFFRGBAImageEnd(&rgbaOwner.image);
		rgbaOwner.active = false;
		std::vector<std::uint8_t> bgra(pixelCount * 4);
		const auto unpremultiply = [](std::uint8_t channel, std::uint8_t alpha) {
			if (alpha == 0) return static_cast<std::uint8_t>(0);
			const unsigned value = (static_cast<unsigned>(channel) * 255u + alpha / 2u) / alpha;
			return static_cast<std::uint8_t>(std::min(value, 255u));
		};
		for (uint32_t y = 0; y < height; ++y) {
			if ((y & 15u) == 0 && !context.Continue()) {
				errorMessage = "TIFF decode was cancelled";
				return false;
			}
			for (uint32_t x = 0; x < width; ++x) {
				const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
				const uint32_t rgba = raster[pixel];
				const std::uint8_t alpha = TIFFGetA(rgba);
				// TIFFRGBAImageGet emits associated-alpha RGB for both TIFF alpha forms;
				// the viewer stores straight-alpha BGRA for its processing and blending paths.
				bgra[pixel * 4] = unpremultiply(TIFFGetB(rgba), alpha);
				bgra[pixel * 4 + 1] = unpremultiply(TIFFGetG(rgba), alpha);
				bgra[pixel * 4 + 2] = unpremultiply(TIFFGetR(rgba), alpha);
				bgra[pixel * 4 + 3] = alpha;
			}
		}
		int orientedWidth = static_cast<int>(width);
		int orientedHeight = static_cast<int>(height);
		if (!ApplyExifOrientation(orientation, orientedWidth, orientedHeight,
			bgra, errorMessage, [&context] { return context.Continue(); }) ||
			!AppendBGRA(image, orientedWidth, orientedHeight, std::move(bgra),
				0, errorMessage, true, context, frameBudget)) {
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
	const WorkContext* context = nullptr;
	bool* cancelled = nullptr;
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
	if (input->context != nullptr && !input->context->Continue()) {
		if (input->cancelled != nullptr) *input->cancelled = true;
		return 1;
	}
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
	std::string& errorMessage, const WorkContext& supplied) {
	const WorkContext work = ResolveWorkContext(filename,
		SourceWorkPriority::Foreground, supplied);
	const auto cancelled = [&] {
		if (CheckDecoderCancellation(work, errorMessage)) return false;
		image = {};
		return true;
	};
	CodecSourceReadTracker sourceReads("libheif");
	std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(nullptr, &std::fclose);
	bool inputCancelled = false;
	HeifInput input{};
	const heif_reader reader = {
		1, PositionHeif, ReadHeif, SeekHeif, WaitForHeifFileSize};
	std::unique_ptr<heif_context, decltype(&heif_context_free)> context(
		heif_context_alloc(), &heif_context_free);
	if (context == nullptr) {
		errorMessage = "HEIF decoder unavailable";
		return false;
	}
	heif_error readError{};
	if (cancelled()) return false;
	file = OpenSourceFile(filename);
	struct stat status{};
	if (file == nullptr || ::fstat(::fileno(file.get()), &status) != 0 || status.st_size < 0) {
		errorMessage = "HEIF read failed";
		return false;
	}
	input.file = file.get();
	input.reads = &sourceReads;
	input.context = &work;
	input.cancelled = &inputCancelled;
	input.fileSize = static_cast<std::uint64_t>(status.st_size);
	readError = heif_context_read_from_reader(context.get(), &reader, &input, nullptr);
	if (inputCancelled) {
		errorMessage = "source work was cancelled";
		image = {};
		return false;
	}
	if (cancelled()) return false;
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
	AnimationFrameBudget frameBudget(image);
	for (int index = 0; index < filled; ++index) {
		if (cancelled()) return false;
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
		if (inputCancelled || cancelled()) return false;
		const int handleWidth = heif_image_handle_get_width(handleOwner.get());
		const int handleHeight = heif_image_handle_get_height(handleOwner.get());
		if (!ValidDimensions(handleWidth, handleHeight, errorMessage)) return false;
		heif_image* decoded = nullptr;
		const heif_error decodeError = heif_decode_image(handleOwner.get(), &decoded, heif_colorspace_RGB,
			heif_chroma_interleaved_RGBA, nullptr);
		handleOwner.reset();
		if (inputCancelled) {
			if (decoded != nullptr) heif_image_release(decoded);
			errorMessage = "source work was cancelled";
			image = {};
			return false;
		}
		if (cancelled()) {
			if (decoded != nullptr) heif_image_release(decoded);
			return false;
		}
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
		if (!ValidDimensions(width, height, errorMessage)) return false;
		if (pixels == nullptr || static_cast<std::int64_t>(stride) <
			static_cast<std::int64_t>(width) * 4) {
			errorMessage = "HEIF pixel plane unavailable";
			return false;
		}
		std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
		for (int y = 0; y < height; ++y) {
			if (cancelled()) return false;
			const std::uint8_t* source = pixels + static_cast<std::size_t>(y) * stride;
			std::uint8_t* target = bgra.data() + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) {
				target[x * 4] = source[x * 4 + 2];
				target[x * 4 + 1] = source[x * 4 + 1];
				target[x * 4 + 2] = source[x * 4];
				target[x * 4 + 3] = source[x * 4 + 3];
			}
		}
		if (cancelled()) return false;
		ApplyIccProfile(bgra, iccProfile);
		if (cancelled()) return false;
		decodedOwner.reset();
		if (!AppendBGRA(image, width, height, std::move(bgra),
            100, errorMessage, true, work, frameBudget)) {
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
	const WorkContext* context = nullptr;
	bool* cancelled = nullptr;
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
	if (input->context != nullptr && !input->context->Continue()) {
		if (input->cancelled != nullptr) *input->cancelled = true;
		return AVIF_RESULT_IO_ERROR;
	}
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
	std::string& errorMessage, const WorkContext& supplied) {
	const WorkContext context = ResolveWorkContext(filename,
		SourceWorkPriority::Foreground, supplied);
	const auto cancelled = [&] {
		if (CheckDecoderCancellation(context, errorMessage)) return false;
		image = {};
		return true;
	};
	CodecSourceReadTracker sourceReads("libavif");
	std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(nullptr, &std::fclose);
	AvifInput input{};
	bool inputCancelled = false;
	if (cancelled()) return false;
	file = OpenSourceFile(filename);
	struct stat status{};
	if (file == nullptr || ::fstat(::fileno(file.get()), &status) != 0 || status.st_size < 0) {
		errorMessage = "invalid AVIF image";
		return false;
	}
	input.file = file.get();
	input.reads = &sourceReads;
	input.context = &context;
	input.cancelled = &inputCancelled;
	input.fileSize = static_cast<std::uint64_t>(status.st_size);
	input.io.read = ReadAvif;
	input.io.sizeHint = input.fileSize;
	input.io.persistent = AVIF_FALSE;
	input.io.data = &input;
	std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder(
		avifDecoderCreate(), &avifDecoderDestroy);
	if (decoder == nullptr) {
		errorMessage = "AVIF decoder unavailable";
		return false;
	}
	// Keep libavif and its AV1 backend inside the same bounds as ValidDimensions.
	decoder->imageSizeLimit = static_cast<std::uint32_t>(kMaxImagePixels);
#if AVIF_VERSION >= 1000000
	decoder->imageDimensionLimit = kMaxImageDimension;
#endif
	avifDecoderSetIO(decoder.get(), &input.io);
	if (avifDecoderParse(decoder.get()) != AVIF_RESULT_OK || decoder->imageCount == 0) {
		if (inputCancelled) {
			errorMessage = "source work was cancelled";
			image = {};
			return false;
		}
		if (cancelled()) return false;
		errorMessage = "invalid AVIF image";
		return false;
	}
	if (decoder->image == nullptr ||
		decoder->image->width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
		decoder->image->height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
		!ValidDimensions(static_cast<int>(decoder->image->width),
			static_cast<int>(decoder->image->height), errorMessage)) return false;
	AnimationFrameBudget frameBudget(image);
	for (uint32_t index = 0; index < static_cast<uint32_t>(decoder->imageCount); ++index) {
		if (cancelled()) return false;
		if (avifDecoderNthImage(decoder.get(), index) != AVIF_RESULT_OK || decoder->image == nullptr) {
			if (inputCancelled) {
				errorMessage = "source work was cancelled";
				image = {};
				return false;
			}
			if (cancelled()) return false;
			errorMessage = "AVIF frame decode failed";
			return false;
		}
		if (cancelled()) return false;
		avifRGBImage rgb{};
		if (decoder->image->width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
			decoder->image->height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
			!ValidDimensions(static_cast<int>(decoder->image->width),
				static_cast<int>(decoder->image->height), errorMessage)) return false;
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
		if (cancelled()) return false;
		std::vector<std::uint8_t> iccProfile;
		if (decoder->image->icc.data != nullptr && decoder->image->icc.size > 0) {
			iccProfile.assign(decoder->image->icc.data,
				decoder->image->icc.data + decoder->image->icc.size);
		}
		std::vector<std::uint8_t> bgra(static_cast<std::size_t>(rgb.width) * rgb.height * 4);
		for (uint32_t y = 0; y < rgb.height; ++y) {
			if (cancelled()) return false;
			std::copy_n(rgb.pixels + static_cast<std::size_t>(y) * rgb.rowBytes,
				static_cast<std::size_t>(rgb.width) * 4, bgra.data() + static_cast<std::size_t>(y) * rgb.width * 4);
		}
		if (cancelled()) return false;
		ApplyIccProfile(bgra, iccProfile);
		if (cancelled()) return false;
		const int delay = static_cast<int>(std::lround(decoder->imageTiming.duration * 1000.0));
		const bool appended = AppendBGRA(image, static_cast<int>(rgb.width), static_cast<int>(rgb.height),
			std::move(bgra), std::max(10, delay), errorMessage, true, context, frameBudget);
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
	std::string& errorMessage, const WorkContext& supplied) {
	const WorkContext context = ResolveWorkContext(filename,
		SourceWorkPriority::Foreground, supplied);
	std::vector<std::uint8_t> encoded;
	if (!ReadFile(filename, encoded, errorMessage, context)) return false;
	JxlDecoder* decoder = JxlDecoderCreate(nullptr);
	void* runner = JxlResizableParallelRunnerCreate(nullptr);
	JxlDecoderResources resources{decoder, runner};
	if (decoder == nullptr || runner == nullptr) {
		errorMessage = "JPEG XL decoder unavailable";
		return false;
	}
	// This decode holds one shared CPU permit; leave the runner in its default
	// caller-thread mode instead of starting a codec-private worker pool.
	if (JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
		JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
		JxlDecoderSetParallelRunner(decoder, JxlResizableParallelRunner, runner) != JXL_DEC_SUCCESS ||
		JxlDecoderSetCoalescing(decoder, JXL_TRUE) != JXL_DEC_SUCCESS) {
		errorMessage = "JPEG XL decoder initialization failed";
		return false;
	}
	JxlDecoderSetInput(decoder, encoded.data(), encoded.size());
	JxlDecoderCloseInput(decoder);
	JxlBasicInfo basicInfo{};
	std::vector<std::uint8_t> pixels;
	std::vector<std::uint8_t> iccProfile;
	int frameDelay = 100;
	AnimationFrameBudget frameBudget(image);
	for (;;) {
		if (!CheckDecoderCancellation(context, errorMessage)) {
			image = {};
			return false;
		}
		const JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
		if (!CheckDecoderCancellation(context, errorMessage)) {
			image = {};
			return false;
		}
		if (status == JXL_DEC_BASIC_INFO) {
			if (JxlDecoderGetBasicInfo(decoder, &basicInfo) != JXL_DEC_SUCCESS ||
				basicInfo.xsize > std::numeric_limits<int>::max() || basicInfo.ysize > std::numeric_limits<int>::max() ||
				!ValidDimensions(static_cast<int>(basicInfo.xsize), static_cast<int>(basicInfo.ysize), errorMessage)) {
				return false;
			}
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
				errorMessage = "JPEG XL output buffer query failed";
				return false;
			}
			try {
				pixels.resize(bufferSize);
			} catch (const std::exception&) {
				errorMessage = "out of memory";
				return false;
			}
			if (JxlDecoderSetImageOutBuffer(decoder, &format, pixels.data(), pixels.size()) != JXL_DEC_SUCCESS) {
				errorMessage = "JPEG XL output buffer setup failed";
				return false;
			}
		} else if (status == JXL_DEC_FULL_IMAGE) {
			std::vector<std::uint8_t> bgra(pixels.size());
			for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
				if ((pixel & 0x3fffu) == 0 &&
					!CheckDecoderCancellation(context, errorMessage)) {
					image = {};
					return false;
				}
				bgra[pixel * 4] = pixels[pixel * 4 + 2];
				bgra[pixel * 4 + 1] = pixels[pixel * 4 + 1];
				bgra[pixel * 4 + 2] = pixels[pixel * 4];
				bgra[pixel * 4 + 3] = pixels[pixel * 4 + 3];
			}
			if (!CheckDecoderCancellation(context, errorMessage)) {
				image = {};
				return false;
			}
			ApplyIccProfile(bgra, iccProfile);
			if (!CheckDecoderCancellation(context, errorMessage)) {
				image = {};
				return false;
			}
			if (!AppendBGRA(image, static_cast<int>(basicInfo.xsize), static_cast<int>(basicInfo.ysize),
				std::move(bgra), frameDelay, errorMessage, true, context, frameBudget)) {
				return false;
			}
		} else if (status == JXL_DEC_SUCCESS) {
			break;
		} else {
			errorMessage = status == JXL_DEC_NEED_MORE_INPUT ? "truncated JPEG XL image" :
				"JPEG XL frame decode failed";
			return false;
		}
	}
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
	std::string& errorMessage, const WorkContext& supplied) {
	const WorkContext context = ResolveWorkContext(filename,
		SourceWorkPriority::Foreground, supplied);
	if (!CheckDecoderCancellation(context, errorMessage)) return false;
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
	if (!CheckDecoderCancellation(context, errorMessage)) return false;
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
	const U32 stride = static_cast<U32>(width * 3);
	// The Ubuntu jxrlib decoder rejects non-origin rectangles unless built in
	// REENTRANT_MODE. Decode its full raster in one call, then check cancellation
	// during the bounded BGRA conversion below.
	const PKRect frame{0, 0, width, height};
	if (resources.converter->Copy(resources.converter, &frame,
		pixels.data(), stride) != WMP_errSuccess) {
		errorMessage = "JPEG XR frame decode failed";
		return false;
	}
	if (!CheckDecoderCancellation(context, errorMessage)) return false;
	resources.Release();
	std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
	for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel) {
		if ((pixel & 0x3fffu) == 0 && !context.Continue()) {
			errorMessage = "source work was cancelled";
			return false;
		}
		bgra[pixel * 4] = pixels[pixel * 3];
		bgra[pixel * 4 + 1] = pixels[pixel * 3 + 1];
		bgra[pixel * 4 + 2] = pixels[pixel * 3 + 2];
		bgra[pixel * 4 + 3] = 255;
	}
	AnimationFrameBudget frameBudget(image);
	return AppendBGRA(image, width, height, std::move(bgra), 0,
		errorMessage, false, context, frameBudget);
}
#endif

#if JPEGVIEW_HAVE_RAW
struct RawDecodeCancellation {
	const WorkContext* context = nullptr;
};

int ContinueRawDecode(void* data, enum LibRaw_progress, int, int) {
	const RawDecodeCancellation* cancellation =
		static_cast<const RawDecodeCancellation*>(data);
	return cancellation == nullptr || cancellation->context == nullptr ||
		!cancellation->context->Continue() ? 1 : 0;
}

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
	std::string& errorMessage, const WorkContext& supplied) {
	const WorkContext context = ResolveWorkContext(filename,
		SourceWorkPriority::Foreground, supplied);
	if (!CheckDecoderCancellation(context, errorMessage)) return false;
	CodecSourceReadTracker sourceReads("libraw");
	// LibRaw uses this caller-owned stream during decoding; declare it before raw
	// so LibRaw is destroyed first and the stream stays alive through cleanup.
	std::unique_ptr<TrackedRawFileDatastream> input;
	LibRaw raw;
	RawDecodeCancellation cancellation{&context};
	raw.set_progress_handler(&ContinueRawDecode, &cancellation);
	const int openResult = sourceReads.Enabled() ?
		([&]() {
			input = std::make_unique<TrackedRawFileDatastream>(filename.string().c_str(), sourceReads);
			return raw.open_datastream(input.get());
		})() : raw.open_file(filename.string().c_str());
	if (openResult != LIBRAW_SUCCESS) {
		if (!context.Continue() || openResult == LIBRAW_CANCELLED_BY_CALLBACK) {
			errorMessage = "source work was cancelled";
			return false;
		}
		errorMessage = "unsupported or invalid RAW image";
		return false;
	}
	if (!CheckDecoderCancellation(context, errorMessage)) return false;
	raw.imgdata.params.output_bps = 8;
	int width = 0;
	int height = 0;
	int colors = 0;
	int bits = 0;
	raw.get_mem_image_format(&width, &height, &colors, &bits);
	if (!ValidDimensions(width, height, errorMessage) || colors < 1 || colors > 4) return false;
	const int unpackResult = raw.unpack();
	if (unpackResult == LIBRAW_CANCELLED_BY_CALLBACK || !context.Continue()) {
		errorMessage = "source work was cancelled";
		return false;
	}
	const int processResult = unpackResult == LIBRAW_SUCCESS ? raw.dcraw_process() : unpackResult;
	if (processResult == LIBRAW_CANCELLED_BY_CALLBACK || !context.Continue()) {
		errorMessage = "source work was cancelled";
		return false;
	}
	if (unpackResult != LIBRAW_SUCCESS || processResult != LIBRAW_SUCCESS) {
		errorMessage = "RAW development failed";
		return false;
	}
	const int stride = width * colors;
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
	const int copyResult = raw.copy_mem_image(pixels.data(), stride, 1);
	if (copyResult == LIBRAW_CANCELLED_BY_CALLBACK || !context.Continue()) {
		errorMessage = "source work was cancelled";
		return false;
	}
	if (copyResult != LIBRAW_SUCCESS) {
		errorMessage = "RAW pixel extraction failed";
		return false;
	}
	std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
	for (int y = 0; y < height; ++y) {
		if (!context.Continue()) {
			errorMessage = "source work was cancelled";
			return false;
		}
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
	AnimationFrameBudget frameBudget(image);
	return AppendBGRA(image, width, height, std::move(bgra), 0,
		errorMessage, colors == 4, context, frameBudget);
}
#endif

} // namespace jpegview_linux::decoder_detail
