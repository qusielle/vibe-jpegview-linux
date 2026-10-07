#include "test_harness.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

std::string gTestExecutablePath;

namespace {

const std::vector<const TestSuite*>& TestSuites() {
	static const std::vector<const TestSuite*> suites = {
		&GetSourceNavigationSuite(),
		&GetWorkAdmissionSuite(),
		&GetCodecDisplaySuite(),
		&GetImageCacheSuite(),
		&GetImageOperationsSuite(),
		&GetImageTransformGeometrySuite(),
		&GetImageTransformPixelsSuite(),
		&GetViewerModelsSuite(),
		&GetDialogsSessionsSuite(),
	};
	return suites;
}

void PrintUsage(std::ostream& output, const char* executable) {
	output << "Usage: " << executable << " [--list | --list-suites | --suite NAME]"
		" [--filter SUBSTRING]\n"
		"Run the complete core suite when no selection option is provided.\n";
}

void RunTest(const TestCase& test, int& failures) {
	try {
		test.function();
		std::cout << "PASS " << test.name << '\n';
	} catch (const std::exception& error) {
		++failures;
		std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
	}
}

} // namespace

int main(int argc, char** argv) {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	if (argc == 3 && std::string(argv[1]) ==
		"--cache-budget-reservation-rejection-race") {
		return RunImageCacheSpecialMode(argv[2]);
	}
#endif
	gTestExecutablePath = argv[0];
	std::string selectedSuite;
	std::string filter;
	bool suiteSpecified = false;
	bool filterSpecified = false;
	bool listTests = false;
	bool listSuites = false;
	for (int index = 1; index < argc; ++index) {
		const std::string argument = argv[index];
		if (argument == "--help" || argument == "-h") {
			PrintUsage(std::cout, argv[0]);
			return 0;
		}
		if (argument == "--list") {
			listTests = true;
			continue;
		}
		if (argument == "--list-suites") {
			listSuites = true;
			continue;
		}
		if ((argument == "--suite" || argument == "--filter") && index + 1 < argc) {
			if (argument == "--suite") {
				suiteSpecified = true;
				selectedSuite = argv[++index];
			} else {
				filterSpecified = true;
				filter = argv[++index];
			}
			continue;
		}
		std::cerr << "Unknown or incomplete test option: " << argument << '\n';
		PrintUsage(std::cerr, argv[0]);
		return 2;
	}
	if (listTests && listSuites) {
		std::cerr << "Choose either --list or --list-suites.\n";
		return 2;
	}
	if ((listTests || listSuites) && (suiteSpecified || filterSpecified)) {
		std::cerr << "Listing options cannot be combined with selection filters.\n";
		return 2;
	}
	if (filterSpecified && filter.empty()) {
		std::cerr << "Test filter cannot be empty.\n";
		return 2;
	}
	if (listSuites) {
		for (const TestSuite* suite : TestSuites()) std::cout << suite->name << '\n';
		return 0;
	}
	if (listTests) {
		for (const TestSuite* suite : TestSuites()) {
			for (std::size_t index = 0; index < suite->count; ++index) {
				std::cout << suite->tests[index].name << '\n';
			}
		}
		return 0;
	}

	const TestSuite* requestedSuite = nullptr;
	if (suiteSpecified) {
		for (const TestSuite* suite : TestSuites()) {
			if (selectedSuite == suite->name) {
				requestedSuite = suite;
				break;
			}
		}
		if (requestedSuite == nullptr) {
			std::cerr << "Unknown test suite: " << selectedSuite << '\n';
			return 2;
		}
	}

	std::vector<const TestCase*> selectedTests;
	for (const TestSuite* suite : TestSuites()) {
		if (requestedSuite != nullptr && suite != requestedSuite) continue;
		for (std::size_t index = 0; index < suite->count; ++index) {
			const TestCase& test = suite->tests[index];
			if (!filterSpecified || std::string(test.name).find(filter) != std::string::npos) {
				selectedTests.push_back(&test);
			}
		}
	}
	if (selectedTests.empty()) {
		std::cerr << "No core tests matched the requested suite and filter.\n";
		return 2;
	}

	int failures = 0;
	for (const TestCase* test : selectedTests) RunTest(*test, failures);
	if (failures != 0) {
		std::cerr << failures << " test group(s) failed\n";
		return 1;
	}
	if (!suiteSpecified && !filterSpecified) {
		std::cout << "All core tests passed\n";
	} else {
		std::cout << "All selected core tests passed (" << selectedTests.size() << ")\n";
	}
	return 0;
}
