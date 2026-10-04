#pragma once

#include "external_commands.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace jpegview_linux {

using ExternalProcessContinuation = std::function<bool()>;

enum class ExternalCommandWaitPolicy {
	CancelWhileRunning,
	FinishAfterLaunch,
};

bool ExternalCommandAvailable(const std::string& executable);
bool RunExternalCommand(const ExternalCommand& command, std::string& errorMessage,
	const ExternalProcessContinuation& shouldContinue = {},
	ExternalCommandWaitPolicy waitPolicy = ExternalCommandWaitPolicy::CancelWhileRunning,
	bool* commandStarted = nullptr);
bool RunExternalCommandWithInput(const ExternalCommand& command,
	const std::uint8_t* data, std::size_t size, pid_t& child,
	std::string& errorMessage,
	const ExternalProcessContinuation& shouldContinue = {});
bool RunExternalCommandWithOutput(const ExternalCommand& command,
	std::vector<std::uint8_t>& output, std::size_t maximumBytes,
	std::string& errorMessage,
	const ExternalProcessContinuation& shouldContinue = {});
bool StartExternalCommandDetached(const ExternalCommand& command, pid_t& child,
	std::string& errorMessage);

} // namespace jpegview_linux
