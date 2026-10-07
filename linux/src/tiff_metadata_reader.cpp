#include "tiff_metadata_reader.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

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
	TiffReader(std::size_t bytesLength, const TiffMetadataReadAt& readAt,
		std::size_t tiffOffset, const WorkContext& context)
		: bytesLength_(bytesLength), readAt_(readAt), tiffOffset_(tiffOffset),
		  context_(context) {}

	bool Read(ExifInfo& info) {
		if (!InRange(tiffOffset_, 8)) return false;
		std::uint16_t byteOrder = 0;
		if (!Read16(tiffOffset_, byteOrder)) return false;
		if (byteOrder == 0x4949) {
			littleEndian_ = true;
		} else if (byteOrder == 0x4D4D) {
			littleEndian_ = false;
		} else {
			return false;
		}
		std::uint16_t magic = 0;
		if (!Read16(tiffOffset_ + 2, magic) || magic != 42) return false;

		std::uint32_t ifdOffset = 0;
		if (!Read32(tiffOffset_ + 4, ifdOffset)) return false;
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
		ReadShootingFields(ifd0, info);

		std::vector<TiffEntry> exifIfd;
		const TiffEntry* exifOffset = Find(ifd0, 0x8769);
		if (exifOffset != nullptr) {
			std::uint32_t offset = 0;
			if (ReadUnsigned(exifOffset, offset)) ReadDirectory(offset, exifIfd);
		}
		if (!exifIfd.empty()) {
			info.acquisitionDate = ReadString(Find(exifIfd, 0x9003));
			ReadShootingFields(exifIfd, info);
			info.userComment = ReadUserComment(Find(exifIfd, 0x9286));
			if (info.userComment == "User comments") info.userComment.clear();
		}

		const TiffEntry* gpsOffset = Find(ifd0, 0x8825);
		std::vector<TiffEntry> gpsIfd;
		std::uint32_t gpsDirectoryOffset = 0;
		if (ReadUnsigned(gpsOffset, gpsDirectoryOffset) && ReadDirectory(gpsDirectoryOffset, gpsIfd)) {
			ReadGps(gpsIfd, info);
		}
		return context_.Continue();
	}

