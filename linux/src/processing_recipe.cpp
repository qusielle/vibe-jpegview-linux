#include "processing_recipe.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <atomic>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

constexpr std::size_t kMaximumRecipeBytes = 64u * 1024u;
constexpr std::size_t kMaximumRecipeNameBytes = 256;

bool Continue(const std::function<bool()>& shouldContinue) {
	if (!shouldContinue) return true;
	try {
		return shouldContinue();
	} catch (...) {
		return false;
	}
}

struct NumberField {
	const char* key;
	double ImageProcessingParams::*member;
};

constexpr std::array<NumberField, 15> kNumberFields = {{
	{"contrast", &ImageProcessingParams::contrast},
	{"gamma", &ImageProcessingParams::gamma},
	{"saturation", &ImageProcessingParams::saturation},
	{"cyan_red", &ImageProcessingParams::cyanRed},
	{"magenta_green", &ImageProcessingParams::magentaGreen},
	{"yellow_blue", &ImageProcessingParams::yellowBlue},
	{"lighten_shadows", &ImageProcessingParams::lightenShadows},
	{"darken_highlights", &ImageProcessingParams::darkenHighlights},
	{"deep_shadows", &ImageProcessingParams::deepShadows},
	{"color_correction", &ImageProcessingParams::colorCorrection},
	{"contrast_correction", &ImageProcessingParams::contrastCorrection},
	{"sharpen", &ImageProcessingParams::sharpen},
	{"unsharp_radius", &ImageProcessingParams::unsharpRadius},
	{"unsharp_amount", &ImageProcessingParams::unsharpAmount},
	{"unsharp_threshold", &ImageProcessingParams::unsharpThreshold},
}};

enum class ScalarKind { String, Number, Boolean };

struct JsonScalar {
	ScalarKind kind = ScalarKind::String;
	std::string stringValue;
	double numberValue = 0.0;
	bool booleanValue = false;
};

bool ValidRecipeName(const std::string& name) {
	if (name.empty() || name.size() > kMaximumRecipeNameBytes) return false;
	for (unsigned char byte : name) {
		if (byte < 0x20u || byte == 0x7fu) return false;
	}
	return true;
}

bool ValidPreset(const ImageProcessingPreset& preset) {
	for (std::size_t index = 0;
		index < static_cast<std::size_t>(LevelControl::Count); ++index) {
		const LevelControl control = static_cast<LevelControl>(index);
		const double value = GetLevelControlValue(preset.processing, control);
		const LevelControlInfo& info = GetLevelControlInfo(control);
		if (!std::isfinite(value) || value < info.minimum || value > info.maximum) return false;
	}
	const ImageProcessingParams& params = preset.processing;
	return std::isfinite(params.unsharpRadius) && params.unsharpRadius >= 0.0 &&
		params.unsharpRadius <= 5.0 && std::isfinite(params.unsharpAmount) &&
		params.unsharpAmount >= 0.0 && params.unsharpAmount <= 10.0 &&
		std::isfinite(params.unsharpThreshold) && params.unsharpThreshold >= 0.0 &&
		params.unsharpThreshold <= 20.0;
}

void AppendUtf8(std::string& output, std::uint32_t codepoint) {
	if (codepoint <= 0x7fu) {
		output.push_back(static_cast<char>(codepoint));
	} else if (codepoint <= 0x7ffu) {
		output.push_back(static_cast<char>(0xc0u | (codepoint >> 6)));
		output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
	} else if (codepoint <= 0xffffu) {
		output.push_back(static_cast<char>(0xe0u | (codepoint >> 12)));
		output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
		output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
	} else {
		output.push_back(static_cast<char>(0xf0u | (codepoint >> 18)));
		output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3fu)));
		output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
		output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
	}
}

class JsonObjectParser {
public:
	explicit JsonObjectParser(const std::string& input) : input_(input) {}

