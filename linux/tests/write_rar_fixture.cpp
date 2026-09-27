#include "rar_test_fixtures.h"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
	if (argc != 2 && argc != 3) {
		std::cerr << "Usage: write-rar-fixture OUTPUT.rar [rar4|rar5-solid]\n";
		return 2;
	}
	const std::string format = argc == 3 ? argv[2] : "rar5-solid";
	std::vector<std::uint8_t> bytes;
	if (format == "rar4") bytes = rar_test_fixtures::Rar4ImageArchive();
	else if (format == "rar5-solid") bytes = rar_test_fixtures::Rar5SolidImageArchive();
	else {
		std::cerr << "Unknown RAR fixture format: " << format << '\n';
		return 2;
	}
	if (bytes.empty()) {
		std::cerr << "Could not decode the embedded RAR fixture\n";
		return 1;
	}
	std::ofstream output(argv[1], std::ios::binary);
	if (!output) {
		std::cerr << "Could not create output file: " << argv[1] << '\n';
		return 1;
	}
	output.write(reinterpret_cast<const char*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	if (!output) {
		std::cerr << "Could not write output file: " << argv[1] << '\n';
		return 1;
	}
	return 0;
}
