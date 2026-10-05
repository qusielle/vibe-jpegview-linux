#include "image.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kSourceWidth = 4000;
constexpr int kSourceHeight = 2500;
constexpr int kResizedWidth = 1200;
constexpr int kResizedHeight = 750;

std::vector<std::uint8_t> MakeSyntheticImage() {
	std::vector<std::uint8_t> pixels(
		static_cast<std::size_t>(kSourceWidth) * kSourceHeight * 4);
	for (int y = 0; y < kSourceHeight; ++y) {
		for (int x = 0; x < kSourceWidth; ++x) {
			const std::size_t offset =
				(static_cast<std::size_t>(y) * kSourceWidth + x) * 4;
			pixels[offset] = static_cast<std::uint8_t>(x * 17 + y * 13);
			pixels[offset + 1] = static_cast<std::uint8_t>(x * 7 + y * 29);
			pixels[offset + 2] = static_cast<std::uint8_t>(x * 31 + y * 3);
			pixels[offset + 3] = 255;
		}
	}
	return pixels;
}

std::uint64_t Checksum(const std::vector<std::uint8_t>& bytes) {
	std::uint64_t hash = 14695981039346656037ull;
	for (std::uint8_t value : bytes) {
		hash ^= value;
		hash *= 1099511628211ull;
	}
	return hash;
}

double MedianMilliseconds(std::vector<double> samples) {
	std::sort(samples.begin(), samples.end());
	const std::size_t middle = samples.size() / 2;
	if ((samples.size() & 1u) != 0) return samples[middle];
	return (samples[middle - 1] + samples[middle]) / 2.0;
}

bool RunImageProcessing(const std::vector<std::uint8_t>& source,
	std::uint64_t& checksum, double& milliseconds) {
	jpegview_linux::Image image;
	if (!image.StoreBGRA(source.data(), kSourceWidth, kSourceHeight)) return false;
	jpegview_linux::ImageProcessingParams params;
	params.gamma = 1.04;
	params.saturation = 1.12;
	params.cyanRed = 0.035;
	params.magentaGreen = -0.02;
	const auto begin = std::chrono::steady_clock::now();
	const bool completed = image.ApplyProcessing(params, false);
	const auto end = std::chrono::steady_clock::now();
	if (!completed) return false;
	milliseconds = std::chrono::duration<double, std::milli>(end - begin).count();
	checksum = Checksum(image.bgra);
	return true;
}

bool RunResize(const std::vector<std::uint8_t>& source,
	std::uint64_t& checksum, double& milliseconds) {
	jpegview_linux::Image image;
	if (!image.StoreBGRA(source.data(), kSourceWidth, kSourceHeight)) return false;
	const auto begin = std::chrono::steady_clock::now();
	const bool completed = image.Resize(kResizedWidth, kResizedHeight, 3);
	const auto end = std::chrono::steady_clock::now();
	if (!completed) return false;
	milliseconds = std::chrono::duration<double, std::milli>(end - begin).count();
	checksum = Checksum(image.bgra);
	return true;
}

bool ParseIterations(int argc, char** argv, int& iterations) {
	iterations = 3;
	if (argc == 1) return true;
	if (argc != 3 || std::string(argv[1]) != "--iterations") return false;
	char* end = nullptr;
	const long value = std::strtol(argv[2], &end, 10);
	if (end == argv[2] || *end != '\0' || value < 1 || value > 25) return false;
	iterations = static_cast<int>(value);
	return true;
}

} // namespace

int main(int argc, char** argv) {
	int iterations = 0;
	if (!ParseIterations(argc, argv, iterations)) {
		std::cerr << "Usage: " << argv[0] << " [--iterations 1..25]\n";
		return 2;
	}
	const std::vector<std::uint8_t> source = MakeSyntheticImage();
	std::vector<double> processingSamples;
	std::vector<double> resizeSamples;
	std::uint64_t processingChecksum = 0;
	std::uint64_t resizedChecksum = 0;
	for (int iteration = 0; iteration < iterations; ++iteration) {
		std::uint64_t nextProcessingChecksum = 0;
		std::uint64_t nextResizedChecksum = 0;
		double processingMilliseconds = 0.0;
		double resizeMilliseconds = 0.0;
		if (!RunImageProcessing(source, nextProcessingChecksum, processingMilliseconds) ||
			!RunResize(source, nextResizedChecksum, resizeMilliseconds)) {
			std::cerr << "Synthetic image workload failed at iteration " << iteration + 1 << '\n';
			return 1;
		}
		if (iteration != 0 && (nextProcessingChecksum != processingChecksum ||
			nextResizedChecksum != resizedChecksum)) {
			std::cerr << "Synthetic image workload produced inconsistent pixels\n";
			return 1;
		}
		processingChecksum = nextProcessingChecksum;
		resizedChecksum = nextResizedChecksum;
		processingSamples.push_back(processingMilliseconds);
		resizeSamples.push_back(resizeMilliseconds);
	}
	const std::size_t sourceBytes = source.size();
	std::cout << "Synthetic image workload: " << kSourceWidth << 'x' << kSourceHeight
		<< " BGRA8 (" << sourceBytes << " bytes), " << iterations << " iteration(s)\n"
		<< "Compiler: " << __VERSION__ << "; logical CPUs: "
		<< std::thread::hardware_concurrency() << '\n'
		<< "ApplyProcessing median: " << MedianMilliseconds(processingSamples)
		<< " ms; checksum: " << processingChecksum << '\n'
		<< "Resize filter 3 to " << kResizedWidth << 'x' << kResizedHeight
		<< " median: " << MedianMilliseconds(resizeSamples)
		<< " ms; checksum: " << resizedChecksum << '\n'
		<< "Measurements are informational; no timing threshold is applied.\n";
	return 0;
}
