/*
 * test_demo2.c - ST7796S Framebuffer 显示测试程序
 * 功能：在 /dev/fb0 上依次显示纯红/绿/蓝、彩色竖条、白底红边框
 * 编译：riscv64-unknown-linux-gnu-gcc -O2 -static -o test_demo2 test_demo2.c
 * 注意：运行前确保 /sys/class/graphics/fb0/state 为 1（显示引擎开启）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <errno.h>

/* 屏幕尺寸（可从 fb_var_screeninfo 动态获取，这里使用固定值用于演示） */
#define DEFAULT_WIDTH  320
#define DEFAULT_HEIGHT 480

/* RGB565 颜色宏 */
#define COLOR_RED   0xF800
#define COLOR_GREEN 0x07E0
#define COLOR_BLUE  0x001F
#define COLOR_WHITE 0xFFFF

/* 填充整个屏幕为指定颜色（RGB565） */
static void fill_screen(unsigned short *fb, int w, int h, unsigned short color)
{
    int total = w * h;
    for (int i = 0; i < total; i++)
        fb[i] = color;
}

/* 绘制垂直彩色条纹（红、绿、蓝、黄、紫、青、白） */
static void draw_color_bars(unsigned short *fb, int w, int h)
{
    int bar_width = w / 7;
    unsigned short colors[] = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF, 0xFFFF};
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = x / bar_width;
            if (idx > 6) idx = 6;
            fb[y * w + x] = colors[idx];
        }
    }
}

/* 绘制边框：背景色 bg，边框颜色 border_color，边框宽度 4 像素 */
static void draw_border(unsigned short *fb, int w, int h, unsigned short bg, unsigned short border_color)
{
    // 先填充背景色
    fill_screen(fb, w, h, bg);

    int border = 4;
    // 上边和底边
    for (int x = 0; x < w; x++) {
        for (int b = 0; b < border; b++) {
            fb[b * w + x] = border_color;
            fb[(h - 1 - b) * w + x] = border_color;
        }
    }
    // 左边和右边
    for (int y = 0; y < h; y++) {
        for (int b = 0; b < border; b++) {
            fb[y * w + b] = border_color;
            fb[y * w + (w - 1 - b)] = border_color;
        }
    }
}

int main(void)
{
    int fd;
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    unsigned short *fb_ptr = NULL;
    size_t screensize = 0;

    printf("[ST7796S Demo] Opening Framebuffer Device: /dev/fb0\n");

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("Error: Cannot open framebuffer device");
        return 1;
    }

    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1) {
        perror("Error reading fixed information");
        close(fd);
        return 1;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("Error reading variable information");
        close(fd);
        return 1;
    }

    printf("\n=== ST7796S Screen Information ===\n");
    printf("Resolution : %dx%d\n", vinfo.xres, vinfo.yres);
    printf("Bits per pixel: %d bpp\n", vinfo.bits_per_pixel);
    printf("Line Length  : %d bytes\n", finfo.line_length);
    printf("===================================\n\n");

    if (vinfo.bits_per_pixel != 16) {
        printf("Warning: Screen format is not 16bpp (RGB565). Current bpp: %d\n", vinfo.bits_per_pixel);
        printf("This demo is designed for RGB565 only. Colors may appear incorrect.\n");
        // 继续执行，但可能颜色不对
    }

    screensize = vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8);
    fb_ptr = (unsigned short *)mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb_ptr == MAP_FAILED) {
        perror("Error: Failed to map framebuffer memory to user space");
        close(fd);
        return 1;
    }

    int w = vinfo.xres;
    int h = vinfo.yres;

    printf("Starting ST7796S Display Tests...\n");

    printf("1. Testing Pure RED...\n");
    fill_screen(fb_ptr, w, h, COLOR_RED);
    sleep(1);

    printf("2. Testing Pure GREEN...\n");
    fill_screen(fb_ptr, w, h, COLOR_GREEN);
    sleep(1);

    printf("3. Testing Pure BLUE...\n");
    fill_screen(fb_ptr, w, h, COLOR_BLUE);
    sleep(1);

    printf("4. Displaying Vertical Color Bars...\n");
    draw_color_bars(fb_ptr, w, h);
    sleep(1);

    printf("5. Drawing 4-Pixel Border Frame (White background, Red Border)...\n");
    draw_border(fb_ptr, w, h, COLOR_WHITE, COLOR_RED);
    sleep(1);

    printf("Cleaning screen and exiting demo.\n");
    fill_screen(fb_ptr, w, h, 0x0000);  // 清屏为黑色

    munmap(fb_ptr, screensize);
    close(fd);
    return 0;
}