	bool Parse(std::map<std::string, JsonScalar>& fields) {
		SkipWhitespace();
		if (position_ + 3 <= input_.size() &&
			static_cast<unsigned char>(input_[position_]) == 0xefu &&
			static_cast<unsigned char>(input_[position_ + 1]) == 0xbbu &&
			static_cast<unsigned char>(input_[position_ + 2]) == 0xbfu) {
			position_ += 3;
			SkipWhitespace();
		}
		if (!Consume('{')) return Fail("expected a JSON object");
		SkipWhitespace();
		if (Consume('}')) return Finish();
		for (;;) {
			std::string key;
			if (!ParseString(key)) return false;
			SkipWhitespace();
			if (!Consume(':')) return Fail("expected ':' after a JSON field name");
			SkipWhitespace();
			JsonScalar value;
			if (!ParseScalar(value)) return false;
			if (!fields.emplace(std::move(key), std::move(value)).second) {
				return Fail("duplicate JSON field");
			}
			SkipWhitespace();
			if (Consume('}')) return Finish();
			if (!Consume(',')) return Fail("expected ',' or '}' in JSON object");
			SkipWhitespace();
		}
	}

	const std::string& Error() const { return error_; }

private:
	bool Finish() {
		SkipWhitespace();
		return position_ == input_.size() || Fail("unexpected data after JSON object");
	}

	void SkipWhitespace() {
		while (position_ < input_.size() &&
			(input_[position_] == ' ' || input_[position_] == '\t' ||
			 input_[position_] == '\r' || input_[position_] == '\n')) ++position_;
	}

	bool Consume(char expected) {
		if (position_ >= input_.size() || input_[position_] != expected) return false;
		++position_;
		return true;
	}

	bool Fail(const char* message) {
		if (error_.empty()) error_ = message;
		return false;
	}

	bool ParseHex4(std::uint32_t& value) {
		if (position_ + 4 > input_.size()) return Fail("incomplete JSON unicode escape");
		value = 0;
		for (int index = 0; index < 4; ++index) {
			const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
			value <<= 4;
			if (byte >= '0' && byte <= '9') value |= byte - '0';
			else if (byte >= 'a' && byte <= 'f') value |= byte - 'a' + 10;
			else if (byte >= 'A' && byte <= 'F') value |= byte - 'A' + 10;
			else return Fail("invalid JSON unicode escape");
		}
		return true;
	}

	bool ParseString(std::string& value) {
		if (!Consume('"')) return Fail("expected a JSON string");
		while (position_ < input_.size()) {
			const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
			if (byte == '"') return true;
			if (byte < 0x20u) return Fail("unescaped control byte in JSON string");
			if (byte != '\\') {
				value.push_back(static_cast<char>(byte));
				continue;
			}
			if (position_ >= input_.size()) return Fail("incomplete JSON escape");
			const char escaped = input_[position_++];
			switch (escaped) {
			case '"': value.push_back('"'); break;
			case '\\': value.push_back('\\'); break;
			case '/': value.push_back('/'); break;
			case 'b': value.push_back('\b'); break;
			case 'f': value.push_back('\f'); break;
			case 'n': value.push_back('\n'); break;
			case 'r': value.push_back('\r'); break;
			case 't': value.push_back('\t'); break;
			case 'u': {
				std::uint32_t codepoint = 0;
				if (!ParseHex4(codepoint)) return false;
				if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
					if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
						input_[position_ + 1] != 'u') return Fail("missing low JSON surrogate");
					position_ += 2;
					std::uint32_t low = 0;
					if (!ParseHex4(low) || low < 0xdc00u || low > 0xdfffu) {
						return Fail("invalid low JSON surrogate");
					}
					codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) + (low - 0xdc00u);
				} else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
					return Fail("unpaired low JSON surrogate");
				}
				AppendUtf8(value, codepoint);
				break;
			}
			default: return Fail("invalid JSON escape");
			}
		}
		return Fail("unterminated JSON string");
	}

	bool ParseNumber(JsonScalar& value) {
		const std::size_t begin = position_;
		Consume('-');
		if (Consume('0')) {
			if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
				return Fail("leading zero in JSON number");
			}
		} else {
			if (position_ >= input_.size() || input_[position_] < '1' || input_[position_] > '9') {
				return Fail("invalid JSON number");
			}
			while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
		}
		if (Consume('.')) {
			const std::size_t fraction = position_;
			while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
			if (position_ == fraction) return Fail("invalid JSON number fraction");
		}
		if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
			++position_;
			if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
			const std::size_t exponent = position_;
			while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
			if (position_ == exponent) return Fail("invalid JSON number exponent");
		}
		std::istringstream number(input_.substr(begin, position_ - begin));
		number.imbue(std::locale::classic());
		value.kind = ScalarKind::Number;
		if (!(number >> value.numberValue) || !std::isfinite(value.numberValue) ||
			number.peek() != std::char_traits<char>::eof()) return Fail("invalid JSON number value");
		return true;
	}

	bool ParseScalar(JsonScalar& value) {
		if (position_ >= input_.size()) return Fail("missing JSON value");
		if (input_[position_] == '"') {
			value.kind = ScalarKind::String;
			return ParseString(value.stringValue);
		}
		if (input_[position_] == '-' || (input_[position_] >= '0' && input_[position_] <= '9')) {
			return ParseNumber(value);
		}
		const auto consumeLiteral = [this](const char* literal) {
			const std::size_t length = std::strlen(literal);
			if (input_.compare(position_, length, literal) != 0) return false;
			position_ += length;
			return true;
		};
		value.kind = ScalarKind::Boolean;
		if (consumeLiteral("true")) value.booleanValue = true;
		else if (consumeLiteral("false")) value.booleanValue = false;
		else return Fail("recipe JSON values must be strings, numbers, or booleans");
		return true;
	}

	const std::string& input_;
	std::size_t position_ = 0;
	std::string error_;
};

