#include "image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>

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
bool Image::AutoContrast() {
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

} // namespace jpegview_linux
