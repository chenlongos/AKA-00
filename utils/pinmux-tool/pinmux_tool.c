/*
 * pinmux_tool.c — SG2002 UART 引脚复用独立配置工具
 *
 * Starry 内核上 UART1/UART2 的 TX/RX 引脚默认复用为 JTAG/IIC，
 * 需要写 /dev/pinmux（FMUX 寄存器，基址 0x0300_1000）路由后才能收发。
 *
 *   UART1(ttyS1): JTAG_CPU_TMS(0x64)/JTAG_CPU_TCK(0x68) -> FSEL=6
 *   UART2(ttyS2): IIC0_SCL(0x70)/IIC0_SDA(0x74)        -> FSEL=2
 *
 * 用法:
 *   pinmux_tool            # 配置 UART1+UART2
 *   pinmux_tool ttyS1      # 只配置 UART1（也接受 uart1 / 1）
 *   pinmux_tool ttyS2      # 只配置 UART2（也接受 uart2 / 2）
 *
 * 退出码: 0 成功；1 部分/全部写入失败；2 /dev/pinmux 不存在（非 Starry 内核）
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PINMUX_DEVICE "/dev/pinmux"

/* FMUX 寄存器偏移（相对基址 0x0300_1000）与功能选择值 */
#define FMUX_JTAG_CPU_TMS 0x64
#define FMUX_JTAG_CPU_TCK 0x68
#define FSEL_UART1 6

#define FMUX_IIC0_SCL 0x70
#define FMUX_IIC0_SDA 0x74
#define FSEL_UART2 2

/* 向 /dev/pinmux 写入一行 "0xOFFSET VALUE"。
 * 整行必须在一次 write 中发出，否则内核按片段解析会返回 EINVAL。 */
static int write_fmux(int fd, unsigned int offset, unsigned int value) {
    char line[32];
    int n = snprintf(line, sizeof line, "0x%X %u", offset, value);
    ssize_t w = write(fd, line, (size_t)n);
    if (w < 0) {
        perror("[pinmux] write failed");
        fprintf(stderr, "  (line was: \"%s\")\n", line);
        return -1;
    }
    return 0;
}

static int configure(int fd, unsigned int off_a, unsigned int off_b,
                     unsigned int fsel, const char *name) {
    int ok = (write_fmux(fd, off_a, fsel) == 0);
    ok = (write_fmux(fd, off_b, fsel) == 0) && ok;
    if (ok)
        printf("[pinmux] %s pads routed (offsets 0x%X/0x%X -> FSEL=%u)\n",
               name, off_a, off_b, fsel);
    else
        fprintf(stderr, "[pinmux] failed to route %s\n", name);
    return ok ? 0 : 1;
}

/* 识别 "ttyS1" / "uart1" / "1" 等写法，大小写不敏感 */
static int matches(const char *arg, const char *suffix) {
    if (strcasecmp(arg, suffix) == 0) return 1;
    char buf[16];
    snprintf(buf, sizeof buf, "ttyS%s", suffix + 4); /* "uart1" -> "ttyS1" */
    if (strcasecmp(arg, buf) == 0) return 1;
    return strcmp(arg, suffix + 4) == 0; /* "1" */
}

int main(int argc, char **argv) {
    int fd = open(PINMUX_DEVICE, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[pinmux] open %s failed: %s\n", PINMUX_DEVICE,
                strerror(errno));
        fprintf(stderr, "[pinmux] 非 Starry 内核或无权限，跳过引脚配置\n");
        return 2;
    }

    int do_uart1 = 0, do_uart2 = 0;
    if (argc <= 1) {
        do_uart1 = do_uart2 = 1; /* 默认两个都配 */
    } else {
        for (int i = 1; i < argc; i++) {
            if (matches(argv[i], "uart1"))
                do_uart1 = 1;
            else if (matches(argv[i], "uart2"))
                do_uart2 = 1;
            else {
                fprintf(stderr, "usage: %s [ttyS1|ttyS2 ...]\n", argv[0]);
                close(fd);
                return 1;
            }
        }
    }

    int ret = 0;
    if (do_uart1)
        ret |= configure(fd, FMUX_JTAG_CPU_TMS, FMUX_JTAG_CPU_TCK, FSEL_UART1, "UART1");
    if (do_uart2)
        ret |= configure(fd, FMUX_IIC0_SCL, FMUX_IIC0_SDA, FSEL_UART2, "UART2");

    close(fd);
    return ret;
}
