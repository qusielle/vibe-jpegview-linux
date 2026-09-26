CXX ?= g++
CXXFLAGS ?= -O2 -pipe -std=c++17 -Wall -Wextra -Wpedantic -DNDEBUG
LDFLAGS ?= -Wl,-rpath,'$$ORIGIN/../lib'

SDL2_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS ?= $(shell pkg-config --libs sdl2 2>/dev/null || echo -l:libSDL2-2.0.so.0)
PANGO_CFLAGS ?= $(shell pkg-config --cflags pangoft2)
PANGO_LIBS ?=
IMAGE_LIBS ?= -lpng -ljpeg -lz

# The original Windows build routes the less common formats through dedicated
# WIC wrappers.  Linux uses the corresponding native codec libraries.  Every
# codec is discovered independently so a lean local build can still use the
# stb_image fallback when optional development packages are not installed.
# Ubuntu 20.04's libgif-dev ships headers and libgif.so but no pkg-config
# metadata. Ask the compiler for the library as a fallback so animated GIF
# support does not silently disappear in the compatibility build.
GIF_LIBS ?= $(shell pkg-config --libs libgif 2>/dev/null || { \
	gif_library="$$($(CXX) -print-file-name=libgif.so)"; \
	test "$$gif_library" = libgif.so || echo -lgif; \
})
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
SANITIZER_BUILD_DIR ?= build-sanitize
SANITIZER_CXXFLAGS ?= -O1 -g -std=c++17 -Wall -Wextra -Wpedantic -fno-omit-frame-pointer -fsanitize=address,undefined
VERSION ?= $(shell sh ./version.sh)
BUILD_INFO_HEADER := $(BUILD_DIR)/build-info-$(VERSION).h
TARGET := $(BUILD_DIR)/jpegview-linux
TEST_TARGET := $(BUILD_DIR)/jpegview-tests
SOURCES := src/main.cpp src/file_list.cpp src/file_list.h src/image_formats.cpp src/image_formats.h src/input_commands.cpp src/input_commands.h src/exif_reader.cpp src/exif_reader.h src/image_writer.cpp src/image_writer.h src/image_decoder.cpp src/image_decoder.h src/cache_budget.cpp src/cache_budget.h src/image_cache.cpp src/image_cache.h src/display_image_cache.cpp src/display_image_cache.h src/image.cpp src/image.h src/image_processing.cpp src/image_processing.h src/image_processing_store.cpp src/image_processing_store.h src/settings.cpp src/settings.h src/transparency_pattern.cpp src/transparency_pattern.h src/sort_mode.cpp src/sort_mode.h src/desktop_applications.cpp src/desktop_applications.h src/external_commands.cpp src/external_commands.h src/batch_copy.cpp src/batch_copy.h src/viewport.cpp src/viewport.h src/resize_model.cpp src/resize_model.h src/context_menu_model.cpp src/context_menu_model.h src/overlay_layout.cpp src/overlay_layout.h src/viewer_chrome.cpp src/viewer_chrome.h src/thumbnail_panel_model.cpp src/thumbnail_panel_model.h src/thumbnail_resampler.cpp src/thumbnail_resampler.h src/image_info_model.cpp src/image_info_model.h src/spectrum_model.cpp src/spectrum_model.h src/file_dialog_model.cpp src/file_dialog_model.h src/playback_scheduler.cpp src/playback_scheduler.h src/system_font.cpp src/system_font.h src/bitmap_font.cpp src/bitmap_font.h src/bitmap_font_data.h src/sdl_abi.h third_party/stb_image.h ../src/JPEGView/resource.h
TEST_SOURCES := tests/test_core.cpp src/file_list.cpp src/file_list.h src/image_formats.cpp src/image_formats.h src/input_commands.cpp src/input_commands.h src/exif_reader.cpp src/exif_reader.h src/image_writer.cpp src/image_writer.h src/image_decoder.cpp src/image_decoder.h src/cache_budget.cpp src/cache_budget.h src/image_cache.cpp src/image_cache.h src/display_image_cache.cpp src/display_image_cache.h src/image.cpp src/image.h src/image_processing.cpp src/image_processing.h src/image_processing_store.cpp src/image_processing_store.h src/settings.cpp src/settings.h src/transparency_pattern.cpp src/transparency_pattern.h src/sort_mode.cpp src/sort_mode.h src/desktop_applications.cpp src/desktop_applications.h src/external_commands.cpp src/external_commands.h src/batch_copy.cpp src/batch_copy.h src/viewport.cpp src/viewport.h src/resize_model.cpp src/resize_model.h src/context_menu_model.cpp src/context_menu_model.h src/overlay_layout.cpp src/overlay_layout.h src/viewer_chrome.cpp src/viewer_chrome.h src/thumbnail_panel_model.cpp src/thumbnail_panel_model.h src/thumbnail_resampler.cpp src/thumbnail_resampler.h src/image_info_model.cpp src/image_info_model.h src/spectrum_model.cpp src/spectrum_model.h src/file_dialog_model.cpp src/file_dialog_model.h src/playback_scheduler.cpp src/playback_scheduler.h src/system_font.cpp src/system_font.h src/bitmap_font.cpp src/bitmap_font.h src/bitmap_font_data.h src/sdl_abi.h third_party/stb_image.h ../src/JPEGView/resource.h
SOURCES += src/crop_selection_model.cpp src/crop_selection_model.h
TEST_SOURCES += src/crop_selection_model.cpp src/crop_selection_model.h
SOURCES += src/crop_size_dialog_model.cpp src/crop_size_dialog_model.h
TEST_SOURCES += src/crop_size_dialog_model.cpp src/crop_size_dialog_model.h
SOURCES += src/zoom_navigator_model.cpp src/zoom_navigator_model.h
TEST_SOURCES += src/zoom_navigator_model.cpp src/zoom_navigator_model.h
SOURCES += src/desktop_association.cpp src/desktop_association.h
TEST_SOURCES += src/desktop_association.cpp src/desktop_association.h
ICON_SOURCE := ../src/JPEGView/res/JPEGView.ico
GENERATED_ICON_HEADER := $(BUILD_DIR)/jpegview_icon_data.h
SOURCES += src/app_icon.cpp src/app_icon.h embed-icon.sh $(ICON_SOURCE) $(GENERATED_ICON_HEADER)
TEST_SOURCES += src/app_icon.cpp src/app_icon.h embed-icon.sh $(ICON_SOURCE) $(GENERATED_ICON_HEADER)
SOURCES += $(BUILD_INFO_HEADER)

