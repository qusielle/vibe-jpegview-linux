#include "image_decoder.h"
#include "image_decoder_internal.h"
#include "archive_source.h"
#include "exif_orientation.h"
#include "image_formats.h"
#include "image_metadata_reader.h"
#include "perf_diagnostics.h"
#include "source_work_coordinator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <utility>

#if JPEGVIEW_HAVE_LCMS2
#include <lcms2.h>
#endif

namespace jpegview_linux {
namespace decoder_detail {

constexpr std::uint64_t kMaxImagePixels = 100ull * 1024ull * 1024ull;

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

bool ValidDimensions(int width, int height, std::string& errorMessage) {
	if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
		errorMessage = "image dimensions are not supported";
		return false;
	}
	if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxImagePixels) {
		errorMessage = "image is too large";
		return false;
	}
	return true;
}

bool AppendBGRA(DecodedImage& image, int width, int height, const std::uint8_t* pixels,
	int delayMs, std::string& errorMessage, bool hasAlphaChannel) {
	if (!ValidDimensions(width, height, errorMessage) || pixels == nullptr) return false;
	const std::size_t byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
	std::uint64_t currentBytes = 0;
	for (const DecodedFrame& frame : image.frames) currentBytes += frame.bgra.size();
	if (byteCount > kMaxAnimationBytes || currentBytes > kMaxAnimationBytes - byteCount) {
		errorMessage = "animation is too large";
		return false;
	}
	try {
		DecodedFrame frame;
		frame.width = width;
		frame.height = height;
		frame.delayMs = std::max(0, delayMs);
		frame.bgra.assign(pixels, pixels + byteCount);
		if (hasAlphaChannel) {
			for (std::size_t offset = 3; offset < byteCount; offset += 4) {
				if (frame.bgra[offset] != 255) {
					frame.hasTransparency = true;
					break;
				}
			}
		}
		image.frames.push_back(std::move(frame));
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	return true;
}

bool AppendBGRA(DecodedImage& image, int width, int height,
	std::vector<std::uint8_t>&& pixels, int delayMs, std::string& errorMessage,
	bool hasAlphaChannel) {
	if (!ValidDimensions(width, height, errorMessage)) return false;
	const std::size_t byteCount = static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height) * 4;
	if (pixels.size() != byteCount) {
		errorMessage = "invalid decoded pixel buffer";
		return false;
	}
	std::uint64_t currentBytes = 0;
	for (const DecodedFrame& existing : image.frames) currentBytes += existing.bgra.size();
	if (byteCount > kMaxAnimationBytes || currentBytes > kMaxAnimationBytes - byteCount) {
		errorMessage = "animation is too large";
		return false;
	}
	try {
		DecodedFrame frame;
		frame.width = width;
		frame.height = height;
		frame.delayMs = std::max(0, delayMs);
		frame.bgra = std::move(pixels);
		if (hasAlphaChannel) {
			for (std::size_t offset = 3; offset < byteCount; offset += 4) {
				if (frame.bgra[offset] != 255) {
					frame.hasTransparency = true;
					break;
				}
			}
		}
		image.frames.push_back(std::move(frame));
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	return true;
}

bool AppendRGBA(DecodedImage& image, int width, int height, const std::uint8_t* pixels,
	int delayMs, std::string& errorMessage, bool hasAlphaChannel) {
	if (!ValidDimensions(width, height, errorMessage) || pixels == nullptr) return false;
	const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
	std::vector<std::uint8_t> bgra;
	try {
		bgra.resize(pixelCount * 4);
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	bool hasTransparency = false;
	for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
		bgra[pixel * 4] = pixels[pixel * 4 + 2];
		bgra[pixel * 4 + 1] = pixels[pixel * 4 + 1];
		bgra[pixel * 4 + 2] = pixels[pixel * 4];
		bgra[pixel * 4 + 3] = pixels[pixel * 4 + 3];
		if (hasAlphaChannel && pixels[pixel * 4 + 3] != 255) hasTransparency = true;
	}
	try {
		DecodedFrame frame;
		frame.width = width;
		frame.height = height;
		frame.delayMs = std::max(0, delayMs);
		frame.bgra = std::move(bgra);
		frame.hasTransparency = hasTransparency;
		std::uint64_t currentBytes = 0;
		for (const DecodedFrame& existing : image.frames) currentBytes += existing.bgra.size();
		if (frame.bgra.size() > kMaxAnimationBytes || currentBytes > kMaxAnimationBytes - frame.bgra.size()) {
			errorMessage = "animation is too large";
			return false;
		}
		image.frames.push_back(std::move(frame));
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	return true;
}

bool AppendRGBA(DecodedImage& image, int width, int height,
	std::vector<std::uint8_t>&& pixels, int delayMs, std::string& errorMessage,
	bool hasAlphaChannel) {
	if (!ValidDimensions(width, height, errorMessage)) return false;
	const std::size_t byteCount = static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height) * 4;
	if (pixels.size() != byteCount) {
		errorMessage = "invalid decoded pixel buffer";
		return false;
	}
	for (std::size_t offset = 0; offset < byteCount; offset += 4) {
		std::swap(pixels[offset], pixels[offset + 2]);
	}
	return AppendBGRA(image, width, height, std::move(pixels), delayMs,
		errorMessage, hasAlphaChannel);
}

[[maybe_unused]] bool ApplyIccProfile(std::vector<std::uint8_t>& bgra, const std::vector<std::uint8_t>& profile) {
#if JPEGVIEW_HAVE_LCMS2
	if (profile.empty() || profile.size() > std::numeric_limits<cmsUInt32Number>::max() ||
		bgra.empty() || bgra.size() / 4 > std::numeric_limits<cmsUInt32Number>::max()) return false;
	struct IccProfileHandles {
		cmsHPROFILE source = nullptr;
		cmsHPROFILE destination = nullptr;
		cmsHTRANSFORM transform = nullptr;
		IccProfileHandles() = default;
		IccProfileHandles(const IccProfileHandles&) = delete;
		IccProfileHandles& operator=(const IccProfileHandles&) = delete;
		~IccProfileHandles() {
			if (transform != nullptr) cmsDeleteTransform(transform);
			if (source != nullptr) cmsCloseProfile(source);
			if (destination != nullptr) cmsCloseProfile(destination);
		}
	} handles;
	handles.source = cmsOpenProfileFromMem(profile.data(),
		static_cast<cmsUInt32Number>(profile.size()));
	handles.destination = cmsCreate_sRGBProfile();
	if (handles.source == nullptr || handles.destination == nullptr) return false;
	handles.transform = cmsCreateTransform(handles.source, TYPE_BGRA_8,
		handles.destination, TYPE_BGRA_8,
		INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA);
	if (handles.transform == nullptr) return false;
	cmsDoTransform(handles.transform, bgra.data(), bgra.data(),
		static_cast<cmsUInt32Number>(bgra.size() / 4));
	return true;
#else
	(void)bgra;
	(void)profile;
	return false;
#endif
}

bool ReadFile(const std::filesystem::path& filename, std::vector<std::uint8_t>& data,
	std::string& errorMessage) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::SourceRead);
	std::ifstream input(filename, std::ios::binary);
	if (!input) {
		errorMessage = "cannot open file";
		return false;
	}
	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	if (size <= 0) {
		errorMessage = "empty file";
		return false;
	}
	input.seekg(0, std::ios::beg);
	try {
		data.resize(static_cast<std::size_t>(size));
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	input.read(reinterpret_cast<char*>(data.data()), size);
	if (!input) {
		errorMessage = "cannot read file";
		return false;
	}
	return true;
}

