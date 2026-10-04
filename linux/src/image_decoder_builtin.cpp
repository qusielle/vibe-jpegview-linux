#include "image_decoder_internal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace jpegview_linux::decoder_detail {

bool DecodeQoi(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> data;
	if (!ReadFile(filename, data, errorMessage)) return false;
	if (data.size() < 14 || std::memcmp(data.data(), "qoif", 4) != 0) {
		errorMessage = "invalid QOI header";
		return false;
	}
	const std::uint32_t widthValue = ReadBE32(data.data() + 4);
	const std::uint32_t heightValue = ReadBE32(data.data() + 8);
	if (widthValue > std::numeric_limits<int>::max() || heightValue > std::numeric_limits<int>::max() ||
		!ValidDimensions(static_cast<int>(widthValue), static_cast<int>(heightValue), errorMessage) ||
		(data[12] != 3 && data[12] != 4)) {
		errorMessage = "invalid QOI image dimensions or channels";
		return false;
	}
	const std::size_t pixelCount = static_cast<std::size_t>(widthValue) * heightValue;
	std::vector<std::uint8_t> rgba(pixelCount * 4);
	std::array<std::array<std::uint8_t, 4>, 64> index{};
	std::array<std::uint8_t, 4> pixel = {0, 0, 0, 255};
	std::size_t position = 14;
	std::size_t outputPixel = 0;
	std::size_t run = 0;
	const auto hash = [](const std::array<std::uint8_t, 4>& value) {
		return static_cast<std::size_t>((value[0] * 3 + value[1] * 5 + value[2] * 7 + value[3] * 11) % 64);
	};
	while (outputPixel < pixelCount) {
		if (run > 0) {
			--run;
		} else {
			if (position >= data.size()) {
				errorMessage = "truncated QOI data";
				return false;
			}
			const std::uint8_t tag = data[position++];
			if (tag == 0xfe) {
				if (position + 3 > data.size()) { errorMessage = "truncated QOI RGB data"; return false; }
				pixel[0] = data[position++]; pixel[1] = data[position++]; pixel[2] = data[position++];
			} else if (tag == 0xff) {
				if (position + 4 > data.size()) { errorMessage = "truncated QOI RGBA data"; return false; }
				for (std::uint8_t& channel : pixel) channel = data[position++];
			} else if ((tag & 0xc0) == 0x00) {
				pixel = index[tag & 0x3f];
			} else if ((tag & 0xc0) == 0x40) {
				pixel[0] = static_cast<std::uint8_t>(pixel[0] + ((tag >> 4) & 3) - 2);
				pixel[1] = static_cast<std::uint8_t>(pixel[1] + (tag & 15) / 4 - 2);
				pixel[2] = static_cast<std::uint8_t>(pixel[2] + (tag & 3) - 2);
			} else if ((tag & 0xc0) == 0x80) {
				if (position >= data.size()) { errorMessage = "truncated QOI luma data"; return false; }
				const std::uint8_t second = data[position++];
				const int green = (tag & 0x3f) - 32;
				const int red = green + ((second >> 4) & 15) - 8;
				const int blue = green + (second & 15) - 8;
				pixel[0] = static_cast<std::uint8_t>(pixel[0] + red);
				pixel[1] = static_cast<std::uint8_t>(pixel[1] + green);
				pixel[2] = static_cast<std::uint8_t>(pixel[2] + blue);
			} else {
				run = (tag & 0x3f);
			}
			index[hash(pixel)] = pixel;
		}
		std::copy(pixel.begin(), pixel.end(), rgba.begin() + static_cast<std::ptrdiff_t>(outputPixel * 4));
		++outputPixel;
	}
	if (position + 8 > data.size()) {
		errorMessage = "QOI end marker is missing";
		return false;
	}
	return AppendRGBA(image, static_cast<int>(widthValue), static_cast<int>(heightValue), std::move(rgba), 0, errorMessage);
}

