# =============================================================================
# csrc — C++ 硬件控制库（src/ 的 C++ 移植）
#
# 本机 (macOS/Linux) 开发构建；板上交叉编译用 Makefile.cross
#
# 用法:
#   make                    # 带屏版本 → build/libcsrc.a
#   make WITH_SCREEN=0      # 不带屏版本 → build-noscreen/libcsrc.a（裁掉显示栈）
#   make clean
# =============================================================================

CXX      ?= g++
AR       ?= ar
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -MMD -MP
ARFLAGS   = rcs

# 带屏 / 不带屏两个变体（与 Makefile.cross、顶层 Makefile 的 screen/noscreen 对齐）
WITH_SCREEN ?= 1
ifeq ($(WITH_SCREEN),1)
SCREEN_SUFFIX :=
SCREEN_DEF    := -DAKA_WITH_SCREEN=1
else
SCREEN_SUFFIX := -noscreen
SCREEN_DEF    := -DAKA_WITH_SCREEN=0
endif
CXXFLAGS += $(SCREEN_DEF)

# 摄像头 JPEG（Linux 主机需要 jpeglib.h；macOS 走 stub，无需）
JPEG_DIR ?= ../third_party/jpeg
JPEG_INC := $(if $(wildcard $(JPEG_DIR)/include/jpeglib.h),-I$(JPEG_DIR)/include,)
CXXFLAGS += $(JPEG_INC)

OBJDIR := build$(SCREEN_SUFFIX)
SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:src/%.cpp=$(OBJDIR)/%.o)
LIB  := $(OBJDIR)/libcsrc.a
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

.DEFAULT_GOAL := all
.PHONY: all clean test-standby

# 开发机单测：熄屏待机图转换（纯计算，不需要板子/屏；非 Linux 分支也编得进来）
TESTDIR := build-tests
test-standby: $(TESTDIR)/test_standby
	@$(TESTDIR)/test_standby

$(TESTDIR)/test_standby: tests/test_standby.cpp src/screen_display.cpp | $(TESTDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(TESTDIR):
	mkdir -p $(TESTDIR)

all: $(LIB)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: src/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $(OBJS)
	@echo "  ✓ libcsrc.a$(SCREEN_SUFFIX) built"

clean:
	rm -rf build build-noscreen
