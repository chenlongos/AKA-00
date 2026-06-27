# dora C++ camera node Makefile

CXX      := g++
CXXFLAGS := -std=c++17 -O3 -Wall -mmacosx-version-min=15.0
LDFLAGS  := -Wl,-dead_strip
TARGET   := camera-node

# OpenCV (via pkg-config)
OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4 2>/dev/null)
OPENCV_LIBS   := $(shell pkg-config --libs opencv4 2>/dev/null)

# dora C FFI (Rust staticlib) — release
WORKSPACE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))..
DORA_FFI_LIB  := $(WORKSPACE_DIR)/target/release/libdora_c_ffi.a
DORA_INCLUDE  := .

# macOS frameworks needed by dora
FRAMEWORKS := -framework System -framework CoreFoundation -framework Security \
              -framework IOKit -framework CoreServices -framework Metal \
              -framework Foundation -framework AppKit

INCLUDES := -I$(DORA_INCLUDE) $(OPENCV_CFLAGS)
LIBS     := $(DORA_FFI_LIB) $(OPENCV_LIBS) $(FRAMEWORKS) -lpthread -ldl

.PHONY: all clean

all: $(TARGET)

# Build the dora C FFI static library (release)
$(DORA_FFI_LIB):
	cd $(WORKSPACE_DIR) && cargo build -p dora-c-ffi --release

# Compile and link
$(TARGET): main.cc $(DORA_FFI_LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ main.cc $(LIBS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
