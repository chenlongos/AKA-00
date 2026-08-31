# =============================================================================
# csrc — C++ 硬件控制库（src/ 的 C++ 移植）
#
# 本机 (macOS/Linux) 开发构建；板上交叉编译用 Makefile.cross
#
# 用法:
#   make            # 编译 libcsrc.a
#   make clean
# =============================================================================

CXX      ?= g++
AR       ?= ar
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -MMD -MP
ARFLAGS   = rcs

# 摄像头 JPEG（Linux 主机需要 jpeglib.h；macOS 走 stub，无需）
JPEG_DIR ?= ../third_party/jpeg
JPEG_INC := $(if $(wildcard $(JPEG_DIR)/include/jpeglib.h),-I$(JPEG_DIR)/include,)
CXXFLAGS += $(JPEG_INC)

SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:src/%.cpp=build/%.o)
LIB  := build/libcsrc.a
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

.DEFAULT_GOAL := all
.PHONY: all clean

all: $(LIB)

build:
	mkdir -p build

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $(OBJS)
	@echo "  ✓ libcsrc.a built"

clean:
	rm -rf build
