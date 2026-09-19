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

// Identifies one renderer-ready bitmap. The key includes the source file
// identity as well as every setting that changes the resulting pixels.
struct DisplayImageRequest {
	std::filesystem::path filename;
	std::shared_ptr<const DecodedImage> decoded;
	std::size_t frameIndex = 0;
	int targetWidth = 0;
	int targetHeight = 0;
	bool autoContrast = false;
	std::string key;

	bool Valid() const;
};

struct PreparedDisplayImage {
	std::string key;
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> bgra;
};

// Captures the source identity at request time. An invalid request is returned
// for a missing file, invalid frame, or invalid target dimensions.
DisplayImageRequest MakeDisplayImageRequest(const std::filesystem::path& filename,
	const std::shared_ptr<const DecodedImage>& decoded, std::size_t frameIndex,
	int targetWidth, int targetHeight, bool autoContrast);

std::size_t PreparedDisplayImageBytes(const PreparedDisplayImage& image);

// Threaded CPU-side display-frame cache. Workers perform correction, copying,
// and high-quality scaling. SDL texture creation deliberately remains outside
// this class because SDL_Renderer is confined to its owning thread.
class DisplayImageCache {
public:
	using ImagePtr = std::shared_ptr<const PreparedDisplayImage>;
	using Processor = std::function<ImagePtr(const DisplayImageRequest&)>;

	explicit DisplayImageCache(std::size_t byteBudget,
		std::size_t workerCount = 0, Processor processor = {});
	~DisplayImageCache();

	DisplayImageCache(const DisplayImageCache&) = delete;
	DisplayImageCache& operator=(const DisplayImageCache&) = delete;

	ImagePtr Find(const DisplayImageRequest& request);
	void Request(const DisplayImageRequest& request);
	void Prefetch(const std::vector<DisplayImageRequest>& requests);
	std::vector<ImagePtr> TakeCompleted(std::size_t maximumCount);
	void Clear();

	std::size_t CachedBytes() const;
	std::size_t CachedImages() const;
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
