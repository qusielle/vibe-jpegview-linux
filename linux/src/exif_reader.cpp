#include "exif_reader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

struct TiffEntry {
	std::uint16_t tag = 0;
	std::uint16_t type = 0;
	std::uint32_t count = 0;
	std::size_t entryOffset = 0;
};

class TiffReader {
public:
	TiffReader(const std::vector<std::uint8_t>& bytes, std::size_t tiffOffset)
		: bytes_(bytes), tiffOffset_(tiffOffset) {}

	bool Read(ExifInfo& info) {
		if (!InRange(tiffOffset_, 8)) return false;
		const std::uint16_t byteOrder = Read16(tiffOffset_);
		if (byteOrder == 0x4949) {
			littleEndian_ = true;
		} else if (byteOrder == 0x4D4D) {
			littleEndian_ = false;
		} else {
			return false;
		}
		if (Read16(tiffOffset_ + 2) != 42) return false;

		const std::uint32_t ifdOffset = Read32(tiffOffset_ + 4);
		std::vector<TiffEntry> ifd0;
		if (!ReadDirectory(ifdOffset, ifd0)) return false;
		info.hasExif = true;

		const TiffEntry* model = Find(ifd0, 0x0110);
		const TiffEntry* make = Find(ifd0, 0x010F);
		info.cameraModel = ReadString(model);
		const std::string makeText = ReadString(make);
		if (!makeText.empty() && !info.cameraModel.empty()) {
			std::string makeName = makeText.substr(0, makeText.find(' '));
			if (info.cameraModel.find(makeName) == std::string::npos) {
				info.cameraModel = makeName + " " + info.cameraModel;
			}
		}
		info.imageDescription = ReadString(Find(ifd0, 0x010E));
		info.software = ReadString(Find(ifd0, 0x0131));
		info.dateTime = ReadString(Find(ifd0, 0x0132));

		std::vector<TiffEntry> exifIfd;
		const TiffEntry* exifOffset = Find(ifd0, 0x8769);
		if (exifOffset != nullptr) {
			std::uint32_t offset = 0;
			if (ReadUnsigned(exifOffset, offset)) ReadDirectory(offset, exifIfd);
		}
		if (!exifIfd.empty()) {
			info.acquisitionDate = ReadString(Find(exifIfd, 0x9003));
			const TiffEntry* exposure = Find(exifIfd, 0x829A);
			info.exposureTime = ReadRationalText(exposure, false);
			const TiffEntry* exposureBias = Find(exifIfd, 0x9204);
			info.hasExposureBias = ReadDouble(exposureBias, info.exposureBias);
			const TiffEntry* flash = Find(exifIfd, 0x9209);
			std::uint32_t flashValue = 0;
			info.hasFlash = ReadUnsigned(flash, flashValue);
			info.flashFired = (flashValue & 1u) != 0;
			info.hasFocalLength = ReadDouble(Find(exifIfd, 0x920A), info.focalLength);
			info.hasFNumber = ReadDouble(Find(exifIfd, 0x829D), info.fNumber);
			const TiffEntry* iso = Find(exifIfd, 0x8827);
			if (iso == nullptr) iso = Find(exifIfd, 0x8833);
			std::uint32_t isoValue = 0;
			if (ReadUnsigned(iso, isoValue) && isoValue <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
				info.isoSpeed = static_cast<int>(isoValue);
			}
			info.userComment = ReadUserComment(Find(exifIfd, 0x9286));
			if (info.userComment == "User comments") info.userComment.clear();
		}

		const TiffEntry* gpsOffset = Find(ifd0, 0x8825);
		std::vector<TiffEntry> gpsIfd;
		std::uint32_t gpsDirectoryOffset = 0;
		if (ReadUnsigned(gpsOffset, gpsDirectoryOffset) && ReadDirectory(gpsDirectoryOffset, gpsIfd)) {
			ReadGps(gpsIfd, info);
		}
		return true;
	}

private:
	bool InRange(std::size_t offset, std::size_t length) const {
		return offset <= bytes_.size() && length <= bytes_.size() - offset;
	}

