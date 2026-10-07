#include "image_transform_pixels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr int kMaximumImageDimension = 65535;
constexpr std::uint64_t kMaximumImagePixels = 100ull * 1024ull * 1024ull;
constexpr double kAlphaEpsilon = 1e-8;
constexpr double kProjectiveDenominatorRelativeTolerance = 1e-12;
constexpr double kMinimumNormalizedDeterminant = 1e-16;

bool Continue(const std::function<bool()>& shouldContinue) {
	return !shouldContinue || shouldContinue();
}

bool HasValidPixels(const Image& image) {
	if (image.width <= 0 || image.height <= 0 ||
		image.width > kMaximumImageDimension || image.height > kMaximumImageDimension) {
		return false;
	}
	const std::uint64_t pixelCount = static_cast<std::uint64_t>(image.width) * image.height;
	return pixelCount <= kMaximumImagePixels &&
		pixelCount <= std::numeric_limits<std::size_t>::max() / 4 &&
		image.bgra.size() == static_cast<std::size_t>(pixelCount) * 4;
}

bool ValidGeometry(const ImageTransformGeometry& geometry) {
	if (geometry.sourceWidth <= 0 || geometry.sourceHeight <= 0 ||
		geometry.outputWidth <= 0 || geometry.outputHeight <= 0 ||
		geometry.sourceWidth > kMaximumImageDimension ||
		geometry.sourceHeight > kMaximumImageDimension ||
		geometry.outputWidth > kMaximumImageDimension ||
		geometry.outputHeight > kMaximumImageDimension ||
		static_cast<std::uint64_t>(geometry.outputWidth) * geometry.outputHeight >
			kMaximumImagePixels ||
		static_cast<std::uint64_t>(geometry.outputWidth) * geometry.outputHeight >
			std::numeric_limits<std::size_t>::max() / 4) return false;
	for (double value : geometry.destinationToSource) {
		if (!std::isfinite(value)) return false;
	}
	return geometry.exactClockwiseQuarterTurns >= -1;
}

bool NormalizeMappingMatrix(const ImageTransformGeometry& geometry,
	std::array<double, 9>& normalized) {
	double scale = 0.0;
	for (double value : geometry.destinationToSource) {
		scale = std::max(scale, std::abs(value));
	}
	if (!std::isfinite(scale) || scale == 0.0) return false;
	for (std::size_t index = 0; index < normalized.size(); ++index) {
		normalized[index] = geometry.destinationToSource[index] / scale;
	}
	return true;
}

bool HasNonsingularMapping(const std::array<double, 9>& normalized) {
	const double determinant =
		normalized[0] * (normalized[4] * normalized[8] - normalized[5] * normalized[7]) -
		normalized[1] * (normalized[3] * normalized[8] - normalized[5] * normalized[6]) +
		normalized[2] * (normalized[3] * normalized[7] - normalized[4] * normalized[6]);
	return std::isfinite(determinant) &&
		std::abs(determinant) > kMinimumNormalizedDeterminant;
}

bool ValidProjectiveMapping(const ImageTransformGeometry& geometry) {
	std::array<double, 9> matrix{};
	if (!NormalizeMappingMatrix(geometry, matrix)) return false;
	const double maximumX = geometry.outputWidth - 1.0;
	const double maximumY = geometry.outputHeight - 1.0;
	const double denominatorScale = std::abs(matrix[6]) * maximumX +
		std::abs(matrix[7]) * maximumY + std::abs(matrix[8]);
	if (!std::isfinite(denominatorScale) || denominatorScale <= 0.0) return false;
	const double minimumSafeDenominator =
		std::max(1.0, denominatorScale) * kProjectiveDenominatorRelativeTolerance;
	const std::array<std::pair<double, double>, 4> corners = {{{0.0, 0.0},
		{maximumX, 0.0}, {maximumX, maximumY}, {0.0, maximumY}}};
	int denominatorSign = 0;
	for (const auto& corner : corners) {
		const double x = corner.first;
		const double y = corner.second;
		const double denominator = matrix[6] * x + matrix[7] * y + matrix[8];
		if (!std::isfinite(denominator) ||
			std::abs(denominator) <= minimumSafeDenominator) return false;
		const int sign = denominator > 0.0 ? 1 : -1;
		if (denominatorSign != 0 && sign != denominatorSign) return false;
		denominatorSign = sign;
		const double mappedX = (matrix[0] * x + matrix[1] * y + matrix[2]) /
			denominator;
		const double mappedY = (matrix[3] * x + matrix[4] * y + matrix[5]) /
			denominator;
		if (!std::isfinite(mappedX) || !std::isfinite(mappedY)) return false;
	}
	// A one-pixel output axis intentionally samples a line or point through the
	// source and therefore has a rank-deficient composed mapping.
	return geometry.outputWidth == 1 || geometry.outputHeight == 1 ||
		HasNonsingularMapping(matrix);
}