.PHONY: all clean run test test-ui test-shell test-sanitize test-version check appimage

all: $(TARGET)

$(GENERATED_ICON_HEADER): $(ICON_SOURCE) embed-icon.sh
	@mkdir -p $(BUILD_DIR)
	./embed-icon.sh $(ICON_SOURCE) $@

$(BUILD_INFO_HEADER): version.sh write-build-info.sh
	sh ./write-build-info.sh "$@" "$(VERSION)"

$(TARGET): $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -include $(BUILD_INFO_HEADER) $(CODEC_DEFINES) $(SDL2_CFLAGS) $(PANGO_CFLAGS) $(CODEC_CFLAGS) -I$(BUILD_DIR) -Isrc -Ithird_party src/main.cpp src/file_list.cpp src/crop_selection_model.cpp src/crop_size_dialog_model.cpp src/zoom_navigator_model.cpp src/image_formats.cpp src/input_commands.cpp src/exif_reader.cpp src/image_writer.cpp src/image_decoder.cpp src/cache_budget.cpp src/image_cache.cpp src/display_image_cache.cpp src/image.cpp src/image_processing.cpp src/image_processing_store.cpp src/clipboard.cpp src/settings.cpp src/transparency_pattern.cpp src/sort_mode.cpp src/desktop_applications.cpp src/desktop_association.cpp src/external_commands.cpp src/batch_copy.cpp src/viewport.cpp src/resize_model.cpp src/context_menu_model.cpp src/overlay_layout.cpp src/viewer_chrome.cpp src/thumbnail_panel_model.cpp src/thumbnail_resampler.cpp src/image_info_model.cpp src/spectrum_model.cpp src/file_dialog_model.cpp src/playback_scheduler.cpp src/system_font.cpp src/bitmap_font.cpp src/app_icon.cpp -o $@ $(LDFLAGS) $(STATIC_RUNTIME) $(SDL2_LIBS) $(PANGO_LIBS) $(IMAGE_LIBS) $(CODEC_LIBS) -ldl -lm -pthread
	@for stale_header in "$(BUILD_DIR)"/build-info-*.h; do \
		[ -f "$$stale_header" ] || continue; \
		[ "$$stale_header" = "$(BUILD_INFO_HEADER)" ] || rm -f "$$stale_header"; \
	done

