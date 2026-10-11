#pragma once

#include "cache_budget.h"
#include "sdl_abi.h"

#include <cstddef>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <vector>

namespace jpegview_linux {

// Owns all image, preview, and thumbnail SDL textures created by Viewer. Cache
// policies keep their keys and cache reservations in their existing owners;
// standalone presentation textures keep one working reservation in their
// allocation record. This object centralizes renderer-thread creation, upload,
// destruction, and final teardown before the renderer/window pair is released.
class RendererTextureOwner {
public:
	struct StandaloneTextureResidency {
		std::size_t bytes = 0;
		std::size_t count = 0;
		int largestWidth = 0;
		int largestHeight = 0;
	};

	RendererTextureOwner() noexcept : ownerThread_(std::this_thread::get_id()) {}
	~RendererTextureOwner();
	RendererTextureOwner(const RendererTextureOwner&) = delete;
	RendererTextureOwner& operator=(const RendererTextureOwner&) = delete;

	void SetRenderer(SDL_Renderer* renderer);
	SDL_Texture* CreateEmpty(int width, int height);
	SDL_Texture* CreateAndUpload(const std::vector<std::uint8_t>& bgra,
		int width, int height, bool hasTransparency);
	SDL_Texture* CreateStandaloneAndUpload(const std::vector<std::uint8_t>& bgra,
		int width, int height, bool hasTransparency,
		CacheReservation reservation);
	bool Destroy(SDL_Texture* texture, std::size_t bytes = 0);
	std::size_t DestroyAll();
	std::size_t LiveTextureCount() const;
	StandaloneTextureResidency GetStandaloneTextureResidency() const;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	void FailNextAdoptionForTesting() noexcept { failNextAdoption_ = true; }
	void FailNextUploadForTesting() noexcept { failNextUpload_ = true; }
	void FailNextBlendModeForTesting() noexcept { failNextBlendMode_ = true; }
#endif

private:
	struct TextureRecord {
		// Keep source dimensions with the allocation for residency diagnostics.
		int width = 0;
		int height = 0;
		std::size_t bytes = 0;
		CacheReservation standaloneReservation;
	};

	void CheckThread() const;
	bool Adopt(SDL_Texture* texture, int width, int height, std::size_t bytes,
		CacheReservation standaloneReservation = {});
	SDL_Texture* CreateAndUploadInternal(const std::vector<std::uint8_t>& bgra,
		int width, int height, bool hasTransparency,
		CacheReservation standaloneReservation);

	SDL_Renderer* renderer_ = nullptr;
	std::thread::id ownerThread_;
	std::unordered_map<SDL_Texture*, TextureRecord> textures_;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	bool failNextAdoption_ = false;
	bool failNextUpload_ = false;
	bool failNextBlendMode_ = false;
#endif
};

// A presentation role either owns one standalone texture or borrows a texture
// whose cache key is pinned by its Viewer owner. Borrowed references do not pin
// cache entries themselves. Move-only ownership keeps captures, pending
// transitions, active transitions, and retained fallbacks from duplicating the
// right to destroy one SDL texture.
class RendererTextureReference {
public:
	RendererTextureReference() noexcept = default;
	~RendererTextureReference();
	RendererTextureReference(const RendererTextureReference&) = delete;
	RendererTextureReference& operator=(const RendererTextureReference&) = delete;
	RendererTextureReference(RendererTextureReference&& other) noexcept;
	RendererTextureReference& operator=(RendererTextureReference&& other) noexcept;

	static RendererTextureReference Borrow(SDL_Texture* texture) noexcept;
	static RendererTextureReference Own(RendererTextureOwner& owner,
		SDL_Texture* texture) noexcept;

	SDL_Texture* Get() const noexcept { return texture_; }
	bool OwnsTexture() const noexcept { return owner_ != nullptr && texture_ != nullptr; }
	explicit operator bool() const noexcept { return texture_ != nullptr; }
	void Reset() noexcept;

	// A borrower can receive the destruction right only for the exact allocation
	// it already references. This makes cancellation handoffs explicit and keeps
	// the SDL pointer stable for any other live borrower.
	bool TakeOwnershipFrom(RendererTextureReference& owner) noexcept;

private:
	RendererTextureReference(RendererTextureOwner* owner, SDL_Texture* texture) noexcept
		: owner_(owner), texture_(texture) {}

	RendererTextureOwner* owner_ = nullptr;
	SDL_Texture* texture_ = nullptr;
};

} // namespace jpegview_linux
