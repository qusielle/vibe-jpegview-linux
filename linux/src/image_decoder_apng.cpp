#include "image_decoder_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

extern "C" {
#include <png.h>
#include <zlib.h>
}

namespace jpegview_linux::decoder_detail {

struct PngChunk {
	std::array<char, 4> type{};
	std::vector<std::uint8_t> data;
};

bool IsChunk(const PngChunk& chunk, std::string_view type) {
	return std::string_view(chunk.type.data(), chunk.type.size()) == type;
}

bool ParsePngChunks(const std::vector<std::uint8_t>& data, std::vector<PngChunk>& chunks,
	std::string& errorMessage) {
	static constexpr std::array<std::uint8_t, 8> signature = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
	if (data.size() < signature.size() || !std::equal(signature.begin(), signature.end(), data.begin())) {
		errorMessage = "invalid PNG signature";
		return false;
	}
	std::size_t position = signature.size();
	while (position + 12 <= data.size()) {
		const std::uint32_t length = ReadBE32(data.data() + position);
		position += 4;
		if (length > data.size() - position - 8) {
			errorMessage = "truncated PNG chunk";
			return false;
		}
		PngChunk chunk;
		std::copy_n(data.data() + position, 4, chunk.type.begin());
		position += 4;
		chunk.data.assign(data.begin() + static_cast<std::ptrdiff_t>(position),
			data.begin() + static_cast<std::ptrdiff_t>(position + length));
		position += length + 4; // Skip the chunk CRC; libpng validates the reconstructed chunks.
		chunks.push_back(std::move(chunk));
		if (IsChunk(chunks.back(), "IEND")) return true;
	}
	errorMessage = "PNG has no IEND chunk";
	return false;
}

void AppendPngChunk(std::vector<std::uint8_t>& output, const std::array<char, 4>& type,
	const std::vector<std::uint8_t>& data) {
	const auto append32 = [&output](std::uint32_t value) {
		output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
		output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
		output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
		output.push_back(static_cast<std::uint8_t>(value & 0xff));
	};
	append32(static_cast<std::uint32_t>(data.size()));
	output.insert(output.end(), type.begin(), type.end());
	output.insert(output.end(), data.begin(), data.end());
	const uLong typeCrc = crc32(0, reinterpret_cast<const Bytef*>(type.data()), type.size());
	const uLong dataCrc = crc32(typeCrc, data.data(), data.size());
	append32(static_cast<std::uint32_t>(dataCrc));
}

bool DecodePngMemory(const std::vector<std::uint8_t>& data, int expectedWidth, int expectedHeight,
	std::vector<std::uint8_t>& rgba, std::string& errorMessage) {
	png_image pngImage{};
	pngImage.version = PNG_IMAGE_VERSION;
	if (png_image_begin_read_from_memory(&pngImage, data.data(), data.size()) == 0) {
		errorMessage = pngImage.message[0] == '\0' ? "PNG decoder failed" : pngImage.message;
		return false;
	}
	if (!ValidDimensions(static_cast<int>(pngImage.width), static_cast<int>(pngImage.height), errorMessage) ||
		static_cast<int>(pngImage.width) != expectedWidth || static_cast<int>(pngImage.height) != expectedHeight) {
		png_image_free(&pngImage);
		if (errorMessage.empty()) errorMessage = "invalid APNG frame dimensions";
		return false;
	}
	pngImage.format = PNG_FORMAT_RGBA;
	try {
		rgba.resize(PNG_IMAGE_SIZE(pngImage));
	} catch (const std::exception&) {
		png_image_free(&pngImage);
		errorMessage = "out of memory";
		return false;
	}
	if (png_image_finish_read(&pngImage, nullptr, rgba.data(), 0, nullptr) == 0) {
		errorMessage = pngImage.message[0] == '\0' ? "PNG decoder failed" : pngImage.message;
		png_image_free(&pngImage);
		return false;
	}
	png_image_free(&pngImage);
	return true;
}

struct ApngFrameControl {
	int width = 0;
	int height = 0;
	int x = 0;
	int y = 0;
	std::uint16_t delayNumerator = 0;
	std::uint16_t delayDenominator = 100;
	std::uint8_t dispose = 0;
	std::uint8_t blend = 0;
};

struct ApngFrameData {
	ApngFrameControl control;
	std::vector<std::vector<std::uint8_t>> compressed;
};

bool DecodeApng(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	std::vector<std::uint8_t> fileData;
	if (!ReadFile(filename, fileData, errorMessage)) return false;
	std::vector<PngChunk> chunks;
	if (!ParsePngChunks(fileData, chunks, errorMessage)) return false;
	if (chunks.empty() || !IsChunk(chunks.front(), "IHDR") || chunks.front().data.size() != 13) {
		errorMessage = "invalid PNG header";
		return false;
	}
	const int canvasWidth = static_cast<int>(ReadBE32(chunks.front().data.data()));
	const int canvasHeight = static_cast<int>(ReadBE32(chunks.front().data.data() + 4));
	if (!ValidDimensions(canvasWidth, canvasHeight, errorMessage)) return false;

	std::uint32_t frameCount = 0;
	std::uint32_t loopCount = 0;
	bool hasAnimationControl = false;
	for (const PngChunk& chunk : chunks) {
		if (IsChunk(chunk, "acTL") && chunk.data.size() == 8) {
			frameCount = ReadBE32(chunk.data.data());
			loopCount = ReadBE32(chunk.data.data() + 4);
			hasAnimationControl = frameCount > 0;
			break;
		}
	}
	if (!hasAnimationControl) return DecodeStb(filename, image, errorMessage);

	std::vector<ApngFrameData> frames;
	for (const PngChunk& chunk : chunks) {
		if (IsChunk(chunk, "fcTL")) {
			if (chunk.data.size() != 26) {
				errorMessage = "invalid APNG frame control";
				return false;
			}
			ApngFrameData frame;
			frame.control.width = static_cast<int>(ReadBE32(chunk.data.data() + 4));
			frame.control.height = static_cast<int>(ReadBE32(chunk.data.data() + 8));
			frame.control.x = static_cast<int>(ReadBE32(chunk.data.data() + 12));
			frame.control.y = static_cast<int>(ReadBE32(chunk.data.data() + 16));
			frame.control.delayNumerator = ReadBE16(chunk.data.data() + 20);
			frame.control.delayDenominator = ReadBE16(chunk.data.data() + 22);
			if (frame.control.delayDenominator == 0) frame.control.delayDenominator = 100;
			frame.control.dispose = chunk.data[24];
			frame.control.blend = chunk.data[25];
			if (!ValidDimensions(frame.control.width, frame.control.height, errorMessage) ||
				frame.control.x < 0 || frame.control.y < 0 ||
				frame.control.width > canvasWidth - frame.control.x ||
				frame.control.height > canvasHeight - frame.control.y ||
				frame.control.dispose > 2 || frame.control.blend > 1) {
				errorMessage = "invalid APNG frame geometry";
				return false;
			}
			frames.push_back(std::move(frame));
			continue;
		}
		if (IsChunk(chunk, "IDAT")) {
			if (frames.empty()) continue; // The default image is not an animation frame.
			frames.back().compressed.push_back(chunk.data);
			continue;
		}
		if (IsChunk(chunk, "fdAT")) {
			if (frames.empty() || chunk.data.size() < 4) {
				errorMessage = "invalid APNG frame data";
				return false;
			}
			frames.back().compressed.emplace_back(chunk.data.begin() + 4, chunk.data.end());
		}
	}
	if (frames.empty() || frames.size() != frameCount) {
		errorMessage = "APNG frame count is invalid";
		return false;
	}

	std::vector<std::uint8_t> pngSignature = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
	std::vector<std::uint8_t> canvas(static_cast<std::size_t>(canvasWidth) * canvasHeight * 4, 0);
	for (const ApngFrameData& frame : frames) {
		if (frame.compressed.empty()) {
			errorMessage = "APNG frame has no image data";
			return false;
		}
		std::vector<std::uint8_t> ihdr = chunks.front().data;
		ihdr[0] = static_cast<std::uint8_t>((frame.control.width >> 24) & 0xff);
		ihdr[1] = static_cast<std::uint8_t>((frame.control.width >> 16) & 0xff);
		ihdr[2] = static_cast<std::uint8_t>((frame.control.width >> 8) & 0xff);
		ihdr[3] = static_cast<std::uint8_t>(frame.control.width & 0xff);
		ihdr[4] = static_cast<std::uint8_t>((frame.control.height >> 24) & 0xff);
		ihdr[5] = static_cast<std::uint8_t>((frame.control.height >> 16) & 0xff);
		ihdr[6] = static_cast<std::uint8_t>((frame.control.height >> 8) & 0xff);
		ihdr[7] = static_cast<std::uint8_t>(frame.control.height & 0xff);
		std::vector<std::uint8_t> framePng = pngSignature;
		AppendPngChunk(framePng, chunks.front().type, ihdr);
		for (const PngChunk& chunk : chunks) {
			if (IsChunk(chunk, "IHDR") || IsChunk(chunk, "IEND") || IsChunk(chunk, "acTL") ||
				IsChunk(chunk, "fcTL") || IsChunk(chunk, "IDAT") || IsChunk(chunk, "fdAT")) continue;
			AppendPngChunk(framePng, chunk.type, chunk.data);
		}
		for (const std::vector<std::uint8_t>& compressed : frame.compressed) {
			AppendPngChunk(framePng, std::array<char, 4>{'I', 'D', 'A', 'T'}, compressed);
		}
		AppendPngChunk(framePng, std::array<char, 4>{'I', 'E', 'N', 'D'}, {});
		std::vector<std::uint8_t> rgba;
		if (!DecodePngMemory(framePng, frame.control.width, frame.control.height, rgba, errorMessage)) return false;

		std::vector<std::uint8_t> previous;
		if (frame.control.dispose == 2) previous = canvas;
		for (int y = 0; y < frame.control.height; ++y) {
			for (int x = 0; x < frame.control.width; ++x) {
				const std::size_t sourceIndex = (static_cast<std::size_t>(y) * frame.control.width + x) * 4;
				const std::size_t targetIndex = (static_cast<std::size_t>(frame.control.y + y) * canvasWidth +
					frame.control.x + x) * 4;
				if (frame.control.blend == 0 || rgba[sourceIndex + 3] == 255) {
					std::copy_n(rgba.data() + sourceIndex, 4, canvas.data() + targetIndex);
					continue;
				}
				const unsigned sourceAlpha = rgba[sourceIndex + 3];
				const unsigned destinationAlpha = canvas[targetIndex + 3];
				const unsigned outputAlpha = sourceAlpha + (destinationAlpha * (255 - sourceAlpha) + 127) / 255;
				if (outputAlpha == 0) {
					std::fill_n(canvas.data() + targetIndex, 4, 0);
					continue;
				}
				for (int channel = 0; channel < 3; ++channel) {
					const unsigned source = rgba[sourceIndex + channel] * sourceAlpha;
					const unsigned destination = canvas[targetIndex + channel] * destinationAlpha * (255 - sourceAlpha) / 255;
					canvas[targetIndex + channel] = static_cast<std::uint8_t>((source + destination + outputAlpha / 2) / outputAlpha);
				}
				canvas[targetIndex + 3] = static_cast<std::uint8_t>(outputAlpha);
			}
		}
		const int delay = frame.control.delayNumerator == 0 ? 10 : std::max(10,
			static_cast<int>((1000ull * frame.control.delayNumerator + frame.control.delayDenominator / 2) /
				frame.control.delayDenominator));
		if (!AppendRGBA(image, canvasWidth, canvasHeight, canvas.data(), delay, errorMessage)) return false;
		if (frame.control.dispose == 1) {
			for (int y = 0; y < frame.control.height; ++y) {
				std::fill_n(canvas.data() + (static_cast<std::size_t>(frame.control.y + y) * canvasWidth + frame.control.x) * 4,
					static_cast<std::size_t>(frame.control.width) * 4, 0);
			}
		} else if (frame.control.dispose == 2) {
			canvas = std::move(previous);
		}
	}
	image.animation = image.frames.size() > 1;
	image.loopCount = static_cast<int>(std::min<std::uint32_t>(loopCount, std::numeric_limits<int>::max()));
	return !image.frames.empty();
}

} // namespace jpegview_linux::decoder_detail
