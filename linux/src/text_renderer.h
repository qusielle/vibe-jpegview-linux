#pragma once

#include "renderer_thread_resource.h"
#include "sdl_abi.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace jpegview_linux {

class SystemFont;
namespace detail {
void DestroyTextRendererTexture(SDL_Texture* texture);
}

// Renderer-thread owner for cached text textures. SDL texture creation,
// access, eviction, and destruction stay on the same UI thread as the renderer.
class TextRenderer {
public:
	TextRenderer() = default;
	~TextRenderer();
	TextRenderer(const TextRenderer&) = delete;
	TextRenderer& operator=(const TextRenderer&) = delete;

	void SetRenderer(SDL_Renderer* renderer);
	void SetFont(SystemFont& font) noexcept { font_ = &font; }
	void Clear();
	int TextWidth(const std::string& text, int scale) const;
	int LineHeight(int scale) const;
	void Draw(const std::string& text, int x, int y, int scale,
		Uint8 red = 235, Uint8 green = 235, Uint8 blue = 235, Uint8 alpha = 255);

private:
	struct CacheEntry {
		RendererThreadResource<SDL_Texture, detail::DestroyTextRendererTexture> texture;
		int width = 0;
		int height = 0;
		int offsetX = 0;
		int offsetY = 0;
		std::uint64_t lastUsed = 0;

		CacheEntry(SDL_Texture* handle, int textureWidth, int textureHeight,
			int textureOffsetX, int textureOffsetY, std::uint64_t useOrder)
			: texture(handle), width(textureWidth), height(textureHeight),
			  offsetX(textureOffsetX), offsetY(textureOffsetY), lastUsed(useOrder) {}
		CacheEntry(CacheEntry&&) noexcept = default;
		CacheEntry& operator=(CacheEntry&&) noexcept = default;
		CacheEntry(const CacheEntry&) = delete;
		CacheEntry& operator=(const CacheEntry&) = delete;
	};

	CacheEntry* FindOrCreate(const std::string& text, int scale);
	void EvictIfNeeded(const std::string& insertedKey);

	SDL_Renderer* renderer_ = nullptr;
	SystemFont* font_ = nullptr;
	std::unordered_map<std::string, CacheEntry> cache_;
	std::uint64_t useCounter_ = 0;
};

} // namespace jpegview_linux
