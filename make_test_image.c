/*
 * make_test_image.c —— 生成一张 24 位 BMP 测试载体（无需任何外部图片，避免版权/依赖问题）
 *
 * 测试图刻意包含三类区域，方便答辩演示：
 *   ① 平滑渐变 —— 若 LSB 可被肉眼看出，这里最明显（预期看不出）
 *   ② 纯色块   —— 检查通道值与越界
 *   ③ 高频棋盘 —— 模拟细节丰富区域
 *
 * 编译: gcc -std=c99 -O2 -o make_test_image.exe make_test_image.c bmp.c
 * 用法: make_test_image.exe [输出.bmp] [宽] [高]   默认 test_image.bmp 512 512
 */
#include <stdio.h>
#include <stdlib.h>

#include "bmp.h"

static void fill_rect(BmpImage *img, int x0, int y0, int w, int h,
                      unsigned char b, unsigned char g, unsigned char r)
{
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            uint8_t *p = bmp_pixel(img, x, y);
            if (!p) continue;
            p[0] = b;
            p[1] = g;
            p[2] = r;
        }
    }
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "test_image.bmp";
    int w = (argc > 2) ? atoi(argv[2]) : 512;
    int h = (argc > 3) ? atoi(argv[3]) : 512;

    BmpImage *img = bmp_create(w, h);
    if (!img) return 1;

    /* ① 平滑双色渐变背景 */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = bmp_pixel(img, x, y);
            p[2] = (uint8_t)(40 + 150 * y / (h > 1 ? h - 1 : 1)); /* R: 上暗下亮 */
            p[1] = (uint8_t)(60 + 120 * x / (w > 1 ? w - 1 : 1)); /* G: 左暗右亮 */
            p[0] = (uint8_t)(90 + 100 * (x + y) / (w + h > 2 ? w + h - 2 : 1)); /* B */
        }
    }

    /* ② 四个角放纯色块 */
    int bw = w / 8 > 8 ? w / 8 : 8;
    int bh = h / 8 > 8 ? h / 8 : 8;
    fill_rect(img, 0,       0,       bw, bh, 0,   0,   255); /* 蓝块(内存顺序 B,G,R) */
    fill_rect(img, w - bw,  0,       bw, bh, 0,   255, 0);   /* 绿块 */
    fill_rect(img, 0,       h - bh,  bw, bh, 255, 0,   0);   /* 红块 */
    fill_rect(img, w - bw,  h - bh,  bw, bh, 255, 255, 255); /* 白块 */

    /* ③ 中央高频棋盘（8 像素一格） */
    int cs = w / 8 > 0 ? w / 8 : 8;
    int cx = (w - cs) / 2, cy = (h - cs) / 2;
    for (int y = 0; y < cs; y++) {
        for (int x = 0; x < cs; x++) {
            int on  = ((x / 8) + (y / 8)) & 1;
            uint8_t v = on ? 240 : 16;
            uint8_t *p = bmp_pixel(img, cx + x, cy + y);
            if (!p) continue;
            p[0] = v;
            p[1] = v;
            p[2] = v;
        }
    }

    if (bmp_save(img, out) != 0) {
        bmp_free(img);
        return 1;
    }

    printf("已生成测试载体: %s  %dx%d  容量(LSB) %zu 字节\n",
           out, w, h, (size_t)w * (size_t)h * 3u / 8u);

    bmp_free(img);
    return 0;
}
