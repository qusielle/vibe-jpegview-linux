#include "text_renderer.h"

#include "bitmap_font.h"
#include "perf_diagnostics.h"
#include "system_font.h"

#include <algorithm>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr const char* kRenderScaleQualityHint = "SDL_RENDER_SCALE_QUALITY";
constexpr const char* kImageTextureScaleQuality = "2";
constexpr const char* kBitmapTextScaleQuality = "0";
constexpr std::size_t kMaximumCachedTextTextures = 512;

} // namespace

namespace detail {

void DestroyTextRendererTexture(SDL_Texture* texture) {
	if (texture == nullptr) return;
	PerfScopedTimer destroyTimer(PerfDiagnostics::Instance(), PerfMetric::TextureDestroy);
	SDL_DestroyTexture(texture);
}

} // namespace detail

TextRenderer::~TextRenderer() {
	Clear();
}

void TextRenderer::SetRenderer(SDL_Renderer* renderer) {
	if (renderer_ == renderer) return;
	Clear();
	renderer_ = renderer;
}

void TextRenderer::Clear() {
	cache_.clear();
}

int TextRenderer::TextWidth(const std::string& text, int scale) const {
	return font_ == nullptr ? 0 : font_->TextWidth(text, scale);
}

int TextRenderer::LineHeight(int scale) const {
	return font_ == nullptr ? 0 : font_->LineHeight(scale);
}

TextRenderer::CacheEntry* TextRenderer::FindOrCreate(const std::string& text, int scale) {
	if (renderer_ == nullptr || font_ == nullptr || text.empty() || scale <= 0) return nullptr;
	const std::string key = std::to_string(scale) + '\n' + text;
	auto found = cache_.find(key);
	if (found != cache_.end()) {
		found->second.lastUsed = ++useCounter_;
		return &found->second;
	}

	const RasterizedText raster = font_->Rasterize(text, scale);
	if (raster.width <= 0 || raster.height <= 0 || raster.argb.empty()) return nullptr;
	// Bitmap text uses one-bit ink and transparent white padding. Nearest
	// sampling prevents faint edge pixels from leaking beyond the final glyph.
	const bool bitmapText = Terminus9CanRender(text);
	if (bitmapText) SDL_SetHint(kRenderScaleQualityHint, kBitmapTextScaleQuality);
	SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STATIC, raster.width, raster.height);
	if (bitmapText) SDL_SetHint(kRenderScaleQualityHint, kImageTextureScaleQuality);
	if (texture == nullptr) return nullptr;
	if (SDL_UpdateTexture(texture, nullptr, raster.argb.data(), raster.width * 4) != 0) {
		detail::DestroyTextRendererTexture(texture);
		return nullptr;
	}
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	CacheEntry entry(texture, raster.width, raster.height, raster.offsetX,
		raster.offsetY, ++useCounter_);
	auto inserted = cache_.emplace(key, std::move(entry)).first;
	EvictIfNeeded(key);
	return &inserted->second;
}

void TextRenderer::EvictIfNeeded(const std::string& insertedKey) {
	if (cache_.size() <= kMaximumCachedTextTextures) return;
	auto oldest = cache_.begin();
	for (auto candidate = cache_.begin(); candidate != cache_.end(); ++candidate) {
		if (candidate->second.lastUsed < oldest->second.lastUsed) oldest = candidate;
	}
	if (oldest->first != insertedKey) cache_.erase(oldest);
}

void TextRenderer::Draw(const std::string& text, int x, int y, int scale,
	Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
	CacheEntry* cached = FindOrCreate(text, scale);
	if (cached == nullptr) return;
	SDL_Texture* texture = cached->texture.Get();
	SDL_SetTextureColorMod(texture, red, green, blue);
	SDL_SetTextureAlphaMod(texture, alpha);
	const SDL_Rect destination{x + cached->offsetX, y + cached->offsetY,
		cached->width, cached->height};
	SDL_RenderCopy(renderer_, texture, nullptr, &destination);
	SDL_SetTextureAlphaMod(texture, 255);
}

} // namespace jpegview_linux
