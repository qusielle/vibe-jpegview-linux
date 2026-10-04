#include "renderer_texture_owner.h"

#include "perf_diagnostics.h"

#include <exception>
#include <iostream>

namespace jpegview_linux {

RendererTextureOwner::~RendererTextureOwner() {
	if (renderer_ != nullptr && !textures_.empty()) DestroyAll();
}

void RendererTextureOwner::CheckThread() const {
	if (ownerThread_ != std::this_thread::get_id()) std::terminate();
}

void RendererTextureOwner::SetRenderer(SDL_Renderer* renderer) {
	CheckThread();
	if (renderer == renderer_) return;
	if (!textures_.empty()) {
		std::cerr << "RendererTextureOwner released " << DestroyAll()
			<< " live image textures while changing renderers\n";
	}
	renderer_ = renderer;
}

bool RendererTextureOwner::Adopt(SDL_Texture* texture) {
	if (texture == nullptr) return false;
	try {
		return textures_.insert(texture).second;
	} catch (...) {
		SDL_DestroyTexture(texture);
		throw;
	}
}

SDL_Texture* RendererTextureOwner::CreateEmpty(int width, int height) {
	CheckThread();
	if (renderer_ == nullptr || width <= 0 || height <= 0) return nullptr;
	SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STATIC, width, height);
	if (texture != nullptr && !Adopt(texture)) return nullptr;
	return texture;
}

SDL_Texture* RendererTextureOwner::CreateAndUpload(
	const std::vector<std::uint8_t>& bgra, int width, int height,
	bool hasTransparency) {
	CheckThread();
	if (renderer_ == nullptr || width <= 0 || height <= 0 || bgra.size() !=
		static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
		return nullptr;
	}
	SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STATIC, width, height);
	if (texture == nullptr) return nullptr;
	if (!Adopt(texture)) return nullptr;
	int updateResult = 0;
	{
		PerfScopedTimer uploadTimer(PerfDiagnostics::Instance(), PerfMetric::TextureUpload,
			bgra.size(), static_cast<std::uint64_t>(width),
			static_cast<std::uint64_t>(height));
		updateResult = SDL_UpdateTexture(texture, nullptr, bgra.data(), width * 4);
	}
	if (updateResult != 0) {
		std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << '\n';
		Destroy(texture, bgra.size());
		return nullptr;
	}
	if (SDL_SetTextureBlendMode(texture,
		hasTransparency ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE) != 0) {
		std::cerr << "SDL_SetTextureBlendMode failed: " << SDL_GetError() << '\n';
		Destroy(texture, bgra.size());
		return nullptr;
	}
	return texture;
}

bool RendererTextureOwner::Destroy(SDL_Texture* texture, std::size_t bytes) {
	CheckThread();
	if (texture == nullptr || textures_.erase(texture) == 0) return false;
	PerfScopedTimer destroyTimer(PerfDiagnostics::Instance(), PerfMetric::TextureDestroy,
		bytes);
	SDL_DestroyTexture(texture);
	return true;
}

std::size_t RendererTextureOwner::DestroyAll() {
	CheckThread();
	const std::size_t count = textures_.size();
	while (!textures_.empty()) Destroy(*textures_.begin());
	return count;
}

std::size_t RendererTextureOwner::LiveTextureCount() const {
	CheckThread();
	return textures_.size();
}

} // namespace jpegview_linux
