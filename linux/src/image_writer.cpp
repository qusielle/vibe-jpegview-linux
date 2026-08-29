#include "image_writer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <vector>

extern "C" {
#include <jpeglib.h>
#include <png.h>
}

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

namespace jpegview_linux {
namespace {

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

bool Validate(const std::uint8_t* bgra, int width, int height, std::string& errorMessage) {
	if (bgra == nullptr || width <= 0 || height <= 0) {
		errorMessage = "invalid image dimensions";
		return false;
	}
	const std::size_t widthValue = static_cast<std::size_t>(width);
	const std::size_t heightValue = static_cast<std::size_t>(height);
	if (widthValue > std::numeric_limits<std::size_t>::max() / heightValue ||
		widthValue * heightValue > std::numeric_limits<std::size_t>::max() / 4) {
		errorMessage = "image is too large";
		return false;
	}
	return true;
}

bool WriteJpeg(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, int quality, std::string& errorMessage) {
	struct ErrorManager {
		jpeg_error_mgr base{};
		jmp_buf jump{};
		char message[JMSG_LENGTH_MAX]{};
	};

	auto errorExit = [](j_common_ptr common) {
		ErrorManager* error = reinterpret_cast<ErrorManager*>(common->err);
		(*common->err->format_message)(common, error->message);
		longjmp(error->jump, 1);
	};

	FILE* file = std::fopen(filename.string().c_str(), "wb");
	if (file == nullptr) {
		errorMessage = "cannot open output file";
		return false;
	}

	unsigned char* row = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(width) * 3));
	if (row == nullptr) {
		std::fclose(file);
		errorMessage = "out of memory";
		return false;
	}

	ErrorManager error{};
	jpeg_compress_struct compressor{};
	bool created = false;
	compressor.err = jpeg_std_error(&error.base);
	error.base.error_exit = errorExit;
	if (setjmp(error.jump) != 0) {
		if (created) jpeg_destroy_compress(&compressor);
		std::free(row);
		std::fclose(file);
		errorMessage = error.message[0] == '\0' ? "JPEG encoder failed" : error.message;
		return false;
	}

	jpeg_create_compress(&compressor);
	created = true;
	jpeg_stdio_dest(&compressor, file);
	compressor.image_width = width;
	compressor.image_height = height;
	compressor.input_components = 3;
	compressor.in_color_space = JCS_RGB;
	jpeg_set_defaults(&compressor);
	jpeg_set_quality(&compressor, std::clamp(quality, 0, 100), TRUE);
	jpeg_start_compress(&compressor, TRUE);
	while (compressor.next_scanline < compressor.image_height) {
		const std::uint8_t* source = bgra + static_cast<std::size_t>(compressor.next_scanline) * width * 4;
		for (int x = 0; x < width; ++x) {
			row[x * 3] = source[x * 4 + 2];
			row[x * 3 + 1] = source[x * 4 + 1];
			row[x * 3 + 2] = source[x * 4];
		}
		JSAMPROW rowPointer = row;
		jpeg_write_scanlines(&compressor, &rowPointer, 1);
	}
	jpeg_finish_compress(&compressor);
	jpeg_destroy_compress(&compressor);
	std::free(row);
	const bool success = std::fclose(file) == 0;
	if (!success) errorMessage = "error writing output file";
	return success;
}

bool WritePng(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	FILE* file = std::fopen(filename.string().c_str(), "wb");
	if (file == nullptr) {
		errorMessage = "cannot open output file";
		return false;
	}
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (png == nullptr) {
		std::fclose(file);
		errorMessage = "PNG encoder unavailable";
		return false;
	}
	png_infop info = png_create_info_struct(png);
	if (info == nullptr) {
		png_destroy_write_struct(&png, nullptr);
		std::fclose(file);
		errorMessage = "PNG encoder unavailable";
		return false;
	}
	unsigned char* row = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(width) * 4));
	if (row == nullptr) {
		png_destroy_write_struct(&png, &info);
		std::fclose(file);
		errorMessage = "out of memory";
		return false;
	}

	if (setjmp(png_jmpbuf(png)) != 0) {
		std::free(row);
		png_destroy_write_struct(&png, &info);
		std::fclose(file);
		errorMessage = "PNG encoder failed";
		return false;
	}
	png_init_io(png, file);
	png_set_IHDR(png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height),
		8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (int y = 0; y < height; ++y) {
		const std::uint8_t* source = bgra + static_cast<std::size_t>(y) * width * 4;
		for (int x = 0; x < width; ++x) {
			row[x * 4] = source[x * 4 + 2];
			row[x * 4 + 1] = source[x * 4 + 1];
			row[x * 4 + 2] = source[x * 4];
			row[x * 4 + 3] = source[x * 4 + 3];
		}
		png_write_row(png, row);
	}
	png_write_end(png, info);
	std::free(row);
	png_destroy_write_struct(&png, &info);
	const bool success = std::fclose(file) == 0;
	if (!success) errorMessage = "error writing output file";
	return success;
}