std::unique_ptr<std::FILE, int (*)(std::FILE*)> OpenSourceFile(
	const std::filesystem::path& filename) {
	return std::unique_ptr<std::FILE, int (*)(std::FILE*)>(
		std::fopen(filename.string().c_str(), "rb"), &std::fclose);
}

std::uint32_t ReadBE32(const std::uint8_t* data) {
	return (static_cast<std::uint32_t>(data[0]) << 24) |
		(static_cast<std::uint32_t>(data[1]) << 16) |
		(static_cast<std::uint32_t>(data[2]) << 8) | data[3];
}

std::uint16_t ReadBE16(const std::uint8_t* data) {
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}


} // namespace

namespace {

template <typename Operation>
bool RunWithSourceAndCpuAdmission(const std::filesystem::path& filename,
	SourceWorkPriority defaultPriority, const WorkContext& supplied,
	std::string& errorMessage, Operation operation) {
	try {
	WorkContext context = ResolveWorkContext(filename, defaultPriority, supplied);
	if (!context.Continue()) {
		errorMessage = "source work was cancelled";
		return false;
	}
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
		context, filename);
	if (!admission) {
		errorMessage = "source or CPU work admission was cancelled";
		return false;
	}
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	ScopedWorkContext activeContext(context);
	try {
		if (!context.Continue()) {
			errorMessage = "source work was cancelled";
			return false;
		}
		const bool succeeded = operation(context);
		if (!context.Continue()) {
			errorMessage = "source work was cancelled";
			return false;
		}
		return succeeded;
	} catch (const std::exception& error) {
		errorMessage = error.what();
		return false;
	} catch (...) {
		errorMessage = "unknown image decoder failure";
		return false;
	}
	} catch (const std::exception& error) {
		errorMessage = error.what();
		return false;
	} catch (...) {
		errorMessage = "unknown image admission failure";
		return false;
	}
}

