#pragma once

#include <cstddef>
#include <cstdint>
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

struct PlaybackAction {
	PlaybackActionType type = PlaybackActionType::None;
	std::size_t frameIndex = 0;
};

class PlaybackScheduler {
public:
	void ConfigureImage(std::vector<int> frameDelaysMs, int loopCount,
		bool animated, std::uint32_t now);
	void NotifyInteraction(std::uint32_t now);

	void StartSlideshow(double seconds, std::uint32_t now);
	void StartMovie(double framesPerSecond, std::uint32_t now);
	void Stop(std::uint32_t now);
	PlaybackAction Resume(std::uint32_t now);
	PlaybackAction Tick(std::uint32_t now);
	void FrameDisplayFailed();

	PlaybackMode Mode() const { return mode_; }
	bool AnimationPlaying() const { return animationPlaying_; }
	bool HasAnimation() const { return hasAnimation_; }
	std::size_t FrameIndex() const { return frameIndex_; }
	double SlideshowSeconds() const { return slideshowSeconds_; }
	double LastSlideshowSeconds() const { return lastSlideshowSeconds_; }
	double MovieFramesPerSecond() const { return movieFramesPerSecond_; }
	std::uint32_t NextTick() const { return nextTick_; }
	int CompletedLoops() const { return completedLoops_; }

private:
	std::uint32_t MovieFrameInterval() const;
	void ScheduleFrame(std::uint32_t now);
	static bool Reached(std::uint32_t now, std::uint32_t deadline);

	PlaybackMode mode_ = PlaybackMode::None;
	double slideshowSeconds_ = 0.0;
	double lastSlideshowSeconds_ = 3.0;
	double movieFramesPerSecond_ = 25.0;
	std::uint32_t nextTick_ = 0;
	std::uint32_t lastInteractionTick_ = 0;
	bool hasAnimation_ = false;
	bool animationPlaying_ = false;
	std::vector<int> frameDelaysMs_;
	std::size_t frameIndex_ = 0;
	int loopCount_ = 0;
	int completedLoops_ = 0;
};

} // namespace jpegview_linux
