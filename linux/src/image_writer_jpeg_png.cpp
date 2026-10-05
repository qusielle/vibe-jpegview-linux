#include "image_writer_internal.h"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <jpeglib.h>
#include <png.h>
}

namespace jpegview_linux::detail {

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
	volatile bool created = false;
	compressor.err = jpeg_std_error(&error.base);
	error.base.error_exit = errorExit;
	// The post-setjmp creation flag must survive longjmp so compressor cleanup remains defined.
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
	if (std::fclose(file) != 0) {
		errorMessage = "error writing output file";
		return false;
	}
	return true;
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

	// libpng uses longjmp for errors; keep cleanup of its handle and row buffer explicit.
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

} // namespace jpegview_linux::detail