bool IsRawPath(const std::filesystem::path& filename) {
	static const std::array<const char*, 25> extensions = {{
		".pef", ".dng", ".crw", ".nef", ".cr2", ".mrw", ".rw2", ".orf", ".x3f", ".arw",
		".kdc", ".nrw", ".dcr", ".sr2", ".raf", ".kc2", ".erf", ".3fr", ".raw", ".mef",
		".mos", ".mdc", ".cr3", ".iiq", ".rwl"
	}};
	const std::string extension = decoder_detail::Lower(filename.extension().string());
	return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool ApplyEmbeddedExifOrientation(const std::filesystem::path& filename,
	DecodedImage& image, std::string& errorMessage, const WorkContext& context) {
	ExifInfo metadata;
	std::string ignoredComment;
	if (!ReadImageMetadata(filename, metadata, ignoredComment, context)) {
		if (!context.Continue()) {
			errorMessage = "source work was cancelled";
			image = {};
			return false;
		}
		return true;
	}
	if (metadata.imageOrientation == 1) return true;
	for (DecodedFrame& frame : image.frames) {
		if (!ApplyExifOrientation(metadata.imageOrientation, frame.width, frame.height,
			frame.bgra, errorMessage, [&context] { return context.Continue(); })) {
			image = {};
			return false;
		}
	}
	return true;
}

bool DecodeDetectedImageContent(const std::filesystem::path& filename,
	ImageContentFormat format, DecodedImage& image, std::string& errorMessage,
	const WorkContext& context) {
	// TIFF magic also belongs to camera containers whose first IFD can be only a preview.
	if (format == ImageContentFormat::Tiff && IsRawPath(filename)) {
		format = ImageContentFormat::Raw;
	}
	switch (format) {
	case ImageContentFormat::Jpeg:
		return decoder_detail::DecodeJpeg(filename, image, errorMessage, 0, 0,
			nullptr, nullptr, context);
	case ImageContentFormat::Png:
		return decoder_detail::DecodeStb(filename, image, errorMessage) &&
			ApplyEmbeddedExifOrientation(filename, image, errorMessage, context);
	case ImageContentFormat::Apng:
		return decoder_detail::DecodeApng(filename, image, errorMessage) &&
			ApplyEmbeddedExifOrientation(filename, image, errorMessage, context);
	case ImageContentFormat::Gif:
#if JPEGVIEW_HAVE_GIF
		return decoder_detail::DecodeGif(filename, image, errorMessage);
#else
		return decoder_detail::DecodeStb(filename, image, errorMessage);
#endif
	case ImageContentFormat::Bmp:
	case ImageContentFormat::Tga:
		return decoder_detail::DecodeStb(filename, image, errorMessage);
	case ImageContentFormat::WebP:
#if JPEGVIEW_HAVE_WEBP
		return decoder_detail::DecodeWebP(filename, image, errorMessage) &&
			ApplyEmbeddedExifOrientation(filename, image, errorMessage, context);
#else
		errorMessage = "WebP content detected, but WebP support is not available in this build";
		return false;
#endif
	case ImageContentFormat::Tiff: {
#if JPEGVIEW_HAVE_TIFF
		if (decoder_detail::DecodeTiff(filename, image, errorMessage, context)) return true;
		const std::string tiffError = errorMessage;
#if JPEGVIEW_HAVE_RAW
		image = {};
		if (decoder_detail::DecodeRaw(filename, image, errorMessage)) return true;
#endif
		errorMessage = tiffError;
		return false;
#elif JPEGVIEW_HAVE_RAW
		if (decoder_detail::DecodeRaw(filename, image, errorMessage)) return true;
		errorMessage = "TIFF support is not available in this build";
		return false;
#else
		errorMessage = "TIFF support is not available in this build";
		return false;
#endif
	}
	case ImageContentFormat::Heif:
#if JPEGVIEW_HAVE_HEIF
		return decoder_detail::DecodeHeif(filename, image, errorMessage);
#else
		errorMessage = "HEIF content detected, but HEIF support is not available in this build";
		return false;
#endif
	case ImageContentFormat::Avif:
#if JPEGVIEW_HAVE_AVIF
		return decoder_detail::DecodeAvif(filename, image, errorMessage);
#elif JPEGVIEW_HAVE_HEIF
		return decoder_detail::DecodeHeif(filename, image, errorMessage);
#else
		errorMessage = "AVIF content detected, but AVIF support is not available in this build";
		return false;
#endif
	case ImageContentFormat::JpegXl:
#if JPEGVIEW_HAVE_JXL
		return decoder_detail::DecodeJxl(filename, image, errorMessage);
#else
		errorMessage = "JPEG XL content detected, but JPEG XL support is not available in this build";
		return false;
#endif
	case ImageContentFormat::JpegXr:
#if JPEGVIEW_HAVE_JXR
		return decoder_detail::DecodeJxr(filename, image, errorMessage);
#else
		errorMessage = "JPEG XR content detected, but JPEG XR support is not available in this build";
		return false;
#endif
	case ImageContentFormat::Psd:
		return decoder_detail::DecodePsd(filename, image, errorMessage);
	case ImageContentFormat::Pnm:
		return decoder_detail::DecodePnm(filename, image, errorMessage);
	case ImageContentFormat::Qoi:
		return decoder_detail::DecodeQoi(filename, image, errorMessage);
	case ImageContentFormat::Svg:
	case ImageContentFormat::Svgz:
		if (!decoder_detail::DecodeSvg(filename, image, errorMessage, context)) return false;
		image.isSvg = true;
		return true;
	case ImageContentFormat::Raw:
#if JPEGVIEW_HAVE_RAW
		return decoder_detail::DecodeRaw(filename, image, errorMessage);
#else
		errorMessage = "RAW content detected, but RAW support is not available in this build";
		return false;
#endif
	case ImageContentFormat::Unknown:
		break;
	}
	errorMessage = "unsupported image content";
	return false;
}

bool DecodeDetectedContentAfterFailure(const std::filesystem::path& filename,
	DecodedImage& image, std::string& errorMessage, const WorkContext& context,
	int* sourceWidth = nullptr, int* sourceHeight = nullptr) {
	const std::string originalError = errorMessage;
	image = {};
	if (sourceWidth != nullptr) *sourceWidth = 0;
	if (sourceHeight != nullptr) *sourceHeight = 0;
	if (!context.Continue()) {
		errorMessage = "source work was cancelled";
		return false;
	}
	const ImageContentFormat format = ReadImageContentFormat(filename,
		[&context] { return context.Continue(); });
	if (!context.Continue()) {
		errorMessage = "source work was cancelled";
		return false;
	}
	if (format == ImageContentFormat::Unknown) {
		errorMessage = originalError;
		return false;
	}
	errorMessage.clear();
	if (!DecodeDetectedImageContent(filename, format, image, errorMessage, context) ||
		image.frames.empty()) {
		image = {};
		if (errorMessage.empty()) errorMessage = "invalid image content";
		return false;
	}
	if (sourceWidth != nullptr) *sourceWidth = image.frames.front().width;
	if (sourceHeight != nullptr) *sourceHeight = image.frames.front().height;
	return true;
}


} // namespace

