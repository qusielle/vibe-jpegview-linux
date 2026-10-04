#include "image_writer.h"
#include "image_writer_internal.h"

#include <algorithm>
#include <cctype>
#include <limits>

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

} // namespace

bool WriteImage(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, const ImageWriteOptions& options, std::string& errorMessage) {
	return WriteImageWithFormat(filename, filename.extension().string(), bgra,
		width, height, options, errorMessage);
}

bool WriteImageWithFormat(const std::filesystem::path& filename,
	const std::string& formatExtension, const std::uint8_t* bgra,
	int width, int height, const ImageWriteOptions& options,
	std::string& errorMessage) {
	if (!Validate(bgra, width, height, errorMessage)) return false;
	const std::string extension = Lower(formatExtension);
	if (extension == ".jpg" || extension == ".jpeg" || extension == ".jpe") {
		return detail::WriteJpeg(filename, bgra, width, height, options.jpegQuality, errorMessage);
	}
	if (extension == ".png") return detail::WritePng(filename, bgra, width, height, errorMessage);
	if (extension == ".bmp") return detail::WriteBmp(filename, bgra, width, height, errorMessage);
	if (extension == ".tga") return detail::WriteTga(filename, bgra, width, height, errorMessage);
	if (extension == ".webp") return detail::WriteWebP(filename, bgra, width, height, options, errorMessage);
	if (extension == ".pnm" || extension == ".ppm" || extension == ".pgm" ||
		extension == ".pbm" || extension == ".pam") {
		return detail::WritePnm(filename, bgra, width, height, extension, errorMessage);
	}
	if (extension == ".qoi") return detail::WriteQoi(filename, bgra, width, height, errorMessage);
	if (extension == ".psd") return detail::WritePsd(filename, bgra, width, height, errorMessage);
#if JPEGVIEW_HAVE_GIF
	if (extension == ".gif") return detail::WriteGif(filename, bgra, width, height, errorMessage);
#endif
#if JPEGVIEW_HAVE_TIFF
	if (extension == ".tif" || extension == ".tiff") return detail::WriteTiff(filename, bgra, width, height, errorMessage);
#endif
#if JPEGVIEW_HAVE_AVIF
	if (extension == ".avif" || extension == ".avifs") {
		return detail::WriteAvif(filename, bgra, width, height, options.webpQuality, errorMessage);
	}
#elif JPEGVIEW_HAVE_HEIF
	if (extension == ".avif" || extension == ".avifs") {
		return detail::WriteHeif(filename, bgra, width, height, options.webpQuality, true, errorMessage);
	}
#endif
#if JPEGVIEW_HAVE_HEIF
	if (extension == ".heic" || extension == ".heif" || extension == ".hif") {
		return detail::WriteHeif(filename, bgra, width, height, options.webpQuality, false, errorMessage);
	}
#endif
#if JPEGVIEW_HAVE_JXL
	if (extension == ".jxl") return detail::WriteJxl(filename, bgra, width, height, options.webpQuality, errorMessage);
#endif
	errorMessage = "unsupported output format (use JPEG, PNG, BMP, TGA, WebP, GIF, TIFF, PSD, PNM, QOI, HEIF, AVIF, or JXL; RAW and JPEG XR are decode-only)";
	return false;
}

} // namespace jpegview_linux
