#include "image_metadata_reader.h"

#include "archive_source.h"
#include "image_formats.h"
#include "tiff_metadata_reader.h"

#include <algorithm>
#include <cctype>
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

	const ImageContentFormat format = ReadImageContentFormat(filename,
		[context] { return context.Continue(); });
	if (!context.Continue()) return false;
	switch (format) {
	case ImageContentFormat::Jpeg:
		return ReadJpegMetadata(filename, info, imageComment, context);
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
