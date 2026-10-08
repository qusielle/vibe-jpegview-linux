#include "test_harness.h"
#include "test_support.h"

#include <zlib.h>

namespace {

std::vector<std::uint8_t> MakeSvgz(const std::string& document) {
	z_stream stream{};
	if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED,
		MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
	std::vector<std::uint8_t> compressed(static_cast<std::size_t>(
		deflateBound(&stream, static_cast<uLong>(document.size()))));
	stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(document.data()));
	stream.avail_in = static_cast<uInt>(document.size());
	stream.next_out = compressed.data();
	stream.avail_out = static_cast<uInt>(compressed.size());
	const int status = deflate(&stream, Z_FINISH);
	const std::size_t written = compressed.size() - stream.avail_out;
	deflateEnd(&stream);
	if (status != Z_STREAM_END) return {};
	compressed.resize(written);
	return compressed;
}

std::string MakeSvgWithExternalDoctype() {
	return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\"\n"
		"  \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n"
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"2\">"
		"<rect width=\"4\" height=\"2\" fill=\"#336699\"/></svg>";
}

void TestImageContentFormatDetection() {
	using jpegview_linux::DetectImageContent;
	using jpegview_linux::ImageContentFormat;
	const auto expect = [](const std::vector<std::uint8_t>& bytes,
		ImageContentFormat expected, const char* label) {
		Expect(DetectImageContent(bytes.data(), bytes.size()) == expected,
			std::string("content signature was not identified as ") + label);
	};
	expect({0xff, 0xd8, 0xff}, ImageContentFormat::Jpeg, "JPEG");
	expect({0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a},
		ImageContentFormat::Png, "PNG");
	expect({0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
		0, 0, 0, 8, 'a', 'c', 'T', 'L'}, ImageContentFormat::Apng, "APNG");
	expect({'G', 'I', 'F', '8', '9', 'a'}, ImageContentFormat::Gif, "GIF");
	expect({'B', 'M'}, ImageContentFormat::Bmp, "BMP");
	expect({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'},
		ImageContentFormat::WebP, "WebP");
	expect({'I', 'I', '*', 0}, ImageContentFormat::Tiff, "TIFF");
	expect({'I', 'I', '*', 0, 0, 0, 0, 0, 'C', 'R', 2, 0},
		ImageContentFormat::Raw, "CR2 RAW");
	expect({'I', 'I', 0x1a, 0, 0, 0, 'H', 'E', 'A', 'P', 'C', 'C', 'D', 'R'},
		ImageContentFormat::Raw, "CRW RAW");
	expect({'F', 'O', 'V', 'b'}, ImageContentFormat::Raw, "X3F RAW");
	expect({0, 0, 0, 24, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f', 0, 0, 0, 0,
		'a', 'v', 'i', 'f', 'm', 'i', 'f', '1'}, ImageContentFormat::Avif, "AVIF");
	expect({0, 0, 0, 24, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c', 0, 0, 0, 0,
		'h', 'e', 'i', 'c', 'm', 'i', 'f', '1'}, ImageContentFormat::Heif, "HEIF");
	expect({0xff, 0x0a}, ImageContentFormat::JpegXl, "JPEG XL");
	expect({'I', 'I', 0xbc, 1}, ImageContentFormat::JpegXr, "JPEG XR");
	expect({'8', 'B', 'P', 'S'}, ImageContentFormat::Psd, "PSD");
	expect({'P', '6', '\n'}, ImageContentFormat::Pnm, "PNM");
	expect({'q', 'o', 'i', 'f'}, ImageContentFormat::Qoi, "QOI");
	const std::string svg = "<?xml version=\"1.0\"?>\n<!-- SVG fixture -->\n"
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"2\">";
	const std::vector<std::uint8_t> svgBytes(svg.begin(), svg.end());
	expect(svgBytes, ImageContentFormat::Svg, "SVG XML");
	const std::string svgWithDoctype = MakeSvgWithExternalDoctype();
	const std::vector<std::uint8_t> svgDoctypeBytes(svgWithDoctype.begin(),
		svgWithDoctype.end());
	expect(svgDoctypeBytes, ImageContentFormat::Svg,
		"SVG XML with an external SVG 1.1 document type");
	expect(MakeSvgz(svg), ImageContentFormat::Svgz, "SVGZ gzip");
	expect({0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 0},
		ImageContentFormat::Tga, "TGA");
	expect({'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'},
		ImageContentFormat::Unknown, "unknown content");
	TemporaryDirectory temporary;
	const fs::path canceledProbe = temporary.path() / "probe.png";
	WriteBytes(canceledProbe, {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a});
	Expect(jpegview_linux::ReadImageContentFormat(canceledProbe, [] { return false; }) ==
		ImageContentFormat::Unknown,
		"content probing ignored cancellation before reading a source");
}

void TestSvgAndSvgzDecoding() {
	TemporaryDirectory temporary;
	const std::string vectorDocument =
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"2\" "
		"viewBox=\"0 0 4 2\"><rect width=\"2\" height=\"2\" fill=\"#ff0000\"/>"
		"<rect x=\"2\" width=\"2\" height=\"2\" fill=\"#0000ff\" "
		"fill-opacity=\"0.5\"/></svg>";
	const fs::path wrongExtension = temporary.path() / "vector.data";
	WriteText(wrongExtension, vectorDocument);
	Expect(!jpegview_linux::IsSupportedImagePath(wrongExtension) &&
		jpegview_linux::ReadImageContentFormat(wrongExtension) ==
			jpegview_linux::ImageContentFormat::Svg,
		"SVG content detection incorrectly depends on the file extension");
	Expect(jpegview_linux::IsSupportedImagePath(temporary.path() / "vector.SVG"),
		"SVG extension is not included in ordinary image lists");
	const fs::path externalDoctypePath = temporary.path() / "matplotlib.svg";
	WriteText(externalDoctypePath, MakeSvgWithExternalDoctype());
	Expect(jpegview_linux::ReadImageContentFormat(externalDoctypePath) ==
		jpegview_linux::ImageContentFormat::Svg,
		"SVG with a standard external doctype was not recognized by content detection");

#if JPEGVIEW_HAVE_SVG
	const fs::path mislabeledSvg = temporary.path() / "vector.jpg";
	WriteText(mislabeledSvg, vectorDocument);
	int detectedSvgWidth = 0;
	int detectedSvgHeight = 0;
	bool detectedSvgFormat = false;
	std::string dimensionsError;
	Expect(jpegview_linux::ReadSourceDimensions(mislabeledSvg, detectedSvgWidth,
		detectedSvgHeight, dimensionsError, {}, &detectedSvgFormat) &&
		detectedSvgFormat && detectedSvgWidth == 4 && detectedSvgHeight == 2,
		"JPEG-suffixed SVG content did not retain its detected vector source format");
	int width = 0;
	int height = 0;
	std::string error;
	const fs::path vectorPath = temporary.path() / "vector.svg";
	WriteText(vectorPath, vectorDocument);
	Expect(jpegview_linux::ReadSvgDimensions(vectorPath, width, height, error) &&
		width == 4 && height == 2,
		"SVG dimensions were not read from its intrinsic geometry: " + error);
	DecodedImage decoded;
	Expect(jpegview_linux::DecodeImage(wrongExtension, decoded, error) &&
		decoded.frames.size() == 1 && decoded.frames[0].width == 4 &&
		decoded.frames[0].height == 2 && decoded.isSvg,
		"content-detected SVG did not decode from a wrong extension: " + error);
	DecodedImage doctypeDecoded;
	Expect(jpegview_linux::DecodeImage(externalDoctypePath, doctypeDecoded, error) &&
		doctypeDecoded.isSvg && doctypeDecoded.frames.size() == 1 &&
		doctypeDecoded.frames.front().width == 4 &&
		doctypeDecoded.frames.front().height == 2,
		"SVG with a standard external document type did not decode: " + error);
	const fs::path internalEntityPath = temporary.path() / "internal-entity.svg";
	WriteText(internalEntityPath,
		"<!DOCTYPE svg [<!ENTITY unsafe \"external-data\">]>"
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"2\">"
		"<text>&unsafe;</text></svg>");
	DecodedImage internalEntityDecoded;
	Expect(!jpegview_linux::DecodeImage(internalEntityPath, internalEntityDecoded, error),
		"SVG internal document type subsets and entities were accepted");
	const auto& pixels = decoded.frames[0].bgra;
	Expect(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 255 && pixels[3] == 255 &&
		pixels[8] >= 250 && pixels[9] == 0 && pixels[10] == 0 &&
		pixels[11] >= 127 && pixels[11] <= 128,
		"SVG rasterization did not publish straight-alpha BGRA pixels");
	const auto directRequest = jpegview_linux::MakeSvgDisplayImageRequest(
		vectorPath, 4, 2, 40, 30, false);
	Expect(directRequest.Valid() && !directRequest.decoded &&
		directRequest.fileBackedFormat == jpegview_linux::FileBackedDisplayFormat::Svg,
		"SVG display request did not retain its file-backed vector source");
	const auto wrongExtensionDisplayRequest =
		jpegview_linux::MakeSvgDisplayImageRequest(wrongExtension, 4, 2,
			40, 20, false);
	jpegview_linux::DisplayImageCache wrongExtensionDisplay(1024 * 1024, 1);
	const auto wrongExtensionPrepared = wrongExtensionDisplay.RequestAndWait(
		wrongExtensionDisplayRequest);
	Expect(wrongExtensionDisplayRequest.Valid() && wrongExtensionPrepared &&
		wrongExtensionPrepared->width == 40 && wrongExtensionPrepared->height == 20,
		"content-detected SVG could not use the direct file-backed display path from a wrong extension");
	const auto decodedSvgRequest = jpegview_linux::MakeDisplayImageRequest(
		vectorPath, std::make_shared<const DecodedImage>(decoded), 0, 40, 20, false);
	Expect(decodedSvgRequest.Valid() && decodedSvgRequest.key != directRequest.key &&
		!jpegview_linux::CanReuseDisplayImageRepresentation(
			directRequest.cacheKey, decodedSvgRequest.cacheKey),
		"file-backed SVG rendering reused a cache key for source-raster preparation");
	auto svgRebuildSource = jpegview_linux::MakeSvgDisplayImageRequest(
		vectorPath, 4, 2, 40, 30, true, 7, {}, 1, true);
	svgRebuildSource.workClass = jpegview_linux::PerfWorkClass::FocusedPreview;
	svgRebuildSource.selectionGeneration = 42;
	const auto rebuiltSvgRequest = jpegview_linux::RebuildDisplayImageRequestAtTarget(
		svgRebuildSource, 20, 15);
	Expect(rebuiltSvgRequest.Valid() &&
		rebuiltSvgRequest.fileBackedFormat == jpegview_linux::FileBackedDisplayFormat::Svg &&
		!rebuiltSvgRequest.decoded && rebuiltSvgRequest.targetWidth == 20 &&
		rebuiltSvgRequest.targetHeight == 15 &&
		rebuiltSvgRequest.rotationQuarterTurns == 1 &&
		rebuiltSvgRequest.includeSpectrum && rebuiltSvgRequest.autoContrast &&
		rebuiltSvgRequest.priority == 7 &&
		rebuiltSvgRequest.workClass == jpegview_linux::PerfWorkClass::FocusedPreview &&
		rebuiltSvgRequest.selectionGeneration == 42 &&
		rebuiltSvgRequest.cacheKey.renderVectorAtTarget,
		"rebuilding a display request lost SVG format, pixel settings or scheduling metadata");
	const fs::path jpegRequestPath = temporary.path() / "request.jpeg";
	WriteText(jpegRequestPath, "request identity fixture");
	auto jpegRebuildSource = jpegview_linux::MakeJpegDisplayImageRequest(
		jpegRequestPath, 100, 50, 80, 40, false, 3);
	const auto rebuiltJpegRequest = jpegview_linux::RebuildDisplayImageRequestAtTarget(
		jpegRebuildSource, 40, 20);
	Expect(jpegRebuildSource.Valid() && rebuiltJpegRequest.Valid() &&
		rebuiltJpegRequest.fileBackedFormat == jpegview_linux::FileBackedDisplayFormat::Jpeg &&
		rebuiltJpegRequest.cacheKey.targetWidth == 40 &&
		rebuiltJpegRequest.cacheKey.targetHeight == 20 &&
		rebuiltJpegRequest.priority == 3,
		"display request reconstruction changed the existing JPEG request identity");
	const auto rebuiltDecodedRequest = jpegview_linux::RebuildDisplayImageRequestAtTarget(
		decodedSvgRequest, 20, 10);
	Expect(rebuiltDecodedRequest.Valid() &&
		rebuiltDecodedRequest.decoded == decodedSvgRequest.decoded &&
		rebuiltDecodedRequest.frameIndex == decodedSvgRequest.frameIndex &&
		!rebuiltDecodedRequest.cacheKey.renderVectorAtTarget,
		"display request reconstruction converted a decoded raster into a file-backed vector request");
	jpegview_linux::DisplayImageCache display(1024 * 1024, 1);
	const auto directPrepared = display.RequestAndWait(directRequest);
	Expect(directPrepared && directPrepared->width == 40 && directPrepared->height == 20 &&
		directPrepared->bgra.size() == 40u * 20u * 4u &&
		directPrepared->source.Metadata().hasDimensions &&
		directPrepared->source.Metadata().width == 4 &&
		directPrepared->source.Metadata().height == 2,
		"SVG display preparation did not rasterize directly at fitted target resolution");
	DecodedImage noUpscaleSvg;
	int noUpscaleSourceWidth = 0;
	int noUpscaleSourceHeight = 0;
	Expect(jpegview_linux::DecodeSvgForDisplay(vectorPath, 40, 30, noUpscaleSvg,
		noUpscaleSourceWidth, noUpscaleSourceHeight, error, {}, false) &&
		noUpscaleSvg.frames.size() == 1 && noUpscaleSvg.frames.front().width == 4 &&
		noUpscaleSvg.frames.front().height == 2 && noUpscaleSourceWidth == 4 &&
		noUpscaleSourceHeight == 2,
		"SVG thumbnail rendering enlarged a source that already fits its requested bounds");
	const auto rotatedSvgRequest = jpegview_linux::MakeSvgDisplayImageRequest(
		vectorPath, 4, 2, 20, 40, false, 0, {}, 1);
	const auto rotatedSvg = display.RequestAndWait(rotatedSvgRequest);
	Expect(rotatedSvg && rotatedSvg->width == 20 && rotatedSvg->height == 40 &&
		rotatedSvg->rotationQuarterTurns == 1,
		"file-backed SVG rendering did not rotate into the requested final dimensions");
	const auto histogramSvgRequest = jpegview_linux::MakeSvgDisplayImageRequest(
		vectorPath, 4, 2, 40, 30, false, 0, {}, 0, true);
	const auto histogramSvg = display.RequestAndWait(histogramSvgRequest);
	Expect(histogramSvg && histogramSvg->width == 40 && histogramSvg->height == 20 &&
		histogramSvg->spectrum && histogramSvg->bgra == directPrepared->bgra,
		"SVG histogram preparation did not retain the direct target-rendered pixels");
	const fs::path fineDetailPath = temporary.path() / "fine-detail.svg";
	std::string fineDetailDocument =
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"1\" "
		"viewBox=\"0 0 4 1\"><rect width=\"4\" height=\"1\" fill=\"white\"/>";
	for (int stripe = 0; stripe < 40; stripe += 2) {
		fineDetailDocument += "<rect x=\"" + std::to_string(stripe / 10) + "." +
			std::to_string(stripe % 10) +
			"\" width=\"0.05\" height=\"1\" fill=\"black\"/>";
	}
	fineDetailDocument += "</svg>";
	WriteText(fineDetailPath, fineDetailDocument);
	const auto fineDetailPlain = display.RequestAndWait(
		jpegview_linux::MakeSvgDisplayImageRequest(fineDetailPath, 4, 1,
			80, 20, false));
	const auto fineDetailHistogram = display.RequestAndWait(
		jpegview_linux::MakeSvgDisplayImageRequest(fineDetailPath, 4, 1,
			80, 20, false, 0, {}, 0, true));
	std::size_t fineDetailTransitions = 0;
	if (fineDetailPlain) {
		const auto isBlack = [&fineDetailPlain](int x) {
			return fineDetailPlain->bgra[static_cast<std::size_t>(x) * 4 + 2] < 128;
		};
		for (int x = 1; x < fineDetailPlain->width; ++x) {
			if (isBlack(x) != isBlack(x - 1)) ++fineDetailTransitions;
		}
	}
	Expect(fineDetailPlain && fineDetailHistogram && fineDetailHistogram->spectrum &&
		fineDetailHistogram->bgra == fineDetailPlain->bgra &&
		fineDetailTransitions >= 30,
		"SVG histogram preparation enlarged a low-resolution intrinsic raster instead of preserving fine vector detail");
	const jpegview_linux::DisplayImageTarget boundedSvgTarget =
		jpegview_linux::BoundSvgDisplayTarget(100000, 100000);
	Expect(boundedSvgTarget.width > 0 && boundedSvgTarget.height > 0 &&
		boundedSvgTarget.width <= 65535 && boundedSvgTarget.height <= 65535 &&
		static_cast<std::uint64_t>(boundedSvgTarget.width) * boundedSvgTarget.height <=
			100ull * 1024ull * 1024ull,
		"SVG display target was not bounded by decoder pixel limits");

	const fs::path physicalUnits = temporary.path() / "physical.svg";
	WriteText(physicalUnits,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2in\" height=\"25.4mm\"/>");
	Expect(jpegview_linux::ReadSvgDimensions(physicalUnits, width, height, error) &&
		width == 192 && height == 96,
		"SVG physical units were not converted at 96 DPI: " + error);
	const fs::path fontRelative = temporary.path() / "font-relative.svg";
	WriteText(fontRelative,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10em\" height=\"5em\" "
		"font-size=\"20\"/>");
	Expect(jpegview_linux::ReadSvgDimensions(fontRelative, width, height, error) &&
		width == 200 && height == 100,
		"SVG font-relative dimensions ignored the computed root font size: " + error);
	const fs::path mixedFontUnits = temporary.path() / "mixed-font-units.svg";
	WriteText(mixedFontUnits,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10em\" height=\"25px\" "
		"font-size=\"20\"/>");
	Expect(jpegview_linux::ReadSvgDimensions(mixedFontUnits, width, height, error) &&
		width == 200 && height == 25,
		"mixed SVG font-relative and pixel dimensions were not resolved by librsvg: " + error);
	const fs::path viewBoxOnly = temporary.path() / "viewbox.svg";
	WriteText(viewBoxOnly,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 400 200\"/>");
	Expect(jpegview_linux::ReadSvgDimensions(viewBoxOnly, width, height, error) &&
		width == 400 && height == 200,
		"SVG viewBox did not provide the fallback intrinsic dimensions: " + error);
	const fs::path oneDimension = temporary.path() / "one-dimension.svg";
	WriteText(oneDimension,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"100%\" "
		"viewBox=\"0 0 200 100\"/>");
	Expect(jpegview_linux::ReadSvgDimensions(oneDimension, width, height, error) &&
		width == 120 && height == 60,
		"SVG percentage dimensions were not derived from the viewBox aspect ratio: " + error);
	const fs::path defaultSize = temporary.path() / "default.svg";
	WriteText(defaultSize, "<svg xmlns=\"http://www.w3.org/2000/svg\"><circle r=\"2\"/></svg>");
	Expect(jpegview_linux::ReadSvgDimensions(defaultSize, width, height, error) &&
		width == 300 && height == 150,
		"SVG without dimensions did not use the default viewport size: " + error);
	const fs::path tooLarge = temporary.path() / "too-large.svg";
	WriteText(tooLarge,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"65536\" height=\"1\"/>");
	Expect(!jpegview_linux::ReadSvgDimensions(tooLarge, width, height, error) &&
		width == 0 && height == 0,
		"SVG dimensions above the decoder limit were accepted");

	const fs::path svgzPath = temporary.path() / "compressed.svgz";
	const std::vector<std::uint8_t> compressed = MakeSvgz(vectorDocument);
	Expect(!compressed.empty(), "could not create SVGZ test document");
	WriteBytes(svgzPath, compressed);
	Expect(jpegview_linux::ReadImageContentFormat(svgzPath) ==
		jpegview_linux::ImageContentFormat::Svgz &&
		jpegview_linux::ReadSvgDimensions(svgzPath, width, height, error) &&
		width == 4 && height == 2,
		"valid SVGZ content did not decompress to SVG geometry: " + error);
	Expect(jpegview_linux::DecodeImage(svgzPath, decoded, error) &&
		decoded.frames.size() == 1 && decoded.frames[0].width == 4,
		"valid SVGZ content did not decode: " + error);
	const fs::path truncatedSvgz = temporary.path() / "truncated.svgz";
	std::vector<std::uint8_t> truncated = compressed;
	truncated.resize(truncated.size() - 4);
	WriteBytes(truncatedSvgz, truncated);
	Expect(!jpegview_linux::DecodeImage(truncatedSvgz, decoded, error) &&
		decoded.frames.empty(), "truncated SVGZ input was accepted");
	const fs::path badCrcSvgz = temporary.path() / "bad-crc.svgz";
	std::vector<std::uint8_t> badCrc = compressed;
	badCrc[badCrc.size() - 8] ^= 0x80;
	WriteBytes(badCrcSvgz, badCrc);
	Expect(!jpegview_linux::DecodeImage(badCrcSvgz, decoded, error) &&
		decoded.frames.empty(), "SVGZ with a bad gzip checksum was accepted");
	std::vector<std::uint8_t> concatenated = compressed;
	concatenated.insert(concatenated.end(), compressed.begin(), compressed.end());
	const fs::path concatenatedSvgz = temporary.path() / "concatenated.svgz";
	WriteBytes(concatenatedSvgz, concatenated);
	Expect(!jpegview_linux::DecodeImage(concatenatedSvgz, decoded, error) &&
		decoded.frames.empty(), "concatenated SVGZ gzip members were accepted");

	const std::vector<std::string> rejectedDocuments = {
		"<!DOCTYPE svg [<!ENTITY x SYSTEM 'file:///etc/passwd'>]>"
		"<svg xmlns=\"http://www.w3.org/2000/svg\"><text>&x;</text></svg>",
		"<svg xmlns=\"http://www.w3.org/2000/svg\"><image href=\"file:///etc/passwd\"/></svg>",
		"<svg xmlns=\"http://www.w3.org/2000/svg\"><style>rect { fill: url(https://example.com/x) }</style></svg>",
		"<?xml-stylesheet href=\"file:///tmp/style.css\"?>"
		"<svg xmlns=\"http://www.w3.org/2000/svg\"/>"
	};
	for (std::size_t index = 0; index < rejectedDocuments.size(); ++index) {
		const fs::path unsafe = temporary.path() / ("unsafe-" + std::to_string(index) + ".svg");
		WriteText(unsafe, rejectedDocuments[index]);
		const bool unsafeDecoded = jpegview_linux::DecodeImage(unsafe, decoded, error);
		Expect(!unsafeDecoded && decoded.frames.empty(),
			"SVG external resource or entity policy was not enforced for case " +
			std::to_string(index) + " (decoded=" + std::to_string(unsafeDecoded) +
			", error=" + error + ")");
	}
	const fs::path localReference = temporary.path() / "local-reference.svg";
	WriteText(localReference,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2\" height=\"1\">"
		"<defs><linearGradient id=\"g\"><stop stop-color=\"red\"/></linearGradient></defs>"
		"<rect width=\"2\" height=\"1\" fill=\"url(#g)\"/></svg>");
	Expect(jpegview_linux::DecodeImage(localReference, decoded, error) &&
		decoded.frames.size() == 1,
		"SVG local fragment resources were rejected: " + error);
	const auto canceledContext = jpegview_linux::MakePathWorkContext(vectorPath,
		jpegview_linux::SourceWorkPriority::Foreground, [] { return false; });
	Expect(!jpegview_linux::DecodeImage(vectorPath, decoded, error, canceledContext) &&
		decoded.frames.empty() && error.find("cancel") != std::string::npos,
		"SVG decode ignored cancellation before parsing");
#else
	DecodedImage decoded;
	std::string error;
	Expect(!jpegview_linux::DecodeImage(wrongExtension, decoded, error) &&
		decoded.frames.empty() && error.find("support is not available") != std::string::npos,
		"build without librsvg did not report the unavailable SVG codec");
#endif
}

void TestImageWriterDecoderRoundTrips() {
	TemporaryDirectory temporary;
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	options.jpegQuality = 100;
	options.webpQuality = 100;

	struct FormatCase {
		const char* extension;
		bool exactRgb;
		bool exactAlpha;
		bool hasTransparency;
	};
	const std::vector<FormatCase> formats = {
		{".png", true, true, true},
		{".bmp", true, false, true},
		{".tga", true, true, true},
		{".ppm", true, false, false},
		{".qoi", true, true, true},
		{".psd", true, false, false},
		{".jpg", false, false, false},
	};
	for (const FormatCase& format : formats) {
		const fs::path filename = temporary.path() / ("roundtrip" + std::string(format.extension));
		std::string error;
		Expect(jpegview_linux::WriteImage(filename, pixels.data(), 2, 2, options, error),
			"cannot write " + filename.extension().string() + ": " + error);
		ExpectDecoded(filename, pixels, format.exactRgb, format.exactAlpha,
			format.hasTransparency);
		if (std::string(format.extension) == ".jpg") {
			int mcuWidth = 0;
			int mcuHeight = 0;
			Expect(jpegview_linux::ReadJpegMcuSize(filename, mcuWidth, mcuHeight, error) &&
				mcuWidth >= 8 && mcuHeight >= 8 && mcuWidth % 8 == 0 && mcuHeight % 8 == 0,
				"JPEG MCU dimensions were not read from the sampling factors");
			Expect(!jpegview_linux::ReadJpegMcuSize(temporary.path() / "missing.jpg",
				mcuWidth, mcuHeight, error), "JPEG MCU reader accepted a missing file");
			const fs::path malformed = temporary.path() / "malformed.jpg";
			WriteText(malformed, "not a JPEG image");
			Expect(!jpegview_linux::ReadJpegMcuSize(malformed, mcuWidth, mcuHeight, error),
				"JPEG MCU reader accepted a malformed file");
		}
	}
	std::vector<std::uint8_t> opaquePixels = pixels;
	for (std::size_t alpha = 3; alpha < opaquePixels.size(); alpha += 4) opaquePixels[alpha] = 255;
	const fs::path opaquePng = temporary.path() / "opaque.png";
	std::string opaqueError;
	Expect(jpegview_linux::WriteImage(opaquePng, opaquePixels.data(), 2, 2, options, opaqueError),
		"cannot write opaque PNG: " + opaqueError);
	ExpectDecoded(opaquePng, opaquePixels, true, true, false);
	const fs::path colorKeyPng = temporary.path() / "color-key.png";
	WriteBytes(colorKeyPng, MakeTransparentColorKeyPng());
	ExpectDecoded(colorKeyPng,
		{0, 0, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 0},
		true, true, true);

	const fs::path uppercase = temporary.path() / "uppercase.PNG";
	std::string error;
	Expect(jpegview_linux::WriteImage(uppercase, pixels.data(), 2, 2, options, error),
		"uppercase extension was not accepted by the writer: " + error);
	ExpectDecoded(uppercase, pixels, true, true, true);

	struct OptionalFormatCase {
		const char* extension;
		int dimension;
	};
	const std::vector<OptionalFormatCase> optionalFormats = {
#if JPEGVIEW_HAVE_GIF
		{".gif", 2},
#endif
#if JPEGVIEW_HAVE_TIFF
		{".tiff", 2},
#endif
#if JPEGVIEW_HAVE_WEBP
		{".webp", 2},
#endif
#if JPEGVIEW_HAVE_HEIF
		// Ubuntu 20.04's libheif/x265 encoder cannot encode a 2x2 image.
		// Exercise the codec with a realistic size while keeping the other
		// optional format tests sensitive to tiny-image regressions.
		{".heic", 64},
#endif
#if JPEGVIEW_HAVE_AVIF
		{".avif", 2},
#endif
#if JPEGVIEW_HAVE_JXL
		{".jxl", 2},
#endif
	};
	for (const OptionalFormatCase& format : optionalFormats) {
		const fs::path filename = temporary.path() / ("optional" + std::string(format.extension));
		std::vector<std::uint8_t> optionalPixels(
			static_cast<std::size_t>(format.dimension) * format.dimension * 4);
		for (int y = 0; y < format.dimension; ++y) {
			for (int x = 0; x < format.dimension; ++x) {
				const std::size_t target = (static_cast<std::size_t>(y) * format.dimension + x) * 4;
				const std::size_t source = (static_cast<std::size_t>(y % 2) * 2 + x % 2) * 4;
				std::copy_n(pixels.data() + source, 4, optionalPixels.data() + target);
			}
		}
		error.clear();
		Expect(jpegview_linux::WriteImage(filename, optionalPixels.data(), format.dimension,
			format.dimension, options, error),
			"cannot write optional " + filename.extension().string() + ": " + error);
		DecodedImage decoded;
		error.clear();
		Expect(jpegview_linux::DecodeImage(filename, decoded, error),
			"cannot decode optional " + filename.extension().string() + ": " + error);
		Expect(decoded.frames.size() == 1 && decoded.frames.front().width == format.dimension &&
			decoded.frames.front().height == format.dimension,
			"optional format returned incorrect decoded dimensions");
	}

	const fs::path unknown = temporary.path() / "image.unknown";
	error.clear();
	Expect(!jpegview_linux::WriteImage(unknown, pixels.data(), 2, 2, options, error),
		"unknown output extension was accepted");
	Expect(error.find("unsupported output format") != std::string::npos,
		"unknown output extension returned an unhelpful error");
	error.clear();
	Expect(!jpegview_linux::WriteImage(temporary.path() / "invalid.png", pixels.data(), 0, 2, options, error),
		"invalid dimensions were accepted by the writer");
	Expect(!error.empty(), "invalid dimensions did not produce an error message");
}

void TestContentDispatchPreservesContainerPolicy() {
	TemporaryDirectory temporary;
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	std::string error;
	const fs::path pngWithProjectSuffix = temporary.path() / "png-content.kra";
	Expect(jpegview_linux::WriteImageWithFormat(pngWithProjectSuffix, ".png",
		pixels.data(), 2, 2, options, error), "cannot write PNG container-policy fixture: " + error);
	ExpectDecoded(pngWithProjectSuffix, pixels, true, true, true);
	const fs::path pngWithRawSuffix = temporary.path() / "png-content.nef";
	fs::copy_file(pngWithProjectSuffix, pngWithRawSuffix);
	ExpectDecoded(pngWithRawSuffix, pixels, true, true, true);
	for (const char* suffix : {".jpg", ".image-data"}) {
		const fs::path colorKeyPng = temporary.path() / ("color-key" + std::string(suffix));
		WriteBytes(colorKeyPng, MakeTransparentColorKeyPng());
		ExpectDecoded(colorKeyPng,
			{0, 0, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 0},
			true, true, true);
	}
#if JPEGVIEW_HAVE_TIFF
	const fs::path tiffWithRawSuffix = temporary.path() / "tiff-content.nef";
	Expect(jpegview_linux::WriteImageWithFormat(tiffWithRawSuffix, ".tiff",
		pixels.data(), 2, 2, options, error), "cannot write TIFF container-policy fixture: " + error);
	Expect(jpegview_linux::ReadImageContentFormat(tiffWithRawSuffix) ==
		jpegview_linux::ImageContentFormat::Tiff,
		"TIFF container-policy fixture did not have ambiguous TIFF magic");
	DecodedImage decoded;
	Expect(!jpegview_linux::DecodeImage(tiffWithRawSuffix, decoded, error) &&
		error.find("RAW") != std::string::npos,
		"ambiguous TIFF magic bypassed the RAW reader selected by a camera suffix");
#endif
}

#if JPEGVIEW_HAVE_WEBP
void TestPhotoSizedLossyVp8WebPDecode() {
	TemporaryDirectory temporary;
	constexpr int width = 1920;
	constexpr int height = 1280;
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
			pixels[offset] = static_cast<std::uint8_t>(x * 255 / (width - 1));
			pixels[offset + 1] = static_cast<std::uint8_t>(y * 255 / (height - 1));
			pixels[offset + 2] = static_cast<std::uint8_t>((x + y) * 255 /
				(width + height - 2));
			pixels[offset + 3] = 255;
		}
	}
	ImageWriteOptions options;
	options.webpQuality = 82;
	const fs::path webpFilename = temporary.path() / "photo-sized-vp8.webp";
	std::string error;
	Expect(jpegview_linux::WriteImage(webpFilename, pixels.data(), width, height, options, error),
		"cannot write lossy VP8 WebP fixture: " + error);

	const std::vector<std::uint8_t> encoded = ReadBytes(webpFilename);
	Expect(encoded.size() >= 20 &&
		std::equal(encoded.begin(), encoded.begin() + 4, "RIFF") &&
		std::equal(encoded.begin() + 8, encoded.begin() + 12, "WEBP"),
		"lossy VP8 fixture did not contain a WebP RIFF container");
	bool hasVp8Chunk = false;
	for (std::size_t offset = 12; offset + 8 <= encoded.size();) {
		const std::uint32_t chunkSize = static_cast<std::uint32_t>(encoded[offset + 4]) |
			(static_cast<std::uint32_t>(encoded[offset + 5]) << 8) |
			(static_cast<std::uint32_t>(encoded[offset + 6]) << 16) |
			(static_cast<std::uint32_t>(encoded[offset + 7]) << 24);
		if (std::equal(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
			encoded.begin() + static_cast<std::ptrdiff_t>(offset + 4), "VP8 ")) {
			hasVp8Chunk = true;
			break;
		}
		const std::size_t chunkBytes = static_cast<std::size_t>(chunkSize) + (chunkSize & 1u);
		if (chunkBytes > encoded.size() - offset - 8) break;
		offset += 8 + chunkBytes;
	}
	Expect(hasVp8Chunk, "photo-sized WebP fixture was not lossy VP8 encoding");

	const fs::path mislabeledFilename = temporary.path() / "photo-sized-vp8.jpg";
	fs::rename(webpFilename, mislabeledFilename);
	int jpegWidth = 0;
	int jpegHeight = 0;
	error.clear();
	Expect(!jpegview_linux::ReadJpegDimensions(mislabeledFilename, jpegWidth,
		jpegHeight, error) && jpegWidth == 0 && jpegHeight == 0,
		"WebP RIFF content was incorrectly accepted as a JPEG header");
	Expect(jpegview_linux::IsJpegPath(mislabeledFilename),
		"mislabeled fixture no longer exercises the JPEG-selected decode path");
	const fs::path unknownSuffixFilename = temporary.path() / "photo-sized-vp8.image-data";
	fs::copy_file(mislabeledFilename, unknownSuffixFilename);
	Expect(!jpegview_linux::IsSupportedImagePath(unknownSuffixFilename) &&
		jpegview_linux::ReadImageContentFormat(unknownSuffixFilename) ==
			jpegview_linux::ImageContentFormat::WebP,
		"content probing did not identify WebP independently of the file suffix");

	DecodedImage decoded;
	error.clear();
	Expect(jpegview_linux::DecodeImage(mislabeledFilename, decoded, error),
		"cannot decode lossy VP8 WebP stored in a JPEG-named file: " + error);
	Expect(decoded.frames.size() == 1 && decoded.frames.front().width == width &&
		decoded.frames.front().height == height &&
		decoded.frames.front().bgra.size() == pixels.size() &&
		!decoded.frames.front().hasTransparency && !decoded.animation,
		"photo-sized lossy VP8 WebP decoded with the wrong frame geometry or alpha state");
	DecodedImage unknownSuffixDecoded;
	error.clear();
	Expect(jpegview_linux::DecodeImage(unknownSuffixFilename, unknownSuffixDecoded, error),
		"cannot decode WebP explicitly opened with an unknown suffix: " + error);
	Expect(unknownSuffixDecoded.frames.size() == 1 &&
		unknownSuffixDecoded.frames.front().width == width &&
		unknownSuffixDecoded.frames.front().height == height,
		"unknown-suffix WebP was decoded with the wrong dimensions");
	const jpegview_linux::DecodedFrame& frame = decoded.frames.front();
	const std::size_t sample = (static_cast<std::size_t>(height / 2) * width + width / 2) * 4;
	for (std::size_t channel = 0; channel < 3; ++channel) {
		Expect(std::abs(static_cast<int>(frame.bgra[sample + channel]) - 127) <= 20,
			"photo-sized lossy VP8 WebP decoded an incorrect center color sample");
	}
	Expect(frame.bgra[sample + 3] == 255,
		"opaque lossy VP8 WebP unexpectedly decoded a transparent center pixel");

	DecodedImage displayDecoded;
	int sourceWidth = 0;
	int sourceHeight = 0;
	error.clear();
	Expect(jpegview_linux::DecodeJpegForDisplay(mislabeledFilename, 640, 480,
		displayDecoded, sourceWidth, sourceHeight, error),
		"JPEG display decode did not fall back to the RIFF/WEBP signature: " + error);
	Expect(sourceWidth == width && sourceHeight == height &&
		displayDecoded.frames.size() == 1 &&
		displayDecoded.frames.front().width == width &&
		displayDecoded.frames.front().height == height,
		"WebP display fallback lost the source dimensions or frame");
}
#else
void TestJpegNamedWebPReportsUnavailableCodec() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "webp-content.jpg";
	WriteBytes(filename, {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'});
	DecodedImage decoded;
	std::string error;
	Expect(!jpegview_linux::DecodeImage(filename, decoded, error) &&
		error.find("WebP content detected") != std::string::npos &&
		error.find("not available") != std::string::npos,
		"JPEG-named WebP content did not report the missing optional WebP decoder");
}
#endif

void TestJpegWriterErrorRecovery() {
	TemporaryDirectory temporary;
	constexpr int dimension = 256;
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(dimension) * dimension * 4);
	std::uint32_t randomState = 31;
	for (std::uint8_t& value : pixels) {
		randomState = randomState * 1664525u + 1013904223u;
		value = static_cast<std::uint8_t>(randomState >> 24);
	}
	ImageWriteOptions options;
	options.jpegQuality = 95;
	std::string error;
	// Noise makes the JPEG larger than stdio's buffer, forcing libjpeg's own
	// write-error/longjmp path rather than a failure only at the final fclose.
	for (int attempt = 0; attempt < 2; ++attempt) {
		error.clear();
		Expect(!jpegview_linux::WriteImageWithFormat("/dev/full", ".jpg", pixels.data(),
			dimension, dimension, options, error) &&
			error.find("Output file write error") != std::string::npos,
			"JPEG output exhaustion did not return the libjpeg write failure");
	}
	const fs::path recovered = temporary.path() / "after-write-failure.jpg";
	error.clear();
	Expect(jpegview_linux::WriteImage(recovered, pixels.data(), dimension, dimension,
		options, error), "JPEG writing did not recover after output exhaustion: " + error);
	DecodedImage decoded;
	Expect(jpegview_linux::DecodeImage(recovered, decoded, error) &&
		decoded.frames.size() == 1 && decoded.frames.front().width == dimension &&
		decoded.frames.front().height == dimension,
		"JPEG written after output exhaustion was not a valid image: " + error);
}

