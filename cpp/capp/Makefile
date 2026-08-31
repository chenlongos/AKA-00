# =============================================================================
# capp — C++ Web 服务（app/ 的 C++ 移植）—— 本机开发构建
#
# 注意：本机构建输出 bin/aka-capp-dev（macOS/Linux 版，仅本机调试用）。
# 板子部署用 bin/aka-capp（riscv64 静态）——由 Makefile.cross 构建，
# 两者互不覆盖。
#
# 用法:
#   make                    # 编译 bin/aka-capp-dev（链接 csrc）
#   make CSRC=../csrc       # 指定 csrc 源码路径（默认 ../csrc）
#   make clean
# =============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -MMD -MP -I$(CSRC)/include
LDFLAGS  ?=

CSRC := ../csrc
TARGET  := bin/aka-capp-dev

# 摄像头 JPEG（Linux 主机需要 jpeglib.h + -ljpeg；macOS 走 stub，无需）。
# 注意：third_party/jpeg 是 riscv64 交叉产物，本机链接不要用它——Linux 主机装
# libjpeg-dev 后加 `make LDFLAGS=-ljpeg` 即可。
JPEG_DIR ?= ../third_party/jpeg
JPEG_INC := $(if $(wildcard $(JPEG_DIR)/include/jpeglib.h),-I$(JPEG_DIR)/include,)
CXXFLAGS += $(JPEG_INC)

SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:src/%.cpp=build/%.o)
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

.DEFAULT_GOAL := all
.PHONY: all clean

all: $(TARGET)

build:
	mkdir -p build bin

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(CSRC)/build/libcsrc.a:
	$(MAKE) -C $(CSRC)

$(TARGET): $(OBJS) $(CSRC)/build/libcsrc.a | bin
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(CSRC)/build/libcsrc.a $(LDFLAGS) -lpthread
	@echo "  ✓ aka-capp-dev built"

clean:
	rm -rf build bin
