#include "persistence_worker.h"

#include "event_loop_model.h"

#include <exception>
#include <utility>

namespace jpegview_linux {

PersistenceWorker::PersistenceWorker() {
	// Start only after every state member, including stopping_, is initialized.
	worker_ = std::thread([this] { Run(); });
}

PersistenceWorker::~PersistenceWorker() {
	Stop();
}

std::uint64_t PersistenceWorker::Enqueue(const std::filesystem::path& destination,
	std::string label, Operation operation, std::string successMessage) {
	if (destination.empty() || !operation) return 0;
	const std::filesystem::path normalized = destination.lexically_normal();
	const std::string key = normalized.string();
	if (key.empty()) return 0;
	std::uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_) return 0;
		generation = nextGeneration_++;
		Request request;
		request.destination = normalized;
		request.key = key;
		request.label = std::move(label);
		request.successMessage = std::move(successMessage);
		request.generation = generation;
		request.operation = std::move(operation);
		if (pending_.find(key) == pending_.end()) readyKeys_.push_back(key);
		pending_[key] = std::move(request);
		latestGeneration_[key] = generation;
		results_.erase(key);
	}
	condition_.notify_one();
	return generation;
}

std::vector<PersistenceWorker::Result> PersistenceWorker::TakeResults() {
	std::vector<Result> results;
	std::lock_guard<std::mutex> lock(mutex_);
	results.reserve(results_.size());
	for (auto& entry : results_) {
		const auto latest = latestGeneration_.find(entry.first);
		if (latest != latestGeneration_.end() &&
			latest->second == entry.second.generation) {
			results.push_back(std::move(entry.second));
			latestGeneration_.erase(latest);
		}
	}
	results_.clear();
	return results;
}

void PersistenceWorker::Stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
	}
	condition_.notify_all();
	if (worker_.joinable()) worker_.join();
}

void PersistenceWorker::Run() {
	for (;;) {
		Request request;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			condition_.wait(lock, [this] { return stopping_ || !readyKeys_.empty(); });
			if (readyKeys_.empty()) {
				if (stopping_) return;
				continue;
			}
			const std::string key = std::move(readyKeys_.front());
			readyKeys_.pop_front();
			auto found = pending_.find(key);
			if (found == pending_.end()) continue;
			request = std::move(found->second);
			pending_.erase(found);
		}

		Result result;
		result.destination = request.destination;
		result.label = request.label;
		result.successMessage = request.successMessage;
		result.generation = request.generation;
		try {
			result.success = request.operation(result.error);
		} catch (const std::exception& exception) {
			result.error = exception.what();
		} catch (...) {
			result.error = "unknown persistence worker failure";
		}
		if (!result.success && result.error.empty()) {
			result.error = "write operation failed";
		}

		bool published = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto latest = latestGeneration_.find(request.key);
			if (latest != latestGeneration_.end() && latest->second == request.generation) {
				results_[request.key] = std::move(result);
				published = true;
			}
		}
		if (published) UiCompletionWakeup().Notify();
	}
}

} // namespace jpegview_linux
