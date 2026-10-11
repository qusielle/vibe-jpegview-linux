#include "renderer_texture_owner.h"

#include "perf_diagnostics.h"

#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <utility>

namespace jpegview_linux {

namespace {

bool TexturePixelBytes(int width, int height, std::size_t& bytes) {
	if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 4) {
		return false;
	}
	const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
	const std::size_t rows = static_cast<std::size_t>(height);
	if (rows > std::numeric_limits<std::size_t>::max() / rowBytes) return false;
	bytes = rowBytes * rows;
	return true;
}

} // namespace

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

bool RendererTextureOwner::Adopt(SDL_Texture* texture, int width, int height,
	std::size_t bytes, CacheReservation standaloneReservation) {
	if (texture == nullptr) return false;
	try {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
		if (failNextAdoption_) {
			failNextAdoption_ = false;
			throw std::bad_alloc();
		}
#endif
		auto inserted = textures_.try_emplace(texture);
		if (!inserted.second) {
			SDL_DestroyTexture(texture);
			return false;
		}
		inserted.first->second.width = width;
		inserted.first->second.height = height;
		inserted.first->second.bytes = bytes;
		// Keep the working charge in the allocation record so every destruction
		// path, including the final owner drain, releases it after SDL does.
		inserted.first->second.standaloneReservation = std::move(standaloneReservation);
		return true;
	} catch (...) {
		SDL_DestroyTexture(texture);
		return false;
	}
}

SDL_Texture* RendererTextureOwner::CreateEmpty(int width, int height) {
	CheckThread();
	std::size_t bytes = 0;
	if (renderer_ == nullptr || !TexturePixelBytes(width, height, bytes)) return nullptr;
	SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STATIC, width, height);
	if (texture != nullptr && !Adopt(texture, width, height, bytes)) return nullptr;
	return texture;
}

SDL_Texture* RendererTextureOwner::CreateAndUpload(
	const std::vector<std::uint8_t>& bgra, int width, int height,
	bool hasTransparency) {
	return CreateAndUploadInternal(bgra, width, height, hasTransparency, {});
}

SDL_Texture* RendererTextureOwner::CreateStandaloneAndUpload(
	const std::vector<std::uint8_t>& bgra, int width, int height,
	bool hasTransparency, CacheReservation reservation) {
	CheckThread();
	std::size_t bytes = 0;
	if (!reservation || reservation.Category() != CacheMemoryCategory::ActiveWorkingData ||
		!TexturePixelBytes(width, height, bytes) || reservation.Bytes() != bytes) {
		return nullptr;
	}
	return CreateAndUploadInternal(bgra, width, height, hasTransparency,
		std::move(reservation));
}

SDL_Texture* RendererTextureOwner::CreateAndUploadInternal(
	const std::vector<std::uint8_t>& bgra, int width, int height,
	bool hasTransparency, CacheReservation standaloneReservation) {
	CheckThread();
	std::size_t bytes = 0;
	if (renderer_ == nullptr || !TexturePixelBytes(width, height, bytes) ||
		bgra.size() != bytes) {
		return nullptr;
	}
	SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STATIC, width, height);
	if (texture == nullptr) return nullptr;
	if (!Adopt(texture, width, height, bytes, std::move(standaloneReservation))) {
		return nullptr;
	}
	int updateResult = 0;
	{
		PerfScopedTimer uploadTimer(PerfDiagnostics::Instance(), PerfMetric::TextureUpload,
			bgra.size(), static_cast<std::uint64_t>(width),
			static_cast<std::uint64_t>(height));
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
		if (failNextUpload_) {
			failNextUpload_ = false;
			updateResult = -1;
		} else
#endif
		updateResult = SDL_UpdateTexture(texture, nullptr, bgra.data(), width * 4);
	}
	if (updateResult != 0) {
		std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << '\n';
		Destroy(texture, bgra.size());
		return nullptr;
	}
	int blendResult = 0;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	if (failNextBlendMode_) {
		failNextBlendMode_ = false;
		blendResult = -1;
	} else
#endif
	blendResult = SDL_SetTextureBlendMode(texture,
		hasTransparency ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
	if (blendResult != 0) {
		std::cerr << "SDL_SetTextureBlendMode failed: " << SDL_GetError() << '\n';
		Destroy(texture, bgra.size());
		return nullptr;
	}
	return texture;
}

bool RendererTextureOwner::Destroy(SDL_Texture* texture, std::size_t bytes) {
	CheckThread();
	if (texture == nullptr) return false;
	auto found = textures_.find(texture);
	if (found == textures_.end()) return false;
	TextureRecord retired = std::move(found->second);
	textures_.erase(found);
	PerfScopedTimer destroyTimer(PerfDiagnostics::Instance(), PerfMetric::TextureDestroy,
		bytes == 0 ? retired.bytes : bytes);
	SDL_DestroyTexture(texture);
	// Renderer allocations remain charged through the actual SDL destruction.
	retired.standaloneReservation.Reset();
	return true;
}

std::size_t RendererTextureOwner::DestroyAll() {
	CheckThread();
	const std::size_t count = textures_.size();
	while (!textures_.empty()) Destroy(textures_.begin()->first);
	return count;
}

std::size_t RendererTextureOwner::LiveTextureCount() const {
	CheckThread();
	return textures_.size();
}

RendererTextureOwner::StandaloneTextureResidency
RendererTextureOwner::GetStandaloneTextureResidency() const {
	CheckThread();
	StandaloneTextureResidency result;
	std::size_t largestBytes = 0;
	for (const auto& texture : textures_) {
		if (!texture.second.standaloneReservation) continue;
		result.bytes += texture.second.bytes;
		++result.count;
		if (texture.second.bytes > largestBytes) {
			largestBytes = texture.second.bytes;
			result.largestWidth = texture.second.width;
			result.largestHeight = texture.second.height;
		}
	}
	return result;
}

RendererTextureReference::~RendererTextureReference() {
	Reset();
}

RendererTextureReference::RendererTextureReference(
	RendererTextureReference&& other) noexcept
	: owner_(other.owner_), texture_(other.texture_) {
	other.owner_ = nullptr;
	other.texture_ = nullptr;
}

RendererTextureReference& RendererTextureReference::operator=(
	RendererTextureReference&& other) noexcept {
	if (this == &other) return *this;
	Reset();
	owner_ = other.owner_;
	texture_ = other.texture_;
	other.owner_ = nullptr;
	other.texture_ = nullptr;
	return *this;
}

RendererTextureReference RendererTextureReference::Borrow(SDL_Texture* texture) noexcept {
	return RendererTextureReference(nullptr, texture);
}

RendererTextureReference RendererTextureReference::Own(
	RendererTextureOwner& owner, SDL_Texture* texture) noexcept {
	return texture == nullptr ? RendererTextureReference{} :
		RendererTextureReference(&owner, texture);
}

void RendererTextureReference::Reset() noexcept {
	if (owner_ != nullptr && texture_ != nullptr && !owner_->Destroy(texture_)) {
		std::cerr << "RendererTextureReference could not release its owned texture\n";
	}
	owner_ = nullptr;
	texture_ = nullptr;
}

bool RendererTextureReference::TakeOwnershipFrom(
	RendererTextureReference& owner) noexcept {
	if (owner_ != nullptr || texture_ == nullptr || texture_ != owner.texture_ ||
		owner.owner_ == nullptr) return false;
	owner_ = owner.owner_;
	owner.owner_ = nullptr;
	owner.texture_ = nullptr;
	return true;
}

} // namespace jpegview_linux
