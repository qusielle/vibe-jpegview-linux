#pragma once

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jpegview_linux {

// Serializes small durable-file writes away from the SDL event thread. Pending
// writes to the same destination coalesce to the newest owned snapshot.
class PersistenceWorker {
public:
	struct Result {
		std::filesystem::path destination;
		std::string label;
		std::string successMessage;
		std::uint64_t generation = 0;
		bool success = false;
		std::string error;
	};

	using Operation = std::function<bool(std::string&)>;

	PersistenceWorker();
	~PersistenceWorker();
	PersistenceWorker(const PersistenceWorker&) = delete;
	PersistenceWorker& operator=(const PersistenceWorker&) = delete;

	std::uint64_t Enqueue(const std::filesystem::path& destination,
		std::string label, Operation operation, std::string successMessage = {});
	std::vector<Result> TakeResults();
	void Stop();

private:
	struct Request {
		std::filesystem::path destination;
		std::string key;
		std::string label;
		std::string successMessage;
		std::uint64_t generation = 0;
		Operation operation;
	};

	void Run();

	std::mutex mutex_;
	std::condition_variable condition_;
	std::deque<std::string> readyKeys_;
	std::map<std::string, Request> pending_;
	std::map<std::string, std::uint64_t> latestGeneration_;
	std::map<std::string, Result> results_;
	std::thread worker_;
	std::uint64_t nextGeneration_ = 1;
	bool stopping_ = false;
};

// A persistence completion may change the shared window title only while its
// request still owns that status. Image-bound actions also expire on navigation.
class PersistenceStatusOwner {
public:
	void Begin(std::uint64_t generation, const std::filesystem::path& destination,
		bool boundToImage, std::uint64_t imageGeneration) {
		generation_ = generation;
		destination_ = destination.lexically_normal();
		boundToImage_ = boundToImage;
		imageGeneration_ = imageGeneration;
	}

	bool InvalidateForNewRequest(const std::filesystem::path& destination,
		std::uint64_t generation) {
		if (generation_ == 0 || generation_ == generation ||
			destination_ != destination.lexically_normal()) return false;
		Clear();
		return true;
	}

	bool ConsumeIfCurrent(std::uint64_t generation,
		std::uint64_t currentImageGeneration) {
		if (generation_ != generation) return false;
		const bool imageStillCurrent = !boundToImage_ ||
			imageGeneration_ == currentImageGeneration;
		Clear();
		return imageStillCurrent;
	}

	void Clear() {
		generation_ = 0;
		destination_.clear();
		boundToImage_ = false;
		imageGeneration_ = 0;
	}

private:
	std::uint64_t generation_ = 0;
	std::filesystem::path destination_;
	std::uint64_t imageGeneration_ = 0;
	bool boundToImage_ = false;
};

} // namespace jpegview_linux
