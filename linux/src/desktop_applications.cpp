#include "desktop_applications.h"

#include "../../src/JPEGView/resource.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

std::string TrimDesktopValue(const std::string& value) {
	const std::size_t first = value.find_first_not_of(" \t\r");
	if (first == std::string::npos) return {};
	const std::size_t last = value.find_last_not_of(" \t\r");
	return value.substr(first, last - first + 1);
}

bool HasExecutable(const std::string& executable) {
	const char* path = std::getenv("PATH");
	if (path == nullptr) return false;
	const std::string searchPath(path);
	std::size_t begin = 0;
	while (begin <= searchPath.size()) {
		const std::size_t end = searchPath.find(':', begin);
		const fs::path directory = searchPath.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		const fs::path candidate = (directory.empty() ? fs::path(".") : directory) / executable;
		if (access(candidate.c_str(), X_OK) == 0) return true;
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return false;
}

bool DesktopBoolean(const std::string& value) {
	return Lower(TrimDesktopValue(value)) == "true";
}

std::string DesktopEntryField(const std::string& line, const std::string& key) {
	const std::string prefix = key + "=";
	if (line.compare(0, prefix.size(), prefix) != 0) return {};
	return UnescapeDesktopValue(TrimDesktopValue(line.substr(prefix.size())));
}

std::vector<fs::path> DesktopApplicationDirectories() {
	std::vector<fs::path> directories;
	std::set<std::string> seen;
	const auto addDirectory = [&directories, &seen](const fs::path& directory) {
		if (directory.empty()) return;
		const std::string key = directory.lexically_normal().string();
		if (seen.insert(key).second) directories.push_back(directory);
	};

	if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && *dataHome != '\0') {
		addDirectory(fs::path(dataHome) / "applications");
	} else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
		addDirectory(fs::path(home) / ".local" / "share" / "applications");
	}

	const char* dataDirectories = std::getenv("XDG_DATA_DIRS");
	const std::string searchPath = dataDirectories != nullptr && *dataDirectories != '\0' ?
		std::string(dataDirectories) : "/usr/local/share:/usr/share";
	std::size_t begin = 0;
	while (begin <= searchPath.size()) {
		const std::size_t end = searchPath.find(':', begin);
		const fs::path directory = searchPath.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		if (!directory.empty()) addDirectory(directory / "applications");
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return directories;
}

fs::path AbsoluteNormalized(const fs::path& path) {
	std::error_code error;
	const fs::path absolute = fs::absolute(path, error);
	return (error ? path : absolute).lexically_normal();
}

std::string BuildFileUri(const fs::path& filename) {
	const std::string path = AbsoluteNormalized(filename).string();
	std::ostringstream uri;
	uri << "file://";
	for (const unsigned char character : path) {
		if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == '-' || character == '_' ||
			character == '.' || character == '/' || character == '~') {
			uri << static_cast<char>(character);
		} else {
			static constexpr char hex[] = "0123456789ABCDEF";
			uri << '%' << hex[character >> 4] << hex[character & 15];
		}
	}
	return uri.str();
}

bool ExpandDesktopExecToken(const std::string& token, const OpenWithApplication& application,
	const fs::path& imageFilename, std::string& expanded, bool& hasFilePlaceholder) {
	const std::string imagePath = AbsoluteNormalized(imageFilename).string();
	const std::string imageUri = BuildFileUri(imageFilename);
	const std::string imageDirectory = AbsoluteNormalized(imageFilename.parent_path()).string();
	const std::string imageName = imageFilename.filename().string();
	expanded.clear();
	for (std::size_t index = 0; index < token.size(); ++index) {
		if (token[index] != '%' || index + 1 >= token.size()) {
			expanded += token[index];
			continue;
		}
		const char field = token[++index];
		switch (field) {
		case '%': expanded += '%'; break;
		case 'f':
		case 'F':
			expanded += imagePath;
			hasFilePlaceholder = true;
			break;
		case 'u':
		case 'U':
			expanded += imageUri;
			hasFilePlaceholder = true;
			break;
		case 'd':
		case 'D': expanded += imageDirectory; break;
		case 'n':
		case 'N': expanded += imageName; break;
		case 'c': expanded += application.name; break;
		case 'k': expanded += application.desktopFile.string(); break;
		case 'i':
		case 'm':
			// The application's icon is not needed to launch it.
			return false;
		default:
			return false;
		}
	}
	return true;
}

} // namespace