void WriteU16(std::ostream& output, std::uint16_t value) {
	const char bytes[] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF)};
	output.write(bytes, sizeof(bytes));
}

void WriteU32(std::ostream& output, std::uint32_t value) {
	const char bytes[] = {
		static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF),
		static_cast<char>((value >> 16) & 0xFF), static_cast<char>((value >> 24) & 0xFF)};
	output.write(bytes, sizeof(bytes));
}

bool WriteBmp(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	const std::uint64_t pixelBytes = static_cast<std::uint64_t>(width) * height * 4;
	if (pixelBytes > std::numeric_limits<std::uint32_t>::max() - 54) {
		errorMessage = "image is too large for BMP";
		return false;
	}
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	WriteU16(output, 0x4D42);
	WriteU32(output, static_cast<std::uint32_t>(54 + pixelBytes));
	WriteU16(output, 0);
	WriteU16(output, 0);
	WriteU32(output, 54);
	WriteU32(output, 40);
	WriteU32(output, static_cast<std::uint32_t>(width));
	WriteU32(output, static_cast<std::uint32_t>(height));
	WriteU16(output, 1);
	WriteU16(output, 32);
	WriteU32(output, 0);
	WriteU32(output, static_cast<std::uint32_t>(pixelBytes));
	WriteU32(output, 2835);
	WriteU32(output, 2835);
	WriteU32(output, 0);
	WriteU32(output, 0);
	for (int y = height - 1; y >= 0; --y) {
		output.write(reinterpret_cast<const char*>(bgra + static_cast<std::size_t>(y) * width * 4),
			static_cast<std::streamsize>(width) * 4);
	}
	if (!output) errorMessage = "error writing output file";
	return static_cast<bool>(output);
}

bool WriteTga(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	if (width > std::numeric_limits<std::uint16_t>::max() || height > std::numeric_limits<std::uint16_t>::max()) {
		errorMessage = "image is too large for TGA";
		return false;
	}
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	const unsigned char header[18] = {
		0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		static_cast<unsigned char>(width & 0xFF), static_cast<unsigned char>((width >> 8) & 0xFF),
		static_cast<unsigned char>(height & 0xFF), static_cast<unsigned char>((height >> 8) & 0xFF),
		32, 0x28};
	output.write(reinterpret_cast<const char*>(header), sizeof(header));
	output.write(reinterpret_cast<const char*>(bgra), static_cast<std::streamsize>(width) * height * 4);
	if (!output) errorMessage = "error writing output file";
	return static_cast<bool>(output);
}

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
	auto encodeBGRA = reinterpret_cast<EncodeBGRA>(dlsym(library, "WebPEncodeBGRA"));
	auto encodeLosslessBGRA = reinterpret_cast<EncodeLosslessBGRA>(dlsym(library, "WebPEncodeLosslessBGRA"));
	auto freeMemory = reinterpret_cast<FreeMemory>(dlsym(library, "WebPFree"));
	if (freeMemory == nullptr || (options.webpLossless ? encodeLosslessBGRA == nullptr : encodeBGRA == nullptr)) {
		dlclose(library);
		errorMessage = "incompatible WebP encoder library";
		return false;
	}
	std::size_t outputSize = 0;
	unsigned char* encoded = nullptr;
	outputSize = options.webpLossless ?
		encodeLosslessBGRA(bgra, width, height, width * 4, &encoded) :
		encodeBGRA(bgra, width, height, width * 4, static_cast<float>(std::clamp(options.webpQuality, 0, 100)), &encoded);
	if (encoded == nullptr || outputSize == 0) {
		if (encoded != nullptr) freeMemory(encoded);
		dlclose(library);
		errorMessage = "WebP encoder failed";
		return false;
	}
	std::ofstream output(filename, std::ios::binary);
	if (output) output.write(reinterpret_cast<const char*>(encoded), static_cast<std::streamsize>(outputSize));
	const bool success = static_cast<bool>(output);
	freeMemory(encoded);
	dlclose(library);
	if (!success) errorMessage = "error writing output file";
	return success;
}

