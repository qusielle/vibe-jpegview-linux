#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace jpegview_linux {

struct OpenWithApplication {
	std::string name;
	std::string exec;
	std::filesystem::path desktopFile;
	bool terminal = false;
};

std::string FileUri(const std::filesystem::path& filename);
std::string UnescapeDesktopValue(const std::string& value);
bool MimeTypeMatches(const std::string& mimeTypes, const std::string& currentMime);
std::string MimeTypeForExtension(const std::string& extension);
bool ReadDesktopApplication(const std::filesystem::path& filename, const std::string& currentMime,
	OpenWithApplication& application);
std::vector<OpenWithApplication> DiscoverOpenWithApplications(const std::string& extension);

std::vector<std::string> TokenizeDesktopExec(const std::string& commandLine);
std::vector<std::string> DesktopExecArguments(const OpenWithApplication& application,
	const std::filesystem::path& imageFilename);

} // namespace jpegview_linux