private:
	bool InRange(std::size_t offset, std::size_t length) const {
		return offset <= bytesLength_ && length <= bytesLength_ - offset;
	}

	bool ReadBytes(std::size_t offset, std::uint8_t* destination,
		std::size_t length) const {
		if (destination == nullptr || !InRange(offset, length) || !context_.Continue()) {
			return false;
		}
		try {
			return readAt_ && readAt_(offset, destination, length) && context_.Continue();
		} catch (...) {
			return false;
		}
	}

	bool Read16(std::size_t offset, std::uint16_t& value) const {
		std::uint8_t bytes[2]{};
		if (!ReadBytes(offset, bytes, sizeof(bytes))) return false;
		value = Decode16(bytes);
		return true;
	}

	bool Read32(std::size_t offset, std::uint32_t& value) const {
		std::uint8_t bytes[4]{};
		if (!ReadBytes(offset, bytes, sizeof(bytes))) return false;
		value = Decode32(bytes);
		return true;
	}

	std::uint16_t Decode16(const std::uint8_t* bytes) const {
		if (littleEndian_) return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
		return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
	}

	std::uint32_t Decode32(const std::uint8_t* bytes) const {
		if (littleEndian_) {
			return static_cast<std::uint32_t>(bytes[0]) |
				(static_cast<std::uint32_t>(bytes[1]) << 8) |
				(static_cast<std::uint32_t>(bytes[2]) << 16) |
				(static_cast<std::uint32_t>(bytes[3]) << 24);
		}
		return (static_cast<std::uint32_t>(bytes[0]) << 24) |
			(static_cast<std::uint32_t>(bytes[1]) << 16) |
			(static_cast<std::uint32_t>(bytes[2]) << 8) |
			bytes[3];
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
		std::uint16_t count = 0;
		if (!Read16(directory, count)) return false;
		const std::size_t entryBytes = static_cast<std::size_t>(count) * 12;
		if (count != 0 && entryBytes / 12 != count) return false;
		if (!InRange(directory + 2, entryBytes)) return false;
		std::vector<std::uint8_t> directoryBytes(entryBytes);
		if (entryBytes > 0 && !ReadBytes(directory + 2, directoryBytes.data(), entryBytes)) {
			return false;
		}
		entries.clear();
		entries.reserve(count);
		for (std::uint16_t index = 0; index < count; ++index) {
			const std::uint8_t* rawEntry = directoryBytes.data() +
				static_cast<std::size_t>(index) * 12;
			TiffEntry entry;
			entry.tag = Decode16(rawEntry);
			entry.type = Decode16(rawEntry + 2);
			entry.count = Decode32(rawEntry + 4);
			entry.entryOffset = directory + 2 + static_cast<std::size_t>(index) * 12;
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

	void ReadShootingFields(const std::vector<TiffEntry>& entries, ExifInfo& info) const {
		const std::string exposureTime = ReadRationalText(Find(entries, 0x829A), false);
		if (!exposureTime.empty()) info.exposureTime = exposureTime;

		double value = 0.0;
		if (ReadDouble(Find(entries, 0x9204), value)) {
			info.hasExposureBias = true;
			info.exposureBias = value;
		}

		std::uint32_t flashValue = 0;
		if (ReadUnsigned(Find(entries, 0x9209), flashValue)) {
			info.hasFlash = true;
			info.flashFired = (flashValue & 1u) != 0;
		}
		if (ReadDouble(Find(entries, 0x920A), value)) {
			info.hasFocalLength = true;
			info.focalLength = value;
		}
		if (ReadDouble(Find(entries, 0x829D), value)) {
			info.hasFNumber = true;
			info.fNumber = value;
		}

		const TiffEntry* iso = Find(entries, 0x8827);
		std::uint32_t isoValue = 0;
		bool hasIso = ReadUnsigned(iso, isoValue);
		if (!hasIso) hasIso = ReadUnsigned(Find(entries, 0x8833), isoValue);
		if (hasIso && isoValue <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
			info.isoSpeed = static_cast<int>(isoValue);
		}
	}

	bool ValueLocation(const TiffEntry* entry, std::size_t& location, std::size_t& length) const {
		if (entry == nullptr) return false;
		const std::size_t typeSize = TypeSize(entry->type);
		if (typeSize == 0 || entry->count > std::numeric_limits<std::size_t>::max() / typeSize) return false;
		length = static_cast<std::size_t>(entry->count) * typeSize;
		if (length <= 4) {
			location = entry->entryOffset + 8;
		} else {
			std::uint32_t relativeLocation = 0;
			if (!Read32(entry->entryOffset + 8, relativeLocation)) return false;
			location = tiffOffset_ + relativeLocation;
			if (location < tiffOffset_) return false;
		}
		return InRange(location, length);
	}

	std::string ReadString(const TiffEntry* entry, std::size_t maximum = 4096) const {
		std::size_t location = 0;
		std::size_t length = 0;
		if (!ValueLocation(entry, location, length) || length == 0 || length > maximum) return {};
		std::vector<std::uint8_t> bytes(length);
		if (!ReadBytes(location, bytes.data(), length)) return {};
		std::string value(reinterpret_cast<const char*>(bytes.data()), length);
		const std::size_t nul = value.find('\0');
		if (nul != std::string::npos) value.resize(nul);
		while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.pop_back();
		return value;
	}

	std::string ReadUserComment(const TiffEntry* entry) const {
		std::size_t location = 0;
		std::size_t length = 0;
		if (!ValueLocation(entry, location, length) || length <= 8 || length > 4096) return {};
		std::vector<std::uint8_t> bytes(length);
		if (!ReadBytes(location, bytes.data(), length)) return {};
		const std::size_t contentLength = length - 8;
		if (std::memcmp(bytes.data(), "ASCII", 5) == 0) {
			std::string value(reinterpret_cast<const char*>(bytes.data() + 8), contentLength);
			const std::size_t nul = value.find('\0');
			if (nul != std::string::npos) value.resize(nul);
			return value;
		}
		// UNICODE comments are retained as a best-effort UTF-16-to-ASCII view.
		if (std::memcmp(bytes.data(), "UNICODE", 7) == 0) {
			std::string value;
			for (std::size_t offset = 8; offset + 1 < length; offset += 2) {
				const std::uint16_t character = littleEndian_ ?
					static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8)) :
					static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
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
		case 7: {
			std::uint8_t byte = 0;
			if (!ReadBytes(location, &byte, 1)) return false;
			value = byte;
			return true;
		}
		case 3: {
			std::uint16_t number = 0;
			if (!Read16(location, number)) return false;
			value = number;
			return true;
		}
		case 4:
			return Read32(location, value);
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
		std::uint32_t rawNumber = 0;
		std::uint32_t rawDivisor = 0;
		if (!Read32(location, rawNumber) || !Read32(location + 4, rawDivisor)) return false;
		const std::int64_t number = entry->type == 10 ?
			static_cast<std::int64_t>(static_cast<std::int32_t>(rawNumber)) :
			static_cast<std::int64_t>(rawNumber);
		const std::int64_t divisor = entry->type == 10 ?
			static_cast<std::int64_t>(static_cast<std::int32_t>(rawDivisor)) :
			static_cast<std::int64_t>(rawDivisor);
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
		info.gpsLatitude = latitudeDecimal;
		info.gpsLongitude = longitudeDecimal;
		info.gpsLocation = location.str();

		std::uint32_t altitudeReference = 0;
		if (ReadDouble(Find(entries, 0x0006), info.altitude)) {
			if (ReadUnsigned(Find(entries, 0x0005), altitudeReference) && altitudeReference == 1) info.altitude = -info.altitude;
			info.hasAltitude = true;
		}
	}

	std::size_t bytesLength_ = 0;
	const TiffMetadataReadAt& readAt_;
	std::size_t tiffOffset_ = 0;
	WorkContext context_;
	bool littleEndian_ = true;
};

} // namespace

