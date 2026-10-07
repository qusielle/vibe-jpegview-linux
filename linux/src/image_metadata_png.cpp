#include "image_metadata_reader_internal.h"

#include "tiff_metadata_reader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>
#include <zlib.h>

namespace jpegview_linux::detail {
namespace {

constexpr std::array<std::uint8_t, 8> kPngSignature = {
	0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
constexpr std::uint32_t kMaximumPngChunkBytes = 0x7fffffffu;
constexpr std::size_t kCrcBufferBytes = 64u * 1024u;

std::uint32_t ReadBigEndian32(const std::uint8_t* bytes) {
	return (static_cast<std::uint32_t>(bytes[0]) << 24) |
		(static_cast<std::uint32_t>(bytes[1]) << 16) |
		(static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
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

bool ValidateExifChunkCrc(std::ifstream& input, std::uint64_t fileSize,
	std::uint64_t dataOffset, std::uint32_t dataLength,
	const WorkContext& context) {
	std::array<std::uint8_t, 4> chunkType = {'e', 'X', 'I', 'f'};
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, chunkType.data(), static_cast<uInt>(chunkType.size()));
	std::array<std::uint8_t, kCrcBufferBytes> buffer{};
	std::uint64_t consumed = 0;
	while (consumed < dataLength) {
		if (!context.Continue()) return false;
		const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
			buffer.size(), static_cast<std::uint64_t>(dataLength) - consumed));
		if (!ReadAt(input, fileSize, dataOffset + consumed, buffer.data(), count,
			context)) return false;
		crc = crc32(crc, buffer.data(), static_cast<uInt>(count));
		consumed += count;
	}
	std::array<std::uint8_t, 4> storedCrc{};
	if (!ReadAt(input, fileSize, dataOffset + dataLength, storedCrc.data(),
		storedCrc.size(), context)) return false;
	return static_cast<std::uint32_t>(crc) == ReadBigEndian32(storedCrc.data());
}

bool IsChunk(const std::array<std::uint8_t, 8>& header, std::string_view name) {
	return name.size() == 4 && std::equal(name.begin(), name.end(), header.begin() + 4,
		[](char left, std::uint8_t right) {
			return static_cast<unsigned char>(left) == right;
		});
}

} // namespace

bool ReadPngMetadata(const std::filesystem::path& filename, ExifInfo& info,
	const WorkContext& context) {
	std::ifstream input(filename, std::ios::binary);
	if (!input || !context.Continue()) return false;
	std::uint64_t fileSize = 0;
	if (!GetFileSize(input, fileSize) || fileSize < kPngSignature.size()) return false;
	std::array<std::uint8_t, 8> signature{};
	if (!ReadAt(input, fileSize, 0, signature.data(), signature.size(), context) ||
		signature != kPngSignature) return false;

	std::uint64_t offset = kPngSignature.size();
	bool sawHeader = false;
	while (offset <= fileSize && fileSize - offset >= 12 && context.Continue()) {
		std::array<std::uint8_t, 8> header{};
		if (!ReadAt(input, fileSize, offset, header.data(), header.size(), context)) {
			return false;
		}
		const std::uint32_t chunkLength = ReadBigEndian32(header.data());
		if (chunkLength > kMaximumPngChunkBytes) return false;
		const std::uint64_t dataOffset = offset + header.size();
		if (chunkLength > fileSize - dataOffset - 4) return false;
		const std::uint64_t next = dataOffset + chunkLength + 4;

		if (!sawHeader) {
			if (!IsChunk(header, "IHDR") || chunkLength != 13) return false;
			sawHeader = true;
		} else if (IsChunk(header, "IHDR")) {
			return false;
		}

		if (IsChunk(header, "eXIf")) {
			if (chunkLength == 0 || chunkLength > kMaximumEmbeddedExifBytes ||
				!ValidateExifChunkCrc(input, fileSize, dataOffset, chunkLength, context)) {
				return false;
			}
			ExifInfo parsed;
			const TiffMetadataReadAt readAt = [&input, fileSize, dataOffset, chunkLength,
				context](std::size_t sourceOffset, std::uint8_t* destination,
				std::size_t length) {
				if (sourceOffset > chunkLength || length > chunkLength - sourceOffset) {
					return false;
				}
				return ReadAt(input, fileSize, dataOffset + sourceOffset, destination,
					length, context);
			};
			if (!ReadTiffMetadataFromSource(chunkLength, readAt, 0, parsed, context) ||
				!parsed.hasExif) return false;
			info = std::move(parsed);
			return true;
		}
		if (IsChunk(header, "IEND")) return false;
		offset = next;
	}
	return false;
}

} // namespace jpegview_linux::detail