bool IsJpegPath(const std::filesystem::path& filename) {
	const std::string extension = decoder_detail::Lower(filename.extension().string());
	return extension == ".jpg" || extension == ".jpeg" || extension == ".jpe";
}

bool IsSvgPath(const std::filesystem::path& filename) {
	const std::string extension = decoder_detail::Lower(filename.extension().string());
	return extension == ".svg" || extension == ".svgz";
}

bool ReadJpegDimensions(const std::filesystem::path& filename, int& width, int& height,
	std::string& errorMessage, const WorkContext& supplied) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Metadata);
	width = 0;
	height = 0;
	errorMessage.clear();
	return RunWithSourceAndCpuAdmission(filename, SourceWorkPriority::Metadata,
		supplied, errorMessage, [&filename, &width, &height, &errorMessage](
			const WorkContext& context) {
			if (!IsJpegPath(filename)) {
				errorMessage = "invalid JPEG header";
				return false;
			}
			if (IsArchiveMemberLocation(filename)) {
				return WithArchiveMemberFile(filename,
					[&width, &height, context](const std::filesystem::path& temporary,
						std::string& decodeError) {
						return ReadJpegDimensions(temporary, width, height, decodeError, context);
					}, errorMessage, nullptr,
					[context] { return context.Continue(); });
			}
			return decoder_detail::ReadJpegSize(filename, width, height, errorMessage, context);
		});
}

