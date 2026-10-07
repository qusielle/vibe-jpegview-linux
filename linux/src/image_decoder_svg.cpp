#include "image_decoder_internal.h"

#include "image_formats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

#if JPEGVIEW_HAVE_SVG
#include <cairo.h>
#include <gio/gio.h>
#include <libxml/parser.h>
#include <librsvg/rsvg.h>
#endif

namespace jpegview_linux::decoder_detail {
namespace {

constexpr std::size_t kMaximumEncodedSvgBytes = 32u * 1024u * 1024u;
constexpr std::size_t kMaximumDecodedSvgBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaximumSvgNodes = 250000;
constexpr std::size_t kMaximumSvgDepth = 256;
constexpr std::uint64_t kMaximumSvgPixels = 100ull * 1024ull * 1024ull;
constexpr int kMaximumSvgDimension = 65535;

bool Continue(const WorkContext& context) {
	return context.Continue();
}

[[maybe_unused]] bool ReadEncodedFile(const std::filesystem::path& filename,
	std::vector<std::uint8_t>& bytes, std::string& errorMessage,
	const WorkContext& context) {
	std::ifstream input(filename, std::ios::binary);
	if (!input) {
		errorMessage = "cannot open SVG source";
		return false;
	}
	input.seekg(0, std::ios::end);
	const std::streamoff fileSize = input.tellg();
	if (fileSize <= 0) {
		errorMessage = "empty SVG source";
		return false;
	}
	if (static_cast<std::uint64_t>(fileSize) > kMaximumEncodedSvgBytes) {
		errorMessage = "SVG source exceeds the encoded size limit";
		return false;
	}
	if (!Continue(context)) {
		errorMessage = "source work was cancelled";
		return false;
	}
	try {
		bytes.resize(static_cast<std::size_t>(fileSize));
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	input.seekg(0, std::ios::beg);
	constexpr std::size_t chunkSize = 64u * 1024u;
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		if (!Continue(context)) {
			errorMessage = "source work was cancelled";
			return false;
		}
		const std::size_t count = std::min(chunkSize, bytes.size() - offset);
		if (!input.read(reinterpret_cast<char*>(bytes.data() + offset),
			static_cast<std::streamsize>(count))) {
			errorMessage = "could not read complete SVG source";
			return false;
		}
		offset += count;
	}
	return Continue(context);
}

[[maybe_unused]] bool InflateSvgz(const std::vector<std::uint8_t>& encoded,
	std::vector<std::uint8_t>& xml, std::string& errorMessage,
	const WorkContext& context) {
	if (encoded.size() < 2 || encoded[0] != 0x1f || encoded[1] != 0x8b ||
		encoded.size() > std::numeric_limits<uInt>::max()) {
		errorMessage = "invalid SVGZ gzip header";
		return false;
	}
	z_stream stream{};
	if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
		errorMessage = "could not initialize SVGZ decompression";
		return false;
	}
	struct InflateEnd {
		z_stream& stream;
		~InflateEnd() { inflateEnd(&stream); }
	} cleanup{stream};
	stream.next_in = const_cast<Bytef*>(
		reinterpret_cast<const Bytef*>(encoded.data()));
	stream.avail_in = static_cast<uInt>(encoded.size());
	std::array<std::uint8_t, 16384> output{};
	for (;;) {
		if (!Continue(context)) {
			errorMessage = "source work was cancelled";
			return false;
		}
		stream.next_out = output.data();
		stream.avail_out = static_cast<uInt>(output.size());
		const int status = inflate(&stream, Z_NO_FLUSH);
		const std::size_t written = output.size() - stream.avail_out;
		if (written > kMaximumDecodedSvgBytes - xml.size()) {
			errorMessage = "SVGZ source exceeds the decoded size limit";
			return false;
		}
		try {
			xml.insert(xml.end(), output.begin(), output.begin() +
				static_cast<std::ptrdiff_t>(written));
		} catch (const std::exception&) {
			errorMessage = "out of memory";
			return false;
		}
		if (status == Z_STREAM_END) {
			if (stream.avail_in != 0) {
				errorMessage = "SVGZ contains trailing or concatenated gzip data";
				return false;
			}
			break;
		}
		if (status != Z_OK || (written == 0 && stream.avail_in == 0)) {
			errorMessage = "invalid or truncated SVGZ gzip stream";
			return false;
		}
	}
	if (xml.empty()) {
		errorMessage = "empty SVGZ document";
		return false;
	}
	return Continue(context);
}

#if JPEGVIEW_HAVE_SVG

struct XmlDocDeleter {
	void operator()(xmlDoc* value) const { if (value != nullptr) xmlFreeDoc(value); }
};

struct GObjectDeleter {
	template <typename T>
	void operator()(T* value) const {
		if (value != nullptr) g_object_unref(value);
	}
};

struct GErrorDeleter {
	void operator()(GError* value) const { if (value != nullptr) g_error_free(value); }
};

using XmlDocPtr = std::unique_ptr<xmlDoc, XmlDocDeleter>;
using RsvgHandlePtr = std::unique_ptr<RsvgHandle, GObjectDeleter>;
using InputStreamPtr = std::unique_ptr<GInputStream, GObjectDeleter>;
using CairoSurfacePtr = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using CairoContextPtr = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;

std::string LowerAscii(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) {
			return static_cast<char>(character >= 'A' && character <= 'Z' ?
				character + ('a' - 'A') : character);
		});
	return value;
}

