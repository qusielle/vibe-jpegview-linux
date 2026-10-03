#pragma once

#include "double_page_model.h"
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
inline constexpr std::size_t kMaximumRecentDisplayModeSnapshots = 256;

struct RecentFileRemoval {
	std::filesystem::path path;
	std::size_t index = 0;
};

// Keeps one most-recent image per normalized absolute parent directory and a
// separately bounded MRU set of per-image viewing snapshots.
class RecentFiles {
public:
	void Add(const std::filesystem::path& filename);
	std::optional<RecentFileRemoval> Remove(const std::filesystem::path& filename);
	bool Restore(const RecentFileRemoval& removal);
	void RememberViewport(const std::filesystem::path& filename,
		const ViewportSnapshot& snapshot);
	std::optional<ViewportSnapshot> FindViewport(
		const std::filesystem::path& filename) const;
	void RememberDoublePageMode(const std::filesystem::path& filename,
		const DoublePageModeState& modes);
	std::optional<DoublePageModeState> FindDoublePageMode(
		const std::filesystem::path& filename) const;

	const std::vector<std::filesystem::path>& Files() const { return files_; }
	std::size_t ViewportSnapshotCount() const { return viewportLru_.size(); }
	std::size_t DisplayModeSnapshotCount() const { return modeLru_.size(); }
	void Clear();

private:
	friend bool LoadRecentFiles(const std::filesystem::path& filename, RecentFiles& recentFiles);
	friend bool SaveRecentFiles(const std::filesystem::path& filename, const RecentFiles& recentFiles);

	std::vector<std::filesystem::path> files_;
	std::vector<std::string> viewportLru_;
	std::unordered_map<std::string, ViewportSnapshot> viewports_;
	std::vector<std::string> modeLru_;
	std::unordered_map<std::string, DoublePageModeState> displayModes_;
};

struct PendingRecentImageLoad {
	std::filesystem::path filename;
	ViewportSnapshot viewportSnapshot;
	ViewportSnapshot intentBaseSnapshot;
};

// Separates a selected image awaiting completion from the last image whose
// history was committed after a successful load.
class RecentImageLoadState {
public:
	const std::filesystem::path& LoadedPath() const { return loadedPath_; }
	bool OwnsLoadedPath(const std::filesystem::path& selectedFilename) const;
	ViewportSnapshot ViewportForSelection(const std::filesystem::path& selectedFilename,
		const ViewportSnapshot& currentViewport,
		const ViewportSnapshot& navigationViewport, const RecentFiles& recentFiles,
		bool restoreRecentViewport = false) const;

	void SaveCurrentBeforeLoad(const std::filesystem::path& target,
		const ViewportSnapshot& currentViewport, const DoublePageModeState& currentModes,
		RecentFiles& recentFiles);
	void BeginLoad(const std::filesystem::path& filename,
		const ViewportSnapshot& viewportSnapshot);
	bool UpdatePendingViewport(const std::filesystem::path& filename,
		const ViewportSnapshot& viewportSnapshot);
	std::optional<PendingRecentImageLoad> TakePendingLoad(
		const std::filesystem::path& filename);
	bool CommitLoad(const std::filesystem::path& filename, RecentFiles& recentFiles);
	bool FailLoad(const std::filesystem::path& filename);
	void CancelPendingLoad();

private:
	std::filesystem::path loadedPath_;
	bool loadedOwnerSnapshotSaved_ = false;
	std::optional<PendingRecentImageLoad> pendingLoad_;
};

std::filesystem::path RecentFilesDatabasePath();
bool LoadRecentFiles(const std::filesystem::path& filename, RecentFiles& recentFiles);
bool SaveRecentFiles(const std::filesystem::path& filename, const RecentFiles& recentFiles);

} // namespace jpegview_linux
