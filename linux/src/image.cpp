#include "image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <vector>

namespace jpegview_linux {
namespace {

constexpr int kResizeFilterCount = 4;
constexpr std::uint64_t kMaxImagePixels = 100ull * 1024ull * 1024ull;
constexpr int kMaxImageDimension = 65535;

bool HasValidPixels(const Image& image) {
	if (image.width <= 0 || image.height <= 0 ||
		image.width > kMaxImageDimension || image.height > kMaxImageDimension) return false;
	const std::uint64_t pixelCount = static_cast<std::uint64_t>(image.width) * image.height;
	return pixelCount <= kMaxImagePixels && pixelCount <= std::numeric_limits<std::size_t>::max() / 4 &&
		image.bgra.size() == static_cast<std::size_t>(pixelCount) * 4;
}

std::array<std::uint8_t, 256> CreateToneLut(double contrast, double gamma) {
	contrast = std::clamp(contrast, -0.5, 0.5);
	gamma = std::clamp(gamma, 0.1, 10.0);
	std::array<std::uint8_t, 256> lut{};
	double x = 0.0;
	const double increment = 1.0 / 255.0;
	for (int index = 0; index < 256; ++index, x += increment) {
		double value = x;
		if (contrast >= 0.0) {
			const double adjusted = contrast * 0.5;
			const double a = 1.0 / (1.0 - adjusted);
			const double b = -a * adjusted / 2.0;
			const double start = (0.25 - b) / a;
			const double end = (0.75 - b) / a;
			const double quadraticA = (a * start - 0.25) / (start * start);
			const double quadraticB = a - 2.0 * quadraticA * start;
			if (x < start) value = quadraticA * x * x + quadraticB * x;
			else if (x > end) value = 1.0 - (quadraticA * (1.0 - x) * (1.0 - x) + quadraticB * (1.0 - x));
			else value = a * x + b;
		} else {
			const double factor = -contrast * 1.5;
			if (x < 0.5) value = x + factor * (0.25 * 0.25 - (x - 0.25) * (x - 0.25));
			else value = x - factor * (0.25 * 0.25 - (x - 0.75) * (x - 0.75));
		}
		lut[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(std::clamp(
			std::lround(255.0 * std::pow(std::clamp(value, 0.0, 1.0), gamma)), 0l, 255l));
	}
	return lut;
}

struct LocalDensityMap {
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> values;
};

LocalDensityMap CreateLocalDensityMap(const Image& image) {
	LocalDensityMap map;
	map.width = std::clamp(image.width / 32, 1, 256);
	map.height = std::clamp(image.height / 32, 1, 256);
	if (image.width > 2) map.width = std::max(2, map.width);
	if (image.height > 2) map.height = std::max(2, map.height);
	map.width = std::min(map.width, image.width);
	map.height = std::min(map.height, image.height);
	map.values.resize(static_cast<std::size_t>(map.width) * map.height);
	for (int my = 0; my < map.height; ++my) {
		const int y0 = my * image.height / map.height;
		const int y1 = std::max(y0 + 1, (my + 1) * image.height / map.height);
		for (int mx = 0; mx < map.width; ++mx) {
			const int x0 = mx * image.width / map.width;
			const int x1 = std::max(x0 + 1, (mx + 1) * image.width / map.width);
			std::uint64_t sumBlue = 0, sumGreen = 0, sumRed = 0, count = 0;
			for (int y = y0; y < std::min(y1, image.height); ++y) {
				for (int x = x0; x < std::min(x1, image.width); ++x) {
					const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
					sumBlue += image.bgra[offset];
					sumGreen += image.bgra[offset + 1];
					sumRed += image.bgra[offset + 2];
					++count;
				}
			}
			const int blue = static_cast<int>(sumBlue / count);
			const int green = static_cast<int>(sumGreen / count);
			const int red = static_cast<int>(sumRed / count);
			int grey = (blue * 256 + green * 512 + red * 256) >> 10;
			const int blueCast = blue - std::max(green, red);
			if (blueCast > 0 && grey < 128) {
				if (blueCast <= 30) grey += (128 - grey) * blueCast / 30;
				else grey += (128 - grey) * (blueCast - 255) / (30 - 255);
			}
			const double distance = std::abs(grey - 127.5) / 127.5;
			const double response = 127.0 + (grey < 128 ? 1.0 : -1.0) *
				std::pow(distance, 2.5) * 127.5;
			map.values[static_cast<std::size_t>(my) * map.width + mx] =
				static_cast<std::uint8_t>(std::clamp(std::lround(response), 0l, 255l));
		}
	}
	if (map.width > 2 && map.height > 2) {
		std::vector<std::uint8_t> horizontal(map.values.size());
		for (int y = 0; y < map.height; ++y) for (int x = 0; x < map.width; ++x) {
			const auto at = [&](int xx) { return map.values[static_cast<std::size_t>(y) * map.width + xx]; };
			const int left = std::max(0, x - 1), right = std::min(map.width - 1, x + 1);
			horizontal[static_cast<std::size_t>(y) * map.width + x] = static_cast<std::uint8_t>(
				(x == 0 ? at(x) * 3 + at(right) : x == map.width - 1 ? at(x) * 3 + at(left) :
				at(left) + at(x) * 6 + at(right)) / (x == 0 || x == map.width - 1 ? 4 : 8));
		}
		for (int y = 0; y < map.height; ++y) for (int x = 0; x < map.width; ++x) {
			const int top = std::max(0, y - 1), bottom = std::min(map.height - 1, y + 1);
			const auto at = [&](int yy) { return horizontal[static_cast<std::size_t>(yy) * map.width + x]; };
			map.values[static_cast<std::size_t>(y) * map.width + x] = static_cast<std::uint8_t>(
				(y == 0 ? at(y) * 3 + at(bottom) : y == map.height - 1 ? at(y) * 3 + at(top) :
				at(top) + at(y) * 6 + at(bottom)) / (y == 0 || y == map.height - 1 ? 4 : 8));
		}
	}
	return map;
}

int SampleDensityMap(const LocalDensityMap& map, int x, int y, int imageWidth, int imageHeight) {
	if (map.width <= 1 || map.height <= 1) {
		return map.values.empty() ? 127 : map.values.front();
	}
	const double fx = static_cast<double>(x) * (map.width - 1) / std::max(1, imageWidth - 1);
	const double fy = static_cast<double>(y) * (map.height - 1) / std::max(1, imageHeight - 1);
	const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
	const int x1 = std::min(map.width - 1, x0 + 1), y1 = std::min(map.height - 1, y0 + 1);
	const double tx = fx - x0, ty = fy - y0;
	const double top = map.values[static_cast<std::size_t>(y0) * map.width + x0] * (1.0 - tx) +
		map.values[static_cast<std::size_t>(y0) * map.width + x1] * tx;
	const double bottom = map.values[static_cast<std::size_t>(y1) * map.width + x0] * (1.0 - tx) +
		map.values[static_cast<std::size_t>(y1) * map.width + x1] * tx;
	return static_cast<int>(std::lround(top * (1.0 - ty) + bottom * ty));
}

bool ApplyUnsharpMask(Image& image, const ImageProcessingParams& params) {
	const double radius = std::clamp(params.unsharpRadius, 0.0, 5.0);
	const double amount = std::clamp(params.unsharpAmount, 0.0, 10.0);
	const double threshold = std::clamp(params.unsharpThreshold, 0.0, 20.0);
	if (radius <= 0.0 || amount <= 0.0 || image.width <= 0 || image.height <= 0) return true;
	const std::size_t pixelCount = static_cast<std::size_t>(image.width) * image.height;
	std::vector<std::uint8_t> horizontal;
	try {
		horizontal.resize(pixelCount);
	} catch (const std::exception&) {
		return false;
	}
	const int kernelRadius = std::max(1, static_cast<int>(std::ceil(radius * 2.0)));
	const double sigma = std::max(0.35, radius);
	std::vector<double> kernel(static_cast<std::size_t>(kernelRadius * 2 + 1));
	double weightSum = 0.0;
	for (int delta = -kernelRadius; delta <= kernelRadius; ++delta) {
		const double weight = std::exp(-(delta * delta) / (2.0 * sigma * sigma));
		kernel[static_cast<std::size_t>(delta + kernelRadius)] = weight;
		weightSum += weight;
	}
	for (double& weight : kernel) weight /= weightSum;
	const auto grayAt = [&image](int x, int y) {
		const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
		return image.bgra[offset] * 0.114 + image.bgra[offset + 1] * 0.587 + image.bgra[offset + 2] * 0.299;
	};
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			double smooth = 0.0;
			for (int delta = -kernelRadius; delta <= kernelRadius; ++delta) {
				const int sampleX = std::clamp(x + delta, 0, image.width - 1);
				smooth += kernel[static_cast<std::size_t>(delta + kernelRadius)] * grayAt(sampleX, y);
			}
			horizontal[static_cast<std::size_t>(y) * image.width + x] =
				static_cast<std::uint8_t>(std::clamp(std::lround(smooth), 0l, 255l));
		}
	}
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			double smooth = 0.0;
			for (int delta = -kernelRadius; delta <= kernelRadius; ++delta) {
				const int sampleY = std::clamp(y + delta, 0, image.height - 1);
				smooth += kernel[static_cast<std::size_t>(delta + kernelRadius)] *
					horizontal[static_cast<std::size_t>(sampleY) * image.width + x];
			}
			const double difference = grayAt(x, y) - smooth;
			const double magnitude = std::abs(difference);
			if (magnitude <= threshold) continue;
			const double effectiveDifference = std::copysign(magnitude - threshold, difference);
			const double factor = effectiveDifference * amount / 255.0;
			const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
			for (int channel = 0; channel < 3; ++channel) {
				image.bgra[offset + static_cast<std::size_t>(channel)] =
					static_cast<std::uint8_t>(std::clamp(std::lround(
						image.bgra[offset + static_cast<std::size_t>(channel)] * (1.0 + factor)), 0l, 255l));
			}
		}
	}
	return true;
}

} // namespace

