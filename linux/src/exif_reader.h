#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace jpegview_linux {

// The fields mirror the information selected by JPEGView's CEXIFDisplayCtl.
// The Linux reader intentionally owns its strings so the metadata remains
// valid after the encoded JPEG buffer is released.
struct ExifInfo {
	bool hasExif = false;
	std::string cameraModel;
	std::string imageDescription;
	std::string software;
	std::string userComment;
	std::string acquisitionDate;
	std::string dateTime;
	std::string exposureTime;
	bool hasExposureBias = false;
	double exposureBias = 0.0;
	bool hasFlash = false;
	bool flashFired = false;
	bool hasFocalLength = false;
	double focalLength = 0.0;
	bool hasFNumber = false;
	double fNumber = 0.0;
	int isoSpeed = 0;
	bool hasGps = false;
	std::string gpsLocation;
	bool hasAltitude = false;
	double altitude = 0.0;
};

// Reads JPEG COM and APP1/Exif header segments without consuming compressed
// scan data. Unsupported or malformed metadata is treated as absent; image
// decoding itself is handled by the codec layer.
bool ReadJpegMetadata(const std::filesystem::path& filename, ExifInfo& info,
	std::string& jpegComment);

} // namespace jpegview_linux