void ExpectStaticFrame(const fs::path& filename, int width, int height,
	const std::vector<std::uint8_t>& expectedPixels) {
	DecodedImage decoded;
	std::string error;
	Expect(jpegview_linux::DecodeImage(filename, decoded, error),
		"cannot decode PNM fixture " + filename.filename().string() + ": " + error);
	Expect(!decoded.animation && decoded.frames.size() == 1, "PNM fixture was not decoded as a static image");
	Expect(decoded.frames.front().width == width && decoded.frames.front().height == height,
		"PNM fixture dimensions are incorrect for " + filename.filename().string());
	if (decoded.frames.front().bgra != expectedPixels) {
		std::ostringstream actual;
		for (std::uint8_t value : decoded.frames.front().bgra) actual << static_cast<int>(value) << ',';
		throw TestFailure("PNM fixture pixels are incorrect for " + filename.filename().string() +
			" (actual=" + actual.str() + ")");
	}
}

void TestPnmVariants() {
	TemporaryDirectory temporary;
	WriteText(temporary.path() / "ascii.pgm", "P2\n# grayscale comment\n2 1\n255\n0 255\n");
	ExpectStaticFrame(temporary.path() / "ascii.pgm", 2, 1,
		{0, 0, 0, 255, 255, 255, 255, 255});
	WriteText(temporary.path() / "ascii.pbm", "P1\n2 1\n0 1\n");
	ExpectStaticFrame(temporary.path() / "ascii.pbm", 2, 1,
		{255, 255, 255, 255, 0, 0, 0, 255});

	WriteBytes(temporary.path() / "binary.pgm", {
		'P', '5', '\n', '2', ' ', '1', '\n', '6', '5', '5', '3', '5', '\n',
		0x00, 0x00, 0xff, 0xff});
	ExpectStaticFrame(temporary.path() / "binary.pgm", 2, 1,
		{0, 0, 0, 255, 255, 255, 255, 255});

	WriteBytes(temporary.path() / "bitmap.pbm", {
		'P', '4', '\n', '8', ' ', '1', '\n', 0xaa});
	std::vector<std::uint8_t> pbmExpected;
	for (int bit = 0; bit < 8; ++bit) {
		const std::uint8_t value = (bit % 2 == 0) ? 0 : 255;
		pbmExpected.insert(pbmExpected.end(), {value, value, value, 255});
	}
	ExpectStaticFrame(temporary.path() / "bitmap.pbm", 8, 1, pbmExpected);

	WriteText(temporary.path() / "ascii.ppm", "P3\n2 1\n255\n255 0 0   0 255 0\n");
	ExpectStaticFrame(temporary.path() / "ascii.ppm", 2, 1,
		{0, 0, 255, 255, 0, 255, 0, 255});

	WriteText(temporary.path() / "alpha.pam",
		"P7\nWIDTH 2\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n");
	std::ofstream alpha(temporary.path() / "alpha.pam", std::ios::binary | std::ios::app);
	alpha.write("\xff\x00\x00\xff\x00\xff\x00\x80", 8);
	alpha.close();
	ExpectStaticFrame(temporary.path() / "alpha.pam", 2, 1,
		{0, 0, 255, 255, 0, 255, 0, 128});
}

