# pinmux 独立工具 —— 交叉编译（工具链路径与 cpp/scripts/build.sh 保持一致）
CC      := riscv64-unknown-linux-musl-gcc
TOOLCHAIN_DIR ?= /opt/gcc/riscv64-linux-musl-x86_64/
ifneq ($(TOOLCHAIN_DIR),)
CC := $(TOOLCHAIN_DIR)bin/$(CC)
endif

CFLAGS  := -O2 -Wall -Wextra
TARGET  := pinmux_tool

all: $(TARGET)

$(TARGET): pinmux_tool.c
	$(CC) $(CFLAGS) -static -o $@ $<

# 本机编译测试（不需要 Starry 内核，验证编译/参数解析路径）
host-test: pinmux_tool.c
	cc $(CFLAGS) -o pinmux_tool_host $<
	./pinmux_tool_host || true
	./pinmux_tool_host ttyS9 || true

clean:
	rm -f $(TARGET) pinmux_tool_host

.PHONY: all host-test clean
