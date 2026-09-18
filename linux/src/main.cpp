#include "sdl_abi.h"
#include "file_list.h"
#include "exif_reader.h"
#include "clipboard.h"
#include "image_writer.h"
#include "image_decoder.h"

// Keep Linux command dispatch aligned with the original Windows application.
// resource.h is deliberately platform-neutral: it contains the command IDs
// shared by JPEGView.rc, CMainDlg::ExecuteCommand, and KeyMap.txt.default.
#include "../../src/JPEGView/resource.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <deque>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr double kMinZoom = 0.01;
constexpr double kMaxZoom = 32.0;
constexpr int kUiTextScale = 1;
constexpr int kContextMenuItemHeight = 18;
constexpr int kContextMenuSeparatorHeight = 7;
constexpr int kBatchSelectAll = 0;
constexpr int kBatchSelectNone = 1;
constexpr int kBatchPreview = 2;
constexpr int kBatchSavePattern = 3;
constexpr int kBatchRename = 4;
constexpr int kBatchClose = 5;
constexpr int kResizePercent = 0;
constexpr int kResizeWidth = 1;
constexpr int kResizeHeight = 2;
constexpr int kResizeFilter = 3;
constexpr int kResizeApply = 0;
constexpr int kResizeCancel = 1;
constexpr int kResizeFilterCount = 4;
constexpr std::uint64_t kMaxImagePixels = 100ull * 1024ull * 1024ull;
constexpr int kMaxImageDimension = 65535;

struct ControlButton {
	SDL_Rect rect{};
	int command = IDM_NEXT;
};

struct MenuItem {
	const char* label = nullptr;
	int command = IDM_NEXT;
	bool separator = false;
	bool checked = false;
	bool enabled = true;
	const char* shortcut = nullptr;
};

struct OpenWithApplication {
	std::string name;
	std::string exec;
	fs::path desktopFile;
	bool terminal = false;
};

struct FileDialogEntry {
	fs::path path;
	bool directory = false;
	bool parent = false;
};

struct BatchCopyItem {
	fs::path source;
	std::time_t modificationTime = 0;
	bool selected = false;
	bool copy = false;
	fs::path destination;
	std::string destinationText;
};

struct FontGlyph {
	char character;
	std::array<Uint8, 7> rows;
};

