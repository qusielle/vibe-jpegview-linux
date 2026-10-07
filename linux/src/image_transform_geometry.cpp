#include "image_transform_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace jpegview_linux {
namespace {

constexpr int kMaximumImageDimension = 65535;
constexpr std::uint64_t kMaximumImagePixels = 100ull * 1024ull * 1024ull;
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kExactAngleTolerance = 1e-10;
constexpr double kGeometryEpsilon = 1e-12;

bool ValidDimensions(int width, int height) {
	return width > 0 && height > 0 && width <= kMaximumImageDimension &&
	height <= kMaximumImageDimension &&
	static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) <=
		kMaximumImagePixels;
}

bool ValidPerspectiveFraction(double fraction) {
	return std::isfinite(fraction) &&
		std::abs(fraction) <= kMaximumPerspectiveCorrectionFraction;
}

bool SafeExpandedBounds(double minimum, double maximum, int& origin, int& extent) {
	if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) return false;
	const double roundedMinimum = std::floor(minimum);
	const double roundedMaximum = std::ceil(maximum);
	const double span = roundedMaximum - roundedMinimum + 1.0;
	if (span < 1.0 || span > kMaximumImageDimension ||
		roundedMinimum < std::numeric_limits<int>::min() ||
		roundedMinimum > std::numeric_limits<int>::max()) return false;
	origin = static_cast<int>(roundedMinimum);
	extent = static_cast<int>(span);
	return true;
}

int SafeContainedExtent(double span) {
	if (!std::isfinite(span) || span < 0.0) return 0;
	const double extent = std::floor(span + kGeometryEpsilon) + 1.0;
	if (extent < 1.0 || extent > kMaximumImageDimension) return 0;
	return static_cast<int>(extent);
}

std::array<double, 9> MultiplyMatrices(const std::array<double, 9>& left,
	const std::array<double, 9>& right) {
	std::array<double, 9> product{};
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column) {
			for (int inner = 0; inner < 3; ++inner) {
				product[row * 3 + column] += left[row * 3 + inner] *
					right[inner * 3 + column];
			}
		}
	}
	return product;
}

int SafeExtent(double halfExtent, bool roundOutward) {
	if (!std::isfinite(halfExtent) || halfExtent < 0.0) return 0;
	const double span = 2.0 * halfExtent;
	const double rounded = (roundOutward ?
		std::ceil(span - kGeometryEpsilon) :
		std::floor(span + kGeometryEpsilon)) + 1.0;
	if (rounded > kMaximumImageDimension) return 0;
	return std::max(1, static_cast<int>(rounded));
}

struct HalfExtents {
	double width = 0.0;
	double height = 0.0;
};

HalfExtents ContainedHalfExtents(double halfSourceWidth, double halfSourceHeight,
	double cosine, double sine, double aspectRatio) {
	if (aspectRatio > 0.0) {
		const double halfHeight = std::min(
			halfSourceWidth / std::max(kGeometryEpsilon,
				cosine * aspectRatio + sine),
			halfSourceHeight / std::max(kGeometryEpsilon,
				sine * aspectRatio + cosine));
		return {aspectRatio * halfHeight, halfHeight};
	}

	const double maximumHalfHeight = std::min(
		sine > kGeometryEpsilon ? halfSourceWidth / sine :
			std::numeric_limits<double>::infinity(),
		cosine > kGeometryEpsilon ? halfSourceHeight / cosine :
			std::numeric_limits<double>::infinity());
	const auto areaAt = [halfSourceWidth, halfSourceHeight, cosine, sine](double y) {
		if (y < 0.0) return 0.0;
		double x = std::numeric_limits<double>::infinity();
		if (cosine > kGeometryEpsilon) x = std::min(x,
			(halfSourceWidth - sine * y) / cosine);
		if (sine > kGeometryEpsilon) x = std::min(x,
			(halfSourceHeight - cosine * y) / sine);
		return std::max(0.0, x) * y;
	};
	double bestY = 0.0;
	double bestArea = 0.0;
	const auto consider = [&](double y, double& selectedY, double& selectedArea) {
		if (!std::isfinite(y) || y < 0.0 || y > maximumHalfHeight + kGeometryEpsilon) return;
		const double area = areaAt(y);
		if (area > selectedArea) {
			selectedArea = area;
			selectedY = y;
		}
	};
	consider(maximumHalfHeight, bestY, bestArea);
	if (sine > kGeometryEpsilon) {
		consider(halfSourceWidth / (2.0 * sine), bestY, bestArea);
	}
	if (cosine > kGeometryEpsilon) {
		consider(halfSourceHeight / (2.0 * cosine), bestY, bestArea);
	}
	const double determinant = cosine * cosine - sine * sine;
	if (std::abs(determinant) > kGeometryEpsilon) {
		consider((cosine * halfSourceHeight - sine * halfSourceWidth) /
			determinant, bestY, bestArea);
	}
	if (bestArea <= 0.0) return {};
	double bestX = std::numeric_limits<double>::infinity();
	if (cosine > kGeometryEpsilon) {
		bestX = std::min(bestX, (halfSourceWidth - sine * bestY) / cosine);
	}
	if (sine > kGeometryEpsilon) {
		bestX = std::min(bestX, (halfSourceHeight - cosine * bestY) / sine);
	}
	if (!std::isfinite(bestX) || bestX < 0.0) return {};
	return {bestX, bestY};
}

} // namespace

