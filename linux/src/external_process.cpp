#include "external_process.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace jpegview_linux {
namespace {

class SpawnActions {
public:
	SpawnActions() : error_(posix_spawn_file_actions_init(&actions_)),
		initialized_(error_ == 0) {}
	~SpawnActions() {
		if (initialized_) posix_spawn_file_actions_destroy(&actions_);
	}
	SpawnActions(const SpawnActions&) = delete;
	SpawnActions& operator=(const SpawnActions&) = delete;

	bool Valid() const { return error_ == 0; }
	int AddDup2(int source, int destination) {
		if (error_ == 0) error_ = posix_spawn_file_actions_adddup2(
			&actions_, source, destination);
		return error_;
	}
	int AddClose(int descriptor) {
		if (error_ == 0) error_ = posix_spawn_file_actions_addclose(
			&actions_, descriptor);
		return error_;
	}
	posix_spawn_file_actions_t* Get() { return &actions_; }

private:
	posix_spawn_file_actions_t actions_{};
	int error_ = 0;
	bool initialized_ = false;
};

class SpawnAttributes {
public:
	SpawnAttributes() : error_(posix_spawnattr_init(&attributes_)),
		initialized_(error_ == 0) {}
	~SpawnAttributes() {
		if (initialized_) posix_spawnattr_destroy(&attributes_);
	}
	SpawnAttributes(const SpawnAttributes&) = delete;
	SpawnAttributes& operator=(const SpawnAttributes&) = delete;