// A small built-in 5x7 font keeps the native SDL frontend independent of a
// host font or an additional text-rendering shared library.  The Windows
// popup menu is text based too; this is its portable drawing equivalent.
const std::array<Uint8, 7>& GlyphRows(char character) {
	static const std::array<Uint8, 7> empty{};
	static const std::array<FontGlyph, 49> glyphs = {{
		{'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
		{'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
		{'C', {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}},
		{'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
		{'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
		{'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
		{'G', {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F}},
		{'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
		{'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
		{'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}},
		{'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
		{'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
		{'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
		{'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
		{'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
		{'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
		{'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
		{'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
		{'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
		{'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
		{'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
		{'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
		{'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
		{'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
		{'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
		{'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
		{'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
		{'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
		{'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
		{'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
		{'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
		{'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
		{'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
		{'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
		{'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
		{'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
		{'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}},
		{':', {0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00}},
		{'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
		{'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
		{'%', {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13}},
		{'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
		{'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
		{'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},
		{']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
		{'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
		{')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
		{'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
		{'=', {0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00}},
	}};
	const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	for (const FontGlyph& glyph : glyphs) {
		if (glyph.character == upper) return glyph.rows;
	}
	return empty;
}

int TextWidth(const std::string& text, int scale) {
	return text.empty() ? 0 : static_cast<int>(text.size()) * (5 * scale + scale) - scale;
}

std::string ClipText(const std::string& value, int maximumWidth, int scale = kUiTextScale) {
	if (maximumWidth <= 0) return {};
	if (TextWidth(value, scale) <= maximumWidth) return value;
	const std::size_t maximumCharacters = static_cast<std::size_t>(std::max(3,
		maximumWidth / (6 * scale)));
	if (maximumCharacters <= 3) return value.substr(0, 1) + "...";
	return value.substr(0, maximumCharacters - 3) + "...";
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

bool IsImagePath(const fs::path& path) {
	static const std::set<std::string> extensions = {
		".jpg", ".jpeg", ".jpe", ".png", ".gif", ".bmp", ".tga",
		".psd", ".pnm", ".pbm", ".pgm", ".ppm", ".pam", ".pic", ".qoi", ".apng", ".webp",
		".tif", ".tiff", ".heic", ".heif", ".hif", ".avif", ".avifs", ".jxl",
		".jxr", ".wdp", ".hdp", ".mdp", ".pef", ".dng", ".crw", ".nef", ".cr2",
		".mrw", ".rw2", ".orf", ".x3f", ".arw", ".kdc", ".nrw", ".dcr", ".sr2",
		".raf", ".kc2", ".erf", ".3fr", ".raw", ".mef", ".mos", ".mdc", ".cr3",
		".iiq", ".rwl"
	};
	return extensions.find(Lower(path.extension().string())) != extensions.end();
}

fs::path AbsoluteNormalized(const fs::path& path) {
	std::error_code error;
	const fs::path absolute = fs::absolute(path, error);
	return (error ? path : absolute).lexically_normal();
}

struct Image {
	int width = 0;
	int height = 0;
	int originalWidth = 0;
	int originalHeight = 0;
	std::vector<std::uint8_t> bgra;

	bool StoreBGRA(const std::uint8_t* bgraPixels, int imageWidth, int imageHeight) {
		if (bgraPixels == nullptr || imageWidth <= 0 || imageHeight <= 0) return false;
		const std::size_t pixelCount = static_cast<std::size_t>(imageWidth) *
			static_cast<std::size_t>(imageHeight);
		if (pixelCount > std::numeric_limits<std::size_t>::max() / 4) return false;
		try {
			bgra.assign(bgraPixels, bgraPixels + pixelCount * 4);
		} catch (const std::exception&) {
			return false;
		}
		width = imageWidth;
		height = imageHeight;
		originalWidth = imageWidth;
		originalHeight = imageHeight;
		return true;
	}

	bool Rotate(bool clockwise) {
		if (width <= 0 || height <= 0) return false;
		const int newWidth = height;
		const int newHeight = width;
		std::vector<std::uint8_t> transformed;
		try {
			transformed.resize(bgra.size());
		} catch (const std::exception&) {
			return false;
		}
		for (int sourceY = 0; sourceY < height; ++sourceY) {
			for (int sourceX = 0; sourceX < width; ++sourceX) {
				const int targetX = clockwise ? height - sourceY - 1 : sourceY;
				const int targetY = clockwise ? sourceX : width - sourceX - 1;
				const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
				const std::size_t targetOffset = (static_cast<std::size_t>(targetY) * newWidth + targetX) * 4;
				std::copy_n(bgra.data() + sourceOffset, 4, transformed.data() + targetOffset);
			}
		}
		width = newWidth;
		height = newHeight;
		bgra.swap(transformed);
		return true;
	}

	bool Mirror(bool horizontal) {
		if (width <= 0 || height <= 0) return false;
		std::vector<std::uint8_t> transformed;
		try {
			transformed.resize(bgra.size());
		} catch (const std::exception&) {
			return false;
		}
		for (int sourceY = 0; sourceY < height; ++sourceY) {
			for (int sourceX = 0; sourceX < width; ++sourceX) {
				const int targetX = horizontal ? width - sourceX - 1 : sourceX;
				const int targetY = horizontal ? sourceY : height - sourceY - 1;
				const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
				const std::size_t targetOffset = (static_cast<std::size_t>(targetY) * width + targetX) * 4;
				std::copy_n(bgra.data() + sourceOffset, 4, transformed.data() + targetOffset);
			}
		}
		bgra.swap(transformed);
		return true;
	}

	// Resample using the same four choices exposed by JPEGView's ResizeDlg:
	// point sampling, Lanczos, and two sharpened best-quality kernels.
	bool Resize(int newWidth, int newHeight, int filter = 3) {
		if (width <= 0 || height <= 0 || newWidth <= 0 || newHeight <= 0) return false;
		if (newWidth > kMaxImageDimension || newHeight > kMaxImageDimension ||
			static_cast<std::uint64_t>(newWidth) * static_cast<std::uint64_t>(newHeight) > kMaxImagePixels) return false;
		if (newWidth == width && newHeight == height) return true;
		filter = std::clamp(filter, 0, kResizeFilterCount - 1);
		// JPEGView breaks very large reductions into several passes. This avoids
		// an excessively wide filter kernel and preserves detail better than a
		// single extreme reduction.
		const double totalReduction = static_cast<double>(width) / newWidth;
		if (filter != 0 && totalReduction > 5.0) {
			const int steps = std::max(2, static_cast<int>(std::ceil(std::log(totalReduction) / std::log(5.0))));
			const double factor = std::pow(totalReduction, 1.0 / steps);
			int passWidth = width;
			int passHeight = height;
			for (int pass = 0; pass < steps; ++pass) {
				const int nextWidth = pass == steps - 1 ? newWidth :
					std::max(newWidth, static_cast<int>(passWidth / factor));
				const int nextHeight = pass == steps - 1 ? newHeight :
					std::max(newHeight, static_cast<int>(passHeight / factor));
				const int passFilter = pass == steps - 1 ? filter : (filter == 3 ? 2 : 1);
				if (nextWidth == passWidth && nextHeight == passHeight) continue;
				if (!Resize(nextWidth, nextHeight, passFilter)) return false;
				passWidth = nextWidth;
				passHeight = nextHeight;
			}
			return true;
		}

		if (filter == 0) {
			std::vector<std::uint8_t> sampled;
			try {
				sampled.resize(static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight) * 4);
			} catch (const std::exception&) {
				return false;
			}
			const auto sourceCoordinate = [](int target, int sourceSize, int targetSize) {
				if (targetSize <= sourceSize) {
					return std::min(sourceSize - 1,
						static_cast<int>((static_cast<std::uint64_t>(target) * sourceSize) / targetSize));
				}
				if (targetSize == 1 || sourceSize == 1) return 0;
				return std::min(sourceSize - 1,
					static_cast<int>((static_cast<std::uint64_t>(target) * (sourceSize - 1)) / (targetSize - 1)));
			};
			for (int targetY = 0; targetY < newHeight; ++targetY) {
				const int sourceY = sourceCoordinate(targetY, height, newHeight);
				for (int targetX = 0; targetX < newWidth; ++targetX) {
					const int sourceX = sourceCoordinate(targetX, width, newWidth);
					const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
					const std::size_t targetOffset = (static_cast<std::size_t>(targetY) * newWidth + targetX) * 4;
					std::copy_n(bgra.data() + sourceOffset, 4, sampled.data() + targetOffset);
				}
			}
			width = newWidth;
			height = newHeight;
			bgra.swap(sampled);
			return true;
		}

		struct Sample {
			int index = 0;
			double weight = 0.0;
		};
		struct Kernel {
			std::vector<Sample> samples;
		};
		const auto cubic = [](double distance) {
			constexpr double parameter = -0.5; // Catmull-Rom, matching JPEGView's bicubic upsampling.
			const double value = std::abs(distance);
			if (value < 1.0) {
				return (parameter + 2.0) * value * value * value - (parameter + 3.0) * value * value + 1.0;
			}
			if (value < 2.0) {
				return parameter * value * value * value - 5.0 * parameter * value * value +
					8.0 * parameter * value - 4.0 * parameter;
			}
			return 0.0;
		};
		const auto bestQuality = [](double distance, double sharpen) {
			if (distance < -2.0 || distance > 2.0) return 0.0;
			if (distance < -1.0) {
				const double value = 2.0 * distance + 3.0;
				return -sharpen * (1.0 - value * value);
			}
			if (distance < 1.0) return 1.0 - distance * distance;
			const double value = 2.0 * distance - 3.0;
			return -sharpen * (1.0 - value * value);
		};
		const auto lanczos = [](double distance, double sharpen) {
			if (distance < -2.0 || distance > 2.0) return 0.0;
			if (std::abs(distance) < 1e-6) return 1.0;
			const double pi = 3.14159265358979323846;
			const double value = (2.0 * std::sin(pi * distance) * std::sin(0.5 * pi * distance)) /
				(pi * pi * distance * distance);
			return std::abs(distance) < 1.0 ? value : sharpen * value;
		};
		const auto integratedBestQuality = [&bestQuality](double distance, double multiplier, double sharpen) {
			// JPEGView convolves its downsampling kernel with a one-pixel box.
			// The 32-point integration is the same approximation used by the
			// original ResizeFilter implementation.
			constexpr int steps = 32;
			double position = distance * multiplier - multiplier * 0.5;
			const double step = multiplier / (steps - 1);
			double sum = 0.0;
			for (int i = 0; i < steps; ++i) {
				sum += bestQuality(position, sharpen);
				position += step;
			}
			return sum;
		};
		const auto buildKernels = [&cubic, &lanczos, &integratedBestQuality, filter](int sourceSize, int targetSize) {
			std::vector<Kernel> kernels(static_cast<std::size_t>(targetSize));
			if (sourceSize == targetSize) {
				for (int target = 0; target < targetSize; ++target) {
					kernels[static_cast<std::size_t>(target)].samples.push_back(Sample{target, 1.0});
				}
				return kernels;
			}

			const bool downsampling = targetSize < sourceSize;
			const double scale = static_cast<double>(targetSize) / sourceSize;
			const double sourcePerTarget = 1.0 / scale;
			const bool useLanczos = filter == 1;
			const double sharpen = filter == 2 ? 0.15 : 0.3;
			const double downsamplingMultiplier = downsampling ? (useLanczos ? sourcePerTarget :
				(sourcePerTarget < 2.0 ? 1.0 / (sourcePerTarget - 0.5) :
				1.0 / ((sourcePerTarget + 1.0) * 0.5))) : 1.0;
			for (int target = 0; target < targetSize; ++target) {
				// Downsampling uses the source-space center of each destination
				// pixel. Upsampling follows JPEGView's endpoint-preserving
				// bicubic mapping, so the first and last source pixels stay sharp.
				const double center = downsampling ?
					(target + 0.5) * sourcePerTarget - 0.5 :
					(sourceSize == 1 || targetSize == 1) ? 0.0 :
					static_cast<double>(target) * (sourceSize - 1) / (targetSize - 1);
				const int integerCenter = static_cast<int>(std::floor(center));
				const double fraction = center - integerCenter;
				const int filterLength = downsampling ? std::min(64, static_cast<int>(useLanczos ?
					5.0 * sourcePerTarget : 4.0 * (sourcePerTarget + 1.0) * 0.5 + 0.99)) : 4;
				const int filterOffset = downsampling ? (filterLength - 1) / 2 : 1;
				const int first = integerCenter - filterOffset;
				const int last = first + filterLength - 1;
				double weightSum = 0.0;
				for (int source = first; source <= last; ++source) {
					if (source < 0 || source >= sourceSize) continue;
					const double weight = downsampling ? (useLanczos ?
						lanczos((source - integerCenter - fraction) * downsamplingMultiplier, 1.0) :
						integratedBestQuality(source - integerCenter - fraction, downsamplingMultiplier, sharpen)) :
						cubic(center - source);
					if (std::abs(weight) < 1e-12) continue;
					kernels[static_cast<std::size_t>(target)].samples.push_back(Sample{source, weight});
					weightSum += weight;
				}
				if (std::abs(weightSum) < 1e-12) {
					kernels[static_cast<std::size_t>(target)].samples.push_back(
						Sample{std::clamp(static_cast<int>(std::round(center)), 0, sourceSize - 1), 1.0});
				} else {
					for (Sample& sample : kernels[static_cast<std::size_t>(target)].samples) sample.weight /= weightSum;
				}
			}
			return kernels;
		};

		const std::size_t bytesPerPixel = 4;
		const std::size_t maximum = std::numeric_limits<std::size_t>::max();
		if (static_cast<std::size_t>(newWidth) > maximum / bytesPerPixel ||
			static_cast<std::size_t>(height) > maximum / (static_cast<std::size_t>(newWidth) * bytesPerPixel) ||
			static_cast<std::size_t>(newHeight) > maximum / (static_cast<std::size_t>(newWidth) * bytesPerPixel)) return false;
		const std::size_t horizontalBytes = static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(height) * bytesPerPixel;
		const std::size_t outputBytes = static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight) * bytesPerPixel;
		std::vector<std::uint8_t> horizontal;
		std::vector<std::uint8_t> resized;
		try {
			horizontal.resize(horizontalBytes);
			resized.resize(outputBytes);
		} catch (const std::exception&) {
			return false;
		}
		const std::vector<Kernel> horizontalKernels = buildKernels(width, newWidth);
		const std::vector<Kernel> verticalKernels = buildKernels(height, newHeight);
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < newWidth; ++x) {
				std::array<double, 4> values{};
				for (const Sample& sample : horizontalKernels[static_cast<std::size_t>(x)].samples) {
					const std::size_t sourceOffset = (static_cast<std::size_t>(y) * width + sample.index) * 4;
					for (int channel = 0; channel < 4; ++channel) values[static_cast<std::size_t>(channel)] +=
						sample.weight * bgra[sourceOffset + static_cast<std::size_t>(channel)];
				}
				const std::size_t targetOffset = (static_cast<std::size_t>(y) * newWidth + x) * 4;
				for (int channel = 0; channel < 4; ++channel) horizontal[targetOffset + static_cast<std::size_t>(channel)] =
					static_cast<std::uint8_t>(std::clamp(std::lround(values[static_cast<std::size_t>(channel)]), 0l, 255l));
			}
		}
		for (int y = 0; y < newHeight; ++y) {
			for (int x = 0; x < newWidth; ++x) {
				std::array<double, 4> values{};
				for (const Sample& sample : verticalKernels[static_cast<std::size_t>(y)].samples) {
					const std::size_t sourceOffset = (static_cast<std::size_t>(sample.index) * newWidth + x) * 4;
					for (int channel = 0; channel < 4; ++channel) values[static_cast<std::size_t>(channel)] +=
						sample.weight * horizontal[sourceOffset + static_cast<std::size_t>(channel)];
				}
				const std::size_t targetOffset = (static_cast<std::size_t>(y) * newWidth + x) * 4;
				for (int channel = 0; channel < 4; ++channel) resized[targetOffset + static_cast<std::size_t>(channel)] =
					static_cast<std::uint8_t>(std::clamp(std::lround(values[static_cast<std::size_t>(channel)]), 0l, 255l));
			}
		}
		width = newWidth;
		height = newHeight;
		bgra.swap(resized);
		return true;
	}

	// Port of JPEGView's CHistogram/CHistogramCorr path. The correction is
	// deliberately applied to a copy by Viewer, so toggling it never changes
	// the decoded source pixels.
	bool AutoContrast() {
		if (width <= 0 || height <= 0 || bgra.empty()) return false;

		std::array<int, 256> channelB{};
		std::array<int, 256> channelG{};
		std::array<int, 256> channelR{};
		std::array<int, 256> channelGrey{};
		long long sumB = 0;
		long long sumG = 0;
		long long sumR = 0;
		const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
		const int grid = std::max(1, static_cast<int>(0.5 + std::sqrt(1.0 + pixelCount / 50000.0)));
		const int pixelsPerLine = std::max(1, width / grid);
		const int lines = std::max(1, height / grid);
		int sampledPixels = 0;
		for (int line = 0; line < lines; ++line) {
			const int y = std::min(height - 1, line * grid);
			for (int column = 0; column < pixelsPerLine; ++column) {
				const int x = std::min(width - 1, column * grid);
				const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
				const int blue = bgra[offset];
				const int green = bgra[offset + 1];
				const int red = bgra[offset + 2];
				++channelB[static_cast<std::size_t>(blue)];
				++channelG[static_cast<std::size_t>(green)];
				++channelR[static_cast<std::size_t>(red)];
				channelGrey[static_cast<std::size_t>((blue * 128 + green * 640 + red * 256) >> 10)]++;
				sumB += blue;
				sumG += green;
				sumR += red;
				++sampledPixels;
			}
		}
		if (sampledPixels <= 0) return false;

		const auto calculateBlackWhite = [sampledPixels](const std::array<int, 256>& values) {
			const int level = static_cast<int>(sampledPixels * 0.001 + 0.5);
			int sum = 0;
			int index = 0;
			while (sum < level && index < 256) sum += values[static_cast<std::size_t>(index++)];
			const double black = index == 0 ? 0.0 : (index - 1) / 255.0;
			sum = 0;
			index = 255;
			while (sum < level && index >= 0) sum += values[static_cast<std::size_t>(index--)];
			const double white = index == 255 ? 1.0 : (index + 1) / 255.0;
			return std::array<double, 2>{black, white};
		};

		const std::array<double, 2> bwB = calculateBlackWhite(channelB);
		const std::array<double, 2> bwG = calculateBlackWhite(channelG);
		const std::array<double, 2> bwR = calculateBlackWhite(channelR);
		const std::array<double, 2> bwGrey = calculateBlackWhite(channelGrey);
		constexpr double contrastStrength = 0.5; // JPEGView's default AutoContrastCorrectionAmount.
		constexpr double brightnessStrength = 0.2; // JPEGView's default AutoBrightnessCorrectionAmount.
		constexpr double contrastFactor = 0.5; // default ContrastCorrectionFactor.
		const double logHalf = std::log10(0.5);
		const double strength = std::max(1e-4, (std::log10(contrastStrength) / logHalf) *
			(std::log10(contrastFactor) / logHalf));
		const double histogramWidth = std::max(0.0, bwGrey[1] - bwGrey[0]);
		const double widthFactor = std::pow(histogramWidth, strength * 0.5);
		double blackB = bwB[0] * std::pow((1.0 - bwB[0]) * (1.0 - bwGrey[0]), strength) * widthFactor;
		double blackG = bwG[0] * std::pow((1.0 - bwG[0]) * (1.0 - bwGrey[0]), strength) * widthFactor;
		double blackR = bwR[0] * std::pow((1.0 - bwR[0]) * (1.0 - bwGrey[0]), strength) * widthFactor;
		double whiteB = 1.0 - (1.0 - bwB[1]) * std::pow(bwB[1] * bwGrey[1], strength) * widthFactor;
		double whiteG = 1.0 - (1.0 - bwG[1]) * std::pow(bwG[1] * bwGrey[1], strength) * widthFactor;
		double whiteR = 1.0 - (1.0 - bwR[1]) * std::pow(bwR[1] * bwGrey[1], strength) * widthFactor;
		const double meanBlack = (blackB + blackG + blackR) / 3.0;
		const double meanWhite = (whiteB + whiteG + whiteR) / 3.0;
		const double meanRange = meanWhite - meanBlack;
		const double correctionA = 1.0 / std::max(0.001, meanRange);
		const double correctionB = -meanBlack * correctionA;
		const double midPoint = (meanWhite + meanBlack) * 0.5;
		double correction = midPoint * (1.0 - correctionA) - correctionB;
		correction = std::max(0.0, correction);

		const double meanB = static_cast<double>(sumB) / sampledPixels;
		const double meanG = static_cast<double>(sumG) / sampledPixels;
		const double meanR = static_cast<double>(sumR) / sampledPixels;
		const double middleGrey = (meanB + meanG + meanR) / (255.0 * 3.0);
		const double blueCast = meanB / 255.0 - middleGrey;
		const double greenCast = meanG / 255.0 - middleGrey;
		const double redCast = meanR / 255.0 - middleGrey;
		const auto colorCastCorrection = [](int channel, double cast) {
			static constexpr double strengths[6] = {0.2, 0.1, 0.3, 0.3, 0.3, 0.15};
			if (channel == 0) return cast * strengths[cast > 0.0 ? 2 : 5];
			if (channel == 1) return cast * strengths[cast > 0.0 ? 1 : 4];
			return cast * strengths[cast > 0.0 ? 0 : 3];
		};
		const double castB = colorCastCorrection(0, blueCast);
		const double castG = colorCastCorrection(1, greenCast);
		const double castR = colorCastCorrection(2, redCast);
		const double castMean = (castB + castG + castR) / 3.0;
		const double maxCast = std::max(std::abs(castB - castMean),
			std::max(std::abs(castG - castMean), std::abs(castR - castMean)));
		const double castFactor = 0.05 / std::max(0.00001, maxCast);
		constexpr double colorCorrectionFactor = 0.0; // Linux currently uses JPEGView's default.
		const double castMultiplier = 2.0 * (castFactor + 1.0) * colorCorrectionFactor + 1.0;
		const double yMean = correctionA * midPoint + correctionB;
		const auto bwCompensation = [midPoint](double black, double white) {
			const double a = 1.0 / std::max(0.001, white - black);
			return a * midPoint - black * a;
		};
		double correctionBlue = yMean - bwCompensation(blackB, whiteB) - castMultiplier * castB + 0.3 * 0.0;
		double correctionGreen = yMean - bwCompensation(blackG, whiteG) - castMultiplier * castG + 0.3 * 0.0;
		double correctionRed = yMean - bwCompensation(blackR, whiteR) - castMultiplier * castR + 0.3 * 0.0;
		const double meanGrey = (meanB * 0.1 + meanG * 0.6 + meanR * 0.3) / 255.0;
		const double brightnessAdd = meanGrey < 0.5 ? (0.5 - meanGrey) * brightnessStrength : 0.0;
		correction = (correction - correctionBlue) * 0.1 + (correction - correctionGreen) * 0.6 +
			(correction - correctionRed) * 0.3 + brightnessAdd;

		const auto calculateLut = [](double black, double white, double brightness, double mid) {
			std::array<std::uint8_t, 256> lut{};
			if (white - black < 0.001) {
				black = 0.0;
				white = 1.0;
			}
			const double a = 1.0 / (white - black);
			const double b = -black * a;
			for (int index = 0; index < 256; ++index) {
				const double x = index / 255.0;
				const double y = a * x + b;
				double target = y;
				if (y > 0.0 && y < 1.0) {
					if (x < mid) {
						const double value = (x - mid) / std::max(0.001, mid - black);
						target = y + brightness * (1.0 - value * value);
					} else {
						const double value = (x - mid) / std::max(0.001, white - mid);
						target = y + brightness * (1.0 - value * value);
					}
				}
				lut[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(std::lround(
					std::clamp(target, 0.0, 1.0) * 255.0));
			}
			return lut;
		};
		const std::array<std::uint8_t, 256> lutB = calculateLut(blackB, whiteB, correctionBlue + correction, midPoint);
		const std::array<std::uint8_t, 256> lutG = calculateLut(blackG, whiteG, correctionGreen + correction, midPoint);
		const std::array<std::uint8_t, 256> lutR = calculateLut(blackR, whiteR, correctionRed + correction, midPoint);
		for (std::size_t offset = 0; offset < bgra.size(); offset += 4) {
			bgra[offset] = lutB[bgra[offset]];
			bgra[offset + 1] = lutG[bgra[offset + 1]];
			bgra[offset + 2] = lutR[bgra[offset + 2]];
		}
		return true;
	}
};

std::string InfoText(const std::string& value) {
	std::string result;
	result.reserve(value.size());
	for (const unsigned char character : value) {
		result.push_back(character >= 32 && character < 127 ? static_cast<char>(character) : '?');
	}
	return result;
}

std::string FormatFileTime(const fs::path& filename) {
	struct stat status{};
	if (stat(filename.c_str(), &status) != 0) return {};
	std::tm localTime{};
	if (localtime_r(&status.st_mtime, &localTime) == nullptr) return {};
	char formatted[32]{};
	if (std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &localTime) == 0) return {};
	return formatted;
}

std::time_t FileModificationTime(const fs::path& filename) {
	struct stat status{};
	if (stat(filename.c_str(), &status) != 0) return 0;
	return status.st_mtime;
}

std::string FormatBatchDate(std::time_t timestamp) {
	if (timestamp == 0) return {};
	std::tm localTime{};
	if (localtime_r(&timestamp, &localTime) == nullptr) return {};
	char formatted[32]{};
	if (std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &localTime) == 0) return {};
	return formatted;
}

void ReplaceAll(std::string& value, const std::string& from, const std::string& to) {
	if (from.empty()) return;
	std::size_t position = 0;
	while ((position = value.find(from, position)) != std::string::npos) {
		value.replace(position, from.size(), to);
		position += to.size();
	}
}

std::string FirstNumber(const std::string& filename) {
	std::size_t start = std::string::npos;
	for (std::size_t index = 0; index < filename.size(); ++index) {
		if (std::isdigit(static_cast<unsigned char>(filename[index]))) {
			if (start == std::string::npos) start = index;
		} else if (start != std::string::npos) {
			return filename.substr(start, index - start);
		}
	}
	return start == std::string::npos ? std::string() : filename.substr(start);
}

fs::path PicturesDirectory() {
	if (const char* pictures = std::getenv("XDG_PICTURES_DIR"); pictures != nullptr && *pictures != '\0') {
		return AbsoluteNormalized(fs::path(pictures));
	}
	if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
		return AbsoluteNormalized(fs::path(home) / "Pictures");
	}
	return {};
}

std::string ExpandBatchPattern(const std::string& pattern, std::size_t selectedIndex,
	const fs::path& source, std::time_t modificationTime) {
	std::string result = pattern;
	const std::string title = source.filename().string();
	const std::string stem = source.stem().string();
	const std::string extension = source.extension().string();
	const std::string number = FirstNumber(title);
	ReplaceAll(result, "%x", std::to_string(selectedIndex + 1));
	ReplaceAll(result, "%n", number);
	for (int digits = 2; digits <= 9; ++digits) {
		std::ostringstream formatted;
		formatted << std::setw(digits) << std::setfill('0') << selectedIndex + 1;
		ReplaceAll(result, "%" + std::to_string(digits) + "x", formatted.str());
	}
	ReplaceAll(result, "%f", title);
	ReplaceAll(result, "%F", stem);
	ReplaceAll(result, "%e", extension.empty() ? std::string() : extension.substr(1));

	std::tm localTime{};
	if (modificationTime != 0) localtime_r(&modificationTime, &localTime);
	char datePart[32]{};
	if (modificationTime != 0) {
		std::strftime(datePart, sizeof(datePart), "%H", &localTime);
		ReplaceAll(result, "%h", datePart);
		std::strftime(datePart, sizeof(datePart), "%M", &localTime);
		ReplaceAll(result, "%min", datePart);
		std::strftime(datePart, sizeof(datePart), "%d", &localTime);
		ReplaceAll(result, "%d", datePart);
		std::strftime(datePart, sizeof(datePart), "%m", &localTime);
		ReplaceAll(result, "%m", datePart);
		std::strftime(datePart, sizeof(datePart), "%Y", &localTime);
		ReplaceAll(result, "%y", datePart);
		std::strftime(datePart, sizeof(datePart), "%y", &localTime);
		ReplaceAll(result, "%2y", datePart);
		std::strftime(datePart, sizeof(datePart), "%B", &localTime);
		ReplaceAll(result, "%3M", std::string(datePart).substr(0, 3));
		ReplaceAll(result, "%M", datePart);
	}
	const fs::path pictures = PicturesDirectory();
	if (!pictures.empty()) ReplaceAll(result, "%pictures%", pictures.string());
	// Windows JPEGView patterns use backslashes for subdirectories. Accept
	// those templates on Linux while still allowing the native '/' separator.
	std::replace(result.begin(), result.end(), '\\', '/');
	return result;
}

std::string FormatFileSize(std::uintmax_t size) {
	static constexpr const char* suffixes[] = {"B", "KB", "MB", "GB"};
	double value = static_cast<double>(size);
	std::size_t suffix = 0;
	while (value >= 1024.0 && suffix + 1 < std::size(suffixes)) {
		value /= 1024.0;
		++suffix;
	}
	std::ostringstream stream;
	if (suffix == 0) {
		stream << size << ' ' << suffixes[suffix];
	} else {
		stream << std::fixed << std::setprecision(value >= 10.0 ? 0 : 1) << value << ' ' << suffixes[suffix];
	}
	return stream.str();
}

bool HasExecutable(const std::string& executable) {
	const char* path = std::getenv("PATH");
	if (path == nullptr) return false;
	const std::string searchPath(path);
	std::size_t begin = 0;
	while (begin <= searchPath.size()) {
		const std::size_t end = searchPath.find(':', begin);
		const fs::path directory = searchPath.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		const fs::path candidate = (directory.empty() ? fs::path(".") : directory) / executable;
		if (access(candidate.c_str(), X_OK) == 0) return true;
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return false;
}

std::string TrimDesktopValue(const std::string& value) {
	const std::size_t first = value.find_first_not_of(" \t\r");
	if (first == std::string::npos) return {};
	const std::size_t last = value.find_last_not_of(" \t\r");
	return value.substr(first, last - first + 1);
}

std::string UnescapeDesktopValue(const std::string& value) {
	std::string result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (value[index] != '\\' || index + 1 >= value.size()) {
			result += value[index];
			continue;
		}
		const char escaped = value[++index];
		switch (escaped) {
		case 'n': result += '\n'; break;
		case 'r': result += '\r'; break;
		case 's': result += ' '; break;
		case 't': result += '\t'; break;
		case '\\': result += '\\'; break;
		default: result += escaped; break;
		}
	}
	return result;
}

bool DesktopBoolean(const std::string& value) {
	return Lower(TrimDesktopValue(value)) == "true";
}

std::string DesktopEntryField(const std::string& line, const std::string& key) {
	const std::string prefix = key + "=";
	if (line.compare(0, prefix.size(), prefix) != 0) return {};
	return UnescapeDesktopValue(TrimDesktopValue(line.substr(prefix.size())));
}

bool MimeTypeMatches(const std::string& mimeTypes, const std::string& currentMime) {
	if (mimeTypes.empty() || currentMime.empty()) return false;
	std::size_t begin = 0;
	while (begin <= mimeTypes.size()) {
		const std::size_t end = mimeTypes.find(';', begin);
		const std::string mime = Lower(TrimDesktopValue(mimeTypes.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin)));
		const bool compatibleAlias =
			(mime == "image/x-ms-bmp" && currentMime == "image/bmp") ||
			(mime == "image/bmp" && currentMime == "image/x-ms-bmp") ||
			(mime == "image/targa" && currentMime == "image/x-tga") ||
			(mime == "image/x-tga" && currentMime == "image/targa");
		if (mime == currentMime || compatibleAlias || mime == "*/*" ||
			(mime.size() > 2 && mime.back() == '*' && mime[mime.size() - 2] == '/' &&
			currentMime.compare(0, mime.size() - 1, mime, 0, mime.size() - 1) == 0)) {
			return true;
		}
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return false;
}

std::string MimeTypeForExtension(const std::string& extension) {
	if (extension == ".jpg" || extension == ".jpeg" || extension == ".jpe") return "image/jpeg";
	if (extension == ".png") return "image/png";
	if (extension == ".gif") return "image/gif";
	if (extension == ".bmp") return "image/bmp";
	if (extension == ".tga") return "image/x-tga";
	if (extension == ".psd") return "image/vnd.adobe.photoshop";
	if (extension == ".ppm") return "image/x-portable-pixmap";
	if (extension == ".pgm") return "image/x-portable-graymap";
	if (extension == ".pnm") return "image/x-portable-anymap";
	if (extension == ".webp") return "image/webp";
	return {};
}

std::vector<fs::path> DesktopApplicationDirectories() {
	std::vector<fs::path> directories;
	std::set<std::string> seen;
	const auto addDirectory = [&directories, &seen](const fs::path& directory) {
		if (directory.empty()) return;
		const std::string key = directory.lexically_normal().string();
		if (seen.insert(key).second) directories.push_back(directory);
	};

	if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && *dataHome != '\0') {
		addDirectory(fs::path(dataHome) / "applications");
	} else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
		addDirectory(fs::path(home) / ".local" / "share" / "applications");
	}

	const char* dataDirectories = std::getenv("XDG_DATA_DIRS");
	const std::string searchPath = dataDirectories != nullptr && *dataDirectories != '\0' ?
		std::string(dataDirectories) : "/usr/local/share:/usr/share";
	std::size_t begin = 0;
	while (begin <= searchPath.size()) {
		const std::size_t end = searchPath.find(':', begin);
		const fs::path directory = searchPath.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		if (!directory.empty()) addDirectory(directory / "applications");
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return directories;
}

bool ReadDesktopApplication(const fs::path& filename, const std::string& currentMime,
	OpenWithApplication& application) {
	std::ifstream input(filename);
	if (!input) return false;

	bool inDesktopEntry = false;
	std::string type;
	std::string name;
	std::string exec;
	std::string tryExec;
	std::string mimeTypes;
	bool hidden = false;
	bool noDisplay = false;
	bool terminal = false;
	std::string line;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line == "[Desktop Entry]") {
			inDesktopEntry = true;
			continue;
		}
		if (!line.empty() && line.front() == '[') {
			inDesktopEntry = false;
			continue;
		}
		if (!inDesktopEntry || line.empty() || line.front() == '#') continue;
		if (line.compare(0, 5, "Type=") == 0) type = DesktopEntryField(line, "Type");
		else if (line.compare(0, 5, "Name=") == 0) name = DesktopEntryField(line, "Name");
		else if (line.compare(0, 5, "Exec=") == 0) exec = DesktopEntryField(line, "Exec");
		else if (line.compare(0, 8, "TryExec=") == 0) tryExec = DesktopEntryField(line, "TryExec");
		else if (line.compare(0, 9, "MimeType=") == 0) mimeTypes = DesktopEntryField(line, "MimeType");
		else if (line.compare(0, 7, "Hidden=") == 0) hidden = DesktopBoolean(DesktopEntryField(line, "Hidden"));
		else if (line.compare(0, 9, "NoDisplay=") == 0) noDisplay = DesktopBoolean(DesktopEntryField(line, "NoDisplay"));
		else if (line.compare(0, 9, "Terminal=") == 0) terminal = DesktopBoolean(DesktopEntryField(line, "Terminal"));
	}

	if (type != "Application" || name.empty() || exec.empty() || hidden || noDisplay ||
		!MimeTypeMatches(mimeTypes, currentMime)) return false;
	if (!tryExec.empty() && !HasExecutable(tryExec)) return false;
	application = OpenWithApplication{name, exec, filename, terminal};
	return true;
}

std::vector<OpenWithApplication> DiscoverOpenWithApplications(const std::string& extension) {
	std::vector<OpenWithApplication> applications;
	const std::string currentMime = MimeTypeForExtension(extension);
	std::set<std::string> seenDesktopIds;
	for (const fs::path& directory : DesktopApplicationDirectories()) {
		std::error_code iteratorError;
		for (const fs::directory_entry& entry : fs::directory_iterator(directory, iteratorError)) {
			if (iteratorError) break;
			std::error_code fileError;
			if (!entry.is_regular_file(fileError) || fileError ||
				Lower(entry.path().extension().string()) != ".desktop") continue;
			const std::string desktopId = entry.path().filename().string();
			if (!seenDesktopIds.insert(desktopId).second) continue;
			OpenWithApplication application;
			if (ReadDesktopApplication(entry.path(), currentMime, application)) {
				applications.push_back(std::move(application));
			}
		}
	}

	std::sort(applications.begin(), applications.end(), [](const OpenWithApplication& left,
		const OpenWithApplication& right) {
		const std::string leftName = Lower(left.name);
		const std::string rightName = Lower(right.name);
		if (leftName != rightName) return leftName < rightName;
		return left.desktopFile.string() < right.desktopFile.string();
	});
	if (applications.size() > static_cast<std::size_t>(IDM_LAST_OPENWITH_CMD - IDM_FIRST_OPENWITH_CMD + 1)) {
		applications.resize(static_cast<std::size_t>(IDM_LAST_OPENWITH_CMD - IDM_FIRST_OPENWITH_CMD + 1));
	}
	return applications;
}

[[noreturn]] void ExecProcess(const std::string& executable, const std::vector<std::string>& arguments) {
	std::vector<char*> argv;
	argv.reserve(arguments.size() + 2);
	argv.push_back(const_cast<char*>(executable.c_str()));
	for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
	argv.push_back(nullptr);
	execvp(executable.c_str(), argv.data());
	_exit(127);
}

bool RunProcess(const std::string& executable, const std::vector<std::string>& arguments,
	std::string& errorMessage) {
	if (!HasExecutable(executable)) {
		errorMessage = executable + " is not installed";
		return false;
	}
	const pid_t child = fork();
	if (child < 0) {
		errorMessage = "cannot start " + executable;
		return false;
	}
	if (child == 0) ExecProcess(executable, arguments);
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errorMessage = executable + " failed";
		return false;
	}
	return true;
}

bool StartDetachedProcess(const std::string& executable, const std::vector<std::string>& arguments,
	std::string& errorMessage) {
	if (!HasExecutable(executable)) {
		errorMessage = executable + " is not installed";
		return false;
	}
	const pid_t child = fork();
	if (child < 0) {
		errorMessage = "cannot start " + executable;
		return false;
	}
	if (child == 0) {
		const pid_t detached = fork();
		if (detached < 0) _exit(127);
		if (detached > 0) _exit(0);
		setsid();
		ExecProcess(executable, arguments);
	}
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errorMessage = executable + " failed to start";
		return false;
	}
	return true;
}

std::string FileUri(const fs::path& filename) {
	const std::string path = AbsoluteNormalized(filename).string();
	std::ostringstream uri;
	uri << "file://";
	for (const unsigned char character : path) {
		if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == '-' || character == '_' ||
			character == '.' || character == '/' || character == '~') {
			uri << static_cast<char>(character);
		} else {
			static constexpr char hex[] = "0123456789ABCDEF";
			uri << '%' << hex[character >> 4] << hex[character & 15];
		}
	}
	return uri.str();
}

std::vector<std::string> TokenizeDesktopExec(const std::string& commandLine) {
	std::vector<std::string> tokens;
	std::string token;
	char quote = '\0';
	bool escaped = false;
	for (const char character : commandLine) {
		if (escaped) {
			token += character;
			escaped = false;
			continue;
		}
		if (quote != '\0') {
			if (character == quote) {
				quote = '\0';
			} else if (character == '\\') {
				escaped = true;
			} else {
				token += character;
			}
			continue;
		}
		if (character == '\'' || character == '"') {
			quote = character;
		} else if (character == '\\') {
			escaped = true;
		} else if (std::isspace(static_cast<unsigned char>(character))) {
			if (!token.empty()) {
				tokens.push_back(std::move(token));
				token.clear();
			}
		} else {
			token += character;
		}
	}
	if (escaped) token += '\\';
	if (quote != '\0' || !token.empty()) {
		if (!token.empty()) tokens.push_back(std::move(token));
	}
	return tokens;
}

bool ExpandDesktopExecToken(const std::string& token, const OpenWithApplication& application,
	const fs::path& imageFilename, std::string& expanded, bool& hasFilePlaceholder) {
	const std::string imagePath = AbsoluteNormalized(imageFilename).string();
	const std::string imageUri = FileUri(imageFilename);
	const std::string imageDirectory = AbsoluteNormalized(imageFilename.parent_path()).string();
	const std::string imageName = imageFilename.filename().string();
	expanded.clear();
	for (std::size_t index = 0; index < token.size(); ++index) {
		if (token[index] != '%' || index + 1 >= token.size()) {
			expanded += token[index];
			continue;
		}
		const char field = token[++index];
		switch (field) {
		case '%': expanded += '%'; break;
		case 'f':
		case 'F':
			expanded += imagePath;
			hasFilePlaceholder = true;
			break;
		case 'u':
		case 'U':
			expanded += imageUri;
			hasFilePlaceholder = true;
			break;
		case 'd':
		case 'D': expanded += imageDirectory; break;
		case 'n':
		case 'N': expanded += imageName; break;
		case 'c': expanded += application.name; break;
		case 'k': expanded += application.desktopFile.string(); break;
		case 'i':
		case 'm':
			// The desktop entry's icon is not needed to launch the application;
			// omitting the field is the useful equivalent on Linux.
			return false;
		default:
			// Unknown field codes are invalid in a desktop Exec key.  Do not
			// accidentally pass the code as an argument to an application.
			return false;
		}
	}
	return true;
}

std::vector<std::string> DesktopExecArguments(const OpenWithApplication& application,
	const fs::path& imageFilename) {
	const std::vector<std::string> tokens = TokenizeDesktopExec(application.exec);
	if (tokens.empty()) return {};

	std::vector<std::string> arguments;
	arguments.reserve(tokens.size() + 1);
	bool hasFilePlaceholder = false;
	for (const std::string& token : tokens) {
		std::string expanded;
		if (!ExpandDesktopExecToken(token, application, imageFilename, expanded, hasFilePlaceholder)) continue;
		if (!expanded.empty()) arguments.push_back(std::move(expanded));
	}
	if (arguments.empty()) return {};
	if (!hasFilePlaceholder) arguments.push_back(AbsoluteNormalized(imageFilename).string());
	return arguments;
}

fs::path ScaleSettingsPath() {
	if (const char* configHome = std::getenv("XDG_CONFIG_HOME"); configHome != nullptr && *configHome != '\0') {
		return fs::path(configHome) / "jpegview-linux" / "settings.conf";
	}
	if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
		return fs::path(home) / ".config" / "jpegview-linux" / "settings.conf";
	}
	return {};
}

class Viewer {
public:
	Viewer(jpegview_linux::FileList fileList, double slideshowSeconds, bool startFullscreen)
		: fileList_(std::move(fileList)), slideshowSeconds_(slideshowSeconds),
		  lastSlideshowSeconds_(slideshowSeconds > 0.0 ? slideshowSeconds : 3.0), startFullscreen_(startFullscreen) {}

	int Run() {
		LoadSettings();
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
			return 1;
		}

		// Keep the window hidden while SDL and the window manager apply the
		// initial state.  Showing it first makes a restored maximized window
		// visibly appear in its normal size before it is maximized.
		Uint32 windowFlags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
		if (maximized_ && !startFullscreen_) windowFlags |= SDL_WINDOW_MAXIMIZED;
		window_ = SDL_CreateWindow("JPEGView Linux", 0x2FFF0000, 0x2FFF0000,
			kDefaultWidth, kDefaultHeight, windowFlags);
		if (window_ == nullptr) {
			std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n';
			SDL_Quit();
			return 1;
		}
		SDL_SetHint("SDL_RENDER_SCALE_QUALITY", "2");
		renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (renderer_ == nullptr) {
			// This is useful for software-only systems and also makes the viewer
			// testable with SDL_VIDEODRIVER=dummy in CI.
			renderer_ = SDL_CreateRenderer(window_, -1, 0);
			if (renderer_ == nullptr) {
				std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << '\n';
				SDL_DestroyWindow(window_);
				SDL_Quit();
				return 1;
			}
		}

		if (startFullscreen_) {
			fullscreen_ = true;
			SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
		} else if (maximized_) {
			// The creation flag handles backends that can apply the state before
			// mapping; this call covers backends that require an explicit request.
			SDL_MaximizeWindow(window_);
		}

		if (!LoadCurrent()) {
			Cleanup();
			return 1;
		}
		ShowControls();
		SDL_ShowWindow(window_);

		bool running = true;
		while (running) {
			HandleEvents(running);
			if (quitRequested_) running = false;
			TickPlayback();
			Render();
			SDL_Delay(4);
		}

		Cleanup();
		return 0;
	}

private:
	enum class PlaybackMode {
		None,
		Slideshow,
		Movie,
	};

	void Cleanup() {
		SaveSettings();
		if (clipboardMode_) {
			std::error_code removeError;
			fs::remove(clipboardTempFile_, removeError);
			if (!clipboardTempDirectory_.empty()) fs::remove(clipboardTempDirectory_, removeError);
		}
		if (texture_ != nullptr) {
			SDL_DestroyTexture(texture_);
			texture_ = nullptr;
		}
		ClearDisplayTexture();
		ClearTransition();
		if (renderer_ != nullptr) {
			SDL_DestroyRenderer(renderer_);
			renderer_ = nullptr;
		}
		if (window_ != nullptr) {
			SDL_DestroyWindow(window_);
			window_ = nullptr;
		}
		SDL_Quit();
	}

	void LoadSettings() {
		const fs::path settingsPath = ScaleSettingsPath();
		if (settingsPath.empty()) return;

		std::ifstream input(settingsPath);
		if (!input) return;

		std::string scaleMode;
		std::string copyRenamePattern;
		double manualZoom = zoom_;
		bool hasManualZoom = false;
		std::string line;
		while (std::getline(input, line)) {
			if (line.empty() || line[0] == '#') continue;
			const std::size_t separator = line.find('=');
			if (separator == std::string::npos) continue;
			std::string key = line.substr(0, separator);
			std::string value = line.substr(separator + 1);
			const auto trim = [](std::string text) {
				const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char character) {
					return std::isspace(character) != 0;
				});
				const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char character) {
					return std::isspace(character) != 0;
				}).base();
				if (first >= last) return std::string{};
				return std::string(first, last);
			};
			key = trim(std::move(key));
			value = trim(std::move(value));
			if (key == "scale_mode") {
				scaleMode = value;
			} else if (key == "copy_rename_pattern") {
				copyRenamePattern = value;
			} else if (key == "manual_zoom") {
				try {
					std::size_t parsedCharacters = 0;
					const double parsedZoom = std::stod(value, &parsedCharacters);
					if (parsedCharacters == value.size() && std::isfinite(parsedZoom)) {
						manualZoom = std::clamp(parsedZoom, kMinZoom, kMaxZoom);
						hasManualZoom = true;
					}
				} catch (const std::exception&) {
					// Ignore malformed settings and retain the built-in default.
				}
			} else if (key == "maximized") {
				maximized_ = value == "1" || value == "true";
			} else if (key == "navigation_panel_enabled") {
				navigationPanelEnabled_ = value == "1" || value == "true";
			} else if (key == "auto_contrast") {
				autoContrastEnabled_ = value == "1" || value == "true";
			}
		}
		copyRenamePattern_ = copyRenamePattern;

		if (scaleMode == "fit") {
			fitToWindow_ = true;
			fillWithCrop_ = false;
			autoZoomNoEnlarge_ = false;
		} else if (scaleMode == "fill") {
			fitToWindow_ = true;
			fillWithCrop_ = true;
			autoZoomNoEnlarge_ = false;
		} else if (scaleMode == "fit_no_enlarge") {
			fitToWindow_ = true;
			fillWithCrop_ = false;
			autoZoomNoEnlarge_ = true;
		} else if (scaleMode == "fill_no_enlarge") {
			fitToWindow_ = true;
			fillWithCrop_ = true;
			autoZoomNoEnlarge_ = true;
		} else if (scaleMode == "manual") {
			fitToWindow_ = false;
			fillWithCrop_ = false;
			autoZoomNoEnlarge_ = false;
			if (hasManualZoom) zoom_ = manualZoom;
		}
	}

	const char* CurrentScaleMode() const {
		if (!fitToWindow_) return "manual";
		if (fillWithCrop_ && autoZoomNoEnlarge_) return "fill_no_enlarge";
		if (fillWithCrop_) return "fill";
		if (autoZoomNoEnlarge_) return "fit_no_enlarge";
		return "fit";
	}

	void SaveSettings() const {
		const fs::path settingsPath = ScaleSettingsPath();
		if (settingsPath.empty()) return;

		std::error_code error;
		fs::create_directories(settingsPath.parent_path(), error);
		if (error) return;

		fs::path temporaryPath = settingsPath;
		temporaryPath += ".tmp";
		bool lastMaximized = maximized_;
		{
			std::ofstream output(temporaryPath, std::ios::trunc);
			if (!output) return;
			output << "# JPEGView Linux display and batch-operation settings\n"
			       << "scale_mode=" << CurrentScaleMode() << '\n'
			       << std::setprecision(17) << "manual_zoom=" << zoom_ << '\n'
			       << "maximized=" << (lastMaximized ? 1 : 0) << '\n'
			       << "navigation_panel_enabled=" << (navigationPanelEnabled_ ? 1 : 0) << '\n'
			       << "auto_contrast=" << (autoContrastEnabled_ ? 1 : 0) << '\n'
			       << "copy_rename_pattern=" << copyRenamePattern_ << '\n';
			if (!output) {
				output.close();
				fs::remove(temporaryPath, error);
				return;
			}
		}

		fs::rename(temporaryPath, settingsPath, error);
		if (error) fs::remove(temporaryPath, error);
	}

	bool LoadCurrent() {
		if (fileList_.Empty()) {
			return false;
		}
		const bool wasFitToWindow = fitToWindow_;
		const bool wasFillWithCrop = fillWithCrop_;
		const bool wasAutoZoomNoEnlarge = autoZoomNoEnlarge_;
		const double manualZoom = zoom_;
		metadata_ = {};
		jpegComment_.clear();
		ClearTransition();
		std::string errorMessage;
		jpegview_linux::DecodedImage decoded;
		if (!jpegview_linux::DecodeImage(fileList_.Current(), decoded, errorMessage) || decoded.frames.empty()) {
			SetTitle(fileList_.Current().filename().string() + " — decode failed: " + errorMessage);
			std::cerr << fileList_.Current() << ": " << errorMessage << '\n';
			return false;
		}
		std::vector<Image> decodedFrames;
		decodedFrames.reserve(decoded.frames.size());
		for (const jpegview_linux::DecodedFrame& decodedFrame : decoded.frames) {
			Image frame;
			if (!frame.StoreBGRA(decodedFrame.bgra.data(), decodedFrame.width, decodedFrame.height)) {
				SetTitle(fileList_.Current().filename().string() + " — image is too large");
				std::cerr << fileList_.Current() << ": image is too large\n";
				return false;
			}
			decodedFrames.push_back(std::move(frame));
		}
		animationFrames_ = std::move(decodedFrames);
		animationFrameDelaysMs_.clear();
		animationFrameDelaysMs_.reserve(decoded.frames.size());
		for (const jpegview_linux::DecodedFrame& decodedFrame : decoded.frames) {
			animationFrameDelaysMs_.push_back(std::max(10, decodedFrame.delayMs));
		}
		animationFrameIndex_ = 0;
		animationLoopCount_ = decoded.loopCount;
		animationLoopsCompleted_ = 0;
		image_ = animationFrames_.front();
		correctionBase_ = image_;
		correctionBaseValid_ = true;
		if (autoContrastEnabled_ && !image_.AutoContrast()) {
			SetTitle(fileList_.Current().filename().string() + " — automatic correction failed");
			return false;
		}
		jpegview_linux::ReadJpegMetadata(fileList_.Current(), metadata_, jpegComment_);

		if (texture_ != nullptr) {
			SDL_DestroyTexture(texture_);
		}
		texture_ = nullptr;
		if (!UpdateTexture()) return false;

		RestoreScaleMode(wasFitToWindow, wasFillWithCrop, wasAutoZoomNoEnlarge, manualZoom);
		const Uint32 now = SDL_GetTicks();
		lastInteractionTick_ = now;
		imageModified_ = false;
		animationPlaying_ = decoded.animation && animationFrames_.size() > 1 &&
			playbackMode_ != PlaybackMode::Slideshow;
		if (animationPlaying_) {
			ScheduleAnimation(now);
		} else if (playbackMode_ == PlaybackMode::Movie) {
			nextPlaybackTick_ = now + MovieFrameInterval();
		} else {
			nextPlaybackTick_ = 0;
		}
		SetTitle();
		return true;
	}

	bool UpdateTexture() {
		SDL_Texture* newTexture = CreateTexture(image_);
		if (newTexture == nullptr) {
			std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << '\n';
			return false;
		}
		ClearDisplayTexture();
		if (texture_ != nullptr) SDL_DestroyTexture(texture_);
		texture_ = newTexture;
		return true;
	}

	SDL_Texture* CreateTexture(const Image& source) {
		SDL_Texture* result = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
			source.width, source.height);
		if (result == nullptr) return nullptr;
		if (SDL_UpdateTexture(result, nullptr, source.bgra.data(), source.width * 4) != 0) {
			std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << '\n';
			SDL_DestroyTexture(result);
			return nullptr;
		}
		return result;
	}

	void ClearDisplayTexture() {
		if (displayTexture_ != nullptr) {
			SDL_DestroyTexture(displayTexture_);
			displayTexture_ = nullptr;
		}
		displayTextureWidth_ = 0;
		displayTextureHeight_ = 0;
		displayImage_ = {};
	}

	SDL_Texture* DisplayTextureFor(int width, int height) {
		if (texture_ == nullptr || image_.width <= 0 || image_.height <= 0) return texture_;
		if (width >= image_.width && height >= image_.height) return texture_;
		if (displayTexture_ != nullptr && displayTextureWidth_ == width && displayTextureHeight_ == height) {
			return displayTexture_;
		}

		ClearDisplayTexture();
		displayImage_ = image_;
		if (!displayImage_.Resize(width, height)) {
			displayImage_ = {};
			return texture_;
		}
		displayTexture_ = CreateTexture(displayImage_);
		if (displayTexture_ == nullptr) {
			displayImage_ = {};
			return texture_;
		}
		displayTextureWidth_ = width;
		displayTextureHeight_ = height;
		return displayTexture_;
	}

	void ClearTransition() {
		if (transitionTexture_ != nullptr) {
			SDL_DestroyTexture(transitionTexture_);
			transitionTexture_ = nullptr;
		}
		transitionImage_ = {};
		transitionStartTick_ = 0;
	}

	void StartTransition(const Image& previousImage) {
		ClearTransition();
		if (transitionEffect_ == IDM_EFFECT_NONE || previousImage.width <= 0 || previousImage.height <= 0) return;
		transitionImage_ = previousImage;
		transitionTexture_ = CreateTexture(transitionImage_);
		if (transitionTexture_ == nullptr) {
			transitionImage_ = {};
			return;
		}
		SDL_SetTextureBlendMode(transitionTexture_, SDL_BLENDMODE_BLEND);
		SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
		transitionStartTick_ = SDL_GetTicks();
	}

	bool TransformImage(Image& image, int command) {
		switch (command) {
		case IDM_ROTATE_90: return image.Rotate(true);
		case IDM_ROTATE_270: return image.Rotate(false);
		case IDM_MIRROR_H: return image.Mirror(true);
		case IDM_MIRROR_V: return image.Mirror(false);
		default: return false;
		}
	}

	void ApplyTransform(int command) {
		const bool wasFitToWindow = fitToWindow_;
		const bool wasFillWithCrop = fillWithCrop_;
		const bool wasAutoZoomNoEnlarge = autoZoomNoEnlarge_;
		const double manualZoom = zoom_;
		if (!TransformImage(image_, command) ||
			(correctionBaseValid_ && !TransformImage(correctionBase_, command)) || !UpdateTexture()) {
			SetTitle("Image transform failed");
			return;
		}
		imageModified_ = true;
		RestoreScaleMode(wasFitToWindow, wasFillWithCrop, wasAutoZoomNoEnlarge, manualZoom);
	}

	void RebuildAutoContrastImage(bool wasFitToWindow, bool wasFillWithCrop,
		bool wasAutoZoomNoEnlarge, double manualZoom) {
		if (!correctionBaseValid_) return;
		image_ = correctionBase_;
		if (autoContrastEnabled_) image_.AutoContrast();
		if (!UpdateTexture()) {
			SetTitle("Automatic correction failed: could not update the display texture");
			return;
		}
		RestoreScaleMode(wasFitToWindow, wasFillWithCrop, wasAutoZoomNoEnlarge, manualZoom);
		SetTitle();
	}

	void ToggleAutoContrast() {
		if (!correctionBaseValid_) return;
		const bool wasFitToWindow = fitToWindow_;
		const bool wasFillWithCrop = fillWithCrop_;
		const bool wasAutoZoomNoEnlarge = autoZoomNoEnlarge_;
		const double manualZoom = zoom_;
		autoContrastEnabled_ = !autoContrastEnabled_;
		RebuildAutoContrastImage(wasFitToWindow, wasFillWithCrop, wasAutoZoomNoEnlarge, manualZoom);
		SaveSettings();
	}

	void ApplyLosslessJpegTransform(int command) {
		if (fileList_.Empty() || clipboardMode_) return;
		const std::string extension = Lower(fileList_.Current().extension().string());
		if (extension != ".jpg" && extension != ".jpeg" && extension != ".jpe") {
			SetTitle("Lossless JPEG transformation requires a JPEG image");
			return;
		}
		std::string operation;
		switch (command) {
		case IDM_ROTATE_90_LOSSLESS:
		case IDM_ROTATE_90_LOSSLESS_CONFIRM:
			operation = "90";
			break;
		case IDM_ROTATE_270_LOSSLESS:
		case IDM_ROTATE_270_LOSSLESS_CONFIRM:
			operation = "270";
			break;
		case IDM_ROTATE_180_LOSSLESS:
			operation = "180";
			break;
		default:
			break;
		}
		const bool horizontalFlip = command == IDM_MIRROR_H_LOSSLESS;
		const bool verticalFlip = command == IDM_MIRROR_V_LOSSLESS;
		if (operation.empty() && !horizontalFlip && !verticalFlip) return;
		if (!HasExecutable("jpegtran")) {
			SetTitle("Lossless JPEG transformation requires jpegtran");
			return;
		}

		char temporaryDirectoryName[] = "/tmp/jpegview-jpegtran-XXXXXX";
		if (mkdtemp(temporaryDirectoryName) == nullptr) {
			SetTitle("Lossless JPEG transformation failed: cannot create temporary file");
			return;
		}
		const fs::path temporaryDirectory(temporaryDirectoryName);
		const fs::path temporaryFile = temporaryDirectory / "transformed.jpg";
		std::vector<std::string> arguments{"-copy", "all"};
		if (!operation.empty()) {
			arguments.push_back("-rotate");
			arguments.push_back(operation);
		} else {
			arguments.push_back("-flip");
			arguments.push_back(horizontalFlip ? "horizontal" : "vertical");
		}
		arguments.push_back("-outfile");
		arguments.push_back(temporaryFile.string());
		arguments.push_back(fileList_.Current().string());
		std::string errorMessage;
		const bool transformed = RunProcess("jpegtran", arguments, errorMessage);
		if (!transformed) {
			std::error_code removeError;
			fs::remove_all(temporaryDirectory, removeError);
			SetTitle("Lossless JPEG transformation failed: " + errorMessage);
			return;
		}
		if (::rename(temporaryFile.c_str(), fileList_.Current().c_str()) != 0) {
			std::error_code removeError;
			fs::remove_all(temporaryDirectory, removeError);
			SetTitle("Lossless JPEG transformation failed: cannot replace original file");
			return;
		}
		std::error_code removeError;
		fs::remove_all(temporaryDirectory, removeError);
		ReloadAfterFileChange();
		SetTitle("Applied lossless JPEG transformation");
	}

	void SetTitle(const std::string& title) {
		SDL_SetWindowTitle(window_, title.c_str());
	}

	void SetTitle() {
		if (fileList_.Empty() || image_.originalWidth <= 0 || image_.originalHeight <= 0) {
			SetTitle("JPEGView");
			return;
		}
		std::error_code fileError;
		const std::uintmax_t fileSize = fs::file_size(fileList_.Current(), fileError);
		std::ostringstream title;
		title << fileList_.Current().filename().string()
			<< " (" << image_.originalWidth << 'x' << image_.originalHeight;
		if (!fileError) title << ", " << FormatFileSize(fileSize);
		title << ") - JPEGView";
		SetTitle(title.str());
	}

	void OpenDroppedFiles(const std::vector<std::string>& droppedFiles) {
		if (droppedFiles.empty()) {
			return;
		}
		RestoreClipboardImage();

		try {
			jpegview_linux::FileList droppedFileList(
				droppedFiles, fileList_.GetSorting(), fileList_.IsSortedAscending(), fileList_.WrapAroundFolder());
			// Construction performs the same initial-file/folder discovery as
			// the Windows CFileList constructor.
			if (droppedFileList.Empty()) {
				SetTitle("No supported images in dropped input");
				return;
			}

			const jpegview_linux::FileList::NavigationMode navigationMode = fileList_.GetNavigationMode();
			fileList_ = std::move(droppedFileList);
			fileList_.SetNavigationMode(navigationMode);
		} catch (const fs::filesystem_error& error) {
			SetTitle(std::string("Cannot open dropped input: ") + error.what());
			return;
		}
		dragging_ = false;
		CloseFileDialog();
		LoadCurrent();
	}

	void CloseFileDialog() {
		if (fileDialogSave_) SDL_StopTextInput();
		fileDialogOpen_ = false;
		fileDialogSave_ = false;
		fileDialogFilename_.clear();
	}

	void SaveImageFromDialog() {
		if (!fileDialogSave_ || fileDialogFilename_.empty()) return;
		fs::path filename(fileDialogFilename_);
		if (filename.extension().empty()) filename += ".jpg";
		const fs::path output = AbsoluteNormalized(fileDialogDirectory_ / filename);
		std::error_code existsError;
		if (fs::exists(output, existsError) && !existsError && !fileDialogOverwriteConfirmed_) {
			fileDialogOverwriteConfirmed_ = true;
			fileDialogMessage_ = "File exists; press ENTER to overwrite or ESC to cancel";
			return;
		}

		Image outputImage = image_;
		if (!fileDialogSaveFullSize_) {
			const int outputWidth = std::max(1, static_cast<int>(std::round(image_.width * zoom_)));
			const int outputHeight = std::max(1, static_cast<int>(std::round(image_.height * zoom_)));
			if (!outputImage.Resize(outputWidth, outputHeight)) {
				fileDialogMessage_ = "Cannot resize image for screen-size output";
				return;
			}
		}

		jpegview_linux::ImageWriteOptions options;
		std::string errorMessage;
		if (!jpegview_linux::WriteImage(output, outputImage.bgra.data(), outputImage.width,
			outputImage.height, options, errorMessage)) {
			fileDialogMessage_ = "Save failed: " + errorMessage;
			return;
		}

		const std::string savedName = output.filename().string();
		CloseFileDialog();
		SetTitle("Saved processed image: " + savedName);
	}

	void CopyCurrentImage(bool fullSize) {
		if (image_.width <= 0 || image_.height <= 0) return;
		Image copied = image_;
		if (!fullSize) {
			const int outputWidth = std::max(1, static_cast<int>(std::round(image_.width * zoom_)));
			const int outputHeight = std::max(1, static_cast<int>(std::round(image_.height * zoom_)));
			if (!copied.Resize(outputWidth, outputHeight)) {
				SetTitle("Copy failed: image is too large");
				return;
			}
		}
		std::string errorMessage;
		if (jpegview_linux::CopyImageToClipboard(copied.bgra.data(), copied.width, copied.height, errorMessage)) {
			SetTitle(fullSize ? "Copied original-size image to clipboard" : "Copied displayed image to clipboard");
		} else {
			SetTitle("Copy image failed: " + errorMessage);
		}
	}

	void CopyCurrentPath() {
		if (fileList_.Empty() || clipboardMode_) return;
		std::string errorMessage;
		if (jpegview_linux::CopyTextToClipboard(fileList_.Current().string(), errorMessage)) {
			SetTitle("Copied image path to clipboard");
		} else {
			SetTitle("Copy path failed: " + errorMessage);
		}
	}

	void OpenContainingFolder() {
		if (fileList_.Empty() || clipboardMode_) return;
		const fs::path directory = fileList_.Current().parent_path();
		std::string errorMessage;
		if (StartDetachedProcess("xdg-open", {directory.string()}, errorMessage) ||
			StartDetachedProcess("gio", {"open", directory.string()}, errorMessage)) {
			SetTitle("Opened containing folder");
		} else {
			SetTitle("Cannot open containing folder: " + errorMessage);
		}
	}

	void OpenCurrentWith(std::size_t applicationIndex) {
		if (fileList_.Empty() || clipboardMode_ || applicationIndex >= openWithApplications_.size()) return;
		const OpenWithApplication& application = openWithApplications_[applicationIndex];
		std::vector<std::string> command = DesktopExecArguments(application, fileList_.Current());
		if (command.empty()) {
			SetTitle("Cannot open image with " + application.name + ": invalid desktop command");
			return;
		}

		std::string errorMessage;
		bool started = false;
		if (application.terminal && HasExecutable("x-terminal-emulator")) {
			std::vector<std::string> terminalArguments{"-e"};
			terminalArguments.insert(terminalArguments.end(), command.begin(), command.end());
			started = StartDetachedProcess("x-terminal-emulator", terminalArguments, errorMessage);
		} else {
			const std::string executable = command.front();
			command.erase(command.begin());
			started = StartDetachedProcess(executable, command, errorMessage);
		}
		SetTitle(started ? "Opened image with " + application.name :
			"Cannot open image with " + application.name + ": " + errorMessage);
	}

	void PrintCurrentImage() {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
		char temporaryDirectoryName[] = "/tmp/jpegview-print-XXXXXX";
		if (mkdtemp(temporaryDirectoryName) == nullptr) {
			SetTitle("Print failed: cannot create temporary file");
			return;
		}
		const fs::path temporaryDirectory(temporaryDirectoryName);
		const fs::path temporaryFile = temporaryDirectory / "image.png";
		jpegview_linux::ImageWriteOptions options;
		std::string errorMessage;
		const bool written = jpegview_linux::WriteImage(temporaryFile, image_.bgra.data(), image_.width,
			image_.height, options, errorMessage);
		if (!written) {
			std::error_code removeError;
			fs::remove_all(temporaryDirectory, removeError);
			SetTitle("Print failed: " + errorMessage);
			return;
		}
		const bool printed = RunProcess("lp", {temporaryFile.string()}, errorMessage);
		std::error_code removeError;
		fs::remove_all(temporaryDirectory, removeError);
		SetTitle(printed ? "Sent image to the default printer" : "Print failed: " + errorMessage);
	}

	static bool ParseExifTimestamp(const std::string& value, std::time_t& result) {
		int year = 0;
		int month = 0;
		int day = 0;
		int hour = 0;
		int minute = 0;
		int second = 0;
		if (std::sscanf(value.c_str(), "%d:%d:%d %d:%d:%d", &year, &month, &day,
			&hour, &minute, &second) != 6) return false;
		std::tm localTime{};
		localTime.tm_year = year - 1900;
		localTime.tm_mon = month - 1;
		localTime.tm_mday = day;
		localTime.tm_hour = hour;
		localTime.tm_min = minute;
		localTime.tm_sec = second;
		localTime.tm_isdst = -1;
		const std::time_t converted = std::mktime(&localTime);
		if (converted == static_cast<std::time_t>(-1)) return false;
		result = converted;
		return true;
	}

	static bool SetFileModificationTime(const fs::path& filename, std::time_t timestamp) {
		const timespec times[2] = {
			{0, UTIME_OMIT},
			{timestamp, 0},
		};
		return utimensat(AT_FDCWD, filename.c_str(), times, 0) == 0;
	}

	void ReloadAfterFileChange() {
		if (fileList_.Reload()) LoadCurrent();
	}

	void TouchCurrentImage(bool useExifDate) {
		if (fileList_.Empty() || clipboardMode_) return;
		std::time_t timestamp = std::time(nullptr);
		if (useExifDate) {
			const std::string& exifDate = !metadata_.acquisitionDate.empty() ? metadata_.acquisitionDate : metadata_.dateTime;
			if (exifDate.empty() || !ParseExifTimestamp(exifDate, timestamp)) {
				SetTitle("Cannot set date: image has no usable EXIF date");
				return;
			}
		}
		if (!SetFileModificationTime(fileList_.Current(), timestamp)) {
			SetTitle("Cannot set image modification date");
			return;
		}
		ReloadAfterFileChange();
		SetTitle(useExifDate ? "Set modification date to EXIF date" : "Set modification date to current date");
	}

	void TouchFolderImagesToExifDate() {
		if (fileList_.Empty() || clipboardMode_) return;
		const fs::path directory = fileList_.Current().parent_path();
		int updated = 0;
		std::error_code iteratorError;
		for (const fs::directory_entry& entry : fs::directory_iterator(directory, iteratorError)) {
			if (iteratorError) break;
			if (!entry.is_regular_file(iteratorError) || iteratorError || !IsImagePath(entry.path())) continue;
			jpegview_linux::ExifInfo info;
			std::string comment;
			jpegview_linux::ReadJpegMetadata(entry.path(), info, comment);
			const std::string& exifDate = !info.acquisitionDate.empty() ? info.acquisitionDate : info.dateTime;
			std::time_t timestamp = 0;
			if (!exifDate.empty() && ParseExifTimestamp(exifDate, timestamp) &&
				SetFileModificationTime(entry.path(), timestamp)) {
				++updated;
			}
		}
		ReloadAfterFileChange();
		SetTitle("Set EXIF dates for " + std::to_string(updated) + " image(s)");
	}

	void SetWallpaper(bool processed) {
		if (fileList_.Empty()) return;
		fs::path wallpaperFile = fileList_.Current();
		std::error_code error;
		if (processed) {
			const char* cacheHome = std::getenv("XDG_CACHE_HOME");
			fs::path cacheDirectory;
			if (cacheHome != nullptr && *cacheHome != '\0') {
				cacheDirectory = fs::path(cacheHome) / "jpegview-linux";
			} else {
				const char* home = std::getenv("HOME");
				if (home == nullptr || *home == '\0') {
					SetTitle("Set wallpaper failed: home directory is unknown");
					return;
				}
				cacheDirectory = fs::path(home) / ".cache" / "jpegview-linux";
			}
			fs::create_directories(cacheDirectory, error);
			if (error) {
				SetTitle("Set wallpaper failed: cannot create cache directory");
				return;
			}
			wallpaperFile = cacheDirectory / "wallpaper.png";
			jpegview_linux::ImageWriteOptions options;
			std::string writeError;
			if (!jpegview_linux::WriteImage(wallpaperFile, image_.bgra.data(), image_.width, image_.height,
				options, writeError)) {
				SetTitle("Set wallpaper failed: " + writeError);
				return;
			}
		}

		std::string errorMessage;
		bool applied = false;
		if (HasExecutable("gsettings")) {
			applied = RunProcess("gsettings", {"set", "org.gnome.desktop.background", "picture-uri", FileUri(wallpaperFile)}, errorMessage);
			if (applied) {
				// GNOME 42+ also consults the dark-mode URI.  Older versions simply
				// report an unknown key; the light-mode setting above is sufficient.
				std::string ignoredError;
				RunProcess("gsettings", {"set", "org.gnome.desktop.background", "picture-uri-dark", FileUri(wallpaperFile)}, ignoredError);
			}
		}
		if (!applied && HasExecutable("feh")) {
			applied = RunProcess("feh", {"--bg-fill", wallpaperFile.string()}, errorMessage);
		}
		if (!applied && HasExecutable("nitrogen")) {
			applied = RunProcess("nitrogen", {"--set-zoom-fill", wallpaperFile.string()}, errorMessage);
		}
		SetTitle(applied ? "Set desktop wallpaper" : "Set wallpaper failed: install gsettings, feh, or nitrogen");
	}

	void RequestConfirmation(int command, const std::string& message) {
		confirmationCommand_ = command;
		confirmationMessage_ = message;
		confirmationOpen_ = true;
		contextMenuOpen_ = false;
		fileDialogOpen_ = false;
	}

	void MoveCurrentToTrash() {
		if (fileList_.Empty() || clipboardMode_) return;
		const fs::path filename = fileList_.Current();
		std::string errorMessage;
		bool moved = false;
		if (HasExecutable("gio")) {
			moved = RunProcess("gio", {"trash", filename.string()}, errorMessage);
		} else if (HasExecutable("trash-put")) {
			moved = RunProcess("trash-put", {filename.string()}, errorMessage);
		}
		if (!moved && !HasExecutable("gio") && !HasExecutable("trash-put")) {
			// The confirmation dialog has already made this an explicit user
			// action.  This fallback keeps the key binding useful on minimal
			// systems that do not ship a freedesktop trash helper.
			std::error_code removeError;
			moved = fs::remove(filename, removeError);
			if (!moved) errorMessage = "cannot remove file";
		}
		if (!moved) {
			SetTitle("Delete failed: " + errorMessage);
			return;
		}
		if (fileList_.Reload()) {
			LoadCurrent();
		} else {
			quitRequested_ = true;
		}
		SetTitle("Moved image to trash");
	}

	void HandleConfirmationEvents(const SDL_Event& event) {
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				confirmationOpen_ = false;
			} else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
				const int command = confirmationCommand_;
				confirmationOpen_ = false;
				if (command == IDM_MOVE_TO_RECYCLE_BIN || command == IDM_MOVE_TO_RECYCLE_BIN_CONFIRM ||
					command == IDM_MOVE_TO_RECYCLE_BIN_CONFIRM_PERMANENT_DELETE) {
					MoveCurrentToTrash();
				}
			}
		}
	}

	void OpenAbout() {
		aboutOpen_ = true;
		contextMenuOpen_ = false;
		SetTitle("About JPEGView Linux");
	}

	void HandleAboutEvents(const SDL_Event& event) {
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
			(event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_RETURN ||
			 event.key.keysym.sym == SDLK_SPACE)) {
			aboutOpen_ = false;
			SetTitle();
		} else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
			aboutOpen_ = false;
			SetTitle();
		}
	}

	void RestoreClipboardImage() {
		if (!clipboardMode_) return;
		const fs::path temporaryFile = clipboardTempFile_;
		const fs::path temporaryDirectory = clipboardTempDirectory_;
		if (fileListBeforeClipboard_) {
			fileList_ = std::move(*fileListBeforeClipboard_);
			fileListBeforeClipboard_.reset();
		}
		clipboardMode_ = false;
		clipboardTempFile_.clear();
		clipboardTempDirectory_.clear();
		std::error_code removeError;
		if (!temporaryFile.empty()) fs::remove(temporaryFile, removeError);
		if (!temporaryDirectory.empty()) fs::remove(temporaryDirectory, removeError);
	}

	void PasteCurrentImage() {
		std::vector<std::uint8_t> encodedPng;
		std::string errorMessage;
		if (!jpegview_linux::PasteImageFromClipboard(encodedPng, errorMessage)) {
			SetTitle("Paste image failed: " + errorMessage);
			return;
		}
		char temporaryDirectoryName[] = "/tmp/jpegview-paste-XXXXXX";
		if (mkdtemp(temporaryDirectoryName) == nullptr) {
			SetTitle("Paste image failed: cannot create temporary file");
			return;
		}
		const fs::path temporaryDirectory(temporaryDirectoryName);
		const fs::path temporaryFile = temporaryDirectory / "clipboard.png";
		std::ofstream output(temporaryFile, std::ios::binary);
		output.write(reinterpret_cast<const char*>(encodedPng.data()), static_cast<std::streamsize>(encodedPng.size()));
		const bool written = static_cast<bool>(output);
		output.close();
		std::error_code removeError;
		if (!written) {
			fs::remove(temporaryFile, removeError);
			fs::remove(temporaryDirectory, removeError);
			SetTitle("Paste image failed: cannot write temporary file");
			return;
		}

		RestoreClipboardImage();
		fileListBeforeClipboard_ = std::make_unique<jpegview_linux::FileList>(std::move(fileList_));
		fileList_ = jpegview_linux::FileList({temporaryFile.string()});
		clipboardTempFile_ = temporaryFile;
		clipboardTempDirectory_ = temporaryDirectory;
		clipboardMode_ = true;
		LoadCurrent();
		SetTitle("Clipboard image — press next/previous to return to the file list");
	}

	void RestoreScaleMode(bool wasFitToWindow, bool fillCrop, bool noEnlarge, double manualZoom) {
		if (wasFitToWindow) {
			FitToWindow(fillCrop, noEnlarge);
			return;
		}
		zoom_ = std::clamp(manualZoom, kMinZoom, kMaxZoom);
		fitToWindow_ = false;
		fillWithCrop_ = false;
		autoZoomNoEnlarge_ = false;
		offsetX_ = 0.0;
		offsetY_ = 0.0;
		SetTitle();
	}

	void FitToWindow(bool fillCrop = false, bool noEnlarge = false) {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const double widthScale = static_cast<double>(std::max(1, windowWidth - 16)) / image_.width;
		const double heightScale = static_cast<double>(std::max(1, windowHeight - 16)) / image_.height;
		const double windowScale = fillCrop ? std::max(widthScale, heightScale) : std::min(widthScale, heightScale);
		zoom_ = std::clamp(noEnlarge ? std::min(1.0, windowScale) : windowScale, kMinZoom, kMaxZoom);
		fitToWindow_ = true;
		fillWithCrop_ = fillCrop;
		autoZoomNoEnlarge_ = noEnlarge;
		offsetX_ = 0.0;
		offsetY_ = 0.0;
		SetTitle();
	}

	void ActualSize() {
		zoom_ = 1.0;
		fitToWindow_ = false;
		fillWithCrop_ = false;
		autoZoomNoEnlarge_ = false;
		offsetX_ = 0.0;
		offsetY_ = 0.0;
		SetTitle();
	}

	void ZoomAt(double factor, int mouseX, int mouseY) {
		if (image_.width == 0 || image_.height == 0) {
			return;
		}
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const double oldZoom = zoom_;
		const double imageX = (mouseX - (windowWidth - image_.width * oldZoom) / 2.0 - offsetX_) / oldZoom;
		const double imageY = (mouseY - (windowHeight - image_.height * oldZoom) / 2.0 - offsetY_) / oldZoom;
		zoom_ = std::clamp(oldZoom * factor, kMinZoom, kMaxZoom);
		offsetX_ = mouseX - (windowWidth - image_.width * zoom_) / 2.0 - imageX * zoom_;
		offsetY_ = mouseY - (windowHeight - image_.height * zoom_) / 2.0 - imageY * zoom_;
		fitToWindow_ = false;
		fillWithCrop_ = false;
		autoZoomNoEnlarge_ = false;
		lastInteractionTick_ = SDL_GetTicks();
		SetTitle();
	}

	void NextImage() {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = slideshowSeconds_ > 0.0 && transitionEffect_ != IDM_EFFECT_NONE;
		Image previousImage = animate ? image_ : Image{};
		if (fileList_.Next() && LoadCurrent() && animate) StartTransition(previousImage);
	}

	void PreviousImage() {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = slideshowSeconds_ > 0.0 && transitionEffect_ != IDM_EFFECT_NONE;
		Image previousImage = animate ? image_ : Image{};
		if (fileList_.Previous() && LoadCurrent() && animate) StartTransition(previousImage);
	}

	void FirstImage() {
		if (clipboardMode_) RestoreClipboardImage();
		if (fileList_.Empty() || fileList_.CurrentIndex() == 0) return;
		fileList_.First();
		LoadCurrent();
	}

	void LastImage() {
		if (clipboardMode_) RestoreClipboardImage();
		if (fileList_.Empty() || fileList_.CurrentIndex() + 1 == fileList_.Size()) return;
		fileList_.Last();
		LoadCurrent();
	}

	void ToggleFullscreen() {
		fullscreen_ = !fullscreen_;
		SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : static_cast<Uint32>(0));
		if (fitToWindow_) {
			FitToWindow(fillWithCrop_, autoZoomNoEnlarge_);
		} else {
			SetTitle();
		}
	}

	void FitWindowToImage() {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
		const int width = std::clamp(image_.width + 16, 160, 4096);
		const int height = std::clamp(image_.height + 16, 120, 4096);
		SDL_SetWindowSize(window_, width, height);
		FitToWindow(fillWithCrop_, autoZoomNoEnlarge_);
	}

	void ToggleTitleBar() {
		borderless_ = !borderless_;
		SDL_SetWindowBordered(window_, borderless_ ? 0 : 1);
		SetTitle();
	}

	void ToggleAlwaysOnTop() {
		// SDL_SetWindowAlwaysOnTop is not exported by every SDL2 runtime that
		// can be found on the supported Ubuntu releases. Resolve it lazily so
		// the viewer still links and runs when that optional window-manager
		// feature is unavailable.
		using SetWindowAlwaysOnTop = void (*)(SDL_Window*, int);
		static const SetWindowAlwaysOnTop setWindowAlwaysOnTop =
			reinterpret_cast<SetWindowAlwaysOnTop>(dlsym(RTLD_DEFAULT, "SDL_SetWindowAlwaysOnTop"));
		if (setWindowAlwaysOnTop == nullptr) {
			SetTitle("Always-on-top is not supported by this SDL2 runtime");
			return;
		}
		alwaysOnTop_ = !alwaysOnTop_;
		setWindowAlwaysOnTop(window_, alwaysOnTop_ ? 1 : 0);
		SetTitle();
	}

	void StartSlideshow(double seconds) {
		animationPlaying_ = false;
		playbackMode_ = PlaybackMode::Slideshow;
		slideshowSeconds_ = std::max(0.1, seconds);
		lastSlideshowSeconds_ = slideshowSeconds_;
		nextPlaybackTick_ = 0;
		lastInteractionTick_ = SDL_GetTicks();
		SetTitle();
	}

	void StartMovie(double framesPerSecond) {
		playbackMode_ = PlaybackMode::Movie;
		slideshowSeconds_ = 0.0;
		movieFps_ = std::clamp(framesPerSecond, 1.0, 100.0);
		lastInteractionTick_ = SDL_GetTicks();
		if (animationFrames_.size() > 1 && IsCurrentAnimation()) {
			animationPlaying_ = true;
			ScheduleAnimation(lastInteractionTick_);
		} else {
			animationPlaying_ = false;
			nextPlaybackTick_ = lastInteractionTick_ + MovieFrameInterval();
		}
		SetTitle();
	}

	void StopPlayback() {
		playbackMode_ = PlaybackMode::None;
		slideshowSeconds_ = 0.0;
		animationPlaying_ = false;
		nextPlaybackTick_ = 0;
		lastInteractionTick_ = SDL_GetTicks();
		SetTitle();
	}

	void ResumePlayback() {
		const Uint32 now = SDL_GetTicks();
		if (playbackMode_ == PlaybackMode::Slideshow) {
			StartSlideshow(lastSlideshowSeconds_);
			return;
		}
		if (animationFrames_.size() > 1 && IsCurrentAnimation()) {
			if (animationFrameIndex_ >= animationFrames_.size() - 1) {
				if (!SetAnimationFrame(0)) return;
			}
			animationPlaying_ = true;
			lastInteractionTick_ = now;
			ScheduleAnimation(now);
			SetTitle();
			return;
		}
		StartMovie(movieFps_);
	}

	bool IsCurrentAnimation() const {
		return animationFrames_.size() > 1 &&
			std::any_of(animationFrameDelaysMs_.begin(), animationFrameDelaysMs_.end(),
				[](int delay) { return delay > 0; });
	}

	Uint32 MovieFrameInterval() const {
		return static_cast<Uint32>(std::clamp(
			static_cast<int>(std::lround(1000.0 / std::max(1.0, movieFps_))), 10, 1000));
	}

	void ScheduleAnimation(Uint32 now) {
		int delay = 100;
		if (movieFps_ > 0.0 && playbackMode_ == PlaybackMode::Movie) {
			delay = static_cast<int>(MovieFrameInterval());
		} else if (animationFrameIndex_ < animationFrameDelaysMs_.size()) {
			delay = animationFrameDelaysMs_[animationFrameIndex_];
		}
		nextPlaybackTick_ = now + static_cast<Uint32>(std::clamp(delay, 10, 60000));
	}

	void TickPlayback() {
		const Uint32 now = SDL_GetTicks();
		if (animationPlaying_ && !animationFrames_.empty() && now >= nextPlaybackTick_) {
			if (animationFrameIndex_ + 1 < animationFrames_.size()) {
				++animationFrameIndex_;
				if (!SetAnimationFrame(animationFrameIndex_)) {
					animationPlaying_ = false;
					return;
				}
				ScheduleAnimation(now);
				return;
			}

			++animationLoopsCompleted_;
			if (animationLoopCount_ > 0 && animationLoopsCompleted_ >= animationLoopCount_) {
				animationPlaying_ = false;
				if (playbackMode_ == PlaybackMode::Movie) {
					nextPlaybackTick_ = now + MovieFrameInterval();
					NextImage();
				} else {
					nextPlaybackTick_ = 0;
				}
				return;
			}
			animationFrameIndex_ = 0;
			if (SetAnimationFrame(0)) ScheduleAnimation(now);
			else animationPlaying_ = false;
			return;
		}

		if (playbackMode_ == PlaybackMode::Movie && !animationPlaying_ &&
			nextPlaybackTick_ != 0 && now >= nextPlaybackTick_) {
			nextPlaybackTick_ = now + MovieFrameInterval();
			NextImage();
			return;
		}

		if (playbackMode_ == PlaybackMode::Slideshow && slideshowSeconds_ > 0.0 &&
			static_cast<double>(now - lastInteractionTick_) >= slideshowSeconds_ * 1000.0) {
			NextImage();
		}
	}

	bool SetAnimationFrame(std::size_t index) {
		if (index >= animationFrames_.size()) return false;
		const bool wasFitToWindow = fitToWindow_;
		const bool wasFillWithCrop = fillWithCrop_;
		const bool wasAutoZoomNoEnlarge = autoZoomNoEnlarge_;
		const double manualZoom = zoom_;
		Image base = animationFrames_[index];
		Image displayed = base;
		if (autoContrastEnabled_ && !displayed.AutoContrast()) return false;
		image_ = std::move(displayed);
		correctionBase_ = std::move(base);
		correctionBaseValid_ = true;
		animationFrameIndex_ = index;
		imageModified_ = false;
		if (!UpdateTexture()) return false;
		RestoreScaleMode(wasFitToWindow, wasFillWithCrop, wasAutoZoomNoEnlarge, manualZoom);
		return true;
	}

	void ShowControls() {
		if (!navigationPanelEnabled_) return;
		controlsVisible_ = true;
	}

	std::vector<std::string> ImageInfoLines() const {
		std::vector<std::string> lines;
		if (fileList_.Empty()) return lines;

		std::ostringstream title;
		title << '[' << fileList_.CurrentIndex() + 1 << '/' << fileList_.Size() << "] "
			<< InfoText(fileList_.Current().filename().string());
		lines.push_back(title.str());

		lines.push_back("Image width: " + std::to_string(image_.originalWidth));
		lines.push_back("Image height: " + std::to_string(image_.originalHeight));
		if (animationFrames_.size() > 1) {
			lines.push_back("Frame: " + std::to_string(animationFrameIndex_ + 1) + "/" +
				std::to_string(animationFrames_.size()));
			lines.push_back(std::string("Playback: ") + (animationPlaying_ ? "playing" : "paused"));
		}
		if (image_.width != image_.originalWidth || image_.height != image_.originalHeight) {
			lines.push_back("Displayed size: " + std::to_string(image_.width) + " x " + std::to_string(image_.height));
		}
		std::error_code fileError;
		const std::uintmax_t fileSize = fs::file_size(fileList_.Current(), fileError);
		if (!fileError) lines.push_back("File size: " + FormatFileSize(fileSize));

		const std::string modificationDate = FormatFileTime(fileList_.Current());
		if (!metadata_.acquisitionDate.empty()) {
			lines.push_back("Acquisition date: " + InfoText(metadata_.acquisitionDate));
		} else if (!metadata_.dateTime.empty()) {
			lines.push_back("Exif Date Time: " + InfoText(metadata_.dateTime));
		} else if (!modificationDate.empty()) {
			lines.push_back("Modification date: " + modificationDate);
		}
		if (!metadata_.cameraModel.empty()) lines.push_back("Camera model: " + InfoText(metadata_.cameraModel));
		if (!metadata_.exposureTime.empty()) lines.push_back("Exposure time (s): " + metadata_.exposureTime);
		if (metadata_.hasExposureBias) {
			std::ostringstream value;
			value << std::fixed << std::setprecision(2) << metadata_.exposureBias;
			lines.push_back("Exposure bias (EV): " + value.str());
		}
		if (metadata_.hasFlash) lines.push_back(std::string("Flash fired: ") + (metadata_.flashFired ? "yes" : "no"));
		if (metadata_.hasFocalLength) {
			std::ostringstream value;
			value << std::fixed << std::setprecision(1) << metadata_.focalLength;
			lines.push_back("Focal length (mm): " + value.str());
		}
		if (metadata_.hasFNumber) {
			std::ostringstream value;
			value << std::fixed << std::setprecision(1) << metadata_.fNumber;
			lines.push_back("F-Number: " + value.str());
		}
		if (metadata_.isoSpeed > 0) lines.push_back("ISO Speed: " + std::to_string(metadata_.isoSpeed));
		if (!metadata_.software.empty()) lines.push_back("Software: " + InfoText(metadata_.software));
		if (!metadata_.imageDescription.empty()) lines.push_back("Description: " + InfoText(metadata_.imageDescription));
		if (!metadata_.userComment.empty()) lines.push_back("Comment: " + InfoText(metadata_.userComment));
		if (!jpegComment_.empty() && metadata_.userComment.empty()) lines.push_back("Comment: " + InfoText(jpegComment_));
		if (metadata_.hasGps) lines.push_back("Location: " + metadata_.gpsLocation);
		if (metadata_.hasAltitude) {
			std::ostringstream value;
			value << std::fixed << std::setprecision(0) << metadata_.altitude;
			lines.push_back("Altitude (m): " + value.str());
		}
		return lines;
	}

	SDL_Rect ControlPanelRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int buttonSize = 40;
		const int gap = 5;
		const int margin = 8;
		const int separator = 12;
		const int buttonCount = 8;
		const int panelWidth = margin * 2 + buttonSize * buttonCount + gap * (buttonCount - 2) + separator * 2;
		return SDL_Rect{
			(windowWidth - panelWidth) / 2,
			windowHeight - buttonSize - margin * 2,
			panelWidth,
			buttonSize + margin * 2
		};
	}

	void LayoutControls(std::vector<ControlButton>& buttons) const {
		const SDL_Rect panel = ControlPanelRect();
		const int buttonSize = 40;
		const int gap = 5;
		const int margin = 8;
		const int separator = 12;
		const int commands[] = {
			IDM_FIRST, IDM_PREV, IDM_NEXT,
			IDM_LAST, IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, IDM_FULL_SCREEN_MODE,
			IDM_ROTATE_90, IDM_ROTATE_270
		};
		buttons.clear();
		int x = panel.x + margin;
		for (std::size_t i = 0; i < std::size(commands); ++i) {
			buttons.push_back(ControlButton{SDL_Rect{x, panel.y + margin, buttonSize, buttonSize}, commands[i]});
			x += buttonSize + gap;
			if (i == 3 || i == 5) x += separator;
		}
	}

	static bool PointInRect(int x, int y, const SDL_Rect& rect) {
		return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
	}

	bool HandleControlClick(int x, int y) {
		if (!navigationPanelEnabled_ || !controlsVisible_) return false;
		std::vector<ControlButton> buttons;
		LayoutControls(buttons);
		for (const ControlButton& button : buttons) {
			if (!PointInRect(x, y, button.rect)) continue;
			ExecuteCommand(button.command);
			ShowControls();
			return true;
		}
		return PointInRect(x, y, ControlPanelRect());
	}

	void ExecuteCommand(int command) {
		if (command >= IDM_FIRST_OPENWITH_CMD && command <= IDM_LAST_OPENWITH_CMD) {
			OpenCurrentWith(static_cast<std::size_t>(command - IDM_FIRST_OPENWITH_CMD));
			return;
		}
		switch (command) {
		case IDM_FIRST:
			FirstImage();
			break;
		case IDM_PREV:
			PreviousImage();
			break;
		case IDM_NEXT:
			NextImage();
			break;
		case IDM_LAST:
			LastImage();
			break;
		case IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS:
			if (fitToWindow_) ActualSize(); else FitToWindow();
			break;
		case IDM_FIT_TO_SCREEN:
			FitToWindow();
			break;
		case IDM_ZOOM_100:
			ActualSize();
			break;
		case IDM_FULL_SCREEN_MODE:
			ToggleFullscreen();
			break;
		case IDM_ROTATE_90:
		case IDM_ROTATE_270:
		case IDM_MIRROR_H:
		case IDM_MIRROR_V:
			ApplyTransform(command);
			break;
		case IDM_CHANGESIZE:
			OpenResizeDialog();
			break;
		case IDM_AUTO_CORRECTION:
			ToggleAutoContrast();
			break;
		case IDM_ROTATE_90_LOSSLESS:
		case IDM_ROTATE_90_LOSSLESS_CONFIRM:
		case IDM_ROTATE_270_LOSSLESS:
		case IDM_ROTATE_270_LOSSLESS_CONFIRM:
		case IDM_ROTATE_180_LOSSLESS:
		case IDM_MIRROR_H_LOSSLESS:
		case IDM_MIRROR_V_LOSSLESS:
			ApplyLosslessJpegTransform(command);
			break;
		case IDM_OPEN:
			OpenFileDialog();
			break;
		case IDM_EXPLORE:
			OpenContainingFolder();
			break;
		case IDM_PRINT:
			PrintCurrentImage();
			break;
		case IDM_BATCH_COPY:
			OpenBatchCopyDialog();
			break;
		case IDM_COPY:
			CopyCurrentImage(false);
			break;
		case IDM_COPY_FULL:
			CopyCurrentImage(true);
			break;
		case IDM_COPY_PATH:
			CopyCurrentPath();
			break;
		case IDM_PASTE:
			PasteCurrentImage();
			break;
		case IDM_TOUCH_IMAGE:
			TouchCurrentImage(false);
			break;
		case IDM_TOUCH_IMAGE_EXIF:
			TouchCurrentImage(true);
			break;
		case IDM_TOUCH_IMAGE_EXIF_FOLDER:
			TouchFolderImagesToExifDate();
			break;
		case IDM_SET_WALLPAPER_ORIG:
			SetWallpaper(false);
			break;
		case IDM_SET_WALLPAPER_DISPLAY:
			SetWallpaper(true);
			break;
		case IDM_MOVE_TO_RECYCLE_BIN:
			MoveCurrentToTrash();
			break;
		case IDM_MOVE_TO_RECYCLE_BIN_CONFIRM:
		case IDM_MOVE_TO_RECYCLE_BIN_CONFIRM_PERMANENT_DELETE:
			if (!fileList_.Empty() && !clipboardMode_) {
				RequestConfirmation(command, "Move current image to the desktop trash?");
			}
			break;
		case IDM_SAVE:
		case IDM_SAVE_ALLOW_NO_PROMPT:
			OpenSaveFileDialog(true);
			break;
		case IDM_SAVE_SCREEN:
			OpenSaveFileDialog(false);
			break;
		case IDM_RELOAD:
			if (fileList_.Reload()) LoadCurrent();
			break;
		case IDM_SHOW_FILEINFO:
			infoVisible_ = !infoVisible_;
			break;
		case IDM_SHOW_FILENAME:
			showFileName_ = !showFileName_;
			break;
		case IDM_SHOW_NAVPANEL:
			navigationPanelEnabled_ = !navigationPanelEnabled_;
			if (navigationPanelEnabled_) ShowControls(); else controlsVisible_ = false;
			break;
		case IDM_LOOP_FOLDER:
			fileList_.SetNavigationMode(jpegview_linux::FileList::NavigationMode::LoopDirectory);
			SetTitle();
			break;
		case IDM_LOOP_RECURSIVELY:
			fileList_.SetNavigationMode(jpegview_linux::FileList::NavigationMode::LoopSubDirectories);
			SetTitle();
			break;
		case IDM_LOOP_SIBLINGS:
			fileList_.SetNavigationMode(jpegview_linux::FileList::NavigationMode::LoopSameDirectoryLevel);
			SetTitle();
			break;
		case IDM_SORT_MOD_DATE:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::LastModificationTime,
				fileList_.IsSortedAscending());
			SetTitle();
			break;
		case IDM_SORT_CREATION_DATE:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::CreationTime,
				fileList_.IsSortedAscending());
			SetTitle();
			break;
		case IDM_SORT_NAME:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::FileName,
				fileList_.IsSortedAscending());
			SetTitle();
			break;
		case IDM_SORT_RANDOM:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::Random,
				fileList_.IsSortedAscending());
			SetTitle();
			break;
		case IDM_SORT_SIZE:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::FileSize,
				fileList_.IsSortedAscending());
			SetTitle();
			break;
		case IDM_SORT_ASCENDING:
			fileList_.SetSorting(fileList_.GetSorting(), true);
			SetTitle();
			break;
		case IDM_SORT_DESCENDING:
			fileList_.SetSorting(fileList_.GetSorting(), false);
			SetTitle();
			break;
		case IDM_STOP_MOVIE:
			StopPlayback();
			break;
		case IDM_SLIDESHOW_RESUME:
			ResumePlayback();
			break;
		case IDM_SLIDESHOW_START:
			StartSlideshow(3.0);
			break;
		case IDM_SLIDESHOW_1:
		case IDM_SLIDESHOW_2:
		case IDM_SLIDESHOW_3:
		case IDM_SLIDESHOW_4:
		case IDM_SLIDESHOW_5:
		case IDM_SLIDESHOW_7:
		case IDM_SLIDESHOW_10:
		case IDM_SLIDESHOW_20:
			StartSlideshow(static_cast<double>(command - IDM_SLIDESHOW_START));
			break;
		case IDM_ZOOM_400:
			ZoomAt(4.0 / zoom_, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_200:
			ZoomAt(2.0 / zoom_, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_50:
			ZoomAt(0.5 / zoom_, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_25:
			ZoomAt(0.25 / zoom_, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_INC:
			ZoomAt(1.2, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_DEC:
			ZoomAt(1.0 / 1.2, imageCenterX_, imageCenterY_);
			break;
		case IDM_FILL_WITH_CROP:
			FitToWindow(true, false);
			break;
		case IDM_FIT_TO_SCREEN_NO_ENLARGE:
			FitToWindow(false, true);
			break;
		case IDM_SPAN_SCREENS:
			ToggleFullscreen();
			break;
		case IDM_FIT_WINDOW_TO_IMAGE:
			FitWindowToImage();
			break;
		case IDM_HIDE_TITLE_BAR:
			ToggleTitleBar();
			break;
		case IDM_ALWAYS_ON_TOP:
			ToggleAlwaysOnTop();
			break;
		case IDM_AUTO_ZOOM_FIT_NO_ZOOM:
			FitToWindow(false, true);
			break;
		case IDM_AUTO_ZOOM_FILL_NO_ZOOM:
			FitToWindow(true, true);
			break;
		case IDM_AUTO_ZOOM_FIT:
			FitToWindow(false, false);
			break;
		case IDM_AUTO_ZOOM_FILL:
			FitToWindow(true, false);
			break;
		case IDM_ABOUT:
			OpenAbout();
			break;
		case IDM_MOVIE_START_FPS:
			StartMovie(25.0);
			break;
		case IDM_MOVIE_5_FPS:
		case IDM_MOVIE_10_FPS:
		case IDM_MOVIE_25_FPS:
		case IDM_MOVIE_30_FPS:
		case IDM_MOVIE_50_FPS:
		case IDM_MOVIE_100_FPS:
			StartMovie(static_cast<double>(command - IDM_MOVIE_START_FPS));
			break;
		case IDM_EFFECT_NONE:
		case IDM_EFFECT_BLEND:
		case IDM_EFFECT_SLIDE_RL:
		case IDM_EFFECT_SLIDE_LR:
		case IDM_EFFECT_SLIDE_TB:
		case IDM_EFFECT_SLIDE_BT:
		case IDM_EFFECT_ROLL_RL:
		case IDM_EFFECT_ROLL_LR:
		case IDM_EFFECT_ROLL_TB:
		case IDM_EFFECT_ROLL_BT:
		case IDM_EFFECT_SCROLL_RL:
		case IDM_EFFECT_SCROLL_LR:
		case IDM_EFFECT_SCROLL_TB:
		case IDM_EFFECT_SCROLL_BT:
			transitionEffect_ = command;
			SetTitle();
			break;
		case IDM_EFFECTTIME_VERY_FAST:
			transitionDurationMs_ = 100;
			break;
		case IDM_EFFECTTIME_FAST:
			transitionDurationMs_ = 250;
			break;
		case IDM_EFFECTTIME_NORMAL:
			transitionDurationMs_ = 500;
			break;
		case IDM_EFFECTTIME_SLOW:
			transitionDurationMs_ = 1000;
			break;
		case IDM_EFFECTTIME_VERY_SLOW:
			transitionDurationMs_ = 2000;
			break;
		case IDM_EXIT:
			quitRequested_ = true;
			break;
		case IDM_DEFAULT_ESC:
			if (playbackMode_ != PlaybackMode::None || animationPlaying_) {
				StopPlayback();
			} else {
				quitRequested_ = true;
			}
			break;
		default:
			// The command is known to the Windows resource/key-map files but is
			// not implemented by the focused Linux viewer yet.
			break;
		}
	}

	// This is the supported portion of Config/KeyMap.txt.default expressed in
	// SDL key symbols.  It deliberately returns the original Windows command
	// IDs, just like CKeyMap::GetCommandIdForKey does on Windows.
	int CommandForKey(const SDL_KeyboardEvent& event) const {
		const Sint32 key = event.keysym.sym;
		const Uint16 modifiers = event.keysym.mod;
		const bool ctrl = (modifiers & 0x00C0u) != 0;
		const bool shift = (modifiers & 0x0003u) != 0;
		const bool alt = (modifiers & 0x0300u) != 0;
		if (alt && !ctrl && !shift && key == SDLK_r) return IDM_SLIDESHOW_RESUME;
		if (alt) return 0;

		if (key == SDLK_ESCAPE) return (playbackMode_ != PlaybackMode::None || animationPlaying_) ? IDM_DEFAULT_ESC : IDM_EXIT;
		if (!ctrl && !shift && key == SDLK_q) return IDM_EXIT; // Linux viewer convenience alias.
		if (ctrl && !shift && key == SDLK_o) return IDM_OPEN;
		if (ctrl && !shift && key == SDLK_F2) return IDM_SHOW_FILENAME;
		if (ctrl && !shift && key == 'c') return IDM_COPY;
		if (ctrl && shift && key == 'c') return IDM_COPY_PATH;
		if (ctrl && !shift && key == 'x') return IDM_COPY_FULL;
		if (ctrl && !shift && key == 'v') return IDM_PASTE;
		if (ctrl && !shift && key == 'p') return IDM_PRINT;
		if (ctrl && !shift && key == 's') return IDM_SAVE_ALLOW_NO_PROMPT;
		if (ctrl && shift && key == 's') return IDM_SAVE_SCREEN;
		if (ctrl && !shift && key == SDLK_r) return IDM_RELOAD;
		if (ctrl && shift && key == SDLK_r) return IDM_CHANGESIZE;
		if (ctrl && shift && key == 'm') return IDM_TOUCH_IMAGE;
		if (ctrl && shift && key == 'e') return IDM_TOUCH_IMAGE_EXIF;
		if (ctrl && !shift && key == 'n') return IDM_SHOW_NAVPANEL;
		if (!ctrl && !shift && key == SDLK_F2) return IDM_SHOW_FILEINFO;
		if (!ctrl && !shift && key == SDLK_F3) return IDM_TOGGLE_RESAMPLING_QUALITY;
		if (!ctrl && !shift && key == SDLK_F4) return IDM_KEEP_PARAMETERS;
		if (!ctrl && !shift && key == SDLK_F5) return IDM_AUTO_CORRECTION;
		if (!ctrl && !shift && key == SDLK_F6) return IDM_LDC;
		if (!ctrl && !shift && key == 'c') return IDM_SORT_CREATION_DATE;
		if (!ctrl && !shift && key == 'n') return IDM_SORT_NAME;
		if (!ctrl && !shift && key == 'm') return IDM_SORT_MOD_DATE;
		if (!ctrl && !shift && key == 'z') return IDM_SORT_RANDOM;
		if (!ctrl && !shift && key == SDLK_F7) return IDM_LOOP_FOLDER;
		if (!ctrl && !shift && key == SDLK_F8) return IDM_LOOP_RECURSIVELY;
		if (!ctrl && !shift && key == SDLK_F9) return IDM_LOOP_SIBLINGS;
		if (!ctrl && !shift && key == SDLK_DELETE) return IDM_MOVE_TO_RECYCLE_BIN_CONFIRM;
		if (!ctrl && !shift && key == 'w') return IDM_EXPLORE;

		if (!ctrl && !shift && (key == SDLK_RIGHT || key == SDLK_PAGEDOWN)) return IDM_NEXT;
		if (!ctrl && !shift && (key == SDLK_LEFT || key == SDLK_PAGEUP)) return IDM_PREV;
		if (!ctrl && !shift && key == SDLK_HOME) return IDM_FIRST;
		if (!ctrl && !shift && key == SDLK_END) return IDM_LAST;
		if (!ctrl && !shift && key == SDLK_SPACE) return IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS;
		if (!ctrl && !shift && key == SDLK_RETURN) return IDM_FIT_TO_SCREEN;
		if (!ctrl && !shift && key == SDLK_DOWN) return IDM_ROTATE_90;
		if (!ctrl && !shift && key == SDLK_UP) return IDM_ROTATE_270;
		if (ctrl && key == SDLK_DOWN) return IDM_ZOOM_DEC;
		if (ctrl && key == SDLK_UP) return IDM_ZOOM_INC;
		if (!ctrl && !shift && key == SDLK_F11) return IDM_FULL_SCREEN_MODE;
		if (shift && !ctrl && key == SDLK_F11) return IDM_HIDE_TITLE_BAR;
		if (ctrl && !shift && key == SDLK_F11) return IDM_FIT_WINDOW_TO_IMAGE;
		if (!ctrl && !shift && key == SDLK_F12) return IDM_SPAN_SCREENS;
		if (shift && !ctrl && key == SDLK_F12) return IDM_ALWAYS_ON_TOP;
		if (ctrl && !shift && key == SDLK_RETURN) return IDM_FILL_WITH_CROP;
		if (!ctrl && !shift && key == 'r') return IDM_ROTATE_90_LOSSLESS_CONFIRM;
		if (!ctrl && !shift && key == 't') return IDM_ROTATE_270_LOSSLESS_CONFIRM;

		if (!ctrl && !shift && key == SDLK_0) return IDM_FIT_TO_SCREEN; // retained compatibility alias.
		if (!ctrl && !shift && key == SDLK_f) return IDM_FULL_SCREEN_MODE; // retained compatibility alias.

		const bool plus = key == SDLK_EQUALS || key == SDLK_KP_PLUS;
		if (plus && !ctrl && !alt) return IDM_ZOOM_INC;
		if ((key == SDLK_MINUS || key == SDLK_KP_MINUS) && !ctrl && !alt) return IDM_ZOOM_DEC;
		return 0;
	}

	std::vector<MenuItem> ContextMenuItems() {
		const std::string extension = fileList_.Empty() ? std::string() : Lower(fileList_.Current().extension().string());
		const bool losslessJpegAvailable = !clipboardMode_ && HasExecutable("jpegtran") &&
			(extension == ".jpg" || extension == ".jpeg" || extension == ".jpe");
		openWithApplications_ = DiscoverOpenWithApplications(extension);
		openWithLabels_.clear();
		std::vector<MenuItem> items = {
			// This is a flattened rendering of the complete Windows PopupMenu
			// resource.  Indented entries are the portable equivalent of its
			// submenus. Unsupported Windows-only commands remain visible but
			// disabled instead of silently doing nothing.
			{"Stop slide show/movie", IDM_STOP_MOVIE, false, false,
				playbackMode_ != PlaybackMode::None || animationPlaying_, "Esc"},
			{nullptr, 0, true},
			{"Open image...", IDM_OPEN, false, false, true, "Ctrl+O"},
			{"Open image with", 0},
			{"  (no configured applications)", 0, false, false, false},
			{"Save processed image...", IDM_SAVE, false, false, true, "Ctrl+S"},
			{"Save displayed image...", IDM_SAVE_SCREEN, false, false, true, "Ctrl+Shift+S"},
			{"Reload image", IDM_RELOAD, false, false, true, "Ctrl+R"},
			{"Open containing folder", IDM_EXPLORE, false, false, true, "W"},
			{"Print image...", IDM_PRINT, false, false, true, "Ctrl+P"},
			{"Batch rename/copy...", IDM_BATCH_COPY, false, false, true},
			{"Set modification date", 0},
			{"  To current date", IDM_TOUCH_IMAGE, false, false, true, "Ctrl+Shift+M"},
			{"  To EXIF date", IDM_TOUCH_IMAGE_EXIF, false, false, true, "Ctrl+Shift+E"},
			{"  To EXIF date all files in folder", IDM_TOUCH_IMAGE_EXIF_FOLDER},
			{"Set as desktop wallpaper", 0},
			{"  Use original image", IDM_SET_WALLPAPER_ORIG},
			{"  Use processed image as displayed", IDM_SET_WALLPAPER_DISPLAY},
			{nullptr, 0, true},
			{"Copy to clipboard", IDM_COPY, false, false, true, "Ctrl+C"},
			{"Copy original size image", IDM_COPY_FULL, false, false, true, "Ctrl+X"},
			{"Copy file path", IDM_COPY_PATH, false, false, true, "Ctrl+Shift+C"},
			{"Paste from clipboard", IDM_PASTE, false, false, true, "Ctrl+V"},
			{nullptr, 0, true},
			{"Show picture info (EXIF)", IDM_SHOW_FILEINFO, false, infoVisible_, true, "F2"},
			{"Show filename", IDM_SHOW_FILENAME, false, showFileName_, true, "Ctrl+F2"},
			{"Show navigation panel", IDM_SHOW_NAVPANEL, false, navigationPanelEnabled_, true, "Ctrl+N"},
			{nullptr, 0, true},
			{"Next image", IDM_NEXT, false, false, true, "Right/PgDn"},
			{"Previous image", IDM_PREV, false, false, true, "Left/PgUp"},
			{"First image", IDM_FIRST, false, false, true, "Home"},
			{"Last image", IDM_LAST, false, false, true, "End"},
			{nullptr, 0, true},
			{"Navigation", 0},
			{"  Loop folder", IDM_LOOP_FOLDER, false,
				fileList_.GetNavigationMode() == jpegview_linux::FileList::NavigationMode::LoopDirectory, true, "F7"},
			{"  Loop recursively", IDM_LOOP_RECURSIVELY, false,
				fileList_.GetNavigationMode() == jpegview_linux::FileList::NavigationMode::LoopSubDirectories, true, "F8"},
			{"  Loop siblings", IDM_LOOP_SIBLINGS, false,
				fileList_.GetNavigationMode() == jpegview_linux::FileList::NavigationMode::LoopSameDirectoryLevel, true, "F9"},
			{"Display order", 0},
			{"  Modification date", IDM_SORT_MOD_DATE, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::LastModificationTime, true, "M"},
			{"  Creation date", IDM_SORT_CREATION_DATE, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::CreationTime, true, "C"},
			{"  File name", IDM_SORT_NAME, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::FileName, true, "N"},
			{"  File size", IDM_SORT_SIZE, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::FileSize},
			{"  Random", IDM_SORT_RANDOM, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::Random, true, "Z"},
			{"  Ascending", IDM_SORT_ASCENDING, false, fileList_.IsSortedAscending()},
			{"  Descending", IDM_SORT_DESCENDING, false, !fileList_.IsSortedAscending()},
			{nullptr, 0, true},
			{"Transform image", 0},
			{"  Rotate +90", IDM_ROTATE_90, false, false, true, "Down"},
			{"  Rotate -90", IDM_ROTATE_270, false, false, true, "Up"},
			{"  Rotate...", IDM_ROTATE, false, false, false},
			{"  Change size...", IDM_CHANGESIZE, false, false, image_.width > 0, "Ctrl+Shift+R"},
			{"  Perspective correction...", IDM_PERSPECTIVE, false, false, false},
			{"  Mirror horizontally", IDM_MIRROR_H},
			{"  Mirror vertically", IDM_MIRROR_V},
			{"Lossless JPEG transformations", 0},
			{"  Rotate +90", IDM_ROTATE_90_LOSSLESS, false, false, losslessJpegAvailable, "R"},
			{"  Rotate -90", IDM_ROTATE_270_LOSSLESS, false, false, losslessJpegAvailable, "T"},
			{"  Rotate 180", IDM_ROTATE_180_LOSSLESS, false, false, losslessJpegAvailable},
			{"  Mirror horizontally", IDM_MIRROR_H_LOSSLESS, false, false, losslessJpegAvailable},
			{"  Mirror vertically", IDM_MIRROR_V_LOSSLESS, false, false, losslessJpegAvailable},
			{"Auto correction", IDM_AUTO_CORRECTION, false, autoContrastEnabled_, image_.width > 0, "F5"},
			{"Local density correction", IDM_LDC, false, false, false},
			{"Keep parameters", IDM_KEEP_PARAMETERS, false, false, false},
			{"Save parameters to DB", IDM_SAVE_PARAM_DB, false, false, false},
			{"Clear parameters from DB", IDM_CLEAR_PARAM_DB, false, false, false},
			{nullptr, 0, true},
			{"Scale / zoom", 0},
			{"  Fit to screen", IDM_FIT_TO_SCREEN, false, fitToWindow_ && !fillWithCrop_ && !autoZoomNoEnlarge_, true, "Return/0"},
			{"  Fill with crop", IDM_FILL_WITH_CROP, false, fitToWindow_ && fillWithCrop_ && !autoZoomNoEnlarge_, true, "Ctrl+Return"},
			{"  Span all screens", IDM_SPAN_SCREENS, false, fullscreen_, true, "F12"},
			{"  400 %", IDM_ZOOM_400},
			{"  200 %", IDM_ZOOM_200},
			{"  Actual size (100 %)", IDM_ZOOM_100, false, !fitToWindow_ && std::abs(zoom_ - 1.0) < 0.01, true, "Space"},
			{"  50 %", IDM_ZOOM_50},
			{"  25 %", IDM_ZOOM_25},
			{"  Full screen mode", IDM_FULL_SCREEN_MODE, false, fullscreen_, true, "F11/F"},
			{"  Fit window to image", IDM_FIT_WINDOW_TO_IMAGE, false, false, true, "Ctrl+F11"},
			{"  Hide window title bar", IDM_HIDE_TITLE_BAR, false, borderless_, true, "Shift+F11"},
			{"  Set window always on top", IDM_ALWAYS_ON_TOP, false, alwaysOnTop_, true, "Shift+F12"},
			{"Auto zoom mode", 0},
			{"  Fit to screen no zoom", IDM_AUTO_ZOOM_FIT_NO_ZOOM, false, fitToWindow_ && !fillWithCrop_ && autoZoomNoEnlarge_},
			{"  Fill with crop no zoom", IDM_AUTO_ZOOM_FILL_NO_ZOOM, false, fitToWindow_ && fillWithCrop_ && autoZoomNoEnlarge_},
			{"  Fit to screen", IDM_AUTO_ZOOM_FIT, false, fitToWindow_ && !fillWithCrop_ && !autoZoomNoEnlarge_},
			{"  Fill with crop", IDM_AUTO_ZOOM_FILL, false, fitToWindow_ && fillWithCrop_ && !autoZoomNoEnlarge_},
			{nullptr, 0, true},
			{"Play folder as slideshow/movie", 0},
			{playbackMode_ == PlaybackMode::Slideshow ? "  Stop slideshow" : "  Slideshow",
				playbackMode_ == PlaybackMode::Slideshow ? IDM_STOP_MOVIE : IDM_SLIDESHOW_START,
				false, false, true, "1-9"},
			{"  Waiting time 1 sec", IDM_SLIDESHOW_1, false, false, true, "1"},
			{"  Waiting time 2 sec", IDM_SLIDESHOW_2, false, false, true, "2"},
			{"  Waiting time 3 sec", IDM_SLIDESHOW_3, false, false, true, "3"},
			{"  Waiting time 4 sec", IDM_SLIDESHOW_4, false, false, true, "4"},
			{"  Waiting time 5 sec", IDM_SLIDESHOW_5, false, false, true, "5"},
			{"  Waiting time 7 sec", IDM_SLIDESHOW_7, false, false, true, "7"},
			{"  Waiting time 10 sec", IDM_SLIDESHOW_10, false, false, true},
			{"  Waiting time 20 sec", IDM_SLIDESHOW_20, false, false, true},
			{"  Transition effect", 0},
			{"    None", IDM_EFFECT_NONE, false, transitionEffect_ == IDM_EFFECT_NONE, true},
			{"    Blend", IDM_EFFECT_BLEND, false, transitionEffect_ == IDM_EFFECT_BLEND, true},
			{"    Slide from right", IDM_EFFECT_SLIDE_RL, false, transitionEffect_ == IDM_EFFECT_SLIDE_RL, true},
			{"    Slide from left", IDM_EFFECT_SLIDE_LR, false, transitionEffect_ == IDM_EFFECT_SLIDE_LR, true},
			{"    Slide from top", IDM_EFFECT_SLIDE_TB, false, transitionEffect_ == IDM_EFFECT_SLIDE_TB, true},
			{"    Slide from bottom", IDM_EFFECT_SLIDE_BT, false, transitionEffect_ == IDM_EFFECT_SLIDE_BT, true},
			{"    Roll from right", IDM_EFFECT_ROLL_RL, false, transitionEffect_ == IDM_EFFECT_ROLL_RL, true},
			{"    Roll from left", IDM_EFFECT_ROLL_LR, false, transitionEffect_ == IDM_EFFECT_ROLL_LR, true},
			{"    Roll from top", IDM_EFFECT_ROLL_TB, false, transitionEffect_ == IDM_EFFECT_ROLL_TB, true},
			{"    Roll from bottom", IDM_EFFECT_ROLL_BT, false, transitionEffect_ == IDM_EFFECT_ROLL_BT, true},
			{"    Scroll from right", IDM_EFFECT_SCROLL_RL, false, transitionEffect_ == IDM_EFFECT_SCROLL_RL, true},
			{"    Scroll from left", IDM_EFFECT_SCROLL_LR, false, transitionEffect_ == IDM_EFFECT_SCROLL_LR, true},
			{"    Scroll from top", IDM_EFFECT_SCROLL_TB, false, transitionEffect_ == IDM_EFFECT_SCROLL_TB, true},
			{"    Scroll from bottom", IDM_EFFECT_SCROLL_BT, false, transitionEffect_ == IDM_EFFECT_SCROLL_BT, true},
			{"  Transition speed", 0},
			{"    Very fast", IDM_EFFECTTIME_VERY_FAST, false, transitionDurationMs_ == 100, true},
			{"    Fast", IDM_EFFECTTIME_FAST, false, transitionDurationMs_ == 250, true},
			{"    Normal", IDM_EFFECTTIME_NORMAL, false, transitionDurationMs_ == 500, true},
			{"    Slow", IDM_EFFECTTIME_SLOW, false, transitionDurationMs_ == 1000, true},
			{"    Very slow", IDM_EFFECTTIME_VERY_SLOW, false, transitionDurationMs_ == 2000, true},
			{"  Resume playback", IDM_SLIDESHOW_RESUME, false, false,
				(!animationPlaying_ && (playbackMode_ != PlaybackMode::None || animationFrames_.size() > 1)), "Alt+R"},
			{"  Movie", IDM_MOVIE_START_FPS, false, playbackMode_ == PlaybackMode::Movie &&
				std::abs(movieFps_ - 25.0) < 0.01, true, "25 fps"},
			{"  Playback speed 5 fps", IDM_MOVIE_5_FPS, false, playbackMode_ == PlaybackMode::Movie &&
				std::abs(movieFps_ - 5.0) < 0.01, true, "5"},
			{"  Playback speed 10 fps", IDM_MOVIE_10_FPS, false, playbackMode_ == PlaybackMode::Movie &&
				std::abs(movieFps_ - 10.0) < 0.01, true, "10"},
			{"  Playback speed 25 fps", IDM_MOVIE_25_FPS, false, playbackMode_ == PlaybackMode::Movie &&
				std::abs(movieFps_ - 25.0) < 0.01, true, "25"},
			{"  Playback speed 30 fps", IDM_MOVIE_30_FPS, false, playbackMode_ == PlaybackMode::Movie &&
				std::abs(movieFps_ - 30.0) < 0.01, true, "30"},
			{"  Playback speed 50 fps", IDM_MOVIE_50_FPS, false, playbackMode_ == PlaybackMode::Movie &&
				std::abs(movieFps_ - 50.0) < 0.01, true, "50"},
			{"  Playback speed 100 fps", IDM_MOVIE_100_FPS, false, playbackMode_ == PlaybackMode::Movie &&
				std::abs(movieFps_ - 100.0) < 0.01, true, "100"},
			{nullptr, 0, true},
			{"Settings Admin", 0},
			{"  Edit global settings...", IDM_EDIT_GLOBAL_CONFIG, false, false, false},
			{"  Edit user settings...", IDM_EDIT_USER_CONFIG, false, false, false},
			{"  Update user settings...", IDM_UPDATE_USER_CONFIG, false, false, false},
			{"  Manage Open image with menu...", IDM_MANAGE_OPEN_WITH_MENU, false, false, false},
			{"  Set current parameters as default...", IDM_SAVE_PARAMETERS, false, false, false},
			{"  Set as default viewer...", IDM_SET_AS_DEFAULT_VIEWER, false, false, false},
			{"  Backup parameter DB...", IDM_BACKUP_PARAMDB, false, false, false},
			{"  Restore parameter DB...", IDM_RESTORE_PARAMDB, false, false, false},
			{"User commands", 0},
			{"  (none configured)", 0, false, false, false},
			{nullptr, 0, true},
			{"About JPEGView...", IDM_ABOUT},
			{nullptr, 0, true},
			{"Exit", IDM_EXIT, false, false, true, "Q/Esc"},
		};

		const auto openWithHeader = std::find_if(items.begin(), items.end(), [](const MenuItem& item) {
			return item.label != nullptr && std::string(item.label) == "Open image with";
		});
		if (openWithHeader != items.end()) {
			const std::size_t headerIndex = static_cast<std::size_t>(std::distance(items.begin(), openWithHeader));
			if (!openWithApplications_.empty()) {
				items.erase(items.begin() + static_cast<std::ptrdiff_t>(headerIndex + 1));
				for (std::size_t index = 0; index < openWithApplications_.size(); ++index) {
					openWithLabels_.emplace_back("  " + openWithApplications_[index].name);
					items.insert(items.begin() + static_cast<std::ptrdiff_t>(headerIndex + 1 + index),
						MenuItem{openWithLabels_.back().c_str(),
							static_cast<int>(IDM_FIRST_OPENWITH_CMD + index), false, false, true});
				}
			}
		}
		return items;
	}

	std::string MenuLabel(const MenuItem& item) const {
		if (item.checked) return std::string("[X] ") + item.label;
		return item.label == nullptr ? std::string() : item.label;
	}

	std::string MenuShortcut(const MenuItem& item) const {
		return item.shortcut == nullptr ? std::string() : item.shortcut;
	}

	int ContextMenuVisibleCount() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		return std::max(1, std::min(36, (windowHeight - 24) / kContextMenuItemHeight));
	}

	SDL_Rect ContextMenuRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		int width = 260;
		int height = 12;
		const int visibleCount = ContextMenuVisibleCount();
		const std::size_t visibleEnd = std::min(contextMenuItems_.size(),
			contextMenuScroll_ + static_cast<std::size_t>(visibleCount));
		for (std::size_t index = contextMenuScroll_; index < visibleEnd; ++index) {
			const MenuItem& item = contextMenuItems_[index];
			if (item.separator) {
				height += kContextMenuSeparatorHeight;
				continue;
			}
			const int labelWidth = TextWidth(MenuLabel(item), kUiTextScale);
			const int shortcutWidth = TextWidth(MenuShortcut(item), kUiTextScale);
			width = std::max(width, labelWidth + shortcutWidth + (shortcutWidth > 0 ? 40 : 24));
			height += kContextMenuItemHeight;
		}
		int x = contextMenuX_;
		int y = contextMenuY_;
		if (x + width > windowWidth) x = windowWidth - width - 4;
		if (y + height > windowHeight) y = windowHeight - height - 4;
		x = std::max(4, x);
		y = std::max(4, y);
		return SDL_Rect{x, y, width, height};
	}

	int ContextMenuItemAt(int x, int y) const {
		const SDL_Rect menu = ContextMenuRect();
		if (!PointInRect(x, y, menu)) return -1;
		int itemTop = menu.y + 6;
		const int visibleCount = ContextMenuVisibleCount();
		const std::size_t visibleEnd = std::min(contextMenuItems_.size(),
			contextMenuScroll_ + static_cast<std::size_t>(visibleCount));
		for (std::size_t i = contextMenuScroll_; i < visibleEnd; ++i) {
			const MenuItem& item = contextMenuItems_[i];
			const int itemHeight = item.separator ? kContextMenuSeparatorHeight : kContextMenuItemHeight;
			if (y >= itemTop && y < itemTop + itemHeight) {
				return item.separator || item.command == 0 || !item.enabled ? -1 : static_cast<int>(i);
			}
			itemTop += itemHeight;
		}
		return -1;
	}

	void UpdateContextMenuSelection(int x, int y) {
		menuSelected_ = ContextMenuItemAt(x, y);
	}

	void OpenContextMenu(int x, int y) {
		contextMenuItems_ = ContextMenuItems();
		contextMenuX_ = x;
		contextMenuY_ = y;
		contextMenuScroll_ = 0;
		contextMenuOpen_ = true;
		menuSelected_ = -1;
	}

	void EnsureContextMenuSelectionVisible() {
		const std::size_t visibleCount = static_cast<std::size_t>(ContextMenuVisibleCount());
		if (menuSelected_ >= 0) {
			const std::size_t selected = static_cast<std::size_t>(menuSelected_);
			if (selected < contextMenuScroll_) contextMenuScroll_ = selected;
			if (selected >= contextMenuScroll_ + visibleCount) contextMenuScroll_ = selected - visibleCount + 1;
		}
		const std::size_t maximumScroll = contextMenuItems_.size() > visibleCount ?
			contextMenuItems_.size() - visibleCount : 0;
		contextMenuScroll_ = std::min(contextMenuScroll_, maximumScroll);
	}

	void MoveContextMenuSelection(int direction) {
		if (contextMenuItems_.empty()) return;
		int candidate = menuSelected_;
		for (std::size_t tries = 0; tries < contextMenuItems_.size(); ++tries) {
			candidate += direction;
			if (candidate < 0) candidate = static_cast<int>(contextMenuItems_.size()) - 1;
			if (candidate >= static_cast<int>(contextMenuItems_.size())) candidate = 0;
			if (!contextMenuItems_[candidate].separator && contextMenuItems_[candidate].command != 0 &&
				contextMenuItems_[candidate].enabled) {
				menuSelected_ = candidate;
				EnsureContextMenuSelectionVisible();
				return;
			}
		}
	}

	void ActivateContextMenuSelection(bool& running) {
		if (menuSelected_ < 0 || menuSelected_ >= static_cast<int>(contextMenuItems_.size()) ||
			contextMenuItems_[menuSelected_].separator || contextMenuItems_[menuSelected_].command == 0 ||
			!contextMenuItems_[menuSelected_].enabled) {
			return;
		}
		const int command = contextMenuItems_[menuSelected_].command;
		contextMenuOpen_ = false;
		ExecuteCommand(command);
		if (command == IDM_EXIT) running = false;
	}

	SDL_Rect BatchCopyRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(1120, std::max(760, windowWidth - 40));
		const int height = std::min(760, std::max(520, windowHeight - 40));
		return SDL_Rect{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	SDL_Rect BatchCopyListRect() const {
		const SDL_Rect dialog = BatchCopyRect();
		const int listWidth = std::max(400, dialog.w * 3 / 5);
		return SDL_Rect{dialog.x + 14, dialog.y + 64, listWidth, dialog.h - 136};
	}

	int BatchCopyRightX() const {
		const SDL_Rect list = BatchCopyListRect();
		return list.x + list.w + 20;
	}

	int BatchCopyRightWidth() const {
		const SDL_Rect dialog = BatchCopyRect();
		return std::max(200, dialog.x + dialog.w - BatchCopyRightX() - 14);
	}

	SDL_Rect BatchCopyPatternRect() const {
		const SDL_Rect dialog = BatchCopyRect();
		return SDL_Rect{BatchCopyRightX(), dialog.y + 236, BatchCopyRightWidth(), 30};
	}

	SDL_Rect BatchCopyButtonRect(int button) const {
		const SDL_Rect dialog = BatchCopyRect();
		const SDL_Rect list = BatchCopyListRect();
		const int y = dialog.y + dialog.h - 54;
		const int height = 30;
		switch (button) {
		case kBatchSelectAll: return SDL_Rect{list.x, y, 100, height};
		case kBatchSelectNone: return SDL_Rect{list.x + 108, y, 108, height};
		case kBatchPreview: return SDL_Rect{list.x + 224, y, 90, height};
		case kBatchSavePattern: return SDL_Rect{list.x + 322, y, 118, height};
		case kBatchRename: return SDL_Rect{list.x + 448, y, 118, height};
		case kBatchClose: return SDL_Rect{list.x + 574, y, 80, height};
		default: return SDL_Rect{};
		}
	}

	int BatchCopyEntryAt(int x, int y) const {
		const SDL_Rect list = BatchCopyListRect();
		if (!PointInRect(x, y, list)) return -1;
		const int row = (y - list.y - 22) / 24;
		if (row < 0) return -1;
		const int item = static_cast<int>(batchCopyScroll_) + row;
		return item >= 0 && item < static_cast<int>(batchCopyEntries_.size()) ? item : -1;
	}

	int BatchCopyButtonAt(int x, int y) const {
		for (int button = kBatchSelectAll; button <= kBatchClose; ++button) {
			if (PointInRect(x, y, BatchCopyButtonRect(button))) return button;
		}
		return -1;
	}

	int BatchCopyVisibleRows() const {
		const SDL_Rect list = BatchCopyListRect();
		return std::max(1, (list.h - 22) / 24);
	}

	void EnsureBatchCopySelectionVisible() {
		const int rows = BatchCopyVisibleRows();
		if (batchCopyCursor_ < static_cast<int>(batchCopyScroll_)) batchCopyScroll_ = batchCopyCursor_;
		if (batchCopyCursor_ >= static_cast<int>(batchCopyScroll_) + rows) {
			batchCopyScroll_ = static_cast<std::size_t>(batchCopyCursor_ - rows + 1);
		}
		const int maximumScroll = std::max(0, static_cast<int>(batchCopyEntries_.size()) - rows);
		batchCopyScroll_ = std::min(batchCopyScroll_, static_cast<std::size_t>(maximumScroll));
	}

	void PopulateBatchCopyEntries() {
		batchCopyEntries_.clear();
		for (const fs::path& filename : fileList_.Files()) {
			BatchCopyItem item;
			item.source = filename;
			item.modificationTime = FileModificationTime(filename);
			batchCopyEntries_.push_back(std::move(item));
		}
		if (batchCopyEntries_.empty()) {
			batchCopyCursor_ = 0;
			batchCopyScroll_ = 0;
			return;
		}
		batchCopyCursor_ = std::min(fileList_.CurrentIndex(), batchCopyEntries_.size() - 1);
		batchCopyScroll_ = 0;
		EnsureBatchCopySelectionVisible();
	}

	fs::path BatchCopyDestination(const BatchCopyItem& item, std::size_t selectedIndex) const {
		if (batchCopyPattern_.empty()) return {};
		const std::string expanded = ExpandBatchPattern(batchCopyPattern_, selectedIndex,
			item.source, item.modificationTime);
		if (expanded.empty()) return {};
		const fs::path target(expanded);
		return AbsoluteNormalized(target.is_absolute() ? target : item.source.parent_path() / target);
	}

	void UpdateBatchCopyPreview() {
		std::size_t selectedIndex = 0;
		for (BatchCopyItem& item : batchCopyEntries_) {
			item.destination = fs::path{};
			item.destinationText.clear();
			item.copy = false;
			if (!item.selected) continue;
			item.destinationText = ExpandBatchPattern(batchCopyPattern_, selectedIndex,
				item.source, item.modificationTime);
			if (!item.destinationText.empty()) {
				item.destination = BatchCopyDestination(item, selectedIndex);
				item.copy = item.destination.parent_path() != item.source.parent_path();
			}
			++selectedIndex;
		}
	}

	void PreviewBatchCopy() {
		UpdateBatchCopyPreview();
		if (batchCopyPattern_.empty()) {
			batchCopyMessage_ = "Enter a target pattern first";
			return;
		}
		int selected = 0;
		int copies = 0;
		for (const BatchCopyItem& item : batchCopyEntries_) {
			if (!item.selected) continue;
			++selected;
			if (item.copy) ++copies;
		}
		batchCopyMessage_ = selected == 0 ? "Select one or more files" :
			"Preview: " + std::to_string(selected) + " selected, " + std::to_string(copies) + " copied, " +
			std::to_string(selected - copies) + " renamed";
	}

	void SaveBatchCopyPattern() {
		if (batchCopyPattern_.empty()) {
			batchCopyMessage_ = "Enter a target pattern first";
			return;
		}
		copyRenamePattern_ = batchCopyPattern_;
		SaveSettings();
		batchCopyMessage_ = "Saved batch pattern";
	}

	void PerformBatchCopy() {
		if (batchCopyPattern_.empty()) {
			batchCopyMessage_ = "Enter a target pattern first";
			return;
		}
		UpdateBatchCopyPreview();
		fs::path preferredCurrentPath = fileList_.Current();
		int renamed = 0;
		int copied = 0;
		int createdDirectories = 0;
		int failed = 0;
		std::string firstFailure;
		for (BatchCopyItem& item : batchCopyEntries_) {
			if (!item.selected) continue;
			if (item.destination.empty() || item.destination == item.source) {
				++failed;
				if (firstFailure.empty()) firstFailure = item.source.filename().string() + " has no distinct target";
				continue;
			}
			std::error_code error;
			if (fs::exists(item.destination, error) || error) {
				++failed;
				if (firstFailure.empty()) firstFailure = item.destination.filename().string() + " already exists";
				continue;
			}
			if (item.copy) {
				const fs::path parent = item.destination.parent_path();
				if (!parent.empty()) {
					const bool created = fs::create_directories(parent, error);
					if (error) {
						++failed;
						if (firstFailure.empty()) firstFailure = "cannot create " + parent.string();
						continue;
					}
					if (created) ++createdDirectories;
				}
				if (!fs::copy_file(item.source, item.destination, fs::copy_options::none, error) || error) {
					++failed;
					if (firstFailure.empty()) firstFailure = "cannot copy " + item.source.filename().string();
					continue;
				}
				++copied;
			} else {
				fs::rename(item.source, item.destination, error);
				if (error) {
					++failed;
					if (firstFailure.empty()) firstFailure = "cannot rename " + item.source.filename().string();
					continue;
				}
				if (item.source == preferredCurrentPath) preferredCurrentPath = item.destination;
				++renamed;
			}
		}

		if (renamed > 0 || copied > 0) {
			if (fileList_.Reload(preferredCurrentPath)) LoadCurrent();
		}
		PopulateBatchCopyEntries();
		for (BatchCopyItem& item : batchCopyEntries_) item.selected = false;
		UpdateBatchCopyPreview();
		batchCopyMessage_ = "Completed: " + std::to_string(renamed) + " renamed, " +
			std::to_string(copied) + " copied, " + std::to_string(createdDirectories) + " folder(s) created";
		if (failed > 0) batchCopyMessage_ += "; " + std::to_string(failed) + " failed" +
			(firstFailure.empty() ? std::string() : ": " + firstFailure);
	}

	void OpenBatchCopyDialog() {
		if (fileList_.Empty() || clipboardMode_) return;
		PopulateBatchCopyEntries();
		batchCopyPattern_ = copyRenamePattern_;
		batchCopyMessage_.clear();
		batchCopyOpen_ = true;
		batchCopyPatternFocused_ = true;
		contextMenuOpen_ = false;
		fileDialogOpen_ = false;
		SDL_StartTextInput();
	}

	void CloseBatchCopyDialog() {
		SDL_StopTextInput();
		batchCopyOpen_ = false;
		batchCopyPatternFocused_ = false;
	}

	void HandleBatchCopyButton(int button) {
		switch (button) {
		case kBatchSelectAll:
			for (BatchCopyItem& item : batchCopyEntries_) item.selected = true;
			PreviewBatchCopy();
			break;
		case kBatchSelectNone:
			for (BatchCopyItem& item : batchCopyEntries_) item.selected = false;
			PreviewBatchCopy();
			break;
		case kBatchPreview:
			PreviewBatchCopy();
			break;
		case kBatchSavePattern:
			SaveBatchCopyPattern();
			break;
		case kBatchRename:
			PerformBatchCopy();
			break;
		case kBatchClose:
			CloseBatchCopyDialog();
			break;
		default:
			break;
		}
	}

	void HandleBatchCopyEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN: {
			if (event.key.repeat != 0) break;
			const Uint16 modifiers = event.key.keysym.mod;
			const bool ctrl = (modifiers & 0x00C0u) != 0;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseBatchCopyDialog();
			} else if (ctrl && event.key.keysym.sym == 'a') {
				for (BatchCopyItem& item : batchCopyEntries_) item.selected = true;
				PreviewBatchCopy();
			} else if (event.key.keysym.sym == SDLK_TAB) {
				batchCopyPatternFocused_ = !batchCopyPatternFocused_;
			} else if (batchCopyPatternFocused_ && event.key.keysym.sym == SDLK_BACKSPACE) {
				if (!batchCopyPattern_.empty()) batchCopyPattern_.pop_back();
				PreviewBatchCopy();
			} else if (!batchCopyPatternFocused_ && event.key.keysym.sym == SDLK_UP) {
				batchCopyCursor_ = std::max(0, batchCopyCursor_ - 1);
				EnsureBatchCopySelectionVisible();
			} else if (!batchCopyPatternFocused_ && event.key.keysym.sym == SDLK_DOWN) {
				if (!batchCopyEntries_.empty()) batchCopyCursor_ = std::min(
					static_cast<int>(batchCopyEntries_.size()) - 1, batchCopyCursor_ + 1);
				EnsureBatchCopySelectionVisible();
			} else if (!batchCopyPatternFocused_ && event.key.keysym.sym == SDLK_SPACE) {
				if (batchCopyCursor_ >= 0 && batchCopyCursor_ < static_cast<int>(batchCopyEntries_.size())) {
					BatchCopyItem& item = batchCopyEntries_[static_cast<std::size_t>(batchCopyCursor_)];
					item.selected = !item.selected;
					PreviewBatchCopy();
				}
			} else if (event.key.keysym.sym == SDLK_RETURN) {
				PreviewBatchCopy();
			}
			break;
		}
		case SDL_TEXTINPUT:
			if (batchCopyPatternFocused_) {
				batchCopyPattern_ += event.text.text;
				PreviewBatchCopy();
			}
			break;
		case SDL_MOUSEWHEEL: {
			const int rows = BatchCopyVisibleRows();
			const int maximumScroll = std::max(0, static_cast<int>(batchCopyEntries_.size()) - rows);
			batchCopyScroll_ = static_cast<std::size_t>(std::clamp(
				static_cast<int>(batchCopyScroll_) - event.wheel.y, 0, maximumScroll));
			break;
		}
		case SDL_MOUSEMOTION: {
			const int item = BatchCopyEntryAt(event.motion.x, event.motion.y);
			if (item >= 0) batchCopyCursor_ = item;
			break;
		}
		case SDL_MOUSEBUTTONDOWN:
			if (event.button.button != SDL_BUTTON_LEFT) break;
			if (const int button = BatchCopyButtonAt(event.button.x, event.button.y); button >= 0) {
				HandleBatchCopyButton(button);
				break;
			}
			if (PointInRect(event.button.x, event.button.y, BatchCopyPatternRect())) {
				batchCopyPatternFocused_ = true;
				break;
			}
			if (const int item = BatchCopyEntryAt(event.button.x, event.button.y); item >= 0) {
				batchCopyCursor_ = item;
				batchCopyPatternFocused_ = false;
				batchCopyEntries_[static_cast<std::size_t>(item)].selected =
					!batchCopyEntries_[static_cast<std::size_t>(item)].selected;
				PreviewBatchCopy();
			}
			break;
		default:
			break;
		}
	}

	void RenderBatchButton(int button, const std::string& label) {
		const SDL_Rect rect = BatchCopyButtonRect(button);
		const bool hovered = PointInRect(lastMouseX_, lastMouseY_, rect);
		SDL_SetRenderDrawColor(renderer_, hovered ? 52 : 28, hovered ? 78 : 28, hovered ? 108 : 28, 220);
		SDL_RenderFillRect(renderer_, &rect);
		DrawRect(rect, 125, 145, 165);
		DrawText(ClipText(label, rect.w - 12), rect.x + 6, rect.y + 10, kUiTextScale,
			255, 255, 255);
	}

	void RenderBatchCopy() {
		if (!batchCopyOpen_) return;
		const SDL_Rect dialog = BatchCopyRect();
		const SDL_Rect list = BatchCopyListRect();
		const int rightX = BatchCopyRightX();
		const int rightWidth = BatchCopyRightWidth();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 224);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 190, 190, 190);
		DrawText("BATCH RENAME/COPY OF FILES", dialog.x + 18, dialog.y + 14, kUiTextScale);
		const std::string directory = fileList_.Empty() ? std::string() : fileList_.Current().parent_path().string();
		DrawText(ClipText("IMAGE FILES IN " + directory, dialog.w - 36), dialog.x + 18, dialog.y + 38,
			kUiTextScale, 170, 170, 170);

		SDL_SetRenderDrawColor(renderer_, 25, 25, 25, 215);
		SDL_RenderFillRect(renderer_, &list);
		DrawRect(list, 75, 75, 75);
		DrawText("SEL", list.x + 8, list.y + 7, kUiTextScale, 170, 170, 170);
		DrawText("OLD NAME", list.x + 42, list.y + 7, kUiTextScale, 170, 170, 170);
		DrawText("DATE", list.x + 245, list.y + 7, kUiTextScale, 170, 170, 170);
		DrawText("NEW NAME (>> COPY)", list.x + 380, list.y + 7, kUiTextScale, 170, 170, 170);

		const int rows = BatchCopyVisibleRows();
		for (int row = 0; row < rows; ++row) {
			const int itemIndex = static_cast<int>(batchCopyScroll_) + row;
			if (itemIndex >= static_cast<int>(batchCopyEntries_.size())) break;
			const BatchCopyItem& item = batchCopyEntries_[static_cast<std::size_t>(itemIndex)];
			const int rowTop = list.y + 22 + row * 24;
			if (itemIndex == batchCopyCursor_) {
				SDL_SetRenderDrawColor(renderer_, 45, 82, 120, 205);
				SDL_Rect selection{list.x + 2, rowTop, list.w - 4, 22};
				SDL_RenderFillRect(renderer_, &selection);
			}
			DrawText(item.selected ? "[X]" : "[ ]", list.x + 8, rowTop + 6, kUiTextScale,
				item.selected ? 255 : 150, item.selected ? 255 : 150, item.selected ? 255 : 150);
			DrawText(ClipText(InfoText(item.source.filename().string()), 190), list.x + 42, rowTop + 6,
				kUiTextScale, 235, 235, 235);
			DrawText(ClipText(FormatBatchDate(item.modificationTime), 125), list.x + 245, rowTop + 6,
				kUiTextScale, 210, 210, 210);
			const std::string destination = item.destinationText.empty() ? "-" :
				std::string(item.copy ? ">> " : "") + InfoText(item.destinationText);
			DrawText(ClipText(destination, list.w - 390), list.x + 380, rowTop + 6, kUiTextScale,
				item.copy ? 255 : 220, item.copy ? 220 : 220, item.copy ? 150 : 220);
		}

		DrawText("PLACEHOLDERS", rightX, dialog.y + 68, kUiTextScale, 190, 210, 235);
		DrawText("%x  consecutive number   %Nx  padded number", rightX, dialog.y + 92, kUiTextScale, 205, 205, 205);
		DrawText("%n  number from filename  %f  original filename", rightX, dialog.y + 110, kUiTextScale, 205, 205, 205);
		DrawText("%F  filename without ext  %e  extension", rightX, dialog.y + 128, kUiTextScale, 205, 205, 205);
		DrawText("%d %m %y  day/month/year   %2y  short year", rightX, dialog.y + 146, kUiTextScale, 205, 205, 205);
		DrawText("%h %min  hour/minute       %M %3M  month text", rightX, dialog.y + 164, kUiTextScale, 205, 205, 205);
		DrawText("%pictures%  HOME/Pictures or XDG_PICTURES_DIR", rightX, dialog.y + 182, kUiTextScale, 205, 205, 205);
		DrawText("TARGET PATTERN (use / for folders)", rightX, dialog.y + 216, kUiTextScale, 190, 210, 235);
		const SDL_Rect patternRect = BatchCopyPatternRect();
		SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 220);
		SDL_RenderFillRect(renderer_, &patternRect);
		DrawRect(patternRect, batchCopyPatternFocused_ ? 100 : 75, batchCopyPatternFocused_ ? 130 : 75,
			batchCopyPatternFocused_ ? 165 : 75);
		DrawText(ClipText(batchCopyPattern_, patternRect.w - 16), patternRect.x + 8, patternRect.y + 10,
			kUiTextScale);
		if (!batchCopyMessage_.empty()) {
			DrawText(ClipText(batchCopyMessage_, rightWidth), rightX, dialog.y + dialog.h - 88, kUiTextScale,
				235, 180, 130);
		}
		RenderBatchButton(kBatchSelectAll, "SELECT ALL");
		RenderBatchButton(kBatchSelectNone, "SELECT NONE");
		RenderBatchButton(kBatchPreview, "PREVIEW");
		RenderBatchButton(kBatchSavePattern, "SAVE TEMPLATE");
		RenderBatchButton(kBatchRename, "RENAME/COPY");
		RenderBatchButton(kBatchClose, "CLOSE");
	}

	SDL_Rect ResizeDialogRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(620, std::max(470, windowWidth - 40));
		const int height = std::min(360, std::max(320, windowHeight - 40));
		return SDL_Rect{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	SDL_Rect ResizeFieldRect(int field) const {
		const SDL_Rect dialog = ResizeDialogRect();
		return SDL_Rect{dialog.x + 190, dialog.y + 70 + field * 42, 220, 28};
	}

	SDL_Rect ResizeButtonRect(int button) const {
		const SDL_Rect dialog = ResizeDialogRect();
		const int y = dialog.y + dialog.h - 48;
		if (button == kResizeApply) return SDL_Rect{dialog.x + dialog.w - 198, y, 86, 30};
		if (button == kResizeCancel) return SDL_Rect{dialog.x + dialog.w - 102, y, 86, 30};
		return SDL_Rect{};
	}

	const char* ResizeFilterName() const {
		static constexpr const char* names[kResizeFilterCount] = {
			"BOX / POINT", "LANCZOS / BICUBIC", "SHARPEN LOW", "SHARPEN MEDIUM"
		};
		return names[std::clamp(resizeFilter_, 0, kResizeFilterCount - 1)];
	}

	std::string& ResizeFieldText(int field) {
		if (field == kResizeWidth) return resizeWidthText_;
		if (field == kResizeHeight) return resizeHeightText_;
		return resizePercentText_;
	}

	bool ParseResizePercent(double& percent) const {
		try {
			std::size_t parsedCharacters = 0;
			percent = std::stod(resizePercentText_, &parsedCharacters);
			return parsedCharacters == resizePercentText_.size() && std::isfinite(percent) && percent > 0.0;
		} catch (const std::exception&) {
			return false;
		}
	}

	bool ParseResizeInteger(const std::string& text, int& value) const {
		try {
			std::size_t parsedCharacters = 0;
			const long long parsed = std::stoll(text, &parsedCharacters);
			if (parsedCharacters != text.size() || parsed <= 0 || parsed > kMaxImageDimension) return false;
			value = static_cast<int>(parsed);
			return true;
		} catch (const std::exception&) {
			return false;
		}
	}

	bool ValidResizeSize(int width, int height) const {
		return width > 0 && height > 0 && width <= kMaxImageDimension && height <= kMaxImageDimension &&
			static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) <= kMaxImagePixels;
	}

	void UpdateResizeFieldsFrom(int changedField) {
		if (resizeOriginalWidth_ <= 0 || resizeOriginalHeight_ <= 0) return;
		int width = 0;
		int height = 0;
		double percent = 0.0;
		if (changedField == kResizePercent) {
			if (!ParseResizePercent(percent)) return;
			width = static_cast<int>(std::llround(resizeOriginalWidth_ * percent / 100.0));
			height = static_cast<int>(std::llround(resizeOriginalHeight_ * percent / 100.0));
		} else if (changedField == kResizeWidth) {
			if (!ParseResizeInteger(resizeWidthText_, width)) return;
			percent = 100.0 * width / resizeOriginalWidth_;
			height = static_cast<int>(std::llround(resizeOriginalHeight_ * percent / 100.0));
		} else if (changedField == kResizeHeight) {
			if (!ParseResizeInteger(resizeHeightText_, height)) return;
			percent = 100.0 * height / resizeOriginalHeight_;
			width = static_cast<int>(std::llround(resizeOriginalWidth_ * percent / 100.0));
		}
		if (!ValidResizeSize(width, height)) {
			resizeMessage_ = "Size must be positive and no larger than 65535 x 65535 / 100 MP";
			return;
		}
		resizePercentText_ = std::to_string(std::max(1, static_cast<int>(std::llround(percent))));
		resizeWidthText_ = std::to_string(width);
		resizeHeightText_ = std::to_string(height);
		resizeMessage_.clear();
	}

	bool ResizeTarget(int& width, int& height) const {
		return ParseResizeInteger(resizeWidthText_, width) && ParseResizeInteger(resizeHeightText_, height) &&
			ValidResizeSize(width, height);
	}

	void OpenResizeDialog() {
		if (image_.width <= 0 || image_.height <= 0) return;
		resizeOriginalWidth_ = image_.width;
		resizeOriginalHeight_ = image_.height;
		resizePercentText_ = "100";
		resizeWidthText_ = std::to_string(image_.width);
		resizeHeightText_ = std::to_string(image_.height);
		resizeFilter_ = 2; // CResizeDlg remembers Sharpen low by default.
		resizeField_ = kResizePercent;
		resizeInputPrimed_ = true;
		resizeMessage_.clear();
		resizeDialogOpen_ = true;
		contextMenuOpen_ = false;
		fileDialogOpen_ = false;
		batchCopyOpen_ = false;
		SDL_StartTextInput();
	}

	void CloseResizeDialog() {
		SDL_StopTextInput();
		resizeDialogOpen_ = false;
		resizeInputPrimed_ = false;
		resizeMessage_.clear();
	}

	void ApplyResizeDialog() {
		int width = 0;
		int height = 0;
		if (!ResizeTarget(width, height)) {
			resizeMessage_ = "Enter a valid size (maximum 65535 x 65535 / 100 MP)";
			return;
		}
		if (width == image_.width && height == image_.height) {
			CloseResizeDialog();
			return;
		}
		const bool wasFitToWindow = fitToWindow_;
		const bool wasFillWithCrop = fillWithCrop_;
		const bool wasAutoZoomNoEnlarge = autoZoomNoEnlarge_;
		const double manualZoom = zoom_;
		Image resizedImage = correctionBaseValid_ ? correctionBase_ : image_;
		if (!resizedImage.Resize(width, height, resizeFilter_)) {
			resizeMessage_ = "Resizing failed: not enough memory or the image is too large";
			return;
		}
		resizedImage.originalWidth = width;
		resizedImage.originalHeight = height;
		correctionBase_ = std::move(resizedImage);
		correctionBaseValid_ = true;
		image_ = correctionBase_;
		if (autoContrastEnabled_) image_.AutoContrast();
		if (!UpdateTexture()) {
			resizeMessage_ = "Resizing failed: could not update the display texture";
			return;
		}
		imageModified_ = true;
		RestoreScaleMode(wasFitToWindow, wasFillWithCrop, wasAutoZoomNoEnlarge, manualZoom);
		SetTitle();
		CloseResizeDialog();
	}

	void PrepareResizeTextInput() {
		if (!resizeInputPrimed_ || resizeField_ > kResizeHeight) return;
		ResizeFieldText(resizeField_).clear();
		resizeInputPrimed_ = false;
	}

	void CycleResizeFilter(int direction) {
		resizeFilter_ = (resizeFilter_ + direction + kResizeFilterCount) % kResizeFilterCount;
		resizeField_ = kResizeFilter;
	}

	void HandleResizeDialogEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN: {
			if (event.key.repeat != 0) break;
			const Uint16 modifiers = event.key.keysym.mod;
			const bool shift = (modifiers & 0x0003u) != 0;
			const bool ctrl = (modifiers & 0x00C0u) != 0;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseResizeDialog();
			} else if (event.key.keysym.sym == SDLK_RETURN) {
				ApplyResizeDialog();
			} else if (event.key.keysym.sym == SDLK_TAB) {
				resizeField_ = (resizeField_ + (shift ? kResizeFilterCount - 1 : 1)) % kResizeFilterCount;
				resizeInputPrimed_ = true;
			} else if (ctrl && event.key.keysym.sym == 'a') {
				if (resizeField_ <= kResizeHeight) {
					ResizeFieldText(resizeField_).clear();
					resizeInputPrimed_ = false;
				}
			} else if (resizeField_ <= kResizeHeight && event.key.keysym.sym == SDLK_BACKSPACE) {
				PrepareResizeTextInput();
				std::string& text = ResizeFieldText(resizeField_);
				if (!text.empty()) text.pop_back();
				UpdateResizeFieldsFrom(resizeField_);
			} else if (resizeField_ == kResizeFilter && event.key.keysym.sym == SDLK_LEFT) {
				CycleResizeFilter(-1);
			} else if (resizeField_ == kResizeFilter && event.key.keysym.sym == SDLK_RIGHT) {
				CycleResizeFilter(1);
			} else if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_LEFT) {
				resizeField_ = (resizeField_ + kResizeFilterCount - 1) % kResizeFilterCount;
				resizeInputPrimed_ = true;
			} else if (event.key.keysym.sym == SDLK_DOWN || event.key.keysym.sym == SDLK_RIGHT) {
				resizeField_ = (resizeField_ + 1) % kResizeFilterCount;
				resizeInputPrimed_ = true;
			}
			break;
		}
		case SDL_TEXTINPUT:
			if (resizeField_ <= kResizeHeight) {
				PrepareResizeTextInput();
				std::string& text = ResizeFieldText(resizeField_);
				for (const unsigned char character : std::string(event.text.text)) {
					if (std::isdigit(character) != 0 || (resizeField_ == kResizePercent && character == '.')) {
						text.push_back(static_cast<char>(character));
					}
				}
				UpdateResizeFieldsFrom(resizeField_);
			}
			break;
		case SDL_MOUSEMOTION:
			lastMouseX_ = event.motion.x;
			lastMouseY_ = event.motion.y;
			break;
		case SDL_MOUSEBUTTONDOWN:
			if (event.button.button != SDL_BUTTON_LEFT) break;
			if (PointInRect(event.button.x, event.button.y, ResizeButtonRect(kResizeApply))) {
				ApplyResizeDialog();
			} else if (PointInRect(event.button.x, event.button.y, ResizeButtonRect(kResizeCancel))) {
				CloseResizeDialog();
			} else {
				for (int field = kResizePercent; field <= kResizeFilter; ++field) {
					if (!PointInRect(event.button.x, event.button.y, ResizeFieldRect(field))) continue;
					if (field == kResizeFilter) CycleResizeFilter(1);
					else {
						resizeField_ = field;
						resizeInputPrimed_ = true;
					}
					break;
				}
			}
			break;
		default:
			break;
		}
	}

	void RenderResizeButton(int button, const char* label) {
		const SDL_Rect rect = ResizeButtonRect(button);
		const bool hovered = PointInRect(lastMouseX_, lastMouseY_, rect);
		SDL_SetRenderDrawColor(renderer_, hovered ? 52 : 28, hovered ? 78 : 28, hovered ? 108 : 28, 220);
		SDL_RenderFillRect(renderer_, &rect);
		DrawRect(rect, 125, 145, 165);
		DrawText(label, rect.x + 12, rect.y + 10, kUiTextScale, 255, 255, 255);
	}

	void RenderResizeDialog() {
		if (!resizeDialogOpen_) return;
		const SDL_Rect dialog = ResizeDialogRect();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 232);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 190, 190, 190);
		DrawText("RESIZE IMAGE", dialog.x + 20, dialog.y + 16, kUiTextScale, 255, 255, 255);
		DrawText("ORIGINAL SIZE", dialog.x + 20, dialog.y + 43, kUiTextScale, 180, 195, 215);
		DrawText(std::to_string(resizeOriginalWidth_) + "X" + std::to_string(resizeOriginalHeight_),
			dialog.x + 190, dialog.y + 43, kUiTextScale, 220, 220, 220);

		const char* labels[] = {"NEW SIZE", "NEW WIDTH", "NEW HEIGHT", "FILTER"};
		for (int field = kResizePercent; field <= kResizeFilter; ++field) {
			const SDL_Rect rect = ResizeFieldRect(field);
			const bool focused = resizeField_ == field;
			SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 225);
			SDL_RenderFillRect(renderer_, &rect);
			DrawRect(rect, focused ? 100 : 75, focused ? 130 : 75, focused ? 165 : 75);
			DrawText(labels[field], dialog.x + 20, rect.y + 9, kUiTextScale, 205, 215, 230);
			const std::string value = field == kResizeFilter ? ResizeFilterName() : ResizeFieldText(field);
			DrawText(ClipText(value, rect.w - 16), rect.x + 8, rect.y + 9, kUiTextScale, 255, 255, 255);
			if (field == kResizePercent) DrawText("%", rect.x + rect.w + 10, rect.y + 9, kUiTextScale, 185, 185, 185);
			if (field == kResizeWidth || field == kResizeHeight) {
				DrawText("PIXELS", rect.x + rect.w + 10, rect.y + 9, kUiTextScale, 185, 185, 185);
			}
		}
		if (!resizeMessage_.empty()) {
			DrawText(ClipText(resizeMessage_, dialog.w - 40), dialog.x + 20, dialog.y + dialog.h - 82,
				kUiTextScale, 235, 180, 130);
		}
		DrawText("TAB: NEXT FIELD   ARROWS: CHANGE FILTER/FIELD   ENTER: APPLY   ESC: CANCEL",
			dialog.x + 20, dialog.y + dialog.h - 62, kUiTextScale, 160, 160, 160);
		RenderResizeButton(kResizeApply, "APPLY");
		RenderResizeButton(kResizeCancel, "CANCEL");
	}

	SDL_Rect FileDialogRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(900, std::max(320, windowWidth - 40));
		const int height = std::min(650, std::max(260, windowHeight - 40));
		return SDL_Rect{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	int FileDialogListTop() const {
		return FileDialogRect().y + (fileDialogSave_ ? 112 : 72);
	}

	int FileDialogVisibleRows() const {
		const int reservedHeight = fileDialogSave_ ? 168 : 128;
		return std::max(1, (FileDialogRect().h - reservedHeight) / 26);
	}

	void RefreshFileDialog() {
		fileDialogEntries_.clear();
		std::error_code error;
		const fs::path parent = fileDialogDirectory_.parent_path();
		if (!parent.empty() && parent != fileDialogDirectory_) {
			fileDialogEntries_.push_back(FileDialogEntry{parent, true, true});
		}

		for (const fs::directory_entry& entry : fs::directory_iterator(fileDialogDirectory_, error)) {
			if (error) break;
			std::error_code statusError;
			const bool directory = entry.is_directory(statusError);
			if (statusError || (!directory && (!entry.is_regular_file(statusError) || !IsImagePath(entry.path())))) {
				continue;
			}
			fileDialogEntries_.push_back(FileDialogEntry{AbsoluteNormalized(entry.path()), directory, false});
		}

		std::sort(fileDialogEntries_.begin(), fileDialogEntries_.end(), [](const FileDialogEntry& left, const FileDialogEntry& right) {
			if (left.parent != right.parent) return left.parent;
			if (left.directory != right.directory) return left.directory;
			const std::string leftName = Lower(left.path.filename().string());
			const std::string rightName = Lower(right.path.filename().string());
			return leftName == rightName ? left.path.string() < right.path.string() : leftName < rightName;
		});
		fileDialogSelected_ = 0;
		fileDialogScroll_ = 0;
	}

	void OpenFileDialog() {
		std::error_code error;
		fs::path directory = fs::current_path(error);
		if (!fileList_.Empty()) {
			const fs::path currentDirectory = fileList_.Current().parent_path();
			if (!currentDirectory.empty()) directory = currentDirectory;
		}
		if (error || directory.empty()) directory = fs::path(".");
		fileDialogDirectory_ = AbsoluteNormalized(directory);
		fileDialogSave_ = false;
		fileDialogSaveFullSize_ = true;
		fileDialogFilename_.clear();
		fileDialogMessage_.clear();
		fileDialogOpen_ = true;
		contextMenuOpen_ = false;
		RefreshFileDialog();
	}

	void OpenSaveFileDialog(bool fullSize) {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
		fs::path directory = fileList_.Current().parent_path();
		if (directory.empty()) directory = fs::current_path();
		fileDialogDirectory_ = AbsoluteNormalized(directory);
		fileDialogSave_ = true;
		fileDialogSaveFullSize_ = fullSize;
		fileDialogFilename_ = fileList_.Current().stem().string() + "_proc.jpg";
		fileDialogMessage_.clear();
		fileDialogOverwriteConfirmed_ = false;
		fileDialogOpen_ = true;
		contextMenuOpen_ = false;
		RefreshFileDialog();
		fileDialogSelected_ = -1;
		SDL_StartTextInput();
	}

	std::string FileDialogEntryLabel(const FileDialogEntry& entry) const {
		if (entry.parent) return "[..]";
		return entry.directory ? std::string("[DIR] ") + entry.path.filename().string() : entry.path.filename().string();
	}

	int FileDialogItemAt(int x, int y) const {
		const SDL_Rect dialog = FileDialogRect();
		const int listTop = FileDialogListTop();
		const int rows = FileDialogVisibleRows();
		if (!PointInRect(x, y, SDL_Rect{dialog.x + 12, listTop, dialog.w - 24, rows * 26})) return -1;
		const int row = (y - listTop) / 26;
		const int item = fileDialogScroll_ + row;
		return item >= 0 && item < static_cast<int>(fileDialogEntries_.size()) ? item : -1;
	}

	void EnsureFileDialogSelectionVisible() {
		const int rows = FileDialogVisibleRows();
		if (fileDialogSelected_ < fileDialogScroll_) fileDialogScroll_ = fileDialogSelected_;
		if (fileDialogSelected_ >= fileDialogScroll_ + rows) fileDialogScroll_ = fileDialogSelected_ - rows + 1;
		const int maximumScroll = std::max(0, static_cast<int>(fileDialogEntries_.size()) - rows);
		fileDialogScroll_ = std::clamp(fileDialogScroll_, 0, maximumScroll);
	}

	void MoveFileDialogSelection(int direction) {
		if (fileDialogEntries_.empty()) return;
		fileDialogSelected_ = std::clamp(fileDialogSelected_ + direction, 0,
			static_cast<int>(fileDialogEntries_.size()) - 1);
		EnsureFileDialogSelectionVisible();
	}

	void ActivateFileDialogSelection() {
		if (fileDialogSave_ && fileDialogSelected_ < 0) {
			SaveImageFromDialog();
			return;
		}
		if (fileDialogSelected_ < 0 || fileDialogSelected_ >= static_cast<int>(fileDialogEntries_.size())) return;
		const FileDialogEntry entry = fileDialogEntries_[fileDialogSelected_];
		if (entry.directory) {
			fileDialogDirectory_ = entry.path;
			RefreshFileDialog();
			if (fileDialogSave_) fileDialogSelected_ = -1;
			fileDialogOverwriteConfirmed_ = false;
			return;
		}
		if (fileDialogSave_) {
			fileDialogFilename_ = entry.path.filename().string();
			fileDialogOverwriteConfirmed_ = false;
			SaveImageFromDialog();
		} else {
			OpenDroppedFiles({entry.path.string()});
		}
	}

	void HandleFileDialogEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN:
			if (event.key.repeat != 0) break;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseFileDialog();
			} else if (event.key.keysym.sym == SDLK_UP) {
				MoveFileDialogSelection(-1);
			} else if (event.key.keysym.sym == SDLK_DOWN) {
				MoveFileDialogSelection(1);
			} else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
				ActivateFileDialogSelection();
			} else if (event.key.keysym.sym == SDLK_BACKSPACE) {
				if (fileDialogSave_ && fileDialogSelected_ < 0 && !fileDialogFilename_.empty()) {
					fileDialogFilename_.pop_back();
					fileDialogMessage_.clear();
					fileDialogOverwriteConfirmed_ = false;
					break;
				}
				const fs::path parent = fileDialogDirectory_.parent_path();
				if (!parent.empty() && parent != fileDialogDirectory_) {
					fileDialogDirectory_ = parent;
					RefreshFileDialog();
					if (fileDialogSave_) fileDialogSelected_ = -1;
					if (fileDialogSave_) fileDialogOverwriteConfirmed_ = false;
				}
			}
			break;
		case SDL_TEXTINPUT:
			if (fileDialogSave_) {
				fileDialogFilename_ += event.text.text;
				fileDialogSelected_ = -1;
				fileDialogMessage_.clear();
				fileDialogOverwriteConfirmed_ = false;
			}
			break;
		case SDL_MOUSEMOTION: {
			const int item = FileDialogItemAt(event.motion.x, event.motion.y);
			if (item >= 0) {
				fileDialogSelected_ = item;
				EnsureFileDialogSelectionVisible();
			}
			break;
		}
		case SDL_MOUSEBUTTONDOWN:
			if (event.button.button == SDL_BUTTON_RIGHT ||
			(event.button.button == SDL_BUTTON_LEFT && FileDialogItemAt(event.button.x, event.button.y) < 0)) {
				CloseFileDialog();
			} else if (event.button.button == SDL_BUTTON_LEFT) {
				fileDialogSelected_ = FileDialogItemAt(event.button.x, event.button.y);
				if (fileDialogSave_ && fileDialogSelected_ >= 0 &&
					fileDialogSelected_ < static_cast<int>(fileDialogEntries_.size()) &&
					!fileDialogEntries_[fileDialogSelected_].directory) {
					fileDialogFilename_ = fileDialogEntries_[fileDialogSelected_].path.filename().string();
				}
				if (event.button.clicks >= 2) ActivateFileDialogSelection();
			}
			break;
		default:
			break;
		}
	}

	void RenderFileDialog() {
		if (!fileDialogOpen_) return;
		const SDL_Rect dialog = FileDialogRect();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 220);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 190, 190, 190);
		DrawText(fileDialogSave_ ? "SAVE PROCESSED IMAGE" : "OPEN IMAGE", dialog.x + 18, dialog.y + 14, kUiTextScale);
		DrawText(fileDialogDirectory_.string(), dialog.x + 18, dialog.y + 42, kUiTextScale, 170, 170, 170);
		if (fileDialogSave_) {
			DrawText("FILE NAME", dialog.x + 18, dialog.y + 68, kUiTextScale, 190, 190, 190);
			SDL_Rect inputRect{dialog.x + 12, dialog.y + 86, dialog.w - 24, 28};
			SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 220);
			SDL_RenderFillRect(renderer_, &inputRect);
			DrawRect(inputRect, 100, 130, 165);
			DrawText(fileDialogFilename_, inputRect.x + 10, inputRect.y + 6, kUiTextScale);
		}

		const int listTop = FileDialogListTop();
		const int rows = FileDialogVisibleRows();
		SDL_Rect listRect{dialog.x + 12, listTop, dialog.w - 24, rows * 26};
		SDL_SetRenderDrawColor(renderer_, 25, 25, 25, 210);
		SDL_RenderFillRect(renderer_, &listRect);
		DrawRect(listRect, 75, 75, 75);
		for (int row = 0; row < rows; ++row) {
			const int item = fileDialogScroll_ + row;
			if (item >= static_cast<int>(fileDialogEntries_.size())) break;
			const FileDialogEntry& entry = fileDialogEntries_[item];
			const int rowTop = listTop + row * 26;
			if (item == fileDialogSelected_) {
				SDL_SetRenderDrawColor(renderer_, 45, 82, 120, 205);
				SDL_Rect selection{listRect.x + 2, rowTop + 1, listRect.w - 4, 24};
				SDL_RenderFillRect(renderer_, &selection);
			}
			DrawText(FileDialogEntryLabel(entry), listRect.x + 10, rowTop + 5, kUiTextScale,
				entry.directory ? 185 : 235, entry.directory ? 205 : 235, entry.directory ? 235 : 235);
		}
		if (!fileDialogMessage_.empty()) {
			DrawText(fileDialogMessage_, dialog.x + 18, dialog.y + dialog.h - 60, kUiTextScale, 235, 150, 120);
		}
		DrawText(fileDialogSave_ ? "ENTER SAVE   BACKSPACE EDIT/PARENT   ESC CANCEL" :
			"ENTER OPEN   BACKSPACE PARENT   ESC CANCEL", dialog.x + 18, dialog.y + dialog.h - 34, kUiTextScale, 170, 170, 170);
	}

	void DrawText(const std::string& text, int x, int y, int scale, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235) {
		SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
		int cursorX = x;
		for (const char character : text) {
			const std::array<Uint8, 7>& rows = GlyphRows(character);
			for (int row = 0; row < 7; ++row) {
				for (int column = 0; column < 5; ++column) {
					if ((rows[row] & (1u << (4 - column))) == 0) continue;
					SDL_Rect pixel{cursorX + column * scale, y + row * scale, scale, scale};
					SDL_RenderFillRect(renderer_, &pixel);
				}
			}
			cursorX += 6 * scale;
		}
	}

	void DrawLine(int x1, int y1, int x2, int y2, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235) {
		SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
		SDL_RenderDrawLine(renderer_, x1, y1, x2, y2);
	}

	void DrawRect(const SDL_Rect& rect, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235) {
		SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
		SDL_RenderDrawRect(renderer_, &rect);
	}

	void DrawNavigationIcon(const ControlButton& button, bool hovered) {
		const SDL_Rect r = button.rect;
		if (hovered) {
			SDL_SetRenderDrawColor(renderer_, 65, 65, 65, 165);
			SDL_RenderFillRect(renderer_, &r);
		}
		DrawRect(r, 150, 150, 150);
		const int left = r.x + 8;
		const int right = r.x + r.w - 8;
		const int top = r.y + 8;
		const int bottom = r.y + r.h - 8;
		const int middle = r.y + r.h / 2;
		switch (button.command) {
		case IDM_FIRST:
			DrawLine(left, top, left, bottom);
			DrawLine(left + 8, top, left + 8, bottom);
			DrawLine(right, top, right - 10, middle);
			DrawLine(right - 10, middle, right, bottom);
			break;
		case IDM_PREV:
			DrawLine(left + 5, top, left + 5, bottom);
			DrawLine(right - 1, top, right - 12, middle);
			DrawLine(right - 12, middle, right - 1, bottom);
			break;
		case IDM_NEXT:
			DrawLine(left + 1, top, left + 12, middle);
			DrawLine(left + 12, middle, left + 1, bottom);
			DrawLine(right - 5, top, right - 5, bottom);
			break;
		case IDM_LAST:
			DrawLine(left, top, left + 10, middle);
			DrawLine(left + 10, middle, left, bottom);
			DrawLine(right - 8, top, right - 8, bottom);
			DrawLine(right, top, right, bottom);
			break;
		case IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS:
			if (fitToWindow_) {
				DrawLine(left + 5, top + 4, left + 14, top + 4);
				DrawLine(left + 5, top + 4, left + 5, top + 13);
				DrawLine(right - 5, top + 4, right - 14, top + 4);
				DrawLine(right - 5, top + 4, right - 5, top + 13);
				DrawLine(left + 5, bottom - 4, left + 14, bottom - 4);
				DrawLine(left + 5, bottom - 4, left + 5, bottom - 13);
				DrawLine(right - 5, bottom - 4, right - 14, bottom - 4);
				DrawLine(right - 5, bottom - 4, right - 5, bottom - 13);
			} else {
				DrawLine(left + 7, top + 3, left + 7, bottom - 3);
				DrawLine(left + 7, top + 3, left + 16, top + 3);
				DrawLine(left + 7, bottom - 3, left + 16, bottom - 3);
				DrawLine(right - 7, top + 3, right - 7, bottom - 3);
				DrawLine(right - 7, top + 3, right - 16, top + 3);
				DrawLine(right - 7, bottom - 3, right - 16, bottom - 3);
			}
			break;
		case IDM_FULL_SCREEN_MODE:
			DrawRect(SDL_Rect{left, top, right - left, bottom - top});
			DrawLine(left, top + 7, right, top + 7);
			break;
		case IDM_ROTATE_90:
			DrawLine(left + 4, bottom - 2, right - 2, bottom - 2);
			DrawLine(right - 2, bottom - 2, right - 2, top + 7);
			DrawLine(right - 2, top + 7, right - 9, top + 7);
			DrawLine(right - 9, top + 7, right - 5, top + 3);
			DrawLine(right - 9, top + 7, right - 5, top + 11);
			break;
		case IDM_ROTATE_270:
			DrawLine(left + 2, top + 7, left + 2, bottom - 2);
			DrawLine(left + 2, bottom - 2, right - 4, bottom - 2);
			DrawLine(left + 2, top + 7, left + 9, top + 7);
			DrawLine(left + 9, top + 7, left + 5, top + 3);
			DrawLine(left + 9, top + 7, left + 5, top + 11);
			break;
		default:
			break;
		}
	}

	std::string ControlTooltip(int command) const {
		switch (command) {
		case IDM_FIRST:
			return "Show first image in folder (Home)";
		case IDM_PREV:
			return "Show previous image (Left)";
		case IDM_NEXT:
			return "Show next image (Right)";
		case IDM_LAST:
			return "Show last image in folder (End)";
		case IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS:
			return fitToWindow_ ? "Actual size of image (Space)" : "Fit image to screen (Space)";
		case IDM_FULL_SCREEN_MODE:
			return fullscreen_ ? "Window mode (F11)" : "Full screen mode (F11)";
		case IDM_ROTATE_90:
			return "Rotate image 90 deg clockwise (Down)";
		case IDM_ROTATE_270:
			return "Rotate image 90 deg counter-clockwise (Up)";
		default:
			return {};
		}
	}

	void RenderControlTooltip(const SDL_Rect& anchor, const std::string& text) {
		if (text.empty()) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int maximumWidth = std::max(1, windowWidth - 8);
		std::string label = text;
		const int availableTextWidth = std::max(1, maximumWidth - 16);
		if (TextWidth(label, kUiTextScale) > availableTextWidth) {
			const std::size_t maximumCharacters = static_cast<std::size_t>(std::max(3,
				availableTextWidth / (6 * kUiTextScale)));
			label.resize(maximumCharacters - 3);
			label += "...";
		}
		const int tooltipWidth = std::min(maximumWidth, TextWidth(label, kUiTextScale) + 16);
		const int tooltipHeight = 22;
		int x = anchor.x + (anchor.w - tooltipWidth) / 2;
		x = std::clamp(x, 4, std::max(4, windowWidth - tooltipWidth - 4));
		int y = anchor.y - tooltipHeight - 6;
		if (y < 4) y = anchor.y + anchor.h + 6;
		if (y + tooltipHeight > windowHeight) y = std::max(4, windowHeight - tooltipHeight - 4);
		const SDL_Rect tooltip{x, y, tooltipWidth, tooltipHeight};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 215);
		SDL_RenderFillRect(renderer_, &tooltip);
		DrawRect(tooltip, 190, 190, 190);
		DrawText(label, tooltip.x + 8, tooltip.y + 7, kUiTextScale, 255, 255, 255);
	}

	void RenderFileName() {
		if (!showFileName_ || fileList_.Empty() || contextMenuOpen_ || fileDialogOpen_ || batchCopyOpen_ || resizeDialogOpen_) return;
		int windowWidth = 0;
		SDL_GetWindowSize(window_, &windowWidth, nullptr);
		std::ostringstream text;
		text << '[' << fileList_.CurrentIndex() + 1 << '/' << fileList_.Size() << "] "
			<< InfoText(fileList_.Current().filename().string());
		std::string label = text.str();
		const int panelWidth = std::min(std::max(260, windowWidth - 16), 900);
		const int textWidth = panelWidth - 20;
		if (TextWidth(label, kUiTextScale) > textWidth) {
			const std::size_t maximumCharacters = static_cast<std::size_t>(std::max(3,
				textWidth / (6 * kUiTextScale)));
			label.resize(maximumCharacters - 3);
			label += "...";
		}
		const SDL_Rect panel{8, 8, panelWidth, 28};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 205);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 105, 105, 105);
		DrawText(label, panel.x + 10, panel.y + 6, kUiTextScale, 255, 255, 255);
	}

	void RenderImageInfo() {
		if (!infoVisible_ || contextMenuOpen_ || fileDialogOpen_ || batchCopyOpen_ || resizeDialogOpen_) return;
		std::vector<std::string> lines = ImageInfoLines();
		if (lines.empty()) return;

		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int panelWidth = std::min(std::max(260, windowWidth - 16), 620);
		const int textWidth = panelWidth - 20;
		for (std::string& line : lines) {
			line = InfoText(line);
			if (TextWidth(line, kUiTextScale) <= textWidth) continue;
			const std::size_t maximumCharacters = static_cast<std::size_t>(std::max(3,
				textWidth / (6 * kUiTextScale)));
			line.resize(maximumCharacters - 3);
			line += "...";
		}

		const int lineHeight = 18;
		const int panelHeight = std::min(windowHeight - 16, 20 + static_cast<int>(lines.size()) * lineHeight);
		const int panelTop = showFileName_ ? 42 : 8;
		SDL_Rect panel{8, panelTop, panelWidth, panelHeight};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 205);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 105, 105, 105);
		const int visibleLines = std::max(0, (panelHeight - 12) / lineHeight);
		for (int index = 0; index < visibleLines && index < static_cast<int>(lines.size()); ++index) {
			DrawText(lines[static_cast<std::size_t>(index)], panel.x + 10, panel.y + 10 + index * lineHeight,
				kUiTextScale,
				index == 0 ? 255 : 243, index == 0 ? 255 : 242, index == 0 ? 255 : 231);
		}
	}

	void RenderControls() {
		if (!navigationPanelEnabled_ || !controlsVisible_ || contextMenuOpen_ || fileDialogOpen_ || batchCopyOpen_ || resizeDialogOpen_) return;
		std::vector<ControlButton> buttons;
		LayoutControls(buttons);
		const SDL_Rect panel = ControlPanelRect();
		const ControlButton* hoveredButton = nullptr;

		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 205);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 105, 105, 105);
		for (const ControlButton& button : buttons) {
			const bool hovered = PointInRect(lastMouseX_, lastMouseY_, button.rect);
			if (hovered) hoveredButton = &button;
			DrawNavigationIcon(button, hovered);
		}
		if (hoveredButton != nullptr) RenderControlTooltip(hoveredButton->rect, ControlTooltip(hoveredButton->command));
	}

	void RenderConfirmation() {
		if (!confirmationOpen_) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(760, std::max(360, windowWidth - 40));
		const int height = 136;
		const SDL_Rect panel{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 220);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 220, 170, 110);
		DrawText("CONFIRM ACTION", panel.x + 18, panel.y + 14, kUiTextScale, 255, 220, 150);
		DrawText(confirmationMessage_, panel.x + 18, panel.y + 42, kUiTextScale);
		std::string filename = fileList_.Empty() ? std::string() : InfoText(fileList_.Current().filename().string());
		const int maximumCharacters = std::max(3, (width - 36) / (6 * kUiTextScale));
		if (static_cast<int>(filename.size()) > maximumCharacters) {
			filename.resize(static_cast<std::size_t>(maximumCharacters - 3));
			filename += "...";
		}
		DrawText(filename, panel.x + 18, panel.y + 68, kUiTextScale, 220, 220, 220);
		DrawText("ENTER or SPACE: YES     ESC: CANCEL", panel.x + 18, panel.y + 104, kUiTextScale, 180, 180, 180);
	}

	void RenderAbout() {
		if (!aboutOpen_) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(620, std::max(360, windowWidth - 40));
		const int height = 196;
		const SDL_Rect panel{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 220);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 160, 190, 225);
		DrawText("JPEGVIEW LINUX", panel.x + 18, panel.y + 16, kUiTextScale, 255, 255, 255);
		DrawText("NATIVE SDL2 VIEWER", panel.x + 18, panel.y + 48, kUiTextScale, 210, 225, 250);
		DrawText("PORT OF JPEGVIEW 1.3.46", panel.x + 18, panel.y + 80, kUiTextScale, 210, 225, 250);
		DrawText("FOLDER NAVIGATION AND IMAGE VIEWING", panel.x + 18, panel.y + 112, kUiTextScale, 185, 205, 220);
		DrawText("PRESS ESC TO CLOSE", panel.x + 18, panel.y + 156, kUiTextScale, 180, 180, 180);
	}

	void RenderImageTransition(const SDL_Rect& destination, int windowWidth, int windowHeight, SDL_Texture* currentTexture) {
		if (transitionTexture_ == nullptr || transitionStartTick_ == 0) {
			SDL_RenderCopy(renderer_, currentTexture, nullptr, &destination);
			return;
		}
		const Uint32 elapsed = SDL_GetTicks() - transitionStartTick_;
		const double progress = std::min(1.0, static_cast<double>(elapsed) /
			static_cast<double>(transitionDurationMs_));
		if (progress >= 1.0) {
			ClearTransition();
			SDL_RenderCopy(renderer_, currentTexture, nullptr, &destination);
			return;
		}

		SDL_Rect oldDestination{
			static_cast<int>(std::round((windowWidth - transitionImage_.width * zoom_) / 2.0 + offsetX_)),
			static_cast<int>(std::round((windowHeight - transitionImage_.height * zoom_) / 2.0 + offsetY_)),
			std::max(1, static_cast<int>(std::round(transitionImage_.width * zoom_))),
			std::max(1, static_cast<int>(std::round(transitionImage_.height * zoom_))),
		};
		SDL_Rect enteringDestination = destination;
		const int horizontalDistance = std::max(windowWidth, destination.w);
		const int verticalDistance = std::max(windowHeight, destination.h);
		const int effect = transitionEffect_;
		const bool fromRight = effect == IDM_EFFECT_SLIDE_RL || effect == IDM_EFFECT_ROLL_RL || effect == IDM_EFFECT_SCROLL_RL;
		const bool fromLeft = effect == IDM_EFFECT_SLIDE_LR || effect == IDM_EFFECT_ROLL_LR || effect == IDM_EFFECT_SCROLL_LR;
		const bool fromTop = effect == IDM_EFFECT_SLIDE_TB || effect == IDM_EFFECT_ROLL_TB || effect == IDM_EFFECT_SCROLL_TB;
		const bool fromBottom = effect == IDM_EFFECT_SLIDE_BT || effect == IDM_EFFECT_ROLL_BT || effect == IDM_EFFECT_SCROLL_BT;
		if (fromRight || fromLeft) {
			const int distance = static_cast<int>(std::round(horizontalDistance * (1.0 - progress)));
			enteringDestination.x += fromRight ? distance : -distance;
			oldDestination.x += fromRight ? -static_cast<int>(std::round(horizontalDistance * progress)) :
				static_cast<int>(std::round(horizontalDistance * progress));
		} else if (fromTop || fromBottom) {
			const int distance = static_cast<int>(std::round(verticalDistance * (1.0 - progress)));
			enteringDestination.y += fromBottom ? distance : -distance;
			oldDestination.y += fromBottom ? -static_cast<int>(std::round(verticalDistance * progress)) :
				static_cast<int>(std::round(verticalDistance * progress));
		}

		SDL_SetTextureAlphaMod(transitionTexture_, 255);
		SDL_SetTextureBlendMode(currentTexture, SDL_BLENDMODE_BLEND);
		SDL_SetTextureAlphaMod(currentTexture, 255);
		if (effect == IDM_EFFECT_BLEND) {
			SDL_SetTextureAlphaMod(transitionTexture_, static_cast<Uint8>(std::round(255.0 * (1.0 - progress))));
		}
		SDL_RenderCopy(renderer_, transitionTexture_, nullptr, &oldDestination);
		SDL_RenderCopy(renderer_, currentTexture, nullptr, &enteringDestination);
	}

	void RenderContextMenu() {
		if (!contextMenuOpen_) return;
		const SDL_Rect menu = ContextMenuRect();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 220);
		SDL_RenderFillRect(renderer_, &menu);
		DrawRect(menu, 185, 185, 185);

		int itemTop = menu.y + 6;
		const int visibleCount = ContextMenuVisibleCount();
		const std::size_t visibleEnd = std::min(contextMenuItems_.size(),
			contextMenuScroll_ + static_cast<std::size_t>(visibleCount));
		for (std::size_t i = contextMenuScroll_; i < visibleEnd; ++i) {
			const MenuItem& item = contextMenuItems_[i];
			if (item.separator) {
				DrawLine(menu.x + 10, itemTop + 4, menu.x + menu.w - 10, itemTop + 4, 75, 75, 75);
				itemTop += kContextMenuSeparatorHeight;
				continue;
			}
			if (static_cast<int>(i) == menuSelected_) {
				SDL_SetRenderDrawColor(renderer_, 45, 82, 120, 205);
				SDL_Rect selection{menu.x + 3, itemTop, menu.w - 6, kContextMenuItemHeight};
				SDL_RenderFillRect(renderer_, &selection);
			}
			const Uint8 textColor = item.command == 0 ? 135 : (item.enabled ? 235 : 100);
			const std::string label = MenuLabel(item);
			const std::string shortcut = MenuShortcut(item);
			DrawText(label, menu.x + 12, itemTop + 4, kUiTextScale,
				textColor, textColor, textColor);
			if (!shortcut.empty()) {
				const int shortcutWidth = TextWidth(shortcut, kUiTextScale);
				const Uint8 shortcutColor = item.enabled ? 175 : 90;
				DrawText(shortcut, menu.x + menu.w - 12 - shortcutWidth, itemTop + 4, kUiTextScale,
					shortcutColor, shortcutColor, shortcutColor);
			}
			itemTop += kContextMenuItemHeight;
		}
	}

	void HandleEvents(bool& running) {
		SDL_Event event{};
		while (SDL_PollEvent(&event) != 0) {
			lastInteractionTick_ = SDL_GetTicks();
			if (confirmationOpen_) {
				if (event.type == SDL_QUIT) running = false;
				else HandleConfirmationEvents(event);
				continue;
			}
			if (aboutOpen_) {
				if (event.type == SDL_QUIT) running = false;
				else HandleAboutEvents(event);
				continue;
			}
			if (fileDialogOpen_) {
				HandleFileDialogEvents(event, running);
				continue;
			}
			if (batchCopyOpen_) {
				HandleBatchCopyEvents(event, running);
				continue;
			}
			if (resizeDialogOpen_) {
				HandleResizeDialogEvents(event, running);
				continue;
			}
			if (contextMenuOpen_) {
				switch (event.type) {
				case SDL_QUIT:
					running = false;
					break;
				case SDL_KEYDOWN:
					if (event.key.repeat != 0) break;
					if (event.key.keysym.sym == SDLK_ESCAPE) {
						contextMenuOpen_ = false;
					} else if (event.key.keysym.sym == SDLK_UP) {
						MoveContextMenuSelection(-1);
					} else if (event.key.keysym.sym == SDLK_DOWN) {
						MoveContextMenuSelection(1);
					} else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
						ActivateContextMenuSelection(running);
					}
					break;
				case SDL_MOUSEMOTION:
					UpdateContextMenuSelection(event.motion.x, event.motion.y);
					break;
				case SDL_MOUSEWHEEL: {
					const int maximumScroll = std::max(0, static_cast<int>(contextMenuItems_.size()) - ContextMenuVisibleCount());
					contextMenuScroll_ = static_cast<std::size_t>(std::clamp(
						static_cast<int>(contextMenuScroll_) - event.wheel.y, 0, maximumScroll));
					menuSelected_ = -1;
					break;
				}
				case SDL_MOUSEBUTTONDOWN:
					if (event.button.button == SDL_BUTTON_LEFT) {
						const int item = ContextMenuItemAt(event.button.x, event.button.y);
						if (item >= 0) {
							menuSelected_ = item;
							ActivateContextMenuSelection(running);
						} else {
							contextMenuOpen_ = false;
						}
					} else {
						contextMenuOpen_ = false;
					}
					break;
				default:
					break;
				}
				continue;
			}
			switch (event.type) {
			case SDL_QUIT:
				running = false;
				break;
			case SDL_WINDOWEVENT:
				if (event.window.event == SDL_WINDOWEVENT_MAXIMIZED) {
					maximized_ = true;
				} else if (event.window.event == SDL_WINDOWEVENT_RESTORED) {
					maximized_ = false;
				} else if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
					event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					if (fitToWindow_) FitToWindow(fillWithCrop_, autoZoomNoEnlarge_);
				}
				break;
			case SDL_KEYDOWN:
			{
				const Uint16 modifiers = event.key.keysym.mod;
				const bool plainNavigationKey =
					(modifiers & 0x03C3u) == 0 &&
					(event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_RIGHT);
				// SDL marks OS key-repeat events instead of generating a fresh
				// physical key press. Keep repeating navigation, while retaining
				// one-shot behavior for commands such as delete, save, and rotate.
				if (event.key.repeat != 0 && !plainNavigationKey) break;
				const bool plainKey = (modifiers & 0x03C3u) == 0;
				if (plainKey && event.key.keysym.sym >= '1' && event.key.keysym.sym <= '9') {
					// CMainDlg::OnKeyDown reserves the number row for quick
					// slideshow intervals before consulting KeyMap.txt.default.
					StartSlideshow(static_cast<double>(event.key.keysym.sym - '0'));
					break;
				}
				const int command = CommandForKey(event.key);
				if (command != 0) ExecuteCommand(command);
				if (quitRequested_) running = false;
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
				if (event.button.button == SDL_BUTTON_LEFT) {
					if (HandleControlClick(event.button.x, event.button.y)) {
						dragging_ = false;
						break;
					}
					dragging_ = true;
					lastMouseX_ = event.button.x;
					lastMouseY_ = event.button.y;
					imageCenterX_ = event.button.x;
					imageCenterY_ = event.button.y;
				} else if (event.button.button == SDL_BUTTON_RIGHT) {
					OpenContextMenu(event.button.x, event.button.y);
				}
				break;
			case SDL_MOUSEBUTTONUP:
				if (event.button.button == SDL_BUTTON_LEFT) dragging_ = false;
				break;
			case SDL_MOUSEMOTION:
				imageCenterX_ = event.motion.x;
				imageCenterY_ = event.motion.y;
				ShowControls();
				if (dragging_) {
					offsetX_ += event.motion.xrel;
					offsetY_ += event.motion.yrel;
					fitToWindow_ = false;
					SetTitle();
				}
				lastMouseX_ = event.motion.x;
				lastMouseY_ = event.motion.y;
				break;
			case SDL_MOUSEWHEEL:
				if (event.wheel.y > 0) {
					ZoomAt(1.2, lastMouseX_, lastMouseY_);
				} else if (event.wheel.y < 0) {
					ZoomAt(1.0 / 1.2, lastMouseX_, lastMouseY_);
				}
				break;
			case SDL_DROPBEGIN:
				pendingDroppedFiles_.clear();
				break;
			case SDL_DROPFILE:
				if (event.drop.file != nullptr) {
					pendingDroppedFiles_.emplace_back(event.drop.file);
					SDL_free(event.drop.file);
				}
				break;
			case SDL_DROPCOMPLETE:
				OpenDroppedFiles(pendingDroppedFiles_);
				pendingDroppedFiles_.clear();
				break;
			default:
				break;
			}
		}
	}

	void Render() {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		imageCenterX_ = windowWidth / 2;
		imageCenterY_ = windowHeight / 2;
		const int renderWidth = std::max(1, static_cast<int>(std::round(image_.width * zoom_)));
		const int renderHeight = std::max(1, static_cast<int>(std::round(image_.height * zoom_)));
		SDL_Rect destination{
			static_cast<int>(std::round((windowWidth - renderWidth) / 2.0 + offsetX_)),
			static_cast<int>(std::round((windowHeight - renderHeight) / 2.0 + offsetY_)),
			renderWidth,
			renderHeight
		};
		SDL_Texture* renderTexture = DisplayTextureFor(renderWidth, renderHeight);
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		SDL_SetRenderDrawColor(renderer_, 18, 18, 18, 255);
		SDL_RenderClear(renderer_);
		RenderImageTransition(destination, windowWidth, windowHeight, renderTexture);
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		RenderFileName();
		RenderImageInfo();
		RenderControls();
		RenderContextMenu();
		RenderFileDialog();
		RenderBatchCopy();
		RenderResizeDialog();
		RenderConfirmation();
		RenderAbout();
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		SDL_RenderPresent(renderer_);
	}

	jpegview_linux::FileList fileList_;
	double slideshowSeconds_ = 0.0;
	double lastSlideshowSeconds_ = 3.0;
	double movieFps_ = 25.0;
	PlaybackMode playbackMode_ = PlaybackMode::None;
	Uint32 nextPlaybackTick_ = 0;
	bool animationPlaying_ = false;
	std::vector<Image> animationFrames_;
	std::vector<int> animationFrameDelaysMs_;
	std::size_t animationFrameIndex_ = 0;
	int animationLoopCount_ = 0;
	int animationLoopsCompleted_ = 0;
	int transitionEffect_ = IDM_EFFECT_NONE;
	Uint32 transitionDurationMs_ = 500;
	Uint32 transitionStartTick_ = 0;
	bool startFullscreen_ = false;
	Uint32 lastInteractionTick_ = 0;
	Image image_;
	Image correctionBase_;
	bool correctionBaseValid_ = false;
	SDL_Window* window_ = nullptr;
	SDL_Renderer* renderer_ = nullptr;
	SDL_Texture* texture_ = nullptr;
	Image displayImage_;
	SDL_Texture* displayTexture_ = nullptr;
	int displayTextureWidth_ = 0;
	int displayTextureHeight_ = 0;
	Image transitionImage_;
	SDL_Texture* transitionTexture_ = nullptr;
	double zoom_ = 1.0;
	double offsetX_ = 0.0;
	double offsetY_ = 0.0;
	bool fitToWindow_ = true;
	bool fillWithCrop_ = false;
	bool autoZoomNoEnlarge_ = false;
	bool fullscreen_ = false;
	bool maximized_ = false;
	bool borderless_ = false;
	bool alwaysOnTop_ = false;
	bool dragging_ = false;
	bool controlsVisible_ = true;
	bool navigationPanelEnabled_ = true;
	bool infoVisible_ = false;
	bool showFileName_ = false;
	bool confirmationOpen_ = false;
	int confirmationCommand_ = 0;
	std::string confirmationMessage_;
	bool aboutOpen_ = false;
	jpegview_linux::ExifInfo metadata_;
	std::string jpegComment_;
	bool contextMenuOpen_ = false;
	int contextMenuX_ = 0;
	int contextMenuY_ = 0;
	std::size_t contextMenuScroll_ = 0;
	int menuSelected_ = -1;
	std::vector<MenuItem> contextMenuItems_;
	std::vector<OpenWithApplication> openWithApplications_;
	std::deque<std::string> openWithLabels_;
	bool imageModified_ = false;
	bool autoContrastEnabled_ = false;
	bool quitRequested_ = false;
	bool fileDialogOpen_ = false;
	bool fileDialogSave_ = false;
	bool fileDialogSaveFullSize_ = true;
	bool fileDialogOverwriteConfirmed_ = false;
	fs::path fileDialogDirectory_;
	std::string fileDialogFilename_;
	std::string fileDialogMessage_;
	std::vector<FileDialogEntry> fileDialogEntries_;
	int fileDialogSelected_ = 0;
	int fileDialogScroll_ = 0;
	bool batchCopyOpen_ = false;
	bool batchCopyPatternFocused_ = false;
	std::string copyRenamePattern_;
	std::string batchCopyPattern_;
	std::string batchCopyMessage_;
	std::vector<BatchCopyItem> batchCopyEntries_;
	std::size_t batchCopyScroll_ = 0;
	int batchCopyCursor_ = 0;
	bool resizeDialogOpen_ = false;
	int resizeField_ = kResizePercent;
	bool resizeInputPrimed_ = false;
	int resizeOriginalWidth_ = 0;
	int resizeOriginalHeight_ = 0;
	int resizeFilter_ = 2;
	std::string resizePercentText_;
	std::string resizeWidthText_;
	std::string resizeHeightText_;
	std::string resizeMessage_;
	std::vector<std::string> pendingDroppedFiles_;
	std::unique_ptr<jpegview_linux::FileList> fileListBeforeClipboard_;
	fs::path clipboardTempFile_;
	fs::path clipboardTempDirectory_;
	bool clipboardMode_ = false;
	int lastMouseX_ = kDefaultWidth / 2;
	int lastMouseY_ = kDefaultHeight / 2;
	int imageCenterX_ = kDefaultWidth / 2;
	int imageCenterY_ = kDefaultHeight / 2;
};

