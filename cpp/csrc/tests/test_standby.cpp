// csrc/tests/test_standby.cpp — 熄屏待机图转换单测（开发机上直接跑，不需要板子/屏）
//
// 待机图最容易错的地方是**几何**：旋转方向、翻转位、cover 裁切、缩放取样。
// 板上不好调（改一次得烧一次），所以这里把纯计算部分压死在单测里：
//   convert_box() 不碰 framebuffer，开发机（非 Linux 分支）也编得进来。
//
// 编译运行：make -C cpp/csrc test-standby

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "csrc/screen_display.hpp"

using csrc::ScreenDisplay;

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond)                                                   \
    do {                                                              \
        g_total++;                                                    \
        if (!(cond)) {                                                \
            g_fail++;                                                 \
            printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
        }                                                             \
    } while (0)

#define TEST(name) printf("· %s\n", name)

namespace {

/// 造一张 RGB8 测试图：整幅灰底 + 指定位置贴一个纯色方块
std::vector<uint8_t> make_image(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> img((size_t)w * h * 3);
    for (size_t i = 0; i < img.size(); i += 3) {
        img[i] = r;
        img[i + 1] = g;
        img[i + 2] = b;
    }
    return img;
}

void fill_block(std::vector<uint8_t>& img, int w, int x0, int y0, int bw, int bh,
                uint8_t r, uint8_t g, uint8_t b) {
    for (int y = y0; y < y0 + bh; y++) {
        for (int x = x0; x < x0 + bw; x++) {
            const size_t o = ((size_t)y * w + x) * 3;
            img[o] = r;
            img[o + 1] = g;
            img[o + 2] = b;
        }
    }
}

/// RGB565 解码回 8bit 近似值，便于比较
void unpack(uint16_t p, int& r, int& g, int& b) {
    r = (p >> 11) & 0x1F;
    g = (p >> 5) & 0x3F;
    b = p & 0x1F;
}

/// 找出与给定颜色最接近的目标像素（返回数量：用于确认"标记块被搬到了哪里"）
int count_near(const std::vector<uint16_t>& dst, int r, int g, int b, int tol = 2) {
    int n = 0;
    for (uint16_t p : dst) {
        int dr, dg, db;
        unpack(p, dr, dg, db);
        if (std::abs(dr - (r >> 3)) <= tol && std::abs(dg - (g >> 2)) <= tol &&
            std::abs(db - (b >> 3)) <= tol)
            n++;
    }
    return n;
}

void find_first_near(const std::vector<uint16_t>& dst, int w, int r, int g, int b, int& ox, int& oy) {
    ox = oy = -1;
    for (size_t i = 0; i < dst.size(); i++) {
        int dr, dg, db;
        unpack(dst[i], dr, dg, db);
        if (std::abs(dr - (r >> 3)) <= 2 && std::abs(dg - (g >> 2)) <= 2 &&
            std::abs(db - (b >> 3)) <= 2) {
            ox = (int)(i % (size_t)w);
            oy = (int)(i / (size_t)w);
            return;
        }
    }
}

}  // namespace

// ── 1. 尺寸与纯色 ────────────────────────────────────────────────────────────
static void test_uniform() {
    TEST("纯色图 → 目标整幅都是该色（无黑边/无越界）");
    const int W = 480, H = 320;         // 与面板 320x480 完全同比例
    auto img = make_image(W, H, 200, 100, 50);
    std::vector<uint16_t> dst((size_t)320 * 480, 0xFFFF);
    ScreenDisplay::convert_box(img.data(), W, H, 320, 480, 0, dst.data());
    CHECK(count_near(dst, 200, 100, 50) == 320 * 480);
}

