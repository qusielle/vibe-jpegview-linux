#pragma once

#include "archive_source.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>

namespace jpegview_linux {

struct PreparedThumbnailImage;

enum class ThumbnailPixelStoreOutcome {
	Stored,
	AlreadyPresent,
	InvalidImage,
	AllocationFailure,
};

struct ThumbnailRepositoryDiagnostics {
	std::size_t imageCount = 0;
	std::size_t pixelBytes = 0;
};

// Non-blocking access to retained thumbnail pixels. Implementations may keep
// durable storage, but any disk work must run on a worker and never delay the
// event or renderer thread.
class ThumbnailRepository {
public:
	using ImagePtr = std::shared_ptr<const PreparedThumbnailImage>;
	using ReleaseHandler = std::function<void(const ImagePtr&)>;

	virtual ~ThumbnailRepository() = default;
	virtual ThumbnailPixelStoreOutcome Store(const ImagePtr& image) = 0;
	virtual ImagePtr Find(const SourceKey& key) const = 0;
	virtual ImagePtr Erase(const SourceKey& key) = 0;
	virtual void Clear(const ReleaseHandler& beforeRelease = {}) = 0;
	virtual void SetGeometry(int maximumWidth, int maximumHeight,
		const ReleaseHandler& beforeRelease = {}) = 0;
	virtual ThumbnailRepositoryDiagnostics Diagnostics() const = 0;
};

// Retains validated pixels in memory for the active file list, independently
// from renderer-thread SDL textures.
class InMemoryThumbnailRepository final : public ThumbnailRepository {
public:
	ThumbnailPixelStoreOutcome Store(const ImagePtr& image) override;
	ImagePtr Find(const SourceKey& key) const override;
	ImagePtr Erase(const SourceKey& key) override;
	void Clear(const ReleaseHandler& beforeRelease = {}) override;
	void SetGeometry(int maximumWidth, int maximumHeight,
		const ReleaseHandler& beforeRelease = {}) override;
	ThumbnailRepositoryDiagnostics Diagnostics() const override;

private:
	struct Entry {
		ImagePtr image;
		std::size_t bytes = 0;
	};

	std::unordered_map<SourceKey, Entry, SourceKeyHash> entries_;
	std::size_t pixelBytes_ = 0;
	int maximumWidth_ = 0;
	int maximumHeight_ = 0;
};

std::unique_ptr<ThumbnailRepository> MakeInMemoryThumbnailRepository();

} // namespace jpegview_linux
