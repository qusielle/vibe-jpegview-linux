#include "image_writer.h"

#include <algorithm>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>

extern "C" {
#include <jpeglib.h>
#include <png.h>
}

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
	errorMessage = "unsupported output format (use .jpg, .png, .bmp, .tga, or .webp)";
	return false;
}

} // namespace jpegview_linux
