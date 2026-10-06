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
ZIP_CFLAGS ?= $(shell pkg-config --cflags libzip 2>/dev/null)
ZIP_LIBS ?= $(shell pkg-config --libs libzip 2>/dev/null || echo -lzip)
ARCHIVE_CFLAGS ?= $(shell pkg-config --cflags libarchive 2>/dev/null)
ARCHIVE_LIBS ?= $(shell pkg-config --libs libarchive 2>/dev/null || echo -larchive)
SEVENZIP_SOURCE_ROOT ?=
RAR_BACKEND_ROOT ?=
CARGO ?= cargo
# libraw's pkg-config file redundantly adds its C++ runtime. Let the compiler's
# static-runtime switches control that dependency instead of forcing an
# additional shared libstdc++ link on the executable.
RAW_LIBS := $(filter-out -lstdc++,$(RAW_LIBS))

CODEC_CFLAGS := $(GIF_CFLAGS) $(TIFF_CFLAGS) $(WEBP_CFLAGS) $(HEIF_CFLAGS) \
	$(AVIF_CFLAGS) $(JXL_CFLAGS) $(JXR_CFLAGS) $(RAW_CFLAGS) $(LCMS2_CFLAGS) \
	$(ZIP_CFLAGS) $(ARCHIVE_CFLAGS)
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
	$(AVIF_LIBS) $(JXL_LIBS) $(JXR_LIBS) $(RAW_LIBS) $(LCMS2_LIBS) \
	$(ZIP_LIBS) $(ARCHIVE_LIBS)
STATIC_RUNTIME ?= -static-libstdc++ -static-libgcc

BUILD_DIR ?= build
SANITIZER_BUILD_DIR ?= build-sanitize
SANITIZER_CXXFLAGS ?= -O1 -g -std=c++17 -Wall -Wextra -Wpedantic -fno-omit-frame-pointer -fsanitize=address,undefined
VERSION ?= $(shell sh ./version.sh)
TARGET := $(BUILD_DIR)/jpegview-linux
TEST_TARGET := $(BUILD_DIR)/jpegview-tests
ifneq ($(strip $(SEVENZIP_SOURCE_ROOT)),)
SEVENZIP_BUNDLE_DIR := $(SEVENZIP_SOURCE_ROOT)/CPP/7zip/Bundles/Format7zF
SEVENZIP_CPPFLAGS := -I"$(SEVENZIP_SOURCE_ROOT)/CPP"
SEVENZIP_BACKEND_SOURCE := src/seven_zip_backend_7zip.cpp
SEVENZIP_PLUGIN := $(BUILD_DIR)/lib/jpegview-linux/7z.so
SEVENZIP_PLUGIN_DEPENDENCY := $(SEVENZIP_PLUGIN)
else
SEVENZIP_CPPFLAGS :=
SEVENZIP_BACKEND_SOURCE := src/seven_zip_backend_unavailable.cpp
SEVENZIP_PLUGIN_DEPENDENCY :=
endif
ifneq ($(strip $(RAR_BACKEND_ROOT)),)
RAR_BACKEND_SOURCE := src/rar_backend_ffi.cpp
RAR_BACKEND_PLUGIN := $(BUILD_DIR)/lib/jpegview-linux/librar_backend.so
RAR_BACKEND_PLUGIN_DEPENDENCY := $(RAR_BACKEND_PLUGIN)
RAR_CARGO_TARGET_DIR := $(BUILD_DIR)/rar-cargo
else
RAR_BACKEND_SOURCE := src/rar_backend_unavailable.cpp
RAR_BACKEND_PLUGIN_DEPENDENCY :=
endif
# Shared source ownership is explicit: platform-independent modules are listed once,
# SDL adapters are isolated, and each executable supplies its own entry point.
IMAGE_WRITER_SOURCES := src/image_writer_jpeg_png.cpp src/image_writer_basic.cpp \
	src/image_writer_optional.cpp
