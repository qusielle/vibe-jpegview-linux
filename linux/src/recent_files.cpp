#include "recent_files.h"

#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr std::size_t kMaximumEncodedPathLength = 128u * 1024u;

fs::path NormalizeAbsolute(const fs::path& path) {
	if (path.empty()) return {};
	std::error_code error;
	const fs::path absolute = fs::absolute(path, error);
	return error ? fs::path() : absolute.lexically_normal();
}

bool NormalizeSnapshot(const ViewportSnapshot& input, ViewportSnapshot& output) {
	if (!std::isfinite(input.zoom) || !std::isfinite(input.relativeZoom) || input.relativeZoom <= 0.0) {
		return false;
	}
	output = input;
	output.zoom = std::clamp(input.zoom, kMinimumZoom, kMaximumZoom);
	output.relativeZoom = std::clamp(input.relativeZoom,
		kMinimumZoom / kMaximumZoom, kMaximumZoom / kMinimumZoom);
	return true;
}

std::string EncodePath(const fs::path& path) {
	const std::string bytes = path.string();
	std::string encoded;
	encoded.reserve(bytes.size() * 2);
	for (const unsigned char byte : bytes) {
		encoded.push_back(kHexDigits[byte >> 4]);
		encoded.push_back(kHexDigits[byte & 0x0f]);
	}
	return encoded;
}

int HexValue(char character) {
	if (character >= '0' && character <= '9') return character - '0';
	if (character >= 'a' && character <= 'f') return character - 'a' + 10;
	if (character >= 'A' && character <= 'F') return character - 'A' + 10;
	return -1;
}

bool DecodePath(const std::string& encoded, fs::path& path) {
	if (encoded.empty() || encoded.size() > kMaximumEncodedPathLength ||
		(encoded.size() % 2) != 0) return false;
	std::string bytes;
	bytes.reserve(encoded.size() / 2);
	for (std::size_t index = 0; index < encoded.size(); index += 2) {
		const int high = HexValue(encoded[index]);
		const int low = HexValue(encoded[index + 1]);
		if (high < 0 || low < 0 || (high == 0 && low == 0)) return false;
		bytes.push_back(static_cast<char>((high << 4) | low));
	}
	path = fs::path(bytes);
	return path.is_absolute();
}

bool ParseBoolean(int value) {
	return value == 0 || value == 1;
}

bool ParseRecentRow(const std::string& line, fs::path& path) {
	std::istringstream row(line);
	char record = 0;
	std::string encoded;
	std::string extra;
	if (!(row >> record >> encoded) || record != 'R' || (row >> extra)) return false;
	return DecodePath(encoded, path);
}

bool ParseViewportRow(const std::string& line, fs::path& path,
	ViewportSnapshot& snapshot) {
	std::istringstream row(line);
	char record = 0;
	std::string encoded;
	int fit = 0;
	int fill = 0;
	int noEnlarge = 0;
	double zoom = 0.0;
	double relativeZoom = 1.0;
	std::string relativeZoomToken;
	std::string extra;
	if (!(row >> record >> encoded >> fit >> fill >> noEnlarge >> zoom) || record != 'V' ||
		!ParseBoolean(fit) || !ParseBoolean(fill) ||
		!ParseBoolean(noEnlarge) || !std::isfinite(zoom)) return false;
	if (row >> relativeZoomToken) {
		try {
			std::size_t parsedCharacters = 0;
			relativeZoom = std::stod(relativeZoomToken, &parsedCharacters);
			if (parsedCharacters != relativeZoomToken.size() || !std::isfinite(relativeZoom) ||
				relativeZoom <= 0.0 || (row >> extra)) return false;
		} catch (const std::exception&) {
			return false;
		}
	} else if (!row.eof()) {
		return false;
	}
	if (!DecodePath(encoded, path)) return false;
	snapshot.fitToWindow = fit != 0;
	snapshot.fillWithCrop = fill != 0;
	snapshot.noEnlarge = noEnlarge != 0;
	snapshot.zoom = std::clamp(zoom, kMinimumZoom, kMaximumZoom);
	snapshot.relativeZoom = std::clamp(relativeZoom,
		kMinimumZoom / kMaximumZoom, kMaximumZoom / kMinimumZoom);
	return true;
}