bool WritePnm(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, const std::string& extension, std::string& errorMessage) {
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	if (extension == ".pbm") {
		output << "P4\n" << width << ' ' << height << "\n";
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; x += 8) {
				std::uint8_t packed = 0;
				for (int bit = 0; bit < 8 && x + bit < width; ++bit) {
					const std::uint8_t* pixel = row + static_cast<std::size_t>(x + bit) * 4;
					const int luminance = (static_cast<int>(pixel[2]) * 299 +
						static_cast<int>(pixel[1]) * 587 + static_cast<int>(pixel[0]) * 114) / 1000;
					if (luminance < 128) packed |= static_cast<std::uint8_t>(0x80u >> bit);
				}
				output.put(static_cast<char>(packed));
			}
		}
	} else if (extension == ".pgm") {
		output << "P5\n" << width << ' ' << height << "\n255\n";
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) {
				const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
				const std::uint8_t luminance = static_cast<std::uint8_t>(
					(static_cast<int>(pixel[2]) * 299 + static_cast<int>(pixel[1]) * 587 +
						static_cast<int>(pixel[0]) * 114) / 1000);
				output.put(static_cast<char>(luminance));
			}
		}
	} else if (extension == ".pam") {
		output << "P7\nWIDTH " << width << "\nHEIGHT " << height
			   << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) {
				const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
				const char rgba[] = {static_cast<char>(pixel[2]), static_cast<char>(pixel[1]),
					static_cast<char>(pixel[0]), static_cast<char>(pixel[3])};
				output.write(rgba, sizeof(rgba));
			}
		}
	} else {
		output << "P6\n" << width << ' ' << height << "\n255\n";
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) {
				const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
				const char rgb[] = {static_cast<char>(pixel[2]), static_cast<char>(pixel[1]),
					static_cast<char>(pixel[0])};
				output.write(rgb, sizeof(rgb));
			}
		}
	}
	if (!output) errorMessage = "error writing output file";
	return static_cast<bool>(output);
}

