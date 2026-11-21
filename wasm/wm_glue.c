/*
 * wm_glue.c —— 把整库（LSB + DCT + Tile + HMAC）包成 WebAssembly 接口
 *
 * 与 M1 版的区别：不再只导出 LSB，而是直接转发到 wm.h 的公开 API，
 * 所以网页上能选方法、能填口令 —— 浏览器里跑的就是和 CLI 一模一样的引擎。
 *
 * 输入布局（都在 g_in 里，靠长度参数切分）：
 *   g_in = [BMP][消息][口令]
 *
 * 导出：
 *   wm_in_ptr() / wm_out_ptr() / wm_in_cap()
 *   wm_capacity_any(bmp_len, method)                  -> 容量字节数
 *   wm_embed_any(bmp_len, msg_len, method, key_len)   -> 输出字节数 / 负错误码
 *   wm_extract_any(bmp_len, key_len)                  -> 消息字节数 / 负错误码
 *   wm_last_flags()  bit0=带密钥 bit1=密钥通过 bit2=CRC通过
 *   wm_last_method() 0=LSB 1=DCT 2=Tile
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmp.h"
#include "lsb.h"
#include "dctwm.h"
#include "tilewm.h"
#include "wm.h"

/* wasm 里没有控制台：把日志换成空函数，避免把整套 printf 拖进来（体积 1MB -> 7KB） */
int wm_noop_printf(const char *fmt, ...) { (void)fmt; return 0; }
int wm_noop_fprintf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }

#define IN_CAP  (16u * 1024u * 1024u)

static uint8_t g_in[IN_CAP];
static uint8_t g_out[IN_CAP];

static int g_flags  = 0;
static int g_method = -1;
static int g_msglen = 0;

__attribute__((export_name("wm_in_ptr")))  uint8_t *wm_in_ptr(void)  { return g_in; }
__attribute__((export_name("wm_out_ptr"))) uint8_t *wm_out_ptr(void) { return g_out; }
__attribute__((export_name("wm_in_cap")))  int      wm_in_cap(void)  { return (int)IN_CAP; }

__attribute__((export_name("wm_last_flags")))  int wm_last_flags(void)  { return g_flags; }
__attribute__((export_name("wm_last_method"))) int wm_last_method(void) { return g_method; }
__attribute__((export_name("wm_last_msglen"))) int wm_last_msglen(void) { return g_msglen; }

/* 容量查询：先让调用方把 BMP 写进 g_in */
__attribute__((export_name("wm_capacity_any")))
int wm_capacity_any(int bmp_len, int method)
{
    if (bmp_len <= 0 || bmp_len > (int)IN_CAP) return -100;
    BmpImage *img = bmp_load_mem(g_in, (size_t)bmp_len);
    if (!img) return -101;

    int cap = 0;
    if (method == 0)      cap = (int)lsb_capacity_bytes(img);
    else if (method == 1) cap = (int)wm2_capacity_bytes(img);
    else                  cap = (int)wm3_max_payload();

    bmp_free(img);
    return cap;
}

/* 嵌入：g_in = [BMP][消息][口令] */
__attribute__((export_name("wm_embed_any")))
int wm_embed_any(int bmp_len, int msg_len, int method, int key_len)
{
    if (bmp_len <= 0 || msg_len <= 0 || key_len < 0) return WM_ERR_ARG;
    if ((long)bmp_len + msg_len + key_len > (long)IN_CAP) return -100;

    const unsigned char *msg = g_in + bmp_len;
    const char          *key = (key_len > 0) ? (const char *)(g_in + bmp_len + msg_len) : NULL;

    WmEmbedOptions opt;
    opt.method  = (method == 0) ? WM_METHOD_LSB : (method == 1) ? WM_METHOD_DCT : WM_METHOD_TILE;
    opt.key     = key;
    opt.key_len = (size_t)key_len;

    unsigned char *out = NULL;
    size_t         olen = 0;
    WmStatus st = wm_embed_bmp(g_in, (size_t)bmp_len, msg, (size_t)msg_len, &opt, &out, &olen);
    if (st != WM_OK) return (int)st;

    if (olen > (size_t)IN_CAP) { wm_free(out); return -100; }
    memcpy(g_out, out, olen);
    wm_free(out);
    return (int)olen;
}

/* 提取/验证：g_in = [BMP][口令] */
__attribute__((export_name("wm_extract_any")))
int wm_extract_any(int bmp_len, int key_len)
{
    if (bmp_len <= 0 || key_len < 0) return WM_ERR_ARG;
    if ((long)bmp_len + key_len > (long)IN_CAP) return -100;

    const char *key = (key_len > 0) ? (const char *)(g_in + bmp_len) : NULL;

    char         *msg = NULL;
    size_t        mlen = 0;
    WmVerifyInfo  info;
    memset(&info, 0, sizeof(info));

    WmStatus st = wm_extract_bmp(g_in, (size_t)bmp_len, key, (size_t)key_len, &msg, &mlen, &info);

    g_flags  = (info.keyed ? 1 : 0) | (info.authenticated ? 2 : 0) | (info.crc_ok ? 4 : 0);
    g_method = (int)info.method;
    g_msglen = (int)mlen;

    if (msg && mlen > 0 && mlen < (size_t)IN_CAP && (st == WM_OK || st == WM_ERR_TAMPERED)) {
        memcpy(g_out, msg, mlen);
        g_out[mlen] = 0;
    }
    wm_free(msg);

    if (st != WM_OK) return (int)st;
    return (int)mlen;
}
