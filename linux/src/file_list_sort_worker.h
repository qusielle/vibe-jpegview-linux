#pragma once

#include "file_list.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jpegview_linux {

struct FileListSortResult {
	std::uint64_t generation = 0;
	FileListPreparedSort prepared;
	std::string error;
};

// Sorts an immutable active-catalog snapshot without disturbing navigation.
// New requests supersede pending work; accepted results still require the
// FileList revision checks performed by ApplyPreparedSort.
class FileListSortWorker {
public:
	FileListSortWorker();
	~FileListSortWorker();
	FileListSortWorker(const FileListSortWorker&) = delete;
	FileListSortWorker& operator=(const FileListSortWorker&) = delete;

	void Stop();
	std::uint64_t Request(FileListSortRequest request);
	void Clear();
	std::vector<FileListSortResult> TakeReady();
	void Retire(FileListPreparedSort&& prepared);
	void Retire(std::shared_ptr<const void> storage);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
