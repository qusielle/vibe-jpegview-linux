CXX ?= g++
CXXFLAGS ?= -O2 -pipe -std=c++17 -Wall -Wextra -Wpedantic -DNDEBUG
LDFLAGS ?= -Wl,-rpath,'$$ORIGIN/../lib'

SDL2_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS ?= $(shell pkg-config --libs sdl2 2>/dev/null || echo -l:libSDL2-2.0.so.0)
IMAGE_LIBS ?= -lpng -ljpeg -lz

# The original Windows build routes the less common formats through dedicated
# WIC wrappers.  Linux uses the corresponding native codec libraries.  Every
# codec is discovered independently so a lean local build can still use the
# stb_image fallback when optional development packages are not installed.
GIF_LIBS ?= $(shell pkg-config --libs libgif 2>/dev/null)
TIFF_CFLAGS ?= $(shell pkg-config --cflags libtiff-4 2>/dev/null)
TIFF_LIBS ?= $(shell pkg-config --libs libtiff-4 2>/dev/null)
WEBP_CFLAGS ?= $(shell pkg-config --cflags libwebpdemux 2>/dev/null)
WEBP_LIBS ?= $(shell pkg-config --libs libwebpdemux libwebp 2>/dev/null)
HEIF_CFLAGS ?= $(shell pkg-config --cflags libheif 2>/dev/null)
HEIF_LIBS ?= $(shell pkg-config --libs libheif 2>/dev/null)
AVIF_CFLAGS ?= $(shell pkg-config --cflags libavif 2>/dev/null)
AVIF_LIBS ?= $(shell pkg-config --libs libavif 2>/dev/null)
JXL_CFLAGS ?= $(shell pkg-config --cflags libjxl libjxl_threads 2>/dev/null)
JXL_LIBS ?= $(shell pkg-config --libs libjxl libjxl_threads 2>/dev/null)
JXR_CFLAGS ?= $(shell pkg-config --cflags libjxr 2>/dev/null)
JXR_LIBS ?= $(shell pkg-config --libs libjxr 2>/dev/null)
RAW_CFLAGS ?= $(shell pkg-config --cflags libraw 2>/dev/null)
RAW_LIBS ?= $(shell pkg-config --libs libraw 2>/dev/null)
LCMS2_CFLAGS ?= $(shell pkg-config --cflags lcms2 2>/dev/null)
LCMS2_LIBS ?= $(shell pkg-config --libs lcms2 2>/dev/null)
# libraw's pkg-config file redundantly adds its C++ runtime. Let the compiler's
# static-runtime switches control that dependency instead of forcing an
# additional shared libstdc++ link on the executable.
RAW_LIBS := $(filter-out -lstdc++,$(RAW_LIBS))

CODEC_CFLAGS := $(GIF_CFLAGS) $(TIFF_CFLAGS) $(WEBP_CFLAGS) $(HEIF_CFLAGS) \
	$(AVIF_CFLAGS) $(JXL_CFLAGS) $(JXR_CFLAGS) $(RAW_CFLAGS) $(LCMS2_CFLAGS)
CODEC_DEFINES := $(if $(strip $(GIF_LIBS)),-DJPEGVIEW_HAVE_GIF=1,) \
	$(if $(strip $(TIFF_LIBS)),-DJPEGVIEW_HAVE_TIFF=1,) \
	$(if $(strip $(WEBP_LIBS)),-DJPEGVIEW_HAVE_WEBP=1,) \
	$(if $(strip $(HEIF_LIBS)),-DJPEGVIEW_HAVE_HEIF=1,) \
	$(if $(strip $(AVIF_LIBS)),-DJPEGVIEW_HAVE_AVIF=1,) \
	$(if $(strip $(JXL_LIBS)),-DJPEGVIEW_HAVE_JXL=1,) \
	$(if $(strip $(JXR_LIBS)),-DJPEGVIEW_HAVE_JXR=1,) \
	$(if $(strip $(RAW_LIBS)),-DJPEGVIEW_HAVE_RAW=1,) \
	$(if $(strip $(LCMS2_LIBS)),-DJPEGVIEW_HAVE_LCMS2=1,)
CODEC_LIBS := $(GIF_LIBS) $(TIFF_LIBS) $(WEBP_LIBS) $(HEIF_LIBS) \
	$(AVIF_LIBS) $(JXL_LIBS) $(JXR_LIBS) $(RAW_LIBS) $(LCMS2_LIBS)
STATIC_RUNTIME ?= -static-libstdc++ -static-libgcc

BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/jpegview-linux
SOURCES := src/main.cpp src/file_list.cpp src/file_list.h src/exif_reader.cpp src/exif_reader.h src/image_writer.cpp src/image_writer.h src/image_decoder.cpp src/image_decoder.h src/clipboard.cpp src/clipboard.h src/sdl_abi.h third_party/stb_image.h ../src/JPEGView/resource.h

.PHONY: all clean run appimage

all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CODEC_DEFINES) $(SDL2_CFLAGS) $(CODEC_CFLAGS) -Isrc -Ithird_party src/main.cpp src/file_list.cpp src/exif_reader.cpp src/image_writer.cpp src/image_decoder.cpp src/clipboard.cpp -o $@ $(LDFLAGS) $(STATIC_RUNTIME) $(SDL2_LIBS) $(IMAGE_LIBS) $(CODEC_LIBS) -ldl -lm -pthread

run: $(TARGET)
	$(TARGET) $(ARGS)

appimage: $(TARGET)
	./package-appimage.sh $(VERSION)

clean:
	rm -rf $(BUILD_DIR)
