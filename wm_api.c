/*
 * wm_api.c —— 公开 API 的实现：把 lsb / dctwm / tilewm 三个模块收进一个门面
 *
 * 密钥标签怎么放（不改任何水印格式，只在载荷里加 8 字节）：
 *     payload = "WMK1"(4) + tag(4) + 原文
 *   tag = HMAC-SHA256(口令, 原文) 的前 4 字节
 * 提取时：
 *   看到 "WMK1" + 有口令  → 重算 tag 比对，不一致就是 WM_ERR_BAD_KEY（伪造/篡改）
 *   看到 "WMK1" + 没口令  → WM_ERR_NEED_KEY（拒绝在不知道密钥的情况下下结论）
 *
 * 为什么不直接改三个模块的水印头：
 *   一是改动面小、风险低；二是"密钥"属于**格式之外的一层封装**，
 *   以后换 HMAC 为 Ed25519 签名，也只需要换这一层。
 */
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmp.h"
#include "crc16.h"
#include "crypto.h"
#include "dctwm.h"
#include "lsb.h"
#include "tilewm.h"

#define KEY_MAGIC     "WMK1"
#define KEY_TAG_LEN   4
#define KEY_OVERHEAD  (4 + KEY_TAG_LEN)   /* 8 字节 */

/* ---------------- 说明性文字 ---------------- */

void wm_free(void *p) { free(p); }

const char *wm_version(void) { return WM_VERSION_STRING; }

const char *wm_method_name(WmMethod m)
{
    switch (m) {
    case WM_METHOD_LSB:  return "LSB";
    case WM_METHOD_DCT:  return "DCT";
    case WM_METHOD_TILE: return "Tile";
    default:             return "auto";
    }
}

const char *wm_status_str(WmStatus st)
{
    switch (st) {
    case WM_OK:               return "成功";
    case WM_ERR_ARG:          return "参数非法";
    case WM_ERR_IO:           return "文件读写失败";
    case WM_ERR_FORMAT:       return "不是 24 位 BMP，或水印头部损坏";
    case WM_ERR_CAPACITY:     return "载体容量不足";
    case WM_ERR_NO_WATERMARK: return "未找到水印";
    case WM_ERR_TAMPERED:     return "校验失败：内容与嵌入时不一致（疑似被篡改）";
    case WM_ERR_NEED_KEY:     return "该水印带密钥标签，需要提供口令才能验证";
    case WM_ERR_BAD_KEY:      return "口令不正确：密钥标签验证失败（内容不可信）";
    default:                  return "未知错误";
    }
}

/* ---------------- 密钥标签 ---------------- */

static void make_tag(const char *key, size_t key_len,
                     const unsigned char *msg, size_t msg_len, uint8_t tag[4])
{
    hmac_tag4(key, key_len, msg, msg_len, tag);
}

/* 把 原文 包成 "WMK1"+tag+原文；成功返回 malloc 的缓冲 */
static unsigned char *wrap_keyed(const char *key, size_t key_len,
                                 const unsigned char *msg, size_t msg_len, size_t *out_len)
{
    unsigned char *buf = (unsigned char *)malloc(msg_len + KEY_OVERHEAD);
    if (!buf) return NULL;

    memcpy(buf, KEY_MAGIC, 4);
    make_tag(key, key_len, msg, msg_len, buf + 4);
    memcpy(buf + KEY_OVERHEAD, msg, msg_len);

    *out_len = msg_len + KEY_OVERHEAD;
    return buf;
}

/*
 * 拆包：返回 WM_OK 并把 msg 指针/长度指向载荷内部（不拷贝）。
 * 若载荷带 WMK1 但没有口令 → WM_ERR_NEED_KEY；标签不符 → WM_ERR_BAD_KEY。
 */
static WmStatus unwrap(const unsigned char *payload, size_t payload_len,
                       const char *key, size_t key_len,
                       const unsigned char **msg, size_t *msg_len,
                       int *keyed, int *authenticated)
{
    *keyed = 0;
    *authenticated = 0;

    if (payload_len >= KEY_OVERHEAD && memcmp(payload, KEY_MAGIC, 4) == 0) {
        *keyed = 1;
        if (!key || key_len == 0) return WM_ERR_NEED_KEY;

        const unsigned char *body = payload + KEY_OVERHEAD;
        size_t body_len = payload_len - KEY_OVERHEAD;

        uint8_t tag[4];
        make_tag(key, key_len, body, body_len, tag);
        if (memcmp(tag, payload + 4, 4) != 0) return WM_ERR_BAD_KEY;

        *authenticated = 1;
        *msg = body;
        *msg_len = body_len;
        return WM_OK;
    }

    *msg = payload;
    *msg_len = payload_len;
    return WM_OK;
}

/* ---------------- 提取（三种方法自动尝试） ---------------- */