void PrintUsage(const char* program) {
	std::cout << "Usage: " << program << " [options] [image-or-directory ...]\n\n"
		<< "Options:\n"
		<< "  --fullscreen       Start fullscreen\n"
		<< "  --slideshow N      Advance every N seconds\n"
		<< "  --decode-check     Decode inputs and exit (useful for CI)\n"
		<< "  --help             Show this help\n\n"
		<< "Controls: Right/Left navigate, Up/Down rotate, mouse wheel zooms, left-drag pans, drop files to open,\n"
		<< "          Space toggles fit/actual, Enter fits, 0 fits, 1-9 start a slideshow, F11/F fullscreen,\n"
		<< "          F7/F8/F9 select folder/recursive/sibling navigation, N/M/C/Z select display order,\n"
		<< "          F2 toggles picture information, Ctrl+O opens, Ctrl+S saves full size, Ctrl+Shift+S saves screen size, Ctrl+R reloads, Ctrl+N toggles the navigation panel,\n"
		<< "          right-click opens the context menu, Esc or Q quits.\n";
}

} // namespace

int main(int argc, char** argv) {
	bool startFullscreen = false;
	double slideshowSeconds = 0.0;
	bool decodeCheck = false;
	std::vector<std::string> inputs;
	for (int argument = 1; argument < argc; ++argument) {
		const std::string value = argv[argument];
		if (value == "--help" || value == "-h") {
			PrintUsage(argv[0]);
			return 0;
		}
		if (value == "--fullscreen" || value == "-f") {
			startFullscreen = true;
			continue;
		}
		if (value == "--slideshow") {
			if (argument + 1 >= argc) {
				std::cerr << "--slideshow requires a positive duration in seconds.\n";
				return 2;
			}
			try {
				const std::string duration = argv[++argument];
				std::size_t parsedCharacters = 0;
				const double parsedDuration = std::stod(duration, &parsedCharacters);
				if (parsedCharacters != duration.size() || !std::isfinite(parsedDuration) || parsedDuration <= 0.0) {
					throw std::invalid_argument("invalid slideshow duration");
				}
				slideshowSeconds = std::max(0.1, parsedDuration);
			} catch (const std::exception&) {
				std::cerr << "--slideshow requires a positive duration in seconds.\n";
				return 2;
			}
			continue;
		}
		if (value == "--decode-check") {
			decodeCheck = true;
			continue;
		}
		if (!value.empty() && value[0] == '-') {
			std::cerr << "Unknown option: " << value << '\n';
			PrintUsage(argv[0]);
			return 2;
		}
		inputs.push_back(value);
	}

	jpegview_linux::FileList fileList(inputs);
	if (fileList.Empty()) {
		std::cerr << "No supported images found.\n";
		return 2;
	}

	if (decodeCheck) {
		for (const fs::path& file : fileList.Files()) {
			jpegview_linux::DecodedImage decoded;
			std::string errorMessage;
			if (!jpegview_linux::DecodeImage(file, decoded, errorMessage) || decoded.frames.empty()) {
				std::cerr << file << ": " << errorMessage << '\n';
				return 1;
			}
			const jpegview_linux::DecodedFrame& firstFrame = decoded.frames.front();
			std::cout << file << ": " << firstFrame.width << 'x' << firstFrame.height;
			if (decoded.frames.size() > 1) {
				std::cout << " (" << decoded.frames.size() << " frames";
				if (decoded.animation) std::cout << ", animation";
				std::cout << ')';
			}
			std::cout << '\n';
		}
		return 0;
	}

	Viewer viewer(std::move(fileList), slideshowSeconds, startFullscreen);
	const int result = viewer.Run();
	return result;
}