run: $(TARGET)
	$(TARGET) $(ARGS)

$(TEST_TARGET): $(TEST_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CODEC_DEFINES) $(PANGO_CFLAGS) $(CODEC_CFLAGS) -I$(BUILD_DIR) -Isrc -Ithird_party tests/test_core.cpp src/file_list.cpp src/crop_selection_model.cpp src/crop_size_dialog_model.cpp src/zoom_navigator_model.cpp src/image_formats.cpp src/input_commands.cpp src/exif_reader.cpp src/image_writer.cpp src/image_decoder.cpp src/cache_budget.cpp src/image_cache.cpp src/display_image_cache.cpp src/image.cpp src/image_processing.cpp src/image_processing_store.cpp src/settings.cpp src/transparency_pattern.cpp src/sort_mode.cpp src/desktop_applications.cpp src/desktop_association.cpp src/external_commands.cpp src/batch_copy.cpp src/viewport.cpp src/resize_model.cpp src/context_menu_model.cpp src/overlay_layout.cpp src/viewer_chrome.cpp src/thumbnail_panel_model.cpp src/thumbnail_resampler.cpp src/image_info_model.cpp src/spectrum_model.cpp src/file_dialog_model.cpp src/playback_scheduler.cpp src/system_font.cpp src/bitmap_font.cpp src/app_icon.cpp -o $@ $(PANGO_LIBS) $(IMAGE_LIBS) $(CODEC_LIBS) -ldl -lm -pthread

test: $(TEST_TARGET)
	$(TEST_TARGET)

test-ui: $(TARGET)
	@version_output=$$($(TARGET) --version); \
	expected_version="JPEGView Linux $(VERSION)"; \
	if [ "$$version_output" != "$$expected_version" ]; then \
		echo "Version metadata mismatch: expected '$$expected_version', got '$$version_output'" >&2; \
		exit 1; \
	fi
	./tests/ui_smoke.sh $(TARGET)

test-version:
	sh tests/version_test.sh

test-shell:
	sh -n tests/ui_smoke.sh tests/version_test.sh package-appimage.sh package-deb.sh embed-icon.sh docker-build.sh version.sh write-build-info.sh ../set_env.sh
	$(MAKE) test-version
	bash -n release-assets.sh
	@if command -v shellcheck >/dev/null 2>&1; then \
		shellcheck tests/ui_smoke.sh tests/version_test.sh package-appimage.sh package-deb.sh embed-icon.sh docker-build.sh version.sh write-build-info.sh release-assets.sh ../set_env.sh; \
	else \
		echo "ShellCheck: SKIP (shellcheck not installed)"; \
	fi

test-sanitize:
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
		$(MAKE) BUILD_DIR=$(SANITIZER_BUILD_DIR) STATIC_RUNTIME= \
		CXXFLAGS='$(SANITIZER_CXXFLAGS)' test

check: test test-ui test-shell

appimage: $(TARGET)
	./package-appimage.sh $(VERSION)

clean:
	rm -rf $(BUILD_DIR)
