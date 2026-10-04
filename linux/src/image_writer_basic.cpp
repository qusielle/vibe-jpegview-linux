#include "image_writer_internal.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>

namespace jpegview_linux::detail {

void WriteU16(std::ostream& output, std::uint16_t value) {
	const char bytes[] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF)};
	output.write(bytes, sizeof(bytes));
}

void WriteU32(std::ostream& output, std::uint32_t value) {
	const char bytes[] = {
		static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF),
		static_cast<char>((value >> 16) & 0xFF), static_cast<char>((value >> 24) & 0xFF)};
	output.write(bytes, sizeof(bytes));
}

bool WriteBmp(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	const std::uint64_t pixelBytes = static_cast<std::uint64_t>(width) * height * 4;
	if (pixelBytes > std::numeric_limits<std::uint32_t>::max() - 54) {
		errorMessage = "image is too large for BMP";
		return false;
	}
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	WriteU16(output, 0x4D42);
	WriteU32(output, static_cast<std::uint32_t>(54 + pixelBytes));
	WriteU16(output, 0);
	WriteU16(output, 0);
	WriteU32(output, 54);
	WriteU32(output, 40);
	WriteU32(output, static_cast<std::uint32_t>(width));
	WriteU32(output, static_cast<std::uint32_t>(height));
	WriteU16(output, 1);
	WriteU16(output, 32);
	WriteU32(output, 0);
	WriteU32(output, static_cast<std::uint32_t>(pixelBytes));
	WriteU32(output, 2835);
	WriteU32(output, 2835);
	WriteU32(output, 0);
	WriteU32(output, 0);
	for (int y = height - 1; y >= 0; --y) {
		output.write(reinterpret_cast<const char*>(bgra + static_cast<std::size_t>(y) * width * 4),
			static_cast<std::streamsize>(width) * 4);
	}
	if (!output) errorMessage = "error writing output file";
	return static_cast<bool>(output);
}

bool WriteTga(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	if (width > std::numeric_limits<std::uint16_t>::max() || height > std::numeric_limits<std::uint16_t>::max()) {
		errorMessage = "image is too large for TGA";
		return false;
	}
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	const unsigned char header[18] = {
		0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		static_cast<unsigned char>(width & 0xFF), static_cast<unsigned char>((width >> 8) & 0xFF),
		static_cast<unsigned char>(height & 0xFF), static_cast<unsigned char>((height >> 8) & 0xFF),
		32, 0x28};
	output.write(reinterpret_cast<const char*>(header), sizeof(header));
	output.write(reinterpret_cast<const char*>(bgra), static_cast<std::streamsize>(width) * height * 4);
	if (!output) errorMessage = "error writing output file";
	return static_cast<bool>(output);
}
bool WritePnm(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, const std::string& extension, std::string& errorMessage) {
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	if (extension == ".pbm") {
		output << "P4\n" << width << ' ' << height << "\n";
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; x += 8) {
				std::uint8_t packed = 0;
				for (int bit = 0; bit < 8 && x + bit < width; ++bit) {
					const std::uint8_t* pixel = row + static_cast<std::size_t>(x + bit) * 4;
					const int luminance = (static_cast<int>(pixel[2]) * 299 +
						static_cast<int>(pixel[1]) * 587 + static_cast<int>(pixel[0]) * 114) / 1000;
					if (luminance < 128) packed |= static_cast<std::uint8_t>(0x80u >> bit);
				}
				output.put(static_cast<char>(packed));
			}
		}
	} else if (extension == ".pgm") {
		output << "P5\n" << width << ' ' << height << "\n255\n";
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) {
				const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
				const std::uint8_t luminance = static_cast<std::uint8_t>(
					(static_cast<int>(pixel[2]) * 299 + static_cast<int>(pixel[1]) * 587 +
						static_cast<int>(pixel[0]) * 114) / 1000);
				output.put(static_cast<char>(luminance));
			}
		}
	} else if (extension == ".pam") {
		output << "P7\nWIDTH " << width << "\nHEIGHT " << height
			   << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) {
				const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
				const char rgba[] = {static_cast<char>(pixel[2]), static_cast<char>(pixel[1]),
					static_cast<char>(pixel[0]), static_cast<char>(pixel[3])};
				output.write(rgba, sizeof(rgba));
			}
		}
	} else {
		output << "P6\n" << width << ' ' << height << "\n255\n";
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) {
				const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
				const char rgb[] = {static_cast<char>(pixel[2]), static_cast<char>(pixel[1]),
					static_cast<char>(pixel[0])};
				output.write(rgb, sizeof(rgb));
			}
		}
	}
	if (!output) errorMessage = "error writing output file";
	return static_cast<bool>(output);
}

