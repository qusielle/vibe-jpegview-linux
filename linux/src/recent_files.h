#pragma once

#include "viewport.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace jpegview_linux {

inline constexpr std::size_t kMaximumRecentFolders = 100;
inline constexpr std::size_t kMaximumRecentViewportSnapshots = 256;

// Keeps one most-recent image per normalized absolute parent directory and a
// separately bounded MRU set of per-image viewing snapshots.
class RecentFiles {
public:
	void Add(const std::filesystem::path& filename);
	void RememberViewport(const std::filesystem::path& filename,
		const ViewportSnapshot& snapshot);
	std::optional<ViewportSnapshot> FindViewport(
		const std::filesystem::path& filename) const;

	const std::vector<std::filesystem::path>& Files() const { return files_; }
	std::size_t ViewportSnapshotCount() const { return viewportLru_.size(); }
	void Clear();

private:
	friend bool LoadRecentFiles(const std::filesystem::path& filename, RecentFiles& recentFiles);
	friend bool SaveRecentFiles(const std::filesystem::path& filename, const RecentFiles& recentFiles);

	std::vector<std::filesystem::path> files_;
	std::vector<std::string> viewportLru_;
	std::unordered_map<std::string, ViewportSnapshot> viewports_;
};

std::filesystem::path RecentFilesDatabasePath();
bool LoadRecentFiles(const std::filesystem::path& filename, RecentFiles& recentFiles);
bool SaveRecentFiles(const std::filesystem::path& filename, const RecentFiles& recentFiles);

} // namespace jpegview_linux
