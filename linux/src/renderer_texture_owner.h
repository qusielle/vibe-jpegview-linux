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

} // namespace jpegview_linux