bool DecodePackBitsRow(const std::vector<std::uint8_t>& data, std::size_t& position,
	std::size_t rowEnd, std::uint8_t* output, std::size_t outputSize, bool allowShortRow,
	std::string& errorMessage) {
	std::size_t written = 0;
	while (written < outputSize) {
		if (position >= rowEnd) {
			if (allowShortRow) {
				std::fill(output + written, output + outputSize, 0);
				return true;
			}
			errorMessage = "truncated PSD PackBits data";
			return false;
		}
		const std::int8_t count = static_cast<std::int8_t>(data[position++]);
		if (count >= 0) {
			const std::size_t literalCount = static_cast<std::size_t>(count) + 1;
			if (literalCount > outputSize - written || literalCount > rowEnd - position) {
				errorMessage = "invalid PSD PackBits literal";
				return false;
			}
			std::copy_n(data.data() + position, literalCount, output + written);
			position += literalCount;
			written += literalCount;
		} else if (count != -128) {
			if (position >= rowEnd || static_cast<std::size_t>(1 - count) > outputSize - written) {
				errorMessage = "invalid PSD PackBits run";
				return false;
			}
			std::fill_n(output + written, static_cast<std::size_t>(1 - count), data[position++]);
			written += static_cast<std::size_t>(1 - count);
		}
	}
	return true;
}