bool ReadTiffMetadataFromSource(std::size_t bytesLength,
	const TiffMetadataReadAt& readAt, std::size_t tiffOffset,
	ExifInfo& info, const WorkContext& context) {
	if (!readAt || !context.Continue()) return false;
	return TiffReader(bytesLength, readAt, tiffOffset, context).Read(info);
}

bool ReadTiffMetadata(const std::uint8_t* bytes, std::size_t bytesLength,
	std::size_t tiffOffset, ExifInfo& info, const WorkContext& context) {
	if (bytes == nullptr) return false;
	const TiffMetadataReadAt readAt = [bytes, bytesLength](std::size_t offset,
		std::uint8_t* destination, std::size_t length) {
		if (offset > bytesLength || length > bytesLength - offset) return false;
		if (length != 0) std::memcpy(destination, bytes + offset, length);
		return true;
	};
	return ReadTiffMetadataFromSource(bytesLength, readAt, tiffOffset, info, context);
}

bool ReadTiffMetadataFile(const std::filesystem::path& filename,
	ExifInfo& info, const WorkContext& supplied) {
	info = {};
	const WorkContext context = ResolveWorkContext(filename,
		SourceWorkPriority::Metadata, supplied);
	if (!context.Continue()) return false;
	std::error_code error;
	const std::uintmax_t fileSize = std::filesystem::file_size(filename, error);
	if (error || fileSize == 0 ||
		fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
		fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max()) ||
		fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
		return false;
	}
	std::ifstream input(filename, std::ios::binary);
	if (!input) return false;
	const std::size_t length = static_cast<std::size_t>(fileSize);
	const TiffMetadataReadAt readAt = [&input, length, context](std::size_t offset,
		std::uint8_t* destination, std::size_t bytesToRead) {
		if (!context.Continue() || offset > length || bytesToRead > length - offset ||
			bytesToRead > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
			return false;
		}
		input.clear();
		input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		if (!input) return false;
		if (bytesToRead == 0) return true;
		input.read(reinterpret_cast<char*>(destination),
			static_cast<std::streamsize>(bytesToRead));
		return static_cast<std::size_t>(input.gcount()) == bytesToRead && context.Continue();
	};
	return ReadTiffMetadataFromSource(length, readAt, 0, info, context);
}

} // namespace jpegview_linux
