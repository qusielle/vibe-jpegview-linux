#include "desktop_association.h"

#include "atomic_file.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

constexpr char kDesktopFileId[] = "jpegview-linux-user.desktop";

std::string QuoteDesktopExecutable(const fs::path& executable) {
	std::string quoted = "\"";
	for (const char character : executable.string()) {
		if (character == '\\' || character == '"') quoted.push_back('\\');
		if (character == '%') quoted.push_back('%');
		quoted.push_back(character);
	}
	quoted.push_back('"');
	return quoted;
}

std::string MimeTypeList() {
	std::ostringstream value;
	for (const std::string& mimeType : DefaultViewerMimeTypes()) value << mimeType << ';';
	return value.str();
}

std::string UpdateMimeApps(const std::string& current) {
	const std::set<std::string> managed(DefaultViewerMimeTypes().begin(), DefaultViewerMimeTypes().end());
	std::set<std::string> missing = managed;
	std::istringstream input(current);
	std::ostringstream output;
	std::string line;
	bool inDefaultApplications = false;
	bool foundDefaultApplications = false;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty() && line.front() == '[') {
			if (inDefaultApplications && !missing.empty()) {
				for (const std::string& mimeType : missing) {
					output << mimeType << '=' << kDesktopFileId << ";\n";
				}
				missing.clear();
			}
			inDefaultApplications = line == "[Default Applications]";
			if (inDefaultApplications) foundDefaultApplications = true;
			output << line << '\n';
			continue;
		}
		if (inDefaultApplications) {
			const std::size_t separator = line.find('=');
			if (separator != std::string::npos) {
				const std::string mimeType = line.substr(0, separator);
				if (managed.find(mimeType) != managed.end()) {
					if (missing.erase(mimeType) != 0) {
						output << mimeType << '=' << kDesktopFileId << ";\n";
					}
					continue;
				}
			}
		}
		output << line << '\n';
	}
	if (inDefaultApplications && !missing.empty()) {
		for (const std::string& mimeType : missing) {
			output << mimeType << '=' << kDesktopFileId << ";\n";
		}
		missing.clear();
	}
	if (!foundDefaultApplications) {
		if (!current.empty() && current.back() != '\n') output << '\n';
		output << "\n[Default Applications]\n";
		for (const std::string& mimeType : missing) {
			output << mimeType << '=' << kDesktopFileId << ";\n";
		}
	}
	return output.str();
}

bool ReadMimeApps(const fs::path& filename, std::string& contents, std::string& errorMessage) {
	std::error_code statusError;
	const bool exists = fs::exists(filename, statusError);
	if (statusError) {
		errorMessage = "cannot inspect " + filename.string() + ": " + statusError.message();
		return false;
	}
	if (!exists) {
		contents.clear();
		return true;
	}
	std::ifstream input(filename);
	if (!input) {
		errorMessage = "cannot read " + filename.string();
		return false;
	}
	std::ostringstream buffer;
	buffer << input.rdbuf();
	if (!input.good() && !input.eof()) {
		errorMessage = "cannot read " + filename.string();
		return false;
	}
	contents = buffer.str();
	return true;
}

} // namespace

const std::vector<std::string>& DefaultViewerMimeTypes() {
	static const std::vector<std::string> mimeTypes = {
		"image/jpeg", "image/png", "image/gif", "image/apng", "image/bmp",
		"image/x-tga", "image/vnd.adobe.photoshop", "image/x-portable-anymap",
		"image/x-portable-bitmap", "image/x-portable-graymap",
		"image/x-portable-pixmap", "image/x-portable-arbitrarymap", "image/qoi",
		"image/webp", "image/tiff", "image/heic", "image/heif", "image/avif",
		"image/jxl", "image/jxr", "image/svg+xml"};
	return mimeTypes;
}

std::string DefaultViewerDesktopEntry(const fs::path& executable) {
	if (executable.empty() || !executable.is_absolute()) return {};
	return "[Desktop Entry]\n"
		"Type=Application\n"
		"Name=JPEGView Linux (User)\n"
		"Comment=Fast, minimal image viewer\n"
		"Exec=" + QuoteDesktopExecutable(executable) + " %F\n"
		"Icon=jpegview-linux\n"
		"Terminal=false\n"
		"NoDisplay=true\n"
		"StartupWMClass=jpegview-linux\n"
		"Categories=Graphics;Viewer;\n"
		"MimeType=" + MimeTypeList() + "\n";
}

bool RegisterDefaultViewer(const fs::path& executable, const fs::path& dataHome,
	const fs::path& configHome, std::string& errorMessage) {
	if (DefaultViewerDesktopEntry(executable).empty()) {
		errorMessage = "cannot register a viewer without an absolute executable path";
		return false;
	}
	if (dataHome.empty() || configHome.empty() || !dataHome.is_absolute() || !configHome.is_absolute()) {
		errorMessage = "cannot determine absolute XDG data and configuration directories";
		return false;
	}
	for (const char character : executable.string()) {
		if (character == '\n' || character == '\r') {
			errorMessage = "the executable path contains an unsupported line break";
			return false;
		}
	}

	const fs::path desktopFile = dataHome / "applications" / kDesktopFileId;
	if (!WriteFileAtomically(desktopFile, DefaultViewerDesktopEntry(executable), errorMessage)) return false;

	const fs::path mimeAppsFile = configHome / "mimeapps.list";
	std::string existing;
	if (!ReadMimeApps(mimeAppsFile, existing, errorMessage)) return false;
	return WriteFileAtomically(mimeAppsFile, UpdateMimeApps(existing), errorMessage);
}

} // namespace jpegview_linux
