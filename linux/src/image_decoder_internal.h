#pragma once

#include "image_decoder.h"
#include "perf_diagnostics.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace jpegview_linux::decoder_detail {

inline constexpr std::uint64_t kMaxAnimationBytes = 512ull * 1024ull * 1024ull;
inline constexpr std::size_t kMaxAnimationFrames = 16384;
inline constexpr std::uint32_t kMaxImageDimension = 65535;
inline constexpr std::uint32_t kMaxImagePixels = 100u * 1024u * 1024u;

// A decoder owns this budget while appending frames; published images remain freely mutable.
class AnimationFrameBudget {
public:
    explicit AnimationFrameBudget(const DecodedImage& image) : frames_(image.frames.size()) {
        if (frames_ >= kMaxAnimationFrames) return;
        for (const DecodedFrame& frame : image.frames) {
            if (frame.bgra.size() > kMaxAnimationBytes - bytes_) {
                bytes_ = kMaxAnimationBytes + 1;
                break;
            }
            bytes_ += frame.bgra.size();
        }
    }

    bool CanAppend(std::size_t bytes, std::string& errorMessage) const {
        if (frames_ >= kMaxAnimationFrames) {
            errorMessage = "animation has too many frames";
            return false;
        }
        if (bytes > kMaxAnimationBytes || bytes_ > kMaxAnimationBytes - bytes) {
            errorMessage = "animation is too large";
            return false;
        }
        return true;
    }

    // Charge only after the frame was successfully appended, never on cancellation or failure.
    void RecordAppend(std::size_t bytes) {
        bytes_ += bytes;
        ++frames_;
    }

private:
    std::uint64_t bytes_ = 0;
    std::size_t frames_ = 0;
};

inline bool CheckDecoderCancellation(const WorkContext& context, std::string& errorMessage) {
    if (context.Continue()) return true;
    errorMessage = "source work was cancelled";
    return false;
}

std::string Lower(std::string value);
bool ValidDimensions(int width, int height, std::string& errorMessage);
bool AppendBGRA(DecodedImage& image, int width, int height, const std::uint8_t* pixels,
    int delayMs, std::string& errorMessage, bool hasAlphaChannel,
    const WorkContext& context, AnimationFrameBudget& budget);
bool AppendBGRA(DecodedImage& image, int width, int height,
    std::vector<std::uint8_t>&& pixels, int delayMs, std::string& errorMessage,
    bool hasAlphaChannel, const WorkContext& context, AnimationFrameBudget& budget);
bool AppendRGBA(DecodedImage& image, int width, int height, const std::uint8_t* pixels,
    int delayMs, std::string& errorMessage, bool hasAlphaChannel,
    const WorkContext& context, AnimationFrameBudget& budget);
bool AppendRGBA(DecodedImage& image, int width, int height,
    std::vector<std::uint8_t>&& pixels, int delayMs, std::string& errorMessage,
    bool hasAlphaChannel, const WorkContext& context, AnimationFrameBudget& budget);
bool ApplyIccProfile(std::vector<std::uint8_t>& bgra,
    const std::vector<std::uint8_t>& profile);
bool ReadFile(const std::filesystem::path& filename,
    std::vector<std::uint8_t>& data, std::string& errorMessage,
    const WorkContext& context = {});
std::uint32_t ReadBE32(const std::uint8_t* data);
std::uint16_t ReadBE16(const std::uint8_t* data);
std::unique_ptr<std::FILE, int (*)(std::FILE*)> OpenSourceFile(
	const std::filesystem::path& filename);

class CodecSourceReadTracker {
public:
    explicit CodecSourceReadTracker(const char* codec)
        : diagnostics_(PerfDiagnostics::Instance()), enabled_(diagnostics_.Enabled()), codec_(codec) {}

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    bool Enabled() const { return enabled_; }
    TimePoint BeginRead() const { return enabled_ ? Clock::now() : TimePoint{}; }
    void CompleteRead(TimePoint start, std::uint64_t bytes) {
        CompleteRead(start, bytes, Clock::now());
    }
    void CompleteRead(TimePoint start, std::uint64_t bytes, TimePoint finish) {
        if (!enabled_) return;
        if (start != TimePoint{} && finish >= start) {
            readDurationNs_ += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
        }
        bytesRead_ += bytes;
        ++readCalls_;
    }
    std::size_t Read(std::FILE* file, void* destination, std::size_t requested) {
        if (!enabled_) return std::fread(destination, 1, requested, file);
        const TimePoint start = BeginRead();
        const std::size_t bytes = std::fread(destination, 1, requested, file);
        CompleteRead(start, bytes);
        return bytes;
    }
    ~CodecSourceReadTracker() {
        if (!enabled_ || bytesRead_ == 0 || readCalls_ == 0) return;
        const std::uint64_t durationUs = readDurationNs_ == 0 ? 0 :
            (readDurationNs_ + 999) / 1000;
        diagnostics_.RecordText(PerfMetric::SourceRead, durationUs,
            bytesRead_, readCalls_, 0, 0, 0, 0, codec_);
    }
    CodecSourceReadTracker(const CodecSourceReadTracker&) = delete;
    CodecSourceReadTracker& operator=(const CodecSourceReadTracker&) = delete;

private:
    PerfDiagnostics& diagnostics_;
    bool enabled_;
    const char* codec_;
    std::uint64_t readDurationNs_ = 0;
    std::uint64_t bytesRead_ = 0;
    std::uint64_t readCalls_ = 0;
};

bool DecodeStb(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodeJpeg(const std::filesystem::path&, DecodedImage&, std::string&,
    int minimumWidth = 0, int minimumHeight = 0, int* sourceWidth = nullptr,
    int* sourceHeight = nullptr, const WorkContext& context = {});
bool ReadJpegSize(const std::filesystem::path&, int&, int&, std::string&,
    const WorkContext&);
bool ReadJpegCropMetadataCore(const std::filesystem::path&, JpegCropMetadata&,
	std::string&, const WorkContext&);
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
using JpegInputOpenedTestHook = void (*)(const std::filesystem::path&, void*);
void SetJpegInputOpenedTestHookForTesting(JpegInputOpenedTestHook hook, void* context);
using PsdOutputAllocationTestHook = bool (*)(std::size_t, void*);
void SetPsdOutputAllocationTestHookForTesting(PsdOutputAllocationTestHook hook, void* context);
#endif
bool DecodeApng(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodeQoi(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodePsd(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodePnm(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodeGif(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext&);
bool DecodeWebP(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodeTiff(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext&);
bool DecodeHeif(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodeAvif(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodeJxl(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodeJxr(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodeRaw(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext& context = {});
bool DecodeSvg(const std::filesystem::path&, DecodedImage&, std::string&,
	const WorkContext&);
bool DecodeSvgForDisplay(const std::filesystem::path&, int, int, DecodedImage&,
	int&, int&, std::string&, const WorkContext&, bool allowUpscale = true);
bool ReadSvgDimensions(const std::filesystem::path&, int&, int&, std::string&,
	const WorkContext&);

} // namespace jpegview_linux::decoder_detail