bool ReadSourceDimensions(const std::filesystem::path& filename, int& width, int& height,
	std::string& errorMessage, const WorkContext& context, bool* isSvg) {
	if (isSvg != nullptr) *isSvg = false;
	if (IsSvgPath(filename)) {
		const bool succeeded = ReadSvgDimensions(filename, width, height, errorMessage,
			context);
		if (succeeded && isSvg != nullptr) *isSvg = true;
		return succeeded;
	}
	if (IsJpegPath(filename)) {
		if (ReadJpegDimensions(filename, width, height, errorMessage, context)) return true;
		const ImageContentFormat format = ReadImageContentFormat(filename,
			[&context] { return context.Continue(); });
		if (format == ImageContentFormat::Svg || format == ImageContentFormat::Svgz) {
			const bool succeeded = ReadSvgDimensions(filename, width, height,
				errorMessage, context);
			if (succeeded && isSvg != nullptr) *isSvg = true;
			return succeeded;
		}
		return false;
	}
	width = 0;
	height = 0;
	errorMessage = "source format has no lightweight dimensions reader";
	return false;
}

bool ReadSvgDimensions(const std::filesystem::path& filename, int& width,
	int& height, std::string& errorMessage, const WorkContext& supplied) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Metadata);
	width = 0;
	height = 0;
	errorMessage.clear();
	return RunWithSourceAndCpuAdmission(filename, SourceWorkPriority::Metadata,
		supplied, errorMessage, [&filename, &width, &height, &errorMessage](
			const WorkContext& context) {
			if (IsArchiveMemberLocation(filename)) {
				return WithArchiveMemberFile(filename,
					[&width, &height, context](const std::filesystem::path& temporary,
						std::string& decodeError) {
						return decoder_detail::ReadSvgDimensions(temporary, width, height,
							decodeError, context);
					}, errorMessage, nullptr,
					[context] { return context.Continue(); });
			}
			return decoder_detail::ReadSvgDimensions(filename, width, height,
				errorMessage, context);
		});
}