typedef struct {
    unsigned char *buf;
    size_t         cap;
} ExtractBuf;

static int alloc_buf(const BmpImage *img, ExtractBuf *eb)
{
    size_t c = lsb_capacity_bytes(img);
    size_t d = wm2_capacity_bytes(img) + 32u;
    if (d > c) c = d;
    if (wm3_max_payload() + 32u > c) c = wm3_max_payload() + 32u;
    c += 32u;

    eb->buf = (unsigned char *)malloc(c);
    eb->cap = eb->buf ? c : 0;
    return eb->buf ? 0 : -1;
}

/* 返回：0=命中且校验通过，1=命中但校验失败，2=没找到；*method 输出命中方法 */
static int try_extract(const BmpImage *img, unsigned char *buf, size_t cap,
                       size_t *len, WmMethod *method)
{
    size_t tlen = 0;
    int    dx = 0, dy = 0;
    Wm3Status s3 = wm3_extract(img, buf, cap, &tlen, &dx, &dy, NULL, 0);
    if (s3 == WM3_OK || s3 == WM3_ERR_CRC) {
        *len = tlen;
        *method = WM_METHOD_TILE;
        return (s3 == WM3_OK) ? 0 : 1;
    }

    uint16_t hc = 0;
    size_t   wlen = 0;
    Wm2Status s2 = wm2_extract(img, buf, cap, &wlen, &hc, 0);
    if (s2 == WM2_OK || s2 == WM2_ERR_CRC) {
        *len = wlen;
        *method = WM_METHOD_DCT;
        return (s2 == WM2_OK) ? 0 : 1;
    }

    size_t    llen = 0;
    size_t    lcap = lsb_capacity_bytes(img);
    LsbStatus s1 = lsb_extract(img, buf, (lcap + 1u < cap) ? lcap + 1u : cap, &llen, 0);
    if (s1 == LSB_OK) {
        *len = llen;
        *method = WM_METHOD_LSB;
        return 0;
    }
    return 2;
}

/* ---------------- 嵌入 ---------------- */

WmStatus wm_embed_bmp(const unsigned char *bmp_in, size_t bmp_len,
                      const unsigned char *msg, size_t msg_len,
                      const WmEmbedOptions *opt,
                      unsigned char **bmp_out, size_t *bmp_out_len)
{
    if (!bmp_in || !bmp_out || !bmp_out_len || (!msg && msg_len > 0) || msg_len == 0)
        return WM_ERR_ARG;

    WmMethod method = (opt && opt->method >= 0) ? opt->method : WM_METHOD_TILE;
    const char *key = (opt && opt->key && opt->key_len > 0) ? opt->key : NULL;
    size_t key_len  = key ? opt->key_len : 0;

    /* 需要密钥时先把载荷包起来 */
    unsigned char *wrapped = NULL;
    size_t         wrapped_len = msg_len;
    const unsigned char *payload = msg;

    if (key) {
        wrapped = wrap_keyed(key, key_len, msg, msg_len, &wrapped_len);
        if (!wrapped) return WM_ERR_ARG;
        payload = wrapped;
    }

    BmpImage *img = bmp_load_mem(bmp_in, bmp_len);
    if (!img) { free(wrapped); return WM_ERR_FORMAT; }

    WmStatus res = WM_OK;

    if (method == WM_METHOD_LSB) {
        LsbStatus st = lsb_embed(img, payload, wrapped_len, 0);
        if (st == LSB_ERR_CAPACITY) res = WM_ERR_CAPACITY;
        else if (st != LSB_OK)      res = WM_ERR_FORMAT;
    } else if (method == WM_METHOD_DCT) {
        Wm2Status st = wm2_embed(img, payload, wrapped_len, 0);
        if (st == WM2_ERR_CAPACITY) res = WM_ERR_CAPACITY;
        else if (st != WM2_OK)      res = WM_ERR_FORMAT;
    } else if (method == WM_METHOD_TILE) {
        Wm3Status st = wm3_embed(img, payload, wrapped_len, 0);
        if (st == WM3_ERR_CAPACITY) res = WM_ERR_CAPACITY;
        else if (st != WM3_OK)      res = WM_ERR_FORMAT;
    } else {
        res = WM_ERR_ARG;
    }

    free(wrapped);

    if (res != WM_OK) { bmp_free(img); return res; }

    unsigned char *out = NULL;
    size_t         olen = 0;
    if (bmp_save_mem(img, &out, &olen) != 0) {
        bmp_free(img);
        return WM_ERR_IO;
    }
    bmp_free(img);

    *bmp_out = out;
    *bmp_out_len = olen;
    return WM_OK;
}

/* ---------------- 提取 / 验证 ---------------- */