// ── 2. 旋转方向：源左上角应落到输出右上角（90° 顺时针）──────────────────────
static void test_rotation_direction() {
    TEST("旋转方向：源(0,0) 的红块 → 输出右上角（90° 顺时针）");
    const int W = 480, H = 320;
    auto img = make_image(W, H, 0, 0, 0);
    fill_block(img, W, 0, 0, 16, 16, 255, 0, 0);            // 源左上角
    std::vector<uint16_t> dst((size_t)320 * 480, 0);
    ScreenDisplay::convert_box(img.data(), W, H, 320, 480, 0, dst.data());
    int ox = -1, oy = -1;
    find_first_near(dst, 320, 255, 0, 0, ox, oy);
    CHECK(ox >= 0);
    // 90° 顺时针：源左上 → 输出右上（ox 靠近 319，oy 靠近 0）
    CHECK(ox > 320 - 20);
    CHECK(oy < 20);

    TEST("旋转方向：源左下角 → 输出左上角");
    auto img2 = make_image(W, H, 0, 0, 0);
    fill_block(img2, W, 0, H - 16, 16, 16, 0, 255, 0);      // 源左下角
    std::vector<uint16_t> d2((size_t)320 * 480, 0);
    ScreenDisplay::convert_box(img2.data(), W, H, 320, 480, 0, d2.data());
    int x2 = -1, y2 = -1;
    find_first_near(d2, 320, 0, 255, 0, x2, y2);
    CHECK(x2 >= 0);
    CHECK(x2 < 20);
    CHECK(y2 < 20);
}

// ── 3. orient 翻转位 ─────────────────────────────────────────────────────────
static void test_orient_flip() {
    TEST("orient=1 水平翻 / orient=2 垂直翻 / orient=3 双向翻");
    const int W = 480, H = 320;
    auto img = make_image(W, H, 0, 0, 0);
    fill_block(img, W, 0, 0, 16, 16, 255, 255, 255);        // 源左上角白块

    auto run = [&](int orient, int& ox, int& oy) {
        std::vector<uint16_t> dst((size_t)320 * 480, 0);
        ScreenDisplay::convert_box(img.data(), W, H, 320, 480, orient, dst.data());
        find_first_near(dst, 320, 255, 255, 255, ox, oy);
    };
    int ox = 0, oy = 0;
    run(0, ox, oy);
    CHECK(ox > 300 && oy < 20);            // 右上
    run(1, ox, oy);
    CHECK(ox < 20 && oy < 20);             // 水平翻 → 左上
    run(2, ox, oy);
    CHECK(ox > 300 && oy > 460);           // 垂直翻 → 右下
    run(3, ox, oy);
    CHECK(ox < 20 && oy > 460);            // 双向翻 → 左下
}

// ── 4. cover 裁切：同比例图不留边；超宽图裁上下 ──────────────────────────────
static void test_cover() {
    TEST("cover：1500x1000（2:3）→ 320x480 恰好铺满，不裁不补");
    const int W = 1500, H = 1000;
    auto img = make_image(W, H, 40, 80, 120);
    // 四角各贴一个不同颜色的块，确认四角都还在（= 没有裁切）
    fill_block(img, W, 0, 0, 30, 30, 255, 0, 0);
    fill_block(img, W, W - 30, 0, 30, 30, 0, 255, 0);
    fill_block(img, W, 0, H - 30, 30, 30, 0, 0, 255);
    fill_block(img, W, W - 30, H - 30, 30, 30, 255, 255, 0);
    std::vector<uint16_t> dst((size_t)320 * 480, 0);
    ScreenDisplay::convert_box(img.data(), W, H, 320, 480, 0, dst.data());
    CHECK(count_near(dst, 255, 0, 0) > 0);       // 源左上
    CHECK(count_near(dst, 0, 255, 0) > 0);       // 源右上
    CHECK(count_near(dst, 0, 0, 255) > 0);       // 源左下
    CHECK(count_near(dst, 255, 255, 0) > 0);     // 源右下

    TEST("cover：超宽图（1000x200）→ 左右被裁掉一部分（上下铺满）");
    const int W2 = 1000, H2 = 200;
    auto wide = make_image(W2, H2, 0, 0, 0);
    fill_block(wide, W2, W2 / 2 - 10, 0, 20, H2, 255, 255, 255);   // 中间白条（竖）
    std::vector<uint16_t> d2((size_t)320 * 480, 0);
    ScreenDisplay::convert_box(wide.data(), W2, H2, 320, 480, 0, d2.data());
    // 旋转后 sw=200、sh=1000；s = max(320/200, 480/1000) = 1.6 → 旋转后的宽度被裁到 320/1.6=200 不够，
    // 实际是高度方向 1000*1.6=1600 > 480 → 源列被裁（图中部白条仍在，且铺满整列方向）
    CHECK(count_near(d2, 255, 255, 255) > 0);
    CHECK(count_near(d2, 255, 255, 255) < 320 * 480);   // 不是整幅都白
}

