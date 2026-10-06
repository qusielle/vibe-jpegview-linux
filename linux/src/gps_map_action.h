#pragma once

#include <string>
#include <string_view>

namespace jpegview_linux {

inline constexpr char kDefaultGpsMapProviderUrl[] =
	"https://opentopomap.org/#marker=15/{lat}/{lng}";

bool IsValidGpsMapProviderUrlTemplate(std::string_view provider);
bool AreGpsCoordinatesValid(double latitude, double longitude);

// Returns an empty string when the provider or coordinates are invalid.
std::string BuildGpsMapUrl(std::string_view provider, double latitude,
	double longitude);

} // namespace jpegview_linux