bool DecodeSvgForDisplay(const std::filesystem::path& filename, int targetWidth,
	int targetHeight, DecodedImage& image, int& sourceWidth, int& sourceHeight,
	std::string& errorMessage, const WorkContext& supplied, bool allowUpscale) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Decode);
	image = {};
	sourceWidth = sourceHeight = 0;
	errorMessage.clear();
	return RunWithSourceAndCpuAdmission(filename, SourceWorkPriority::Foreground,
		supplied, errorMessage, [&filename, targetWidth, targetHeight, &image,
			&sourceWidth, &sourceHeight, &errorMessage, allowUpscale](const WorkContext& context) {
			if (targetWidth <= 0 || targetHeight <= 0) {
				errorMessage = "invalid SVG display request";
				return false;
			}
			if (IsArchiveMemberLocation(filename)) {
				return WithArchiveMemberFile(filename,
					[targetWidth, targetHeight, &image, &sourceWidth, &sourceHeight,
						context, allowUpscale](const std::filesystem::path& temporary,
						std::string& decodeError) {
						return decoder_detail::DecodeSvgForDisplay(temporary, targetWidth,
							targetHeight, image, sourceWidth, sourceHeight, decodeError,
							context, allowUpscale);
					}, errorMessage, nullptr,
					[context] { return context.Continue(); });
			}
			return decoder_detail::DecodeSvgForDisplay(filename, targetWidth,
				targetHeight, image, sourceWidth, sourceHeight, errorMessage, context,
				allowUpscale);
		});
}

bool ReadJpegMcuSize(const std::filesystem::path& filename, int& width, int& height,
	std::string& errorMessage, const WorkContext& supplied) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Metadata);
	width = 0;
	height = 0;
	errorMessage.clear();
	return RunWithSourceAndCpuAdmission(filename, SourceWorkPriority::Metadata,
		supplied, errorMessage, [&filename, &width, &height, &errorMessage](
			const WorkContext& context) {
			if (!IsJpegPath(filename)) {
				errorMessage = "invalid JPEG header";
				return false;
			}
			return decoder_detail::ReadJpegMcuSizeCore(filename, width, height, errorMessage, context);
		});
}

bool DecodeJpegForDisplay(const std::filesystem::path& filename,
	int minimumWidth, int minimumHeight, DecodedImage& image,
	int& sourceWidth, int& sourceHeight, std::string& errorMessage,
	const WorkContext& supplied) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Decode);
	image = {};
	sourceWidth = 0;
	sourceHeight = 0;
	errorMessage.clear();
	return RunWithSourceAndCpuAdmission(filename, SourceWorkPriority::Foreground,
		supplied, errorMessage, [&filename, minimumWidth, minimumHeight, &image,
			&sourceWidth, &sourceHeight, &errorMessage](const WorkContext& context) {
			if (!IsJpegPath(filename) || minimumWidth <= 0 || minimumHeight <= 0) {
				errorMessage = "invalid JPEG display request";
				return false;
			}
			if (IsArchiveMemberLocation(filename)) {
				return WithArchiveMemberFile(filename,
					[minimumWidth, minimumHeight, &image, &sourceWidth, &sourceHeight,
						context](const std::filesystem::path& temporary,
						std::string& decodeError) {
						return DecodeJpegForDisplay(temporary, minimumWidth, minimumHeight,
							image, sourceWidth, sourceHeight, decodeError, context);
					}, errorMessage, nullptr,
					[context] { return context.Continue(); });
			}
			if (decoder_detail::DecodeJpeg(filename, image, errorMessage, minimumWidth,
				minimumHeight, &sourceWidth, &sourceHeight, context)) return true;
			return DecodeDetectedContentAfterFailure(filename, image, errorMessage, context,
				&sourceWidth, &sourceHeight);
		});
}

