/*
 * demo_image.c - ST7796S Framebuffer 单图显示 demo
 *
 * 解码一张 JPEG，按长宽比缩放 + 居中适配到屏幕 (320x480 RGB565)，
 * 写到 /dev/fb0 后退出。一次性使用，不循环刷新。
 *
 * 编译（riscv64 musl，与 screen-node-cpp 共用同一份 libjpeg.a）：
 *   riscv64-unknown-linux-musl-gcc -O2 -static -o demo_image demo_image.c \
 *       -I../../dora/libs/jpeg ../../dora/libs/jpeg/libjpeg.a
 *
 *   # 或者本机（host）编译验证：
 *   gcc -O2 -o demo_image demo_image.c -ljpeg
 *
 * 用法：
 *   ./demo_image /path/to/pic.jpg
 *
 * 注意：运行前确保 /sys/class/graphics/fb0/state 为 1（显示引擎开启）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>

/* framebuffer 头仅在 Linux 可用；本机（macOS）做编译验证时跳过。 */
#if defined(__linux__)
  #include <linux/fb.h>
#endif

#include <jpeglib.h>
#include <setjmp.h>

/* ── libjpeg 错误桩：避免坏 JPEG 触发默认 exit() ───────────────────────── */
struct JpegErr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};
static void jpeg_on_error(j_common_ptr cinfo)
{
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    fprintf(stderr, "[JPEG] decode error: %s\n", buf);
    longjmp(((struct JpegErr *)cinfo->err)->jump, 1);
}

/* 把整个文件读进内存 */
static int read_file_all(const char *path, unsigned char **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[demo_image] open %s failed: %s\n", path, strerror(errno));
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return 0; }
    rewind(f);
    *buf = (unsigned char *)malloc((size_t)n);
    if (!*buf) { fclose(f); return 0; }
    if (fread(*buf, 1, (size_t)n, f) != (size_t)n) {
        free(*buf); fclose(f); return 0;
    }
    *len = (size_t)n;
    fclose(f);
    return 1;
}

/* 解码 JPEG → RGB888 (w*h*3)，调用者预分配 rgb_buf。返回 0/1。*/
static int decode_jpeg_to_rgb(const unsigned char *src, size_t src_len,
                              unsigned char *rgb_buf, int *out_w, int *out_h)
{
    struct jpeg_decompress_struct cinfo;
    struct JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;

    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, src, src_len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    *out_w = (int)cinfo.output_width;
    *out_h = (int)cinfo.output_height;
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *row = rgb_buf + (size_t)cinfo.output_scanline * (*out_w) * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 1;
}

/* RGB888 (src_w x src_h) → RGB565 fb，居中适配（含按比例缩放 + 逆时针 90° 旋转）。
 * 多余区域填黑 (0x0000)。最近邻采样；列/行映射预计算避免软浮点慢。
 *
 * 逆时针 90° 旋转后图像尺寸：宽 = src_h，高 = src_w。
 *   源 (sy, sx) → 目标 (oy, ox)：
 *     oy = src_w - 1 - sx    // 原图右→上，左→下；sx=0(最左) 旋转到 oy=src_w-1(最下)
 *     ox = sy                // 原图上→左，下→右
 *
 * 缩放因子仍按 (out_w/src_h) vs (out_h/src_w) 取较小者，让旋转后的图保持
 * 长宽比落在屏幕里；屏幕 320x480 旋转后刚好命中横屏→竖屏的常见图方向。*/
static void draw_image_centered(unsigned short *fb, int fb_w, int fb_h,
                                const unsigned char *rgb, int src_w, int src_h)
{
    /* 清黑底 */
    {
        int total = fb_w * fb_h;
        for (int i = 0; i < total; i++) fb[i] = 0x0000;
    }

    if (src_w <= 0 || src_h <= 0) return;

    /* 旋转后图像：宽 = src_h, 高 = src_w */
    double s_for_w = (double)fb_w / src_h;
    double s_for_h = (double)fb_h / src_w;
    double s = s_for_w < s_for_h ? s_for_w : s_for_h;

    int rot_w = (int)(src_h * s + 0.5);   // 旋转后宽度
    int rot_h = (int)(src_w * s + 0.5);   // 旋转后高度
    if (rot_w < 1) rot_w = 1;
    if (rot_h < 1) rot_h = 1;
    if (rot_w > fb_w) rot_w = fb_w;
    if (rot_h > fb_h) rot_h = fb_h;

    int off_x = (fb_w - rot_w) / 2;
    int off_y = (fb_h - rot_h) / 2;

    int max_w = rot_w < fb_w ? rot_w : fb_w;
    int max_h = rot_h < fb_h ? rot_h : fb_h;

    /* 预计算映射：目标 (oy, ox) → 源 (sy, sx)。
     * ox → sx (ox 只依赖 sy 方向上的原图 sx)
     * oy → sy (oy 只依赖 sx 方向上的原图 sy, 注意 ox/sx 含义要反着看) */
    int sy_map[2048];   // 目标列 ox → 源行 sy = ox
    int sx_map[2048];   // 目标行 oy → 源列 sx = src_w-1-oy
    if (max_w > 2048) max_w = 2048;
    if (max_h > 2048) max_h = 2048;

    for (int ox = 0; ox < max_w; ox++) {
        int sy = (int)((double)ox / s + 0.5);
        if (sy < 0) sy = 0;
        else if (sy >= src_h) sy = src_h - 1;
        sy_map[ox] = sy;
    }
    for (int oy = 0; oy < max_h; oy++) {
        double sx_src = (double)(src_w - 1) - oy / s;
        int sx = (int)(sx_src + 0.5);
        if (sx < 0) sx = 0;
        else if (sx >= src_w) sx = src_w - 1;
        sx_map[oy] = sx;
    }

    for (int oy = 0; oy < max_h; oy++) {
        const unsigned char *src_row = rgb + (size_t)sx_map[oy] * 3;
        unsigned short *dst_row = fb + (off_y + oy) * fb_w + off_x;
        for (int ox = 0; ox < max_w; ox++) {
            const unsigned char *p = src_row + (size_t)sy_map[ox] * src_w * 3;
            dst_row[ox] = (unsigned short)(((p[0] >> 3) << 11) |
                                           ((p[1] >> 2) << 5)  |
                                           (p[2] >> 3));
        }
    }
}

