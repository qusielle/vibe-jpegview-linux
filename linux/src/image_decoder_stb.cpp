#include "image_decoder_internal.h"

#include <cstdio>
#include <memory>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../third_party/stb_image.h"

namespace jpegview_linux::decoder_detail {

struct StbInput {
	std::FILE* file = nullptr;
	CodecSourceReadTracker* reads = nullptr;
	WorkContext context;
	bool cancelled = false;
};

int ReadStb(void* user, char* destination, int requested) {
	StbInput* input = static_cast<StbInput*>(user);
	if (input == nullptr || requested <= 0) return 0;
	if (!input->context.Continue()) {
		input->cancelled = true;
		return 0;
	}
	const int read = static_cast<int>(input->reads->Read(input->file, destination,
		static_cast<std::size_t>(requested)));
	if (!input->context.Continue()) {
		input->cancelled = true;
		return 0;
	}
	return read;
}

void SkipStb(void* user, int bytes) {
	StbInput* input = static_cast<StbInput*>(user);
	if (input == nullptr) return;
	if (!input->context.Continue()) {
		input->cancelled = true;
		return;
	}
	(void)std::fseek(input->file, bytes, SEEK_CUR);
}

int IsEofStb(void* user) {
	StbInput* input = static_cast<StbInput*>(user);
	if (input == nullptr) return 1;
	if (!input->context.Continue()) {
		input->cancelled = true;
		return 1;
	}
	return std::feof(input->file) != 0 || std::ferror(input->file) != 0;
}

bool DecodeStb(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage, const WorkContext& supplied) {
	const WorkContext context = ResolveWorkContext(filename,
		SourceWorkPriority::Foreground, supplied);
	if (!CheckDecoderCancellation(context, errorMessage)) return false;
	CodecSourceReadTracker sourceReads("stb");
	std::unique_ptr<std::FILE, int (*)(std::FILE*)> file = OpenSourceFile(filename);
	if (file == nullptr) {
		errorMessage = "can't fopen";
		return false;
	}
	StbInput input{file.get(), &sourceReads, context};
	const stbi_io_callbacks callbacks = {ReadStb, SkipStb, IsEofStb};
	int channels = 0;
	int width = 0;
	int height = 0;
	if (stbi_info_from_callbacks(&callbacks, &input, &width, &height, &channels) == 0) {
		if (input.cancelled || !context.Continue()) {
			errorMessage = "source work was cancelled";
			return false;
		}
		errorMessage = stbi_failure_reason() == nullptr ? "invalid image header" : stbi_failure_reason();
		return false;
	}
	if (input.cancelled || !CheckDecoderCancellation(context, errorMessage)) return false;
	// Apply the viewer's limits before stb allocates or decompresses source-sized pixels.
	if (!ValidDimensions(width, height, errorMessage)) return false;
	if (std::fseek(file.get(), 0, SEEK_SET) != 0) {
		errorMessage = "cannot rewind image source";
		return false;
	}
	unsigned char* rgba = stbi_load_from_callbacks(&callbacks, &input,
		&width, &height, &channels, 4);
	if (rgba == nullptr) {
		if (input.cancelled || !context.Continue()) {
			errorMessage = "source work was cancelled";
			return false;
		}
		errorMessage = stbi_failure_reason() == nullptr ? "unknown decoder error" : stbi_failure_reason();
		return false;
	}
	std::unique_ptr<unsigned char, decltype(&stbi_image_free)> rgbaOwner(
		rgba, &stbi_image_free);
	const std::string extension = Lower(filename.extension().string());
	// PNG tRNS color-key transparency adds alpha to the forced RGBA output while
	// stbi_load still reports the original three color channels.
	const bool hasAlphaChannel = channels == 2 || channels == 4 || extension == ".png";
	AnimationFrameBudget frameBudget(image);
	return AppendRGBA(image, width, height, rgbaOwner.get(), 0, errorMessage,
		hasAlphaChannel, context, frameBudget);
}

} // namespace jpegview_linux::decoder_detail