bool DecodeImage(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage, const WorkContext& supplied) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Decode);
	image = {};
	errorMessage.clear();
	return RunWithSourceAndCpuAdmission(filename, SourceWorkPriority::Foreground,
		supplied, errorMessage, [&filename, &image, &errorMessage](
			const WorkContext& context) {
	if (IsArchiveMemberLocation(filename)) {
		return WithArchiveMemberFile(filename,
			[&image, context](const std::filesystem::path& temporary,
				std::string& decodeError) {
				return DecodeImage(temporary, image, decodeError, context);
			}, errorMessage, nullptr, [context] { return context.Continue(); });
	}
	const std::string extension = decoder_detail::Lower(filename.extension().string());
	const ImageContentFormat contentFormat = ReadImageContentFormat(filename,
		[&context] { return context.Continue(); });
	if (contentFormat != ImageContentFormat::Unknown) {
		if (!context.Continue()) {
			errorMessage = "source work was cancelled";
			return false;
		}
		return DecodeDetectedImageContent(filename, contentFormat, image, errorMessage, context);
	}
	if (!context.Continue()) {
		errorMessage = "source work was cancelled";
		return false;
	}
	if (extension == ".kra") {
		return WithArchiveMemberFile(filename / "mergedimage.png",
			[&image, context](const std::filesystem::path& temporary,
				std::string& decodeError) {
				return DecodeImage(temporary, image, decodeError, context);
			}, errorMessage, nullptr, [context] { return context.Continue(); });
	}
	if (IsSvgPath(filename)) {
		if (!decoder_detail::DecodeSvg(filename, image, errorMessage, context)) return false;
		image.isSvg = true;
		return true;
	}
	if (IsJpegPath(filename)) {
		return decoder_detail::DecodeJpeg(filename, image, errorMessage, 0, 0,
			nullptr, nullptr, context);
	}
	if (extension == ".gif") {
#if JPEGVIEW_HAVE_GIF
		return decoder_detail::DecodeGif(filename, image, errorMessage);
#else
		return decoder_detail::DecodeStb(filename, image, errorMessage);
#endif
	}
	if (extension == ".apng") {
		return decoder_detail::DecodeApng(filename, image, errorMessage) &&
			ApplyEmbeddedExifOrientation(filename, image, errorMessage, context);
	}
	if (extension == ".webp") {
#if JPEGVIEW_HAVE_WEBP
		return decoder_detail::DecodeWebP(filename, image, errorMessage) &&
			ApplyEmbeddedExifOrientation(filename, image, errorMessage, context);
#else
		return decoder_detail::DecodeStb(filename, image, errorMessage);
#endif
	}
	if (extension == ".tif" || extension == ".tiff") {
#if JPEGVIEW_HAVE_TIFF
		return decoder_detail::DecodeTiff(filename, image, errorMessage, context);
#else
		errorMessage = "TIFF support is not available in this build";
		return false;
#endif
	}
	if (extension == ".heic" || extension == ".heif" || extension == ".hif") {
#if JPEGVIEW_HAVE_HEIF
		return decoder_detail::DecodeHeif(filename, image, errorMessage);
#else
		errorMessage = "HEIF support is not available in this build";
		return false;
#endif
	}
	if (extension == ".avif" || extension == ".avifs") {
#if JPEGVIEW_HAVE_AVIF
		return decoder_detail::DecodeAvif(filename, image, errorMessage);
#elif JPEGVIEW_HAVE_HEIF
		return decoder_detail::DecodeHeif(filename, image, errorMessage);
#else
		errorMessage = "AVIF support is not available in this build";
		return false;
#endif
	}
	if (extension == ".jxl") {
#if JPEGVIEW_HAVE_JXL
		return decoder_detail::DecodeJxl(filename, image, errorMessage);
#else
		errorMessage = "JPEG XL support is not available in this build";
		return false;
#endif
	}
	if (extension == ".jxr" || extension == ".wdp" || extension == ".hdp" || extension == ".mdp") {
#if JPEGVIEW_HAVE_JXR
		return decoder_detail::DecodeJxr(filename, image, errorMessage);
#else
		errorMessage = "JPEG XR support is not available in this build";
		return false;
#endif
	}
	if (extension == ".psd") return decoder_detail::DecodePsd(filename, image, errorMessage);
	if (IsRawPath(filename)) {
#if JPEGVIEW_HAVE_RAW
		return decoder_detail::DecodeRaw(filename, image, errorMessage);
#else
		errorMessage = "RAW support is not available in this build";
		return false;
#endif
	}
	if (extension == ".pnm" || extension == ".pbm" || extension == ".pgm" ||
		 extension == ".ppm" || extension == ".pam") {
		return decoder_detail::DecodePnm(filename, image, errorMessage);
	}
	if (extension == ".qoi") return decoder_detail::DecodeQoi(filename, image, errorMessage);
	return decoder_detail::DecodeStb(filename, image, errorMessage) &&
		ApplyEmbeddedExifOrientation(filename, image, errorMessage, context);
		});
}
} // namespace jpegview_linux
