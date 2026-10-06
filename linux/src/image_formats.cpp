#include "image_formats.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
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

bool Matches(const std::uint8_t* bytes, std::size_t size, std::size_t offset,
	std::string_view signature) {
	return bytes != nullptr && offset <= size && signature.size() <= size - offset &&
		std::equal(signature.begin(), signature.end(), bytes + offset,
			[](char left, std::uint8_t right) {
				return static_cast<unsigned char>(left) == right;
			});
}

std::uint32_t ReadBigEndian32(const std::uint8_t* bytes) {
	return (static_cast<std::uint32_t>(bytes[0]) << 24) |
		(static_cast<std::uint32_t>(bytes[1]) << 16) |
		(static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

bool ContainsBrand(const std::uint8_t* bytes, std::size_t size,
	std::string_view brand) {
	if (size < 12 || brand.size() != 4) return false;
	if (!Matches(bytes, size, 4, "ftyp")) return false;
	const std::uint32_t boxSize = ReadBigEndian32(bytes);
	if (boxSize < 16) return false;
	const std::size_t limit = std::min<std::size_t>(size,
		boxSize == 1 && size >= 16 ? size : boxSize);
	if (Matches(bytes, size, 8, brand)) return true;
	for (std::size_t offset = 16; offset + 4 <= limit; offset += 4) {
		if (Matches(bytes, size, offset, brand)) return true;
	}
	return false;
}

bool HasTgaHeader(const std::uint8_t* bytes, std::size_t size) {
	if (bytes == nullptr || size < 18) return false;
	const std::uint8_t colorMapType = bytes[1];
	const std::uint8_t imageType = bytes[2];
	const std::uint16_t width = static_cast<std::uint16_t>(bytes[12] |
		(static_cast<std::uint16_t>(bytes[13]) << 8));
	const std::uint16_t height = static_cast<std::uint16_t>(bytes[14] |
		(static_cast<std::uint16_t>(bytes[15]) << 8));
	const std::uint8_t depth = bytes[16];
	if (colorMapType > 1 || width == 0 || height == 0 ||
		(imageType != 1 && imageType != 2 && imageType != 3 &&
		 imageType != 9 && imageType != 10 && imageType != 11)) return false;
	if ((imageType == 1 || imageType == 9) && colorMapType != 1) return false;
	if ((imageType == 2 || imageType == 10) &&
		(depth != 15 && depth != 16 && depth != 24 && depth != 32)) return false;
	if ((imageType == 3 || imageType == 11) && depth != 8 && depth != 16) return false;
	return true;
}

bool HasPnmHeader(const std::uint8_t* bytes, std::size_t size) {
	if (bytes == nullptr || size < 3 || bytes[0] != 'P' ||
		bytes[1] < '1' || bytes[1] > '7') return false;
	return bytes[2] == ' ' || bytes[2] == '\t' || bytes[2] == '\r' ||
		bytes[2] == '\n' || bytes[2] == '#';
}

bool Continue(const std::function<bool()>& shouldContinue) {
	if (!shouldContinue) return true;
	try {
		return shouldContinue();
	} catch (...) {
		return false;
	}
}

} // namespace

bool IsSupportedImagePath(const std::filesystem::path& path) {
	static const std::vector<std::string_view> extensions = {
		".jpg", ".jpeg", ".jpe", ".png", ".gif", ".bmp", ".tga",
		".psd", ".pnm", ".pbm", ".pgm", ".ppm", ".pam", ".pic", ".qoi", ".apng", ".webp",
		".kra",
		".tif", ".tiff", ".heic", ".heif", ".hif", ".avif", ".avifs", ".jxl",
		".jxr", ".wdp", ".hdp", ".mdp", ".pef", ".dng", ".crw", ".nef", ".cr2",
		".mrw", ".rw2", ".orf", ".x3f", ".arw", ".kdc", ".nrw", ".dcr", ".sr2",
		".raf", ".kc2", ".erf", ".3fr", ".raw", ".mef", ".mos", ".mdc", ".cr3",
		".iiq", ".rwl"};
	const std::string extension = Lower(path.extension().string());
	return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

ImageContentFormat DetectImageContent(const std::uint8_t* bytes, std::size_t size) {
	if (bytes == nullptr || size == 0) return ImageContentFormat::Unknown;
	if (Matches(bytes, size, 0, "\xff\xd8\xff")) return ImageContentFormat::Jpeg;
	if (Matches(bytes, size, 0, "\x89PNG\r\n\x1a\n")) {
		for (std::size_t offset = 8; offset + 8 <= size;) {
			const std::uint32_t chunkSize = ReadBigEndian32(bytes + offset);
			if (Matches(bytes, size, offset + 4, "acTL")) return ImageContentFormat::Apng;
			if (Matches(bytes, size, offset + 4, "IDAT") ||
				Matches(bytes, size, offset + 4, "IEND")) break;
			const std::uint64_t next = static_cast<std::uint64_t>(offset) +
				static_cast<std::uint64_t>(chunkSize) + 12;
			if (next > size || next <= offset) break;
			offset = static_cast<std::size_t>(next);
		}
		return ImageContentFormat::Png;
	}
	if (Matches(bytes, size, 0, "GIF87a") || Matches(bytes, size, 0, "GIF89a")) {
		return ImageContentFormat::Gif;
	}
	if (Matches(bytes, size, 0, "BM")) return ImageContentFormat::Bmp;
	if (Matches(bytes, size, 0, "RIFF") && Matches(bytes, size, 8, "WEBP")) {
		return ImageContentFormat::WebP;
	}
	if (Matches(bytes, size, 0, "qoif")) return ImageContentFormat::Qoi;
	if (Matches(bytes, size, 0, "8BPS")) return ImageContentFormat::Psd;
	if (Matches(bytes, size, 0, "II\xbc\x01") || Matches(bytes, size, 0, "MM\x01\xbc")) {
		return ImageContentFormat::JpegXr;
	}
	if (Matches(bytes, size, 0,
		std::string_view("\x00\x00\x00\x0cJXL \r\n\x87\n", 12)) ||
		Matches(bytes, size, 0, "\xff\x0a")) return ImageContentFormat::JpegXl;
	if (Matches(bytes, size, 0, "IIRO") || Matches(bytes, size, 0, "MMOR") ||
		Matches(bytes, size, 0, "FUJIFILMCCD-RAW") ||
		Matches(bytes, size, 0, std::string_view("II\x1a\x00\x00\x00HEAPCCDR", 14)) ||
		Matches(bytes, size, 0, "FOVb") ||
		Matches(bytes, size, 0, std::string_view("\x00MRM", 4))) {
		return ImageContentFormat::Raw;
	}
	if (size >= 12 && Matches(bytes, size, 4, "ftyp")) {
		if (ContainsBrand(bytes, size, "crx ")) return ImageContentFormat::Raw;
		if (ContainsBrand(bytes, size, "avif") || ContainsBrand(bytes, size, "avis")) {
			return ImageContentFormat::Avif;
		}
		if (ContainsBrand(bytes, size, "heic") || ContainsBrand(bytes, size, "heix") ||
			ContainsBrand(bytes, size, "hevc") || ContainsBrand(bytes, size, "hevx") ||
			ContainsBrand(bytes, size, "heim") || ContainsBrand(bytes, size, "heis") ||
			ContainsBrand(bytes, size, "hevm") || ContainsBrand(bytes, size, "hevs") ||
			ContainsBrand(bytes, size, "mif1") || ContainsBrand(bytes, size, "msf1")) {
			return ImageContentFormat::Heif;
		}
	}
	if (Matches(bytes, size, 0, std::string_view("II*\x00", 4)) ||
		Matches(bytes, size, 0, std::string_view("MM\x00*", 4))) {
		if (Matches(bytes, size, 8, "CR") && size >= 12 && bytes[10] == 2 && bytes[11] == 0) {
			return ImageContentFormat::Raw;
		}
		return ImageContentFormat::Tiff;
	}
	if (HasPnmHeader(bytes, size)) return ImageContentFormat::Pnm;
	if (HasTgaHeader(bytes, size)) return ImageContentFormat::Tga;
	return ImageContentFormat::Unknown;
}

ImageContentFormat ReadImageContentFormat(const std::filesystem::path& path,
	const std::function<bool()>& shouldContinue) {
	if (!Continue(shouldContinue)) return ImageContentFormat::Unknown;
	std::ifstream input(path, std::ios::binary);
	if (!input) return ImageContentFormat::Unknown;
	std::array<std::uint8_t, 64> header{};
	input.read(reinterpret_cast<char*>(header.data()),
		static_cast<std::streamsize>(header.size()));
	const std::size_t bytesRead = static_cast<std::size_t>(input.gcount());
	ImageContentFormat format = DetectImageContent(header.data(), bytesRead);
	if (!Continue(shouldContinue)) return ImageContentFormat::Unknown;
	if (format != ImageContentFormat::Png) return format;

	constexpr std::uint64_t maximumPngProbeBytes = 256 * 1024;
	input.clear();
	input.seekg(8, std::ios::beg);
	std::uint64_t scanned = 8;
	while (input && scanned < maximumPngProbeBytes && Continue(shouldContinue)) {
		std::array<std::uint8_t, 8> chunkHeader{};
		if (!input.read(reinterpret_cast<char*>(chunkHeader.data()),
			static_cast<std::streamsize>(chunkHeader.size()))) break;
		const std::uint32_t chunkSize = ReadBigEndian32(chunkHeader.data());
		if (Matches(chunkHeader.data(), chunkHeader.size(), 4, "acTL")) {
			return ImageContentFormat::Apng;
		}
		if (Matches(chunkHeader.data(), chunkHeader.size(), 4, "IDAT") ||
			Matches(chunkHeader.data(), chunkHeader.size(), 4, "IEND")) break;
		const std::uint64_t skipped = static_cast<std::uint64_t>(chunkSize) + 4;
		if (skipped > maximumPngProbeBytes - scanned) {
			return ImageContentFormat::Unknown;
		}
		input.seekg(static_cast<std::streamoff>(skipped), std::ios::cur);
		if (!input) break;
		scanned += skipped + chunkHeader.size();
	}
	if (!Continue(shouldContinue)) return ImageContentFormat::Unknown;
	if (scanned >= maximumPngProbeBytes) return ImageContentFormat::Unknown;
	return ImageContentFormat::Png;
}

} // namespace jpegview_linux