bool ParseDisplayModeRow(const std::string& line, fs::path& path,
	DoublePageModeState& modes) {
	std::istringstream row(line);
	char record = 0;
	std::string encoded;
	int enabled = 0;
	int mangaReadingOrder = 0;
	std::string extra;
	if (!(row >> record >> encoded >> enabled >> mangaReadingOrder) || record != 'D' ||
		(row >> extra) || !ParseBoolean(enabled) || !ParseBoolean(mangaReadingOrder)) return false;
	if (!DecodePath(encoded, path)) return false;
	modes.enabled = enabled != 0;
	modes.mangaReadingOrder = mangaReadingOrder != 0;
	return true;
}

} // namespace

void RecentFiles::Add(const fs::path& filename) {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty()) return;
	const fs::path folder = path.parent_path();
	files_.erase(std::remove_if(files_.begin(), files_.end(), [&folder](const fs::path& existing) {
		return existing.parent_path() == folder;
	}), files_.end());
	files_.insert(files_.begin(), path);
	if (files_.size() > kMaximumRecentFolders) files_.resize(kMaximumRecentFolders);
}

std::optional<RecentFileRemoval> RecentFiles::Remove(const fs::path& filename) {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty()) return std::nullopt;
	const auto found = std::find(files_.begin(), files_.end(), path);
	if (found == files_.end()) return std::nullopt;
	const std::size_t index = static_cast<std::size_t>(std::distance(files_.begin(), found));
	files_.erase(found);
	return RecentFileRemoval{path, index};
}

bool RecentFiles::Restore(const RecentFileRemoval& removal) {
	const fs::path path = NormalizeAbsolute(removal.path);
	if (path.empty()) return false;
	const fs::path folder = path.parent_path();
	files_.erase(std::remove_if(files_.begin(), files_.end(), [&folder](const fs::path& existing) {
		return existing.parent_path() == folder;
	}), files_.end());
	const std::size_t index = std::min(removal.index, files_.size());
	files_.insert(files_.begin() + static_cast<std::ptrdiff_t>(index), path);
	if (files_.size() > kMaximumRecentFolders) files_.resize(kMaximumRecentFolders);
	return true;
}

void RecentFiles::RememberViewport(const fs::path& filename,
	const ViewportSnapshot& snapshot) {
	const fs::path path = NormalizeAbsolute(filename);
	ViewportSnapshot normalized;
	if (path.empty() || !NormalizeSnapshot(snapshot, normalized)) return;
	const std::string key = path.string();
	const auto existing = std::find(viewportLru_.begin(), viewportLru_.end(), key);
	if (existing != viewportLru_.end()) viewportLru_.erase(existing);
	viewportLru_.insert(viewportLru_.begin(), key);
	viewports_[key] = normalized;
	if (viewportLru_.size() > kMaximumRecentViewportSnapshots) {
		viewports_.erase(viewportLru_.back());
		viewportLru_.pop_back();
	}
}

std::optional<ViewportSnapshot> RecentFiles::FindViewport(const fs::path& filename) const {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty()) return std::nullopt;
	const auto found = viewports_.find(path.string());
	return found == viewports_.end() ? std::nullopt :
		std::optional<ViewportSnapshot>(found->second);
}

void RecentFiles::RememberDoublePageMode(const fs::path& filename,
	const DoublePageModeState& modes) {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty()) return;
	const std::string key = path.string();
	const auto existing = std::find(modeLru_.begin(), modeLru_.end(), key);
	if (existing != modeLru_.end()) modeLru_.erase(existing);
	modeLru_.insert(modeLru_.begin(), key);
	displayModes_[key] = modes;
	if (modeLru_.size() > kMaximumRecentDisplayModeSnapshots) {
		displayModes_.erase(modeLru_.back());
		modeLru_.pop_back();
	}
}

std::optional<DoublePageModeState> RecentFiles::FindDoublePageMode(
	const fs::path& filename) const {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty()) return std::nullopt;
	const auto found = displayModes_.find(path.string());
	return found == displayModes_.end() ? std::nullopt :
		std::optional<DoublePageModeState>(found->second);
}