IMAGE_DECODER_SOURCES := src/image_decoder_stb.cpp src/image_decoder_jpeg.cpp \
	src/image_decoder_apng.cpp src/image_decoder_builtin.cpp \
	src/image_decoder_optional.cpp
ARCHIVE_SOURCE_SOURCES := src/archive_source_catalog.cpp src/archive_source_readers.cpp
CORE_CPP_SOURCES := \
	src/perf_diagnostics.cpp src/source_work_coordinator.cpp \
	src/interaction_work_policy.cpp src/file_list.cpp \
	src/file_list_scan_worker.cpp src/file_list_sort_worker.cpp \
	src/double_page_model.cpp src/presentation_controller.cpp \
	src/display_prefetch_planner.cpp src/display_preparation_controller.cpp \
	src/image_session_controller.cpp src/display_upload_scheduler.cpp \
	src/event_loop_model.cpp src/modal_event_router.cpp \
	src/image_spectrum_worker.cpp src/image_document.cpp \
	src/image_operation_worker.cpp src/file_operation_service.cpp \
	src/external_process.cpp src/exif_metadata_worker.cpp \
	src/pending_image_intents.cpp src/archive_source.cpp \
	$(ARCHIVE_SOURCE_SOURCES) src/cache_policy.cpp \
	src/archive_password_dialog_model.cpp src/crop_selection_model.cpp \
	src/crop_size_dialog_model.cpp src/go_to_image_number_model.cpp \
	src/pixel_color_sampler.cpp \
	src/zoom_navigator_model.cpp \
	src/magnifying_glass_model.cpp src/advanced_configuration_model.cpp \
	src/image_formats.cpp src/input_commands.cpp src/exif_reader.cpp \
	src/image_writer.cpp $(IMAGE_WRITER_SOURCES) \
	src/image_decoder.cpp $(IMAGE_DECODER_SOURCES) \
	src/cache_budget.cpp src/image_cache.cpp src/display_image_cache.cpp \
	src/image.cpp src/image_processing.cpp src/image_processing_store.cpp \
	src/recent_files.cpp src/clipboard.cpp src/settings.cpp \
	src/transparency_pattern.cpp src/sort_mode.cpp \
	src/desktop_applications.cpp src/desktop_association.cpp \
	src/external_commands.cpp src/batch_copy.cpp src/viewport.cpp \
	src/resize_model.cpp src/context_menu_model.cpp src/overlay_layout.cpp \
	src/viewer_chrome.cpp src/thumbnail_panel_model.cpp src/thumbnail_repository.cpp \
	src/thumbnail_resampler.cpp src/image_info_model.cpp \
	src/spectrum_model.cpp src/file_dialog_model.cpp \
	src/playback_scheduler.cpp src/system_font.cpp src/bitmap_font.cpp
SDL_ADAPTER_CPP_SOURCES := \
	src/text_renderer.cpp src/chrome_renderer.cpp \
	src/context_menu_renderer.cpp src/file_dialog_renderer.cpp \
	src/editing_dialog_renderer.cpp src/renderer_texture_owner.cpp \
	src/app_icon.cpp
BACKEND_CPP_SOURCES := $(SEVENZIP_BACKEND_SOURCE) $(RAR_BACKEND_SOURCE)
APP_CPP_SOURCES := src/main.cpp $(CORE_CPP_SOURCES) \
	$(SDL_ADAPTER_CPP_SOURCES) $(BACKEND_CPP_SOURCES)
TEST_SUITE_CPP_SOURCES := tests/test_main.cpp \
	tests/test_source_navigation.cpp tests/test_work_admission.cpp \
	tests/test_codec_display.cpp tests/test_image_cache.cpp \
	tests/test_image_operations.cpp tests/test_viewer_models.cpp \
	tests/test_dialogs_sessions.cpp
TEST_CPP_SOURCES := $(TEST_SUITE_CPP_SOURCES) $(CORE_CPP_SOURCES) \
	$(SDL_ADAPTER_CPP_SOURCES) $(BACKEND_CPP_SOURCES)