	std::uint16_t Read16(std::size_t offset) const {
		if (!InRange(offset, 2)) return 0;
		if (littleEndian_) return static_cast<std::uint16_t>(bytes_[offset] | (bytes_[offset + 1] << 8));
		return static_cast<std::uint16_t>((bytes_[offset] << 8) | bytes_[offset + 1]);
	}

	std::uint32_t Read32(std::size_t offset) const {
		if (!InRange(offset, 4)) return 0;
		if (littleEndian_) {
			return static_cast<std::uint32_t>(bytes_[offset]) |
				(static_cast<std::uint32_t>(bytes_[offset + 1]) << 8) |
				(static_cast<std::uint32_t>(bytes_[offset + 2]) << 16) |
				(static_cast<std::uint32_t>(bytes_[offset + 3]) << 24);
		}
		return (static_cast<std::uint32_t>(bytes_[offset]) << 24) |
			(static_cast<std::uint32_t>(bytes_[offset + 1]) << 16) |
			(static_cast<std::uint32_t>(bytes_[offset + 2]) << 8) |
			bytes_[offset + 3];
	}

	static std::size_t TypeSize(std::uint16_t type) {
		switch (type) {
		case 1: case 2: case 7: return 1;
		case 3: return 2;
		case 4: case 9: return 4;
		case 5: case 10: return 8;
		default: return 0;
		}
	}

	bool ReadDirectory(std::uint32_t relativeOffset, std::vector<TiffEntry>& entries) const {
		const std::size_t directory = tiffOffset_ + relativeOffset;
		if (directory < tiffOffset_ || !InRange(directory, 2)) return false;
		const std::uint16_t count = Read16(directory);
		const std::size_t entryBytes = static_cast<std::size_t>(count) * 12;
		if (count != 0 && entryBytes / 12 != count) return false;
		if (!InRange(directory + 2, entryBytes)) return false;
		entries.clear();
		entries.reserve(count);
		for (std::uint16_t index = 0; index < count; ++index) {
			const std::size_t entryOffset = directory + 2 + static_cast<std::size_t>(index) * 12;
			TiffEntry entry;
			entry.tag = Read16(entryOffset);
			entry.type = Read16(entryOffset + 2);
			entry.count = Read32(entryOffset + 4);
			entry.entryOffset = entryOffset;
			entries.push_back(entry);
		}
		return true;
	}

	static const TiffEntry* Find(const std::vector<TiffEntry>& entries, std::uint16_t tag) {
		for (const TiffEntry& entry : entries) {
			if (entry.tag == tag) return &entry;
		}
		return nullptr;
	}

	bool ValueLocation(const TiffEntry* entry, std::size_t& location, std::size_t& length) const {
		if (entry == nullptr) return false;
		const std::size_t typeSize = TypeSize(entry->type);
		if (typeSize == 0 || entry->count > std::numeric_limits<std::size_t>::max() / typeSize) return false;
		length = static_cast<std::size_t>(entry->count) * typeSize;
		if (length <= 4) {
			location = entry->entryOffset + 8;
		} else {
			const std::uint32_t relativeLocation = Read32(entry->entryOffset + 8);
			location = tiffOffset_ + relativeLocation;
			if (location < tiffOffset_) return false;
		}
		return InRange(location, length);
	}

	std::string ReadString(const TiffEntry* entry, std::size_t maximum = 4096) const {
		std::size_t location = 0;
		std::size_t length = 0;
		if (!ValueLocation(entry, location, length) || length == 0 || length > maximum) return {};
		std::string value(reinterpret_cast<const char*>(bytes_.data() + location), length);
		const std::size_t nul = value.find('\0');
		if (nul != std::string::npos) value.resize(nul);
		while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.pop_back();
		return value;
	}