// ── 5. 盒式平均：缩小黑白棋盘 → 灰，而不是非黑即白 ───────────────────────────
static void test_box_average() {
    TEST("盒式平均：1px 黑白棋盘缩到 1/4 → 接近中灰（最近邻会是非黑即白）");
    const int W = 1280, H = 960;
    std::vector<uint8_t> img((size_t)W * H * 3);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t v = ((x + y) & 1) ? 255 : 0;
            const size_t o = ((size_t)y * W + x) * 3;
            img[o] = img[o + 1] = img[o + 2] = v;
        }
    }
    std::vector<uint16_t> dst((size_t)320 * 480, 0);
    ScreenDisplay::convert_box(img.data(), W, H, 320, 480, 0, dst.data());

    // 统计：应该几乎全是"中间灰"（既不是全黑也不是全白）
    int mid = 0, extreme = 0;
    for (uint16_t p : dst) {
        int r, g, b;
        unpack(p, r, g, b);
        const int v = (r << 3) | (r >> 2);   // 回到 8bit 近似
        if (v > 90 && v < 165) mid++;
        if (v < 40 || v > 215) extreme++;
    }
    printf("    中灰 %d / 极端 %d（共 %d）\n", mid, extreme, 320 * 480);
    CHECK(mid > 320 * 480 * 8 / 10);
    CHECK(extreme < 320 * 480 / 20);
}

// ── 6. 越界安全：目标缓冲前后各留金丝雀，转换不得越界写 ──────────────────────
static void test_bounds() {
    TEST("目标缓冲越界写检查（前后金丝雀不变）");
    const int W = 300, H = 500;
    auto img = make_image(W, H, 10, 20, 30);
    const size_t n = (size_t)320 * 480;
    std::vector<uint16_t> buf(n + 64, 0xDEAD);
    ScreenDisplay::convert_box(img.data(), W, H, 320, 480, 3, buf.data() + 32);
    bool canary_ok = true;
    for (int i = 0; i < 32; i++) {
        if (buf[i] != 0xDEAD || buf[32 + n + i] != 0xDEAD) canary_ok = false;
    }
    CHECK(canary_ok);
}

// ── 7. 空/异常输入不崩 ───────────────────────────────────────────────────────
static void test_invalid_input() {
    TEST("空指针 / 0 尺寸：直接返回不崩");
    std::vector<uint16_t> dst(16, 0);
    ScreenDisplay::convert_box(nullptr, 480, 320, 4, 4, 0, dst.data());
    ScreenDisplay::convert_box(dst.data() ? nullptr : nullptr, 0, 0, 4, 4, 0, dst.data());
    CHECK(true);   // 不崩就算过
}

int main() {
    printf("== 熄屏待机图转换单测 ==\n");
    test_uniform();
    test_rotation_direction();
    test_orient_flip();
    test_cover();
    test_box_average();
    test_bounds();
    test_invalid_input();
    printf("== %d 项断言, %d 失败 ==\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