std::string FileUri(const fs::path& filename) {
	return BuildFileUri(filename);
}

std::string UnescapeDesktopValue(const std::string& value) {
	std::string result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (value[index] != '\\' || index + 1 >= value.size()) {
			result += value[index];
			continue;
		}
		const char escaped = value[++index];
		switch (escaped) {
		case 'n': result += '\n'; break;
		case 'r': result += '\r'; break;
		case 's': result += ' '; break;
		case 't': result += '\t'; break;
		case '\\': result += '\\'; break;
		default: result += escaped; break;
		}
	}
	return result;
}

bool MimeTypeMatches(const std::string& mimeTypes, const std::string& currentMime) {
	if (mimeTypes.empty() || currentMime.empty()) return false;
	std::size_t begin = 0;
	while (begin <= mimeTypes.size()) {
		const std::size_t end = mimeTypes.find(';', begin);
		const std::string mime = Lower(TrimDesktopValue(mimeTypes.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin)));
		const bool compatibleAlias =
			(mime == "image/x-ms-bmp" && currentMime == "image/bmp") ||
			(mime == "image/bmp" && currentMime == "image/x-ms-bmp") ||
			(mime == "image/targa" && currentMime == "image/x-tga") ||
			(mime == "image/x-tga" && currentMime == "image/targa");
		if (mime == currentMime || compatibleAlias || mime == "*/*" ||
			(mime.size() > 2 && mime.back() == '*' && mime[mime.size() - 2] == '/' &&
			currentMime.compare(0, mime.size() - 1, mime, 0, mime.size() - 1) == 0)) {
			return true;
		}
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return false;
}

std::string MimeTypeForExtension(const std::string& extension) {
	if (extension == ".jpg" || extension == ".jpeg" || extension == ".jpe") return "image/jpeg";
	if (extension == ".png") return "image/png";
	if (extension == ".gif") return "image/gif";
	if (extension == ".bmp") return "image/bmp";
	if (extension == ".tga") return "image/x-tga";
	if (extension == ".psd") return "image/vnd.adobe.photoshop";
	if (extension == ".ppm") return "image/x-portable-pixmap";
	if (extension == ".pgm") return "image/x-portable-graymap";
	if (extension == ".pnm") return "image/x-portable-anymap";
	if (extension == ".webp") return "image/webp";
	return {};
}

bool ReadDesktopApplication(const fs::path& filename, const std::string& currentMime,
	OpenWithApplication& application) {
	std::ifstream input(filename);
	if (!input) return false;

	bool inDesktopEntry = false;
	std::string type;
	std::string name;
	std::string exec;
	std::string tryExec;
	std::string mimeTypes;
	bool hidden = false;
	bool noDisplay = false;
	bool terminal = false;
	std::string line;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line == "[Desktop Entry]") {
			inDesktopEntry = true;
			continue;
		}
		if (!line.empty() && line.front() == '[') {
			inDesktopEntry = false;
			continue;
		}
		if (!inDesktopEntry || line.empty() || line.front() == '#') continue;
		if (line.compare(0, 5, "Type=") == 0) type = DesktopEntryField(line, "Type");
		else if (line.compare(0, 5, "Name=") == 0) name = DesktopEntryField(line, "Name");
		else if (line.compare(0, 5, "Exec=") == 0) exec = DesktopEntryField(line, "Exec");
		else if (line.compare(0, 8, "TryExec=") == 0) tryExec = DesktopEntryField(line, "TryExec");
		else if (line.compare(0, 9, "MimeType=") == 0) mimeTypes = DesktopEntryField(line, "MimeType");
		else if (line.compare(0, 7, "Hidden=") == 0) hidden = DesktopBoolean(DesktopEntryField(line, "Hidden"));
		else if (line.compare(0, 9, "NoDisplay=") == 0) noDisplay = DesktopBoolean(DesktopEntryField(line, "NoDisplay"));
		else if (line.compare(0, 9, "Terminal=") == 0) terminal = DesktopBoolean(DesktopEntryField(line, "Terminal"));
	}

	if (type != "Application" || name.empty() || exec.empty() || hidden || noDisplay ||
		!MimeTypeMatches(mimeTypes, currentMime)) return false;
	if (!tryExec.empty() && !HasExecutable(tryExec)) return false;
	application = OpenWithApplication{name, exec, filename, terminal};
	return true;
}