bool WriteQoi(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	const auto write32 = [&output](std::uint32_t value) {
		const char bytes[] = {
			static_cast<char>((value >> 24) & 0xff), static_cast<char>((value >> 16) & 0xff),
			static_cast<char>((value >> 8) & 0xff), static_cast<char>(value & 0xff)};
		output.write(bytes, sizeof(bytes));
	};
	output.write("qoif", 4);
	write32(static_cast<std::uint32_t>(width));
	write32(static_cast<std::uint32_t>(height));
	output.put(4); // Preserve alpha.
	output.put(0); // sRGB with linear alpha.

	struct Pixel { std::uint8_t r, g, b, a; };
	std::array<Pixel, 64> index{};
	Pixel previous{0, 0, 0, 255};
	int run = 0;
	const auto hash = [](Pixel pixel) {
		return static_cast<std::size_t>((pixel.r * 3 + pixel.g * 5 + pixel.b * 7 + pixel.a * 11) % 64);
	};
	const auto flushRun = [&output, &run]() {
		if (run != 0) {
			output.put(static_cast<char>(0xc0 | (run - 1)));
			run = 0;
		}
	};
	for (int y = 0; y < height; ++y) {
		const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
		for (int x = 0; x < width; ++x) {
			const std::uint8_t* source = row + static_cast<std::size_t>(x) * 4;
			const Pixel current{source[2], source[1], source[0], source[3]};
			if (current.r == previous.r && current.g == previous.g && current.b == previous.b && current.a == previous.a) {
				++run;
				if (run == 62) flushRun();
				continue;
			}
			flushRun();
			const std::size_t slot = hash(current);
			const Pixel indexed = index[slot];
			if (indexed.r == current.r && indexed.g == current.g && indexed.b == current.b && indexed.a == current.a) {
				output.put(static_cast<char>(slot));
			} else {
				index[slot] = current;
				const int dr = static_cast<int>(current.r) - previous.r;
				const int dg = static_cast<int>(current.g) - previous.g;
				const int db = static_cast<int>(current.b) - previous.b;
				if (current.a == previous.a && dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
					output.put(static_cast<char>(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
				} else if (current.a == previous.a && dg >= -32 && dg <= 31 &&
					dr - dg >= -8 && dr - dg <= 7 && db - dg >= -8 && db - dg <= 7) {
					output.put(static_cast<char>(0x80 | (dg + 32)));
					output.put(static_cast<char>(((dr - dg + 8) << 4) | (db - dg + 8)));
				} else if (current.a == previous.a) {
					output.put(static_cast<char>(0xfe));
					output.put(static_cast<char>(current.r));
					output.put(static_cast<char>(current.g));
					output.put(static_cast<char>(current.b));
				} else {
					output.put(static_cast<char>(0xff));
					output.put(static_cast<char>(current.r));
					output.put(static_cast<char>(current.g));
					output.put(static_cast<char>(current.b));
					output.put(static_cast<char>(current.a));
				}
			}
			previous = current;
		}
	}
	flushRun();
	const char endMarker[] = {0, 0, 0, 0, 0, 0, 0, 1};
	output.write(endMarker, sizeof(endMarker));
	if (!output) errorMessage = "error writing QOI file";
	return static_cast<bool>(output);
}

void WriteBE16(std::ostream& output, std::uint16_t value) {
	const char bytes[] = {static_cast<char>((value >> 8) & 0xFF), static_cast<char>(value & 0xFF)};
	output.write(bytes, sizeof(bytes));
}

void WriteBE32(std::ostream& output, std::uint32_t value) {
	const char bytes[] = {
		static_cast<char>((value >> 24) & 0xFF), static_cast<char>((value >> 16) & 0xFF),
		static_cast<char>((value >> 8) & 0xFF), static_cast<char>(value & 0xFF)};
	output.write(bytes, sizeof(bytes));
}

bool WritePsd(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, std::string& errorMessage) {
	if (static_cast<std::uint64_t>(width) > std::numeric_limits<std::uint32_t>::max() ||
		static_cast<std::uint64_t>(height) > std::numeric_limits<std::uint32_t>::max()) {
		errorMessage = "image is too large for PSD";
		return false;
	}
	std::ofstream output(filename, std::ios::binary);
	if (!output) {
		errorMessage = "cannot open output file";
		return false;
	}
	output.write("8BPS", 4);
	WriteBE16(output, 1);
	const char reserved[6] = {};
	output.write(reserved, sizeof(reserved));
	WriteBE16(output, 3); // RGB channels; alpha is omitted by this simple PSD writer.
	WriteBE32(output, static_cast<std::uint32_t>(height));
	WriteBE32(output, static_cast<std::uint32_t>(width));
	WriteBE16(output, 8);
	WriteBE16(output, 3); // RGB color mode.
	WriteBE32(output, 0); // color mode data
	WriteBE32(output, 0); // image resources
	WriteBE32(output, 0); // layer and mask information
	WriteBE16(output, 0); // raw image data
	for (int channel = 2; channel >= 0; --channel) {
		for (int y = 0; y < height; ++y) {
			const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * width * 4;
			for (int x = 0; x < width; ++x) output.put(static_cast<char>(row[x * 4 + channel]));
		}
	}
	if (!output) errorMessage = "error writing output file";
	return static_cast<bool>(output);
}

} // namespace jpegview_linux::detail