void TestDirectCodecSourceReadAttribution() {
	TemporaryDirectory temporary;
	const fs::path bitmap = temporary.path() / "tracked-bitmap.bmp";
	const std::vector<std::uint8_t> pixels = {
		10, 20, 30, 255, 40, 50, 60, 255,
		70, 80, 90, 255, 100, 110, 120, 255};
	ImageWriteOptions options;
	std::string error;
	Expect(jpegview_linux::WriteImage(bitmap, pixels.data(), 2, 2, options, error),
		"cannot create direct-reader BMP fixture: " + error);
	std::error_code sizeError;
	const std::uint64_t bitmapSize = fs::file_size(bitmap, sizeError);
	Expect(!sizeError && bitmapSize > 0, "cannot determine direct-reader BMP fixture size");
	DecodedImage decoded;
	bool decodedSuccessfully = false;
	std::thread worker([&] {
		jpegview_linux::PerfContextScope attribution(
			jpegview_linux::PerfWorkClass::FocusedPreview,
			jpegview_linux::PerfExecution::WorkerThread);
		decodedSuccessfully = jpegview_linux::DecodeImage(bitmap, decoded, error);
	});
	worker.join();
	Expect(decodedSuccessfully && decoded.frames.size() == 1 &&
		decoded.frames.front().width == 2 && decoded.frames.front().height == 2,
		"cannot decode direct-reader BMP fixture: " + error);
	Expect(HasCodecSourceReadTrace("stb", bitmapSize),
		"stb direct-read trace did not report timed bytes under the focused-preview worker class");
}

void TestAnimatedImageDecoders() {
	TemporaryDirectory temporary;
	ImageWriteOptions options;
	const std::vector<std::uint8_t> red = {
		0, 0, 255, 255, 0, 0, 255, 255,
		0, 0, 255, 255, 0, 0, 255, 255};
	const std::vector<std::uint8_t> blue = {
		255, 0, 0, 255, 255, 0, 0, 255,
		255, 0, 0, 255, 255, 0, 0, 255};
	std::string error;
	const fs::path firstPng = temporary.path() / "first.png";
	const fs::path secondPng = temporary.path() / "second.png";
	Expect(jpegview_linux::WriteImage(firstPng, red.data(), 2, 2, options, error), "cannot write APNG first frame");
	Expect(jpegview_linux::WriteImage(secondPng, blue.data(), 2, 2, options, error), "cannot write APNG second frame");
	const fs::path apng = temporary.path() / "animated.apng";
	const std::vector<std::uint8_t> apngBytes = MakeApng(
		ReadBytes(firstPng), ReadBytes(secondPng));
	WriteBytes(apng, apngBytes);
	DecodedImage decoded;
	Expect(jpegview_linux::DecodeImage(apng, decoded, error), "cannot decode APNG: " + error);
	Expect(decoded.animation && decoded.frames.size() == 2 && decoded.loopCount == 1,
		"APNG animation metadata is incorrect");
	Expect(decoded.frames[0].delayMs == 70 && decoded.frames[1].delayMs == 70,
		"APNG frame delay is incorrect");
	Expect(decoded.frames[0].bgra != decoded.frames[1].bgra, "APNG frames were not composited independently");

	std::vector<std::uint8_t> delayedApng = apngBytes;
	const std::size_t afterIhdr = 8 + 12 + 13;
	delayedApng.resize(afterIhdr);
	AppendPngChunk(delayedApng, "teSt", std::vector<std::uint8_t>(256 * 1024, 'x'));
	delayedApng.insert(delayedApng.end(), apngBytes.begin() +
		static_cast<std::ptrdiff_t>(afterIhdr), apngBytes.end());
	const fs::path delayedAnimation = temporary.path() / "large-metadata.apng";
	WriteBytes(delayedAnimation, delayedApng);
	Expect(jpegview_linux::ReadImageContentFormat(delayedAnimation) ==
		jpegview_linux::ImageContentFormat::Unknown,
		"bounded PNG probing guessed static PNG before reaching its animation marker");
	DecodedImage delayedDecoded;
	Expect(jpegview_linux::DecodeImage(delayedAnimation, delayedDecoded, error),
		"suffix fallback did not decode APNG beyond the signature-probe bound: " + error);
	Expect(delayedDecoded.animation && delayedDecoded.frames.size() == 2,
		"bounded content probing replaced APNG extension fallback with static PNG decoding");

	const std::vector<std::uint8_t> gif = {
		0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x02, 0x00, 0xf0, 0x00,
		0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x21, 0xff, 0x0b, 0x4e, 0x45,
		0x54, 0x53, 0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x01,
		0x00, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x21, 0xff,
		0x0b, 0x49, 0x6d, 0x61, 0x67, 0x65, 0x4d, 0x61, 0x67, 0x69, 0x63, 0x6b,
		0x0e, 0x67, 0x61, 0x6d, 0x6d, 0x61, 0x3d, 0x30, 0x2e, 0x34, 0x35, 0x34,
		0x35, 0x34, 0x35, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02,
		0x00, 0x00, 0x02, 0x02, 0x84, 0x51, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x02,
		0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x49, 0x6d, 0x61, 0x67, 0x65, 0x4d,
		0x61, 0x67, 0x69, 0x63, 0x6b, 0x0e, 0x67, 0x61, 0x6d, 0x6d, 0x61, 0x3d,
		0x30, 0x2e, 0x34, 0x35, 0x34, 0x35, 0x34, 0x35, 0x00, 0x2c, 0x00, 0x00,
		0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x80, 0x00, 0x00, 0xff, 0xff, 0xff,
		0xff, 0x02, 0x02, 0x84, 0x51, 0x00, 0x3b};
	const fs::path gifFile = temporary.path() / "animated.gif";
	WriteBytes(gifFile, gif);
	error.clear();
	decoded = {};
#if JPEGVIEW_HAVE_GIF
	bool decodedGif = false;
	std::thread gifWorker([&] {
		jpegview_linux::PerfContextScope attribution(
			jpegview_linux::PerfWorkClass::FocusedPreview,
			jpegview_linux::PerfExecution::WorkerThread);
		decodedGif = jpegview_linux::DecodeImage(gifFile, decoded, error);
	});
	gifWorker.join();
	Expect(decodedGif, "cannot decode GIF: " + error);
	Expect(HasCodecSourceReadTrace("giflib", gif.size()),
		"giflib direct-read trace did not report timed bytes under the focused-preview worker class");
	Expect(decoded.animation && decoded.isGif && decoded.frames.size() == 2 && decoded.loopCount == 1,
		"GIF animation metadata is incorrect");
	Expect(decoded.frames[0].delayMs == 70 && decoded.frames[1].delayMs == 20,
		"GIF frame delays did not preserve their encoded centisecond values");
	const auto gifDelayOffset = [](const std::vector<std::uint8_t>& bytes,
		std::size_t requested) {
		std::size_t found = 0;
		for (std::size_t index = 0; index + 7 < bytes.size(); ++index) {
			if (bytes[index] == 0x21 && bytes[index + 1] == 0xf9 &&
				bytes[index + 2] == 0x04 && found++ == requested) {
				return index + 4;
			}
		}
		return bytes.size();
	};
	std::vector<std::uint8_t> zeroAndShortDelayGif = gif;
	const std::size_t firstDelayOffset = gifDelayOffset(zeroAndShortDelayGif, 0);
	const std::size_t secondDelayOffset = gifDelayOffset(zeroAndShortDelayGif, 1);
	Expect(firstDelayOffset < zeroAndShortDelayGif.size() &&
		secondDelayOffset < zeroAndShortDelayGif.size(),
		"GIF fixture did not contain both frame delay controls");
	zeroAndShortDelayGif[firstDelayOffset] = 0;
	zeroAndShortDelayGif[secondDelayOffset] = 1;
	const fs::path zeroAndShortGifFile = temporary.path() / "zero-and-short.gif";
	WriteBytes(zeroAndShortGifFile, zeroAndShortDelayGif);
	DecodedImage normalizedGif;
	Expect(jpegview_linux::DecodeImage(zeroAndShortGifFile, normalizedGif, error),
		"cannot decode zero and short-delay GIF: " + error);
	Expect(normalizedGif.frames.size() == 2 &&
		normalizedGif.frames[0].delayMs == 100 &&
		normalizedGif.frames[1].delayMs == 20,
		"GIF zero and sub-20ms delays were not normalized to 100ms and 20ms");
	std::vector<std::uint8_t> missingDelayGif = gif;
	const std::size_t missingDelayOffset = gifDelayOffset(missingDelayGif, 0);
	Expect(missingDelayOffset < missingDelayGif.size(),
		"GIF fixture did not contain a removable frame delay control");
	missingDelayGif.erase(missingDelayGif.begin() +
		static_cast<std::ptrdiff_t>(missingDelayOffset - 4),
		missingDelayGif.begin() + static_cast<std::ptrdiff_t>(missingDelayOffset + 4));
	const fs::path missingDelayGifFile = temporary.path() / "missing-delay.gif";
	WriteBytes(missingDelayGifFile, missingDelayGif);
	DecodedImage defaultDelayGif;
	Expect(jpegview_linux::DecodeImage(missingDelayGifFile, defaultDelayGif, error),
		"cannot decode GIF without frame delay metadata: " + error);
	Expect(defaultDelayGif.frames.size() == 2 &&
		defaultDelayGif.frames[0].delayMs == 100,
		"GIF frame without delay metadata did not use the 100ms default");
#else
	Expect(jpegview_linux::DecodeImage(gifFile, decoded, error),
		"stb fallback could not decode the first GIF frame: " + error);
	Expect(!decoded.animation && decoded.frames.size() == 1,
		"stb fallback did not preserve its documented first-frame GIF behavior");
#endif
}

