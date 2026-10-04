#include "clipboard.h"

#include "external_commands.h"
#include "external_process.h"
#include "image_writer.h"
#include "sdl_abi.h"
#include "source_work_coordinator.h"
#include "work_context.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

class ClipboardTemporaryFiles {
public:
	explicit ClipboardTemporaryFiles(const char* directory)
		: directory_(directory) {}
	~ClipboardTemporaryFiles() { Remove(); }

	void SetPng(const fs::path& png) { png_ = png; }
	void Remove() {
		if (!png_.empty()) (void)::unlink(png_.c_str());
		if (directory_ != nullptr) (void)::rmdir(directory_);
		png_.clear();
		directory_ = nullptr;
	}

private:
	const char* directory_ = nullptr;
	fs::path png_;
};

bool HasExecutable(const char* executable) {
	const char* path = std::getenv("PATH");
	if (path == nullptr) return false;
	std::string paths(path);
	std::size_t begin = 0;
	while (begin <= paths.size()) {
		const std::size_t end = paths.find(':', begin);
		const std::string directory = paths.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		const fs::path candidate = (directory.empty() ? fs::path(".") : fs::path(directory)) / executable;
		if (access(candidate.c_str(), X_OK) == 0) return true;
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return false;
}

bool WaylandSession() {
	return std::getenv("WAYLAND_DISPLAY") != nullptr ||
		(std::getenv("XDG_SESSION_TYPE") != nullptr &&
			std::string(std::getenv("XDG_SESSION_TYPE")) == "wayland");
}

ExternalCommand ClipboardWriter() {
	return ClipboardWriteCommand(SelectClipboardBackend(WaylandSession(),
		HasExecutable("wl-copy"), HasExecutable("xclip")));
}

ExternalCommand ClipboardReader() {
	return ClipboardReadCommand(SelectClipboardBackend(WaylandSession(),
		HasExecutable("wl-paste"), HasExecutable("xclip")));
}

bool SendToClipboard(const std::vector<std::uint8_t>& data,
	pid_t& clipboardOwnerChild, std::string& errorMessage,
	const std::function<bool()>& shouldContinue) {
	const ExternalCommand command = ClipboardWriter();
	if (!command.Valid()) {
		errorMessage = "install xclip (X11) or wl-clipboard (Wayland) to copy images";
		return false;
	}
	clipboardOwnerChild = -1;
	return RunExternalCommandWithInput(command, data.data(), data.size(), clipboardOwnerChild,
		errorMessage, shouldContinue);
}

bool ReadFile(const fs::path& filename, std::vector<std::uint8_t>& data,
	const std::function<bool()>& shouldContinue) {
	std::ifstream input(filename, std::ios::binary);
	if (!input) return false;
	data.clear();
	std::uint8_t buffer[65536];
	while (input) {
		if (shouldContinue && !shouldContinue()) return false;
		input.read(reinterpret_cast<char*>(buffer), sizeof(buffer));
		const std::streamsize count = input.gcount();
		if (count > 0) data.insert(data.end(), buffer, buffer + count);
	}
	return !data.empty();
}

} // namespace

bool CopyTextToClipboard(const std::string& text, std::string& errorMessage) {
	if (SDL_SetClipboardText(text.c_str()) != 0) {
		errorMessage = SDL_GetError();
		return false;
	}
	return true;
}

bool CopyImageToClipboard(const std::uint8_t* bgra, int width, int height,
	std::string& errorMessage, pid_t& clipboardOwnerChild,
	const std::function<bool()>& shouldContinue) {
	clipboardOwnerChild = -1;
	if (bgra == nullptr || width <= 0 || height <= 0) {
		errorMessage = "invalid image dimensions";
		return false;
	}
	char temporaryName[] = "/tmp/jpegview-clipboard-XXXXXX";
	if (mkdtemp(temporaryName) == nullptr) {
		errorMessage = "cannot create temporary clipboard image";
		return false;
	}
	ClipboardTemporaryFiles temporaryFiles(temporaryName);
	const fs::path pngFile = fs::path(temporaryName) / "image.png";
	temporaryFiles.SetPng(pngFile);
	ImageWriteOptions options;
	std::string writeError;
	if (shouldContinue && !shouldContinue()) {
		errorMessage = "clipboard image copy was cancelled";
		return false;
	}
	WorkContext context = MakePathWorkContext(pngFile,
		SourceWorkPriority::Foreground, shouldContinue);
	bool written = false;
	{
		SourceCpuWorkLease admission =
			SourceWorkCoordinator::Global().AcquireSourceAndCpu(context, pngFile);
		if (!admission || (shouldContinue && !shouldContinue())) {
			errorMessage = "clipboard image copy was cancelled";
			return false;
		}
		context.sourceAccessAlreadyAdmitted = true;
		context.cpuProcessingAlreadyAdmitted = true;
		ScopedWorkContext workContext(context);
		written = WriteImage(pngFile, bgra, width, height, options, writeError);
	}
	// Keep the suffix in a private directory so the image writer selects PNG
	// without exposing a predictable path in the shared temporary directory.
	if (!written) {
		errorMessage = writeError;
		return false;
	}
	std::vector<std::uint8_t> encoded;
	context = MakePathWorkContext(pngFile, SourceWorkPriority::Foreground,
		shouldContinue);
	SourceWorkLease source = SourceWorkCoordinator::Global().Acquire(context, pngFile);
	bool read = false;
	if (source) {
		context.sourceAccessAlreadyAdmitted = true;
		ScopedWorkContext workContext(context);
		read = ReadFile(pngFile, encoded, shouldContinue);
	}
	source.Reset();
	temporaryFiles.Remove();
	if (!read) {
		errorMessage = shouldContinue && !shouldContinue() ?
			"clipboard image copy was cancelled" : "cannot read temporary clipboard image";
		return false;
	}
	return SendToClipboard(encoded, clipboardOwnerChild, errorMessage, shouldContinue);
}

bool PasteImageFromClipboard(std::vector<std::uint8_t>& encodedPng,
	std::string& errorMessage, const std::function<bool()>& shouldContinue) {
	const ExternalCommand command = ClipboardReader();
	if (!command.Valid()) {
		errorMessage = "install xclip (X11) or wl-clipboard (Wayland) to paste images";
		return false;
	}
	if (!RunExternalCommandWithOutput(command, encodedPng,
		256u * 1024u * 1024u, errorMessage, shouldContinue)) {
		if (errorMessage.empty()) errorMessage = "clipboard does not contain a PNG image";
		return false;
	}
	if (encodedPng.empty()) {
		errorMessage = "clipboard does not contain a PNG image";
		return false;
	}
	return true;
}

} // namespace jpegview_linux