TEST_FIXTURE_FILES := \
	tests/fixtures/data-encrypted.7z tests/fixtures/header-encrypted.7z \
	tests/fixtures/encrypted_rar/private.png \
	tests/fixtures/encrypted_rar/rar4-data-encrypted.rar \
	tests/fixtures/encrypted_rar/rar4-header-encrypted.rar \
	tests/fixtures/encrypted_rar/rar5-data-encrypted.rar \
	tests/fixtures/encrypted_rar/rar5-header-encrypted.rar \
	tests/rar_test_fixtures.h
ICON_SOURCE := ../src/JPEGView/res/JPEGView.ico
GENERATED_ICON_HEADER := $(BUILD_DIR)/jpegview_icon_data.h
BUILD_INFO_HEADER := $(BUILD_DIR)/build-info.h
ARCHIVE_FIXTURE_WRITER := $(BUILD_DIR)/write-7z-fixture
RAR_FIXTURE_WRITER := $(BUILD_DIR)/write-rar-fixture
ENCRYPTED_ZIP_FIXTURE_WRITER := $(BUILD_DIR)/write-encrypted-zip-fixture
APP_OBJECT_DIR := $(BUILD_DIR)/obj/app
TEST_OBJECT_DIR := $(BUILD_DIR)/obj/tests
PERF_OBJECT_DIR := $(BUILD_DIR)/obj/perf
APP_MAIN_OBJECT := $(APP_OBJECT_DIR)/src/main.o
APP_ICON_OBJECT := $(APP_OBJECT_DIR)/src/app_icon.o
TEST_ICON_OBJECT := $(TEST_OBJECT_DIR)/src/app_icon.o
PERF_OBJECT := $(PERF_OBJECT_DIR)/tests/perf_workload.o
PERF_CPP_SOURCES := src/image.cpp src/image_processing.cpp
PERF_SUPPORT_OBJECTS := $(patsubst %.cpp,$(PERF_OBJECT_DIR)/%.o,$(PERF_CPP_SOURCES))
PERF_OBJECTS := $(PERF_SUPPORT_OBJECTS) $(PERF_OBJECT)
PERF_TARGET := $(BUILD_DIR)/jpegview-perf-workload
APP_OBJECTS := $(patsubst %.cpp,$(APP_OBJECT_DIR)/%.o,$(APP_CPP_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,$(TEST_OBJECT_DIR)/%.o,$(TEST_CPP_SOURCES))
APP_DEPFILES := $(APP_OBJECTS:.o=.d)
TEST_DEPFILES := $(TEST_OBJECTS:.o=.d)
PERF_DEPFILES := $(PERF_OBJECT:.o=.d)
PERF_SUPPORT_DEPFILES := $(PERF_SUPPORT_OBJECTS:.o=.d)
APP_CPPFLAGS := $(CODEC_DEFINES) $(SDL2_CFLAGS) $(PANGO_CFLAGS) \
	$(CODEC_CFLAGS) $(SEVENZIP_CPPFLAGS) -I"$(BUILD_DIR)" -Isrc -Ithird_party
TEST_CPPFLAGS := -DJPEGVIEW_CACHE_BUDGET_TEST_HOOKS $(CODEC_DEFINES) \
	$(PANGO_CFLAGS) $(CODEC_CFLAGS) $(SEVENZIP_CPPFLAGS) \
	-I"$(BUILD_DIR)" -Isrc -Ithird_party -Itests
APP_LINK_LIBS := $(LDFLAGS) $(STATIC_RUNTIME) $(SDL2_LIBS) $(PANGO_LIBS) \
	$(IMAGE_LIBS) $(CODEC_LIBS) -ldl -lm -pthread
TEST_LINK_LIBS := $(SDL2_LIBS) $(PANGO_LIBS) $(IMAGE_LIBS) \
	$(CODEC_LIBS) -ldl -lm -pthread
PERF_CPPFLAGS := -Isrc
PERF_LINK_LIBS := -lm
APP_COMPILE_CONFIG_STAMP := $(BUILD_DIR)/config/app-compile.flags
TEST_COMPILE_CONFIG_STAMP := $(BUILD_DIR)/config/tests-compile.flags
APP_LINK_CONFIG_STAMP := $(BUILD_DIR)/config/app-link.flags
TEST_LINK_CONFIG_STAMP := $(BUILD_DIR)/config/tests-link.flags
PERF_COMPILE_CONFIG_STAMP := $(BUILD_DIR)/config/perf-compile.flags
PERF_LINK_CONFIG_STAMP := $(BUILD_DIR)/config/perf-link.flags
ARCHIVE_FIXTURE_CONFIG_STAMP := $(BUILD_DIR)/config/archive-fixture.flags
RAR_FIXTURE_CONFIG_STAMP := $(BUILD_DIR)/config/rar-fixture.flags
ZIP_FIXTURE_CONFIG_STAMP := $(BUILD_DIR)/config/zip-fixture.flags
SEVENZIP_PLUGIN_CONFIG_STAMP := $(BUILD_DIR)/config/sevenzip-plugin.inputs
RAR_PLUGIN_CONFIG_STAMP := $(BUILD_DIR)/config/rar-plugin.inputs
ifneq ($(strip $(SEVENZIP_SOURCE_ROOT)),)
SEVENZIP_PLUGIN_INPUTS := $(sort $(shell find "$(SEVENZIP_SOURCE_ROOT)/CPP" \
	-type f -not -path '*/_o/*' -print 2>/dev/null))
endif
ifneq ($(strip $(RAR_BACKEND_ROOT)),)
RAR_BACKEND_INPUTS := $(sort $(shell find "$(RAR_BACKEND_ROOT)" -type f -name '*.rs' -not -path '*/target/*' -print 2>/dev/null))
endif

.PHONY: all clean run test test-focused test-list test-list-suites test-perf \
	test-ui test-shell test-sanitize test-version check appimage FORCE \
	regen-encrypted-zip-fixture

all: $(TARGET) $(SEVENZIP_PLUGIN_DEPENDENCY) $(RAR_BACKEND_PLUGIN_DEPENDENCY)

FORCE:

shell_quote = '$(subst ','"'"',$(1))'

ifneq ($(strip $(SEVENZIP_SOURCE_ROOT)),)
$(SEVENZIP_PLUGIN_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,source-root=$(SEVENZIP_SOURCE_ROOT)) \
		$(call shell_quote,inputs=$(SEVENZIP_PLUGIN_INPUTS)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(SEVENZIP_PLUGIN): $(SEVENZIP_BUNDLE_DIR)/makefile.gcc \
	$(SEVENZIP_PLUGIN_CONFIG_STAMP) $(SEVENZIP_PLUGIN_INPUTS) \
	$(APP_COMPILE_CONFIG_STAMP)
	$(MAKE) -C "$(SEVENZIP_BUNDLE_DIR)" -f makefile.gcc DISABLE_RAR=1
	@mkdir -p "$(dir $@)"
	cp "$(SEVENZIP_BUNDLE_DIR)/_o/7z.so" "$@"
endif

ifneq ($(strip $(RAR_BACKEND_ROOT)),)
$(RAR_PLUGIN_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,source-root=$(RAR_BACKEND_ROOT)) \
		$(call shell_quote,inputs=$(RAR_BACKEND_INPUTS)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(RAR_BACKEND_PLUGIN): $(RAR_BACKEND_ROOT)/Cargo.toml $(RAR_BACKEND_ROOT)/Cargo.lock \
	$(RAR_BACKEND_ROOT)/rust-toolchain.toml $(RAR_BACKEND_INPUTS) \
	$(RAR_PLUGIN_CONFIG_STAMP) $(APP_COMPILE_CONFIG_STAMP)
	CARGO_HOME="$${CARGO_HOME:-$$HOME/.cargo}" RUSTUP_HOME="$${RUSTUP_HOME:-$$HOME/.rustup}" \
	CARGO_TARGET_DIR="$(abspath $(RAR_CARGO_TARGET_DIR))" \
	$(CARGO) build --manifest-path "$(RAR_BACKEND_ROOT)/Cargo.toml" --release --locked
	@mkdir -p "$(dir $@)"
	cp "$(abspath $(RAR_CARGO_TARGET_DIR))/release/librar_backend.so" "$@"
endif

$(APP_COMPILE_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,CXX=$(CXX)) \
		$(call shell_quote,CPPFLAGS=$(CPPFLAGS)) \
		$(call shell_quote,CXXFLAGS=$(CXXFLAGS)) \
		$(call shell_quote,APP_CPPFLAGS=$(APP_CPPFLAGS)) \
		$(call shell_quote,SEVENZIP_SOURCE_ROOT=$(SEVENZIP_SOURCE_ROOT)) \
		$(call shell_quote,RAR_BACKEND_ROOT=$(RAR_BACKEND_ROOT)) \
		$(call shell_quote,BACKEND_CPP_SOURCES=$(BACKEND_CPP_SOURCES)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(TEST_COMPILE_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,CXX=$(CXX)) \
		$(call shell_quote,CPPFLAGS=$(CPPFLAGS)) \
		$(call shell_quote,CXXFLAGS=$(CXXFLAGS)) \
		$(call shell_quote,TEST_CPPFLAGS=$(TEST_CPPFLAGS)) \
		$(call shell_quote,SEVENZIP_SOURCE_ROOT=$(SEVENZIP_SOURCE_ROOT)) \
		$(call shell_quote,RAR_BACKEND_ROOT=$(RAR_BACKEND_ROOT)) \
		$(call shell_quote,BACKEND_CPP_SOURCES=$(BACKEND_CPP_SOURCES)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(APP_LINK_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,CXX=$(CXX)) \
		$(call shell_quote,CXXFLAGS=$(CXXFLAGS)) \
		$(call shell_quote,APP_LINK_LIBS=$(APP_LINK_LIBS)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(TEST_LINK_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,CXX=$(CXX)) \
		$(call shell_quote,CXXFLAGS=$(CXXFLAGS)) \
		$(call shell_quote,TEST_LINK_LIBS=$(TEST_LINK_LIBS)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(BUILD_INFO_HEADER): version.sh write-build-info.sh FORCE
	@mkdir -p "$(BUILD_DIR)"
	@tmp="$@.tmp"; \
		sh ./write-build-info.sh "$$tmp" "$(VERSION)" && \
		if cmp -s "$$tmp" "$@"; then rm -f "$$tmp"; else mv "$$tmp" "$@"; fi

$(GENERATED_ICON_HEADER): $(ICON_SOURCE) embed-icon.sh
	@mkdir -p "$(BUILD_DIR)"
	./embed-icon.sh "$(ICON_SOURCE)" "$@"

$(APP_MAIN_OBJECT): APP_EXTRA_CPPFLAGS := -include "$(BUILD_INFO_HEADER)"
$(APP_MAIN_OBJECT): $(BUILD_INFO_HEADER)
$(APP_ICON_OBJECT) $(TEST_ICON_OBJECT): $(GENERATED_ICON_HEADER)

$(APP_OBJECT_DIR)/%.o: %.cpp $(APP_COMPILE_CONFIG_STAMP) $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(APP_CPPFLAGS) $(APP_EXTRA_CPPFLAGS) \
		-MMD -MP -MF "$(@:.o=.d)" -MT "$@" -c "$<" -o "$@"

$(TEST_OBJECT_DIR)/%.o: %.cpp $(TEST_COMPILE_CONFIG_STAMP) $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_CPPFLAGS) \
		-MMD -MP -MF "$(@:.o=.d)" -MT "$@" -c "$<" -o "$@"

$(PERF_COMPILE_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,CXX=$(CXX)) \
		$(call shell_quote,CPPFLAGS=$(CPPFLAGS)) \
		$(call shell_quote,CXXFLAGS=$(CXXFLAGS)) \
		$(call shell_quote,PERF_CPPFLAGS=$(PERF_CPPFLAGS)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(PERF_LINK_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,CXX=$(CXX)) \
		$(call shell_quote,CXXFLAGS=$(CXXFLAGS)) \
		$(call shell_quote,PERF_LINK_LIBS=$(PERF_LINK_LIBS)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(ARCHIVE_FIXTURE_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,CXX=$(CXX)) \
		$(call shell_quote,CXXFLAGS=$(CXXFLAGS)) \
		$(call shell_quote,ARCHIVE_CFLAGS=$(ARCHIVE_CFLAGS)) \
		$(call shell_quote,ARCHIVE_LIBS=$(ARCHIVE_LIBS)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(RAR_FIXTURE_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,CXX=$(CXX)) \
		$(call shell_quote,CXXFLAGS=$(CXXFLAGS)) \
		$(call shell_quote,INCLUDE_FLAGS=-Itests) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(ZIP_FIXTURE_CONFIG_STAMP): FORCE $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	@printf '%s\n' \
		$(call shell_quote,CXX=$(CXX)) \
		$(call shell_quote,CXXFLAGS=$(CXXFLAGS)) \
		$(call shell_quote,ZIP_CFLAGS=$(ZIP_CFLAGS)) \
		$(call shell_quote,ZIP_LIBS=$(ZIP_LIBS)) > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

$(PERF_OBJECT_DIR)/%.o: %.cpp $(PERF_COMPILE_CONFIG_STAMP) $(MAKEFILE_LIST)
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PERF_CPPFLAGS) \
		-MMD -MP -MF "$(@:.o=.d)" -MT "$@" -c "$<" -o "$@"

-include $(APP_DEPFILES) $(TEST_DEPFILES) $(PERF_DEPFILES) \
	$(PERF_SUPPORT_DEPFILES)

$(TARGET): $(APP_OBJECTS) $(APP_LINK_CONFIG_STAMP) \
	$(SEVENZIP_PLUGIN_DEPENDENCY) $(RAR_BACKEND_PLUGIN_DEPENDENCY)
	$(CXX) $(CXXFLAGS) $(APP_OBJECTS) -o "$@" $(APP_LINK_LIBS)

run: $(TARGET)
	$(TARGET) $(ARGS)

$(TEST_TARGET): $(TEST_OBJECTS) $(TEST_LINK_CONFIG_STAMP) \
	$(TEST_FIXTURE_FILES) $(SEVENZIP_PLUGIN_DEPENDENCY) \
	$(RAR_BACKEND_PLUGIN_DEPENDENCY)
	$(CXX) $(CXXFLAGS) $(TEST_OBJECTS) -o "$@" $(TEST_LINK_LIBS)

test: $(TEST_TARGET)
	@trace_file="$(abspath $(BUILD_DIR))/perf-trace-core.csv"; \
	JPEGVIEW_PERF_TRACE="$$trace_file" $(TEST_TARGET) && \
	JPEGVIEW_TEST_EXPECT_GIFLIB="$(if $(strip $(GIF_LIBS)),1,0)" \
		sh tests/perf_trace_test.sh "$$trace_file" && \
	sh tests/test_harness_test.sh "$(TEST_TARGET)"

test-focused: $(TEST_TARGET)
	@if [ -n "$(TEST_SUITE)" ]; then \
		if [ -n "$(TEST_FILTER)" ]; then \
			$(TEST_TARGET) --suite "$(TEST_SUITE)" --filter "$(TEST_FILTER)"; \
		else \
			$(TEST_TARGET) --suite "$(TEST_SUITE)"; \
		fi; \
	elif [ -n "$(TEST_FILTER)" ]; then \
		$(TEST_TARGET) --filter "$(TEST_FILTER)"; \
	else \
		echo "Set TEST_SUITE or TEST_FILTER to select focused tests." >&2; exit 2; \
	fi

test-list: $(TEST_TARGET)
	$(TEST_TARGET) --list

test-list-suites: $(TEST_TARGET)
	$(TEST_TARGET) --list-suites

$(PERF_TARGET): $(PERF_OBJECTS) $(PERF_LINK_CONFIG_STAMP)
	$(CXX) $(CXXFLAGS) $(PERF_OBJECTS) -o "$@" $(PERF_LINK_LIBS)

test-perf: $(PERF_TARGET)
	$(PERF_TARGET) $(PERF_ARGS)


$(ARCHIVE_FIXTURE_WRITER): tests/write_7z_fixture.cpp \
	$(ARCHIVE_FIXTURE_CONFIG_STAMP) $(MAKEFILE_LIST)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(ARCHIVE_CFLAGS) $< -o $@ $(ARCHIVE_LIBS)


$(RAR_FIXTURE_WRITER): tests/write_rar_fixture.cpp tests/rar_test_fixtures.h \
	$(RAR_FIXTURE_CONFIG_STAMP) $(MAKEFILE_LIST)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Itests $< -o $@


$(ENCRYPTED_ZIP_FIXTURE_WRITER): tests/write_encrypted_zip_fixture.cpp \
	$(ZIP_FIXTURE_CONFIG_STAMP) $(MAKEFILE_LIST)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(ZIP_CFLAGS) $< -o $@ $(ZIP_LIBS)

regen-encrypted-zip-fixture: $(ENCRYPTED_ZIP_FIXTURE_WRITER)
	$(ENCRYPTED_ZIP_FIXTURE_WRITER) tests/fixtures/encrypted-zip.zip

test-ui: $(TARGET) $(ARCHIVE_FIXTURE_WRITER) $(RAR_FIXTURE_WRITER)
	@version_output=$$($(TARGET) --version); \
	expected_version="JPEGView Linux $(VERSION)"; \
	if [ "$$version_output" != "$$expected_version" ]; then \
		echo "Version metadata mismatch: expected '$$expected_version', got '$$version_output'" >&2; \
		exit 1; \
	fi
	JPEGVIEW_TEST_PERF_TRACE="$(abspath $(BUILD_DIR))/perf-trace-ui.csv" \
	JPEGVIEW_TEST_HAS_7Z_PLUGIN=$(if $(strip $(SEVENZIP_SOURCE_ROOT)),1,0) \
		JPEGVIEW_TEST_HAS_RAR_PLUGIN=$(if $(strip $(RAR_BACKEND_ROOT)),1,0) \
		./tests/ui_smoke.sh $(TARGET) $(ARCHIVE_FIXTURE_WRITER) $(RAR_FIXTURE_WRITER)

test-version:
	sh tests/version_test.sh

test-shell:
	sh -n AppRun retry-command.sh tests/ui_smoke.sh tests/thumbnail_panning_smoke.sh tests/perf_trace_test.sh tests/test_harness_test.sh tests/appimage_x11_smoke.sh tests/version_test.sh tests/docker_build_test.sh tests/appimage_runtime_test.sh tests/retry_command_test.sh package-appimage.sh package-deb.sh fetch-7zip-source.sh install-rust-toolchain.sh fetch-rars-source.sh package-rar-backend-source.sh embed-icon.sh docker-build.sh version.sh write-build-info.sh ../set_env.sh
	$(MAKE) test-version
	sh tests/docker_build_test.sh
	sh tests/appimage_runtime_test.sh
	sh tests/retry_command_test.sh
	bash -n release-assets.sh
	bash tests/release_assets_test.sh
	@if command -v shellcheck >/dev/null 2>&1; then \
		shellcheck AppRun retry-command.sh tests/ui_smoke.sh tests/thumbnail_panning_smoke.sh tests/perf_trace_test.sh tests/test_harness_test.sh tests/appimage_x11_smoke.sh tests/version_test.sh tests/docker_build_test.sh tests/appimage_runtime_test.sh tests/retry_command_test.sh tests/release_assets_test.sh package-appimage.sh package-deb.sh fetch-7zip-source.sh install-rust-toolchain.sh fetch-rars-source.sh package-rar-backend-source.sh embed-icon.sh docker-build.sh version.sh write-build-info.sh release-assets.sh ../set_env.sh; \
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