void RecentFiles::Clear() {
	files_.clear();
	viewportLru_.clear();
	viewports_.clear();
	modeLru_.clear();
	displayModes_.clear();
}

void RecentImageLoadState::SaveCurrentBeforeLoad(const fs::path& target,
	const ViewportSnapshot& currentViewport, const DoublePageModeState& currentModes,
	RecentFiles& recentFiles) {
	const fs::path normalizedTarget = NormalizeAbsolute(target);
	if (loadedPath_.empty() || normalizedTarget.empty() || normalizedTarget == loadedPath_ ||
		loadedOwnerSnapshotSaved_) return;
	recentFiles.RememberViewport(loadedPath_, currentViewport);
	recentFiles.RememberDoublePageMode(loadedPath_, currentModes);
	loadedOwnerSnapshotSaved_ = true;
}

bool RecentImageLoadState::OwnsLoadedPath(const fs::path& selectedFilename) const {
	const fs::path selectedPath = NormalizeAbsolute(selectedFilename);
	return !selectedPath.empty() && !loadedPath_.empty() && selectedPath == loadedPath_ &&
		(!pendingLoad_.has_value() || pendingLoad_->filename == loadedPath_);
}

ViewportSnapshot RecentImageLoadState::ViewportForSelection(
	const fs::path& selectedFilename, const ViewportSnapshot& currentViewport,
	const ViewportSnapshot& navigationViewport, const RecentFiles& recentFiles) const {
	const fs::path selectedPath = NormalizeAbsolute(selectedFilename);
	if (selectedPath.empty()) return navigationViewport;
	if (pendingLoad_.has_value() && pendingLoad_->filename == selectedPath) {
		return pendingLoad_->viewportSnapshot;
	}
	if (!pendingLoad_.has_value() && selectedPath == loadedPath_) {
		return currentViewport;
	}
	const std::optional<ViewportSnapshot> savedViewport =
		recentFiles.FindViewport(selectedPath);
	return savedViewport.has_value() ? *savedViewport : navigationViewport;
}

void RecentImageLoadState::BeginLoad(const fs::path& filename,
	const ViewportSnapshot& viewportSnapshot) {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty()) {
		pendingLoad_.reset();
		return;
	}
	pendingLoad_ = PendingRecentImageLoad{path, viewportSnapshot,
		viewportSnapshot};
}

bool RecentImageLoadState::UpdatePendingViewport(const fs::path& filename,
	const ViewportSnapshot& viewportSnapshot) {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty() || !pendingLoad_.has_value() || pendingLoad_->filename != path) return false;
	ViewportSnapshot normalized;
	if (!NormalizeSnapshot(viewportSnapshot, normalized)) return false;
	pendingLoad_->viewportSnapshot = normalized;
	return true;
}

std::optional<PendingRecentImageLoad> RecentImageLoadState::TakePendingLoad(
	const fs::path& filename) {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty() || !pendingLoad_.has_value() || pendingLoad_->filename != path) {
		return std::nullopt;
	}
	std::optional<PendingRecentImageLoad> result(std::move(pendingLoad_));
	pendingLoad_.reset();
	return result;
}

bool RecentImageLoadState::CommitLoad(const fs::path& filename, RecentFiles& recentFiles) {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty() || !pendingLoad_.has_value() || pendingLoad_->filename != path) return false;
	recentFiles.Add(path);
	loadedPath_ = path;
	loadedOwnerSnapshotSaved_ = false;
	pendingLoad_.reset();
	return true;
}

bool RecentImageLoadState::FailLoad(const fs::path& filename) {
	const fs::path path = NormalizeAbsolute(filename);
	if (path.empty() || !pendingLoad_.has_value() || pendingLoad_->filename != path) return false;
	loadedPath_.clear();
	pendingLoad_.reset();
	return true;
}

void RecentImageLoadState::CancelPendingLoad() {
	pendingLoad_.reset();
}

fs::path RecentFilesDatabasePath() {
	if (const char* stateHome = std::getenv("XDG_STATE_HOME");
		stateHome != nullptr && stateHome[0] == '/') {
		return fs::path(stateHome) / "jpegview-linux" / "recent-files.db";
	}
	if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
		return fs::path(home) / ".local" / "state" / "jpegview-linux" / "recent-files.db";
	}
	return {};
}

