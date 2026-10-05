#include "thumbnail_repository.h"

#include "thumbnail_resampler.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace jpegview_linux {

ThumbnailPixelStoreOutcome InMemoryThumbnailRepository::Store(const ImagePtr& image) {
	if (!image || image->key.Empty() || maximumWidth_ <= 0 || maximumHeight_ <= 0) {
		return ThumbnailPixelStoreOutcome::InvalidImage;
	}
	if (entries_.find(image->key) != entries_.end()) {
		return ThumbnailPixelStoreOutcome::AlreadyPresent;
	}
	if (image->width <= 0 || image->height <= 0 ||
		image->width > maximumWidth_ || image->height > maximumHeight_) {
		return ThumbnailPixelStoreOutcome::InvalidImage;
	}
	const std::size_t width = static_cast<std::size_t>(image->width);
	const std::size_t height = static_cast<std::size_t>(image->height);
	const std::size_t maximum = std::numeric_limits<std::size_t>::max();
	if (width > maximum / height || width * height > maximum / 4) {
		return ThumbnailPixelStoreOutcome::InvalidImage;
	}
	const std::size_t bytes = width * height * 4;
	if (image->bgra.size() != bytes) return ThumbnailPixelStoreOutcome::InvalidImage;
	if (bytes > maximum - pixelBytes_) return ThumbnailPixelStoreOutcome::AllocationFailure;
	try {
		const auto inserted = entries_.emplace(image->key, Entry{image, bytes});
		if (!inserted.second) return ThumbnailPixelStoreOutcome::AlreadyPresent;
	} catch (const std::bad_alloc&) {
		return ThumbnailPixelStoreOutcome::AllocationFailure;
	} catch (const std::length_error&) {
		return ThumbnailPixelStoreOutcome::AllocationFailure;
	}
	pixelBytes_ += bytes;
	return ThumbnailPixelStoreOutcome::Stored;
}

ThumbnailRepository::ImagePtr InMemoryThumbnailRepository::Find(
	const SourceKey& key) const {
	const auto found = entries_.find(key);
	return found == entries_.end() ? ImagePtr{} : found->second.image;
}

ThumbnailRepository::ImagePtr InMemoryThumbnailRepository::Erase(const SourceKey& key) {
	const auto found = entries_.find(key);
	if (found == entries_.end()) return {};
	ImagePtr image = std::move(found->second.image);
	pixelBytes_ -= found->second.bytes;
	entries_.erase(found);
	return image;
}

void InMemoryThumbnailRepository::Clear(const ReleaseHandler& beforeRelease) {
	if (beforeRelease) {
		for (const auto& entry : entries_) {
			try {
				beforeRelease(entry.second.image);
			} catch (...) {
				// A retirement callback must not prevent clearing the remaining entries.
			}
		}
	}
	entries_.clear();
	pixelBytes_ = 0;
}

void InMemoryThumbnailRepository::SetGeometry(int maximumWidth, int maximumHeight,
	const ReleaseHandler& beforeRelease) {
	maximumWidth = std::max(0, maximumWidth);
	maximumHeight = std::max(0, maximumHeight);
	if (maximumWidth_ == maximumWidth && maximumHeight_ == maximumHeight) return;
	Clear(beforeRelease);
	maximumWidth_ = maximumWidth;
	maximumHeight_ = maximumHeight;
}

ThumbnailRepositoryDiagnostics InMemoryThumbnailRepository::Diagnostics() const {
	return {entries_.size(), pixelBytes_};
}

std::unique_ptr<ThumbnailRepository> MakeInMemoryThumbnailRepository() {
	return std::make_unique<InMemoryThumbnailRepository>();
}

} // namespace jpegview_linux