std::vector<OpenWithApplication> DiscoverOpenWithApplications(const std::string& extension) {
	std::vector<OpenWithApplication> applications;
	const std::string currentMime = MimeTypeForExtension(extension);
	std::set<std::string> seenDesktopIds;
	for (const fs::path& directory : DesktopApplicationDirectories()) {
		std::error_code iteratorError;
		for (const fs::directory_entry& entry : fs::directory_iterator(directory, iteratorError)) {
			if (iteratorError) break;
			std::error_code fileError;
			if (!entry.is_regular_file(fileError) || fileError ||
				Lower(entry.path().extension().string()) != ".desktop") continue;
			const std::string desktopId = entry.path().filename().string();
			if (!seenDesktopIds.insert(desktopId).second) continue;
			OpenWithApplication application;
			if (ReadDesktopApplication(entry.path(), currentMime, application)) {
				applications.push_back(std::move(application));
			}
		}
	}

	std::sort(applications.begin(), applications.end(), [](const OpenWithApplication& left,
		const OpenWithApplication& right) {
		const std::string leftName = Lower(left.name);
		const std::string rightName = Lower(right.name);
		if (leftName != rightName) return leftName < rightName;
		return left.desktopFile.string() < right.desktopFile.string();
	});
	if (applications.size() > static_cast<std::size_t>(IDM_LAST_OPENWITH_CMD - IDM_FIRST_OPENWITH_CMD + 1)) {
		applications.resize(static_cast<std::size_t>(IDM_LAST_OPENWITH_CMD - IDM_FIRST_OPENWITH_CMD + 1));
	}
	return applications;
}

std::vector<std::string> TokenizeDesktopExec(const std::string& commandLine) {
	std::vector<std::string> tokens;
	std::string token;
	char quote = '\0';
	bool escaped = false;
	for (const char character : commandLine) {
		if (escaped) {
			token += character;
			escaped = false;
			continue;
		}
		if (quote != '\0') {
			if (character == quote) quote = '\0';
			else if (character == '\\') escaped = true;
			else token += character;
			continue;
		}
		if (character == '\'' || character == '"') quote = character;
		else if (character == '\\') escaped = true;
		else if (std::isspace(static_cast<unsigned char>(character))) {
			if (!token.empty()) {
				tokens.push_back(std::move(token));
				token.clear();
			}
		} else token += character;
	}
	if (escaped) token += '\\';
	if (quote != '\0' || !token.empty()) {
		if (!token.empty()) tokens.push_back(std::move(token));
	}
	return tokens;
}

std::vector<std::string> DesktopExecArguments(const OpenWithApplication& application,
	const fs::path& imageFilename) {
	const std::vector<std::string> tokens = TokenizeDesktopExec(application.exec);
	if (tokens.empty()) return {};

	std::vector<std::string> arguments;
	arguments.reserve(tokens.size() + 1);
	bool hasFilePlaceholder = false;
	for (const std::string& token : tokens) {
		std::string expanded;
		if (!ExpandDesktopExecToken(token, application, imageFilename, expanded, hasFilePlaceholder)) continue;
		if (!expanded.empty()) arguments.push_back(std::move(expanded));
	}
	if (arguments.empty()) return {};
	if (!hasFilePlaceholder) arguments.push_back(AbsoluteNormalized(imageFilename).string());
	return arguments;
}

} // namespace jpegview_linux
