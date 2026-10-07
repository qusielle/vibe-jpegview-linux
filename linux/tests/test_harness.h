#pragma once

#include <cstddef>
#include <string>

struct TestCase {
	const char* name;
	void (*function)();
};

struct TestSuite {
	const char* name;
	const TestCase* tests;
	std::size_t count;
};

const TestSuite& GetSourceNavigationSuite();
const TestSuite& GetWorkAdmissionSuite();
const TestSuite& GetCodecDisplaySuite();
const TestSuite& GetImageCacheSuite();
const TestSuite& GetImageOperationsSuite();
const TestSuite& GetImageTransformGeometrySuite();
const TestSuite& GetViewerModelsSuite();
const TestSuite& GetDialogsSessionsSuite();

int RunImageCacheSpecialMode(const std::string& mode);