static WmStatus extract_core(const unsigned char *bmp_in, size_t bmp_len,
                             const char *key, size_t key_len,
                             char **msg_out, size_t *msg_len_out,
                             WmVerifyInfo *info)
{
    if (!bmp_in || !info) return WM_ERR_ARG;

    BmpImage *img = bmp_load_mem(bmp_in, bmp_len);
    if (!img) return WM_ERR_FORMAT;

    ExtractBuf eb = { NULL, 0 };
    if (alloc_buf(img, &eb) != 0) { bmp_free(img); return WM_ERR_ARG; }

    size_t   plen = 0;
    WmMethod method = WM_METHOD_AUTO;
    int      rc = try_extract(img, eb.buf, eb.cap, &plen, &method);
    bmp_free(img);

    if (rc == 2) { free(eb.buf); return WM_ERR_NO_WATERMARK; }

    memset(info, 0, sizeof(*info));
    info->method = method;
    snprintf(info->method_name, sizeof(info->method_name), "%s", wm_method_name(method));
    info->crc_ok = (rc == 0);

    const unsigned char *body = NULL;
    size_t body_len = 0;
    int keyed = 0, auth = 0;
    WmStatus st = unwrap(eb.buf, plen, key, key_len, &body, &body_len, &keyed, &auth);

    info->keyed = keyed;
    info->authenticated = auth;
    info->msg_len = body_len;

    if (msg_out && msg_len_out && st == WM_OK) {
        char *copy = (char *)malloc(body_len + 1);
        if (!copy) { free(eb.buf); return WM_ERR_ARG; }
        memcpy(copy, body, body_len);
        copy[body_len] = '\0';
        *msg_out = copy;
        *msg_len_out = body_len;
    }

    free(eb.buf);

    if (st != WM_OK) return st;          /* NEED_KEY / BAD_KEY */
    if (rc == 1) return WM_ERR_TAMPERED; /* 内容校验没过 */
    return WM_OK;
}

WmStatus wm_extract_bmp(const unsigned char *bmp_in, size_t bmp_len,
                        const char *key, size_t key_len,
                        char **msg_out, size_t *msg_len,
                        WmVerifyInfo *info)
{
    return extract_core(bmp_in, bmp_len, key, key_len, msg_out, msg_len, info);
}

WmStatus wm_verify_bmp(const unsigned char *bmp_in, size_t bmp_len,
                       const char *key, size_t key_len,
                       WmVerifyInfo *info)
{
    return extract_core(bmp_in, bmp_len, key, key_len, NULL, NULL, info);
}

/* ---------------- 文件接口 ---------------- */

static unsigned char *read_all(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long n = ftell(fp);
    if (n < 0) { fclose(fp); return NULL; }
    rewind(fp);
    unsigned char *buf = (unsigned char *)malloc((size_t)n + 1u);
    if (!buf) { fclose(fp); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    buf[n] = '\0';
    fclose(fp);
    *len = (size_t)n;
    return buf;
}

WmStatus wm_embed_file(const char *in_path, const char *out_path,
                       const char *msg_path, const WmEmbedOptions *opt)
{
    if (!in_path || !out_path || !msg_path) return WM_ERR_ARG;

    size_t blen = 0, mlen = 0;
    unsigned char *bmp = read_all(in_path, &blen);
    if (!bmp) return WM_ERR_IO;
    unsigned char *msg = read_all(msg_path, &mlen);
    if (!msg) { free(bmp); return WM_ERR_IO; }

    unsigned char *out = NULL;
    size_t olen = 0;
    WmStatus st = wm_embed_bmp(bmp, blen, msg, mlen, opt, &out, &olen);
    free(bmp);
    free(msg);
    if (st != WM_OK) return st;

    FILE *fp = fopen(out_path, "wb");
    if (!fp) { free(out); return WM_ERR_IO; }
    size_t n = fwrite(out, 1, olen, fp);
    int ok = (n == olen) && (fclose(fp) == 0);
    free(out);
    return ok ? WM_OK : WM_ERR_IO;
}

WmStatus wm_verify_file(const char *path, const char *key, WmVerifyInfo *info)
{
    if (!path) return WM_ERR_ARG;
    size_t len = 0;
    unsigned char *buf = read_all(path, &len);
    if (!buf) return WM_ERR_IO;
    WmStatus st = wm_verify_bmp(buf, len, key, key ? strlen(key) : 0, info);
    free(buf);
    return st;
}

WmStatus wm_extract_file(const char *path, const char *key,
                         char **msg_out, size_t *msg_len, WmVerifyInfo *info)
{
    if (!path || !msg_out || !msg_len || !info) return WM_ERR_ARG;
    size_t len = 0;
    unsigned char *buf = read_all(path, &len);
    if (!buf) return WM_ERR_IO;
    WmStatus st = wm_extract_bmp(buf, len, key, key ? strlen(key) : 0, msg_out, msg_len, info);
    free(buf);
    return st;
}