bool IsWhitespace(char value) {
	return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool ValidateCssReferences(const std::string& css) {
	std::string normalized;
	try {
		normalized.reserve(css.size());
	} catch (const std::exception&) {
		return false;
	}
	char quote = 0;
	for (std::size_t index = 0; index < css.size(); ++index) {
		const char value = css[index];
		if (value == '\\') return false;
		if (quote != 0) {
			if (value == quote) quote = 0;
			normalized += value;
			continue;
		}
		if (value == '\'' || value == '"') {
			quote = value;
			normalized += value;
			continue;
		}
		if (value == '/' && index + 1 < css.size() && css[index + 1] == '*') {
			const std::size_t end = css.find("*/", index + 2);
			if (end == std::string::npos) return false;
			index = end + 1;
			continue;
		}
		normalized += value;
	}
	if (quote != 0) return false;
	normalized = LowerAscii(std::move(normalized));
	if (normalized.find("@import") != std::string::npos) return false;
	for (std::size_t position = 0;
		(position = normalized.find("url", position)) != std::string::npos;) {
		std::size_t open = position + 3;
		while (open < normalized.size() && IsWhitespace(normalized[open])) ++open;
		if (open >= normalized.size() || normalized[open] != '(') {
			position += 3;
			continue;
		}
		std::size_t begin = open + 1;
		while (begin < normalized.size() && IsWhitespace(normalized[begin])) ++begin;
		if (begin >= normalized.size()) return false;
		std::string_view reference;
		std::size_t close = std::string::npos;
		if (normalized[begin] == '\'' || normalized[begin] == '"') {
			const char cssQuote = normalized[begin++];
			close = normalized.find(cssQuote, begin);
			if (close == std::string::npos) return false;
			reference = std::string_view(normalized).substr(begin, close - begin);
			++close;
			while (close < normalized.size() && IsWhitespace(normalized[close])) ++close;
			if (close >= normalized.size() || normalized[close] != ')') return false;
		} else {
			close = normalized.find(')', begin);
			if (close == std::string::npos) return false;
			std::size_t end = close;
			while (end > begin && IsWhitespace(normalized[end - 1])) --end;
			reference = std::string_view(normalized).substr(begin, end - begin);
		}
		if (reference.size() < 2 || reference.front() != '#' ||
			std::any_of(reference.begin(), reference.end(), IsWhitespace)) return false;
		position = close + 1;
	}
	return true;
}

bool IsLocalFragment(const std::string& href) {
	return href.size() > 1 && href.front() == '#' &&
		std::none_of(href.begin(), href.end(), IsWhitespace);
}

bool ValidateSvgNodeTree(xmlDoc* document, xmlNode* root, std::string& errorMessage,
	const WorkContext& context) {
	struct PendingNode {
		xmlNode* node = nullptr;
		std::size_t depth = 0;
	};
	std::vector<PendingNode> pending;
	try {
		pending.push_back({root, 1});
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	std::size_t nodeCount = 0;
	while (!pending.empty()) {
		if (!Continue(context)) {
			errorMessage = "source work was cancelled";
			return false;
		}
		const PendingNode current = pending.back();
		pending.pop_back();
		if (++nodeCount > kMaximumSvgNodes || current.depth > kMaximumSvgDepth) {
			errorMessage = "SVG document exceeds the structural complexity limit";
			return false;
		}
		xmlNode* node = current.node;
		if (node->type == XML_PI_NODE && node->name != nullptr &&
			xmlStrEqual(node->name, BAD_CAST "xml-stylesheet")) {
			errorMessage = "external SVG stylesheets are not supported";
			return false;
		}
		if (node->type == XML_ENTITY_REF_NODE || node->type == XML_ENTITY_DECL) {
			errorMessage = "SVG entity references are not supported";
			return false;
		}
		if (node->type == XML_ELEMENT_NODE) {
			const std::string localName = node->name == nullptr ? std::string{} :
				reinterpret_cast<const char*>(node->name);
			if (localName == "script") {
				errorMessage = "SVG scripts are not supported";
				return false;
			}
			if (node->ns != nullptr && node->ns->href != nullptr &&
				xmlStrEqual(node->ns->href, BAD_CAST "http://www.w3.org/2001/XInclude") &&
				localName == "include") {
				errorMessage = "SVG XInclude elements are not supported";
				return false;
			}
			for (xmlAttr* attribute = node->properties; attribute != nullptr;
				attribute = attribute->next) {
				const std::string attributeName = attribute->name == nullptr ? std::string{} :
					reinterpret_cast<const char*>(attribute->name);
				if (attribute->ns != nullptr && attribute->ns->href != nullptr &&
					xmlStrEqual(attribute->ns->href, BAD_CAST "http://www.w3.org/XML/1998/namespace") &&
					attributeName == "base") {
					errorMessage = "SVG xml:base references are not supported";
					return false;
				}
				xmlChar* raw = xmlNodeListGetString(document, attribute->children, 1);
				const std::string value = raw == nullptr ? std::string{} :
					reinterpret_cast<const char*>(raw);
				if (raw != nullptr) xmlFree(raw);
				if (attributeName == "href" && !IsLocalFragment(value)) {
					errorMessage = "external SVG references are not supported";
					return false;
				}
				if (attributeName != "href" && !ValidateCssReferences(value)) {
					errorMessage = "external SVG CSS references are not supported";
					return false;
				}
			}
			if (localName == "style" && node->children != nullptr) {
				xmlChar* raw = xmlNodeGetContent(node);
				const std::string css = raw == nullptr ? std::string{} :
					reinterpret_cast<const char*>(raw);
				if (raw != nullptr) xmlFree(raw);
				if (!ValidateCssReferences(css)) {
					errorMessage = "external SVG CSS references are not supported";
					return false;
				}
			}
		}
		if (node->children != nullptr) {
			try {
				for (xmlNode* child = node->children; child != nullptr; child = child->next) {
					pending.push_back({child, current.depth + 1});
				}
			} catch (const std::exception&) {
				errorMessage = "out of memory";
				return false;
			}
		}
	}
	return true;
}

bool ValidateSvgXml(const std::vector<std::uint8_t>& xml, std::string& errorMessage,
	const WorkContext& context) {
	if (xml.empty() || xml.size() > kMaximumDecodedSvgBytes ||
		xml.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		errorMessage = "SVG document size is not supported";
		return false;
	}
	if (!Continue(context)) {
		errorMessage = "source work was cancelled";
		return false;
	}
	const int options = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
		XML_PARSE_COMPACT;
	XmlDocPtr document(xmlReadMemory(reinterpret_cast<const char*>(xml.data()),
		static_cast<int>(xml.size()), nullptr, nullptr, options));
	if (!document) {
		errorMessage = "malformed SVG XML";
		return false;
	}
	if (document->intSubset != nullptr || document->extSubset != nullptr) {
		errorMessage = "SVG document type declarations and entities are not supported";
		return false;
	}
	for (xmlNode* topLevel = document->children; topLevel != nullptr;
		topLevel = topLevel->next) {
		if (topLevel->type == XML_PI_NODE && topLevel->name != nullptr &&
			xmlStrEqual(topLevel->name, BAD_CAST "xml-stylesheet")) {
			errorMessage = "external SVG stylesheets are not supported";
			return false;
		}
	}
	xmlNode* root = xmlDocGetRootElement(document.get());
	if (root == nullptr || root->type != XML_ELEMENT_NODE || root->name == nullptr ||
		!xmlStrEqual(root->name, BAD_CAST "svg") ||
		(root->ns != nullptr && root->ns->href != nullptr &&
		 !xmlStrEqual(root->ns->href, BAD_CAST "http://www.w3.org/2000/svg"))) {
		errorMessage = "XML document root is not an SVG element";
		return false;
	}
	return ValidateSvgNodeTree(document.get(), root, errorMessage, context);
}

bool LengthInPixels(const RsvgLength& length, double& pixels) {
	double scale = 0.0;
	switch (length.unit) {
	case RSVG_UNIT_PX: scale = 1.0; break;
	case RSVG_UNIT_IN: scale = 96.0; break;
	case RSVG_UNIT_CM: scale = 96.0 / 2.54; break;
	case RSVG_UNIT_MM: scale = 96.0 / 25.4; break;
	case RSVG_UNIT_PT: scale = 96.0 / 72.0; break;
	case RSVG_UNIT_PC: scale = 16.0; break;
	case RSVG_UNIT_PERCENT: return false;
	case RSVG_UNIT_EM:
	case RSVG_UNIT_EX:
#if LIBRSVG_CHECK_VERSION(2, 58, 0)
	case RSVG_UNIT_CH:
#endif
		return false;
	default: return false;
	}
	pixels = length.length * scale;
	return std::isfinite(pixels) && pixels > 0.0;
}

bool SetSvgDimensions(double pixelWidth, double pixelHeight, int& width,
	int& height, std::string& errorMessage) {
	if (!std::isfinite(pixelWidth) || !std::isfinite(pixelHeight) ||
		pixelWidth <= 0.0 || pixelHeight <= 0.0 ||
		pixelWidth > kMaximumSvgDimension || pixelHeight > kMaximumSvgDimension) {
		errorMessage = "SVG dimensions are not supported";
		return false;
	}
	const int roundedWidth = static_cast<int>(std::ceil(pixelWidth));
	const int roundedHeight = static_cast<int>(std::ceil(pixelHeight));
	if (static_cast<std::uint64_t>(roundedWidth) * roundedHeight > kMaximumSvgPixels) {
		errorMessage = "SVG image is too large";
		return false;
	}
	width = roundedWidth;
	height = roundedHeight;
	return true;
}

#if !LIBRSVG_CHECK_VERSION(2, 52, 0)
bool HasFontRelativeUnit(const RsvgLength& length) {
	return length.unit == RSVG_UNIT_EM || length.unit == RSVG_UNIT_EX;
}
#endif

bool GetSvgDimensions(RsvgHandle* handle, int& width, int& height,
	std::string& errorMessage) {
	gboolean hasWidth = FALSE;
	gboolean hasHeight = FALSE;
	gboolean hasViewBox = FALSE;
	RsvgLength intrinsicWidth{};
	RsvgLength intrinsicHeight{};
	RsvgRectangle viewBox{};
	rsvg_handle_get_intrinsic_dimensions(handle, &hasWidth, &intrinsicWidth,
		&hasHeight, &intrinsicHeight, &hasViewBox, &viewBox);
	#if LIBRSVG_CHECK_VERSION(2, 52, 0)
	double resolvedWidth = 0.0;
	double resolvedHeight = 0.0;
	if (rsvg_handle_get_intrinsic_size_in_pixels(handle, &resolvedWidth,
		&resolvedHeight)) {
		return SetSvgDimensions(resolvedWidth, resolvedHeight, width, height,
			errorMessage);
	}
	#else
	if ((hasWidth && HasFontRelativeUnit(intrinsicWidth)) ||
		(hasHeight && HasFontRelativeUnit(intrinsicHeight))) {
		RsvgDimensionData legacyDimensions{};
		rsvg_handle_get_dimensions(handle, &legacyDimensions);
		if (legacyDimensions.width > 0 && legacyDimensions.height > 0) {
			return SetSvgDimensions(legacyDimensions.width, legacyDimensions.height,
				width, height, errorMessage);
		}
	}
	#endif
	double pixelWidth = 0.0;
	double pixelHeight = 0.0;
	const bool widthKnown = hasWidth && LengthInPixels(intrinsicWidth, pixelWidth);
	const bool heightKnown = hasHeight && LengthInPixels(intrinsicHeight, pixelHeight);
	const bool validViewBox = hasViewBox && std::isfinite(viewBox.width) &&
		std::isfinite(viewBox.height) && viewBox.width > 0.0 && viewBox.height > 0.0;
	if (validViewBox) {
		if (widthKnown && !heightKnown) {
			pixelHeight = pixelWidth * viewBox.height / viewBox.width;
		} else if (!widthKnown && heightKnown) {
			pixelWidth = pixelHeight * viewBox.width / viewBox.height;
		} else if (!widthKnown && !heightKnown) {
			pixelWidth = viewBox.width;
			pixelHeight = viewBox.height;
		}
	} else {
		if (!widthKnown) pixelWidth = 300.0;
		if (!heightKnown) pixelHeight = 150.0;
	}
	return SetSvgDimensions(pixelWidth, pixelHeight, width, height, errorMessage);
}

bool LoadSvgHandle(const std::filesystem::path& filename,
	RsvgHandlePtr& handle, int& width, int& height,
	std::string& errorMessage, const WorkContext& context) {
	std::vector<std::uint8_t> encoded;
	if (!ReadEncodedFile(filename, encoded, errorMessage, context)) return false;
	std::vector<std::uint8_t> xml;
	if (encoded.size() >= 2 && encoded[0] == 0x1f && encoded[1] == 0x8b) {
		if (!InflateSvgz(encoded, xml, errorMessage, context)) return false;
	} else {
		xml = std::move(encoded);
	}
	if (!ValidateSvgXml(xml, errorMessage, context)) return false;
	GInputStream* memoryStream = g_memory_input_stream_new_from_data(
		xml.data(), static_cast<gssize>(xml.size()), nullptr);
	if (memoryStream == nullptr) {
		errorMessage = "could not create SVG input stream";
		return false;
	}
	InputStreamPtr stream(G_INPUT_STREAM(memoryStream));
	GError* rawError = nullptr;
	RsvgHandle* rawHandle = rsvg_handle_new_from_stream_sync(stream.get(), nullptr,
		RSVG_HANDLE_FLAGS_NONE, nullptr, &rawError);
	std::unique_ptr<GError, GErrorDeleter> parseError(rawError);
	if (rawHandle == nullptr) {
		errorMessage = parseError && parseError->message != nullptr ?
			parseError->message : "librsvg rejected the SVG document";
		return false;
	}
	handle.reset(rawHandle);
	rsvg_handle_set_dpi(handle.get(), 96.0);
	if (!GetSvgDimensions(handle.get(), width, height, errorMessage)) return false;
	return Continue(context);
}

bool FitSvgTarget(int sourceWidth, int sourceHeight, int targetWidth,
	int targetHeight, int& outputWidth, int& outputHeight, bool allowUpscale) {
	if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) {
		return false;
	}
	const double scale = std::min(static_cast<double>(targetWidth) / sourceWidth,
		static_cast<double>(targetHeight) / sourceHeight);
	const double boundedScale = allowUpscale ? scale : std::min(1.0, scale);
	if (!std::isfinite(boundedScale) || boundedScale <= 0.0) return false;
	outputWidth = std::max(1, static_cast<int>(std::floor(sourceWidth * boundedScale + 0.5)));
	outputHeight = std::max(1, static_cast<int>(std::floor(sourceHeight * boundedScale + 0.5)));
	while (outputWidth > targetWidth || outputHeight > targetHeight) {
		if (outputWidth > targetWidth) --outputWidth;
		if (outputHeight > targetHeight) --outputHeight;
	}
	return outputWidth <= kMaximumSvgDimension && outputHeight <= kMaximumSvgDimension &&
		static_cast<std::uint64_t>(outputWidth) * outputHeight <= kMaximumSvgPixels;
}

bool RenderSvg(RsvgHandle* handle, int width, int height,
	std::vector<std::uint8_t>& bgra, std::string& errorMessage,
	const WorkContext& context) {
	if (!ValidDimensions(width, height, errorMessage) || !Continue(context)) {
		if (errorMessage.empty()) errorMessage = "source work was cancelled";
		return false;
	}
	const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
	if (stride <= 0 || static_cast<std::uint64_t>(stride) * height >
		std::numeric_limits<std::size_t>::max()) {
		errorMessage = "SVG raster dimensions are not supported";
		return false;
	}
	CairoSurfacePtr surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		width, height), &cairo_surface_destroy);
	if (!surface || cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
		errorMessage = "could not allocate SVG raster surface";
		return false;
	}
	CairoContextPtr drawing(cairo_create(surface.get()), &cairo_destroy);
	if (!drawing || cairo_status(drawing.get()) != CAIRO_STATUS_SUCCESS) {
		errorMessage = "could not create SVG rendering context";
		return false;
	}
	cairo_set_operator(drawing.get(), CAIRO_OPERATOR_CLEAR);
	cairo_paint(drawing.get());
	cairo_set_operator(drawing.get(), CAIRO_OPERATOR_OVER);
	const RsvgRectangle viewport{0.0, 0.0, static_cast<double>(width),
		static_cast<double>(height)};
	GError* rawError = nullptr;
	const gboolean rendered = rsvg_handle_render_document(handle, drawing.get(),
		&viewport, &rawError);
	std::unique_ptr<GError, GErrorDeleter> renderError(rawError);
	if (!rendered || cairo_status(drawing.get()) != CAIRO_STATUS_SUCCESS ||
		cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
		errorMessage = renderError && renderError->message != nullptr ?
			renderError->message : "librsvg could not render the document";
		return false;
	}
	if (!Continue(context)) {
		errorMessage = "source work was cancelled";
		return false;
	}
	cairo_surface_flush(surface.get());
	const unsigned char* pixels = cairo_image_surface_get_data(surface.get());
	const int actualStride = cairo_image_surface_get_stride(surface.get());
	if (pixels == nullptr || actualStride < width * 4) {
		errorMessage = "librsvg returned an invalid raster buffer";
		return false;
	}
	try {
		bgra.resize(static_cast<std::size_t>(width) * height * 4);
	} catch (const std::exception&) {
		errorMessage = "out of memory";
		return false;
	}
	for (int y = 0; y < height; ++y) {
		if ((y & 31) == 0 && !Continue(context)) {
			errorMessage = "source work was cancelled";
			return false;
		}
		const unsigned char* source = pixels + static_cast<std::size_t>(y) * actualStride;
		for (int x = 0; x < width; ++x) {
			const unsigned char* pixel = source + static_cast<std::size_t>(x) * 4;
			unsigned char blue = 0;
			unsigned char green = 0;
			unsigned char red = 0;
			unsigned char alpha = 0;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
			blue = pixel[0]; green = pixel[1]; red = pixel[2]; alpha = pixel[3];
#else
			alpha = pixel[0]; red = pixel[1]; green = pixel[2]; blue = pixel[3];
#endif
			const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
			if (alpha == 0) {
				bgra[offset] = bgra[offset + 1] = bgra[offset + 2] = 0;
			} else {
				const auto straight = [alpha](unsigned char channel) {
					return static_cast<std::uint8_t>(std::min(255u,
						(static_cast<unsigned int>(channel) * 255u + alpha / 2u) / alpha));
				};
				bgra[offset] = straight(blue);
				bgra[offset + 1] = straight(green);
				bgra[offset + 2] = straight(red);
			}
			bgra[offset + 3] = alpha;
		}
	}
	return Continue(context);
}