bool BuildFreeRotationGeometry(int sourceWidth, int sourceHeight,
	double clockwiseDegrees, bool autoCrop, bool preserveAspectRatio,
	ImageTransformGeometry& geometry) {
	if (!ValidDimensions(sourceWidth, sourceHeight) ||
		!std::isfinite(clockwiseDegrees)) return false;

	const double normalizedDegrees = std::remainder(clockwiseDegrees, 360.0);
	const double quarterTurns = normalizedDegrees / 90.0;
	const double nearestQuarterTurn = std::round(quarterTurns);
	const bool exactQuarterTurn = std::abs(quarterTurns - nearestQuarterTurn) <=
		kExactAngleTolerance / 90.0;
	const int exactTurns = exactQuarterTurn ?
		(static_cast<int>(nearestQuarterTurn) % 4 + 4) % 4 : -1;
	const bool quarterTurnNeedsAspectCrop = exactQuarterTurn && exactTurns % 2 != 0 &&
		autoCrop && preserveAspectRatio && sourceWidth != sourceHeight;
	if (exactQuarterTurn && !quarterTurnNeedsAspectCrop) {
		ImageTransformGeometry built;
		built.sourceWidth = sourceWidth;
		built.sourceHeight = sourceHeight;
		built.outputWidth = exactTurns % 2 == 0 ? sourceWidth : sourceHeight;
		built.outputHeight = exactTurns % 2 == 0 ? sourceHeight : sourceWidth;
		switch (exactTurns) {
		case 0:
			built.destinationToSource = {1.0, 0.0, 0.0,
				0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
			break;
		case 1:
			built.destinationToSource = {0.0, 1.0, 0.0,
				-1.0, 0.0, static_cast<double>(sourceHeight - 1),
				0.0, 0.0, 1.0};
			break;
		case 2:
			built.destinationToSource = {-1.0, 0.0, static_cast<double>(sourceWidth - 1),
				0.0, -1.0, static_cast<double>(sourceHeight - 1),
				0.0, 0.0, 1.0};
			break;
		default:
			built.destinationToSource = {0.0, -1.0, static_cast<double>(sourceWidth - 1),
				1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
			break;
		}
		built.exactClockwiseQuarterTurns = exactTurns;
		geometry = built;
		return true;
	}

	const double radians = normalizedDegrees * kPi / 180.0;
	double cosine = std::cos(radians);
	double sine = std::sin(radians);
	if (std::abs(cosine) < kGeometryEpsilon) cosine = 0.0;
	if (std::abs(sine) < kGeometryEpsilon) sine = 0.0;
	const double absCosine = std::abs(cosine);
	const double absSine = std::abs(sine);
	const double halfSourceWidth = (sourceWidth - 1) * 0.5;
	const double halfSourceHeight = (sourceHeight - 1) * 0.5;
	const double boundHalfWidth = absCosine * halfSourceWidth +
		absSine * halfSourceHeight;
	const double boundHalfHeight = absSine * halfSourceWidth +
		absCosine * halfSourceHeight;

	double outputHalfWidth = boundHalfWidth;
	double outputHalfHeight = boundHalfHeight;
	if (autoCrop) {
		if (halfSourceWidth <= kGeometryEpsilon ||
			halfSourceHeight <= kGeometryEpsilon) {
			outputHalfWidth = 0.0;
			outputHalfHeight = 0.0;
		} else {
			const double aspectRatio = preserveAspectRatio && sourceHeight > 1 ?
				static_cast<double>(sourceWidth - 1) / (sourceHeight - 1) : 0.0;
			const HalfExtents contained = ContainedHalfExtents(halfSourceWidth,
				halfSourceHeight, absCosine, absSine, aspectRatio);
			outputHalfWidth = contained.width;
			outputHalfHeight = contained.height;
		}
	}

	const int outputWidth = SafeExtent(outputHalfWidth, !autoCrop);
	const int outputHeight = SafeExtent(outputHalfHeight, !autoCrop);
	if (!ValidDimensions(outputWidth, outputHeight)) return false;

	ImageTransformGeometry built;
	built.sourceWidth = sourceWidth;
	built.sourceHeight = sourceHeight;
	built.outputWidth = outputWidth;
	built.outputHeight = outputHeight;
	const double outputCenterX = (outputWidth - 1) * 0.5;
	const double outputCenterY = (outputHeight - 1) * 0.5;
	built.destinationToSource = {
		cosine, sine, halfSourceWidth - cosine * outputCenterX - sine * outputCenterY,
		-sine, cosine, halfSourceHeight + sine * outputCenterX - cosine * outputCenterY,
		0.0, 0.0, 1.0};

	geometry = built;
	return true;
}

bool BuildPerspectiveGeometry(int sourceWidth, int sourceHeight,
	const PerspectiveCorrectionParameters& parameters,
	ImageTransformGeometry& geometry) {
	if (!ValidDimensions(sourceWidth, sourceHeight) || sourceWidth < 2 ||
		sourceHeight < 2 ||
		!ValidPerspectiveFraction(parameters.leftDeltaFraction) ||
		!ValidPerspectiveFraction(parameters.rightDeltaFraction)) return false;

	const double pixelWidth = sourceWidth - 1.0;
	const double pixelHeight = sourceHeight - 1.0;
	const double left = parameters.leftDeltaFraction * pixelWidth;
	const double right = parameters.rightDeltaFraction * pixelWidth;
	const double topWidth = pixelWidth + right - left;
	const double bottomWidth = pixelWidth - right + left;
	if (!std::isfinite(topWidth) || !std::isfinite(bottomWidth) ||
		topWidth <= kGeometryEpsilon || bottomWidth <= kGeometryEpsilon) return false;

	const double minimumX = -std::abs(left);
	const double maximumX = pixelWidth + std::abs(right);
	const double centeredConvergence = 2.0 * std::max(std::abs(left), std::abs(right));
	double targetLeft = 0.0;
	double targetRight = 0.0;
	double targetTop = 0.0;
	double targetBottom = pixelHeight;
	int outputWidth = 0;
	int outputHeight = 0;
	int outputOriginX = 0;
	if (!parameters.autoCrop) {
		if (!SafeExpandedBounds(minimumX, maximumX, outputOriginX, outputWidth)) return false;
		outputHeight = sourceHeight;
		targetLeft = outputOriginX;
		targetRight = targetLeft + outputWidth - 1.0;
	} else if (parameters.preserveAspectRatio) {
		const double pixelCenterAspect = pixelWidth / pixelHeight;
		const double targetHeightSpan = std::min(pixelHeight,
			pixelWidth / (pixelCenterAspect + centeredConvergence / pixelHeight));
		const double targetWidthSpan = pixelCenterAspect * targetHeightSpan;
		outputWidth = SafeContainedExtent(targetWidthSpan);
		outputHeight = SafeContainedExtent(targetHeightSpan);
		if (outputWidth == 0 || outputHeight == 0) return false;
		targetLeft = (pixelWidth - targetWidthSpan) * 0.5;
		targetRight = targetLeft + targetWidthSpan;
		targetTop = (pixelHeight - targetHeightSpan) * 0.5;
		targetBottom = targetTop + targetHeightSpan;
	} else {
		targetLeft = std::abs(left);
		targetRight = pixelWidth - std::abs(right);
		const double targetWidthSpan = targetRight - targetLeft;
		if (targetWidthSpan < -kGeometryEpsilon) return false;
		outputWidth = SafeContainedExtent(std::max(0.0, targetWidthSpan));
		outputHeight = sourceHeight;
		if (outputWidth == 0) return false;
		targetRight = targetLeft + std::max(0.0, targetWidthSpan);
	}
	if (!ValidDimensions(outputWidth, outputHeight)) return false;

	const double targetWidthSpan = targetRight - targetLeft;
	const double targetHeightSpan = targetBottom - targetTop;
	const double outputToTargetX = outputWidth > 1 ?
		targetWidthSpan / (outputWidth - 1.0) : 0.0;
	const double outputToTargetY = outputHeight > 1 ?
		targetHeightSpan / (outputHeight - 1.0) : 0.0;
	const double outputTargetOriginX = outputWidth > 1 ?
		targetLeft : (targetLeft + targetRight) * 0.5;
	const double outputTargetOriginY = outputHeight > 1 ?
		targetTop : (targetTop + targetBottom) * 0.5;
	const std::array<double, 9> targetToSource = {
		pixelWidth * pixelHeight, 2.0 * pixelWidth * left,
		-pixelWidth * left * pixelHeight,
		0.0, bottomWidth * pixelHeight, 0.0,
		0.0, -(topWidth - bottomWidth), topWidth * pixelHeight};
	const std::array<double, 9> outputToTarget = {
		outputToTargetX, 0.0, outputTargetOriginX,
		0.0, outputToTargetY, outputTargetOriginY,
		0.0, 0.0, 1.0};
	const std::array<double, 9> destinationToSource =
		MultiplyMatrices(targetToSource, outputToTarget);
	for (double value : destinationToSource) {
		if (!std::isfinite(value)) return false;
	}

	ImageTransformGeometry built;
	built.sourceWidth = sourceWidth;
	built.sourceHeight = sourceHeight;
	built.outputWidth = outputWidth;
	built.outputHeight = outputHeight;
	built.destinationToSource = destinationToSource;
	if (parameters.leftDeltaFraction == 0.0 &&
		parameters.rightDeltaFraction == 0.0 &&
		outputWidth == sourceWidth && outputHeight == sourceHeight) {
		built.exactClockwiseQuarterTurns = 0;
	}
	geometry = built;
	return true;
}

bool MapDestinationToSource(const ImageTransformGeometry& geometry,
	double destinationX, double destinationY,
	double& sourceX, double& sourceY) {
	if (geometry.sourceWidth <= 0 || geometry.sourceHeight <= 0 ||
		geometry.outputWidth <= 0 || geometry.outputHeight <= 0 ||
		!std::isfinite(destinationX) || !std::isfinite(destinationY)) return false;
	const auto& matrix = geometry.destinationToSource;
	const double denominator = matrix[6] * destinationX +
		matrix[7] * destinationY + matrix[8];
	if (!std::isfinite(denominator) || std::abs(denominator) <= kGeometryEpsilon) return false;
	const double mappedX = (matrix[0] * destinationX +
		matrix[1] * destinationY + matrix[2]) / denominator;
	const double mappedY = (matrix[3] * destinationX +
		matrix[4] * destinationY + matrix[5]) / denominator;
	if (!std::isfinite(mappedX) || !std::isfinite(mappedY)) return false;
	sourceX = mappedX;
	sourceY = mappedY;
	return true;
}

} // namespace jpegview_linux
