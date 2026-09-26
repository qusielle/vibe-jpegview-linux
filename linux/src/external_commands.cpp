#include "external_commands.h"

#include <utility>

namespace jpegview_linux {

ExternalCommand LosslessJpegCommand(LosslessJpegOperation operation,
	const std::filesystem::path& source, const std::filesystem::path& output) {
	ExternalCommand command{"jpegtran", {"-copy", "all"}};
	switch (operation) {
	case LosslessJpegOperation::Rotate90:
		command.arguments.insert(command.arguments.end(), {"-rotate", "90"});
		break;
	case LosslessJpegOperation::Rotate180:
		command.arguments.insert(command.arguments.end(), {"-rotate", "180"});
		break;
	case LosslessJpegOperation::Rotate270:
		command.arguments.insert(command.arguments.end(), {"-rotate", "270"});
		break;
	case LosslessJpegOperation::FlipHorizontal:
		command.arguments.insert(command.arguments.end(), {"-flip", "horizontal"});
		break;
	case LosslessJpegOperation::FlipVertical:
		command.arguments.insert(command.arguments.end(), {"-flip", "vertical"});
		break;
	}
	command.arguments.insert(command.arguments.end(),
		{"-outfile", output.string(), source.string()});
	return command;
}

ExternalCommand LosslessJpegCropCommand(const std::filesystem::path& source,
	const std::filesystem::path& output, int x, int y, int width, int height) {
	if (x < 0 || y < 0 || width <= 0 || height <= 0 || source.empty() || output.empty()) return {};
	const std::string rectangle = std::to_string(width) + "x" + std::to_string(height) +
		"+" + std::to_string(x) + "+" + std::to_string(y);
	return {"jpegtran", {"-copy", "all", "-crop", rectangle,
		"-outfile", output.string(), source.string()}};
}

ExternalCommand PrintCommand(const std::filesystem::path& image) {
	return {"lp", {image.string()}};
}

std::vector<ExternalCommand> OpenUrlCommands(const std::string& url) {
	if (url.empty()) return {};
	return {{"xdg-open", {url}}, {"gio", {"open", url}}};
}

std::vector<ExternalCommand> OpenContainingFolderCommands(
	const std::filesystem::path& directory) {
	return {{"xdg-open", {directory.string()}}, {"gio", {"open", directory.string()}}};
}

std::optional<ExternalCommand> OpenWithCommand(const OpenWithApplication& application,
	const std::filesystem::path& image, bool terminalAvailable) {
	std::vector<std::string> expanded = DesktopExecArguments(application, image);
	if (expanded.empty()) return std::nullopt;
	if (application.terminal && terminalAvailable) {
		std::vector<std::string> arguments{"-e"};
		arguments.insert(arguments.end(), expanded.begin(), expanded.end());
		return ExternalCommand{"x-terminal-emulator", std::move(arguments)};
	}
	ExternalCommand command;
	command.executable = std::move(expanded.front());
	expanded.erase(expanded.begin());
	command.arguments = std::move(expanded);
	return command;
}

std::vector<ExternalCommandSequence> WallpaperCommandSequences(
	const std::filesystem::path& image) {
	const std::string uri = FileUri(image);
	return {
		{{"gsettings", {"set", "org.gnome.desktop.background", "picture-uri", uri}},
			{{"gsettings", {"set", "org.gnome.desktop.background", "picture-uri-dark", uri}}}},
		{{"feh", {"--bg-fill", image.string()}}, {}},
		{{"nitrogen", {"--set-zoom-fill", image.string()}}, {}},
	};
}

std::vector<ExternalCommand> TrashCommands(const std::filesystem::path& image) {
	return {{"gio", {"trash", image.string()}}, {"trash-put", {image.string()}}};
}

ClipboardBackend SelectClipboardBackend(bool waylandSession,
	bool waylandHelperAvailable, bool xclipAvailable) {
	if (waylandSession && waylandHelperAvailable) return ClipboardBackend::Wayland;
	if (!waylandSession && xclipAvailable) return ClipboardBackend::Xclip;
	if (waylandHelperAvailable) return ClipboardBackend::Wayland;
	if (xclipAvailable) return ClipboardBackend::Xclip;
	return ClipboardBackend::None;
}

ExternalCommand ClipboardWriteCommand(ClipboardBackend backend) {
	if (backend == ClipboardBackend::Wayland) return {"wl-copy", {"--type", "image/png"}};
	if (backend == ClipboardBackend::Xclip) {
		return {"xclip", {"-selection", "clipboard", "-t", "image/png", "-i"}};
	}
	return {};
}

ExternalCommand ClipboardReadCommand(ClipboardBackend backend) {
	if (backend == ClipboardBackend::Wayland) {
		return {"wl-paste", {"--type", "image/png", "--no-newline"}};
	}
	if (backend == ClipboardBackend::Xclip) {
		return {"xclip", {"-selection", "clipboard", "-t", "image/png", "-o"}};
	}
	return {};
}

} // namespace jpegview_linux