bool WriteQoi(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	const auto write32 = [&output](std::uint32_t value) {
		const char bytes[] = {
			static_cast<char>((value >> 24) & 0xff), static_cast<char>((value >> 16) & 0xff),
			static_cast<char>((value >> 8) & 0xff), static_cast<char>(value & 0xff)};
		output.write(bytes, sizeof(bytes));
	};
	output.write("qoif", 4);
	write32(static_cast<std::uint32_t>(width));
	write32(static_cast<std::uint32_t>(height));
	output.put(4); // Preserve alpha.
	output.put(0); // sRGB with linear alpha.

	struct Pixel { std::uint8_t r, g, b, a; };
	std::array<Pixel, 64> index{};
	Pixel previous{0, 0, 0, 255};
	int run = 0;
	const auto hash = [](Pixel pixel) {
		return static_cast<std::size_t>((pixel.r * 3 + pixel.g * 5 + pixel.b * 7 + pixel.a * 11) % 64);
	};
	const auto flushRun = [&output, &run]() {
		if (run != 0) {
			output.put(static_cast<char>(0xc0 | (run - 1)));
			run = 0;
		}
	};
	for (int y = 0; y < height; ++y) {
		const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
		for (int x = 0; x < width; ++x) {
			const std::uint8_t* source = row + static_cast<std::size_t>(x) * 4;
			const Pixel current{source[2], source[1], source[0], source[3]};
			if (current.r == previous.r && current.g == previous.g && current.b == previous.b && current.a == previous.a) {
				++run;
				if (run == 62) flushRun();
				continue;
			}
			flushRun();
			const std::size_t slot = hash(current);
			const Pixel indexed = index[slot];
			if (indexed.r == current.r && indexed.g == current.g && indexed.b == current.b && indexed.a == current.a) {
				output.put(static_cast<char>(slot));
			} else {
				index[slot] = current;
				const int dr = static_cast<int>(current.r) - previous.r;
				const int dg = static_cast<int>(current.g) - previous.g;
				const int db = static_cast<int>(current.b) - previous.b;
				if (current.a == previous.a && dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
					output.put(static_cast<char>(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
				} else if (current.a == previous.a && dg >= -32 && dg <= 31 &&
					dr - dg >= -8 && dr - dg <= 7 && db - dg >= -8 && db - dg <= 7) {
					output.put(static_cast<char>(0x80 | (dg + 32)));
					output.put(static_cast<char>(((dr - dg + 8) << 4) | (db - dg + 8)));
				} else if (current.a == previous.a) {
					output.put(static_cast<char>(0xfe));
					output.put(static_cast<char>(current.r));
					output.put(static_cast<char>(current.g));
					output.put(static_cast<char>(current.b));
				} else {
					output.put(static_cast<char>(0xff));
					output.put(static_cast<char>(current.r));
					output.put(static_cast<char>(current.g));
					output.put(static_cast<char>(current.b));
					output.put(static_cast<char>(current.a));
				}
			}
			previous = current;
		}
	}
	flushRun();
	const char endMarker[] = {0, 0, 0, 0, 0, 0, 0, 1};
	output.write(endMarker, sizeof(endMarker));
	if (!output) errorMessage = "error writing QOI file";
	return static_cast<bool>(output);
}

void WriteBE16(std::ostream& output, std::uint16_t value) {
	const char bytes[] = {static_cast<char>((value >> 8) & 0xFF), static_cast<char>(value & 0xFF)};
	output.write(bytes, sizeof(bytes));
}

void WriteBE32(std::ostream& output, std::uint32_t value) {
	const char bytes[] = {
		static_cast<char>((value >> 24) & 0xFF), static_cast<char>((value >> 16) & 0xFF),
		static_cast<char>((value >> 8) & 0xFF), static_cast<char>(value & 0xFF)};
	output.write(bytes, sizeof(bytes));
}

bool WritePsd(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	if (static_cast<std::uint64_t>(width) > std::numeric_limits<std::uint32_t>::max() ||
		static_cast<std::uint64_t>(height) > std::numeric_limits<std::uint32_t>::max()) {
		errorMessage = "image is too large for PSD";
		return false;
	}
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	output.write("8BPS", 4);
	WriteBE16(output, 1);
	const char reserved[6] = {};
	output.write(reserved, sizeof(reserved));
	WriteBE16(output, 3); // RGB channels; alpha is omitted by this simple PSD writer.
	WriteBE32(output, static_cast<std::uint32_t>(height));
	WriteBE32(output, static_cast<std::uint32_t>(width));
	WriteBE16(output, 8);
	WriteBE16(output, 3); // RGB color mode.
	WriteBE32(output, 0); // color mode data
	WriteBE32(output, 0); // image resources
	WriteBE32(output, 0); // layer and mask information
	WriteBE16(output, 0); // raw image data
	for (int channel = 2; channel >= 0; --channel) {
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) output.put(static_cast<char>(row[x * 4 + channel]));
		}
	}
	if (!output) errorMessage = "error writing output file";
	return static_cast<bool>(output);
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
	TIFF* tiff = TIFFOpen(filename.string().c_str(), "w");
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
	TIFFClose(tiff);
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
	heif_context* context = heif_context_alloc();
	if (context == nullptr) {
		errorMessage = "HEIF encoder unavailable";
		return false;
	}
	heif_encoder* encoder = nullptr;
	heif_image* image = nullptr;
	heif_image_handle* handle = nullptr;
	const auto fail = [&](const heif_error& error, const char* fallback) {
		errorMessage = error.message == nullptr ? fallback : error.message;
	};
	heif_error error = heif_context_get_encoder_for_format(context, format, &encoder);
	if (error.code == heif_error_Ok) error = heif_encoder_set_lossy_quality(encoder, std::clamp(quality, 0, 100));
	if (error.code == heif_error_Ok) {
		error = heif_image_create(width, height, heif_colorspace_RGB,
			heif_chroma_interleaved_RGBA, &image);
	}
	if (error.code == heif_error_Ok) error = heif_image_add_plane(image, heif_channel_interleaved, width, height, 8);
	if (error.code == heif_error_Ok) {
		int stride = 0;
		std::uint8_t* target = heif_image_get_plane(image, heif_channel_interleaved, &stride);
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
		error = heif_context_encode_image(context, image, encoder, nullptr, &handle);
	}
	if (error.code == heif_error_Ok) error = heif_context_write_to_file(context, filename.string().c_str());
	if (error.code != heif_error_Ok) fail(error, avif ? "AVIF encoder failed" : "HEIF encoder failed");
	if (handle != nullptr) heif_image_handle_release(handle);
	if (image != nullptr) heif_image_release(image);
	if (encoder != nullptr) heif_encoder_release(encoder);
	heif_context_free(context);
	return error.code == heif_error_Ok;
}
#endif