bool DecodePsd(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> data;
	if (!ReadFile(filename, data, errorMessage)) return false;
	if (data.size() < 26 || std::memcmp(data.data(), "8BPS", 4) != 0 || ReadBE16(data.data() + 4) != 1) {
		errorMessage = "invalid PSD header";
		return false;
	}
	const std::uint16_t channels = ReadBE16(data.data() + 12);
	const std::uint32_t heightValue = ReadBE32(data.data() + 14);
	const std::uint32_t widthValue = ReadBE32(data.data() + 18);
	const std::uint16_t depth = ReadBE16(data.data() + 22);
	const std::uint16_t colorMode = ReadBE16(data.data() + 24);
	if (channels == 0 || channels > 56 || widthValue > std::numeric_limits<int>::max() ||
		heightValue > std::numeric_limits<int>::max() ||
		!ValidDimensions(static_cast<int>(widthValue), static_cast<int>(heightValue), errorMessage)) return false;
	const int width = static_cast<int>(widthValue);
	const int height = static_cast<int>(heightValue);
	if (!((depth == 1 && colorMode == 0) || depth == 8 || depth == 16)) {
		errorMessage = "unsupported PSD bit depth or color mode";
		return false;
	}
	std::size_t position = 26;
	const auto skipSection = [&data, &position](std::string& error) {
		if (position + 4 > data.size()) { error = "truncated PSD section"; return false; }
		const std::uint32_t length = ReadBE32(data.data() + position);
		position += 4;
		if (length > data.size() - position) { error = "truncated PSD section"; return false; }
		position += length;
		return true;
	};
	std::vector<std::uint8_t> colorModeData;
	if (position + 4 > data.size()) { errorMessage = "truncated PSD color mode data"; return false; }
	const std::uint32_t colorModeLength = ReadBE32(data.data() + position);
	position += 4;
	if (colorModeLength > data.size() - position) { errorMessage = "truncated PSD color mode data"; return false; }
	colorModeData.assign(data.begin() + static_cast<std::ptrdiff_t>(position),
		data.begin() + static_cast<std::ptrdiff_t>(position + colorModeLength));
	position += colorModeLength;
	if (!skipSection(errorMessage) || !skipSection(errorMessage)) return false;
	if (position + 2 > data.size()) { errorMessage = "truncated PSD image data"; return false; }
	const std::uint16_t compression = ReadBE16(data.data() + position);
	position += 2;
	if (compression > 1) {
		errorMessage = "unsupported PSD compression";
		return false;
	}
	const std::size_t sampleBytes = depth == 16 ? 2 : 1;
	const std::size_t rowBytes = depth == 1 ? (static_cast<std::size_t>(width) + 7) / 8 :
		static_cast<std::size_t>(width) * sampleBytes;
	if (rowBytes == 0 || static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / rowBytes) {
		errorMessage = "PSD image is too large";
		return false;
	}
	const std::size_t planeBytes = rowBytes * static_cast<std::size_t>(height);
	std::vector<std::vector<std::uint8_t>> planes(channels);
	try {
		for (std::vector<std::uint8_t>& plane : planes) plane.resize(planeBytes);
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	if (compression == 0) {
		for (std::vector<std::uint8_t>& plane : planes) {
			if (planeBytes > data.size() - position) {
				errorMessage = "truncated PSD image data";
				return false;
			}
			std::copy_n(data.data() + position, planeBytes, plane.data());
			position += planeBytes;
		}
	} else {
		const std::size_t rowCount = static_cast<std::size_t>(channels) * height;
		if (rowCount > (data.size() - position) / 2) {
			errorMessage = "truncated PSD PackBits row table";
			return false;
		}
		std::vector<std::uint16_t> rowLengths(rowCount);
		for (std::uint16_t& length : rowLengths) {
			length = ReadBE16(data.data() + position);
			position += 2;
		}
		for (std::size_t channel = 0; channel < channels; ++channel) {
			for (int y = 0; y < height; ++y) {
				const std::size_t rowIndex = channel * static_cast<std::size_t>(height) + y;
				const std::size_t compressedSize = rowLengths[rowIndex];
				if (compressedSize > data.size() - position) {
					errorMessage = "truncated PSD PackBits row";
					return false;
				}
				const std::size_t rowEnd = position + compressedSize;
				if (!DecodePackBitsRow(data, position, rowEnd,
					planes[channel].data() + static_cast<std::size_t>(y) * rowBytes, rowBytes,
					colorMode == 2 && depth == 8, errorMessage) || position > rowEnd) return false;
				position = rowEnd;
			}
		}
	}
	if (colorMode == 2 && colorModeData.size() < 768) {
		errorMessage = "PSD indexed palette is missing";
		return false;
	}
	if (colorMode == 3 && channels < 3) {
		errorMessage = "PSD RGB channels are missing";
		return false;
	}
	if (colorMode == 1 && channels < 1) {
		errorMessage = "PSD grayscale channel is missing";
		return false;
	}
	if (colorMode == 4 && channels < 4) {
		errorMessage = "PSD CMYK channels are missing";
		return false;
	}
	if (colorMode != 0 && colorMode != 1 && colorMode != 2 && colorMode != 3 && colorMode != 4) {
		errorMessage = "unsupported PSD color mode";
		return false;
	}
	const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
	std::vector<std::uint8_t> bgra(pixelCount * 4, 255);
	const auto sample = [&](const std::vector<std::uint8_t>& plane, std::size_t index) {
		return depth == 16 ? plane[index * 2] : plane[index];
	};
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t pixelIndex = static_cast<std::size_t>(y) * width + x;
			std::uint8_t red = 0;
			std::uint8_t green = 0;
			std::uint8_t blue = 0;
			std::uint8_t alpha = 255;
			if (colorMode == 0) {
				const bool black = (planes[0][static_cast<std::size_t>(y) * rowBytes + x / 8] & (0x80u >> (x % 8))) != 0;
				red = green = blue = black ? 0 : 255;
			} else if (colorMode == 1) {
				red = green = blue = sample(planes[0], pixelIndex);
				if (channels > 1) alpha = sample(planes[1], pixelIndex);
			} else if (colorMode == 2) {
				const std::size_t paletteIndex = static_cast<std::size_t>(sample(planes[0], pixelIndex));
				red = colorModeData[paletteIndex];
				green = colorModeData[256 + paletteIndex];
				blue = colorModeData[512 + paletteIndex];
				if (channels > 1) alpha = sample(planes[1], pixelIndex);
			} else if (colorMode == 3) {
				red = sample(planes[0], pixelIndex);
				green = sample(planes[1], pixelIndex);
				blue = sample(planes[2], pixelIndex);
				if (channels > 3) alpha = sample(planes[3], pixelIndex);
			} else {
				const int cyan = sample(planes[0], pixelIndex);
				const int magenta = sample(planes[1], pixelIndex);
				const int yellow = sample(planes[2], pixelIndex);
				const int black = sample(planes[3], pixelIndex);
				red = static_cast<std::uint8_t>((255 - cyan) * (255 - black) / 255);
				green = static_cast<std::uint8_t>((255 - magenta) * (255 - black) / 255);
				blue = static_cast<std::uint8_t>((255 - yellow) * (255 - black) / 255);
				if (channels > 4) alpha = sample(planes[4], pixelIndex);
			}
			bgra[pixelIndex * 4] = blue;
			bgra[pixelIndex * 4 + 1] = green;
			bgra[pixelIndex * 4 + 2] = red;
			bgra[pixelIndex * 4 + 3] = alpha;
		}
	}
	const bool hasAlphaChannel = (colorMode == 1 || colorMode == 2) ? channels > 1 :
		colorMode == 3 ? channels > 3 : colorMode == 4 && channels > 4;
	return AppendBGRA(image, width, height, std::move(bgra), 0, errorMessage, hasAlphaChannel);
}