#endif // JPEGVIEW_HAVE_SVG

} // namespace

bool DecodeSvg(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage, const WorkContext& context) {
	image = {};
#if JPEGVIEW_HAVE_SVG
	RsvgHandlePtr handle;
	int width = 0;
	int height = 0;
	if (!LoadSvgHandle(filename, handle, width, height, errorMessage, context)) return false;
	std::vector<std::uint8_t> pixels;
	if (!RenderSvg(handle.get(), width, height, pixels, errorMessage, context)) return false;
	return AppendBGRA(image, width, height, std::move(pixels), 0, errorMessage, true);
#else
	(void)filename;
	(void)context;
	errorMessage = "SVG content detected, but SVG support is not available in this build";
	return false;
#endif
}

bool DecodeSvgForDisplay(const std::filesystem::path& filename, int targetWidth,
	int targetHeight, DecodedImage& image, int& sourceWidth, int& sourceHeight,
	std::string& errorMessage, const WorkContext& context, bool allowUpscale) {
	image = {};
	sourceWidth = sourceHeight = 0;
#if JPEGVIEW_HAVE_SVG
	if (targetWidth <= 0 || targetHeight <= 0) {
		errorMessage = "invalid SVG display target";
		return false;
	}
	RsvgHandlePtr handle;
	if (!LoadSvgHandle(filename, handle, sourceWidth, sourceHeight, errorMessage, context)) {
		sourceWidth = sourceHeight = 0;
		return false;
	}
	int outputWidth = 0;
	int outputHeight = 0;
	if (!FitSvgTarget(sourceWidth, sourceHeight, targetWidth, targetHeight,
		outputWidth, outputHeight, allowUpscale)) {
		errorMessage = "SVG display target exceeds the supported raster size";
		return false;
	}
	std::vector<std::uint8_t> pixels;
	if (!RenderSvg(handle.get(), outputWidth, outputHeight, pixels, errorMessage, context)) {
		sourceWidth = sourceHeight = 0;
		return false;
	}
	if (!AppendBGRA(image, outputWidth, outputHeight, std::move(pixels), 0,
		errorMessage, true)) return false;
	image.isSvg = true;
	return true;
#else
	(void)filename;
	(void)targetWidth;
	(void)targetHeight;
	(void)context;
	(void)allowUpscale;
	errorMessage = "SVG content detected, but SVG support is not available in this build";
	return false;
#endif
}

bool ReadSvgDimensions(const std::filesystem::path& filename, int& width,
	int& height, std::string& errorMessage, const WorkContext& context) {
	width = height = 0;
#if JPEGVIEW_HAVE_SVG
	RsvgHandlePtr handle;
	if (!LoadSvgHandle(filename, handle, width, height, errorMessage, context)) {
		width = height = 0;
		return false;
	}
	return true;
#else
	(void)filename;
	(void)context;
	errorMessage = "SVG content detected, but SVG support is not available in this build";
	return false;
#endif
}

} // namespace jpegview_linux::decoder_detail
