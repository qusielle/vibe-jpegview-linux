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
};

int ReadStb(void* user, char* destination, int requested) {
	StbInput* input = static_cast<StbInput*>(user);
	if (input == nullptr || requested <= 0) return 0;
	return static_cast<int>(input->reads->Read(input->file, destination,
		static_cast<std::size_t>(requested)));
}

void SkipStb(void* user, int bytes) {
	StbInput* input = static_cast<StbInput*>(user);
	if (input != nullptr) (void)std::fseek(input->file, bytes, SEEK_CUR);
}

int IsEofStb(void* user) {
	const StbInput* input = static_cast<const StbInput*>(user);
	return input == nullptr || std::feof(input->file) != 0 || std::ferror(input->file) != 0;
}

bool DecodeStb(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage) {
	CodecSourceReadTracker sourceReads("stb");
	std::unique_ptr<std::FILE, int (*)(std::FILE*)> file = sourceReads.Enabled() ?
		OpenSourceFile(filename) : std::unique_ptr<std::FILE, int (*)(std::FILE*)>(nullptr, &std::fclose);
	StbInput input{file.get(), &sourceReads};
	const stbi_io_callbacks callbacks = {ReadStb, SkipStb, IsEofStb};
	int channels = 0;
	int width = 0;
	int height = 0;
	unsigned char* rgba = sourceReads.Enabled() ?
		(file == nullptr ? nullptr : stbi_load_from_callbacks(&callbacks, &input,
			&width, &height, &channels, 4)) :
		stbi_load(filename.string().c_str(), &width, &height, &channels, 4);
	if (rgba == nullptr) {
		errorMessage = sourceReads.Enabled() && file == nullptr ? "can't fopen" :
			(stbi_failure_reason() == nullptr ? "unknown decoder error" : stbi_failure_reason());
		return false;
	}
	std::unique_ptr<unsigned char, decltype(&stbi_image_free)> rgbaOwner(
		rgba, &stbi_image_free);
	const std::string extension = Lower(filename.extension().string());
	// PNG tRNS color-key transparency adds alpha to the forced RGBA output while
	// stbi_load still reports the original three color channels.
	const bool hasAlphaChannel = channels == 2 || channels == 4 || extension == ".png";
	return AppendRGBA(image, width, height, rgbaOwner.get(), 0, errorMessage, hasAlphaChannel);
}

} // namespace jpegview_linux::decoder_detail
