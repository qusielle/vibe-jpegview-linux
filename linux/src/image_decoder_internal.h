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

std::string Lower(std::string value);
bool ValidDimensions(int width, int height, std::string& errorMessage);
bool AppendBGRA(DecodedImage& image, int width, int height, const std::uint8_t* pixels,
    int delayMs, std::string& errorMessage, bool hasAlphaChannel = true);
bool AppendBGRA(DecodedImage& image, int width, int height,
    std::vector<std::uint8_t>&& pixels, int delayMs, std::string& errorMessage,
    bool hasAlphaChannel = true);
bool AppendRGBA(DecodedImage& image, int width, int height, const std::uint8_t* pixels,
    int delayMs, std::string& errorMessage, bool hasAlphaChannel = true);
bool AppendRGBA(DecodedImage& image, int width, int height,
    std::vector<std::uint8_t>&& pixels, int delayMs, std::string& errorMessage,
    bool hasAlphaChannel = true);
bool ApplyIccProfile(std::vector<std::uint8_t>& bgra,
    const std::vector<std::uint8_t>& profile);
bool ReadFile(const std::filesystem::path& filename,
    std::vector<std::uint8_t>& data, std::string& errorMessage);
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

bool DecodeStb(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeJpeg(const std::filesystem::path&, DecodedImage&, std::string&,
    int minimumWidth = 0, int minimumHeight = 0, int* sourceWidth = nullptr,
    int* sourceHeight = nullptr, const WorkContext& context = {});
bool ReadJpegSize(const std::filesystem::path&, int&, int&, std::string&,
    const WorkContext&);
bool ReadJpegMcuSizeCore(const std::filesystem::path&, int&, int&,
    std::string&, const WorkContext&);
bool DecodeApng(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeQoi(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodePsd(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodePnm(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeGif(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeWebP(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeTiff(const std::filesystem::path&, DecodedImage&, std::string&,
    const WorkContext&);
bool DecodeHeif(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeAvif(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeJxl(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeJxr(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeRaw(const std::filesystem::path&, DecodedImage&, std::string&);
bool DecodeSvg(const std::filesystem::path&, DecodedImage&, std::string&,
	const WorkContext&);
bool DecodeSvgForDisplay(const std::filesystem::path&, int, int, DecodedImage&,
	int&, int&, std::string&, const WorkContext&, bool allowUpscale = true);
bool ReadSvgDimensions(const std::filesystem::path&, int&, int&, std::string&,
	const WorkContext&);

} // namespace jpegview_linux::decoder_detail
