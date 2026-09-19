#include "clipboard.h"

#include "external_commands.h"
#include "image_writer.h"
#include "sdl_abi.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

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

[[noreturn]] void ExecCommand(const ExternalCommand& command) {
	std::vector<char*> arguments;
	arguments.reserve(command.arguments.size() + 2);
	arguments.push_back(const_cast<char*>(command.executable.c_str()));
	for (const std::string& argument : command.arguments) {
		arguments.push_back(const_cast<char*>(argument.c_str()));
	}
	arguments.push_back(nullptr);
	execvp(command.executable.c_str(), arguments.data());
	_exit(127);
}

bool WriteAll(int descriptor, const std::uint8_t* data, std::size_t size) {
	while (size != 0) {
		const ssize_t written = write(descriptor, data, size);
		if (written < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		if (written == 0) return false;
		data += written;
		size -= static_cast<std::size_t>(written);
	}
	return true;
}

bool WriteClipboardPayload(int descriptor, const std::vector<std::uint8_t>& data) {
	// A clipboard manager is allowed to reject a format immediately.  Ignore
	// SIGPIPE for this short write so that a closed helper becomes a normal
	// error instead of terminating the viewer.
	using SignalHandler = void (*)(int);
	const SignalHandler previous = signal(SIGPIPE, SIG_IGN);
	const bool written = WriteAll(descriptor, data.data(), data.size());
	signal(SIGPIPE, previous);
	return written;
}

bool SendToClipboard(const std::vector<std::uint8_t>& data, std::string& errorMessage) {
	const ExternalCommand command = ClipboardWriter();
	if (!command.Valid()) {
		errorMessage = "install xclip (X11) or wl-clipboard (Wayland) to copy images";
		return false;
	}
	int descriptors[2]{};
	if (pipe(descriptors) != 0) {
		errorMessage = "cannot create clipboard pipe";
		return false;
	}
	const pid_t child = fork();
	if (child < 0) {
		close(descriptors[0]);
		close(descriptors[1]);
		errorMessage = "cannot start clipboard helper";
		return false;
	}
	if (child == 0) {
		// Leave the clipboard owner alive after JPEGView exits.  The first child
		// is reaped by the viewer, while the helper is adopted by init.
		const pid_t helper = fork();
		if (helper == 0) {
			close(descriptors[1]);
			if (dup2(descriptors[0], STDIN_FILENO) < 0) _exit(126);
			close(descriptors[0]);
			ExecCommand(command);
		}
		close(descriptors[0]);
		close(descriptors[1]);
		_exit(helper < 0 ? 127 : 0);
	}
	close(descriptors[0]);
	int childStatus = 0;
	while (waitpid(child, &childStatus, 0) < 0 && errno == EINTR) {}
	const bool written = WriteClipboardPayload(descriptors[1], data);
	close(descriptors[1]);
	if (!written) {
		errorMessage = "clipboard helper closed before receiving the image";
		return false;
	}
	return true;
}

bool CaptureFromClipboard(const ExternalCommand& command, std::vector<std::uint8_t>& data) {
	int descriptors[2]{};
	if (pipe(descriptors) != 0) return false;
	const pid_t child = fork();
	if (child < 0) {
		close(descriptors[0]);
		close(descriptors[1]);
		return false;
	}
	if (child == 0) {
		close(descriptors[0]);
		if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(126);
		close(descriptors[1]);
		ExecCommand(command);
	}
	close(descriptors[1]);
	data.clear();
	std::uint8_t buffer[8192];
	for (;;) {
		const ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
		if (count == 0) break;
		if (count < 0) {
			if (errno == EINTR) continue;
			close(descriptors[0]);
			waitpid(child, nullptr, 0);
			return false;
		}
		data.insert(data.end(), buffer, buffer + count);
		if (data.size() > 256u * 1024u * 1024u) {
			close(descriptors[0]);
			waitpid(child, nullptr, 0);
			return false;
		}
	}
	close(descriptors[0]);
	int status = 0;
	if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
	return !data.empty();
}

bool ReadFile(const fs::path& filename, std::vector<std::uint8_t>& data) {
	std::ifstream input(filename, std::ios::binary);
	if (!input) return false;
	data.assign(std::istreambuf_iterator<char>(input), {});
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

bool CopyImageToClipboard(const std::uint8_t* bgra, int width, int height, std::string& errorMessage) {
	if (bgra == nullptr || width <= 0 || height <= 0) {
		errorMessage = "invalid image dimensions";
		return false;
	}
	char temporaryName[] = "/tmp/jpegview-clipboard-XXXXXX";
	const int descriptor = mkstemp(temporaryName);
	if (descriptor < 0) {
		errorMessage = "cannot create temporary clipboard image";
		return false;
	}
	close(descriptor);
	const fs::path temporaryFile(temporaryName);
	ImageWriteOptions options;
	std::string writeError;
	const bool written = WriteImage(temporaryFile.string() + ".png", bgra, width, height, options, writeError);
	// WriteImage selects the codec by extension, so the temporary path with a
	// suffix is separate from the mkstemp placeholder.
	unlink(temporaryName);
	const fs::path pngFile = temporaryFile.string() + ".png";
	if (!written) {
		errorMessage = writeError;
		return false;
	}
	std::vector<std::uint8_t> encoded;
	const bool read = ReadFile(pngFile, encoded);
	unlink(pngFile.c_str());
	if (!read) {
		errorMessage = "cannot read temporary clipboard image";
		return false;
	}
	return SendToClipboard(encoded, errorMessage);
}

bool PasteImageFromClipboard(std::vector<std::uint8_t>& encodedPng, std::string& errorMessage) {
	const ExternalCommand command = ClipboardReader();
	if (!command.Valid()) {
		errorMessage = "install xclip (X11) or wl-clipboard (Wayland) to paste images";
		return false;
	}
	if (!CaptureFromClipboard(command, encodedPng)) {
		errorMessage = "clipboard does not contain a PNG image";
		return false;
	}
	return true;
}

} // namespace jpegview_linux
