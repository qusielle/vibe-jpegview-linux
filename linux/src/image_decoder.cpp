#include "image_decoder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>

extern "C" {
#include <png.h>
#include <zlib.h>
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../third_party/stb_image.h"

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

namespace jpegview_linux {
namespace {

constexpr std::uint64_t kMaxImagePixels = 100ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxAnimationBytes = 512ull * 1024ull * 1024ull;

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

bool ValidDimensions(int width, int height, std::string& errorMessage) {
	if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
		errorMessage = "image dimensions are not supported";
		return false;
	}
	if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxImagePixels) {
		errorMessage = "image is too large";
		return false;
	}
	return true;
}

bool AppendBGRA(DecodedImage& image, int width, int height, const std::uint8_t* pixels,
	int delayMs, std::string& errorMessage) {
	if (!ValidDimensions(width, height, errorMessage) || pixels == nullptr) return false;
	const std::size_t byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
	std::uint64_t currentBytes = 0;
	for (const DecodedFrame& frame : image.frames) currentBytes += frame.bgra.size();
	if (byteCount > kMaxAnimationBytes || currentBytes > kMaxAnimationBytes - byteCount) {
		errorMessage = "animation is too large";
		return false;
	}
	try {
		DecodedFrame frame;
		frame.width = width;
		frame.height = height;
		frame.delayMs = std::max(0, delayMs);
		frame.bgra.assign(pixels, pixels + byteCount);
		image.frames.push_back(std::move(frame));
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	return true;
}

bool AppendRGBA(DecodedImage& image, int width, int height, const std::uint8_t* pixels,
	int delayMs, std::string& errorMessage) {
	if (!ValidDimensions(width, height, errorMessage) || pixels == nullptr) return false;
	const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
	std::vector<std::uint8_t> bgra;
	try {
		bgra.resize(pixelCount * 4);
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
		bgra[pixel * 4] = pixels[pixel * 4 + 2];
		bgra[pixel * 4 + 1] = pixels[pixel * 4 + 1];
		bgra[pixel * 4 + 2] = pixels[pixel * 4];
		bgra[pixel * 4 + 3] = pixels[pixel * 4 + 3];
	}
	try {
		DecodedFrame frame;
		frame.width = width;
		frame.height = height;
		frame.delayMs = std::max(0, delayMs);
		frame.bgra = std::move(bgra);
		std::uint64_t currentBytes = 0;
		for (const DecodedFrame& existing : image.frames) currentBytes += existing.bgra.size();
		if (frame.bgra.size() > kMaxAnimationBytes || currentBytes > kMaxAnimationBytes - frame.bgra.size()) {
			errorMessage = "animation is too large";
			return false;
		}
		image.frames.push_back(std::move(frame));
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	return true;
}

[[maybe_unused]] bool ApplyIccProfile(std::vector<std::uint8_t>& bgra, const std::vector<std::uint8_t>& profile) {
#if JPEGVIEW_HAVE_LCMS2
	if (profile.empty() || profile.size() > std::numeric_limits<cmsUInt32Number>::max() ||
		bgra.empty() || bgra.size() / 4 > std::numeric_limits<cmsUInt32Number>::max()) return false;
	cmsHPROFILE source = cmsOpenProfileFromMem(profile.data(), static_cast<cmsUInt32Number>(profile.size()));
	cmsHPROFILE destination = cmsCreate_sRGBProfile();
	if (source == nullptr || destination == nullptr) {
		if (source != nullptr) cmsCloseProfile(source);
		if (destination != nullptr) cmsCloseProfile(destination);
		return false;
	}
	cmsHTRANSFORM transform = cmsCreateTransform(source, TYPE_BGRA_8, destination, TYPE_BGRA_8,
		INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA);
	if (transform == nullptr) {
		cmsCloseProfile(source);
		cmsCloseProfile(destination);
		return false;
	}
	cmsDoTransform(transform, bgra.data(), bgra.data(), static_cast<cmsUInt32Number>(bgra.size() / 4));
	cmsDeleteTransform(transform);
	cmsCloseProfile(source);
	cmsCloseProfile(destination);
	return true;
#else
	(void)bgra;
	(void)profile;
	return false;
#endif
}

bool ReadFile(const std::filesystem::path& filename, std::vector<std::uint8_t>& data,
	std::string& errorMessage) {
	std::ifstream input(filename, std::ios::binary);
	if (!input) {
		errorMessage = "cannot open file";
		return false;
	}
	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	if (size <= 0) {
		errorMessage = "empty file";
		return false;
	}
	input.seekg(0, std::ios::beg);
	try {
		data.resize(static_cast<std::size_t>(size));
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	input.read(reinterpret_cast<char*>(data.data()), size);
	if (!input) {
		errorMessage = "cannot read file";
		return false;
	}
	return true;
}

bool DecodeStb(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	int channels = 0;
	int width = 0;
	int height = 0;
	unsigned char* rgba = stbi_load(filename.string().c_str(), &width, &height, &channels, 4);
	if (rgba == nullptr) {
		errorMessage = stbi_failure_reason() == nullptr ? "unknown decoder error" : stbi_failure_reason();
		return false;
	}
	const bool result = AppendRGBA(image, width, height, rgba, 0, errorMessage);
	stbi_image_free(rgba);
	return result;
}

std::uint32_t ReadBE32(const std::uint8_t* data) {
	return (static_cast<std::uint32_t>(data[0]) << 24) |
		(static_cast<std::uint32_t>(data[1]) << 16) |
		(static_cast<std::uint32_t>(data[2]) << 8) | data[3];
}

std::uint16_t ReadBE16(const std::uint8_t* data) {
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

struct PngChunk {
	std::array<char, 4> type{};
	std::vector<std::uint8_t> data;
};

bool IsChunk(const PngChunk& chunk, std::string_view type) {
	return std::string_view(chunk.type.data(), chunk.type.size()) == type;
}

bool ParsePngChunks(const std::vector<std::uint8_t>& data, std::vector<PngChunk>& chunks,
	std::string& errorMessage) {
	static constexpr std::array<std::uint8_t, 8> signature = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
	if (data.size() < signature.size() || !std::equal(signature.begin(), signature.end(), data.begin())) {
		errorMessage = "invalid PNG signature";
		return false;
	}
	std::size_t position = signature.size();
	while (position + 12 <= data.size()) {
		const std::uint32_t length = ReadBE32(data.data() + position);
		position += 4;
		if (length > data.size() - position - 8) {
			errorMessage = "truncated PNG chunk";
			return false;
		}
		PngChunk chunk;
		std::copy_n(data.data() + position, 4, chunk.type.begin());
		position += 4;
		chunk.data.assign(data.begin() + static_cast<std::ptrdiff_t>(position),
			data.begin() + static_cast<std::ptrdiff_t>(position + length));
		position += length + 4; // Skip the chunk CRC; libpng validates the reconstructed chunks.
		chunks.push_back(std::move(chunk));
		if (IsChunk(chunks.back(), "IEND")) return true;
	}
	errorMessage = "PNG has no IEND chunk";
	return false;
}

void AppendPngChunk(std::vector<std::uint8_t>& output, const std::array<char, 4>& type,
	const std::vector<std::uint8_t>& data) {
	const auto append32 = [&output](std::uint32_t value) {
		output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
		output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
		output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
		output.push_back(static_cast<std::uint8_t>(value & 0xff));
	};
	append32(static_cast<std::uint32_t>(data.size()));
	output.insert(output.end(), type.begin(), type.end());
	output.insert(output.end(), data.begin(), data.end());
	const uLong typeCrc = crc32(0, reinterpret_cast<const Bytef*>(type.data()), type.size());
	const uLong dataCrc = crc32(typeCrc, data.data(), data.size());
	append32(static_cast<std::uint32_t>(dataCrc));
}

bool DecodePngMemory(const std::vector<std::uint8_t>& data, int expectedWidth, int expectedHeight,
	std::vector<std::uint8_t>& rgba, std::string& errorMessage) {
	png_image pngImage{};
	pngImage.version = PNG_IMAGE_VERSION;
	if (png_image_begin_read_from_memory(&pngImage, data.data(), data.size()) == 0) {
		errorMessage = pngImage.message[0] == '\0' ? "PNG decoder failed" : pngImage.message;
		return false;
	}
	if (!ValidDimensions(static_cast<int>(pngImage.width), static_cast<int>(pngImage.height), errorMessage) ||
		static_cast<int>(pngImage.width) != expectedWidth || static_cast<int>(pngImage.height) != expectedHeight) {
		png_image_free(&pngImage);
		if (errorMessage.empty()) errorMessage = "invalid APNG frame dimensions";
		return false;
	}
	pngImage.format = PNG_FORMAT_RGBA;
	try {
		rgba.resize(PNG_IMAGE_SIZE(pngImage));
	} catch (const std::exception&) {
		png_image_free(&pngImage);
		errorMessage = "out of memory";
		return false;
	}
	if (png_image_finish_read(&pngImage, nullptr, rgba.data(), 0, nullptr) == 0) {
		errorMessage = pngImage.message[0] == '\0' ? "PNG decoder failed" : pngImage.message;
		png_image_free(&pngImage);
		return false;
	}
	png_image_free(&pngImage);
	return true;
}

struct ApngFrameControl {
	int width = 0;
	int height = 0;
	int x = 0;
	int y = 0;
	std::uint16_t delayNumerator = 0;
	std::uint16_t delayDenominator = 100;
	std::uint8_t dispose = 0;
	std::uint8_t blend = 0;
};

struct ApngFrameData {
	ApngFrameControl control;
	std::vector<std::vector<std::uint8_t>> compressed;
};

bool DecodeApng(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> fileData;
	if (!ReadFile(filename, fileData, errorMessage)) return false;
	std::vector<PngChunk> chunks;
	if (!ParsePngChunks(fileData, chunks, errorMessage)) return false;
	if (chunks.empty() || !IsChunk(chunks.front(), "IHDR") || chunks.front().data.size() != 13) {
		errorMessage = "invalid PNG header";
		return false;
	}
	const int canvasWidth = static_cast<int>(ReadBE32(chunks.front().data.data()));
	const int canvasHeight = static_cast<int>(ReadBE32(chunks.front().data.data() + 4));
	if (!ValidDimensions(canvasWidth, canvasHeight, errorMessage)) return false;

	std::uint32_t frameCount = 0;
	std::uint32_t loopCount = 0;
	bool hasAnimationControl = false;
	for (const PngChunk& chunk : chunks) {
		if (IsChunk(chunk, "acTL") && chunk.data.size() == 8) {
			frameCount = ReadBE32(chunk.data.data());
			loopCount = ReadBE32(chunk.data.data() + 4);
			hasAnimationControl = frameCount > 0;
			break;
		}
	}
	if (!hasAnimationControl) return DecodeStb(filename, image, errorMessage);

	std::vector<ApngFrameData> frames;
	for (const PngChunk& chunk : chunks) {
		if (IsChunk(chunk, "fcTL")) {
			if (chunk.data.size() != 26) {
				errorMessage = "invalid APNG frame control";
				return false;
			}
			ApngFrameData frame;
			frame.control.width = static_cast<int>(ReadBE32(chunk.data.data() + 4));
			frame.control.height = static_cast<int>(ReadBE32(chunk.data.data() + 8));
			frame.control.x = static_cast<int>(ReadBE32(chunk.data.data() + 12));
			frame.control.y = static_cast<int>(ReadBE32(chunk.data.data() + 16));
			frame.control.delayNumerator = ReadBE16(chunk.data.data() + 20);
			frame.control.delayDenominator = ReadBE16(chunk.data.data() + 22);
			if (frame.control.delayDenominator == 0) frame.control.delayDenominator = 100;
			frame.control.dispose = chunk.data[24];
			frame.control.blend = chunk.data[25];
			if (!ValidDimensions(frame.control.width, frame.control.height, errorMessage) ||
				frame.control.x < 0 || frame.control.y < 0 ||
				frame.control.width > canvasWidth - frame.control.x ||
				frame.control.height > canvasHeight - frame.control.y ||
				frame.control.dispose > 2 || frame.control.blend > 1) {
				errorMessage = "invalid APNG frame geometry";
				return false;
			}
			frames.push_back(std::move(frame));
			continue;
		}
		if (IsChunk(chunk, "IDAT")) {
			if (frames.empty()) continue; // The default image is not an animation frame.
			frames.back().compressed.push_back(chunk.data);
			continue;
		}
		if (IsChunk(chunk, "fdAT")) {
			if (frames.empty() || chunk.data.size() < 4) {
				errorMessage = "invalid APNG frame data";
				return false;
			}
			frames.back().compressed.emplace_back(chunk.data.begin() + 4, chunk.data.end());
		}
	}
	if (frames.empty() || frames.size() != frameCount) {
		errorMessage = "APNG frame count is invalid";
		return false;
	}

	std::vector<std::uint8_t> pngSignature = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
	std::vector<std::uint8_t> canvas(static_cast<std::size_t>(canvasWidth) * canvasHeight * 4, 0);
	for (const ApngFrameData& frame : frames) {
		if (frame.compressed.empty()) {
			errorMessage = "APNG frame has no image data";
			return false;
		}
		std::vector<std::uint8_t> ihdr = chunks.front().data;
		ihdr[0] = static_cast<std::uint8_t>((frame.control.width >> 24) & 0xff);
		ihdr[1] = static_cast<std::uint8_t>((frame.control.width >> 16) & 0xff);
		ihdr[2] = static_cast<std::uint8_t>((frame.control.width >> 8) & 0xff);
		ihdr[3] = static_cast<std::uint8_t>(frame.control.width & 0xff);
		ihdr[4] = static_cast<std::uint8_t>((frame.control.height >> 24) & 0xff);
		ihdr[5] = static_cast<std::uint8_t>((frame.control.height >> 16) & 0xff);
		ihdr[6] = static_cast<std::uint8_t>((frame.control.height >> 8) & 0xff);
		ihdr[7] = static_cast<std::uint8_t>(frame.control.height & 0xff);
		std::vector<std::uint8_t> framePng = pngSignature;
		AppendPngChunk(framePng, chunks.front().type, ihdr);
		for (const PngChunk& chunk : chunks) {
			if (IsChunk(chunk, "IHDR") || IsChunk(chunk, "IEND") || IsChunk(chunk, "acTL") ||
				IsChunk(chunk, "fcTL") || IsChunk(chunk, "IDAT") || IsChunk(chunk, "fdAT")) continue;
			AppendPngChunk(framePng, chunk.type, chunk.data);
		}
		for (const std::vector<std::uint8_t>& compressed : frame.compressed) {
			AppendPngChunk(framePng, std::array<char, 4>{'I', 'D', 'A', 'T'}, compressed);
		}
		AppendPngChunk(framePng, std::array<char, 4>{'I', 'E', 'N', 'D'}, {});
		std::vector<std::uint8_t> rgba;
		if (!DecodePngMemory(framePng, frame.control.width, frame.control.height, rgba, errorMessage)) return false;

		std::vector<std::uint8_t> previous;
		if (frame.control.dispose == 2) previous = canvas;
		for (int y = 0; y < frame.control.height; ++y) {
			for (int x = 0; x < frame.control.width; ++x) {
				const std::size_t sourceIndex = (static_cast<std::size_t>(y) * frame.control.width + x) * 4;
				const std::size_t targetIndex = (static_cast<std::size_t>(frame.control.y + y) * canvasWidth +
					frame.control.x + x) * 4;
				if (frame.control.blend == 0 || rgba[sourceIndex + 3] == 255) {
					std::copy_n(rgba.data() + sourceIndex, 4, canvas.data() + targetIndex);
					continue;
				}
				const unsigned sourceAlpha = rgba[sourceIndex + 3];
				const unsigned destinationAlpha = canvas[targetIndex + 3];
				const unsigned outputAlpha = sourceAlpha + (destinationAlpha * (255 - sourceAlpha) + 127) / 255;
				if (outputAlpha == 0) {
					std::fill_n(canvas.data() + targetIndex, 4, 0);
					continue;
				}
				for (int channel = 0; channel < 3; ++channel) {
					const unsigned source = rgba[sourceIndex + channel] * sourceAlpha;
					const unsigned destination = canvas[targetIndex + channel] * destinationAlpha * (255 - sourceAlpha) / 255;
					canvas[targetIndex + channel] = static_cast<std::uint8_t>((source + destination + outputAlpha / 2) / outputAlpha);
				}
				canvas[targetIndex + 3] = static_cast<std::uint8_t>(outputAlpha);
			}
		}
		const int delay = frame.control.delayNumerator == 0 ? 10 : std::max(10,
			static_cast<int>((1000ull * frame.control.delayNumerator + frame.control.delayDenominator / 2) /
				frame.control.delayDenominator));
		if (!AppendRGBA(image, canvasWidth, canvasHeight, canvas.data(), delay, errorMessage)) return false;
		if (frame.control.dispose == 1) {
			for (int y = 0; y < frame.control.height; ++y) {
				std::fill_n(canvas.data() + (static_cast<std::size_t>(frame.control.y + y) * canvasWidth + frame.control.x) * 4,
					static_cast<std::size_t>(frame.control.width) * 4, 0);
			}
		} else if (frame.control.dispose == 2) {
			canvas = std::move(previous);
		}
	}
	image.animation = image.frames.size() > 1;
	image.loopCount = static_cast<int>(std::min<std::uint32_t>(loopCount, std::numeric_limits<int>::max()));
	return !image.frames.empty();
}

bool DecodeQoi(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> data;
	if (!ReadFile(filename, data, errorMessage)) return false;
	if (data.size() < 14 || std::memcmp(data.data(), "qoif", 4) != 0) {
		errorMessage = "invalid QOI header";
		return false;
	}
	const std::uint32_t widthValue = ReadBE32(data.data() + 4);
	const std::uint32_t heightValue = ReadBE32(data.data() + 8);
	if (widthValue > std::numeric_limits<int>::max() || heightValue > std::numeric_limits<int>::max() ||
		!ValidDimensions(static_cast<int>(widthValue), static_cast<int>(heightValue), errorMessage) ||
		(data[12] != 3 && data[12] != 4)) {
		errorMessage = "invalid QOI image dimensions or channels";
		return false;
	}
	const std::size_t pixelCount = static_cast<std::size_t>(widthValue) * heightValue;
	std::vector<std::uint8_t> rgba(pixelCount * 4);
	std::array<std::array<std::uint8_t, 4>, 64> index{};
	std::array<std::uint8_t, 4> pixel = {0, 0, 0, 255};
	std::size_t position = 14;
	std::size_t outputPixel = 0;
	std::size_t run = 0;
	const auto hash = [](const std::array<std::uint8_t, 4>& value) {
		return static_cast<std::size_t>((value[0] * 3 + value[1] * 5 + value[2] * 7 + value[3] * 11) % 64);
	};
	while (outputPixel < pixelCount) {
		if (run > 0) {
			--run;
		} else {
			if (position >= data.size()) {
				errorMessage = "truncated QOI data";
				return false;
			}
			const std::uint8_t tag = data[position++];
			if (tag == 0xfe) {
				if (position + 3 > data.size()) { errorMessage = "truncated QOI RGB data"; return false; }
				pixel[0] = data[position++]; pixel[1] = data[position++]; pixel[2] = data[position++];
			} else if (tag == 0xff) {
				if (position + 4 > data.size()) { errorMessage = "truncated QOI RGBA data"; return false; }
				for (std::uint8_t& channel : pixel) channel = data[position++];
			} else if ((tag & 0xc0) == 0x00) {
				pixel = index[tag & 0x3f];
			} else if ((tag & 0xc0) == 0x40) {
				pixel[0] = static_cast<std::uint8_t>(pixel[0] + ((tag >> 4) & 3) - 2);
				pixel[1] = static_cast<std::uint8_t>(pixel[1] + (tag & 15) / 4 - 2);
				pixel[2] = static_cast<std::uint8_t>(pixel[2] + (tag & 3) - 2);
			} else if ((tag & 0xc0) == 0x80) {
				if (position >= data.size()) { errorMessage = "truncated QOI luma data"; return false; }
				const std::uint8_t second = data[position++];
				const int green = (tag & 0x3f) - 32;
				const int red = green + ((second >> 4) & 15) - 8;
				const int blue = green + (second & 15) - 8;
				pixel[0] = static_cast<std::uint8_t>(pixel[0] + red);
				pixel[1] = static_cast<std::uint8_t>(pixel[1] + green);
				pixel[2] = static_cast<std::uint8_t>(pixel[2] + blue);
			} else {
				run = (tag & 0x3f);
			}
			index[hash(pixel)] = pixel;
		}
		std::copy(pixel.begin(), pixel.end(), rgba.begin() + static_cast<std::ptrdiff_t>(outputPixel * 4));
		++outputPixel;
	}
	if (position + 8 > data.size()) {
		errorMessage = "QOI end marker is missing";
		return false;
	}
	return AppendRGBA(image, static_cast<int>(widthValue), static_cast<int>(heightValue), rgba.data(), 0, errorMessage);
}

bool DecodePackBitsRow(const std::vector<std::uint8_t>& data, std::size_t& position,
	std::size_t rowEnd, std::uint8_t* output, std::size_t outputSize, bool allowShortRow,
	std::string& errorMessage) {
	std::size_t written = 0;
	while (written < outputSize) {
		if (position >= rowEnd) {
			if (allowShortRow) {
				std::fill(output + written, output + outputSize, 0);
				return true;
			}
			errorMessage = "truncated PSD PackBits data";
			return false;
		}
		const std::int8_t count = static_cast<std::int8_t>(data[position++]);
		if (count >= 0) {
			const std::size_t literalCount = static_cast<std::size_t>(count) + 1;
			if (literalCount > outputSize - written || literalCount > rowEnd - position) {
				errorMessage = "invalid PSD PackBits literal";
				return false;
			}
			std::copy_n(data.data() + position, literalCount, output + written);
			position += literalCount;
			written += literalCount;
		} else if (count != -128) {
			if (position >= rowEnd || static_cast<std::size_t>(1 - count) > outputSize - written) {
				errorMessage = "invalid PSD PackBits run";
				return false;
			}
			std::fill_n(output + written, static_cast<std::size_t>(1 - count), data[position++]);
			written += static_cast<std::size_t>(1 - count);
		}
	}
	return true;
}

bool DecodePsd(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> data;
	if (!ReadFile(filename, data, errorMessage)) return false;
	if (data.size() < 26 || std::memcmp(data.data(), "8BPS", 4) != 0 || ReadBE16(data.data() + 4) != 1) {
		errorMessage = "invalid PSD header";
		return false;
	}
	const std::uint16_t channels = ReadBE16(data.data() + 12);
	const std::uint32_t heightValue = ReadBE32(data.data() + 14);
	const std::uint32_t widthValue = ReadBE32(data.data() + 18);
	const std::uint16_t depth = ReadBE16(data.data() + 22);
	const std::uint16_t colorMode = ReadBE16(data.data() + 24);
	if (channels == 0 || channels > 56 || widthValue > std::numeric_limits<int>::max() ||
		heightValue > std::numeric_limits<int>::max() ||
		!ValidDimensions(static_cast<int>(widthValue), static_cast<int>(heightValue), errorMessage)) return false;
	const int width = static_cast<int>(widthValue);
	const int height = static_cast<int>(heightValue);
	if (!((depth == 1 && colorMode == 0) || depth == 8 || depth == 16)) {
		errorMessage = "unsupported PSD bit depth or color mode";
		return false;
	}
	std::size_t position = 26;
	const auto skipSection = [&data, &position](std::string& error) {
		if (position + 4 > data.size()) { error = "truncated PSD section"; return false; }
		const std::uint32_t length = ReadBE32(data.data() + position);
		position += 4;
		if (length > data.size() - position) { error = "truncated PSD section"; return false; }
		position += length;
		return true;
	};
	std::vector<std::uint8_t> colorModeData;
	if (position + 4 > data.size()) { errorMessage = "truncated PSD color mode data"; return false; }
	const std::uint32_t colorModeLength = ReadBE32(data.data() + position);
	position += 4;
	if (colorModeLength > data.size() - position) { errorMessage = "truncated PSD color mode data"; return false; }
	colorModeData.assign(data.begin() + static_cast<std::ptrdiff_t>(position),
		data.begin() + static_cast<std::ptrdiff_t>(position + colorModeLength));
	position += colorModeLength;
	if (!skipSection(errorMessage) || !skipSection(errorMessage)) return false;
	if (position + 2 > data.size()) { errorMessage = "truncated PSD image data"; return false; }
	const std::uint16_t compression = ReadBE16(data.data() + position);
	position += 2;
	if (compression > 1) {
		errorMessage = "unsupported PSD compression";
		return false;
	}
	const std::size_t sampleBytes = depth == 16 ? 2 : 1;
	const std::size_t rowBytes = depth == 1 ? (static_cast<std::size_t>(width) + 7) / 8 :
		static_cast<std::size_t>(width) * sampleBytes;
	if (rowBytes == 0 || static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / rowBytes) {
		errorMessage = "PSD image is too large";
		return false;
	}
	const std::size_t planeBytes = rowBytes * static_cast<std::size_t>(height);
	std::vector<std::vector<std::uint8_t>> planes(channels);
	try {
		for (std::vector<std::uint8_t>& plane : planes) plane.resize(planeBytes);
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	if (compression == 0) {
		for (std::vector<std::uint8_t>& plane : planes) {
			if (planeBytes > data.size() - position) {
				errorMessage = "truncated PSD image data";
				return false;
			}
			std::copy_n(data.data() + position, planeBytes, plane.data());
			position += planeBytes;
		}
	} else {
		const std::size_t rowCount = static_cast<std::size_t>(channels) * height;
		if (rowCount > (data.size() - position) / 2) {
			errorMessage = "truncated PSD PackBits row table";
			return false;
		}
		std::vector<std::uint16_t> rowLengths(rowCount);
		for (std::uint16_t& length : rowLengths) {
			length = ReadBE16(data.data() + position);
			position += 2;
		}
		for (std::size_t channel = 0; channel < channels; ++channel) {
			for (int y = 0; y < height; ++y) {
				const std::size_t rowIndex = channel * static_cast<std::size_t>(height) + y;
				const std::size_t compressedSize = rowLengths[rowIndex];
				if (compressedSize > data.size() - position) {
					errorMessage = "truncated PSD PackBits row";
					return false;
				}
				const std::size_t rowEnd = position + compressedSize;
				if (!DecodePackBitsRow(data, position, rowEnd,
					planes[channel].data() + static_cast<std::size_t>(y) * rowBytes, rowBytes,
					colorMode == 2 && depth == 8, errorMessage) || position > rowEnd) return false;
				position = rowEnd;
			}
		}
	}
	if (colorMode == 2 && colorModeData.size() < 768) {
		errorMessage = "PSD indexed palette is missing";
		return false;
	}
	if (colorMode == 3 && channels < 3) {
		errorMessage = "PSD RGB channels are missing";
		return false;
	}
	if (colorMode == 1 && channels < 1) {
		errorMessage = "PSD grayscale channel is missing";
		return false;
	}
	if (colorMode == 4 && channels < 4) {
		errorMessage = "PSD CMYK channels are missing";
		return false;
	}
	if (colorMode != 0 && colorMode != 1 && colorMode != 2 && colorMode != 3 && colorMode != 4) {
		errorMessage = "unsupported PSD color mode";
		return false;
	}
	const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
	std::vector<std::uint8_t> bgra(pixelCount * 4, 255);
	const auto sample = [&](const std::vector<std::uint8_t>& plane, std::size_t index) {
		return depth == 16 ? plane[index * 2] : plane[index];
	};
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t pixelIndex = static_cast<std::size_t>(y) * width + x;
			std::uint8_t red = 0;
			std::uint8_t green = 0;
			std::uint8_t blue = 0;
			std::uint8_t alpha = 255;
			if (colorMode == 0) {
				const bool black = (planes[0][static_cast<std::size_t>(y) * rowBytes + x / 8] & (0x80u >> (x % 8))) != 0;
				red = green = blue = black ? 0 : 255;
			} else if (colorMode == 1) {
				red = green = blue = sample(planes[0], pixelIndex);
				if (channels > 1) alpha = sample(planes[1], pixelIndex);
			} else if (colorMode == 2) {
				const std::size_t paletteIndex = static_cast<std::size_t>(sample(planes[0], pixelIndex));
				red = colorModeData[paletteIndex];
				green = colorModeData[256 + paletteIndex];
				blue = colorModeData[512 + paletteIndex];
				if (channels > 1) alpha = sample(planes[1], pixelIndex);
			} else if (colorMode == 3) {
				red = sample(planes[0], pixelIndex);
				green = sample(planes[1], pixelIndex);
				blue = sample(planes[2], pixelIndex);
				if (channels > 3) alpha = sample(planes[3], pixelIndex);
			} else {
				const int cyan = sample(planes[0], pixelIndex);
				const int magenta = sample(planes[1], pixelIndex);
				const int yellow = sample(planes[2], pixelIndex);
				const int black = sample(planes[3], pixelIndex);
				red = static_cast<std::uint8_t>((255 - cyan) * (255 - black) / 255);
				green = static_cast<std::uint8_t>((255 - magenta) * (255 - black) / 255);
				blue = static_cast<std::uint8_t>((255 - yellow) * (255 - black) / 255);
				if (channels > 4) alpha = sample(planes[4], pixelIndex);
			}
			bgra[pixelIndex * 4] = blue;
			bgra[pixelIndex * 4 + 1] = green;
			bgra[pixelIndex * 4 + 2] = red;
			bgra[pixelIndex * 4 + 3] = alpha;
		}
	}
	return AppendBGRA(image, width, height, bgra.data(), 0, errorMessage);
}

bool ReadPnmToken(const std::vector<std::uint8_t>& data, std::size_t& position, std::string& token) {
	while (position < data.size()) {
		if (std::isspace(data[position]) != 0) {
			++position;
			continue;
		}
		if (data[position] == '#') {
			while (position < data.size() && data[position] != '\n') ++position;
			continue;
		}
		break;
	}
	const std::size_t begin = position;
	while (position < data.size() && std::isspace(data[position]) == 0 && data[position] != '#') ++position;
	if (begin == position) return false;
	token.assign(reinterpret_cast<const char*>(data.data() + begin), position - begin);
	return true;
}

bool ParsePositiveInteger(const std::string& text, int& value) {
	try {
		std::size_t parsed = 0;
		const long long number = std::stoll(text, &parsed);
		if (parsed != text.size() || number <= 0 || number > std::numeric_limits<int>::max()) return false;
		value = static_cast<int>(number);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

bool ParseNonNegativeInteger(const std::string& text, int& value) {
	try {
		std::size_t parsed = 0;
		const long long number = std::stoll(text, &parsed);
		if (parsed != text.size() || number < 0 || number > std::numeric_limits<int>::max()) return false;
		value = static_cast<int>(number);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

bool DecodePnm(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> data;
	if (!ReadFile(filename, data, errorMessage)) return false;
	std::size_t position = 0;
	std::string magic;
	if (!ReadPnmToken(data, position, magic) || magic.size() != 2 || magic[0] != 'P' ||
		(magic[1] < '1' || magic[1] > '7')) {
		errorMessage = "invalid PNM header";
		return false;
	}
	int width = 0;
	int height = 0;
	const bool ascii = magic == "P1" || magic == "P2" || magic == "P3";
	int depth = magic == "P7" ? 0 : (magic == "P1" || magic == "P2" || magic == "P4" || magic == "P5" ? 1 : 3);
	int maxValue = magic == "P1" || magic == "P4" ? 1 : 0;
	if (magic == "P7") {
		bool ended = false;
		while (position < data.size()) {
			const std::size_t lineBegin = position;
			while (position < data.size() && data[position] != '\n') ++position;
			const std::string line(reinterpret_cast<const char*>(data.data() + lineBegin), position - lineBegin);
			if (position < data.size()) ++position;
			if (line == "ENDHDR" || line == "ENDHDR\r") {
				ended = true;
				break;
			}
			const std::size_t separator = line.find_first_of(" \t");
			if (separator == std::string::npos) continue;
			const std::string key = line.substr(0, separator);
			const std::string value = line.substr(separator + 1);
			if (key == "WIDTH") ParsePositiveInteger(value, width);
			else if (key == "HEIGHT") ParsePositiveInteger(value, height);
			else if (key == "DEPTH") ParsePositiveInteger(value, depth);
			else if (key == "MAXVAL") ParsePositiveInteger(value, maxValue);
		}
		if (!ended || width <= 0 || height <= 0 || depth <= 0 || maxValue <= 0) {
			errorMessage = "invalid PAM header";
			return false;
		}
	} else {
		std::string widthText;
		std::string heightText;
		if (!ReadPnmToken(data, position, widthText) || !ReadPnmToken(data, position, heightText) ||
			!ParsePositiveInteger(widthText, width) || !ParsePositiveInteger(heightText, height)) {
			errorMessage = "invalid PNM dimensions";
			return false;
		}
		if (magic != "P1" && magic != "P4") {
			std::string maxValueText;
			if (!ReadPnmToken(data, position, maxValueText) || !ParsePositiveInteger(maxValueText, maxValue) || maxValue > 65535) {
				errorMessage = "invalid PNM maximum value";
				return false;
			}
		}
		if (position >= data.size() || std::isspace(data[position]) == 0) {
			errorMessage = "invalid PNM data offset";
			return false;
		}
		++position;
	}
	if (!ValidDimensions(width, height, errorMessage)) return false;
	const std::size_t sampleBytes = maxValue > 255 ? 2 : 1;
	const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
	std::vector<std::uint8_t> bgra(pixelCount * 4, 255);
	if (ascii) {
		const std::size_t requiredSamples = pixelCount * static_cast<std::size_t>(depth);
		std::vector<int> samples;
		try {
			samples.reserve(requiredSamples);
		} catch (const std::exception&) {
			errorMessage = "out of memory";
			return false;
		}
		for (std::size_t sampleIndex = 0; sampleIndex < requiredSamples; ++sampleIndex) {
			std::string sampleText;
			int value = 0;
			if (!ReadPnmToken(data, position, sampleText) ||
				!ParseNonNegativeInteger(sampleText, value) || value > maxValue) {
				errorMessage = "invalid ASCII PNM sample";
				return false;
			}
			samples.push_back(value);
		}
		const auto scale = [maxValue](int value) {
			return static_cast<std::uint8_t>((value * 255 + maxValue / 2) / maxValue);
		};
		for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
			std::uint8_t* pixel = bgra.data() + pixelIndex * 4;
			if (magic == "P1") {
				const std::uint8_t value = samples[pixelIndex] == 0 ? 255 : 0;
				pixel[0] = pixel[1] = pixel[2] = value;
			} else if (depth == 1) {
				pixel[0] = pixel[1] = pixel[2] = scale(samples[pixelIndex]);
			} else {
				pixel[2] = scale(samples[pixelIndex * 3]);
				pixel[1] = scale(samples[pixelIndex * 3 + 1]);
				pixel[0] = scale(samples[pixelIndex * 3 + 2]);
			}
		}
	} else if (magic == "P4") {
		const std::size_t rowBytes = (static_cast<std::size_t>(width) + 7) / 8;
		if (position > data.size() || (rowBytes > 0 && static_cast<std::size_t>(height) >
			(data.size() - position) / rowBytes)) {
			errorMessage = "truncated PBM image";
			return false;
		}
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const bool black = (data[position + static_cast<std::size_t>(y) * rowBytes + x / 8] & (0x80u >> (x % 8))) != 0;
				const std::uint8_t value = black ? 0 : 255;
				std::uint8_t* pixel = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4;
				pixel[0] = pixel[1] = pixel[2] = value;
			}
		}
	} else {
		const std::size_t requiredSamples = pixelCount * static_cast<std::size_t>(depth);
		if (position > data.size() || (sampleBytes > 0 && requiredSamples >
			(data.size() - position) / sampleBytes)) {
			errorMessage = "truncated PNM image";
			return false;
		}
		const auto readSample = [&](std::size_t& offset) {
			int value = data[position + offset++];
			if (sampleBytes == 2) value = (value << 8) | data[position + offset++];
			return static_cast<std::uint8_t>((value * 255 + maxValue / 2) / maxValue);
		};
		std::size_t offset = 0;
		for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
			std::uint8_t* pixel = bgra.data() + pixelIndex * 4;
			if (depth == 1) {
				pixel[0] = pixel[1] = pixel[2] = readSample(offset);
			} else if (depth == 2) {
				pixel[0] = pixel[1] = pixel[2] = readSample(offset);
				pixel[3] = readSample(offset);
			} else {
				pixel[2] = readSample(offset);
				pixel[1] = readSample(offset);
				pixel[0] = readSample(offset);
				if (depth >= 4) pixel[3] = readSample(offset);
				for (int extra = 4; extra < depth; ++extra) readSample(offset);
			}
		}
	}
	return AppendBGRA(image, width, height, bgra.data(), 0, errorMessage);
}

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

bool DecodeGif(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	int gifError = 0;
	GifFileType* gif = DGifOpenFileName(filename.string().c_str(), &gifError);
	if (gif == nullptr) {
		errorMessage = GifErrorString(gifError) == nullptr ? "cannot open GIF" : GifErrorString(gifError);
		return false;
	}
	if (DGifSlurp(gif) != GIF_OK || gif->SWidth <= 0 || gif->SHeight <= 0) {
		errorMessage = GifErrorString(gif->Error) == nullptr ? "invalid GIF" : GifErrorString(gif->Error);
		DGifCloseFile(gif, nullptr);
		return false;
	}
	if (!ValidDimensions(gif->SWidth, gif->SHeight, errorMessage)) {
		DGifCloseFile(gif, nullptr);
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
			DGifCloseFile(gif, nullptr);
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
			DGifCloseFile(gif, nullptr);
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
	DGifCloseFile(gif, nullptr);
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
	WebPDemuxer* demuxer = WebPDemux(&data);
	if (demuxer != nullptr) {
		WebPChunkIterator iterator{};
		if (WebPDemuxGetChunk(demuxer, "ICCP", 1, &iterator)) {
			iccProfile.assign(iterator.chunk.bytes, iterator.chunk.bytes + iterator.chunk.size);
			WebPDemuxReleaseChunkIterator(&iterator);
		}
		WebPDemuxDelete(demuxer);
	}
	options.color_mode = MODE_BGRA;
	options.use_threads = 1;
	WebPAnimDecoder* decoder = WebPAnimDecoderNew(&data, &options);
	if (decoder == nullptr) {
		errorMessage = "invalid WebP image";
		return false;
	}
	WebPAnimInfo info{};
	if (!WebPAnimDecoderGetInfo(decoder, &info) || info.canvas_width <= 0 || info.canvas_height <= 0) {
		WebPAnimDecoderDelete(decoder);
		errorMessage = "invalid WebP animation";
		return false;
	}
	image.loopCount = info.loop_count;
	int previousTimestamp = 0;
	while (WebPAnimDecoderHasMoreFrames(decoder)) {
		uint8_t* pixels = nullptr;
		int timestamp = 0;
		if (!WebPAnimDecoderGetNext(decoder, &pixels, &timestamp) || pixels == nullptr) {
			errorMessage = "WebP frame decode failed";
			WebPAnimDecoderDelete(decoder);
			return false;
		}
		const int delay = timestamp > previousTimestamp ? timestamp - previousTimestamp : 100;
		std::vector<std::uint8_t> frame(pixels,
			pixels + static_cast<std::size_t>(info.canvas_width) * info.canvas_height * 4);
		ApplyIccProfile(frame, iccProfile);
		if (!AppendBGRA(image, info.canvas_width, info.canvas_height, frame.data(), std::max(10, delay), errorMessage)) {
			WebPAnimDecoderDelete(decoder);
			return false;
		}
		previousTimestamp = timestamp;
	}
	WebPAnimDecoderDelete(decoder);
	image.animation = image.frames.size() > 1;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_TIFF
bool DecodeTiff(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	TIFF* tiff = TIFFOpen(filename.string().c_str(), "r");
	if (tiff == nullptr) {
		errorMessage = "cannot open TIFF";
		return false;
	}
	for (tdir_t directory = 0; TIFFSetDirectory(tiff, directory) != 0; ++directory) {
		uint32_t width = 0;
		uint32_t height = 0;
		if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) == 0 ||
			TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) == 0 ||
			width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
			errorMessage = "invalid TIFF dimensions";
			TIFFClose(tiff);
			return false;
		}
		const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
		std::vector<uint32_t> raster;
		try {
			raster.resize(pixelCount);
		} catch (const std::exception&) {
			errorMessage = "out of memory";
			TIFFClose(tiff);
			return false;
		}
		if (!TIFFReadRGBAImageOriented(tiff, width, height, raster.data(), ORIENTATION_TOPLEFT, 0)) {
			errorMessage = "TIFF frame decode failed";
			TIFFClose(tiff);
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
		if (!AppendBGRA(image, static_cast<int>(width), static_cast<int>(height), bgra.data(), 0, errorMessage)) {
			TIFFClose(tiff);
			return false;
		}
	}
	TIFFClose(tiff);
	// TIFF pages are navigable frames but are not timed animations.
	image.animation = false;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_HEIF
bool DecodeHeif(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	heif_context* context = heif_context_alloc();
	if (context == nullptr) {
		errorMessage = "HEIF decoder unavailable";
		return false;
	}
	const heif_error readError = heif_context_read_from_file(context, filename.string().c_str(), nullptr);
	if (readError.code != heif_error_Ok) {
		errorMessage = readError.message == nullptr ? "HEIF read failed" : readError.message;
		heif_context_free(context);
		return false;
	}
	const int count = heif_context_get_number_of_top_level_images(context);
	if (count <= 0) {
		errorMessage = "HEIF contains no images";
		heif_context_free(context);
		return false;
	}
	std::vector<heif_item_id> ids(static_cast<std::size_t>(count));
	const int filled = heif_context_get_list_of_top_level_image_IDs(context, ids.data(), count);
	for (int index = 0; index < filled; ++index) {
		heif_image_handle* handle = nullptr;
		heif_error handleError = heif_context_get_image_handle(context, ids[static_cast<std::size_t>(index)], &handle);
		if (handleError.code != heif_error_Ok || handle == nullptr) {
			errorMessage = handleError.message == nullptr ? "HEIF image handle failed" : handleError.message;
			heif_context_free(context);
			return false;
		}
		std::vector<std::uint8_t> iccProfile;
		const size_t iccSize = heif_image_handle_get_raw_color_profile_size(handle);
		if (iccSize > 0 && iccSize <= kMaxAnimationBytes) {
			iccProfile.resize(iccSize);
			const heif_error profileError = heif_image_handle_get_raw_color_profile(handle, iccProfile.data());
			if (profileError.code != heif_error_Ok) iccProfile.clear();
		}
		heif_image* decoded = nullptr;
		const heif_error decodeError = heif_decode_image(handle, &decoded, heif_colorspace_RGB,
			heif_chroma_interleaved_RGBA, nullptr);
		heif_image_handle_release(handle);
		if (decodeError.code != heif_error_Ok || decoded == nullptr) {
			errorMessage = decodeError.message == nullptr ? "HEIF frame decode failed" : decodeError.message;
			heif_context_free(context);
			return false;
		}
		const int width = heif_image_get_width(decoded, heif_channel_interleaved);
		const int height = heif_image_get_height(decoded, heif_channel_interleaved);
		int stride = 0;
		const std::uint8_t* pixels = heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
		if (pixels == nullptr || stride < width * 4 || !ValidDimensions(width, height, errorMessage)) {
			errorMessage = pixels == nullptr ? "HEIF pixel plane unavailable" : errorMessage;
			heif_image_release(decoded);
			heif_context_free(context);
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
		heif_image_release(decoded);
		if (!AppendBGRA(image, width, height, bgra.data(), 100, errorMessage)) {
			heif_context_free(context);
			return false;
		}
	}
	heif_context_free(context);
	image.animation = image.frames.size() > 1;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_AVIF
bool DecodeAvif(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	avifDecoder* decoder = avifDecoderCreate();
	if (decoder == nullptr) {
		errorMessage = "AVIF decoder unavailable";
		return false;
	}
	if (avifDecoderSetIOFile(decoder, filename.string().c_str()) != AVIF_RESULT_OK ||
		avifDecoderParse(decoder) != AVIF_RESULT_OK || decoder->imageCount == 0) {
		errorMessage = "invalid AVIF image";
		avifDecoderDestroy(decoder);
		return false;
	}
	for (uint32_t index = 0; index < static_cast<uint32_t>(decoder->imageCount); ++index) {
		if (avifDecoderNthImage(decoder, index) != AVIF_RESULT_OK || decoder->image == nullptr) {
			errorMessage = "AVIF frame decode failed";
			avifDecoderDestroy(decoder);
			return false;
		}
		avifRGBImage rgb{};
		avifRGBImageSetDefaults(&rgb, decoder->image);
		rgb.depth = 8;
		rgb.format = AVIF_RGB_FORMAT_BGRA;
		avifRGBImageAllocatePixels(&rgb);
		if (rgb.pixels == nullptr ||
			avifImageYUVToRGB(decoder->image, &rgb) != AVIF_RESULT_OK) {
			errorMessage = "AVIF color conversion failed";
			avifRGBImageFreePixels(&rgb);
			avifDecoderDestroy(decoder);
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
			bgra.data(), std::max(10, delay), errorMessage);
		avifRGBImageFreePixels(&rgb);
		if (!appended) {
			avifDecoderDestroy(decoder);
			return false;
		}
	}
	avifDecoderDestroy(decoder);
	image.animation = image.frames.size() > 1;
	return !image.frames.empty();
}
#endif

#if JPEGVIEW_HAVE_JXL
bool DecodeJxl(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> encoded;
	if (!ReadFile(filename, encoded, errorMessage)) return false;
	JxlDecoder* decoder = JxlDecoderCreate(nullptr);
	void* runner = JxlResizableParallelRunnerCreate(nullptr);
	if (decoder == nullptr || runner == nullptr) {
		if (decoder != nullptr) JxlDecoderDestroy(decoder);
		if (runner != nullptr) JxlResizableParallelRunnerDestroy(runner);
		errorMessage = "JPEG XL decoder unavailable";
		return false;
	}
	const auto cleanup = [&]() {
		JxlDecoderDestroy(decoder);
		JxlResizableParallelRunnerDestroy(runner);
	};
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
			if (JxlDecoderGetICCProfileSize(decoder, nullptr, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
				&profileSize) == JXL_DEC_SUCCESS && profileSize > 0 && profileSize <= kMaxAnimationBytes) {
				iccProfile.resize(profileSize);
				if (JxlDecoderGetColorAsICCProfile(decoder, nullptr, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
					iccProfile.data(), iccProfile.size()) != JXL_DEC_SUCCESS) iccProfile.clear();
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
				bgra.data(), frameDelay, errorMessage)) {
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
bool DecodeJxr(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	PKImageDecode* decoder = nullptr;
	if (PKCodecFactory_CreateDecoderFromFile(filename.string().c_str(), &decoder) != WMP_errSuccess || decoder == nullptr) {
		errorMessage = "JPEG XR decoder unavailable or invalid image";
		return false;
	}
	PKFormatConverter* converter = nullptr;
	I32 width = 0;
	I32 height = 0;
	const bool initialized = PKImageDecode_GetSize(decoder, &width, &height) == WMP_errSuccess &&
		ValidDimensions(width, height, errorMessage) &&
		PKCodecFactory_CreateFormatConverter(&converter) == WMP_errSuccess && converter != nullptr &&
		PKFormatConverter_Initialize(converter, decoder, nullptr, GUID_PKPixelFormat24bppBGR) == WMP_errSuccess;
	if (!initialized) {
		if (converter != nullptr) PKFormatConverter_Release(&converter);
		PKImageDecode_Release(&decoder);
		if (errorMessage.empty()) errorMessage = "JPEG XR decoder initialization failed";
		return false;
	}
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
	const PKRect rect{0, 0, width, height};
	const bool copied = converter->Convert(converter, &rect, pixels.data(), static_cast<U32>(width * 3)) == WMP_errSuccess;
	PKFormatConverter_Release(&converter);
	PKImageDecode_Release(&decoder);
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
	return AppendBGRA(image, width, height, bgra.data(), 0, errorMessage);
}
#endif

#if JPEGVIEW_HAVE_RAW
bool DecodeRaw(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	LibRaw raw;
	if (raw.open_file(filename.string().c_str()) != LIBRAW_SUCCESS) {
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
	return AppendBGRA(image, width, height, bgra.data(), 0, errorMessage);
}
#endif

} // namespace

bool DecodeImage(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	image = {};
	errorMessage.clear();
	const std::string extension = Lower(filename.extension().string());
	if (extension == ".gif") {
#if JPEGVIEW_HAVE_GIF
		return DecodeGif(filename, image, errorMessage);
#else
		return DecodeStb(filename, image, errorMessage);
#endif
	}
	if (extension == ".apng") return DecodeApng(filename, image, errorMessage);
	if (extension == ".webp") {
#if JPEGVIEW_HAVE_WEBP
		return DecodeWebP(filename, image, errorMessage);
#else
		return DecodeStb(filename, image, errorMessage);
#endif
	}
	if (extension == ".tif" || extension == ".tiff") {
#if JPEGVIEW_HAVE_TIFF
		return DecodeTiff(filename, image, errorMessage);
#else
		errorMessage = "TIFF support is not available in this build";
		return false;
#endif
	}
	if (extension == ".heic" || extension == ".heif" || extension == ".hif") {
#if JPEGVIEW_HAVE_HEIF
		return DecodeHeif(filename, image, errorMessage);
#else
		errorMessage = "HEIF support is not available in this build";
		return false;
#endif
	}
	if (extension == ".avif" || extension == ".avifs") {
#if JPEGVIEW_HAVE_AVIF
		return DecodeAvif(filename, image, errorMessage);
#elif JPEGVIEW_HAVE_HEIF
		return DecodeHeif(filename, image, errorMessage);
#else
		errorMessage = "AVIF support is not available in this build";
		return false;
#endif
	}
	if (extension == ".jxl") {
#if JPEGVIEW_HAVE_JXL
		return DecodeJxl(filename, image, errorMessage);
#else
		errorMessage = "JPEG XL support is not available in this build";
		return false;
#endif
	}
	if (extension == ".jxr" || extension == ".wdp" || extension == ".hdp" || extension == ".mdp") {
#if JPEGVIEW_HAVE_JXR
		return DecodeJxr(filename, image, errorMessage);
#else
		errorMessage = "JPEG XR support is not available in this build";
		return false;
#endif
	}
	if (extension == ".psd") return DecodePsd(filename, image, errorMessage);
	static const std::array<const char*, 25> rawExtensions = {{
		".pef", ".dng", ".crw", ".nef", ".cr2", ".mrw", ".rw2", ".orf", ".x3f", ".arw",
		".kdc", ".nrw", ".dcr", ".sr2", ".raf", ".kc2", ".erf", ".3fr", ".raw", ".mef",
		".mos", ".mdc", ".cr3", ".iiq", ".rwl"
	}};
	if (std::find(rawExtensions.begin(), rawExtensions.end(), extension) != rawExtensions.end()) {
#if JPEGVIEW_HAVE_RAW
		return DecodeRaw(filename, image, errorMessage);
#else
		errorMessage = "RAW support is not available in this build";
		return false;
#endif
	}
	if (extension == ".pnm" || extension == ".pbm" || extension == ".pgm" ||
		 extension == ".ppm" || extension == ".pam") {
		return DecodePnm(filename, image, errorMessage);
	}
	if (extension == ".qoi") return DecodeQoi(filename, image, errorMessage);
	return DecodeStb(filename, image, errorMessage);
}
} // namespace jpegview_linux