	std::string ReadUserComment(const TiffEntry* entry) const {
		std::size_t location = 0;
		std::size_t length = 0;
		if (!ValueLocation(entry, location, length) || length <= 8 || length > 4096) return {};
		const std::size_t contentOffset = location + 8;
		const std::size_t contentLength = length - 8;
		if (std::memcmp(bytes_.data() + location, "ASCII", 5) == 0) {
			std::string value(reinterpret_cast<const char*>(bytes_.data() + contentOffset), contentLength);
			const std::size_t nul = value.find('\0');
			if (nul != std::string::npos) value.resize(nul);
			return value;
		}
		// UNICODE comments are retained as a best-effort UTF-16-to-ASCII view.
		if (std::memcmp(bytes_.data() + location, "UNICODE", 7) == 0) {
			std::string value;
			for (std::size_t offset = contentOffset; offset + 1 < location + length; offset += 2) {
				const std::uint16_t character = littleEndian_ ?
					static_cast<std::uint16_t>(bytes_[offset] | (bytes_[offset + 1] << 8)) :
					static_cast<std::uint16_t>((bytes_[offset] << 8) | bytes_[offset + 1]);
				if (character == 0) break;
				value.push_back(character < 128 ? static_cast<char>(character) : '?');
			}
			return value;
		}
		return {};
	}

	bool ReadUnsigned(const TiffEntry* entry, std::uint32_t& value) const {
		std::size_t location = 0;
		std::size_t length = 0;
		if (!ValueLocation(entry, location, length) || entry->count == 0) return false;
		switch (entry->type) {
		case 1:
		case 7:
			value = bytes_[location];
			return true;
		case 3:
			value = Read16(location);
			return true;
		case 4:
			value = Read32(location);
			return true;
		default:
			return false;
		}
	}

	bool ReadRational(const TiffEntry* entry, std::size_t index, double& value,
		std::int64_t* numerator = nullptr, std::int64_t* denominator = nullptr) const {
		if (entry == nullptr || index >= entry->count || (entry->type != 5 && entry->type != 10)) return false;
		std::size_t location = 0;
		std::size_t length = 0;
		if (!ValueLocation(entry, location, length) || index > (length - 8) / 8) return false;
		location += index * 8;
		const std::int64_t number = entry->type == 10 ?
			static_cast<std::int64_t>(static_cast<std::int32_t>(Read32(location))) :
			static_cast<std::int64_t>(Read32(location));
		const std::int64_t divisor = entry->type == 10 ?
			static_cast<std::int64_t>(static_cast<std::int32_t>(Read32(location + 4))) :
			static_cast<std::int64_t>(Read32(location + 4));
		if (divisor == 0) return false;
		value = static_cast<double>(number) / static_cast<double>(divisor);
		if (numerator != nullptr) *numerator = number;
		if (denominator != nullptr) *denominator = divisor;
		return std::isfinite(value);
	}

	bool ReadDouble(const TiffEntry* entry, double& value) const {
		return ReadRational(entry, 0, value);
	}

	std::string ReadRationalText(const TiffEntry* entry, bool signedValue) const {
		(void)signedValue;
		double value = 0.0;
		std::int64_t numerator = 0;
		std::int64_t denominator = 0;
		if (!ReadRational(entry, 0, value, &numerator, &denominator)) return {};
		if (denominator == 1) return std::to_string(numerator);
		if (std::abs(value) < 1.0 && numerator != 0) {
			std::ostringstream stream;
			stream << "1/" << std::llabs(denominator / numerator);
			return stream.str();
		}
		std::ostringstream stream;
		stream << std::fixed << std::setprecision(2) << value;
		return stream.str();
	}