int main(int argc, char **argv)
{
#if __linux__
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <jpeg_path>\n", argv[0]);
        return 1;
    }
    const char *jpeg_path = argv[1];

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("Error: Cannot open /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    {
        unsigned long request = 0x4600;  /* FBIOGET_VSCREENINFO */
        struct { unsigned xres, yres, xres_v, yres_v, xoff, yoff, bpp; } vi;
        memset(&vi, 0, sizeof vi);
        if (ioctl(fd, request, &vi) == 0 && vi.xres > 0 && vi.bpp > 0) {
            vinfo.xres = vi.xres;
            vinfo.yres = vi.yres;
            vinfo.bits_per_pixel = vi.bpp;
        } else {
            fprintf(stderr, "Warning: FBIOGET_VSCREENINFO failed, assuming 320x480@16bpp\n");
            vinfo.xres = 320;
            vinfo.yres = 480;
            vinfo.bits_per_pixel = 16;
        }
    }

    printf("[demo_image] fb0: %dx%d @%dbpp\n",
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    size_t screensize = (size_t)vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8);
    unsigned short *fb_ptr = (unsigned short *)mmap(NULL, screensize,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb_ptr == MAP_FAILED) {
        perror("Error: mmap /dev/fb0");
        close(fd);
        return 1;
    }

    /* 读 JPEG 文件 */
    unsigned char *file_buf = NULL;
    size_t file_len = 0;
    if (!read_file_all(jpeg_path, &file_buf, &file_len)) {
        munmap(fb_ptr, screensize); close(fd);
        return 1;
    }

    /* 先看 header 拿尺寸（libjpeg 要求先知道 w/h 才能分配 row buffer） */
    int src_w = 0, src_h = 0;
    {
        struct jpeg_decompress_struct cinfo;
        struct JpegErr jerr;
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpeg_on_error;
        int header_ok = 1;
        if (setjmp(jerr.jump)) {
            header_ok = 0;
        } else {
            jpeg_create_decompress(&cinfo);
            jpeg_mem_src(&cinfo, file_buf, file_len);
            if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
                header_ok = 0;
            } else {
                cinfo.out_color_space = JCS_RGB;
                src_w = (int)cinfo.image_width;
                src_h = (int)cinfo.image_height;
            }
            jpeg_destroy_decompress(&cinfo);
        }
        if (!header_ok) {
            fprintf(stderr, "JPEG header read failed: %s\n", jpeg_path);
            free(file_buf);
            munmap(fb_ptr, screensize); close(fd);
            return 1;
        }
    }

    if (src_w > 16384 || src_h > 16384) {
        fprintf(stderr, "JPEG too large (%dx%d), refusing (>16384)\n", src_w, src_h);
        free(file_buf);
        munmap(fb_ptr, screensize); close(fd);
        return 1;
    }

    unsigned char *rgb = (unsigned char *)malloc((size_t)src_w * src_h * 3);
    if (!rgb) {
        fprintf(stderr, "OOM allocating %dx%d RGB buffer\n", src_w, src_h);
        free(file_buf);
        munmap(fb_ptr, screensize); close(fd);
        return 1;
    }

    int dec_w = 0, dec_h = 0;
    if (!decode_jpeg_to_rgb(file_buf, file_len, rgb, &dec_w, &dec_h)) {
        fprintf(stderr, "JPEG decode failed: %s\n", jpeg_path);
        free(rgb); free(file_buf);
        munmap(fb_ptr, screensize); close(fd);
        return 1;
    }
    free(file_buf);

    printf("[demo_image] %s decoded: %dx%d, drawing centered to %dx%d\n",
           jpeg_path, dec_w, dec_h, vinfo.xres, vinfo.yres);
    draw_image_centered(fb_ptr, vinfo.xres, vinfo.yres, rgb, dec_w, dec_h);

    /* 一次性 demo：画完就退出，不循环刷新也不清屏（让图留在屏上）。
     * 如果想退出前清屏，把下面这一行解除注释。                         */
    /* fill_screen_black(fb_ptr, vinfo.xres, vinfo.yres); */

    free(rgb);
    munmap(fb_ptr, screensize);
    close(fd);
    return 0;
#else
    /* 非 Linux host：只做 libjpeg 链接烟雾测试 */
    (void)argc; (void)argv;
    printf("[demo_image] non-Linux host build: skipping framebuffer demo.\n");
    if (argc >= 2) {
        unsigned char *file_buf = NULL; size_t file_len = 0;
        if (!read_file_all(argv[1], &file_buf, &file_len)) return 1;
        struct jpeg_decompress_struct cinfo;
        struct JpegErr jerr;
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpeg_on_error;
        if (setjmp(jerr.jump)) { jpeg_destroy_decompress(&cinfo); free(file_buf); return 1; }
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, file_buf, file_len);
        int rc = (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK);
        jpeg_destroy_decompress(&cinfo);
        free(file_buf);
        if (!rc) { fprintf(stderr, "JPEG header read failed: %s\n", argv[1]); return 1; }
        printf("[demo_image] JPEG header OK on host build.\n");
    }
    return 0;
#endif
}