void TestDecoderFailures() {
	TemporaryDirectory temporary;
	const fs::path invalid = temporary.path() / "invalid.png";
	WriteBytes(invalid, {0x89, 0x50, 0x4e, 0x47, 0x00});
	DecodedImage decoded;
	decoded.animation = true;
	decoded.frames.resize(1);
	std::string error;
	Expect(!jpegview_linux::DecodeImage(invalid, decoded, error), "truncated PNG was accepted");
	Expect(decoded.frames.empty() && !decoded.animation, "decoder did not reset output on failure");
	Expect(!error.empty(), "truncated PNG did not produce an error message");

	error.clear();
	Expect(!jpegview_linux::DecodeImage(temporary.path() / "missing.jpg", decoded, error),
		"missing image was accepted");
	Expect(error == "cannot open file" || !error.empty(), "missing image did not produce an error message");

	const fs::path truncatedJpeg = temporary.path() / "truncated.jpg";
	WriteBytes(truncatedJpeg, {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F'});
	error.clear();
	Expect(!jpegview_linux::DecodeImage(truncatedJpeg, decoded, error) && !error.empty(),
		"truncated native JPEG decode did not fail cleanly");
}

void TestJpegDisplayDecodeScaling() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "large-preview.jpg";
	constexpr int width = 64;
	constexpr int height = 48;
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
			pixels[offset] = static_cast<std::uint8_t>(x * 3);
			pixels[offset + 1] = static_cast<std::uint8_t>(y * 4);
			pixels[offset + 2] = static_cast<std::uint8_t>(x + y);
			pixels[offset + 3] = 255;
		}
	}
	ImageWriteOptions options;
	options.jpegQuality = 95;
	std::string error;
	Expect(jpegview_linux::WriteImage(filename, pixels.data(), width, height, options, error),
		"cannot create scaled JPEG fixture: " + error);
	Expect(jpegview_linux::IsJpegPath(filename) &&
		jpegview_linux::IsJpegPath(temporary.path() / "PHOTO.JPEG") &&
		!jpegview_linux::IsJpegPath(temporary.path() / "photo.png"),
		"JPEG display-path extension policy is incorrect");

	int headerWidth = 0;
	int headerHeight = 0;
	Expect(jpegview_linux::ReadJpegDimensions(filename, headerWidth, headerHeight, error) &&
		headerWidth == width && headerHeight == height,
		"JPEG header probe returned incorrect source dimensions");
	const jpegview_linux::SourceDescriptor cropSource =
		jpegview_linux::DescribeImageSource(filename);
	const jpegview_linux::WorkContext cropWork = jpegview_linux::MakeWorkContext(
		cropSource, jpegview_linux::SourceWorkPriority::Foreground);
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(true);
	int mcuWidth = 0;
	int mcuHeight = 0;
	std::string mcuError;
	auto mcuRead = std::async(std::launch::async, [&] {
		return jpegview_linux::ReadJpegMcuSize(filename, mcuWidth, mcuHeight,
			mcuError, cropWork);
	});
	const bool mcuReadWhileForegroundPending =
		mcuRead.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	if (!mcuReadWhileForegroundPending) coordinator.SetForegroundPending(false);
	const bool mcuReadCompleted = mcuRead.wait_for(std::chrono::seconds(2)) ==
		std::future_status::ready;
	const bool mcuReadSucceeded = mcuReadCompleted && mcuRead.get();
	coordinator.SetForegroundPending(false);
	Expect(mcuReadWhileForegroundPending && mcuReadSucceeded &&
		mcuWidth > 0 && mcuHeight > 0,
		"foreground JPEG crop metadata waited behind its own pending foreground gate: " +
		mcuError);
	DecodedImage scaled;
	int sourceWidth = 0;
	int sourceHeight = 0;
	Expect(jpegview_linux::DecodeJpegForDisplay(filename, 9, 7, scaled,
		sourceWidth, sourceHeight, error),
		"scaled JPEG display decode failed: " + error);
	Expect(sourceWidth == width && sourceHeight == height && scaled.frames.size() == 1,
		"scaled JPEG decode lost its full source dimensions");
	Expect(scaled.frames[0].width >= 9 && scaled.frames[0].height >= 7 &&
		scaled.frames[0].width < width && scaled.frames[0].height < height,
		"JPEG display decode did not select a reduced DCT size above the target");

	const fs::path cancellableJpeg = temporary.path() / "cancellable-scanlines.jpg";
	constexpr int cancellableHeight = 512;
	std::vector<std::uint8_t> tallPixels(
		static_cast<std::size_t>(width) * cancellableHeight * 4, 127);
	Expect(jpegview_linux::WriteImage(cancellableJpeg, tallPixels.data(), width,
		cancellableHeight, options, error),
		"cannot create tall JPEG cancellation fixture: " + error);
	int continuationChecks = 0;
	const jpegview_linux::WorkContext cancelDuringScanlines =
		jpegview_linux::MakePathWorkContext(cancellableJpeg,
			jpegview_linux::SourceWorkPriority::Foreground, [&continuationChecks] {
				return ++continuationChecks < 11;
			});
	DecodedImage canceledDecode;
	error.clear();
	Expect(!jpegview_linux::DecodeImage(cancellableJpeg, canceledDecode, error,
		cancelDuringScanlines) && canceledDecode.frames.empty() &&
		continuationChecks >= 11 && error.find("cancel") != std::string::npos,
		"JPEG decode did not cancel at a scanline batch boundary and clean its pixels");

	const jpegview_linux::DisplayImageRequest request =
		jpegview_linux::MakeJpegDisplayImageRequest(filename, width, height,
			10, 8, false, 1);
	Expect(request.Valid() && !request.decoded,
		"file-backed JPEG display request was not valid without full decoded pixels");
	jpegview_linux::DisplayImageCache display(4096, 1);
	const jpegview_linux::DisplayImageCache::ImagePtr prepared =
		display.RequestAndWait(request);
	Expect(prepared && prepared->filename == filename &&
		prepared->width == 10 && prepared->height == 8 &&
		prepared->bgra.size() == 10u * 8u * 4u,
		"file-backed JPEG preparation did not produce exact display-size pixels");
	const jpegview_linux::SourceMetadata& preparedMetadata = prepared->source.Metadata();
	Expect(preparedMetadata.hasDimensions && preparedMetadata.width == width &&
		preparedMetadata.height == height,
		"reduced-DCT JPEG preparation replaced source metadata dimensions with decoded dimensions");
	const jpegview_linux::DisplayImageRequest rotatedRequest =
		jpegview_linux::MakeJpegDisplayImageRequest(filename, width, height,
			8, 10, false, 1, {}, 1);
	const jpegview_linux::DisplayImageRequest sameSizeUnrotatedRequest =
		jpegview_linux::MakeJpegDisplayImageRequest(filename, width, height,
			8, 10, false, 1);
	Expect(rotatedRequest.Valid() && sameSizeUnrotatedRequest.Valid() &&
		rotatedRequest.key != sameSizeUnrotatedRequest.key,
		"rotated JPEG display request was invalid or reused the unrotated cache key");
	const jpegview_linux::DisplayImageCache::ImagePtr rotatedPrepared =
		display.RequestAndWait(rotatedRequest);
	Expect(rotatedPrepared && rotatedPrepared->width == 8 &&
		rotatedPrepared->height == 10 && rotatedPrepared->rotationQuarterTurns == 1 &&
		rotatedPrepared->bgra.size() == 8u * 10u * 4u,
		"reduced-DCT JPEG preparation did not rotate before producing the final slot size");
}


void TestPerfContextAndPendingInputDiagnostics() {
	using jpegview_linux::PerfExecution;
	using jpegview_linux::PerfWorkClass;
	const std::array<PerfWorkClass, 5> workClasses = {{
		PerfWorkClass::ActiveImageSpread,
		PerfWorkClass::FocusedPreview,
		PerfWorkClass::VisibleThumbnail,
		PerfWorkClass::NearestNavigationNeighbor,
		PerfWorkClass::DistantSpeculation,
	}};
	const std::array<const char*, 5> workClassNames = {{
		"active_image_spread", "focused_preview", "visible_thumbnail",
		"nearest_navigation_neighbor", "distant_speculation",
	}};
	for (std::size_t index = 0; index < workClasses.size(); ++index) {
		Expect(std::string(jpegview_linux::PerfWorkClassName(workClasses[index])) ==
			workClassNames[index], "performance trace used an unexpected work-class label");
	}
	Expect(std::string(jpegview_linux::PerfExecutionName(PerfExecution::EventThread)) ==
		"event_thread" && std::string(jpegview_linux::PerfExecutionName(PerfExecution::WorkerThread)) ==
		"worker_thread", "performance trace did not distinguish event and worker execution");

	{
		jpegview_linux::PerfContextScope active(PerfWorkClass::ActiveImageSpread,
			PerfExecution::EventThread);
		const jpegview_linux::PerfContext eventContext = jpegview_linux::CurrentPerfContext();
		Expect(eventContext.workClass == PerfWorkClass::ActiveImageSpread &&
			eventContext.execution == PerfExecution::EventThread,
			"event-thread active-image context was not installed");
		{
			jpegview_linux::PerfContextScope thumbnail(PerfWorkClass::VisibleThumbnail,
				PerfExecution::WorkerThread);
			const jpegview_linux::PerfContext workerContext =
				jpegview_linux::CurrentPerfContext();
			Expect(workerContext.workClass == PerfWorkClass::VisibleThumbnail &&
				workerContext.execution == PerfExecution::WorkerThread,
				"worker thumbnail context did not distinguish its execution thread");
		}
		const jpegview_linux::PerfContext restored = jpegview_linux::CurrentPerfContext();
		Expect(restored.workClass == PerfWorkClass::ActiveImageSpread &&
			restored.execution == PerfExecution::EventThread,
			"nested performance context did not restore its caller's work attribution");
	}
	const jpegview_linux::PerfContext cleared = jpegview_linux::CurrentPerfContext();
	Expect(cleared.workClass == PerfWorkClass::Unspecified &&
		cleared.execution == PerfExecution::Unspecified,
		"performance context leaked past its stack scope");

	jpegview_linux::PendingInputPresentation pending;
	Expect(pending.PendingSince() == 0 && pending.TakeLatency(900) == 0,
		"input-to-presentation diagnostics produced a latency without a pending input");
	pending.Mark(1000);
	pending.Mark(1500);
	Expect(pending.PendingSince() == 1000 && pending.TakeLatency(1650) == 650 &&
		pending.PendingSince() == 0,
		"a later queued input replaced the oldest unpresented dispatch latency");
	pending.Mark(2000);
	Expect(pending.TakeLatency(1900) == 0 && pending.PendingSince() == 0,
		"input-to-presentation diagnostics retained a consumed or out-of-order timestamp");
}

void TestDecodedPrefetchWorkClassAttribution() {
	TemporaryDirectory temporary;
	std::vector<fs::path> files;
	for (int index = 0; index < 5; ++index) {
		const fs::path filename = temporary.path() /
			("work-class-" + std::to_string(index) + ".ppm");
		WriteText(filename, "P3\n1 1\n255\n10 20 30\n");
		files.push_back(filename);
	}
	std::mutex contextsMutex;
	std::vector<jpegview_linux::PerfContext> contexts;
	jpegview_linux::DecodedImageCache cache(256,
		[&](const fs::path& filename, DecodedImage& image, std::string& error) {
			{
				std::lock_guard<std::mutex> lock(contextsMutex);
				contexts.push_back(jpegview_linux::CurrentPerfContext());
			}
			return jpegview_linux::DecodeImage(filename, image, error);
		});
	cache.Prefetch(files, 2, 1, 4, {}, {}, 1);
	Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
		"decoded work-class fixtures did not finish before their bounded deadline");
	std::size_t nearest = 0;
	std::size_t distant = 0;
	{
		std::lock_guard<std::mutex> lock(contextsMutex);
		for (const jpegview_linux::PerfContext& context : contexts) {
			Expect(context.execution == jpegview_linux::PerfExecution::WorkerThread,
				"decoded speculative work was not attributed to its worker thread");
			if (context.workClass == jpegview_linux::PerfWorkClass::NearestNavigationNeighbor) {
				++nearest;
			} else if (context.workClass == jpegview_linux::PerfWorkClass::DistantSpeculation) {
				++distant;
			}
		}
	}
	Expect(nearest == 1 && distant == 3,
		"decoded prefetch did not separate nearest navigation work from distant speculation");
}

void TestDisplayPrefetchPlannerWorkerSnapshotsAndCancellation() {
	TemporaryDirectory temporary;
	std::vector<jpegview_linux::DisplayPrefetchCandidate> candidates;
	for (int index = 0; index < 4; ++index) {
		const fs::path filename = temporary.path() /
			("planner-" + std::to_string(index) + (index == 0 ? ".svg" : ".jpg"));
		WriteText(filename, "captured source descriptor");
		const auto source = jpegview_linux::DescribeImageSource(filename);
		candidates.push_back({filename, source, static_cast<std::size_t>(index),
			static_cast<std::size_t>(index + 1), {}, false, index != 0});
	}
	std::atomic<int> dimensionReads{0};
	jpegview_linux::DisplayPrefetchPlannerWorker planner(
		[&dimensionReads](const jpegview_linux::SourceDescriptor&, int& width, int& height,
			std::string&, const jpegview_linux::DisplayPrefetchPlannerWorker::Continue& keepGoing) {
			if (!keepGoing()) return false;
			++dimensionReads;
			width = 800;
			height = 600;
			return keepGoing();
		});
	jpegview_linux::DisplayPrefetchPlannerRequest request;
	request.catalogRevision = 17;
	request.descriptorRevision = 23;
	request.viewportRevision = 31;
	request.currentIndex = 1;
	request.pageCount = 4;
	request.preferredDirection = 1;
	request.imageAreaWidth = 640;
	request.imageAreaHeight = 480;
	request.maximumCount = 2;
	request.neighbors = {candidates[2], candidates[0], candidates[3]};
	const std::uint64_t generation = planner.Request(request);
	Expect(planner.WaitUntilIdle(std::chrono::seconds(2)),
		"captured JPEG neighbor planning did not finish before its deadline");
	auto ready = planner.TakeReady();
	Expect(ready.size() == 1 && ready.front().generation == generation &&
		ready.front().catalogRevision == request.catalogRevision &&
		ready.front().descriptorRevision == request.descriptorRevision &&
		ready.front().viewportRevision == request.viewportRevision &&
		ready.front().currentIndex == request.currentIndex &&
		ready.front().preferredDirection == request.preferredDirection &&
		ready.front().imageAreaWidth == request.imageAreaWidth &&
		ready.front().imageAreaHeight == request.imageAreaHeight &&
		ready.front().dimensions.size() == 2 && ready.front().requests.size() == 2 &&
		dimensionReads.load() == 2,
		"JPEG planner did not honor the captured revisions or bounded neighbor window");
	const auto& result = ready.front();
	Expect(result.requests[1].fileBackedFormat ==
		jpegview_linux::FileBackedDisplayFormat::Svg &&
		result.requests[1].cacheKey.renderVectorAtTarget && !result.requests[1].decoded,
		"SVG neighbor planning lost its file-backed vector identity");
	Expect(jpegview_linux::MatchesDisplayPrefetchSnapshot(result, generation,
		request.catalogRevision, request.descriptorRevision, request.viewportRevision,
		request.currentIndex, request.preferredDirection, request.viewport,
		request.imageAreaWidth, request.imageAreaHeight),
		"current planner response did not match its captured catalog and viewport");
	Expect(!jpegview_linux::MatchesDisplayPrefetchSnapshot(result, generation,
		request.catalogRevision + 1, request.descriptorRevision,
		request.viewportRevision, request.currentIndex, request.preferredDirection,
		request.viewport, request.imageAreaWidth, request.imageAreaHeight) &&
		!jpegview_linux::MatchesDisplayPrefetchSnapshot(result, generation,
		request.catalogRevision, request.descriptorRevision,
		request.viewportRevision + 1, request.currentIndex, request.preferredDirection,
		request.viewport, request.imageAreaWidth, request.imageAreaHeight),
		"stale catalog or viewport planner output remained eligible for application");

	jpegview_linux::DisplayPrefetchPlannerRequest cachedDimensionsRequest = request;
	for (jpegview_linux::DisplayPrefetchCandidate& candidate : cachedDimensionsRequest.neighbors) {
		for (const jpegview_linux::DisplayPrefetchPlannedDimensions& dimensions : result.dimensions) {
			if (candidate.index == dimensions.index && candidate.source.Key() == dimensions.source) {
				candidate.source = candidate.source.WithImageProperties(
					dimensions.width, dimensions.height, false);
				break;
			}
		}
	}
	const std::uint64_t cachedGeneration = planner.Request(cachedDimensionsRequest);
	Expect(planner.WaitUntilIdle(std::chrono::seconds(2)),
		"cached-dimension neighbor plan did not finish before its deadline");
	ready = planner.TakeReady();
	Expect(ready.size() == 1 && ready.front().generation == cachedGeneration &&
		ready.front().dimensions.size() == 2 && dimensionReads.load() == 2,
		"the next neighbor plan reprobed dimensions already returned to the viewer cache");

	std::mutex blockerMutex;
	std::condition_variable blockerChanged;
	bool firstReadStarted = false;
	bool releaseFirstRead = false;
	const std::string blockedPath = candidates[0].filename.string();
	jpegview_linux::DisplayPrefetchPlannerWorker cancellable(
		[&](const jpegview_linux::SourceDescriptor& source, int& width, int& height,
			std::string&, const jpegview_linux::DisplayPrefetchPlannerWorker::Continue& keepGoing) {
			if (source.LogicalPath().string() == blockedPath) {
				std::unique_lock<std::mutex> lock(blockerMutex);
				firstReadStarted = true;
				blockerChanged.notify_all();
				if (!blockerChanged.wait_for(lock, std::chrono::seconds(2),
					[&] { return releaseFirstRead; })) return false;
			}
			if (!keepGoing()) return false;
			width = 320;
			height = 240;
			return true;
		});
	jpegview_linux::DisplayPrefetchPlannerRequest obsolete = request;
	obsolete.maximumCount = 1;
	obsolete.neighbors = {candidates[0]};
	const std::uint64_t obsoleteGeneration = cancellable.Request(obsolete);
	{
		std::unique_lock<std::mutex> lock(blockerMutex);
		Expect(blockerChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return firstReadStarted; }),
			"cancellable planner did not enter its injected header read");
	}
	jpegview_linux::DisplayPrefetchPlannerRequest replacement = obsolete;
	replacement.catalogRevision++;
	replacement.viewportRevision++;
	replacement.neighbors = {candidates[1]};
	const std::uint64_t replacementGeneration = cancellable.Request(replacement);
	{
		std::lock_guard<std::mutex> lock(blockerMutex);
		releaseFirstRead = true;
	}
	blockerChanged.notify_all();
	Expect(cancellable.WaitUntilIdle(std::chrono::seconds(2)),
		"replacement planner request did not finish after the old read was released");
	ready = cancellable.TakeReady();
	Expect(ready.size() == 1 && ready.front().generation == replacementGeneration &&
		ready.front().catalogRevision == replacement.catalogRevision &&
		ready.front().dimensions.size() == 1 &&
		ready.front().dimensions.front().source == candidates[1].source.Key() &&
		obsoleteGeneration != replacementGeneration,
		"canceled planner work published dimensions from the obsolete catalog snapshot");

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	std::atomic<int> admissionDimensionReads{0};
	jpegview_linux::DisplayPrefetchPlannerWorker admissionPlanner(
		[&admissionDimensionReads](const jpegview_linux::SourceDescriptor&, int& width,
			int& height, std::string&, const jpegview_linux::DisplayPrefetchPlannerWorker::Continue& keepGoing) {
		++admissionDimensionReads;
		if (!keepGoing()) return false;
		width = 400;
		height = 300;
		return true;
	});
	jpegview_linux::DisplayPrefetchPlannerRequest admissionRequest = obsolete;
	SourceWorkFailureInjection admissionInjection{
		jpegview_linux::detail::SourceWorkTestHookPoint::ActiveCpuRegistration};
	coordinator.SetTestHookForTesting(ThrowAtSourceWorkHook, &admissionInjection);
	const std::uint64_t failedAdmissionGeneration = admissionPlanner.Request(admissionRequest);
	const bool failedAdmissionIdle = admissionPlanner.WaitUntilIdle(std::chrono::seconds(3));
	coordinator.SetTestHookForTesting(nullptr, nullptr);
	const auto failedAdmissionResults = admissionPlanner.TakeReady();
	const std::uint64_t retriedAdmissionGeneration = admissionPlanner.Request(admissionRequest);
	const bool retriedAdmissionIdle = admissionPlanner.WaitUntilIdle(std::chrono::seconds(3));
	const auto retriedAdmissionResults = admissionPlanner.TakeReady();
	Expect(admissionInjection.fired.load() && failedAdmissionIdle &&
		failedAdmissionResults.size() == 1 &&
		failedAdmissionResults.front().generation == failedAdmissionGeneration &&
		failedAdmissionResults.front().failure.kind ==
			jpegview_linux::WorkerFailureKind::Exception &&
		failedAdmissionResults.front().dimensions.empty() &&
		retriedAdmissionIdle && retriedAdmissionResults.size() == 1 &&
		retriedAdmissionResults.front().generation == retriedAdmissionGeneration &&
		!retriedAdmissionResults.front().failure.Failed() &&
		retriedAdmissionResults.front().dimensions.size() == 1 &&
		admissionDimensionReads.load() == 1,
		"planner admission exception escaped its worker or prevented a valid retry");
}