double CubicWeight(double distance) {
	constexpr double parameter = -0.5;
	const double value = std::abs(distance);
	if (value < 1.0) {
		return (parameter + 2.0) * value * value * value -
			(parameter + 3.0) * value * value + 1.0;
	}
	if (value < 2.0) {
		return parameter * value * value * value - 5.0 * parameter * value * value +
			8.0 * parameter * value - 4.0 * parameter;
	}
	return 0.0;
}

struct PremultipliedSample {
	std::array<double, 3> color{};
	double alpha = 0.0;
};

void AccumulatePixel(const Image& source, int x, int y, double weight,
	PremultipliedSample& sample) {
	if (weight == 0.0 || x < 0 || y < 0 || x >= source.width || y >= source.height) return;
	const std::size_t offset = (static_cast<std::size_t>(y) * source.width + x) * 4;
	const double alpha = source.bgra[offset + 3] / 255.0;
	sample.alpha += weight * alpha;
	for (std::size_t channel = 0; channel < sample.color.size(); ++channel) {
		sample.color[channel] += weight * alpha * source.bgra[offset + channel];
	}
}

std::uint8_t ToByte(double value) {
	return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0l, 255l));
}

void StoreSample(const PremultipliedSample& sample, std::uint8_t* output) {
	const double alpha = std::clamp(sample.alpha, 0.0, 1.0);
	if (alpha <= kAlphaEpsilon || sample.alpha <= kAlphaEpsilon) {
		output[0] = output[1] = output[2] = output[3] = 0;
		return;
	}
	for (std::size_t channel = 0; channel < sample.color.size(); ++channel) {
		output[channel] = ToByte(sample.color[channel] / sample.alpha);
	}
	output[3] = ToByte(alpha * 255.0);
}

bool SampleBilinear(const Image& source, double x, double y, std::uint8_t* output) {
	if (!std::isfinite(x) || !std::isfinite(y) ||
		x < -1.0 || y < -1.0 || x > source.width || y > source.height) {
		output[0] = output[1] = output[2] = output[3] = 0;
		return true;
	}
	const int left = static_cast<int>(std::floor(x));
	const int top = static_cast<int>(std::floor(y));
	const double fractionX = x - left;
	const double fractionY = y - top;
	PremultipliedSample sample;
	AccumulatePixel(source, left, top, (1.0 - fractionX) * (1.0 - fractionY), sample);
	AccumulatePixel(source, left + 1, top, fractionX * (1.0 - fractionY), sample);
	AccumulatePixel(source, left, top + 1, (1.0 - fractionX) * fractionY, sample);
	AccumulatePixel(source, left + 1, top + 1, fractionX * fractionY, sample);
	StoreSample(sample, output);
	return true;
}

bool SampleBicubic(const Image& source, double x, double y, std::uint8_t* output) {
	if (!std::isfinite(x) || !std::isfinite(y) ||
		x <= -2.0 || y <= -2.0 || x >= source.width + 1.0 ||
		y >= source.height + 1.0) {
		output[0] = output[1] = output[2] = output[3] = 0;
		return true;
	}
	const int left = static_cast<int>(std::floor(x));
	const int top = static_cast<int>(std::floor(y));
	PremultipliedSample sample;
	for (int offsetY = -1; offsetY <= 2; ++offsetY) {
		const int sourceY = top + offsetY;
		const double weightY = CubicWeight(y - sourceY);
		for (int offsetX = -1; offsetX <= 2; ++offsetX) {
			const int sourceX = left + offsetX;
			const double weight = weightY * CubicWeight(x - sourceX);
			AccumulatePixel(source, sourceX, sourceY, weight, sample);
		}
	}
	StoreSample(sample, output);
	return true;
}

bool CopyExactTurn(const Image& source, const ImageTransformGeometry& geometry,
	Image& result, const std::function<bool()>& shouldContinue) {
	const int turns = geometry.exactClockwiseQuarterTurns;
	for (int y = 0; y < geometry.outputHeight; ++y) {
		if (!Continue(shouldContinue)) return false;
		for (int x = 0; x < geometry.outputWidth; ++x) {
			if (x > 0 && (x & 4095) == 0 && !Continue(shouldContinue)) return false;
			int sourceX = x;
			int sourceY = y;
			switch (turns) {
			case 1:
				sourceX = y;
				sourceY = source.height - 1 - x;
				break;
			case 2:
				sourceX = source.width - 1 - x;
				sourceY = source.height - 1 - y;
				break;
			case 3:
				sourceX = source.width - 1 - y;
				sourceY = x;
				break;
			default:
				break;
			}
			const std::size_t sourceOffset =
				(static_cast<std::size_t>(sourceY) * source.width + sourceX) * 4;
			const std::size_t outputOffset =
				(static_cast<std::size_t>(y) * result.width + x) * 4;
			std::copy_n(source.bgra.data() + sourceOffset, 4,
				result.bgra.data() + outputOffset);
		}
	}
	return Continue(shouldContinue);
}

} // namespace

