#include <archive.h>
#include <archive_entry.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool WriteEntry(struct archive* writer, const char* name, const std::string& contents) {
	struct archive_entry* entry = archive_entry_new();
	if (entry == nullptr) return false;
	archive_entry_set_pathname(entry, name);
	archive_entry_set_filetype(entry, AE_IFREG);
	archive_entry_set_perm(entry, 0644);
	archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
	archive_entry_set_mtime(entry, 1700000000, 0);
	const int headerResult = archive_write_header(writer, entry);
	archive_entry_free(entry);
	return headerResult == ARCHIVE_OK &&
		archive_write_data(writer, contents.data(), contents.size()) ==
			static_cast<la_ssize_t>(contents.size()) &&
		archive_write_finish_entry(writer) == ARCHIVE_OK;
}

bool ReadFile(const char* path, std::string& contents) {
	std::ifstream input(path, std::ios::binary);
	if (!input) return false;
	contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	return input.good() || input.eof();
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 2 && argc != 4) {
		std::cerr << "Usage: write-7z-fixture OUTPUT.7z [ROOT_FILE NESTED_FILE]\n";
		return 2;
	}
	struct archive* writer = archive_write_new();
	if (writer == nullptr) {
		std::cerr << "Cannot allocate 7z fixture writer\n";
		return 1;
	}
	bool success = archive_write_set_format_7zip(writer) == ARCHIVE_OK &&
		archive_write_open_filename(writer, argv[1]) == ARCHIVE_OK;
	if (success && argc == 2) {
		std::string pixels = "P6\n2 2\n255\n";
		pixels.append("\x30\x60\x90\x40\x70\xa0\x50\x80\xb0\x60\x90\xc0", 12);
		success = WriteEntry(writer, "inside-7z.ppm", pixels);
	} else if (success) {
		std::string rootContents;
		std::string nestedContents;
		success = ReadFile(argv[2], rootContents) && ReadFile(argv[3], nestedContents) &&
			WriteEntry(writer, "sample.bmp", rootContents) &&
			WriteEntry(writer, "nested/sample.png", nestedContents);
	}
	if (success) success = archive_write_close(writer) == ARCHIVE_OK;
	if (!success) {
		const char* message = archive_error_string(writer);
		std::cerr << (message == nullptr ? "Cannot create 7z fixture" : message) << '\n';
		archive_write_free(writer);
		return 1;
	}
	archive_write_free(writer);
	return 0;
}
