#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace jpegview_linux {

enum class PlaybackMode {
	None,
	Slideshow,
	Movie,
};

enum class PlaybackActionType {
	None,
	ShowFrame,
	NextImage,
};

constexpr int kMaximumAdjustableFrameDelayMs = 60000;
constexpr int kMaximumGifFrameDelayMs = 655350;

struct PlaybackAction {
	PlaybackActionType type = PlaybackActionType::None;
	std::size_t frameIndex = 0;
	bool overlayChanged = false;
};

class PlaybackScheduler {
public:
	void ConfigureImage(std::vector<int> frameDelaysMs, int loopCount,
		bool animated, std::uint32_t now,
		int maximumFrameDelayMs = kMaximumAdjustableFrameDelayMs);
	void ConfigureStillImage(std::uint32_t now);
	void NotifyInteraction(std::uint32_t now);
	void SetImageReady(bool ready, std::uint32_t now);
	// Suppress scheduled advancement without changing whether a frame is ready.
	// Resuming starts a fresh interval from the supplied tick.
	void SetTemporarilyPaused(bool paused, std::uint32_t now);

	void StartSlideshow(double seconds, std::uint32_t now);
	void StartMovie(double framesPerSecond, std::uint32_t now);
	void Stop(std::uint32_t now);
	PlaybackAction Resume(std::uint32_t now);
	PlaybackAction StepAnimationFrame(int direction, std::uint32_t now);
	PlaybackAction ToggleAnimationPlayback(std::uint32_t now);
	bool AdjustAnimationDelay(int deltaMs, std::uint32_t now);
	bool ResetAnimationDelay(std::uint32_t now);
	// Manual file selection stops Movie's automatic folder advancement.
	bool LeaveMovieForManualNavigation(std::uint32_t now);
	PlaybackAction Tick(std::uint32_t now);
	std::optional<std::uint32_t> NextDeadline() const;
	void FrameDisplayFailed(std::optional<std::size_t> presentedFrame = std::nullopt);

	PlaybackMode Mode() const { return mode_; }
	bool AnimationPlaying() const { return animationPlaying_; }
	bool AnimationManuallyPaused() const { return manuallyPaused_; }
	bool HasAnimation() const { return hasAnimation_; }
	bool ImageReady() const { return imageReady_; }
	std::size_t FrameIndex() const { return frameIndex_; }
	int MaximumFrameDelayMs() const { return maximumFrameDelayMs_; }
	int FrameDelayMs() const;
	int FrameDelayMs(std::size_t frameIndex) const;
	int OriginalFrameDelayMs() const;
	std::optional<int> AnimationDelayOverrideMs() const { return frameDelayOverrideMs_; }
	double SlideshowSeconds() const { return slideshowSeconds_; }
	double LastSlideshowSeconds() const { return lastSlideshowSeconds_; }
	double MovieFramesPerSecond() const { return movieFramesPerSecond_; }
	std::uint32_t NextTick() const { return nextTick_.value_or(0); }
	int CompletedLoops() const { return completedLoops_; }

private:
	int OriginalFrameDelayMs(std::size_t frameIndex) const;
	std::uint32_t MovieFrameInterval() const;
	void ScheduleFrame(std::uint32_t now);
	PlaybackAction StartAnimation(std::uint32_t now);
	static bool Reached(std::uint32_t now, std::uint32_t deadline);

	PlaybackMode mode_ = PlaybackMode::None;
	double slideshowSeconds_ = 0.0;
	double lastSlideshowSeconds_ = 3.0;
	double movieFramesPerSecond_ = 25.0;
	std::optional<std::uint32_t> nextTick_;
	std::uint32_t lastInteractionTick_ = 0;
	bool hasAnimation_ = false;
	bool animationPlaying_ = false;
	bool manuallyPaused_ = false;
	bool manualResumeAnimation_ = false;
	bool sequenceExhausted_ = false;
	bool imageReady_ = true;
	bool temporarilyPaused_ = false;
	std::vector<int> frameDelaysMs_;
	std::optional<int> frameDelayOverrideMs_;
	int maximumFrameDelayMs_ = kMaximumAdjustableFrameDelayMs;
	std::size_t frameIndex_ = 0;
	int loopCount_ = 0;
	int completedLoops_ = 0;
};

} // namespace jpegview_linux
