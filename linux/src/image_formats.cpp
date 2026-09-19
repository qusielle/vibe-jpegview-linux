#include "image_formats.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace jpegview_linux {
namespace {

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

} // namespace

bool IsSupportedImagePath(const std::filesystem::path& path) {
	static const std::vector<std::string_view> extensions = {
		".jpg", ".jpeg", ".jpe", ".png", ".gif", ".bmp", ".tga",
		".psd", ".pnm", ".pbm", ".pgm", ".ppm", ".pam", ".pic", ".qoi", ".apng", ".webp",
		".tif", ".tiff", ".heic", ".heif", ".hif", ".avif", ".avifs", ".jxl",
		".jxr", ".wdp", ".hdp", ".mdp", ".pef", ".dng", ".crw", ".nef", ".cr2",
		".mrw", ".rw2", ".orf", ".x3f", ".arw", ".kdc", ".nrw", ".dcr", ".sr2",
		".raf", ".kc2", ".erf", ".3fr", ".raw", ".mef", ".mos", ".mdc", ".cr3",
		".iiq", ".rwl"};
	const std::string extension = Lower(path.extension().string());
	return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

} // namespace jpegview_linux
