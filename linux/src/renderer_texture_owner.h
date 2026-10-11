#pragma once

#include "sdl_abi.h"

#include <cstddef>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

namespace jpegview_linux {

// Owns all image, preview, and thumbnail SDL textures created by Viewer. Cache
// policies keep their keys and reservations in their existing owners; this
// object centralizes renderer-thread creation, upload, destruction, and final
// teardown before the renderer/window pair is released.
class RendererTextureOwner {
public:
	RendererTextureOwner() noexcept : ownerThread_(std::this_thread::get_id()) {}
	~RendererTextureOwner();
	RendererTextureOwner(const RendererTextureOwner&) = delete;
	RendererTextureOwner& operator=(const RendererTextureOwner&) = delete;

	void SetRenderer(SDL_Renderer* renderer);
	SDL_Texture* CreateEmpty(int width, int height);
	SDL_Texture* CreateAndUpload(const std::vector<std::uint8_t>& bgra,
		int width, int height, bool hasTransparency);
	bool Destroy(SDL_Texture* texture, std::size_t bytes = 0);
	std::size_t DestroyAll();
	std::size_t LiveTextureCount() const;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	void FailNextAdoptionForTesting() noexcept { failNextAdoption_ = true; }
#endif

private:
	void CheckThread() const;
	bool Adopt(SDL_Texture* texture);

	SDL_Renderer* renderer_ = nullptr;
	std::thread::id ownerThread_;
	std::unordered_set<SDL_Texture*> textures_;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	bool failNextAdoption_ = false;
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
