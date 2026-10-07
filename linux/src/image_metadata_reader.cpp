#include "image_metadata_reader.h"

#include "archive_source.h"
#include "image_formats.h"
#include "image_metadata_reader_internal.h"
#include "tiff_metadata_reader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
	return value;
}

bool HasPngSignature(const fs::path& filename, const WorkContext& context) {
	static constexpr std::array<std::uint8_t, 8> signature = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	if (!context.Continue()) return false;
	std::ifstream input(filename, std::ios::binary);
	std::array<std::uint8_t, 8> bytes{};
	return input && input.read(reinterpret_cast<char*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size())) && bytes == signature &&
		context.Continue();
}

} // namespace

bool ReadImageMetadata(const fs::path& filename, ExifInfo& info,
	std::string& imageComment, const WorkContext& supplied) {
	info = {};
	imageComment.clear();
	const WorkContext context = ResolveWorkContext(filename,
		SourceWorkPriority::Metadata, supplied);
	if (!context.Continue()) return false;
	if (IsArchiveMemberLocation(filename)) {
		std::string errorMessage;
		return WithArchiveMemberFile(filename,
			[&info, &imageComment, context](const fs::path& temporary, std::string&) {
				return ReadImageMetadata(temporary, info, imageComment, context);
			}, errorMessage, nullptr, [context] { return context.Continue(); });
	}

	ImageContentFormat format = ReadImageContentFormat(filename,
		[context] { return context.Continue(); });
	if (!context.Continue()) return false;
	if (format == ImageContentFormat::Unknown && HasPngSignature(filename, context)) {
		format = ImageContentFormat::Png;
	}
	switch (format) {
	case ImageContentFormat::Jpeg:
		return ReadJpegMetadata(filename, info, imageComment, context);
	case ImageContentFormat::Png:
	case ImageContentFormat::Apng:
		return detail::ReadPngMetadata(filename, info, context);
	case ImageContentFormat::WebP:
		return detail::ReadWebpMetadata(filename, info, context);
	case ImageContentFormat::Tiff:
		return ReadTiffMetadataFile(filename, info, context);
	case ImageContentFormat::Raw:
		if (Lower(filename.extension().string()) == ".dng") {
			return ReadTiffMetadataFile(filename, info, context);
		}
		return false;
	default:
		return false;
	}
}

} // namespace jpegview_linux