bool ReadPnmToken(const std::vector<std::uint8_t>& data, std::size_t& position, std::string& token) {
	while (position < data.size()) {
		if (std::isspace(data[position]) != 0) {
			++position;
			continue;
		}
		if (data[position] == '#') {
			while (position < data.size() && data[position] != '\n') ++position;
			continue;
		}
		break;
	}
	const std::size_t begin = position;
	while (position < data.size() && std::isspace(data[position]) == 0 && data[position] != '#') ++position;
	if (begin == position) return false;
	token.assign(reinterpret_cast<const char*>(data.data() + begin), position - begin);
	return true;
}

bool ParsePositiveInteger(const std::string& text, int& value) {
	try {
		std::size_t parsed = 0;
		const long long number = std::stoll(text, &parsed);
		if (parsed != text.size() || number <= 0 || number > std::numeric_limits<int>::max()) return false;
		value = static_cast<int>(number);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

bool ParseNonNegativeInteger(const std::string& text, int& value) {
	try {
		std::size_t parsed = 0;
		const long long number = std::stoll(text, &parsed);
		if (parsed != text.size() || number < 0 || number > std::numeric_limits<int>::max()) return false;
		value = static_cast<int>(number);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

bool DecodePnm(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> data;
	if (!ReadFile(filename, data, errorMessage)) return false;
	std::size_t position = 0;
	std::string magic;
	if (!ReadPnmToken(data, position, magic) || magic.size() != 2 || magic[0] != 'P' ||
		(magic[1] < '1' || magic[1] > '7')) {
		errorMessage = "invalid PNM header";
		return false;
	}
	int width = 0;
	int height = 0;
	const bool ascii = magic == "P1" || magic == "P2" || magic == "P3";
	int depth = magic == "P7" ? 0 : (magic == "P1" || magic == "P2" || magic == "P4" || magic == "P5" ? 1 : 3);
	int maxValue = magic == "P1" || magic == "P4" ? 1 : 0;
	if (magic == "P7") {
		bool ended = false;
		while (position < data.size()) {
			const std::size_t lineBegin = position;
			while (position < data.size() && data[position] != '\n') ++position;
			const std::string line(reinterpret_cast<const char*>(data.data() + lineBegin), position - lineBegin);
			if (position < data.size()) ++position;
			if (line == "ENDHDR" || line == "ENDHDR\r") {
				ended = true;
				break;
			}
			const std::size_t separator = line.find_first_of(" \t");
			if (separator == std::string::npos) continue;
			const std::string key = line.substr(0, separator);
			const std::string value = line.substr(separator + 1);
			if (key == "WIDTH") ParsePositiveInteger(value, width);
			else if (key == "HEIGHT") ParsePositiveInteger(value, height);
			else if (key == "DEPTH") ParsePositiveInteger(value, depth);
			else if (key == "MAXVAL") ParsePositiveInteger(value, maxValue);
		}
		if (!ended || width <= 0 || height <= 0 || depth <= 0 || maxValue <= 0) {
			errorMessage = "invalid PAM header";
			return false;
		}
	} else {
		std::string widthText;
		std::string heightText;
		if (!ReadPnmToken(data, position, widthText) || !ReadPnmToken(data, position, heightText) ||
			!ParsePositiveInteger(widthText, width) || !ParsePositiveInteger(heightText, height)) {
			errorMessage = "invalid PNM dimensions";
			return false;
		}
		if (magic != "P1" && magic != "P4") {
			std::string maxValueText;
			if (!ReadPnmToken(data, position, maxValueText) || !ParsePositiveInteger(maxValueText, maxValue) || maxValue > 65535) {
				errorMessage = "invalid PNM maximum value";
				return false;
			}
		}
		if (position >= data.size() || std::isspace(data[position]) == 0) {
			errorMessage = "invalid PNM data offset";
			return false;
		}
		++position;
	}
	if (!ValidDimensions(width, height, errorMessage)) return false;
	const std::size_t sampleBytes = maxValue > 255 ? 2 : 1;
	const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
	std::vector<std::uint8_t> bgra(pixelCount * 4, 255);
	if (ascii) {
		const std::size_t requiredSamples = pixelCount * static_cast<std::size_t>(depth);
		std::vector<int> samples;
		try {
			samples.reserve(requiredSamples);
		} catch (const std::exception&) {
			errorMessage = "out of memory";
			return false;
		}
		for (std::size_t sampleIndex = 0; sampleIndex < requiredSamples; ++sampleIndex) {
			std::string sampleText;
			int value = 0;
			if (!ReadPnmToken(data, position, sampleText) ||
				!ParseNonNegativeInteger(sampleText, value) || value > maxValue) {
				errorMessage = "invalid ASCII PNM sample";
				return false;
			}
			samples.push_back(value);
		}
		const auto scale = [maxValue](int value) {
			return static_cast<std::uint8_t>((value * 255 + maxValue / 2) / maxValue);
		};
		for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
			std::uint8_t* pixel = bgra.data() + pixelIndex * 4;
			if (magic == "P1") {
				const std::uint8_t value = samples[pixelIndex] == 0 ? 255 : 0;
				pixel[0] = pixel[1] = pixel[2] = value;
			} else if (depth == 1) {
				pixel[0] = pixel[1] = pixel[2] = scale(samples[pixelIndex]);
			} else {
				pixel[2] = scale(samples[pixelIndex * 3]);
				pixel[1] = scale(samples[pixelIndex * 3 + 1]);
				pixel[0] = scale(samples[pixelIndex * 3 + 2]);
			}
		}
	} else if (magic == "P4") {
		const std::size_t rowBytes = (static_cast<std::size_t>(width) + 7) / 8;
		if (position > data.size() || (rowBytes > 0 && static_cast<std::size_t>(height) >
			(data.size() - position) / rowBytes)) {
			errorMessage = "truncated PBM image";
			return false;
		}
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const bool black = (data[position + static_cast<std::size_t>(y) * rowBytes + x / 8] & (0x80u >> (x % 8))) != 0;
				const std::uint8_t value = black ? 0 : 255;
				std::uint8_t* pixel = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4;
				pixel[0] = pixel[1] = pixel[2] = value;
			}
		}
	} else {
		const std::size_t requiredSamples = pixelCount * static_cast<std::size_t>(depth);
		if (position > data.size() || (sampleBytes > 0 && requiredSamples >
			(data.size() - position) / sampleBytes)) {
			errorMessage = "truncated PNM image";
			return false;
		}
		const auto readSample = [&](std::size_t& offset) {
			int value = data[position + offset++];
			if (sampleBytes == 2) value = (value << 8) | data[position + offset++];
			return static_cast<std::uint8_t>((value * 255 + maxValue / 2) / maxValue);
		};
		std::size_t offset = 0;
		for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
			std::uint8_t* pixel = bgra.data() + pixelIndex * 4;
			if (depth == 1) {
				pixel[0] = pixel[1] = pixel[2] = readSample(offset);
			} else if (depth == 2) {
				pixel[0] = pixel[1] = pixel[2] = readSample(offset);
				pixel[3] = readSample(offset);
			} else {
				pixel[2] = readSample(offset);
				pixel[1] = readSample(offset);
				pixel[0] = readSample(offset);
				if (depth >= 4) pixel[3] = readSample(offset);
				for (int extra = 4; extra < depth; ++extra) readSample(offset);
			}
		}
	}
	const bool hasAlphaChannel = depth == 2 || depth >= 4;
	return AppendBGRA(image, width, height, std::move(bgra), 0, errorMessage, hasAlphaChannel);
}

} // namespace jpegview_linux::decoder_detail
