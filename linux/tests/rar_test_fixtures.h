#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

// Small CC0 archives from https://github.com/ssokolow/rar-test-files. The RAR4 archive is
// non-solid; the RAR5 archive is solid and contains both testfile.jpg and testfile.png.
namespace rar_test_fixtures {

inline constexpr std::string_view kRar4ImageArchiveBase64 =
	"UmFyIRoHAM+QcwAADQAAAAAAAAAMJXQggCwAtgAAANwAAAAAbLFw2gAAISodNQwAIAAAAHRlc3RmaWxlLmpwZ+cYFf7V/ydkeNQh1MKmm6OAexVPlvVHrzqPE5mVHC08ghs85LfafCcldrlGYJPnjkgNzK9t4fZEpePKwmfeq9nqNRVssG8auWw3ppmChio3P4QobbIIu+aDvAnlNhtw7eU/yPyMBuPEssZjwTehsh4DZgC5HXWGFUVxgDrn+6KnVDXP/2B26ds102b/eZa5elob/BycnuXvN92AXobBxHJ4Ebq+7rCITbK7Lz6UAAC/iGf2qf/UUW50IIAsAFQAAABXAAAAAGKssK8AACEqHTUMACAAAAB0ZXN0ZmlsZS5wbmenGIjF+7VC0fPe1feyyXAlT4G/SVtSdAyd7pMHsE3FAkIqbrYBRgyQp7m1pxzv+HOHpfwCaeA8jA41QCxmUCvsyGsqR5gONgQCwAAAAL+IZ/ap/9TEPXsAQAcA";

inline constexpr std::string_view kRar5SolidImageArchiveBase64 =
	"UmFyIRoHAQAgtvoRCgEFBgQFAQGAgABGxEvRIgIC7AEG3AG2gwLQDlA6bLFw2oAdAQx0ZXN0ZmlsZS5qcGfEd+knZURDL1cESsmZlt5eZkl3MRgQQeEoIogjHMi4iQxSamImCiDodCCCpNi6ngLqQdS7HgaCC8AQkBkf4GbXU7HUlgpKq9U4xd3X8X6vV6q5NXxRVUVRXjx99r/cXXeXgBOrXlrg00AD4nL9g0gboM4x7f/OGc/s5jOmeOhAs88iClAh+i+MsKoaNJKVJEKnFTTEU6V+APnCgPRW1Cp5BjsXcKchclIjOiTFpCWOPHE4w3y/USMoaAZVup59/47829uo/75xuLM6r21duXLv12+PPaNn+mzuZfHV4bW90jvgcDrkc2upUX2u+NMc19ciAgK+AAbXALaDAtAOUDpirLCvwB0BDHRlc3RmaWxlLnBuZ0UkO+iaxpwoqoJahl1I9hipjHbJ8m/+5BQNNrWZCita+y/1/BSwSx6Nnla3Z53KxfQbzcHIy4CWSxH3Vp+YHXdWUQMFBAA=";

inline std::vector<std::uint8_t> DecodeBase64(std::string_view encoded) {
	static constexpr std::string_view alphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::vector<std::uint8_t> decoded;
	decoded.reserve(encoded.size() * 3 / 4);
	std::uint32_t buffer = 0;
	int bits = -8;
	for (const char character : encoded) {
		if (character == '=') break;
		const std::size_t value = alphabet.find(character);
		if (value == std::string_view::npos) return {};
		buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
		bits += 6;
		if (bits >= 0) {
			decoded.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xffu));
			bits -= 8;
		}
	}
	return decoded;
}

inline std::vector<std::uint8_t> Rar4ImageArchive() {
	return DecodeBase64(kRar4ImageArchiveBase64);
}

inline std::vector<std::uint8_t> Rar5SolidImageArchive() {
	return DecodeBase64(kRar5SolidImageArchiveBase64);
}

} // namespace rar_test_fixtures
