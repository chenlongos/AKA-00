# =============================================================================
# capp — C++ Web 服务（app/ 的 C++ 移植）—— 本机开发构建
#
# 注意：本机构建输出 bin/aka-capp-dev（macOS/Linux 版，仅本机调试用）。
# 板子部署用 bin/aka-capp（riscv64 静态）——由 Makefile.cross 构建，
# 两者互不覆盖。
#
# 用法:
#   make                    # 带屏版本 → bin/aka-capp-dev（链接 csrc）
#   make WITH_SCREEN=0      # 不带屏版本 → bin/aka-capp-dev-noscreen
#   make CSRC=../csrc       # 指定 csrc 源码路径（默认 ../csrc）
#   make clean
# =============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -MMD -MP -I$(CSRC)/include
LDFLAGS  ?=

CSRC := ../csrc

# 带屏 / 不带屏两个变体（与 csrc、顶层 Makefile 的 screen/noscreen 对齐）
WITH_SCREEN ?= 1
ifeq ($(WITH_SCREEN),1)
SCREEN_SUFFIX :=
SCREEN_DEF    := -DAKA_WITH_SCREEN=1
else
SCREEN_SUFFIX := -noscreen
SCREEN_DEF    := -DAKA_WITH_SCREEN=0
endif
CXXFLAGS += $(SCREEN_DEF)

TARGET  := bin/aka-capp-dev$(SCREEN_SUFFIX)
CSRC_LIB := $(CSRC)/build$(SCREEN_SUFFIX)/libcsrc.a

# 摄像头 JPEG（Linux 主机需要 jpeglib.h + -ljpeg；macOS 走 stub，无需）。
# 注意：third_party/jpeg 是 riscv64 交叉产物，本机链接不要用它——Linux 主机装
# libjpeg-dev 后加 `make LDFLAGS=-ljpeg` 即可。
JPEG_DIR ?= ../third_party/jpeg
JPEG_INC := $(if $(wildcard $(JPEG_DIR)/include/jpeglib.h),-I$(JPEG_DIR)/include,)
CXXFLAGS += $(JPEG_INC)

# mbedTLS（HTTPS 可选）：仅当 third_party/mbedtls 已交叉编译好才挂上 include/lib，
# 本机 dev 可不装（HTTP-only 也跑得起来）。真上 mbedTLS 后 brew 装系统版即可。
MBEDTLS_DIR ?= ../third_party/mbedtls
MBEDTLS_INC := $(if $(wildcard $(MBEDTLS_DIR)/include/mbedtls/ssl.h),-I$(MBEDTLS_DIR)/include,)
MBEDTLS_LIB := $(if $(wildcard $(MBEDTLS_DIR)/lib/libmbedtls.a),-L$(MBEDTLS_DIR)/lib,)
MBEDTLS_LINK := $(if $(wildcard $(MBEDTLS_DIR)/lib/libmbedtls.a),-lmbedtls -lmbedx509 -lmbedcrypto,)
CXXFLAGS += $(MBEDTLS_INC)

OBJDIR := build$(SCREEN_SUFFIX)
SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:src/%.cpp=$(OBJDIR)/%.o)
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

.DEFAULT_GOAL := all
.PHONY: all clean

all: $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR) bin

$(OBJDIR)/%.o: src/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(CSRC_LIB):
	$(MAKE) -C $(CSRC) WITH_SCREEN=$(WITH_SCREEN)

$(TARGET): $(OBJS) $(CSRC_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(CSRC_LIB) \
	    $(LDFLAGS) $(MBEDTLS_LIB) $(MBEDTLS_LINK) -lpthread
	@echo "  ✓ aka-capp-dev$(SCREEN_SUFFIX) built"

clean:
	rm -rf build build-noscreen bin
