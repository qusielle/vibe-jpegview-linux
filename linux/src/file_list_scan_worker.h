#pragma once

#include "file_list.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jpegview_linux {

struct FileListScanResult {
	std::uint64_t generation = 0;
	FileListPreparedScan prepared;
	std::string error;
};

// One lazy worker handles viewer-list scans. New requests supersede pending and
// active scans; filesystem and archive enumeration check that generation while
// walking, and only the newest complete result can reach Viewer.
class FileListScanWorker {
public:
	FileListScanWorker();
	~FileListScanWorker();
	FileListScanWorker(const FileListScanWorker&) = delete;
	FileListScanWorker& operator=(const FileListScanWorker&) = delete;

	std::uint64_t Request(FileList::ScanRequest request);
	void Clear();
	std::vector<FileListScanResult> TakeReady();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