void AppendJsonString(std::ostringstream& output, const std::string& value) {
	output << '"';
	for (unsigned char byte : value) {
		switch (byte) {
		case '"': output << "\\\""; break;
		case '\\': output << "\\\\"; break;
		case '\b': output << "\\b"; break;
		case '\f': output << "\\f"; break;
		case '\n': output << "\\n"; break;
		case '\r': output << "\\r"; break;
		case '\t': output << "\\t"; break;
		default:
			if (byte < 0x20u) {
				output << "\\u00" << std::hex << std::setw(2) << std::setfill('0') <<
					static_cast<unsigned int>(byte) << std::dec << std::setfill(' ');
			} else output << static_cast<char>(byte);
		}
	}
	output << '"';
}

bool RequireField(const std::map<std::string, JsonScalar>& fields,
	const std::string& key, ScalarKind kind, const JsonScalar*& value,
	std::string& errorMessage) {
	const auto found = fields.find(key);
	if (found == fields.end() || found->second.kind != kind) {
		errorMessage = "recipe JSON is missing or has an invalid '" + key + "' field";
		return false;
	}
	value = &found->second;
	return true;
}

bool WriteAll(int descriptor, const std::string& contents, std::string& errorMessage,
	const std::function<bool()>& shouldContinue) {
	std::size_t offset = 0;
	while (offset < contents.size()) {
		if (!Continue(shouldContinue)) {
			errorMessage = "processing recipe write was cancelled";
			return false;
		}
		const ssize_t written = ::write(descriptor, contents.data() + offset,
			contents.size() - offset);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) {
			errorMessage = "cannot write processing recipe: " +
				std::string(std::strerror(errno));
			return false;
		}
		offset += static_cast<std::size_t>(written);
	}
	return true;
}

bool PublishNoReplace(const fs::path& temporary, const fs::path& output,
	std::string& errorMessage, bool& outputAppeared) {
#if defined(__linux__) && defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
	if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
		output.c_str(), RENAME_NOREPLACE) == 0) return true;
	const int renameError = errno;
	if (renameError == EEXIST) {
		outputAppeared = true;
		errorMessage = "output appeared before recipe publication";
		return false;
	}
	if (renameError != ENOSYS && renameError != EINVAL && renameError != EOPNOTSUPP) {
		errorMessage = "cannot publish recipe without replacing output: " +
			std::string(std::strerror(renameError));
		return false;
	}
#endif
	if (::link(temporary.c_str(), output.c_str()) != 0) {
		const int linkError = errno;
		if (linkError == EEXIST) outputAppeared = true;
		errorMessage = linkError == EEXIST ? "output appeared before recipe publication" :
			"cannot publish recipe without replacing output: " +
			std::string(std::strerror(linkError));
		return false;
	}
	if (::unlink(temporary.c_str()) == 0) return true;
	errorMessage = "recipe was published but its temporary link could not be removed";
	return false;
}

} // namespace