bool Image::StoreBGRA(const std::uint8_t* bgraPixels, int imageWidth, int imageHeight) {
	if (bgraPixels == nullptr || imageWidth <= 0 || imageHeight <= 0 ||
		imageWidth > kMaxImageDimension || imageHeight > kMaxImageDimension) return false;
	const std::size_t pixelCount = static_cast<std::size_t>(imageWidth) *
		static_cast<std::size_t>(imageHeight);
	if (pixelCount > kMaxImagePixels || pixelCount > std::numeric_limits<std::size_t>::max() / 4) return false;
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

bool Image::Rotate(bool clockwise) {
	if (!HasValidPixels(*this)) return false;
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

bool Image::Mirror(bool horizontal) {
	if (!HasValidPixels(*this)) return false;
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
bool Image::Resize(int newWidth, int newHeight, int filter) {
	if (!HasValidPixels(*this) || newWidth <= 0 || newHeight <= 0) return false;
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
bool Image::AutoContrast(double colorCorrection, double contrastCorrection) {
	if (!HasValidPixels(*this)) return false;

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
	const double contrastFactor = std::clamp(0.5 + contrastCorrection * 0.5, 0.05, 1.0);
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
	colorCorrection = std::clamp(colorCorrection, -0.5, 0.5);
	const double castMultiplier = colorCorrection > 0.0 ?
		2.0 * (2.0 * castFactor - 1.0) * colorCorrection + 1.0 :
		2.0 * (castFactor + 1.0) * colorCorrection + 1.0;
	const double yMean = correctionA * midPoint + correctionB;
	const auto bwCompensation = [midPoint](double black, double white) {
		const double a = 1.0 / std::max(0.001, white - black);
		return a * midPoint - black * a;
	};
	double correctionBlue = yMean - bwCompensation(blackB, whiteB) - castMultiplier * castB;
	double correctionGreen = yMean - bwCompensation(blackG, whiteG) - castMultiplier * castG;
	double correctionRed = yMean - bwCompensation(blackR, whiteR) - castMultiplier * castR;
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

bool Image::ApplyProcessing(const ImageProcessingParams& params, bool autoContrast) {
	if (!HasValidPixels(*this)) return false;
	if (autoContrast && !AutoContrast(params.colorCorrection, params.contrastCorrection)) return false;
	const bool localDensityActive = params.localDensityEnabled &&
		(params.lightenShadows > 0.0 || params.darkenHighlights > 0.0);
	const bool hasLevelChanges = std::abs(params.contrast) > 1e-9 ||
		std::abs(params.gamma - 1.0) > 1e-9 || std::abs(params.saturation - 1.0) > 1e-9 ||
		std::abs(params.cyanRed) > 1e-9 || std::abs(params.magentaGreen) > 1e-9 ||
		std::abs(params.yellowBlue) > 1e-9 || localDensityActive ||
		std::abs(params.sharpen) > 1e-9;
	if (!hasLevelChanges && !(params.unsharpRadius > 0.0 && params.unsharpAmount > 0.0)) return true;
	if (!hasLevelChanges) return ApplyUnsharpMask(*this, params);

	const std::array<std::uint8_t, 256> toneLut = CreateToneLut(params.contrast, params.gamma);
	const double saturation = std::clamp(params.saturation, 0.0, 2.0);
	constexpr double scale = 65536.0;
	std::array<std::array<std::int32_t, 256>, 6> saturationLut{};
	for (int value = 0; value < 256; ++value) {
		saturationLut[0][value] = static_cast<std::int32_t>(std::lround(value * (0.299 + 0.701 * saturation) * scale));
		saturationLut[1][value] = static_cast<std::int32_t>(std::lround(value * 0.587 * (1.0 - saturation) * scale));
		saturationLut[2][value] = static_cast<std::int32_t>(std::lround(value * 0.114 * (1.0 - saturation) * scale));
		saturationLut[3][value] = static_cast<std::int32_t>(std::lround(value * 0.299 * (1.0 - saturation) * scale));
		saturationLut[4][value] = static_cast<std::int32_t>(std::lround(value * (0.587 + 0.413 * saturation) * scale));
		saturationLut[5][value] = static_cast<std::int32_t>(std::lround(value * (0.114 + 0.886 * saturation) * scale));
	}
	const double cyanRed = std::clamp(params.cyanRed, -1.0, 1.0);
	const double magentaGreen = std::clamp(params.magentaGreen, -1.0, 1.0);
	const double yellowBlue = std::clamp(params.yellowBlue, -1.0, 1.0);
	const auto colorCast = [cyanRed, magentaGreen, yellowBlue](int channel, int value) {
		const double midtone = 1.0 - std::pow((value - 127.5) / 127.5, 2.0);
		double amount = 0.0;
		if (channel == 2) amount = 0.18 * cyanRed + 0.12 * yellowBlue - 0.06 * magentaGreen;
		else if (channel == 1) amount = 0.18 * magentaGreen + 0.12 * yellowBlue - 0.06 * cyanRed;
		else amount = -0.18 * yellowBlue - 0.06 * cyanRed - 0.06 * magentaGreen;
		return static_cast<int>(std::lround(amount * midtone * 255.0));
	};

	LocalDensityMap densityMap;
	std::array<int, 256> densityResponse{};
	int blackPoint = 0, whitePoint = 255;
	if (params.localDensityEnabled && (params.lightenShadows > 0.0 || params.darkenHighlights > 0.0)) {
		densityMap = CreateLocalDensityMap(*this);
		std::array<std::size_t, 256> histogram{};
		for (std::size_t offset = 0; offset < bgra.size(); offset += 4) {
			const int grey = (bgra[offset] * 128 + bgra[offset + 1] * 640 + bgra[offset + 2] * 256) >> 10;
			++histogram[static_cast<std::size_t>(grey)];
		}
		const std::size_t clipping = static_cast<std::size_t>(bgra.size() / 4 / 100);
		std::size_t accumulated = 0;
		for (blackPoint = 0; blackPoint < 255; ++blackPoint) {
			accumulated += histogram[static_cast<std::size_t>(blackPoint)];
			if (accumulated > clipping) break;
		}
		accumulated = 0;
		for (whitePoint = 255; whitePoint > 0; --whitePoint) {
			accumulated += histogram[static_cast<std::size_t>(whitePoint)];
			if (accumulated > clipping) break;
		}
		const double black = blackPoint / 255.0;
		const double white = whitePoint / 255.0;
		const double steepness = std::clamp(1.0 - 0.98 * params.deepShadows, 0.02, 1.0);
		for (int value = 0; value < 256; ++value) {
			const double x = value / 255.0;
			double response = 0.0;
			if (white > black && x >= black && x < white) {
				const double middle = (black + white) * 0.5;
				const double endBlack = black + 0.6 * (middle - black);
				const double endWhite = white - 0.6 * (white - middle);
				if (x < endBlack) response = 0.8 * std::pow((x - black) / std::max(1e-6, endBlack - black), steepness);
				else if (x > endWhite) response = 0.8 * (1.0 - (x - endWhite) / std::max(1e-6, white - endWhite));
				else response = 0.8;
			}
			densityResponse[static_cast<std::size_t>(value)] = static_cast<int>(std::lround(response * 16384.0));
		}
	}

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
			const int blue = bgra[offset], green = bgra[offset + 1], red = bgra[offset + 2];
			int outRed = static_cast<int>(std::lround((saturationLut[0][red] + saturationLut[1][green] + saturationLut[2][blue]) / scale));
			int outGreen = static_cast<int>(std::lround((saturationLut[3][red] + saturationLut[4][green] + saturationLut[2][blue]) / scale));
			int outBlue = static_cast<int>(std::lround((saturationLut[3][red] + saturationLut[1][green] + saturationLut[5][blue]) / scale));
			outBlue = toneLut[static_cast<std::size_t>(std::clamp(outBlue, 0, 255))];
			outGreen = toneLut[static_cast<std::size_t>(std::clamp(outGreen, 0, 255))];
			outRed = toneLut[static_cast<std::size_t>(std::clamp(outRed, 0, 255))];
			outRed = std::clamp(outRed + colorCast(2, outRed), 0, 255);
			outGreen = std::clamp(outGreen + colorCast(1, outGreen), 0, 255);
			outBlue = std::clamp(outBlue + colorCast(0, outBlue), 0, 255);
			if (!densityMap.values.empty()) {
				const int mask = SampleDensityMap(densityMap, x, y, width, height) - 127;
				const int strength = mask >= 0 ? static_cast<int>(std::lround(params.lightenShadows * 65536.0)) :
					static_cast<int>(std::lround(params.darkenHighlights * 65536.0));
				const auto applyDensity = [&](int value) {
					const std::int64_t change = static_cast<std::int64_t>(mask) *
						densityResponse[static_cast<std::size_t>(value)] * strength;
					const int adjusted = value + static_cast<int>(change >> 30);
					return std::clamp(adjusted, 0, 255);
				};
				outBlue = applyDensity(outBlue);
				outGreen = applyDensity(outGreen);
				outRed = applyDensity(outRed);
			}
			bgra[offset] = static_cast<std::uint8_t>(outBlue);
			bgra[offset + 1] = static_cast<std::uint8_t>(outGreen);
			bgra[offset + 2] = static_cast<std::uint8_t>(outRed);
		}
	}

	const double sharpen = std::clamp(params.sharpen, 0.0, 0.5);
	if (sharpen > 0.0 && width > 2 && height > 2) {
		const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
		std::vector<std::uint8_t> previous(rowBytes), current(rowBytes), next(rowBytes);
		std::copy_n(bgra.data(), rowBytes, previous.data());
		std::copy_n(bgra.data() + rowBytes, rowBytes, current.data());
		for (int y = 1; y < height - 1; ++y) {
			std::copy_n(bgra.data() + static_cast<std::size_t>(y + 1) * rowBytes, rowBytes, next.data());
			for (int x = 1; x < width - 1; ++x) for (int channel = 0; channel < 3; ++channel) {
				const std::size_t at = static_cast<std::size_t>(x) * 4 + channel;
				const int center = current[at];
				const int blur = (previous[at] + current[at - 4] + current[at] * 4 +
					current[at + 4] + next[at]) / 8;
				const std::size_t target = static_cast<std::size_t>(y) * rowBytes + at;
				bgra[target] = static_cast<std::uint8_t>(std::clamp(
					static_cast<int>(std::lround(center + (center - blur) * sharpen)), 0, 255));
			}
			previous.swap(current);
			current.swap(next);
		}
	}
	return ApplyUnsharpMask(*this, params);
}

} // namespace jpegview_linux