bool ResampleImageTransform(const Image& source,
	const ImageTransformGeometry& geometry, ImageTransformSampling sampling,
	Image& output, const std::function<bool()>& shouldContinue) {
	if (!HasValidPixels(source) || !ValidGeometry(geometry) ||
		geometry.sourceWidth != source.width || geometry.sourceHeight != source.height ||
		!Continue(shouldContinue) ||
		(sampling != ImageTransformSampling::PreviewBilinear &&
		 sampling != ImageTransformSampling::FinalBicubic)) return false;

	const bool exactTurn = geometry.exactClockwiseQuarterTurns >= 0;
	if (exactTurn) {
		const bool swapsDimensions = geometry.exactClockwiseQuarterTurns % 2 != 0;
		if (geometry.exactClockwiseQuarterTurns > 3 ||
			geometry.outputWidth != (swapsDimensions ? source.height : source.width) ||
			geometry.outputHeight != (swapsDimensions ? source.width : source.height)) return false;
	} else if (!ValidProjectiveMapping(geometry)) {
		return false;
	}

	Image result;
	result.width = geometry.outputWidth;
	result.height = geometry.outputHeight;
	result.originalWidth = source.originalWidth > 0 ? source.originalWidth : source.width;
	result.originalHeight = source.originalHeight > 0 ? source.originalHeight : source.height;
	result.hasTransparency = source.hasTransparency;
	const std::size_t outputBytes = static_cast<std::size_t>(result.width) *
		static_cast<std::size_t>(result.height) * 4;
	try {
		result.bgra.resize(outputBytes);
	} catch (const std::exception&) {
		return false;
	}

	if (exactTurn) {
		if (!CopyExactTurn(source, geometry, result, shouldContinue)) return false;
		output = std::move(result);
		return true;
	}

	const auto& matrix = geometry.destinationToSource;
	if (matrix[6] == 0.0 && matrix[7] == 0.0) {
		const double stepX = result.width > 1 ? matrix[0] / matrix[8] : 0.0;
		const double stepY = result.width > 1 ? matrix[3] / matrix[8] : 0.0;
		for (int y = 0; y < result.height; ++y) {
			if (!Continue(shouldContinue)) return false;
			double sourceX = (matrix[1] * y + matrix[2]) / matrix[8];
			double sourceY = (matrix[4] * y + matrix[5]) / matrix[8];
			for (int x = 0; x < result.width; ++x) {
				if (x > 0 && (x & 255) == 0 && !Continue(shouldContinue)) return false;
				const std::size_t offset = (static_cast<std::size_t>(y) * result.width + x) * 4;
				const bool sampled = sampling == ImageTransformSampling::PreviewBilinear ?
					SampleBilinear(source, sourceX, sourceY, result.bgra.data() + offset) :
					SampleBicubic(source, sourceX, sourceY, result.bgra.data() + offset);
				if (!sampled) return false;
				if (result.bgra[offset + 3] < 255) result.hasTransparency = true;
				sourceX += stepX;
				sourceY += stepY;
			}
		}
	} else {
		std::array<double, 9> normalizedMatrix{};
		if (!NormalizeMappingMatrix(geometry, normalizedMatrix)) return false;
		for (int y = 0; y < result.height; ++y) {
			if (!Continue(shouldContinue)) return false;
			double numeratorX = normalizedMatrix[1] * y + normalizedMatrix[2];
			double numeratorY = normalizedMatrix[4] * y + normalizedMatrix[5];
			double denominator = normalizedMatrix[7] * y + normalizedMatrix[8];
			for (int x = 0; x < result.width; ++x) {
				if (x > 0 && (x & 255) == 0 && !Continue(shouldContinue)) return false;
				if (!std::isfinite(numeratorX) || !std::isfinite(numeratorY) ||
					!std::isfinite(denominator) || denominator == 0.0) return false;
				const double sourceX = numeratorX / denominator;
				const double sourceY = numeratorY / denominator;
				if (!std::isfinite(sourceX) || !std::isfinite(sourceY)) return false;
				const std::size_t offset = (static_cast<std::size_t>(y) * result.width + x) * 4;
				const bool sampled = sampling == ImageTransformSampling::PreviewBilinear ?
					SampleBilinear(source, sourceX, sourceY, result.bgra.data() + offset) :
					SampleBicubic(source, sourceX, sourceY, result.bgra.data() + offset);
				if (!sampled) return false;
				if (result.bgra[offset + 3] < 255) result.hasTransparency = true;
				numeratorX += normalizedMatrix[0];
				numeratorY += normalizedMatrix[3];
				denominator += normalizedMatrix[6];
			}
		}
	}
	if (!Continue(shouldContinue)) return false;
	output = std::move(result);
	return true;
}

} // namespace jpegview_linux