bool SerializeProcessingRecipeJson(const ProcessingRecipe& recipe,
	std::string& json, std::string& errorMessage) {
	if (!ValidRecipeName(recipe.name)) {
		errorMessage = "recipe name must contain 1 to 256 bytes without control characters";
		return false;
	}
	if (!ValidPreset(recipe.preset)) {
		errorMessage = "recipe contains picture-processing values outside their supported ranges";
		return false;
	}
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << std::setprecision(17);
	output << "{\n  \"format\": \"jpegview-linux-processing-recipe\",\n"
		<< "  \"version\": 1,\n  \"name\": ";
	AppendJsonString(output, recipe.name);
	output << ",\n  \"auto_contrast\": " << (recipe.preset.autoContrast ? "true" : "false")
		<< ",\n  \"local_density_enabled\": " <<
		(recipe.preset.processing.localDensityEnabled ? "true" : "false");
	for (const NumberField& field : kNumberFields) {
		output << ",\n  \"" << field.key << "\": " << recipe.preset.processing.*(field.member);
	}
	output << "\n}\n";
	json = output.str();
	errorMessage.clear();
	return true;
}

bool ParseProcessingRecipeJson(const std::string& json,
	ProcessingRecipe& recipe, std::string& errorMessage) {
	if (json.size() > kMaximumRecipeBytes) {
		errorMessage = "processing recipe is larger than 64 KiB";
		return false;
	}
	std::map<std::string, JsonScalar> fields;
	JsonObjectParser parser(json);
	if (!parser.Parse(fields)) {
		errorMessage = "invalid processing recipe JSON: " + parser.Error();
		return false;
	}
	const JsonScalar* value = nullptr;
	if (!RequireField(fields, "format", ScalarKind::String, value, errorMessage)) return false;
	if (value->stringValue != "jpegview-linux-processing-recipe") {
		errorMessage = "unsupported processing recipe format";
		return false;
	}
	if (!RequireField(fields, "version", ScalarKind::Number, value, errorMessage)) return false;
	if (value->numberValue != 1.0) {
		errorMessage = "unsupported processing recipe version";
		return false;
	}
	ProcessingRecipe parsed;
	if (!RequireField(fields, "name", ScalarKind::String, value, errorMessage)) return false;
	parsed.name = value->stringValue;
	if (!ValidRecipeName(parsed.name)) {
		errorMessage = "processing recipe has an invalid name";
		return false;
	}
	if (!RequireField(fields, "auto_contrast", ScalarKind::Boolean, value, errorMessage)) return false;
	parsed.preset.autoContrast = value->booleanValue;
	if (!RequireField(fields, "local_density_enabled", ScalarKind::Boolean, value, errorMessage)) return false;
	parsed.preset.processing.localDensityEnabled = value->booleanValue;
	for (const NumberField& field : kNumberFields) {
		if (!RequireField(fields, field.key, ScalarKind::Number, value, errorMessage)) return false;
		parsed.preset.processing.*(field.member) = value->numberValue;
	}
	if (fields.size() != kNumberFields.size() + 5) {
		errorMessage = "processing recipe contains unknown fields";
		return false;
	}
	if (!ValidPreset(parsed.preset)) {
		errorMessage = "processing recipe contains picture-processing values outside their supported ranges";
		return false;
	}
	recipe = std::move(parsed);
	errorMessage.clear();
	return true;
}

bool ReadProcessingRecipeJson(const fs::path& filename,
	ProcessingRecipe& recipe, std::string& errorMessage,
	const std::function<bool()>& shouldContinue) {
	if (!Continue(shouldContinue)) {
		errorMessage = "processing recipe read was cancelled";
		return false;
	}
	std::ifstream input(filename, std::ios::binary);
	if (!input) {
		errorMessage = "cannot open processing recipe";
		return false;
	}
	std::string json;
	char buffer[4096];
	while (input) {
		if (!Continue(shouldContinue)) {
			errorMessage = "processing recipe read was cancelled";
			return false;
		}
		input.read(buffer, sizeof(buffer));
		const std::streamsize count = input.gcount();
		if (count > 0) {
			if (json.size() + static_cast<std::size_t>(count) > kMaximumRecipeBytes) {
				errorMessage = "processing recipe is larger than 64 KiB";
				return false;
			}
			json.append(buffer, static_cast<std::size_t>(count));
		}
	}
	if (!Continue(shouldContinue)) {
		errorMessage = "processing recipe read was cancelled";
		return false;
	}
	if (!input.eof()) {
		errorMessage = "cannot read processing recipe";
		return false;
	}
	return ParseProcessingRecipeJson(json, recipe, errorMessage);
}

