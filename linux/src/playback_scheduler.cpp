#include "playback_scheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jpegview_linux {
namespace {

constexpr int kMinimumFrameDelayMs = 10;

} // namespace

void PlaybackScheduler::ConfigureImage(std::vector<int> frameDelaysMs,
	int loopCount, bool animated, std::uint32_t now, int maximumFrameDelayMs) {
	frameDelaysMs_ = std::move(frameDelaysMs);
	frameDelayOverrideMs_.reset();
	maximumFrameDelayMs_ = std::clamp(maximumFrameDelayMs,
		kMinimumFrameDelayMs, kMaximumGifFrameDelayMs);
	loopCount_ = std::max(0, loopCount);
	completedLoops_ = 0;
	frameIndex_ = 0;
	manuallyPaused_ = false;
	manualResumeAnimation_ = false;
	sequenceExhausted_ = false;
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

void PlaybackScheduler::ConfigureStillImage(std::uint32_t now) {
	ConfigureImage({}, 0, false, now);
	Stop(now);
	SetImageReady(true, now);
}

void PlaybackScheduler::NotifyInteraction(std::uint32_t now) {
	lastInteractionTick_ = now;
}

void PlaybackScheduler::SetImageReady(bool ready, std::uint32_t now) {
	if (imageReady_ == ready) return;
	imageReady_ = ready;
	if (!ready) return;
	lastInteractionTick_ = now;
	if (manuallyPaused_) {
		nextTick_ = 0;
	} else if (mode_ == PlaybackMode::Movie && !animationPlaying_) {
		nextTick_ = now + MovieFrameInterval();
	} else if (animationPlaying_) {
		ScheduleFrame(now);
	} else {
		nextTick_ = 0;
	}
}

void PlaybackScheduler::SetTemporarilyPaused(bool paused, std::uint32_t now) {
	if (temporarilyPaused_ == paused) return;
	temporarilyPaused_ = paused;
	if (paused || !imageReady_) return;
	lastInteractionTick_ = now;
	if (manuallyPaused_) {
		nextTick_ = 0;
	} else if (mode_ == PlaybackMode::Movie && !animationPlaying_) {
		nextTick_ = now + MovieFrameInterval();
	} else if (animationPlaying_) {
		ScheduleFrame(now);
	} else {
		nextTick_ = 0;
	}
}

void PlaybackScheduler::StartSlideshow(double seconds, std::uint32_t now) {
	animationPlaying_ = false;
	manuallyPaused_ = false;
	manualResumeAnimation_ = false;
	mode_ = PlaybackMode::Slideshow;
	slideshowSeconds_ = std::max(0.1, seconds);
	lastSlideshowSeconds_ = slideshowSeconds_;
	nextTick_ = 0;
	lastInteractionTick_ = now;
}

void PlaybackScheduler::StartMovie(double framesPerSecond, std::uint32_t now) {
	mode_ = PlaybackMode::Movie;
	manuallyPaused_ = false;
	manualResumeAnimation_ = false;
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
	manuallyPaused_ = false;
	manualResumeAnimation_ = false;
	nextTick_ = 0;
	lastInteractionTick_ = now;
}

PlaybackAction PlaybackScheduler::Resume(std::uint32_t now) {
	if (mode_ == PlaybackMode::Slideshow) {
		StartSlideshow(lastSlideshowSeconds_, now);
		return {};
	}
	if (hasAnimation_) {
		return StartAnimation(now);
	}
	StartMovie(movieFramesPerSecond_, now);
	return {};
}

PlaybackAction PlaybackScheduler::StepAnimationFrame(int direction,
	std::uint32_t now) {
	if (!hasAnimation_ || frameDelaysMs_.size() < 2 ||
		(direction != -1 && direction != 1)) return {};
	manualResumeAnimation_ = mode_ != PlaybackMode::Slideshow;
	manuallyPaused_ = true;
	animationPlaying_ = false;
	nextTick_ = 0;
	completedLoops_ = 0;
	sequenceExhausted_ = false;
	if (direction < 0) {
		frameIndex_ = frameIndex_ == 0 ? frameDelaysMs_.size() - 1 : frameIndex_ - 1;
	} else {
		frameIndex_ = (frameIndex_ + 1) % frameDelaysMs_.size();
	}
	lastInteractionTick_ = now;
	return {PlaybackActionType::ShowFrame, frameIndex_};
}

PlaybackAction PlaybackScheduler::ToggleAnimationPlayback(std::uint32_t now) {
	if (!hasAnimation_) return {};
	if (manuallyPaused_) {
		manuallyPaused_ = false;
		if (manualResumeAnimation_) return StartAnimation(now);
		lastInteractionTick_ = now;
		if (mode_ == PlaybackMode::Movie && !animationPlaying_) {
			nextTick_ = now + MovieFrameInterval();
		} else {
			nextTick_ = 0;
		}
		return {};
	}
	if (animationPlaying_) {
		manuallyPaused_ = true;
		manualResumeAnimation_ = true;
		animationPlaying_ = false;
		nextTick_ = 0;
		return {};
	}
	if (mode_ == PlaybackMode::Movie) {
		manuallyPaused_ = true;
		manualResumeAnimation_ = false;
		nextTick_ = 0;
		return {};
	}
	if (mode_ == PlaybackMode::Slideshow) {
		manuallyPaused_ = true;
		manualResumeAnimation_ = false;
		nextTick_ = 0;
		return {};
	}
	return StartAnimation(now);
}

bool PlaybackScheduler::AdjustAnimationDelay(int deltaMs, std::uint32_t now) {
	if (!hasAnimation_ || deltaMs == 0) return false;
	const long long current = frameDelayOverrideMs_.has_value() ?
		*frameDelayOverrideMs_ : OriginalFrameDelayMs();
	const long long adjusted = std::clamp(current + static_cast<long long>(deltaMs),
		static_cast<long long>(kMinimumFrameDelayMs),
		static_cast<long long>(maximumFrameDelayMs_));
	if (adjusted == current) return false;
	frameDelayOverrideMs_ = static_cast<int>(adjusted);
	mode_ = PlaybackMode::None;
	slideshowSeconds_ = 0.0;
	manuallyPaused_ = false;
	manualResumeAnimation_ = false;
	sequenceExhausted_ = false;
	completedLoops_ = 0;
	animationPlaying_ = true;
	lastInteractionTick_ = now;
	if (imageReady_ && !temporarilyPaused_) ScheduleFrame(now);
	else nextTick_ = 0;
	return true;
}

bool PlaybackScheduler::ResetAnimationDelay(std::uint32_t now) {
	if (!hasAnimation_ ||
		(!frameDelayOverrideMs_.has_value() && mode_ != PlaybackMode::Movie)) return false;
	frameDelayOverrideMs_.reset();
	mode_ = PlaybackMode::None;
	slideshowSeconds_ = 0.0;
	manuallyPaused_ = false;
	manualResumeAnimation_ = false;
	sequenceExhausted_ = false;
	completedLoops_ = 0;
	animationPlaying_ = true;
	lastInteractionTick_ = now;
	if (imageReady_ && !temporarilyPaused_) ScheduleFrame(now);
	else nextTick_ = 0;
	return true;
}

bool PlaybackScheduler::LeaveMovieForManualNavigation(std::uint32_t now) {
	if (mode_ != PlaybackMode::Movie) return false;
	if (hasAnimation_) return ResetAnimationDelay(now);
	Stop(now);
	return true;
}

PlaybackAction PlaybackScheduler::Tick(std::uint32_t now) {
	if (!imageReady_ || temporarilyPaused_ || manuallyPaused_) return {};
	if (animationPlaying_ && !frameDelaysMs_.empty() && nextTick_ != 0 && Reached(now, nextTick_)) {
		if (frameIndex_ + 1 < frameDelaysMs_.size()) {
			++frameIndex_;
			ScheduleFrame(now);
			return {PlaybackActionType::ShowFrame, frameIndex_};
		}

		++completedLoops_;
		if (loopCount_ > 0 && completedLoops_ >= loopCount_) {
			animationPlaying_ = false;
			sequenceExhausted_ = true;
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

std::optional<std::uint32_t> PlaybackScheduler::NextDeadline() const {
	if (!imageReady_ || temporarilyPaused_ || manuallyPaused_) return std::nullopt;
	if (nextTick_ != 0 && (animationPlaying_ || mode_ == PlaybackMode::Movie)) {
		return nextTick_;
	}
	if (mode_ == PlaybackMode::Slideshow && slideshowSeconds_ > 0.0) {
		const double delay = std::min(slideshowSeconds_ * 1000.0,
			static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
		return lastInteractionTick_ + static_cast<std::uint32_t>(delay);
	}
	return std::nullopt;
}

void PlaybackScheduler::FrameDisplayFailed(
	std::optional<std::size_t> presentedFrame) {
	animationPlaying_ = false;
	imageReady_ = true;
	nextTick_ = 0;
	if (presentedFrame.has_value() && *presentedFrame < frameDelaysMs_.size()) {
		frameIndex_ = *presentedFrame;
		sequenceExhausted_ = false;
	}
}

std::uint32_t PlaybackScheduler::MovieFrameInterval() const {
	return static_cast<std::uint32_t>(std::clamp(
		static_cast<int>(std::lround(1000.0 / std::max(1.0, movieFramesPerSecond_))), 10, 1000));
}

void PlaybackScheduler::ScheduleFrame(std::uint32_t now) {
	int delay = 100;
	if (mode_ == PlaybackMode::Movie) {
		delay = static_cast<int>(MovieFrameInterval());
	} else {
		delay = FrameDelayMs();
	}
	nextTick_ = now + static_cast<std::uint32_t>(std::clamp(delay,
		kMinimumFrameDelayMs, maximumFrameDelayMs_));
}

PlaybackAction PlaybackScheduler::StartAnimation(std::uint32_t now) {
	PlaybackAction action;
	if (sequenceExhausted_) {
		frameIndex_ = 0;
		completedLoops_ = 0;
		sequenceExhausted_ = false;
		action = {PlaybackActionType::ShowFrame, 0};
	}
	manuallyPaused_ = false;
	manualResumeAnimation_ = true;
	animationPlaying_ = true;
	lastInteractionTick_ = now;
	if (imageReady_ && !temporarilyPaused_) ScheduleFrame(now);
	else nextTick_ = 0;
	return action;
}

int PlaybackScheduler::FrameDelayMs() const {
	return FrameDelayMs(frameIndex_);
}

int PlaybackScheduler::FrameDelayMs(std::size_t frameIndex) const {
	if (frameDelayOverrideMs_.has_value()) return *frameDelayOverrideMs_;
	return OriginalFrameDelayMs(frameIndex);
}

int PlaybackScheduler::OriginalFrameDelayMs() const {
	return OriginalFrameDelayMs(frameIndex_);
}

int PlaybackScheduler::OriginalFrameDelayMs(std::size_t frameIndex) const {
	if (frameIndex >= frameDelaysMs_.size()) return 100;
	return std::clamp(frameDelaysMs_[frameIndex], kMinimumFrameDelayMs,
		maximumFrameDelayMs_);
}

bool PlaybackScheduler::Reached(std::uint32_t now, std::uint32_t deadline) {
	return static_cast<std::int32_t>(now - deadline) >= 0;
}

} // namespace jpegview_linux
