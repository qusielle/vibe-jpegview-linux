#include <zip.h>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
	if (argc != 2) {
		std::cerr << "Usage: write-encrypted-zip-fixture OUTPUT.zip\n";
		return 2;
	}
	int errorCode = 0;
	zip_t* archive = zip_open(argv[1], ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
	if (archive == nullptr) {
		std::cerr << "Cannot create ZIP fixture: " << errorCode << '\n';
		return 1;
	}
	std::string pixels = "P6\n2 2\n255\n";
	pixels.append("\x30\x60\x90\x40\x70\xa0\x50\x80\xb0\x60\x90\xc0", 12);
	zip_source_t* source = zip_source_buffer(archive, pixels.data(), pixels.size(), 0);
	if (source == nullptr) {
		std::cerr << "Cannot create ZIP fixture source: " << zip_strerror(archive) << '\n';
		zip_discard(archive);
		return 1;
	}
	const zip_int64_t index = zip_file_add(archive, "inside-password.ppm", source,
		ZIP_FL_ENC_UTF_8);
	if (index < 0) {
		std::cerr << "Cannot add ZIP fixture image: " << zip_strerror(archive) << '\n';
		zip_source_free(source);
		zip_discard(archive);
		return 1;
	}
	if (zip_file_set_encryption(archive, static_cast<zip_uint64_t>(index),
		ZIP_EM_TRAD_PKWARE, "jpegview-test-password") != 0) {
		std::cerr << "Cannot encrypt ZIP fixture image: " << zip_strerror(archive) << '\n';
		zip_discard(archive);
		return 1;
	}
	if (zip_close(archive) != 0) {
		std::cerr << "Cannot finish ZIP fixture: " << zip_strerror(archive) << '\n';
		zip_discard(archive);
		return 1;
	}
	return 0;
}
