#include "archive_source.h"
#include "exif_reader.h"
#include "tiff_metadata_reader.h"
#include "work_context.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

std::string TrimAscii(const std::uint8_t* data, std::size_t length) {
	std::string value(reinterpret_cast<const char*>(data), length);
	const std::size_t nul = value.find('\0');
	if (nul != std::string::npos) value.resize(nul);
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.pop_back();
	return value;
}

} // namespace

bool ReadJpegMetadata(const fs::path& filename, ExifInfo& info, std::string& jpegComment,
	const WorkContext& supplied) {
	info = {};
	jpegComment.clear();
	WorkContext context = ResolveWorkContext(filename, SourceWorkPriority::Metadata, supplied);
	if (!context.Continue()) return false;
	if (IsArchiveMemberLocation(filename)) {
		std::string errorMessage;
		return WithArchiveMemberFile(filename,
			[&info, &jpegComment, context](const fs::path& temporary, std::string&) {
				return ReadJpegMetadata(temporary, info, jpegComment, context);
			}, errorMessage, nullptr, [context] { return context.Continue(); });
	}
	if (!context.Continue()) return false;
	std::ifstream input(filename, std::ios::binary);
	if (!input) return false;
	const int first = input.get();
	const int second = input.get();
	if (first != 0xFF || second != 0xD8) return false;

	bool foundMetadata = false;
	for (;;) {
		if (!context.Continue()) return false;
		int prefix = input.get();
		if (prefix != 0xFF) break;
		int markerValue = input.get();
		while (markerValue == 0xFF) markerValue = input.get();
		if (markerValue < 0) break;
		const std::uint8_t marker = static_cast<std::uint8_t>(markerValue);
		if (marker == 0xD9 || marker == 0xDA) break;
		if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) continue;
		const int lengthHigh = input.get();
		const int lengthLow = input.get();
		if (lengthHigh < 0 || lengthLow < 0) break;
		const std::size_t segmentLength =
			(static_cast<std::size_t>(lengthHigh) << 8) |
			static_cast<std::size_t>(lengthLow);
		if (segmentLength < 2) break;
		if (!context.Continue()) return false;
		const std::size_t payloadLength = segmentLength - 2;
		if (marker != 0xE1 && marker != 0xFE) {
			input.seekg(static_cast<std::streamoff>(payloadLength), std::ios::cur);
			if (!input) break;
			continue;
		}
		std::vector<std::uint8_t> payload(payloadLength);
		if (payloadLength > 0 && !input.read(reinterpret_cast<char*>(payload.data()),
			static_cast<std::streamsize>(payloadLength))) break;
		if (marker == 0xE1 && payloadLength >= 6 &&
			std::memcmp(payload.data(), "Exif\0\0", 6) == 0) {
			foundMetadata = ReadTiffMetadata(payload.data(), payload.size(), 6, info) ||
				foundMetadata;
		} else if (marker == 0xFE && jpegComment.empty() && payloadLength > 0) {
			jpegComment = TrimAscii(payload.data(), payloadLength);
		}
	}
	return foundMetadata || !jpegComment.empty();
}

} // namespace jpegview_linux
