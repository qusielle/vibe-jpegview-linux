#pragma once

#include "image_decoder.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jpegview_linux {

// Pixel memory retained by a decoded image. Container bookkeeping is small
// compared with the BGRA buffers and is intentionally excluded from the
// configured cache budget.
std::size_t DecodedImageBytes(const DecodedImage& image);

// Returns neighboring indices in likely-use order. The preferred direction
// is visited first at each distance, and folder-loop wraparound is included.
std::vector<std::size_t> ImagePrefetchOrder(std::size_t fileCount,
	std::size_t currentIndex, int preferredDirection, std::size_t maximumCount);

// Thread-safe decoded-pixel LRU with a single background decoder. Cache hits
// avoid file decoding while texture creation remains on SDL's main thread.
class DecodedImageCache {
public:
	using ImagePtr = std::shared_ptr<const DecodedImage>;
	using Decoder = std::function<bool(const std::filesystem::path&, DecodedImage&, std::string&)>;

	explicit DecodedImageCache(std::size_t byteBudget, Decoder decoder = {});
	~DecodedImageCache();

	DecodedImageCache(const DecodedImageCache&) = delete;
	DecodedImageCache& operator=(const DecodedImageCache&) = delete;

	ImagePtr Find(const std::filesystem::path& filename);
	void Store(const std::filesystem::path& filename,
		const std::shared_ptr<DecodedImage>& image);
	void Prefetch(const std::vector<std::filesystem::path>& files,
		std::size_t currentIndex, int preferredDirection,
		std::size_t maximumCount);
	void Clear();

	std::size_t CachedBytes() const;
	std::size_t CachedImages() const;
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
