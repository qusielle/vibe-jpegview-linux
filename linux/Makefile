CXX ?= g++
CXXFLAGS ?= -O2 -pipe -std=c++17 -Wall -Wextra -Wpedantic -DNDEBUG
LDFLAGS ?= -Wl,-rpath,'$$ORIGIN/../lib'

SDL2_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS ?= $(shell pkg-config --libs sdl2 2>/dev/null || echo -l:libSDL2-2.0.so.0)
STATIC_RUNTIME ?= -static-libstdc++ -static-libgcc

BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/jpegview-linux
SOURCES := src/main.cpp src/file_list.cpp src/file_list.h src/sdl_abi.h third_party/stb_image.h ../src/JPEGView/resource.h

.PHONY: all clean run appimage

all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SDL2_CFLAGS) -Isrc -Ithird_party src/main.cpp src/file_list.cpp -o $@ $(LDFLAGS) $(STATIC_RUNTIME) $(SDL2_LIBS) -ldl -lm -pthread

run: $(TARGET)
	$(TARGET) $(ARGS)

appimage: $(TARGET)
	./package-appimage.sh $(VERSION)

clean:
	rm -rf $(BUILD_DIR)