	void ReadGps(const std::vector<TiffEntry>& entries, ExifInfo& info) const {
		const std::string latitudeReference = ReadString(Find(entries, 0x0001), 2);
		const std::string longitudeReference = ReadString(Find(entries, 0x0003), 2);
		const TiffEntry* latitude = Find(entries, 0x0002);
		const TiffEntry* longitude = Find(entries, 0x0004);
		double latitudeDegrees = 0.0;
		double latitudeMinutes = 0.0;
		double latitudeSeconds = 0.0;
		double longitudeDegrees = 0.0;
		double longitudeMinutes = 0.0;
		double longitudeSeconds = 0.0;
		if (!ReadRational(latitude, 0, latitudeDegrees) || !ReadRational(latitude, 1, latitudeMinutes) ||
			!ReadRational(latitude, 2, latitudeSeconds)) return;
		if (!ReadRational(longitude, 0, longitudeDegrees) || !ReadRational(longitude, 1, longitudeMinutes) ||
			!ReadRational(longitude, 2, longitudeSeconds)) return;

		// The original Windows panel displays DMS; decimal coordinates are more
		// compact in the dependency-free Linux text renderer.
		double latitudeDecimal = latitudeDegrees + latitudeMinutes / 60.0 + latitudeSeconds / 3600.0;
		double longitudeDecimal = longitudeDegrees + longitudeMinutes / 60.0 + longitudeSeconds / 3600.0;
		if (latitudeReference == "S" || latitudeReference == "s") latitudeDecimal = -latitudeDecimal;
		if (longitudeReference == "W" || longitudeReference == "w") longitudeDecimal = -longitudeDecimal;
		std::ostringstream location;
		location << std::fixed << std::setprecision(5) << latitudeDecimal << ", " << longitudeDecimal;
		info.hasGps = true;
		info.gpsLocation = location.str();

		std::uint32_t altitudeReference = 0;
		if (ReadDouble(Find(entries, 0x0006), info.altitude)) {
			if (ReadUnsigned(Find(entries, 0x0005), altitudeReference) && altitudeReference == 1) info.altitude = -info.altitude;
			info.hasAltitude = true;
		}
	}

	const std::vector<std::uint8_t>& bytes_;
	std::size_t tiffOffset_ = 0;
	bool littleEndian_ = true;
};

std::string TrimAscii(const std::uint8_t* data, std::size_t length) {
	std::string value(reinterpret_cast<const char*>(data), length);
	const std::size_t nul = value.find('\0');
	if (nul != std::string::npos) value.resize(nul);
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.pop_back();
	return value;
}

} // namespace

bool ReadJpegMetadata(const fs::path& filename, ExifInfo& info, std::string& jpegComment) {
	info = {};
	jpegComment.clear();
	std::ifstream input(filename, std::ios::binary);
	if (!input) return false;
	const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
	if (bytes.size() < 2 || bytes[0] != 0xFF || bytes[1] != 0xD8) return false;

	bool foundMetadata = false;
	std::size_t offset = 2;
	while (offset + 1 < bytes.size()) {
		if (bytes[offset] != 0xFF) break;
		while (offset < bytes.size() && bytes[offset] == 0xFF) ++offset;
		if (offset >= bytes.size()) break;
		const std::uint8_t marker = bytes[offset++];
		if (marker == 0xD9 || marker == 0xDA) break;
		if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) continue;
		if (offset + 2 > bytes.size()) break;
		const std::size_t segmentLength = (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset + 1];
		if (segmentLength < 2 || segmentLength - 2 > bytes.size() - (offset + 2)) break;
		const std::size_t payload = offset + 2;
		const std::size_t payloadLength = segmentLength - 2;
		if (marker == 0xE1 && payloadLength >= 6 && std::memcmp(bytes.data() + payload, "Exif\0\0", 6) == 0) {
			TiffReader reader(bytes, payload + 6);
			foundMetadata = reader.Read(info) || foundMetadata;
		} else if (marker == 0xFE && jpegComment.empty() && payloadLength > 0) {
			jpegComment = TrimAscii(bytes.data() + payload, payloadLength);
		}
		offset += segmentLength;
	}
	return foundMetadata || !jpegComment.empty();
}

} // namespace jpegview_linux
