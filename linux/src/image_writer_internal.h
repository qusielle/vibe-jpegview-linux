#pragma once

#include "image_writer.h"

namespace jpegview_linux::detail {

bool WriteJpeg(const std::filesystem::path&, const std::uint8_t*, int, int, int,
    std::string&);
bool WritePng(const std::filesystem::path&, const std::uint8_t*, int, int,
    std::string&);
bool WriteBmp(const std::filesystem::path&, const std::uint8_t*, int, int,
    std::string&);
bool WriteTga(const std::filesystem::path&, const std::uint8_t*, int, int,
    std::string&);
bool WriteWebP(const std::filesystem::path&, const std::uint8_t*, int, int,
    const ImageWriteOptions&, std::string&);
bool WritePnm(const std::filesystem::path&, const std::uint8_t*, int, int,
    const std::string&, std::string&);
bool WriteQoi(const std::filesystem::path&, const std::uint8_t*, int, int,
    std::string&);
bool WritePsd(const std::filesystem::path&, const std::uint8_t*, int, int,
    std::string&);
#if JPEGVIEW_HAVE_GIF
bool WriteGif(const std::filesystem::path&, const std::uint8_t*, int, int,
    std::string&);
#endif
#if JPEGVIEW_HAVE_TIFF
bool WriteTiff(const std::filesystem::path&, const std::uint8_t*, int, int,
    std::string&);
#endif
#if JPEGVIEW_HAVE_HEIF
bool WriteHeif(const std::filesystem::path&, const std::uint8_t*, int, int,
    int, bool, std::string&);
#endif
#if JPEGVIEW_HAVE_AVIF
bool WriteAvif(const std::filesystem::path&, const std::uint8_t*, int, int,
    int, std::string&);
#endif
#if JPEGVIEW_HAVE_JXL
bool WriteJxl(const std::filesystem::path&, const std::uint8_t*, int, int,
    int, std::string&);
#endif

} // namespace jpegview_linux::detail
