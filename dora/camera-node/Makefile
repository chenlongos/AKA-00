# dora C++ camera node Makefile — macOS (demo / 开发)
#   Linux 交叉编译请用 ../build_release.sh

CXX      := g++
CXXFLAGS := -std=c++17 -O3 -Wall -mmacosx-version-min=15.0
LDFLAGS  := -Wl,-dead_strip
TARGET   := camera-node

SRCS     := main.cc camera_opencv.cpp
OBJS     := main.o camera_opencv.o

# OpenCV (via pkg-config)
OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4 2>/dev/null)
OPENCV_LIBS   := $(shell pkg-config --libs opencv4 2>/dev/null)

# dora C FFI (Rust staticlib) — release
WORKSPACE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))..
DORA_FFI_LIB  := $(WORKSPACE_DIR)/target/release/libdora_c_ffi.a

# macOS frameworks needed by dora
FRAMEWORKS := -framework System -framework CoreFoundation -framework Security \
              -framework IOKit -framework CoreServices -framework Metal \
              -framework Foundation -framework AppKit

INCLUDES := -I. $(OPENCV_CFLAGS)
LIBS     := $(DORA_FFI_LIB) $(OPENCV_LIBS) $(FRAMEWORKS) -lpthread -ldl

.PHONY: all clean

all: $(TARGET)

$(DORA_FFI_LIB):
	cd $(WORKSPACE_DIR) && cargo build -p dora-c-ffi --release

$(TARGET): $(OBJS) $(DORA_FFI_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.cpp camera.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
