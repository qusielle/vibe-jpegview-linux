#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void Write16(std::vector<std::uint8_t>& bytes, std::size_t offset,
	std::uint16_t value) {
	bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
	bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Write32(std::vector<std::uint8_t>& bytes, std::size_t offset,
	std::uint32_t value) {
	for (int byte = 0; byte < 4; ++byte) {
		bytes[offset + static_cast<std::size_t>(byte)] =
			static_cast<std::uint8_t>(value >> (byte * 8));
	}
}

void WriteGpsEntry(std::vector<std::uint8_t>& tiff, std::size_t offset,
	std::uint16_t tag, std::uint16_t type, std::uint32_t count,
	std::uint32_t value) {
	Write16(tiff, offset, tag);
	Write16(tiff, offset + 2, type);
	Write32(tiff, offset + 4, count);
	Write32(tiff, offset + 8, value);
}

std::vector<std::uint8_t> MakeExifPayload() {
	// TIFF header, IFD0, four GPS entries, then latitude and longitude rationals.
	std::vector<std::uint8_t> tiff(128, 0);
	tiff[0] = 'I';
	tiff[1] = 'I';
	Write16(tiff, 2, 42);
	Write32(tiff, 4, 8);

	Write16(tiff, 8, 1);
	WriteGpsEntry(tiff, 10, 0x8825, 4, 1, 26);
	Write32(tiff, 22, 0);

	Write16(tiff, 26, 4);
	WriteGpsEntry(tiff, 28, 0x0001, 2, 2, 'S');
	WriteGpsEntry(tiff, 40, 0x0002, 5, 3, 80);
	WriteGpsEntry(tiff, 52, 0x0003, 2, 2, 'W');
	WriteGpsEntry(tiff, 64, 0x0004, 5, 3, 104);
	Write32(tiff, 76, 0);

	const auto writeRational = [&tiff](std::size_t offset, std::uint32_t value) {
		Write32(tiff, offset, value);
		Write32(tiff, offset + 4, 1);
	};
	writeRational(80, 12);
	writeRational(88, 34);
	writeRational(96, 56);
	writeRational(104, 98);
	writeRational(112, 7);
	writeRational(120, 6);

	std::vector<std::uint8_t> payload = {'E', 'x', 'i', 'f', 0, 0};
	payload.insert(payload.end(), tiff.begin(), tiff.end());
	return payload;
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>& bytes) {
	std::ifstream input(path, std::ios::binary);
	if (!input) return false;
	bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	return input.eof() || input.good();
}

bool WriteFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) return false;
	output.write(reinterpret_cast<const char*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	return output.good();
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 3) {
		std::cerr << "usage: write-gps-exif-fixture input.jpg output.jpg\n";
		return 2;
	}
	std::vector<std::uint8_t> jpeg;
	if (!ReadFile(argv[1], jpeg) || jpeg.size() < 2 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
		std::cerr << "input is not a readable JPEG: " << argv[1] << '\n';
		return 1;
	}
	const std::vector<std::uint8_t> exif = MakeExifPayload();
	const std::size_t segmentLength = exif.size() + 2;
	if (segmentLength > 65535) {
		std::cerr << "EXIF fixture exceeds the JPEG APP1 segment limit\n";
		return 1;
	}
	std::vector<std::uint8_t> result;
	result.reserve(jpeg.size() + exif.size() + 4);
	result.insert(result.end(), jpeg.begin(), jpeg.begin() + 2);
	result.push_back(0xff);
	result.push_back(0xe1);
	result.push_back(static_cast<std::uint8_t>(segmentLength >> 8));
	result.push_back(static_cast<std::uint8_t>(segmentLength & 0xff));
	result.insert(result.end(), exif.begin(), exif.end());
	result.insert(result.end(), jpeg.begin() + 2, jpeg.end());
	if (!WriteFile(argv[2], result)) {
		std::cerr << "could not write JPEG fixture: " << argv[2] << '\n';
		return 1;
	}
	return 0;
}
