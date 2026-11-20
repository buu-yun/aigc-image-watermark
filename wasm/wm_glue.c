/*
 * wm_glue.c —— 把 C 引擎包成浏览器可调用的 WebAssembly 接口
 *
 * 设计要点：
 *   1) 不传指针、不用回调：JS 先通过 wm_in_ptr() 拿到一块固定内存，
 *      把「BMP 字节 + 文本字节」写进去，再调用函数 —— 这是 wasm 最省事的传参方式。
 *   2) 输入区 g_in 的布局：[0, bmp_len) 是 BMP，[bmp_len, bmp_len+text_len) 是 UTF-8 文本。
 *   3) 返回值：>=0 表示输出字节数；<0 是错误码（LSB 的错误码原样透传）。
 *
 * 编译：
 *   zig cc -target wasm32-wasi -mexec-model=reactor -O2 \
 *          -Wl,--export=wm_in_ptr -Wl,--export=wm_out_ptr -Wl,--export=wm_in_cap \
 *          -Wl,--export=wm_embed -Wl,--export=wm_extract -Wl,--export=wm_capacity \
 *          -o wm.wasm bmp.c lsb.c wm_glue.c
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmp.h"
#include "lsb.h"

/*
 * wasm 里没有控制台，日志全部丢弃。
 * 额外好处：编译时用 -Dprintf=wm_noop_printf -Dfprintf=wm_noop_fprintf 替换掉，
 * 就不会把 wasi-libc 整套格式化输出拖进去，wasm 体积能小一个数量级。
 */
int wm_noop_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

int wm_noop_fprintf(FILE *f, const char *fmt, ...)
{
    (void)f;
    (void)fmt;
    return 0;
}

/* 输入/输出各 16 MB：够放 2300x2300 的 24 位 BMP，浏览器里也吃得消 */
#define IN_CAP  (16u * 1024u * 1024u)
#define OUT_CAP (16u * 1024u * 1024u)

/* 胶水层自己的错误码，避开 LSB 的 -1..-4 */
#define WM_E_ARGS   (-100)
#define WM_E_BMP    (-101)
#define WM_E_SAVE   (-102)
#define WM_E_TOOBIG (-103)

static uint8_t g_in[IN_CAP];
static uint8_t g_out[OUT_CAP];

/* ---- 导出给 JS 的入口 ---- */

__attribute__((export_name("wm_in_ptr")))  uint8_t *wm_in_ptr(void)  { return g_in; }
__attribute__((export_name("wm_out_ptr"))) uint8_t *wm_out_ptr(void) { return g_out; }
__attribute__((export_name("wm_in_cap")))  int      wm_in_cap(void)  { return (int)IN_CAP; }

/* 嵌入：输入 g_in = [BMP][文本]，输出 g_out = 带水印的新 BMP；返回输出字节数 */
__attribute__((export_name("wm_embed")))
int wm_embed(int bmp_len, int text_len)
{
    if (bmp_len <= 0 || text_len <= 0) return WM_E_ARGS;
    if ((int64_t)bmp_len + (int64_t)text_len > (int64_t)IN_CAP) return WM_E_TOOBIG;

    BmpImage *img = bmp_load_mem(g_in, (size_t)bmp_len);
    if (!img) return WM_E_BMP;

    LsbStatus st = lsb_embed(img, g_in + bmp_len, (size_t)text_len, 0);
    if (st != LSB_OK) {
        bmp_free(img);
        return (int)st;                 /* -2 容量不足 等，JS 直接翻译成中文 */
    }

    uint8_t *out  = NULL;
    size_t   olen = 0;
    int rc = bmp_save_mem(img, &out, &olen);
    bmp_free(img);
    if (rc != 0) return WM_E_SAVE;
    if (olen > (size_t)OUT_CAP) {
        free(out);
        return WM_E_TOOBIG;
    }

    memcpy(g_out, out, olen);
    free(out);
    return (int)olen;
}

/* 提取：输入 g_in = [BMP]，输出 g_out = 文本；返回文本字节数 */
__attribute__((export_name("wm_extract")))
int wm_extract(int bmp_len)
{
    if (bmp_len <= 0 || bmp_len > (int)IN_CAP) return WM_E_ARGS;

    BmpImage *img = bmp_load_mem(g_in, (size_t)bmp_len);
    if (!img) return WM_E_BMP;

    size_t cap = lsb_capacity_bytes(img);
    if (cap + 2u > (size_t)OUT_CAP) {
        bmp_free(img);
        return WM_E_TOOBIG;
    }

    size_t    len = 0;
    LsbStatus st  = lsb_extract(img, g_out, cap + 1u, &len, 0);
    bmp_free(img);

    if (st != LSB_OK) return (int)st;   /* -3 无水印 / -4 头部损坏 */
    return (int)len;
}

/* 容量查询：JS 在用户选完图后用它显示"还能藏多少字" */
__attribute__((export_name("wm_capacity")))
int wm_capacity(int bmp_len)
{
    if (bmp_len <= 0 || bmp_len > (int)IN_CAP) return WM_E_ARGS;
    BmpImage *img = bmp_load_mem(g_in, (size_t)bmp_len);
    if (!img) return WM_E_BMP;
    int c = (int)lsb_capacity_bytes(img);
    bmp_free(img);
    return c;
}
