#pragma once

#include <exception>
#include <thread>
#include <utility>

namespace jpegview_linux {

// A move-only SDL handle whose destruction stays on the thread that adopted
// it. Explicitly reset renderer resources before SDL_Quit; destruction is a
// final safety net for early exits.
template <typename Handle, void (*Destroy)(Handle*)>
class RendererThreadResource {
public:
	RendererThreadResource() noexcept : ownerThread_(std::this_thread::get_id()) {}
	explicit RendererThreadResource(Handle* handle) noexcept
		: handle_(handle), ownerThread_(std::this_thread::get_id()) {}
	~RendererThreadResource() { Reset(); }

	RendererThreadResource(const RendererThreadResource&) = delete;
	RendererThreadResource& operator=(const RendererThreadResource&) = delete;

	RendererThreadResource(RendererThreadResource&& other) noexcept
		: handle_(other.Release()), ownerThread_(other.ownerThread_) {}
	RendererThreadResource& operator=(RendererThreadResource&& other) noexcept {
		if (this == &other) return *this;
		Reset();
		handle_ = other.Release();
		ownerThread_ = other.ownerThread_;
		return *this;
	}

	RendererThreadResource& operator=(Handle* handle) noexcept {
		Reset(handle);
		return *this;
	}

	Handle* Get() const noexcept { return handle_; }
	operator Handle*() const noexcept { return handle_; }
	explicit operator bool() const noexcept { return handle_ != nullptr; }

	Handle* Release() noexcept { return std::exchange(handle_, nullptr); }

	void Reset(Handle* replacement = nullptr) noexcept {
		if (replacement == handle_) return;
		if (handle_ != nullptr) {
			if (ownerThread_ != std::this_thread::get_id()) std::terminate();
			Destroy(handle_);
		}
		handle_ = replacement;
		if (handle_ != nullptr) ownerThread_ = std::this_thread::get_id();
	}

private:
	Handle* handle_ = nullptr;
	std::thread::id ownerThread_;
};

// Keep the SDL window and renderer lifecycle in one owner with an explicit
// renderer-before-window destruction order.
template <typename Window, typename Renderer,
	void (*DestroyWindow)(Window*), void (*DestroyRenderer)(Renderer*)>
class RendererWindowResources {
public:
	using WindowHandle = RendererThreadResource<Window, DestroyWindow>;
	using RendererHandle = RendererThreadResource<Renderer, DestroyRenderer>;
	RendererWindowResources() = default;
	RendererWindowResources(const RendererWindowResources&) = delete;
	RendererWindowResources& operator=(const RendererWindowResources&) = delete;
	RendererWindowResources(RendererWindowResources&& other) noexcept
		: window_(std::move(other.window_)), renderer_(std::move(other.renderer_)) {}
	RendererWindowResources& operator=(RendererWindowResources&& other) noexcept {
		if (this == &other) return *this;
		Reset();
		window_ = std::move(other.window_);
		renderer_ = std::move(other.renderer_);
		return *this;
	}

	~RendererWindowResources() { Reset(); }

	WindowHandle& WindowResource() noexcept { return window_; }
	RendererHandle& RendererResource() noexcept { return renderer_; }
	const WindowHandle& WindowResource() const noexcept { return window_; }
	const RendererHandle& RendererResource() const noexcept { return renderer_; }

	void Reset() noexcept {
		renderer_.Reset();
		window_.Reset();
	}

	// Derived renderer adapters use the handles as borrowed SDL pointers. The
	// base remains the sole lifetime owner and controls pair teardown order.
protected:
	WindowHandle window_;
	RendererHandle renderer_;
};

} // namespace jpegview_linux