void TestSvgNeighborPrefetchUsesTargetRendering() {
	TemporaryDirectory temporary;
	const fs::path large = temporary.path() / "large.svg";
	const fs::path compressed = temporary.path() / "small.svgz";
	const fs::path unknown = temporary.path() / "explicit.data";
	const fs::path malformed = temporary.path() / "malformed.svg";
	const std::string smallDocument =
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"2\">"
		"<rect width=\"4\" height=\"2\" fill=\"red\"/></svg>";
	WriteText(large,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"9000\" height=\"10000\">"
		"<rect width=\"9000\" height=\"10000\" fill=\"green\"/></svg>");
	WriteBytes(compressed, MakeSvgz(smallDocument));
	WriteText(unknown, smallDocument);
	WriteText(malformed, "<svg broken");
	const fs::path archive = temporary.path() / "vectors.zip";
	WriteZipArchive(archive, {{"small.svg", unknown}});
	const fs::path member = archive / "small.svg";
	jpegview_linux::DisplayPrefetchPlannerRequest plan;
	plan.currentIndex = 0;
	plan.pageCount = 6;
	plan.maximumCount = 5;
	plan.imageAreaWidth = 320;
	plan.imageAreaHeight = 400;
	plan.viewport.noEnlarge = false;
	plan.maximumPreparedBytes = 1024u * 1024u;
	for (const fs::path& filename : {large, compressed, member, unknown, malformed}) {
		const std::size_t position = plan.neighbors.size() + 1;
		plan.neighbors.push_back({filename, jpegview_linux::DescribeImageSource(filename),
			position, position, {}, false, false});
	}
	jpegview_linux::DisplayPrefetchPlannerWorker planner;
	const std::uint64_t generation = planner.Request(plan);
	Expect(planner.WaitUntilIdle(std::chrono::seconds(3)),
		"SVG neighbor dimensions planning did not finish before its deadline");
	const auto results = planner.TakeReady();
	Expect(results.size() == 1 && results.front().generation == generation &&
		!results.front().failure.Failed(), "SVG neighbor planner did not publish its current generation");
#if JPEGVIEW_HAVE_SVG
	const auto& result = results.front();
	Expect(result.requests.size() == 3 && result.dimensions.size() == 3,
		"SVG/SVGZ and archive neighbors were omitted or unknown/malformed sources were admitted");
	jpegview_linux::DisplayImageCache display(plan.maximumPreparedBytes, 1);
	for (const auto& request : result.requests) {
		Expect(request.Valid() && !request.decoded && request.cacheKey.renderVectorAtTarget &&
			request.fileBackedFormat == jpegview_linux::FileBackedDisplayFormat::Svg,
			"SVG neighbor prefetch retained an intrinsic raster instead of its vector source");
		const auto selected = jpegview_linux::MakeSvgDisplayImageRequest(request.source,
			request.sourceWidth, request.sourceHeight, request.targetWidth,
			request.targetHeight, false);
		const auto prepared = display.RequestAndWait(request);
		Expect(prepared && prepared->width <= 320 && prepared->height <= 400 &&
			prepared->bgra.size() <= plan.maximumPreparedBytes &&
			selected.key == request.key && display.Find(selected) == prepared,
			"bounded SVG neighbor pixels were unavailable to the selected display request");
	}
	Expect(result.requests[0].sourceWidth == 9000 && result.requests[0].sourceHeight == 10000 &&
		result.requests[1].targetWidth == 128 && result.requests[1].targetHeight == 64 &&
		result.requests[2].source.Key().backingIdentity ==
			jpegview_linux::DescribeImageSource(archive).Key().backingIdentity,
		"SVG neighbor rendering lost intrinsic geometry, vector upscaling, or archive backing identity");
	plan.maximumPreparedBytes = 0;
	for (std::size_t index = 0; index < 2; ++index) {
		plan.retainedTextures.push_back({result.requests[index].key, result.requests[index].cacheKey});
	}
	planner.Request(plan);
	Expect(planner.WaitUntilIdle(std::chrono::seconds(3)),
		"retained SVG neighbor planning did not finish before its deadline");
	const auto retained = planner.TakeReady();
	Expect(retained.size() == 1 && retained.front().requests.empty() &&
		retained.front().protectedTextureKeys.size() == 2,
		"SVG neighbors were redundantly prepared or lost nearest protection with a full budget");
#else
	Expect(results.front().requests.empty() && results.front().dimensions.empty(),
		"neighbor planning admitted SVG sources without the optional decoder");
#endif
}



void TestFailedDisplayRequestResolutionRecovery() {
	jpegview_linux::DisplayImageRequest failedRequest;
	failedRequest.key = "failed-large-display-request";
	failedRequest.targetWidth = 1200;
	failedRequest.targetHeight = 800;
	const jpegview_linux::DisplayImageTarget unchangedResolution =
		jpegview_linux::ClampDisplayImageTarget(1200, 800, 2400, 1600);
	const jpegview_linux::DisplayImageTarget smallerResolution =
		jpegview_linux::ClampDisplayImageTarget(1200, 800, 600, 400);

	Expect(!jpegview_linux::FailedDisplayRequestNeedsNewResolution(
		failedRequest, failedRequest.key, unchangedResolution),
		"unchanged canonical dimensions stopped suppressing a failed request");
	Expect(jpegview_linux::FailedDisplayRequestNeedsNewResolution(
		failedRequest, failedRequest.key, smallerResolution),
		"a smaller canonical target did not invalidate its failed larger request");
	Expect(!jpegview_linux::FailedDisplayRequestNeedsNewResolution(
		failedRequest, "another-failed-request", smallerResolution),
		"a different failed request key invalidated the current display request");
}

void TestDisplayResolutionRequestCanonicalization() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "resolution-planner.jpg";
	WriteText(filename, "captured source descriptor");
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(filename).WithImageProperties(400, 250, false);
	Expect(source.Valid(), "resolution planner fixture has no source descriptor");

	const auto sourceResolution = jpegview_linux::MakeJpegDisplayImageRequest(source,
		400, 250, 400, 250, false);
	const jpegview_linux::DisplayImageTarget enlargedTarget =
		jpegview_linux::ClampDisplayImageTarget(400, 250, 800, 500);
	const auto enlarged = jpegview_linux::MakeJpegDisplayImageRequest(source,
		400, 250, enlargedTarget.width, enlargedTarget.height, false);
	Expect(sourceResolution.Valid() && enlarged.Valid() &&
		sourceResolution.key == enlarged.key && enlarged.targetWidth == 400 &&
		enlarged.targetHeight == 250,
		"enlargement above source resolution created a duplicate display request");

	const auto rotatedSourceResolution = jpegview_linux::MakeJpegDisplayImageRequest(
		source, 400, 250, 250, 400, false, 0, {}, 1);
	const jpegview_linux::DisplayImageTarget rotatedTarget =
		jpegview_linux::ClampDisplayImageTarget(400, 250, 500, 800, 1);
	const auto rotatedEnlarged = jpegview_linux::MakeJpegDisplayImageRequest(
		source, 400, 250, rotatedTarget.width, rotatedTarget.height, false, 0, {}, 1);
	Expect(rotatedSourceResolution.Valid() && rotatedEnlarged.Valid() &&
		rotatedSourceResolution.key == rotatedEnlarged.key &&
		rotatedEnlarged.targetWidth == 250 && rotatedEnlarged.targetHeight == 400,
		"rotated spread enlargement did not reuse its source-resolution request");
	const auto histogramRequest = jpegview_linux::MakeJpegDisplayImageRequest(
		source, 400, 250, 400, 250, false, 0, {}, 0, true);
	std::size_t estimatedHistogramBytes = 0;
	Expect(jpegview_linux::EstimateDisplayImageBytes(histogramRequest,
		estimatedHistogramBytes) &&
		estimatedHistogramBytes == 400u * 250u * 4u +
			sizeof(jpegview_linux::GrayscaleSpectrum),
		"prepared-frame estimate omitted retained histogram data");
	const auto fittedRequest = jpegview_linux::MakeJpegDisplayImageRequest(
		source, 400, 250, 100, 63, false);
	const auto processingChangedRequest = jpegview_linux::MakeJpegDisplayImageRequest(
		source, 400, 250, 400, 250, false, 0,
		jpegview_linux::ImageProcessingParams{0.1});
	Expect(jpegview_linux::CanReuseDisplayImageRepresentation(
		fittedRequest.cacheKey, sourceResolution.cacheKey) &&
		!jpegview_linux::CanReuseDisplayImageRepresentation(
			sourceResolution.cacheKey, fittedRequest.cacheKey) &&
		!jpegview_linux::CanReuseDisplayImageRepresentation(
			fittedRequest.cacheKey, processingChangedRequest.cacheKey) &&
		!jpegview_linux::CanReuseDisplayImageRepresentation(
			fittedRequest.cacheKey, histogramRequest.cacheKey) &&
		!jpegview_linux::CanReuseDisplayImageRepresentation(
			fittedRequest.cacheKey, rotatedSourceResolution.cacheKey),
		"larger representation reuse ignored required detail, processing, histogram, or orientation");
}


void TestDisplayPrefetchUsesFittedResolution() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "fitted-prefetch.jpg";
	const fs::path secondFilename = temporary.path() / "fitted-prefetch-second.jpg";
	WriteText(filename, "captured source descriptor");
	WriteText(secondFilename, "captured source descriptor");
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(filename).WithImageProperties(400, 250, false);
	const jpegview_linux::SourceDescriptor secondSource =
		jpegview_linux::DescribeImageSource(secondFilename).WithImageProperties(400, 250, false);
	jpegview_linux::DisplayPrefetchCandidate candidate{filename, source, 1, 1,
		{}, false, true};
	jpegview_linux::DisplayPrefetchCandidate secondCandidate{secondFilename,
		secondSource, 2, 2, {}, false, true};
	jpegview_linux::DisplayPrefetchPlannerRequest plan;
	plan.currentIndex = 0;
	plan.pageCount = 3;
	plan.maximumCount = 2;
	plan.imageAreaWidth = 100;
	plan.imageAreaHeight = 80;
	plan.maximumPreparedBytes = 100u * 63u * 4u;
	jpegview_linux::Viewport navigationViewport;
	navigationViewport.SetFitRelativeZoomMode(true);
	navigationViewport.Fit(400, 250, 100, 80);
	navigationViewport.ZoomAt(2.0, 50, 40, 400, 250, 100, 80);
	Expect(!navigationViewport.NavigationSnapshot().fitToWindow &&
		navigationViewport.PrefetchSnapshot().fitToWindow,
		"fit-relative navigation zoom was lost or leaked into the prefetch fit snapshot");
	jpegview_linux::Viewport pendingFitViewport;
	pendingFitViewport.Fit(400, 250, 100, 80);
	pendingFitViewport.Fit(0, 0, 100, 80, true, false);
	const jpegview_linux::ViewportSnapshot pendingFit =
		pendingFitViewport.PrefetchSnapshot();
	Expect(pendingFit.fitToWindow && pendingFit.fillWithCrop && !pendingFit.noEnlarge,
		"fit settings changed before dimensions arrived were omitted from neighbor planning");
	plan.viewport = navigationViewport.NavigationSnapshot();
	plan.neighbors = {candidate, secondCandidate};
	jpegview_linux::DisplayPrefetchPlannerWorker planner(
		[](const jpegview_linux::SourceDescriptor&, int&, int&, std::string&,
			const jpegview_linux::DisplayPrefetchPlannerWorker::Continue&) {
			return false;
		});
	const std::uint64_t generation = planner.Request(plan);
	Expect(planner.WaitUntilIdle(std::chrono::seconds(2)),
		"fitted neighbor planning did not finish before its deadline");
	const auto results = planner.TakeReady();
	Expect(results.size() == 1 && results.front().generation == generation &&
		results.front().requests.size() == 1,
		"neighbor planner did not enforce its estimated prepared-frame byte budget");
	const jpegview_linux::DisplayImageRequest& neighbor = results.front().requests.front();
	Expect(neighbor.targetWidth == 100 && neighbor.targetHeight == 63,
		"neighbor prefetch inherited the selected image's magnified viewport");
	std::size_t estimatedBytes = 0;
	Expect(jpegview_linux::EstimateDisplayImageBytes(neighbor, estimatedBytes) &&
		estimatedBytes == plan.maximumPreparedBytes &&
		neighbor.filename == filename && results.front().protectedTextureKeys.size() == 1,
		"neighbor admission did not use its actual fitted-frame byte estimate");
	const auto secondRetained = jpegview_linux::MakeJpegDisplayImageRequest(
		secondSource, 400, 250, neighbor.targetWidth, neighbor.targetHeight, false, 2);
	plan.retainedTextures = {{secondRetained.key, secondRetained.cacheKey}};
	const std::uint64_t fullBudgetGeneration = planner.Request(plan);
	Expect(planner.WaitUntilIdle(std::chrono::seconds(2)),
		"full-budget nearest-neighbor planning did not finish before its deadline");
	const auto fullBudgetResults = planner.TakeReady();
	Expect(fullBudgetResults.size() == 1 &&
		fullBudgetResults.front().generation == fullBudgetGeneration &&
		fullBudgetResults.front().requests.size() == 1 &&
		fullBudgetResults.front().requests.front().key == neighbor.key &&
		fullBudgetResults.front().protectedTextureKeys.size() == 2 &&
		std::find(fullBudgetResults.front().protectedTextureKeys.begin(),
			fullBudgetResults.front().protectedTextureKeys.end(), secondRetained.key) !=
			fullBudgetResults.front().protectedTextureKeys.end() &&
		fullBudgetResults.front().protectedTextureCacheKeys.size() == 2 &&
		std::find(fullBudgetResults.front().protectedTextureCacheKeys.begin(),
			fullBudgetResults.front().protectedTextureCacheKeys.end(),
			secondRetained.cacheKey) !=
			fullBudgetResults.front().protectedTextureCacheKeys.end(),
		"budget exhaustion skipped protection for a retained second nearest texture");
	const auto retainedHigherResolution = jpegview_linux::MakeJpegDisplayImageRequest(
		source, 400, 250, 400, 250, false, 1);
	plan.maximumCount = 2;
	plan.neighbors = {candidate, secondCandidate};
	plan.maximumPreparedBytes = 2u * 100u * 63u * 4u;
	plan.retainedTextures = {{retainedHigherResolution.key,
		retainedHigherResolution.cacheKey}};
	const std::uint64_t higherResolutionGeneration = planner.Request(plan);
	Expect(planner.WaitUntilIdle(std::chrono::seconds(2)),
		"higher-resolution retained-neighbor planning did not finish before its deadline");
	const auto higherResolutionResults = planner.TakeReady();
	Expect(higherResolutionResults.size() == 1 &&
		higherResolutionResults.front().generation == higherResolutionGeneration &&
		higherResolutionResults.front().requests.size() == 1 &&
		higherResolutionResults.front().requests.front().filename == secondFilename &&
		higherResolutionResults.front().protectedTextureKeys.size() == 2 &&
		std::find(higherResolutionResults.front().protectedTextureKeys.begin(),
			higherResolutionResults.front().protectedTextureKeys.end(),
			retainedHigherResolution.key) !=
			higherResolutionResults.front().protectedTextureKeys.end() &&
		std::find(higherResolutionResults.front().protectedTextureCacheKeys.begin(),
			higherResolutionResults.front().protectedTextureCacheKeys.end(),
			retainedHigherResolution.cacheKey) !=
			higherResolutionResults.front().protectedTextureCacheKeys.end(),
		"fitted planning duplicated or failed to protect a retained source-resolution neighbor");
	plan.maximumPreparedBytes = 0;
	plan.retainedTextures = {{secondRetained.key, secondRetained.cacheKey}};
	const std::uint64_t zeroBudgetGeneration = planner.Request(plan);
	Expect(planner.WaitUntilIdle(std::chrono::seconds(2)),
		"zero-budget nearest-neighbor planning did not finish before its deadline");
	const auto zeroBudgetResults = planner.TakeReady();
	Expect(zeroBudgetResults.size() == 1 &&
		zeroBudgetResults.front().generation == zeroBudgetGeneration &&
		zeroBudgetResults.front().requests.empty() &&
		zeroBudgetResults.front().protectedTextureKeys.size() == 1 &&
		zeroBudgetResults.front().protectedTextureKeys.front() == secondRetained.key,
		"zero prepared-byte budget skipped protection for a retained nearest texture");
	plan.maximumCount = 1;
	plan.neighbors.resize(1);
	plan.maximumPreparedBytes = 100u * 63u * 4u;
	plan.retainedTextures = {{neighbor.key, neighbor.cacheKey}};
	const std::uint64_t retainedGeneration = planner.Request(plan);
	Expect(planner.WaitUntilIdle(std::chrono::seconds(2)),
		"retained nearest-neighbor planning did not finish before its deadline");
	const auto retainedResults = planner.TakeReady();
	Expect(retainedResults.size() == 1 &&
		retainedResults.front().generation == retainedGeneration &&
		retainedResults.front().requests.empty() &&
		retainedResults.front().protectedTextureKeys.size() == 1 &&
		retainedResults.front().protectedTextureKeys.front() == neighbor.key &&
		retainedResults.front().protectedTextureCacheKeys.size() == 1 &&
		retainedResults.front().protectedTextureCacheKeys.front() == neighbor.cacheKey,
		"retained nearest texture was not protected when its replacement request was skipped");

	const fs::path decodedNeighborPath = temporary.path() / "decoded-neighbor.png";
	WriteText(decodedNeighborPath, "decoded neighbor source descriptor");
	const jpegview_linux::SourceDescriptor decodedNeighborSource =
		jpegview_linux::DescribeImageSource(decodedNeighborPath).WithImageProperties(
			400, 250, false);
	auto decodedNeighbor = std::make_shared<jpegview_linux::DecodedImage>();
	decodedNeighbor->frames.push_back({400, 250, std::vector<std::uint8_t>(
		400u * 250u * 4u), false, 0});
	const auto decodedFitRequest = jpegview_linux::MakeDisplayImageRequest(
		decodedNeighborSource, decodedNeighbor, 0, 100, 63, false, 1);
	const auto decodedSourceResolution = jpegview_linux::MakeDisplayImageRequest(
		decodedNeighborSource, decodedNeighbor, 0, 400, 250, false, 1);
	const std::vector<jpegview_linux::RetainedDisplayTexture> decodedRetainedTextures = {
		{decodedSourceResolution.key, decodedSourceResolution.cacheKey}};
	const jpegview_linux::RetainedDisplayTexture* decodedReuse =
		jpegview_linux::FindReusableRetainedDisplayTexture(decodedFitRequest.cacheKey,
			decodedRetainedTextures);
	Expect(decodedFitRequest.Valid() && decodedSourceResolution.Valid() &&
		decodedNeighborPath.extension() == ".png" && decodedReuse != nullptr &&
		decodedReuse->key == decodedSourceResolution.key &&
		decodedReuse->cacheKey == decodedSourceResolution.cacheKey &&
		jpegview_linux::CanReuseDisplayImageRepresentation(decodedFitRequest.cacheKey,
			decodedReuse->cacheKey),
		"decoded non-JPEG neighbor did not reuse and protect its retained higher-resolution texture");
}

