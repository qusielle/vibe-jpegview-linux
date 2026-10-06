#pragma once

#include <filesystem>
#include <functional>
#include <cstddef>
#include <cstdint>

namespace jpegview_linux {

enum class ImageContentFormat {
	Unknown,
	Jpeg,
	Png,
	Apng,
	Gif,
	Bmp,
	Tga,
	WebP,
	Tiff,
	Heif,
	Avif,
	JpegXl,
	JpegXr,
	Psd,
	Pnm,
	Qoi,
	Raw,
};

bool IsSupportedImagePath(const std::filesystem::path& path);
ImageContentFormat DetectImageContent(const std::uint8_t* bytes, std::size_t size);
ImageContentFormat ReadImageContentFormat(const std::filesystem::path& path,
	const std::function<bool()>& shouldContinue = {});

} // namespace jpegview_linux