#if JPEGVIEW_HAVE_AVIF
bool WriteAvif(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, int quality, std::string& errorMessage) {
	avifImage* image = avifImageCreate(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
		8, AVIF_PIXEL_FORMAT_YUV444);
	avifEncoder* encoder = avifEncoderCreate();
	if (image == nullptr || encoder == nullptr) {
		if (image != nullptr) avifImageDestroy(image);
		if (encoder != nullptr) avifEncoderDestroy(encoder);
		errorMessage = "AVIF encoder unavailable";
		return false;
	}
	avifRGBImage rgb{};
	avifRGBImageSetDefaults(&rgb, image);
	rgb.format = AVIF_RGB_FORMAT_BGRA;
	rgb.pixels = const_cast<std::uint8_t*>(bgra);
	rgb.rowBytes = static_cast<uint32_t>(width) * 4;
	avifResult result = avifImageRGBToYUV(image, &rgb);
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
	if (result == AVIF_RESULT_OK) result = avifEncoderWrite(encoder, image, &output);
	if (result == AVIF_RESULT_OK) {
		std::ofstream file(filename, std::ios::binary);
		if (file) file.write(reinterpret_cast<const char*>(output.data),
			static_cast<std::streamsize>(output.size));
		if (!file) result = AVIF_RESULT_IO_ERROR;
	}
	avifRWDataFree(&output);
	avifEncoderDestroy(encoder);
	avifImageDestroy(image);
	if (result != AVIF_RESULT_OK) {
		errorMessage = "AVIF encoder failed";
		return false;
	}
	return true;
}
#endif

#if JPEGVIEW_HAVE_JXL
bool WriteJxl(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, int quality, std::string& errorMessage) {
	JxlEncoder* encoder = JxlEncoderCreate(nullptr);
	void* runner = JxlResizableParallelRunnerCreate(nullptr);
	if (encoder == nullptr || runner == nullptr) {
		if (encoder != nullptr) JxlEncoderDestroy(encoder);
		if (runner != nullptr) JxlResizableParallelRunnerDestroy(runner);
		errorMessage = "JPEG XL encoder unavailable";
		return false;
	}
	const auto cleanup = [&]() {
		JxlEncoderDestroy(encoder);
		JxlResizableParallelRunnerDestroy(runner);
	};
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

} // namespace

bool WriteImage(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, const ImageWriteOptions& options, std::string& errorMessage) {
	if (!Validate(bgra, width, height, errorMessage)) return false;
	const std::string extension = Lower(filename.extension().string());
	if (extension == ".jpg" || extension == ".jpeg" || extension == ".jpe") {
		return WriteJpeg(filename, bgra, width, height, options.jpegQuality, errorMessage);
	}
	if (extension == ".png") return WritePng(filename, bgra, width, height, errorMessage);
	if (extension == ".bmp") return WriteBmp(filename, bgra, width, height, errorMessage);
	if (extension == ".tga") return WriteTga(filename, bgra, width, height, errorMessage);
	if (extension == ".webp") return WriteWebP(filename, bgra, width, height, options, errorMessage);
	if (extension == ".pnm" || extension == ".ppm" || extension == ".pgm" ||
		extension == ".pbm" || extension == ".pam") {
		return WritePnm(filename, bgra, width, height, extension, errorMessage);
	}
	if (extension == ".qoi") return WriteQoi(filename, bgra, width, height, errorMessage);
	if (extension == ".psd") return WritePsd(filename, bgra, width, height, errorMessage);
#if JPEGVIEW_HAVE_GIF
	if (extension == ".gif") return WriteGif(filename, bgra, width, height, errorMessage);
#endif
#if JPEGVIEW_HAVE_TIFF
	if (extension == ".tif" || extension == ".tiff") return WriteTiff(filename, bgra, width, height, errorMessage);
#endif
#if JPEGVIEW_HAVE_AVIF
	if (extension == ".avif" || extension == ".avifs") {
		return WriteAvif(filename, bgra, width, height, options.webpQuality, errorMessage);
	}
#elif JPEGVIEW_HAVE_HEIF
	if (extension == ".avif" || extension == ".avifs") {
		return WriteHeif(filename, bgra, width, height, options.webpQuality, true, errorMessage);
	}
#endif
#if JPEGVIEW_HAVE_HEIF
	if (extension == ".heic" || extension == ".heif" || extension == ".hif") {
		return WriteHeif(filename, bgra, width, height, options.webpQuality, false, errorMessage);
	}
#endif
#if JPEGVIEW_HAVE_JXL
	if (extension == ".jxl") return WriteJxl(filename, bgra, width, height, options.webpQuality, errorMessage);
#endif
	errorMessage = "unsupported output format (use JPEG, PNG, BMP, TGA, WebP, GIF, TIFF, PSD, PNM, QOI, HEIF, AVIF, or JXL; RAW and JPEG XR are decode-only)";
	return false;
}

} // namespace jpegview_linux