void TestCurrentDisplayPanAndZoomRequestReuse() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "pan-reuse.jpg";
	WriteText(filename, "captured source descriptor");
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(filename).WithImageProperties(400, 250, false);
	jpegview_linux::Viewport viewport;
	viewport.SetFitRelativeZoomMode(false);
	viewport.Fit(400, 250, 100, 80);
	std::atomic<std::size_t> preparations{0};
	jpegview_linux::DisplayImageCache cache(1024 * 1024, 1,
		[&preparations](const jpegview_linux::DisplayImageRequest& request) {
			++preparations;
			return DisplayCachePreparedTestImage(request);
		});
	const auto prepareOnce = [&](const jpegview_linux::DisplayImageRequest& request) {
		if (cache.Find(request)) return;
		cache.Request(request);
		Expect(cache.WaitUntilIdle(std::chrono::seconds(2)),
			"display preparation did not finish before its deadline");
	};
	const auto requestForViewport = [&] {
		const auto destination = viewport.Destination(400, 250, 100, 80);
		const jpegview_linux::DisplayImageTarget resolution =
			jpegview_linux::ClampDisplayImageTarget(400, 250,
				destination.width, destination.height);
		return jpegview_linux::MakeJpegDisplayImageRequest(source, 400, 250,
			resolution.width, resolution.height, false);
	};
	const auto fit = requestForViewport();
	prepareOnce(fit);
	viewport.ActualSize();
	const auto actual = requestForViewport();
	prepareOnce(actual);
	viewport.ZoomAt(1.1, 50, 40, 400, 250, 100, 80);
	const auto zoomBurst = requestForViewport();
	viewport.ZoomAt(2.0, 50, 40, 400, 250, 100, 80);
	const auto enlarged = requestForViewport();
	prepareOnce(enlarged);
	viewport.Pan(12.0, -8.0);
	const auto panned = requestForViewport();
	prepareOnce(panned);
	viewport.Fit(400, 250, 100, 80);
	prepareOnce(requestForViewport());
	Expect(preparations.load() == 2,
		"fit, actual-size, enlargement, pan, or fit return repeated sufficient preparation");
	const auto changedProcessing = jpegview_linux::MakeJpegDisplayImageRequest(source,
		400, 250, 400, 250, false, 0,
		jpegview_linux::ImageProcessingParams{0.1});
	prepareOnce(changedProcessing);
	Expect(preparations.load() == 3,
		"changed processing did not request a distinct prepared frame");
	Expect(zoomBurst.key == actual.key && enlarged.key == actual.key &&
		panned.key == actual.key && cache.Find(changedProcessing),
		"zoom burst and pan did not reuse source pixels or preserve processing identity");
}

void TestRotatedSpreadPartnerResolutionAndAdmission() {
	TemporaryDirectory temporary;
	const fs::path anchorPath = temporary.path() / "spread-anchor.jpg";
	const fs::path partnerPath = temporary.path() / "spread-partner.jpg";
	WriteText(anchorPath, "captured anchor source descriptor");
	WriteText(partnerPath, "captured partner source descriptor");
	const jpegview_linux::SourceDescriptor anchorSource =
		jpegview_linux::DescribeImageSource(anchorPath).WithImageProperties(600, 900, false);
	const jpegview_linux::SourceDescriptor partnerSource =
		jpegview_linux::DescribeImageSource(partnerPath).WithImageProperties(600, 1200, false);
	const jpegview_linux::PageDimensions anchorDimensions{600, 900};
	const jpegview_linux::PageDimensions partnerDimensions{600, 1200};
	const auto spread = jpegview_linux::BuildDoublePageSpread(1, 3,
		anchorDimensions, partnerDimensions, {true, false}, true, 1);
	Expect(spread.has_value(), "rotated source-resolution fixture did not form a spread");

	jpegview_linux::Viewport viewport;
	viewport.Restore({false, false, true, 2.0, 2.0}, spread->canvasWidth,
		spread->canvasHeight, 2800, 3000);
	const jpegview_linux::ViewportRect canvas = viewport.Destination(
		spread->canvasWidth, spread->canvasHeight, 2800, 3000);
	const auto placementSize = [&](const jpegview_linux::SpreadPagePlacement& page) {
		const int left = static_cast<int>(std::lround(
			static_cast<double>(canvas.width) * page.x / spread->canvasWidth));
		const int top = static_cast<int>(std::lround(
			static_cast<double>(canvas.height) * page.y / spread->canvasHeight));
		const int right = static_cast<int>(std::lround(
			static_cast<double>(canvas.width) * (page.x + page.width) /
				spread->canvasWidth));
		const int bottom = static_cast<int>(std::lround(
			static_cast<double>(canvas.height) * (page.y + page.height) /
				spread->canvasHeight));
		return jpegview_linux::DisplayImageTarget{right - left, bottom - top};
	};
	const jpegview_linux::DisplayImageTarget enlargedPartner = placementSize(spread->nextPage);
	const jpegview_linux::DisplayImageTarget canonicalPartner =
		jpegview_linux::ClampDisplayImageTarget(partnerDimensions.width,
			partnerDimensions.height, enlargedPartner.width, enlargedPartner.height,
			spread->clockwiseQuarterTurns);
	Expect(enlargedPartner.width > partnerDimensions.height &&
		enlargedPartner.height > partnerDimensions.width &&
		canonicalPartner.width == partnerDimensions.height &&
		canonicalPartner.height == partnerDimensions.width,
		"zoomed rotated spread partner was not clamped to its oriented source resolution");
	const auto partnerAtZoom = jpegview_linux::MakeJpegDisplayImageRequest(
		partnerSource, partnerDimensions.width, partnerDimensions.height,
		canonicalPartner.width, canonicalPartner.height, false, 1, {},
		spread->clockwiseQuarterTurns);
	const jpegview_linux::DisplayImageTarget stillLargerPartner =
		jpegview_linux::ClampDisplayImageTarget(partnerDimensions.width,
			partnerDimensions.height, enlargedPartner.width * 2,
			enlargedPartner.height * 2, spread->clockwiseQuarterTurns);
	const auto partnerAtLargerZoom = jpegview_linux::MakeJpegDisplayImageRequest(
		partnerSource, partnerDimensions.width, partnerDimensions.height,
		stillLargerPartner.width, stillLargerPartner.height, false, 1, {},
		spread->clockwiseQuarterTurns);
	Expect(partnerAtZoom.Valid() && partnerAtLargerZoom.Valid() &&
		stillLargerPartner.width == canonicalPartner.width &&
		stillLargerPartner.height == canonicalPartner.height &&
		partnerAtZoom.key == partnerAtLargerZoom.key,
		"additional spread enlargement created another rotated partner request");

	const jpegview_linux::DisplayImageTarget enlargedAnchor = placementSize(spread->currentPage);
	const jpegview_linux::DisplayImageTarget canonicalAnchor =
		jpegview_linux::ClampDisplayImageTarget(anchorDimensions.width,
			anchorDimensions.height, enlargedAnchor.width, enlargedAnchor.height);
	const auto anchorRequest = jpegview_linux::MakeJpegDisplayImageRequest(anchorSource,
		anchorDimensions.width, anchorDimensions.height, canonicalAnchor.width,
		canonicalAnchor.height, false);
	std::size_t anchorBytes = 0;
	std::size_t partnerBytes = 0;
	Expect(jpegview_linux::EstimateDisplayImageBytes(anchorRequest, anchorBytes) &&
		jpegview_linux::EstimateDisplayImageBytes(partnerAtZoom, partnerBytes),
		"canonical spread frame sizes could not be estimated");
	const std::size_t canonicalPairBytes = anchorBytes + partnerBytes;
	const std::size_t rawPartnerBytes = static_cast<std::size_t>(enlargedPartner.width) *
		static_cast<std::size_t>(enlargedPartner.height) * 4;
	jpegview_linux::PresentationController presentation;
	const auto decision = presentation.PlanSpreadRequests({true, anchorBytes,
		partnerBytes, canonicalPairBytes, false});
	Expect(rawPartnerBytes > canonicalPairBytes &&
		decision.action == jpegview_linux::SpreadRequestAction::StartSpreadRequests,
		"spread admission rejected source-resolution textures based on enlarged destination bytes");

	const fs::path budgetPartnerPath = temporary.path() / "spread-budget-partner.jpg";
	WriteText(budgetPartnerPath, "captured budget partner source descriptor");
	const jpegview_linux::SourceDescriptor budgetPartnerSource =
		jpegview_linux::DescribeImageSource(budgetPartnerPath).WithImageProperties(
			600, 900, false);
	const auto actualSizeAnchor = jpegview_linux::MakeJpegDisplayImageRequest(
		anchorSource, 600, 900, 600, 900, false);
	const auto fittedAnchor = jpegview_linux::MakeJpegDisplayImageRequest(
		anchorSource, 600, 900, 400, 600, false);
	const auto fittedPartner = jpegview_linux::MakeJpegDisplayImageRequest(
		budgetPartnerSource, 600, 900, 400, 600, false);
	std::size_t actualSizeAnchorBytes = 0;
	std::size_t fittedAnchorBytes = 0;
	std::size_t fittedPartnerBytes = 0;
	Expect(jpegview_linux::EstimateDisplayImageBytes(actualSizeAnchor,
		actualSizeAnchorBytes) && jpegview_linux::EstimateDisplayImageBytes(
			fittedAnchor, fittedAnchorBytes) &&
		jpegview_linux::EstimateDisplayImageBytes(fittedPartner, fittedPartnerBytes),
		"actual-size to fitted spread fixture sizes could not be estimated");
	const std::size_t spreadBudget = 2500u * 1024u;
	const auto actualSizeSpread = presentation.PlanSpreadRequests({true,
		actualSizeAnchorBytes, fittedPartnerBytes, spreadBudget, false});
	const auto fittedSpread = presentation.PlanSpreadRequests({true,
		fittedAnchorBytes, fittedPartnerBytes, spreadBudget, false});
	Expect(actualSizeAnchorBytes + fittedPartnerBytes > spreadBudget &&
		fittedAnchorBytes + fittedPartnerBytes <= spreadBudget &&
		actualSizeSpread.action ==
			jpegview_linux::SpreadRequestAction::ReturnToSinglePage &&
		fittedSpread.action == jpegview_linux::SpreadRequestAction::StartSpreadRequests,
		"actual-size anchor admission did not permit retrying with a fitted anchor after returning to fit");
}

void TestDisplayPreparationControllerOwnsGenerationTaggedCompletions() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "controller-neighbor.jpg";
	WriteText(filename, "captured JPEG descriptor");
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(filename);
	std::atomic<int> dimensionReads{0};
	jpegview_linux::DisplayPreparationController controller(
		[&dimensionReads](const jpegview_linux::SourceDescriptor&, int& width, int& height,
			std::string&, const jpegview_linux::DisplayPrefetchPlannerWorker::Continue& keepGoing) {
			if (!keepGoing()) return false;
			++dimensionReads;
			width = 640;
			height = 480;
			return keepGoing();
		});

	jpegview_linux::DisplayPrefetchPlannerRequest plan;
	plan.catalogRevision = 4;
	plan.descriptorRevision = 7;
	plan.viewportRevision = controller.AdvanceViewportRevision();
	plan.currentIndex = 0;
	plan.pageCount = 2;
	plan.imageAreaWidth = 800;
	plan.imageAreaHeight = 600;
	plan.maximumCount = 1;
	plan.neighbors.push_back({filename, source, 1, 1, {}, false, true});
	const std::uint64_t planGeneration = controller.RequestPlan(plan);
	Expect(planGeneration != 0 && controller.WaitUntilPlanIdle(std::chrono::seconds(2)),
		"display-preparation controller did not complete its captured plan");
	const auto planned = controller.TakePlanResults();
	Expect(planned.size() == 1 && planned.front().generation == planGeneration &&
		planned.front().dimensions.size() == 1 && dimensionReads.load() == 1,
		"display-preparation controller lost the planner generation or dimensions result");

	const std::uint64_t firstBatch = controller.BeginRequestBatch();
	const auto channel = controller.RequestChannel();
	jpegview_linux::DisplayImageRequest firstRequest;
	firstRequest.key = "first-generation-request";
	Expect(firstBatch != 0 && channel && channel->Publish(firstBatch, firstRequest),
		"active display batch rejected a generation-matched request completion");
	const auto firstReady = controller.TakeRequestBatch(firstBatch);
	Expect(firstReady.size() == 1 && firstReady.front().key == firstRequest.key,
		"event-thread drain did not transfer the owned request completion");

	const std::uint64_t replacementBatch = controller.BeginRequestBatch();
	jpegview_linux::DisplayImageRequest staleRequest;
	staleRequest.key = "stale-generation-request";
	jpegview_linux::DisplayImageRequest replacementRequest;
	replacementRequest.key = "replacement-generation-request";
	Expect(replacementBatch > firstBatch && !channel->Publish(firstBatch, staleRequest) &&
		channel->Publish(replacementBatch, replacementRequest),
		"replaced batch accepted stale work or rejected its current generation");
	controller.CancelRequestBatch(replacementBatch);
	Expect(controller.TakeRequestBatch(replacementBatch).empty() &&
		!channel->Publish(replacementBatch, replacementRequest),
		"canceled batch published or retained a late completion");
	const std::uint64_t boundedBatch = controller.BeginRequestBatch();
	bool acceptedAllBoundedRequests = boundedBatch != 0;
	for (std::size_t index = 0;
		index < jpegview_linux::kMaximumDisplayPreparationCompletions; ++index) {
		jpegview_linux::DisplayImageRequest request;
		request.key = "bounded-request-" + std::to_string(index);
		acceptedAllBoundedRequests = acceptedAllBoundedRequests &&
			channel->Publish(boundedBatch, std::move(request));
	}
	jpegview_linux::DisplayImageRequest overflowRequest;
	overflowRequest.key = "overflow-request";
	Expect(acceptedAllBoundedRequests &&
		!channel->Publish(boundedBatch, std::move(overflowRequest)) &&
		controller.TakeRequestBatch(boundedBatch).size() ==
			jpegview_linux::kMaximumDisplayPreparationCompletions,
		"display-preparation channel exceeded its explicit completion bound");
	controller.Shutdown();
	Expect(!channel->Publish(boundedBatch, replacementRequest),
		"a retained worker channel accepted work after explicit shutdown");
}

