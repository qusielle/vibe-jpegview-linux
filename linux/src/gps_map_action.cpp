#include "gps_map_action.h"

#include <cmath>
#include <cctype>
#include <iomanip>
#include <locale>
#include <sstream>

namespace jpegview_linux {
namespace {

constexpr std::size_t kMaximumProviderTemplateBytes = 2048;
constexpr std::size_t kMaximumGpsMapUrlBytes = 4096;

bool StartsWithInsensitive(std::string_view value, std::string_view prefix) {
	if (value.size() < prefix.size()) return false;
	for (std::size_t index = 0; index < prefix.size(); ++index) {
		const unsigned char character = static_cast<unsigned char>(value[index]);
		if (static_cast<char>(std::tolower(character)) != prefix[index]) return false;
	}
	return true;
}

bool HasPlaceholder(std::string_view provider, std::string_view placeholder) {
	return provider.find(placeholder) != std::string_view::npos;
}

bool IsValidPort(std::string_view port) {
	if (port.empty()) return false;
	unsigned int value = 0;
	for (const char character : port) {
		if (character < '0' || character > '9') return false;
		value = value * 10 + static_cast<unsigned int>(character - '0');
		if (value > 65535) return false;
	}
	return value > 0;
}

bool IsValidHostName(std::string_view host) {
	if (host.empty() || host.front() == '.' || host.find("..") != std::string_view::npos) {
		return false;
	}
	for (std::size_t index = 0; index < host.size(); ++index) {
		const unsigned char character = static_cast<unsigned char>(host[index]);
		const bool alphanumeric = (character >= 'a' && character <= 'z') ||
			(character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9');
		if (!alphanumeric && character != '-' && character != '.' && character != '_') {
			return false;
		}
		if (character == '-' && (index == 0 || host[index - 1] == '.' ||
			index + 1 == host.size() || host[index + 1] == '.')) return false;
	}
	return true;
}

bool IsValidAuthority(std::string_view authority) {
	if (authority.empty() || authority.find('@') != std::string_view::npos ||
		authority.find_first_of("{}") != std::string_view::npos) return false;
	if (authority.front() == '[') {
		const std::size_t close = authority.find(']');
		if (close == std::string_view::npos || close == 1) return false;
		const std::string_view address = authority.substr(1, close - 1);
		if (address.find(':') == std::string_view::npos) return false;
		for (const unsigned char character : address) {
			const bool hexadecimal = (character >= '0' && character <= '9') ||
				(character >= 'a' && character <= 'f') ||
				(character >= 'A' && character <= 'F');
			if (!hexadecimal && character != ':' && character != '.') return false;
		}
		const std::string_view suffix = authority.substr(close + 1);
		return suffix.empty() || (suffix.front() == ':' &&
			IsValidPort(suffix.substr(1)));
	}

	const std::size_t colon = authority.find(':');
	const std::string_view host = colon == std::string_view::npos ? authority :
		authority.substr(0, colon);
	if (!IsValidHostName(host)) return false;
	if (colon == std::string_view::npos) return true;
	if (authority.find(':', colon + 1) != std::string_view::npos) return false;
	return IsValidPort(authority.substr(colon + 1));
}

std::string FormatCoordinate(double coordinate) {
	if (coordinate == 0.0) coordinate = 0.0;
	std::ostringstream formatted;
	formatted.imbue(std::locale::classic());
	formatted << std::fixed << std::setprecision(5) << coordinate;
	return formatted.str();
}

void ReplaceAll(std::string& value, std::string_view placeholder,
	std::string_view replacement) {
	std::size_t position = 0;
	while ((position = value.find(placeholder, position)) != std::string::npos) {
		value.replace(position, placeholder.size(), replacement);
		position += replacement.size();
	}
}

} // namespace

bool IsValidGpsMapProviderUrlTemplate(std::string_view provider) {
	if (provider.empty() || provider.size() > kMaximumProviderTemplateBytes ||
		(!StartsWithInsensitive(provider, "https://") &&
			!StartsWithInsensitive(provider, "http://"))) return false;
	const std::size_t schemeLength = StartsWithInsensitive(provider, "https://") ? 8 : 7;
	const std::size_t authorityEnd = provider.find_first_of("/?#", schemeLength);
	const std::size_t authorityLength = (authorityEnd == std::string_view::npos ?
		provider.size() : authorityEnd) - schemeLength;
	if (!IsValidAuthority(provider.substr(schemeLength, authorityLength)) ||
		!HasPlaceholder(provider, "{lat}") ||
		!HasPlaceholder(provider, "{lng}")) return false;

	for (std::size_t index = 0; index < provider.size(); ++index) {
		const unsigned char character = static_cast<unsigned char>(provider[index]);
		if (character < 0x20 || character == 0x7f || std::isspace(character) != 0 ||
			character == '\\') return false;
		if (provider[index] == '{') {
			if (provider.compare(index, 5, "{lat}") == 0) index += 4;
			else if (provider.compare(index, 5, "{lng}") == 0) index += 4;
			else return false;
		} else if (provider[index] == '}') {
			return false;
		}
	}
	return true;
}

bool AreGpsCoordinatesValid(double latitude, double longitude) {
	return std::isfinite(latitude) && std::isfinite(longitude) &&
		latitude >= -90.0 && latitude <= 90.0 &&
		longitude >= -180.0 && longitude <= 180.0;
}

std::string BuildGpsMapUrl(std::string_view provider, double latitude,
	double longitude) {
	if (!IsValidGpsMapProviderUrlTemplate(provider) ||
		!AreGpsCoordinatesValid(latitude, longitude)) return {};
	std::string url(provider);
	ReplaceAll(url, "{lat}", FormatCoordinate(latitude));
	ReplaceAll(url, "{lng}", FormatCoordinate(longitude));
	if (url.size() > kMaximumGpsMapUrlBytes) return {};
	return url;
}

} // namespace jpegview_linux
