#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace jpegview_linux {

class ArchivePasswordDialogModel {
public:
	void Begin(std::string archivePath, std::string errorMessage = {});
	void Cancel();

	bool AppendText(std::string_view utf8Text);
	bool Backspace();
	std::optional<std::string> Submit();

	bool IsOpen() const { return open_; }
	const std::string& ArchivePath() const { return archivePath_; }
	const std::string& Error() const { return errorMessage_; }
	std::string DisplayText() const;

private:
	void Reset();

	bool open_ = false;
	std::string archivePath_;
	std::string password_;
	std::string errorMessage_;
};

} // namespace jpegview_linux
