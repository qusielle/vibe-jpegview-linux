#pragma once

#include "desktop_applications.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jpegview_linux {

struct ExternalCommand {
	std::string executable;
	std::vector<std::string> arguments;

	bool Valid() const { return !executable.empty(); }
};

struct ExternalCommandSequence {
	ExternalCommand required;
	std::vector<ExternalCommand> afterSuccess;
};

enum class LosslessJpegOperation {
	Rotate90,
	Rotate180,
	Rotate270,
	FlipHorizontal,
	FlipVertical,
};

ExternalCommand LosslessJpegCommand(LosslessJpegOperation operation,
	const std::filesystem::path& source, const std::filesystem::path& output);
ExternalCommand LosslessJpegCropCommand(const std::filesystem::path& source,
	const std::filesystem::path& output, int x, int y, int width, int height);
ExternalCommand PrintCommand(const std::filesystem::path& image);
std::vector<ExternalCommand> OpenContainingFolderCommands(const std::filesystem::path& directory);
std::optional<ExternalCommand> OpenWithCommand(const OpenWithApplication& application,
	const std::filesystem::path& image, bool terminalAvailable);
std::vector<ExternalCommandSequence> WallpaperCommandSequences(const std::filesystem::path& image);
std::vector<ExternalCommand> TrashCommands(const std::filesystem::path& image);

enum class ClipboardBackend {
	None,
	Wayland,
	Xclip,
};

ClipboardBackend SelectClipboardBackend(bool waylandSession,
	bool waylandHelperAvailable, bool xclipAvailable);
ExternalCommand ClipboardWriteCommand(ClipboardBackend backend);
ExternalCommand ClipboardReadCommand(ClipboardBackend backend);

} // namespace jpegview_linux