void TestViewportInvalidationRetainsCurrentActiveSpreadBatch() {
	jpegview_linux::WorkBatchGate activeSpreadGate;
	jpegview_linux::WorkBatchGate speculativePlannerGate;
	std::mutex activeMutex;
	std::condition_variable activeChanged;
	bool activeReaderStarted = false;
	bool releaseActiveReader = false;
	int activeDimensionsPublications = 0;
	int activeWidth = 0;
	int activeHeight = 0;
	std::uint64_t currentSpreadGeneration = 19;
	const std::uint64_t capturedSpreadGeneration = currentSpreadGeneration;
	std::thread blockedDimensionsReader([&] {
		{
			std::unique_lock<std::mutex> lock(activeMutex);
			activeReaderStarted = true;
			activeChanged.notify_all();
			activeChanged.wait(lock, [&] { return releaseActiveReader; });
		}
		(void)activeSpreadGate.Publish([&] {
			if (currentSpreadGeneration != capturedSpreadGeneration) return;
			activeWidth = 640;
			activeHeight = 480;
			++activeDimensionsPublications;
		});
	});

	std::mutex plannerMutex;
	std::condition_variable plannerChanged;
	bool plannerCompletionCopied = false;
	bool releasePlannerCompletion = false;
	int stalePlannerPublications = 0;
	std::uint64_t currentPlannerGeneration = 31;
	const std::uint64_t capturedPlannerGeneration = currentPlannerGeneration;
	std::thread blockedPlannerCompletion([&] {
		{
			std::unique_lock<std::mutex> lock(plannerMutex);
			plannerCompletionCopied = true;
			plannerChanged.notify_all();
			plannerChanged.wait(lock, [&] { return releasePlannerCompletion; });
		}
		(void)speculativePlannerGate.Publish([&] {
			if (currentPlannerGeneration == capturedPlannerGeneration) {
				++stalePlannerPublications;
			}
		});
	});

	bool activeReaderBlocked = false;
	{
		std::unique_lock<std::mutex> lock(activeMutex);
		activeReaderBlocked = activeChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return activeReaderStarted; });
	}
	bool plannerCompletionBlocked = false;
	{
		std::unique_lock<std::mutex> lock(plannerMutex);
		plannerCompletionBlocked = plannerChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return plannerCompletionCopied; });
	}
	for (int invalidation = 0; invalidation < 3; ++invalidation) {
		if (jpegview_linux::ShouldDeactivateDisplayPrefetchBatch(
			jpegview_linux::DisplayPrefetchBatchOwner::ActiveSpread,
			jpegview_linux::DisplayPrefetchBatchInvalidation::ViewportChanged,
			currentSpreadGeneration == capturedSpreadGeneration)) {
			activeSpreadGate.Deactivate();
		}
		if (jpegview_linux::ShouldDeactivateDisplayPrefetchBatch(
			jpegview_linux::DisplayPrefetchBatchOwner::NeighborPlanner,
			jpegview_linux::DisplayPrefetchBatchInvalidation::ViewportChanged, false)) {
			speculativePlannerGate.Deactivate();
		}
		++currentPlannerGeneration;
	}
	{
		std::lock_guard<std::mutex> lock(activeMutex);
		releaseActiveReader = true;
	}
	activeChanged.notify_all();
	{
		std::lock_guard<std::mutex> lock(plannerMutex);
		releasePlannerCompletion = true;
	}
	plannerChanged.notify_all();
	blockedDimensionsReader.join();
	blockedPlannerCompletion.join();
	Expect(activeReaderBlocked && plannerCompletionBlocked &&
		activeDimensionsPublications == 1 && activeWidth == 640 && activeHeight == 480 &&
		stalePlannerPublications == 0 &&
		currentPlannerGeneration != capturedPlannerGeneration,
		"viewport invalidation dropped current active-spread dimensions or admitted stale planner work");

	jpegview_linux::WorkBatchGate replacedSpreadGate;
	std::mutex replacementMutex;
	std::condition_variable replacementChanged;
	bool replacementReaderStarted = false;
	bool releaseReplacementReader = false;
	int replacedPublications = 0;
	currentSpreadGeneration = 44;
	const std::uint64_t replacedGeneration = currentSpreadGeneration;
	std::thread replacedDimensionsReader([&] {
		{
			std::unique_lock<std::mutex> lock(replacementMutex);
			replacementReaderStarted = true;
			replacementChanged.notify_all();
			replacementChanged.wait(lock, [&] { return releaseReplacementReader; });
		}
		(void)replacedSpreadGate.Publish([&] {
			if (currentSpreadGeneration == replacedGeneration) ++replacedPublications;
		});
	});
	bool replacementReaderBlocked = false;
	{
		std::unique_lock<std::mutex> lock(replacementMutex);
		replacementReaderBlocked = replacementChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return replacementReaderStarted; });
	}
	const bool deactivateReplacement = jpegview_linux::ShouldDeactivateDisplayPrefetchBatch(
		jpegview_linux::DisplayPrefetchBatchOwner::ActiveSpread,
		jpegview_linux::DisplayPrefetchBatchInvalidation::OwnerChanged, false);
	if (deactivateReplacement) replacedSpreadGate.Deactivate();
	++currentSpreadGeneration;
	{
		std::lock_guard<std::mutex> lock(replacementMutex);
		releaseReplacementReader = true;
	}
	replacementChanged.notify_all();
	replacedDimensionsReader.join();
	Expect(replacementReaderBlocked && deactivateReplacement && replacedPublications == 0,
		"source replacement allowed a blocked active-spread dimensions result to publish");
}

void TestDisplayPrefetchRequestMergeCompletionOrders() {
	const auto request = [](const std::string& key) {
		jpegview_linux::DisplayImageRequest value;
		value.key = key;
		return value;
	};
	const std::vector<jpegview_linux::DisplayImageRequest> jpegRequests{
		request("jpeg-forward"), request("jpeg-backward")};
	const std::vector<jpegview_linux::DisplayImageRequest> decodedRequests{
		request("decoded-forward"), request("decoded-backward")};
	const auto preservesBothSets = [](const std::vector<jpegview_linux::DisplayImageRequest>& merged) {
		std::set<std::string> keys;
		for (const auto& item : merged) keys.insert(item.key);
		return merged.size() == 4 && keys.size() == 4 &&
			keys.find("jpeg-forward") != keys.end() &&
			keys.find("jpeg-backward") != keys.end() &&
			keys.find("decoded-forward") != keys.end() &&
			keys.find("decoded-backward") != keys.end();
	};

	std::vector<jpegview_linux::DisplayImageRequest> merged;
	jpegview_linux::AppendDisplayPrefetchRequests(merged, jpegRequests);
	jpegview_linux::AppendDisplayPrefetchRequests(merged, decodedRequests);
	Expect(preservesBothSets(merged),
		"decoded neighbors arriving after JPEG planning replaced the JPEG request set");
	merged.clear();
	jpegview_linux::AppendDisplayPrefetchRequests(merged, decodedRequests);
	jpegview_linux::AppendDisplayPrefetchRequests(merged, jpegRequests);
	Expect(preservesBothSets(merged),
		"JPEG planning arriving after decoded neighbors replaced the decoded request set");
}

void TestExifMetadataWorkerReloadAndStaleResultRejection() {
	TemporaryDirectory temporary;
	const fs::path firstPath = temporary.path() / "metadata-first.jpg";
	const fs::path secondPath = temporary.path() / "metadata-second.jpg";
	WriteText(firstPath, "first JPEG source");
	WriteText(secondPath, "second JPEG source");
	const auto firstSource = jpegview_linux::DescribeImageSource(firstPath);
	const auto secondSource = jpegview_linux::DescribeImageSource(secondPath);
	std::mutex blockerMutex;
	std::condition_variable blockerChanged;
	bool firstReadStarted = false;
	bool releaseFirstRead = false;
	std::atomic<int> reads{0};
	jpegview_linux::ExifMetadataWorker worker(
		[&](const fs::path& filename, jpegview_linux::ExifInfo& info, std::string&) {
			const int read = ++reads;
			if (filename == firstPath) {
				std::unique_lock<std::mutex> lock(blockerMutex);
				firstReadStarted = true;
				blockerChanged.notify_all();
				if (!blockerChanged.wait_for(lock, std::chrono::seconds(2),
					[&] { return releaseFirstRead; })) return false;
			}
			info.hasExif = true;
			info.cameraModel = "metadata-read-" + std::to_string(read);
			if (filename == secondPath) info.dateTime = "2024:05:06 07:08:09";
			return true;
		});
	const std::uint64_t staleGeneration = worker.Request(firstSource);
	{
		std::unique_lock<std::mutex> lock(blockerMutex);
		Expect(blockerChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return firstReadStarted; }),
			"EXIF worker did not enter the injected slow metadata read");
	}
	const std::uint64_t currentGeneration = worker.Request(secondSource);
	jpegview_linux::DeferredExifDateAction deferredDateAction;
	deferredDateAction.Begin(secondPath, secondSource.Key(), currentGeneration);
	Expect(deferredDateAction.MustDeferFor(secondPath, secondSource.Key()) &&
		deferredDateAction.Defer(secondPath, secondSource.Key()),
		"EXIF-date action did not wait for the delayed matching metadata request");
	{
		std::lock_guard<std::mutex> lock(blockerMutex);
		releaseFirstRead = true;
	}
	blockerChanged.notify_all();
	Expect(worker.WaitUntilIdle(std::chrono::seconds(2)),
		"replacement EXIF read did not finish before its deadline");
	auto ready = worker.TakeReady();
	Expect(ready.size() == 1 && ready.front().source == secondSource.Key() &&
		ready.front().generation == currentGeneration &&
		jpegview_linux::IsCurrentExifMetadataResult(ready.front(),
			currentGeneration, secondSource.Key()) &&
		!jpegview_linux::IsCurrentExifMetadataResult(ready.front(),
			staleGeneration, firstSource.Key()),
		"late EXIF data from a previous image could replace the selected image metadata");
	Expect(reads.load() == 2,
		"obsolete EXIF result was allowed to replace the current worker request");
	Expect(ready.front().metadata.dateTime == "2024:05:06 07:08:09",
		"delayed EXIF result did not carry the usable date for the selected image");
	const auto metadataFirst = deferredDateAction.Complete(
		ready.front(), secondPath, secondSource.Key());
	Expect(metadataFirst.matchedPendingRead && !metadataFirst.runDeferredAction &&
		deferredDateAction.MustDeferFor(secondPath, secondSource.Key()),
		"EXIF date changed the source while a cold dimensions read could still be active");
	const auto committedAfterMetadata = deferredDateAction.MarkImageCommitted(
		secondPath, secondSource.Key());
	Expect(committedAfterMetadata.runDeferredAction,
		"valid deferred EXIF-date action did not resume after image commit");

	const std::uint64_t reloadGeneration = worker.Request(secondSource);
	Expect(worker.WaitUntilIdle(std::chrono::seconds(2)),
		"EXIF metadata reload did not finish before its deadline");
	ready = worker.TakeReady();
	Expect(ready.size() == 1 && ready.front().generation == reloadGeneration &&
		ready.front().metadata.cameraModel == "metadata-read-3" && reads.load() == 3,
		"reloading the same source did not refresh its EXIF metadata");

	std::mutex validationMutex;
	std::condition_variable validationChanged;
	bool finalValidationStarted = false;
	bool releaseFinalValidation = false;
	bool eventCallsCompleted = false;
	std::atomic<int> firstSourceValidations{0};
	jpegview_linux::ExifMetadataWorker validationWorker(
		[](const fs::path&, jpegview_linux::ExifInfo& info, std::string&) {
			info.cameraModel = "validated outside the worker lock";
			return true;
		}, [&](const jpegview_linux::SourceDescriptor& source) {
			if (source.Key() == firstSource.Key() && ++firstSourceValidations == 3) {
				std::unique_lock<std::mutex> lock(validationMutex);
				finalValidationStarted = true;
				validationChanged.notify_all();
				if (!validationChanged.wait_for(lock, std::chrono::seconds(2),
					[&] { return releaseFinalValidation; })) return false;
			}
			return true;
		});
	validationWorker.Request(firstSource);
	{
		std::unique_lock<std::mutex> lock(validationMutex);
		Expect(validationChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return finalValidationStarted; }),
			"EXIF worker did not enter its final source identity validation");
	}
	std::thread eventFacingCalls([&] {
		validationWorker.Cancel();
		validationWorker.Request(secondSource);
		(void)validationWorker.TakeReady();
		{
			std::lock_guard<std::mutex> lock(validationMutex);
			eventCallsCompleted = true;
		}
		validationChanged.notify_all();
	});
	bool eventCallsReturnedBeforeValidation = false;
	{
		std::unique_lock<std::mutex> lock(validationMutex);
		eventCallsReturnedBeforeValidation = validationChanged.wait_for(lock,
			std::chrono::milliseconds(750), [&] { return eventCallsCompleted; });
		releaseFinalValidation = true;
	}
	validationChanged.notify_all();
	eventFacingCalls.join();
	Expect(eventCallsReturnedBeforeValidation,
		"Request, Cancel, or TakeReady waited for final source identity I/O");
	Expect(validationWorker.WaitUntilIdle(std::chrono::seconds(2)),
		"replacement EXIF request did not finish after final validation was released");
	ready = validationWorker.TakeReady();
	Expect(ready.size() == 1 && ready.front().source == secondSource.Key() &&
		ready.front().metadata.cameraModel == "validated outside the worker lock",
		"slow final identity validation allowed canceled EXIF metadata to publish");

	std::mutex shutdownMutex;
	std::condition_variable shutdownChanged;
	bool shutdownReadStarted = false;
	bool releaseShutdownRead = false;
	jpegview_linux::ExifMetadataWorker shutdownWorker(
		[&](const fs::path&, jpegview_linux::ExifInfo& info, std::string&) {
			std::unique_lock<std::mutex> lock(shutdownMutex);
			shutdownReadStarted = true;
			shutdownChanged.notify_all();
			if (!shutdownChanged.wait_for(lock, std::chrono::seconds(2),
				[&] { return releaseShutdownRead; })) return false;
			info.cameraModel = "canceled-during-shutdown";
			return true;
		});
	shutdownWorker.Request(firstSource);
	{
		std::unique_lock<std::mutex> lock(shutdownMutex);
		Expect(shutdownChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return shutdownReadStarted; }),
			"EXIF shutdown fixture did not enter its reader before the deadline");
	}
	shutdownWorker.Cancel();
	{
		std::lock_guard<std::mutex> lock(shutdownMutex);
		releaseShutdownRead = true;
	}
	shutdownChanged.notify_all();
	shutdownWorker.Stop();
	Expect(shutdownWorker.WaitUntilIdle(std::chrono::seconds(2)) &&
		shutdownWorker.TakeReady().empty(),
		"EXIF worker shutdown waited on the event loop or published canceled metadata");
}

void TestExifArchiveForegroundYieldRetriesCurrentGeneration() {
	TemporaryDirectory temporary;
	const fs::path archive = temporary.path() / "metadata-foreground.zip";
	const fs::path payload = temporary.path() / "metadata-photo.jpg";
	const std::string archiveComment = "archive comment";
	std::vector<std::uint8_t> jpegBytes{
		0xff, 0xd8, 0xff, 0xfe, 0x00,
		static_cast<std::uint8_t>(archiveComment.size() + 2)};
	for (unsigned char byte : archiveComment) jpegBytes.push_back(byte);
	jpegBytes.insert(jpegBytes.end(), {0xff, 0xd9});
	WriteBytes(payload, jpegBytes);
	WriteZipArchive(archive, {{"photo.jpg", payload}});
	const fs::path member = archive / "photo.jpg";
	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(member);
	Expect(source.Valid() && source.Metadata().archiveMember,
		"EXIF foreground-yield fixture did not capture an archive-member descriptor");

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const bool coordinatorInitiallyIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(2));
	Expect(coordinatorInitiallyIdle,
		"EXIF foreground-yield fixture started with unrelated source work active");

	std::mutex readerMutex;
	std::condition_variable readerChanged;
	bool foregroundRaisedByFirstRead = false;
	std::atomic<int> reads{0};
	jpegview_linux::ExifMetadataWorker worker(
		[&](const fs::path& filename, jpegview_linux::ExifInfo& info,
			std::string& comment) {
			const int read = ++reads;
			if (read == 1) {
				coordinator.SetForegroundPending(true);
				{
					std::lock_guard<std::mutex> lock(readerMutex);
					foregroundRaisedByFirstRead = true;
				}
				readerChanged.notify_all();
			}
			return jpegview_linux::ReadJpegMetadata(filename, info, comment);
		});
	struct ClearForegroundGateOnExit {
		jpegview_linux::SourceWorkCoordinator& coordinator;
		~ClearForegroundGateOnExit() { coordinator.SetForegroundPending(false); }
	} clearForegroundGate{coordinator};

	const std::uint64_t generation = worker.Request(source);
	{
		std::unique_lock<std::mutex> lock(readerMutex);
		Expect(readerChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return foregroundRaisedByFirstRead; }),
			"EXIF reader did not raise foreground demand before returning");
	}
	const bool admissionReleasedWhileForegroundPending = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.foregroundPending && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 &&
				snapshot.waitingSpeculative == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(2));
	coordinator.SetForegroundPending(false);
	const bool completed = worker.WaitUntilIdle(std::chrono::seconds(3));
	const auto ready = worker.TakeReady();
	Expect(admissionReleasedWhileForegroundPending,
		"EXIF archive metadata retained paired admission while foreground work was pending");
	Expect(completed && ready.size() == 1 &&
		ready.front().generation == generation && ready.front().source == source.Key() &&
		ready.front().metadataAvailable &&
		ready.front().imageComment == archiveComment &&
		!ready.front().failure.Failed() && reads.load() == 2,
		"current archive EXIF request did not retry and publish after its foreground yield");
}