bool LoadRecentFiles(const fs::path& filename, RecentFiles& recentFiles) {
	std::ifstream input(filename);
	if (!input) {
		std::error_code error;
		const bool exists = fs::exists(filename, error);
		if (!error && !exists) {
			recentFiles.Clear();
			return true;
		}
		return false;
	}
	RecentFiles loaded;
	std::string line;
	while (std::getline(input, line)) {
		if (line.empty() || line[0] == '#') continue;
		fs::path path;
		if (line[0] == 'R') {
			if (!ParseRecentRow(line, path)) continue;
			path = NormalizeAbsolute(path);
			if (path.empty()) continue;
			const fs::path folder = path.parent_path();
			const bool duplicateFolder = std::any_of(loaded.files_.begin(), loaded.files_.end(),
				[&folder](const fs::path& existing) { return existing.parent_path() == folder; });
			if (!duplicateFolder && loaded.files_.size() < kMaximumRecentFolders) {
				loaded.files_.push_back(path);
			}
		} else if (line[0] == 'V') {
			ViewportSnapshot snapshot;
			if (!ParseViewportRow(line, path, snapshot)) continue;
			path = NormalizeAbsolute(path);
			if (path.empty()) continue;
			const std::string key = path.string();
			if (loaded.viewports_.find(key) != loaded.viewports_.end()) continue;
			if (loaded.viewportLru_.size() >= kMaximumRecentViewportSnapshots) continue;
			loaded.viewportLru_.push_back(key);
			loaded.viewports_.emplace(key, snapshot);
		} else if (line[0] == 'D') {
			DoublePageModeState modes;
			if (!ParseDisplayModeRow(line, path, modes)) continue;
			path = NormalizeAbsolute(path);
			if (path.empty()) continue;
			const std::string key = path.string();
			if (loaded.displayModes_.find(key) != loaded.displayModes_.end()) continue;
			if (loaded.modeLru_.size() >= kMaximumRecentDisplayModeSnapshots) continue;
			loaded.modeLru_.push_back(key);
			loaded.displayModes_.emplace(key, modes);
		}
	}
	if (!input.eof()) return false;
	recentFiles = std::move(loaded);
	return true;
}

bool SaveRecentFiles(const fs::path& filename, const RecentFiles& recentFiles) {
	if (filename.empty()) return false;
	std::error_code error;
	if (!filename.parent_path().empty()) {
		fs::create_directories(filename.parent_path(), error);
		if (error) return false;
	}
	fs::path temporary = filename;
	temporary += ".tmp." + std::to_string(static_cast<long long>(getpid()));
	{
		std::ofstream output(temporary, std::ios::trunc);
		if (!output) return false;
		output << "# JPEGView Linux recent files, version 3\n";
		for (const fs::path& path : recentFiles.files_) {
			output << "R " << EncodePath(path) << '\n';
		}
		output << std::setprecision(17);
		for (const std::string& key : recentFiles.viewportLru_) {
			const auto found = recentFiles.viewports_.find(key);
			if (found == recentFiles.viewports_.end()) continue;
			const ViewportSnapshot& snapshot = found->second;
			output << "V " << EncodePath(fs::path(key)) << ' '
				<< (snapshot.fitToWindow ? 1 : 0) << ' '
				<< (snapshot.fillWithCrop ? 1 : 0) << ' '
				<< (snapshot.noEnlarge ? 1 : 0) << ' ' << snapshot.zoom << ' '
				<< snapshot.relativeZoom << '\n';
		}
		for (const std::string& key : recentFiles.modeLru_) {
			const auto found = recentFiles.displayModes_.find(key);
			if (found == recentFiles.displayModes_.end()) continue;
			const DoublePageModeState& modes = found->second;
			output << "D " << EncodePath(fs::path(key)) << ' '
				<< (modes.enabled ? 1 : 0) << ' '
				<< (modes.mangaReadingOrder ? 1 : 0) << '\n';
		}
		if (!output) {
			output.close();
			fs::remove(temporary, error);
			return false;
		}
	}
	error.clear();
	fs::rename(temporary, filename, error);
	if (error) {
		fs::remove(temporary, error);
		return false;
	}
	return true;
}

} // namespace jpegview_linux
