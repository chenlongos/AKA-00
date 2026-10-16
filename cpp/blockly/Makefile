QUICKJS := ../third_party/quickjs
CXX ?= g++
CC ?= gcc
AR ?= ar
OBJDIR := build
TARGET := bin/aka-blockly-runner-dev
LDFLAGS := -lm -ldl -pthread
ifeq ($(CROSS),1)
PREFIX := $(if $(strip $(AKA_TOOLCHAIN_DIR)),$(patsubst %/,%,$(AKA_TOOLCHAIN_DIR))/bin/,)riscv64-unknown-linux-musl-
CXX := $(PREFIX)g++
CC := $(PREFIX)gcc
AR := $(PREFIX)ar
OBJDIR := build-cross
TARGET := bin/aka-blockly-runner
LDFLAGS += -static
ARCH := -march=rv64gc -mabi=lp64d
endif
FLAGS := -Os -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION=\"2026-06-04\" -I$(QUICKJS) $(ARCH)
QJS_NAMES := quickjs dtoa libregexp libunicode cutils
QJS_OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(QJS_NAMES)))
.DEFAULT_GOAL := all
.PHONY: all clean
all: $(TARGET)
# All source targets are produced by one extraction, including on a clean tree.
$(addprefix $(QUICKJS)/,$(addsuffix .c,$(QJS_NAMES))) $(QUICKJS)/quickjs.h &: ../scripts/build-quickjs.sh
	sh ../scripts/build-quickjs.sh
$(OBJDIR)/%.o: $(QUICKJS)/%.c $(QUICKJS)/quickjs.c
	@mkdir -p $(OBJDIR)
	$(CC) $(FLAGS) -c $< -o $@
$(TARGET): runner.cpp ../csrc/include/csrc/json.hpp $(QUICKJS)/quickjs.h $(QJS_OBJS)
	@mkdir -p bin
	$(CXX) $(FLAGS) -std=c++17 -Wall -Wextra -I../csrc/include -s -o $@ $< $(QJS_OBJS) $(LDFLAGS)
clean:
	rm -rf build build-cross bin
