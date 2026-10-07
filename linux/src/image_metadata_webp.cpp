#include "image_metadata_reader_internal.h"

#include "tiff_metadata_reader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <utility>

namespace jpegview_linux::detail {
namespace {

std::uint32_t ReadLittleEndian32(const std::uint8_t* bytes) {
	return static_cast<std::uint32_t>(bytes[0]) |
		(static_cast<std::uint32_t>(bytes[1]) << 8) |
		(static_cast<std::uint32_t>(bytes[2]) << 16) |
		(static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool GetFileSize(std::ifstream& input, std::uint64_t& size) {
	input.seekg(0, std::ios::end);
	const std::streamoff end = input.tellg();
	if (end < 0) return false;
	size = static_cast<std::uint64_t>(end);
	return true;
}

bool ReadAt(std::ifstream& input, std::uint64_t fileSize, std::uint64_t offset,
	std::uint8_t* destination, std::size_t length, const WorkContext& context) {
	if (!context.Continue() || destination == nullptr || offset > fileSize ||
		length > fileSize - offset ||
		offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
		length > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
		return false;
	}
	input.clear();
	input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
	if (!input) return false;
	if (length != 0 && !input.read(reinterpret_cast<char*>(destination),
		static_cast<std::streamsize>(length))) return false;
	return context.Continue();
}

bool IsExifChunk(const std::array<std::uint8_t, 8>& header) {
	static constexpr std::array<std::uint8_t, 4> name = {'E', 'X', 'I', 'F'};
	return std::equal(name.begin(), name.end(), header.begin());
}

} // namespace

bool ReadWebpMetadata(const std::filesystem::path& filename, ExifInfo& info,
	const WorkContext& context) {
	std::ifstream input(filename, std::ios::binary);
	if (!input || !context.Continue()) return false;
	std::uint64_t fileSize = 0;
	if (!GetFileSize(input, fileSize) || fileSize < 12) return false;
	std::array<std::uint8_t, 12> riffHeader{};
	if (!ReadAt(input, fileSize, 0, riffHeader.data(), riffHeader.size(), context) ||
		riffHeader[0] != 'R' || riffHeader[1] != 'I' || riffHeader[2] != 'F' ||
		riffHeader[3] != 'F' || riffHeader[8] != 'W' || riffHeader[9] != 'E' ||
		riffHeader[10] != 'B' || riffHeader[11] != 'P') return false;

	const std::uint64_t riffEnd =
		static_cast<std::uint64_t>(ReadLittleEndian32(riffHeader.data() + 4)) + 8;
	if (riffEnd < 12 || riffEnd > fileSize) return false;
	std::uint64_t offset = 12;
	while (offset < riffEnd && context.Continue()) {
		if (riffEnd - offset < 8) return false;
		std::array<std::uint8_t, 8> header{};
		if (!ReadAt(input, fileSize, offset, header.data(), header.size(), context)) {
			return false;
		}
		const std::uint32_t chunkLength = ReadLittleEndian32(header.data() + 4);
		const std::uint64_t dataOffset = offset + header.size();
		const std::uint64_t paddedLength =
			static_cast<std::uint64_t>(chunkLength) + (chunkLength & 1u);
		if (paddedLength > riffEnd - dataOffset) return false;
		if (IsExifChunk(header)) {
			if (chunkLength == 0 || chunkLength > kMaximumEmbeddedExifBytes) return false;
			std::array<std::uint8_t, 6> exifPrefix{};
			std::size_t tiffOffset = 0;
			if (chunkLength >= exifPrefix.size() &&
				ReadAt(input, fileSize, dataOffset, exifPrefix.data(),
					exifPrefix.size(), context) && exifPrefix[0] == 'E' &&
					exifPrefix[1] == 'x' && exifPrefix[2] == 'i' &&
					exifPrefix[3] == 'f' && exifPrefix[4] == 0 && exifPrefix[5] == 0) {
				tiffOffset = exifPrefix.size();
			}
			ExifInfo parsed;
			const TiffMetadataReadAt readAt = [&input, fileSize, dataOffset,
				chunkLength, context](std::size_t sourceOffset,
				std::uint8_t* destination, std::size_t length) {
				if (sourceOffset > chunkLength || length > chunkLength - sourceOffset) {
					return false;
				}
				return ReadAt(input, fileSize, dataOffset + sourceOffset, destination,
					length, context);
			};
			if (!ReadTiffMetadataFromSource(chunkLength, readAt, tiffOffset,
				parsed, context) || !parsed.hasExif) return false;
			info = std::move(parsed);
			return true;
		}
		offset = dataOffset + paddedLength;
	}
	return false;
}

} // namespace jpegview_linux::detail
