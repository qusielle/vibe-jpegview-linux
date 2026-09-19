#include "playback_scheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jpegview_linux {

void PlaybackScheduler::ConfigureImage(std::vector<int> frameDelaysMs,
	int loopCount, bool animated, std::uint32_t now) {
	frameDelaysMs_ = std::move(frameDelaysMs);
	loopCount_ = std::max(0, loopCount);
	completedLoops_ = 0;
	frameIndex_ = 0;
	hasAnimation_ = animated && frameDelaysMs_.size() > 1 &&
		std::any_of(frameDelaysMs_.begin(), frameDelaysMs_.end(),
			[](int delay) { return delay > 0; });
	lastInteractionTick_ = now;
	animationPlaying_ = hasAnimation_ && mode_ != PlaybackMode::Slideshow;
	if (animationPlaying_) {
		ScheduleFrame(now);
	} else if (mode_ == PlaybackMode::Movie) {
		nextTick_ = now + MovieFrameInterval();
	} else {
		nextTick_ = 0;
	}
}

void PlaybackScheduler::NotifyInteraction(std::uint32_t now) {
	lastInteractionTick_ = now;
}

void PlaybackScheduler::StartSlideshow(double seconds, std::uint32_t now) {
	animationPlaying_ = false;
	mode_ = PlaybackMode::Slideshow;
	slideshowSeconds_ = std::max(0.1, seconds);
	lastSlideshowSeconds_ = slideshowSeconds_;
	nextTick_ = 0;
	lastInteractionTick_ = now;
}

void PlaybackScheduler::StartMovie(double framesPerSecond, std::uint32_t now) {
	mode_ = PlaybackMode::Movie;
	slideshowSeconds_ = 0.0;
	movieFramesPerSecond_ = std::clamp(framesPerSecond, 1.0, 100.0);
	lastInteractionTick_ = now;
	animationPlaying_ = hasAnimation_;
	if (animationPlaying_) ScheduleFrame(now);
	else nextTick_ = now + MovieFrameInterval();
}

void PlaybackScheduler::Stop(std::uint32_t now) {
	mode_ = PlaybackMode::None;
	slideshowSeconds_ = 0.0;
	animationPlaying_ = false;
	nextTick_ = 0;
	lastInteractionTick_ = now;
}

PlaybackAction PlaybackScheduler::Resume(std::uint32_t now) {
	if (mode_ == PlaybackMode::Slideshow) {
		StartSlideshow(lastSlideshowSeconds_, now);
		return {};
	}
	if (hasAnimation_) {
		PlaybackAction action;
		if (frameIndex_ + 1 >= frameDelaysMs_.size()) {
			frameIndex_ = 0;
			action = {PlaybackActionType::ShowFrame, 0};
		}
		animationPlaying_ = true;
		lastInteractionTick_ = now;
		ScheduleFrame(now);
		return action;
	}
	StartMovie(movieFramesPerSecond_, now);
	return {};
}

PlaybackAction PlaybackScheduler::Tick(std::uint32_t now) {
	if (animationPlaying_ && !frameDelaysMs_.empty() && nextTick_ != 0 && Reached(now, nextTick_)) {
		if (frameIndex_ + 1 < frameDelaysMs_.size()) {
			++frameIndex_;
			ScheduleFrame(now);
			return {PlaybackActionType::ShowFrame, frameIndex_};
		}

		++completedLoops_;
		if (loopCount_ > 0 && completedLoops_ >= loopCount_) {
			animationPlaying_ = false;
			if (mode_ == PlaybackMode::Movie) {
				nextTick_ = now + MovieFrameInterval();
				return {PlaybackActionType::NextImage, 0};
			}
			nextTick_ = 0;
			return {};
		}
		frameIndex_ = 0;
		ScheduleFrame(now);
		return {PlaybackActionType::ShowFrame, 0};
	}

	if (mode_ == PlaybackMode::Movie && !animationPlaying_ &&
		nextTick_ != 0 && Reached(now, nextTick_)) {
		nextTick_ = now + MovieFrameInterval();
		return {PlaybackActionType::NextImage, 0};
	}

	if (mode_ == PlaybackMode::Slideshow && slideshowSeconds_ > 0.0 &&
		static_cast<double>(now - lastInteractionTick_) >= slideshowSeconds_ * 1000.0) {
		return {PlaybackActionType::NextImage, 0};
	}
	return {};
}

void PlaybackScheduler::FrameDisplayFailed() {
	animationPlaying_ = false;
	nextTick_ = 0;
}

std::uint32_t PlaybackScheduler::MovieFrameInterval() const {
	return static_cast<std::uint32_t>(std::clamp(
		static_cast<int>(std::lround(1000.0 / std::max(1.0, movieFramesPerSecond_))), 10, 1000));
}

void PlaybackScheduler::ScheduleFrame(std::uint32_t now) {
	int delay = 100;
	if (mode_ == PlaybackMode::Movie) {
		delay = static_cast<int>(MovieFrameInterval());
	} else if (frameIndex_ < frameDelaysMs_.size()) {
		delay = frameDelaysMs_[frameIndex_];
	}
	nextTick_ = now + static_cast<std::uint32_t>(std::clamp(delay, 10, 60000));
}

bool PlaybackScheduler::Reached(std::uint32_t now, std::uint32_t deadline) {
	return static_cast<std::int32_t>(now - deadline) >= 0;
}

} // namespace jpegview_linux