bool WriteProcessingRecipeJson(const fs::path& filename,
	const ProcessingRecipe& recipe, bool overwriteConfirmed,
	std::string& errorMessage,
	const std::function<bool()>& shouldContinue, bool* overwriteRequired) {
	if (overwriteRequired != nullptr) *overwriteRequired = false;
	if (!Continue(shouldContinue)) {
		errorMessage = "processing recipe write was cancelled";
		return false;
	}
	if (filename.empty()) {
		errorMessage = "processing recipe output path is empty";
		return false;
	}
	std::string contents;
	if (!SerializeProcessingRecipeJson(recipe, contents, errorMessage)) return false;
	static std::atomic<std::uint64_t> sequence{0};
	const fs::path directory = filename.parent_path().empty() ? fs::path(".") : filename.parent_path();
	fs::path temporary;
	int descriptor = -1;
	for (int attempt = 0; attempt < 128; ++attempt) {
		const auto serial = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
		temporary = directory / (".jpegview-processing-recipe-" +
			std::to_string(static_cast<long long>(::getpid())) + "-" +
			std::to_string(serial) + ".tmp");
		descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
		if (descriptor >= 0 || errno != EEXIST) break;
	}
	if (descriptor < 0) {
		errorMessage = "cannot create a private recipe temporary: " +
			std::string(std::strerror(errno));
		return false;
	}
	struct stat temporaryStatus{};
	if (::fstat(descriptor, &temporaryStatus) != 0 || ::fchmod(descriptor, 0600) != 0) {
		errorMessage = "cannot keep recipe temporary private: " +
			std::string(std::strerror(errno));
		(void)::close(descriptor);
		(void)::unlink(temporary.c_str());
		return false;
	}
	mode_t outputMode = temporaryStatus.st_mode & 0777;
	struct stat targetStatus{};
	if (::lstat(filename.c_str(), &targetStatus) == 0) {
		if (S_ISREG(targetStatus.st_mode)) outputMode = targetStatus.st_mode & 07777;
	} else if (errno != ENOENT) {
		errorMessage = "cannot inspect recipe output: " + std::string(std::strerror(errno));
		(void)::close(descriptor);
		(void)::unlink(temporary.c_str());
		return false;
	}
	bool success = WriteAll(descriptor, contents, errorMessage, shouldContinue);
	if (success && !Continue(shouldContinue)) {
		errorMessage = "processing recipe write was cancelled";
		success = false;
	}
	if (success && ::fsync(descriptor) != 0) {
		errorMessage = "cannot flush processing recipe: " + std::string(std::strerror(errno));
		success = false;
	}
	if (success && !Continue(shouldContinue)) {
		errorMessage = "processing recipe write was cancelled";
		success = false;
	}
	if (success && ::fchmod(descriptor, outputMode) != 0) {
		errorMessage = "cannot set recipe permissions: " + std::string(std::strerror(errno));
		success = false;
	}
	if (success && !Continue(shouldContinue)) {
		errorMessage = "processing recipe write was cancelled";
		success = false;
	}
	if (success && ::fsync(descriptor) != 0) {
		errorMessage = "cannot flush recipe permissions: " + std::string(std::strerror(errno));
		success = false;
	}
	if (::close(descriptor) != 0 && success) {
		errorMessage = "cannot close processing recipe: " + std::string(std::strerror(errno));
		success = false;
	}
	if (success && !Continue(shouldContinue)) {
		errorMessage = "processing recipe write was cancelled";
		success = false;
	}
	if (success) {
		bool outputAppeared = false;
		success = overwriteConfirmed ? (::rename(temporary.c_str(), filename.c_str()) == 0) :
			PublishNoReplace(temporary, filename, errorMessage, outputAppeared);
		if (!success && overwriteRequired != nullptr) *overwriteRequired = outputAppeared;
		if (!success && errorMessage.empty()) {
			errorMessage = "cannot publish processing recipe: " +
				std::string(std::strerror(errno));
		}
	}
	if (!success) (void)::unlink(temporary.c_str());
	else errorMessage.clear();
	return success;
}

} // namespace jpegview_linux