	bool Valid() const { return error_ == 0; }
	int SetProcessGroup() {
		if (error_ != 0) return error_;
		error_ = posix_spawnattr_setpgroup(&attributes_, 0);
		if (error_ == 0) error_ = posix_spawnattr_setflags(&attributes_,
			static_cast<short>(POSIX_SPAWN_SETPGROUP));
		return error_;
	}
	int SetDetachedSession() {
		if (error_ != 0) return error_;
#ifdef POSIX_SPAWN_SETSID
		error_ = posix_spawnattr_setflags(&attributes_,
			static_cast<short>(POSIX_SPAWN_SETSID));
#else
		error_ = posix_spawnattr_setpgroup(&attributes_, 0);
		if (error_ == 0) error_ = posix_spawnattr_setflags(&attributes_,
			static_cast<short>(POSIX_SPAWN_SETPGROUP));
#endif
		return error_;
	}
	posix_spawnattr_t* Get() { return &attributes_; }

private:
	posix_spawnattr_t attributes_{};
	int error_ = 0;
	bool initialized_ = false;
};

std::vector<char*> MakeArgv(const ExternalCommand& command) {
	std::vector<char*> arguments;
	arguments.reserve(command.arguments.size() + 2);
	arguments.push_back(const_cast<char*>(command.executable.c_str()));
	for (const std::string& argument : command.arguments) {
		arguments.push_back(const_cast<char*>(argument.c_str()));
	}
	arguments.push_back(nullptr);
	return arguments;
}

bool SetCloseOnExec(int descriptor) {
	const int flags = fcntl(descriptor, F_GETFD);
	return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

class ScopedDescriptor {
public:
	explicit ScopedDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
	~ScopedDescriptor() { Reset(); }
	ScopedDescriptor(const ScopedDescriptor&) = delete;
	ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

	int Get() const { return descriptor_; }
	void Reset(int descriptor = -1) {
		if (descriptor_ >= 0) (void)::close(descriptor_);
		descriptor_ = descriptor;
	}

private:
	int descriptor_ = -1;
};

bool EnsurePipeDescriptor(int& descriptor) {
	if (descriptor < 3) {
		const int promoted = fcntl(descriptor, F_DUPFD, 3);
		if (promoted < 0) return false;
		if (!SetCloseOnExec(promoted)) {
			const int savedError = errno;
			(void)::close(promoted);
			errno = savedError;
			return false;
		}
		(void)::close(descriptor);
		descriptor = promoted;
		return true;
	}
	return SetCloseOnExec(descriptor);
}

bool CreatePipe(int descriptors[2]) {
#ifdef O_CLOEXEC
	if (pipe2(descriptors, O_CLOEXEC) != 0) return false;
#else
	if (pipe(descriptors) != 0) return false;
#endif
	if (EnsurePipeDescriptor(descriptors[0]) &&
		EnsurePipeDescriptor(descriptors[1])) return true;
	const int savedError = errno;
	close(descriptors[0]);
	close(descriptors[1]);
	errno = savedError;
	return false;
}

bool Spawn(const ExternalCommand& command, SpawnActions* actions,
	SpawnAttributes* attributes, pid_t& child, std::string& errorMessage) {
	if (!command.Valid()) {
		errorMessage = "invalid external command";
		return false;
	}
	if (actions != nullptr && !actions->Valid()) {
		errorMessage = "cannot prepare external command file actions";
		return false;
	}
	if (attributes != nullptr && !attributes->Valid()) {
		errorMessage = "cannot prepare external command process attributes";
		return false;
	}
	std::vector<char*> arguments = MakeArgv(command);
	const int error = posix_spawnp(&child, command.executable.c_str(),
		actions == nullptr ? nullptr : actions->Get(),
		attributes == nullptr ? nullptr : attributes->Get(), arguments.data(), environ);
	if (error == 0) return true;
	errorMessage = error == ENOENT ? command.executable + " is not installed" :
		"cannot start " + command.executable + ": " + std::strerror(error);
	return false;
}

bool Continue(const ExternalProcessContinuation& shouldContinue) {
	if (!shouldContinue) return true;
	try {
		return shouldContinue();
	} catch (...) {
		return false;
	}
}

bool ReapAfterCancellation(pid_t child, int* status = nullptr) {
	(void)::kill(-child, SIGTERM);
	(void)::kill(child, SIGTERM);
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(200);
	int childStatus = 0;
	bool childReaped = false;
	for (;;) {
		if (!childReaped) {
			const pid_t waited = waitpid(child, &childStatus, WNOHANG);
			if (waited == child) {
				childReaped = true;
			} else if (waited < 0 && errno != EINTR) {
				if (errno == ECHILD) childReaped = true;
				else return false;
			}
		}
		if (childReaped && ::kill(-child, 0) != 0 && errno == ESRCH) {
			if (status != nullptr) *status = childStatus;
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) break;
		(void)::poll(nullptr, 0, 10);
	}
	(void)::kill(-child, SIGKILL);
	if (!childReaped) (void)::kill(child, SIGKILL);
	if (childReaped) {
		if (status != nullptr) *status = childStatus;
		return true;
	}
	for (;;) {
		const pid_t waited = waitpid(child, &childStatus, 0);
		if (waited == child) {
			if (status != nullptr) *status = childStatus;
			return true;
		}
		if (waited < 0 && errno == EINTR) continue;
		return waited < 0 && errno == ECHILD;
	}
}

class SpawnedChild {
public:
	explicit SpawnedChild(pid_t child) : child_(child) {}
	~SpawnedChild() {
		if (child_ > 0) (void)ReapAfterCancellation(child_);
	}
	SpawnedChild(const SpawnedChild&) = delete;
	SpawnedChild& operator=(const SpawnedChild&) = delete;

	pid_t Get() const { return child_; }
	pid_t Release() {
		const pid_t child = child_;
		child_ = -1;
		return child;
	}
	void MarkReaped() { child_ = -1; }
	void Terminate() {
		if (child_ > 0 && ReapAfterCancellation(child_)) child_ = -1;
	}

private:
	pid_t child_ = -1;
};

bool WaitForChild(SpawnedChild& process, std::string& errorMessage,
	const ExternalProcessContinuation& shouldContinue) {
	const pid_t child = process.Get();
	int status = 0;
	for (;;) {
		const pid_t waited = waitpid(child, &status, WNOHANG);
		if (waited == child) {
			process.MarkReaped();
			break;
		}
		if (waited < 0) {
			if (errno == EINTR) continue;
			if (errno == ECHILD) process.MarkReaped();
			errorMessage = "cannot wait for external command";
			return false;
		}
		if (!Continue(shouldContinue)) {
			process.Terminate();
			errorMessage = "external command was cancelled";
			return false;
		}
		(void)::poll(nullptr, 0, 10);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errorMessage = "external command failed";
		return false;
	}
	return true;
}

bool SpawnWithPipe(const ExternalCommand& command, int childDescriptor,
	int childPipeEnd, int parentPipeEnd, SpawnActions& actions,
	SpawnAttributes& attributes, pid_t& child, std::string& errorMessage) {
	int actionError = actions.AddDup2(childPipeEnd, childDescriptor);
	if (actionError == 0) actionError = actions.AddClose(parentPipeEnd);
	if (actionError == 0) actionError = actions.AddClose(childPipeEnd);
	if (actionError != 0) {
		errorMessage = "cannot prepare external command pipe: " +
			std::string(std::strerror(actionError));
		return false;
	}
	if (attributes.SetProcessGroup() != 0) {
		errorMessage = "cannot prepare external command process group";
		return false;
	}
	return Spawn(command, &actions, &attributes, child, errorMessage);
}

} // namespace

bool ExternalCommandAvailable(const std::string& executable) {
	if (executable.empty()) return false;
	if (executable.find('/') != std::string::npos) return access(executable.c_str(), X_OK) == 0;
	const char* pathValue = std::getenv("PATH");
	if (pathValue == nullptr) return false;
	const std::string path(pathValue);
	std::size_t begin = 0;
	while (begin <= path.size()) {
		const std::size_t end = path.find(':', begin);
		const std::string directory = path.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		const std::string candidate = (directory.empty() ? "." : directory) + "/" + executable;
		if (access(candidate.c_str(), X_OK) == 0) return true;
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return false;
}

bool RunExternalCommand(const ExternalCommand& command, std::string& errorMessage,
	const ExternalProcessContinuation& shouldContinue,
	ExternalCommandWaitPolicy waitPolicy, bool* commandStarted) {
	errorMessage.clear();
	if (commandStarted != nullptr) *commandStarted = false;
	if (!Continue(shouldContinue)) {
		errorMessage = "external command was cancelled";
		return false;
	}
	SpawnAttributes attributes;
	if (attributes.SetProcessGroup() != 0) {
		errorMessage = "cannot prepare external command process group";
		return false;
	}
	pid_t child = -1;
	if (!Spawn(command, nullptr, &attributes, child, errorMessage)) return false;
	if (commandStarted != nullptr) *commandStarted = true;
	SpawnedChild process(child);
	return WaitForChild(process, errorMessage,
		waitPolicy == ExternalCommandWaitPolicy::FinishAfterLaunch ?
			ExternalProcessContinuation{} : shouldContinue);
}

bool RunExternalCommandWithInput(const ExternalCommand& command,
	const std::uint8_t* data, std::size_t size, pid_t& child,
	std::string& errorMessage,
	const ExternalProcessContinuation& shouldContinue) {
	errorMessage.clear();
	child = -1;
	if ((size != 0 && data == nullptr) || !Continue(shouldContinue)) {
		errorMessage = "invalid or cancelled external command input";
		return false;
	}
	int descriptors[2]{};
	if (!CreatePipe(descriptors)) {
		errorMessage = "cannot create external command input pipe";
		return false;
	}
	ScopedDescriptor childReadEnd(descriptors[0]);
	ScopedDescriptor parentWriteEnd(descriptors[1]);
	SpawnActions actions;
	SpawnAttributes attributes;
	pid_t spawned = -1;
	if (!SpawnWithPipe(command, STDIN_FILENO, childReadEnd.Get(),
		parentWriteEnd.Get(), actions, attributes, spawned, errorMessage)) {
		return false;
	}
	SpawnedChild process(spawned);
	childReadEnd.Reset();
	const int flags = fcntl(parentWriteEnd.Get(), F_GETFL);
	if (flags < 0 || fcntl(parentWriteEnd.Get(), F_SETFL, flags | O_NONBLOCK) != 0) {
		errorMessage = "cannot configure external command input pipe";
		return false;
	}
	sigset_t blocked{};
	sigset_t previous{};
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGPIPE);
	const int maskError = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
	if (maskError != 0) {
		parentWriteEnd.Reset();
		errorMessage = "cannot protect external command input pipe";
		return false;
	}
	std::size_t offset = 0;
	bool written = true;
	while (offset < size) {
		if (!Continue(shouldContinue)) {
			written = false;
			errno = ECANCELED;
			break;
		}
		pollfd descriptor{parentWriteEnd.Get(), POLLOUT, 0};
		const int polled = poll(&descriptor, 1, 25);
		if (polled < 0) {
			if (errno == EINTR) continue;
			written = false;
			break;
		}
		if (polled == 0) continue;
		if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			written = false;
			errno = EPIPE;
			break;
		}
		const ssize_t count = write(parentWriteEnd.Get(), data + offset, size - offset);
		if (count < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
			written = false;
			break;
		}
		if (count == 0) {
			written = false;
			errno = EPIPE;
			break;
		}
		offset += static_cast<std::size_t>(count);
	}
	const int writeError = written ? 0 : errno;
	if (writeError == EPIPE) {
		timespec timeout{0, 0};
		(void)sigtimedwait(&blocked, nullptr, &timeout);
	}
	(void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	parentWriteEnd.Reset();
	if (!written) {
		errorMessage = writeError == ECANCELED ? "external command input was cancelled" :
			"external command closed its input";
		return false;
	}
	child = process.Release();
	return true;
}

bool RunExternalCommandWithOutput(const ExternalCommand& command,
	std::vector<std::uint8_t>& output, std::size_t maximumBytes,
	std::string& errorMessage,
	const ExternalProcessContinuation& shouldContinue) {
	errorMessage.clear();
	output.clear();
	if (!Continue(shouldContinue)) {
		errorMessage = "external command was cancelled";
		return false;
	}
	int descriptors[2]{};
	if (!CreatePipe(descriptors)) {
		errorMessage = "cannot create external command output pipe";
		return false;
	}
	ScopedDescriptor parentReadEnd(descriptors[0]);
	ScopedDescriptor childWriteEnd(descriptors[1]);
	SpawnActions actions;
	SpawnAttributes attributes;
	pid_t spawned = -1;
	if (!SpawnWithPipe(command, STDOUT_FILENO, childWriteEnd.Get(),
		parentReadEnd.Get(), actions, attributes, spawned, errorMessage)) {
		return false;
	}
	SpawnedChild process(spawned);
	childWriteEnd.Reset();
	const int flags = fcntl(parentReadEnd.Get(), F_GETFL);
	if (flags < 0 || fcntl(parentReadEnd.Get(), F_SETFL, flags | O_NONBLOCK) != 0) {
		errorMessage = "cannot configure external command output pipe";
		return false;
	}
	std::uint8_t buffer[8192];
	bool reachedEnd = false;
	while (!reachedEnd) {
		if (!Continue(shouldContinue)) {
			process.Terminate();
			errorMessage = "external command was cancelled";
			return false;
		}
		pollfd descriptor{parentReadEnd.Get(), POLLIN | POLLHUP, 0};
		const int polled = poll(&descriptor, 1, 25);
		if (polled < 0) {
			if (errno == EINTR) continue;
			process.Terminate();
			errorMessage = "cannot read external command output";
			return false;
		}
		if (polled == 0) continue;
		for (;;) {
			const ssize_t count = read(parentReadEnd.Get(), buffer, sizeof(buffer));
			if (count == 0) {
				reachedEnd = true;
				break;
			}
			if (count < 0) {
				if (errno == EINTR) continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK) break;
				process.Terminate();
				errorMessage = "cannot read external command output";
				return false;
			}
			const std::size_t byteCount = static_cast<std::size_t>(count);
			if (byteCount > maximumBytes - std::min(maximumBytes, output.size())) {
				process.Terminate();
				output.clear();
				errorMessage = "external command output exceeded its size limit";
				return false;
			}
			output.insert(output.end(), buffer, buffer + byteCount);
		}
	}
	parentReadEnd.Reset();
	return WaitForChild(process, errorMessage, shouldContinue);
}

bool StartExternalCommandDetached(const ExternalCommand& command, pid_t& child,
	std::string& errorMessage) {
	errorMessage.clear();
	SpawnAttributes attributes;
	if (attributes.SetDetachedSession() != 0) {
		errorMessage = "cannot prepare detached external command";
		return false;
	}
	return Spawn(command, nullptr, &attributes, child, errorMessage);
}

} // namespace jpegview_linux