void TestExifArchiveForegroundYieldCanBeReplacedOrStopped() {
	TemporaryDirectory temporary;
	const fs::path archive = temporary.path() / "metadata-cancel.zip";
	const fs::path payload = temporary.path() / "metadata-cancel.jpg";
	const fs::path replacementPath = temporary.path() / "metadata-replacement.jpg";
	WriteText(payload, "archive metadata source");
	WriteText(replacementPath, "replacement metadata source");
	WriteZipArchive(archive, {{"photo.jpg", payload}});
	const fs::path member = archive / "photo.jpg";
	const auto archiveSource = jpegview_linux::DescribeImageSource(member);
	const auto replacementSource = jpegview_linux::DescribeImageSource(replacementPath);
	Expect(archiveSource.Valid() && archiveSource.Metadata().archiveMember &&
		replacementSource.Valid(),
		"EXIF cancellation fixture did not capture its archive and replacement sources");

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const bool coordinatorInitiallyIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(2));
	Expect(coordinatorInitiallyIdle,
		"EXIF cancellation fixture started with unrelated source work active");

	struct ClearForegroundGateOnExit {
		jpegview_linux::SourceWorkCoordinator& coordinator;
		~ClearForegroundGateOnExit() { coordinator.SetForegroundPending(false); }
	} clearForegroundGate{coordinator};
	std::mutex readerMutex;
	std::condition_variable readerChanged;
	bool firstArchiveReaderStarted = false;
	bool releaseFirstArchiveReader = false;
	bool firstArchiveReadRaisedForeground = false;
	std::atomic<int> archiveReads{0};
	std::atomic<int> replacementReads{0};
	jpegview_linux::ExifMetadataWorker worker(
		[&](const fs::path& filename, jpegview_linux::ExifInfo& info, std::string&) {
			if (filename == member) {
				if (++archiveReads == 1) {
					{
						std::unique_lock<std::mutex> lock(readerMutex);
						firstArchiveReaderStarted = true;
						readerChanged.notify_all();
						if (!readerChanged.wait_for(lock, std::chrono::seconds(2),
							[&] { return releaseFirstArchiveReader; })) return false;
					}
					coordinator.SetForegroundPending(true);
					{
						std::lock_guard<std::mutex> lock(readerMutex);
						firstArchiveReadRaisedForeground = true;
					}
					readerChanged.notify_all();
				}
				info.cameraModel = "obsolete archive metadata";
			} else {
				++replacementReads;
				info.cameraModel = "replacement metadata";
			}
			info.hasExif = true;
			return true;
		});
	const std::uint64_t obsoleteGeneration = worker.Request(archiveSource);
	{
		std::unique_lock<std::mutex> lock(readerMutex);
		Expect(readerChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return firstArchiveReaderStarted; }),
			"EXIF reader did not start before foreground demand was raised");
		releaseFirstArchiveReader = true;
	}
	readerChanged.notify_all();
	{
		std::unique_lock<std::mutex> lock(readerMutex);
		Expect(readerChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return firstArchiveReadRaisedForeground; }),
			"EXIF worker did not reach the foreground-yield reader barrier");
	}
	const bool oldAdmissionReleased = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.foregroundPending && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 &&
				snapshot.waitingSpeculative == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(2));
	const bool oldGenerationRemainedActiveWhileYielded =
		!worker.WaitUntilIdle(std::chrono::milliseconds(20));
	const std::uint64_t replacementGeneration = worker.Request(replacementSource);
	const bool replacementStayedPendingWhileForeground =
		!worker.WaitUntilIdle(std::chrono::milliseconds(20)) && replacementReads.load() == 0;
	coordinator.SetForegroundPending(false);
	const bool replacementCompleted = worker.WaitUntilIdle(std::chrono::seconds(3));
	const auto replacementReady = worker.TakeReady();
	Expect(oldAdmissionReleased && oldGenerationRemainedActiveWhileYielded &&
		replacementStayedPendingWhileForeground && replacementCompleted &&
		replacementReady.size() == 1 && replacementReady.front().generation ==
			replacementGeneration && replacementReady.front().source == replacementSource.Key() &&
		replacementReady.front().metadata.cameraModel == "replacement metadata" &&
		archiveReads.load() == 1 && replacementReads.load() == 1 &&
		obsoleteGeneration != replacementGeneration,
		"replacement did not cancel a yielded EXIF generation and publish only current metadata");

	std::mutex shutdownMutex;
	std::condition_variable shutdownChanged;
	bool shutdownReaderStarted = false;
	bool releaseShutdownReader = false;
	bool shutdownReaderRaisedForeground = false;
	jpegview_linux::ExifMetadataWorker shutdownWorker(
		[&](const fs::path&, jpegview_linux::ExifInfo& info, std::string&) {
			{
				std::unique_lock<std::mutex> lock(shutdownMutex);
				shutdownReaderStarted = true;
				shutdownChanged.notify_all();
				if (!shutdownChanged.wait_for(lock, std::chrono::seconds(2),
					[&] { return releaseShutdownReader; })) return false;
			}
			coordinator.SetForegroundPending(true);
			{
				std::lock_guard<std::mutex> lock(shutdownMutex);
				shutdownReaderRaisedForeground = true;
			}
			shutdownChanged.notify_all();
			info.hasExif = true;
			return true;
		});
	shutdownWorker.Request(archiveSource);
	{
		std::unique_lock<std::mutex> lock(shutdownMutex);
		Expect(shutdownChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return shutdownReaderStarted; }),
			"EXIF shutdown reader did not start before foreground demand was raised");
		releaseShutdownReader = true;
	}
	shutdownChanged.notify_all();
	{
		std::unique_lock<std::mutex> lock(shutdownMutex);
		Expect(shutdownChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return shutdownReaderRaisedForeground; }),
			"EXIF shutdown fixture did not raise foreground demand");
	}
	const bool shutdownAdmissionReleased = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.foregroundPending && snapshot.activeSpeculative == 0 &&
				snapshot.activeCpu == 0 &&
				snapshot.waitingSpeculative == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(2));
	const bool shutdownWorkerRemainedActiveWhileYielded =
		!shutdownWorker.WaitUntilIdle(std::chrono::milliseconds(20));
	bool shutdownReturned = false;
	std::thread stopThread([&] {
		shutdownWorker.Stop();
		{
			std::lock_guard<std::mutex> lock(shutdownMutex);
			shutdownReturned = true;
		}
		shutdownChanged.notify_all();
	});
	bool shutdownCompletedWithoutGateRelease = false;
	{
		std::unique_lock<std::mutex> lock(shutdownMutex);
		shutdownCompletedWithoutGateRelease = shutdownChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return shutdownReturned; });
	}
	if (!shutdownCompletedWithoutGateRelease) coordinator.SetForegroundPending(false);
	stopThread.join();
	coordinator.SetForegroundPending(false);
	Expect(shutdownAdmissionReleased && shutdownWorkerRemainedActiveWhileYielded &&
		shutdownCompletedWithoutGateRelease && shutdownWorker.WaitUntilIdle(
			std::chrono::seconds(2)) && shutdownWorker.TakeReady().empty(),
		"EXIF worker shutdown did not wake and cancel a foreground-yielded generation");
}

void TestExifMetadataWorkerPublishesSourceUnavailable() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "vanishing-metadata.jpg";
	WriteText(filename, "source before removal");
	const auto source = jpegview_linux::DescribeImageSource(filename);
	fs::remove(filename);
	std::atomic<int> readerCalls{0};
	jpegview_linux::ExifMetadataWorker worker(
		[&](const fs::path&, jpegview_linux::ExifInfo& info, std::string&) {
			++readerCalls;
			info.hasExif = true;
			return true;
		});
	const std::uint64_t generation = worker.Request(source);
	const bool completed = worker.WaitUntilIdle(std::chrono::seconds(2));
	const auto ready = worker.TakeReady();
	Expect(completed && ready.size() == 1 && ready.front().generation == generation &&
		ready.front().source == source.Key() &&
		ready.front().failure.kind == jpegview_linux::WorkerFailureKind::SourceUnavailable &&
		!ready.front().metadataAvailable && readerCalls.load() == 0,
		"EXIF worker did not publish a terminal source-unavailable result");
}

void TestExifMetadataWorkerPublishesCurrentExceptions() {
	TemporaryDirectory temporary;
	const fs::path throwingPath = temporary.path() / "throwing-reader.jpg";
	const fs::path admissionPath = temporary.path() / "throwing-admission.jpg";
	WriteText(throwingPath, "reader exception source");
	WriteText(admissionPath, "admission exception source");
	const auto throwingSource = jpegview_linux::DescribeImageSource(throwingPath);
	const auto admissionSource = jpegview_linux::DescribeImageSource(admissionPath);
	std::atomic<int> readerCalls{0};
	jpegview_linux::ExifMetadataWorker readerWorker(
		[&](const fs::path& filename, jpegview_linux::ExifInfo& info,
			std::string& comment) {
			++readerCalls;
			if (filename == throwingPath) {
				info.hasExif = true;
				info.cameraModel = "partial metadata before exception";
				comment = "partial comment before exception";
				throw std::runtime_error("injected EXIF reader failure");
			}
			info.hasExif = true;
			info.cameraModel = "later request succeeded";
			return true;
		});
	const std::uint64_t throwingGeneration = readerWorker.Request(throwingSource);
	const bool readerFailureBecameIdle = readerWorker.WaitUntilIdle(std::chrono::seconds(2));
	auto readerFailure = readerWorker.TakeReady();
	const bool readerFailurePublished = readerFailure.size() == 1 &&
		readerFailure.front().generation == throwingGeneration &&
		readerFailure.front().source == throwingSource.Key() &&
		readerFailure.front().failure.kind == jpegview_linux::WorkerFailureKind::Exception &&
		!readerFailure.front().metadataAvailable &&
		!readerFailure.front().metadata.hasExif &&
		readerFailure.front().metadata.cameraModel.empty() &&
		readerFailure.front().imageComment.empty();
	const std::uint64_t readerRetryGeneration = readerWorker.Request(admissionSource);
	const bool readerRetryBecameIdle = readerWorker.WaitUntilIdle(std::chrono::seconds(2));
	auto readerRetry = readerWorker.TakeReady();
	const bool readerRetryPublished = readerRetry.size() == 1 &&
		readerRetry.front().generation == readerRetryGeneration &&
		readerRetry.front().source == admissionSource.Key() &&
		readerRetry.front().metadataAvailable &&
		readerRetry.front().metadata.cameraModel == "later request succeeded" &&
		!readerRetry.front().failure.Failed() && readerCalls.load() == 2;
	std::atomic<int> sourceValidationCalls{0};
	jpegview_linux::ExifMetadataWorker validatorWorker(
		[](const fs::path&, jpegview_linux::ExifInfo& info, std::string&) {
			info.hasExif = true;
			info.cameraModel = "metadata cleared after validator exception";
			return true;
		}, [&](const jpegview_linux::SourceDescriptor& source) {
			if (source.Key() == throwingSource.Key() && ++sourceValidationCalls == 3) {
				throw std::runtime_error("injected EXIF source validator failure");
			}
			return true;
		});
	const std::uint64_t validatorGeneration = validatorWorker.Request(throwingSource);
	const bool validatorFailureBecameIdle = validatorWorker.WaitUntilIdle(
		std::chrono::seconds(2));
	auto validatorFailure = validatorWorker.TakeReady();
	const bool validatorFailurePublished = validatorFailure.size() == 1 &&
		validatorFailure.front().generation == validatorGeneration &&
		validatorFailure.front().source == throwingSource.Key() &&
		validatorFailure.front().failure.kind == jpegview_linux::WorkerFailureKind::Exception &&
		!validatorFailure.front().metadataAvailable &&
		!validatorFailure.front().metadata.hasExif &&
		validatorFailure.front().metadata.cameraModel.empty() &&
		validatorFailure.front().imageComment.empty();
	const std::uint64_t validatorRetryGeneration = validatorWorker.Request(admissionSource);
	const bool validatorRetryBecameIdle = validatorWorker.WaitUntilIdle(
		std::chrono::seconds(2));
	auto validatorRetry = validatorWorker.TakeReady();
	const bool validatorRetryPublished = validatorRetry.size() == 1 &&
		validatorRetry.front().generation == validatorRetryGeneration &&
		validatorRetry.front().metadataAvailable &&
		validatorRetry.front().metadata.cameraModel ==
			"metadata cleared after validator exception";

	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	coordinator.SetForegroundPending(false);
	const bool coordinatorInitiallyIdle = coordinator.WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 0 && snapshot.activeSpeculative == 0 &&
				snapshot.waitingForeground == 0 && snapshot.waitingSpeculative == 0 &&
				snapshot.activeCpu == 0 && snapshot.waitingCpu == 0;
		}, std::chrono::seconds(2));
	SourceWorkFailureInjection injection{
		jpegview_linux::detail::SourceWorkTestHookPoint::InitialSourceQueueRegistration};
	coordinator.SetTestHookForTesting(ThrowAtSourceWorkHook, &injection);
	std::atomic<int> admissionReaderCalls{0};
	jpegview_linux::ExifMetadataWorker admissionWorker(
		[&](const fs::path&, jpegview_linux::ExifInfo& info, std::string&) {
			++admissionReaderCalls;
			info.hasExif = true;
			info.cameraModel = "admission retry succeeded";
			return true;
		});
	const std::uint64_t admissionGeneration = admissionWorker.Request(admissionSource);
	const bool admissionFailureBecameIdle = admissionWorker.WaitUntilIdle(
		std::chrono::seconds(2));
	auto admissionFailure = admissionWorker.TakeReady();
	const bool admissionFailurePublished = admissionFailure.size() == 1 &&
		admissionFailure.front().generation == admissionGeneration &&
		admissionFailure.front().source == admissionSource.Key() &&
		admissionFailure.front().failure.kind == jpegview_linux::WorkerFailureKind::Exception &&
		!admissionFailure.front().metadataAvailable &&
		!admissionFailure.front().metadata.hasExif &&
		admissionFailure.front().metadata.cameraModel.empty() &&
		admissionFailure.front().imageComment.empty() && admissionReaderCalls.load() == 0;
	coordinator.SetTestHookForTesting(nullptr, nullptr);
	const std::uint64_t admissionRetryGeneration = admissionWorker.Request(admissionSource);
	const bool admissionRetryBecameIdle = admissionWorker.WaitUntilIdle(
		std::chrono::seconds(2));
	auto admissionRetry = admissionWorker.TakeReady();
	const bool admissionRetryPublished = admissionRetry.size() == 1 &&
		admissionRetry.front().generation == admissionRetryGeneration &&
		admissionRetry.front().source == admissionSource.Key() &&
		admissionRetry.front().metadataAvailable &&
		admissionRetry.front().metadata.cameraModel == "admission retry succeeded" &&
		!admissionRetry.front().failure.Failed() && admissionReaderCalls.load() == 1;
	const auto finalSnapshot = coordinator.Snapshot();
	const bool admissionReleased = finalSnapshot.activeForeground == 0 &&
		finalSnapshot.activeSpeculative == 0 && finalSnapshot.activeCpu == 0 &&
		finalSnapshot.waitingForeground == 0 && finalSnapshot.waitingSpeculative == 0 &&
		finalSnapshot.waitingCpu == 0;
	std::ostringstream failure;
	failure << "current EXIF reader/admission exceptions did not publish clean terminal results or accept later work"
		<< " (readerIdle=" << readerFailureBecameIdle
		<< ", readerPublished=" << readerFailurePublished
		<< ", readerRetryIdle=" << readerRetryBecameIdle
		<< ", readerRetryPublished=" << readerRetryPublished
		<< ", validatorIdle=" << validatorFailureBecameIdle
		<< ", validatorPublished=" << validatorFailurePublished
		<< ", validatorRetryIdle=" << validatorRetryBecameIdle
		<< ", validatorRetryPublished=" << validatorRetryPublished
		<< ", coordinatorIdle=" << coordinatorInitiallyIdle
		<< ", admissionHook=" << injection.fired.load()
		<< ", admissionIdle=" << admissionFailureBecameIdle
		<< ", admissionPublished=" << admissionFailurePublished
		<< ", admissionRetryIdle=" << admissionRetryBecameIdle
		<< ", admissionRetryPublished=" << admissionRetryPublished
		<< ", admissionReleased=" << admissionReleased << ')';
	Expect(readerFailureBecameIdle && readerFailurePublished && readerRetryBecameIdle &&
		readerRetryPublished && validatorFailureBecameIdle && validatorFailurePublished &&
		validatorRetryBecameIdle && validatorRetryPublished && coordinatorInitiallyIdle &&
		injection.fired.load() &&
		admissionFailureBecameIdle && admissionFailurePublished &&
		admissionRetryBecameIdle && admissionRetryPublished && admissionReleased,
		failure.str());
}
const TestCase kTests[] = {
	{"image-content-format-detection", &TestImageContentFormatDetection},
	{"svg-and-svgz-decoding", &TestSvgAndSvgzDecoding},
	{"image-writer-decoder-round-trips", &TestImageWriterDecoderRoundTrips},
	{"content-dispatch-preserves-container-policy", &TestContentDispatchPreservesContainerPolicy},
#if JPEGVIEW_HAVE_WEBP
	{"photo-sized-lossy-vp8-webp-decode", &TestPhotoSizedLossyVp8WebPDecode},
#else
	{"jpeg-named-webp-reports-unavailable-codec", &TestJpegNamedWebPReportsUnavailableCodec},
#endif
	{"jpeg-writer-error-recovery", &TestJpegWriterErrorRecovery},
	{"pnm-variants", &TestPnmVariants},
	{"animated-image-decoders", &TestAnimatedImageDecoders},
	{"direct-codec-source-read-attribution", &TestDirectCodecSourceReadAttribution},
	{"decoder-failures", &TestDecoderFailures},
	{"jpeg-display-decode-scaling", &TestJpegDisplayDecodeScaling},
	{"perf-context-and-pending-input-diagnostics", &TestPerfContextAndPendingInputDiagnostics},
	{"decoded-prefetch-work-class-attribution", &TestDecodedPrefetchWorkClassAttribution},
	{"display-prefetch-planner-worker-snapshots-and-cancellation", &TestDisplayPrefetchPlannerWorkerSnapshotsAndCancellation},
	{"svg-neighbor-prefetch-uses-target-rendering", &TestSvgNeighborPrefetchUsesTargetRendering},
	{"display-resolution-request-canonicalization", &TestDisplayResolutionRequestCanonicalization},
	{"failed-display-request-resolution-recovery", &TestFailedDisplayRequestResolutionRecovery},
	{"display-prefetch-uses-fitted-resolution", &TestDisplayPrefetchUsesFittedResolution},
	{"current-display-pan-and-zoom-request-reuse", &TestCurrentDisplayPanAndZoomRequestReuse},
	{"rotated-spread-partner-resolution-and-admission", &TestRotatedSpreadPartnerResolutionAndAdmission},
	{"display-preparation-controller-generation-channel", &TestDisplayPreparationControllerOwnsGenerationTaggedCompletions},
	{"viewport-invalidation-retains-current-active-spread-batch", &TestViewportInvalidationRetainsCurrentActiveSpreadBatch},
	{"display-prefetch-request-merge-completion-orders", &TestDisplayPrefetchRequestMergeCompletionOrders},
	{"exif-metadata-worker-reload-and-stale-result-rejection", &TestExifMetadataWorkerReloadAndStaleResultRejection},
	{"exif-archive-foreground-yield-retries-current-generation", &TestExifArchiveForegroundYieldRetriesCurrentGeneration},
	{"exif-archive-foreground-yield-can-be-replaced-or-stopped", &TestExifArchiveForegroundYieldCanBeReplacedOrStopped},
	{"exif-metadata-worker-publishes-source-unavailable", &TestExifMetadataWorkerPublishesSourceUnavailable},
	{"exif-metadata-worker-publishes-current-exceptions", &TestExifMetadataWorkerPublishesCurrentExceptions},
};

} // namespace

const TestSuite& GetCodecDisplaySuite() {
	static const TestSuite suite{"codec_display", kTests, sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
