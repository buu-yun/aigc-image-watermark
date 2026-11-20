/*
 * dctwm.c —— M2 主体：DCT 水印嵌入 / 提取 + CRC + LSB 哨兵
 *
 * 一次嵌入的完整流程（照着这个顺序读代码）：
 *   ① 算容量：块数够不够放下 (头部 80 bit × 5 份) + (正文 × 8 bit × 3 份)
 *   ② 拼 10 字节头（魔数/版本/方法/长度/CRC16）
 *   ③ 逐块处理：算出这块该承载哪个比特 → 取 Y → DCT → 改一对中频系数 → IDCT → 写回 RGB
 *   ④ 最后在像素最低位写 4 字节哨兵（'SN' + CRC），用于判断"像素有没有被动过"
 */
#include "dctwm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc16.h"
#include "dct.h"
#include "lsb.h"

#define BLK 8

/* 中频系数位置（v=行频,u=列频）。挑 3/2 这一对是因为它在 JPEG 量化表里步长适中：
   低频改一点就看得出来，高频直接量化成 0，中频最稳。 */
#define CA_V 3
#define CA_U 2
#define CB_V 2
#define CB_U 3

/* ---------------- 工具 ---------------- */

static size_t block_count(const BmpImage *img)
{
    return (size_t)(img->width / BLK) * (size_t)(img->height / BLK);
}

static int clamp255(double v)
{
    int i = (int)(v + 0.5);
    if (i < 0) return 0;
    if (i > 255) return 255;
    return i;
}

/* 取一个 8×8 块的亮度 Y（BT.601 权重，和 JPEG 用的公式一致） */
static void read_y_block(const BmpImage *img, size_t bi, double y[64])
{
    int bw = img->width / BLK;
    int bx = (int)(bi % (size_t)bw);
    int by = (int)(bi / (size_t)bw);

    for (int r = 0; r < BLK; r++) {
        const uint8_t *row = img->pixels + (size_t)(by * BLK + r) * img->stride
                           + (size_t)(bx * BLK) * 3u;
        for (int c = 0; c < BLK; c++) {
            double b = row[c * 3 + 0];
            double g = row[c * 3 + 1];
            double rr = row[c * 3 + 2];
            y[r * BLK + c] = 0.299 * rr + 0.587 * g + 0.114 * b;
        }
    }
}

/* 把新的 Y 写回 RGB：三个通道加同样的增量 → 亮度变了、色度基本不变，改动最小 */
static void write_y_block(BmpImage *img, size_t bi, const double y_old[64], const double y_new[64])
{
    int bw = img->width / BLK;
    int bx = (int)(bi % (size_t)bw);
    int by = (int)(bi / (size_t)bw);

    for (int r = 0; r < BLK; r++) {
        uint8_t *row = img->pixels + (size_t)(by * BLK + r) * img->stride
                     + (size_t)(bx * BLK) * 3u;
        for (int c = 0; c < BLK; c++) {
            double d = y_new[r * BLK + c] - y_old[r * BLK + c];
            row[c * 3 + 0] = (uint8_t)clamp255((double)row[c * 3 + 0] + d);
            row[c * 3 + 1] = (uint8_t)clamp255((double)row[c * 3 + 1] + d);
            row[c * 3 + 2] = (uint8_t)clamp255((double)row[c * 3 + 2] + d);
        }
    }
}

/* 把 1 个比特写进这一块的系数对：只抬高较小的那个，保证 |A|-|B| 的符号且差值 ≥ T */
static void force_bit(double F[64], int bit)
{
    double *pa = &F[CA_V * BLK + CA_U];
    double *pb = &F[CB_V * BLK + CB_U];
    double a = fabs(*pa);
    double b = fabs(*pb);
    double sa = (*pa < 0.0) ? -1.0 : 1.0;
    double sb = (*pb < 0.0) ? -1.0 : 1.0;

    if (bit) {
        if (a - b < WM2_THRESHOLD) { a = b + WM2_THRESHOLD; }   /* 抬 A */
    } else {
        if (b - a < WM2_THRESHOLD) { b = a + WM2_THRESHOLD; }   /* 抬 B */
    }
    *pa = sa * a;
    *pb = sb * b;
}

/* 读出一块携带的"证据"：|A|-|B|，正数倾向 1，负数倾向 0 */
static double read_bit_strength(const double F[64])
{
    return fabs(F[CA_V * BLK + CA_U]) - fabs(F[CB_V * BLK + CB_U]);
}

/* ---------------- 对外：容量 / 说明 ---------------- */

size_t wm2_capacity_bytes(const BmpImage *img)
{
    if (!img) return 0;
    size_t blocks = block_count(img);
    size_t head   = (size_t)WM2_HEADER_BITS * WM2_REP_HEADER;
    if (blocks <= head) return 0;
    return (blocks - head) / ((size_t)8 * WM2_REP_PAYLOAD);
}

const char *wm2_status_str(Wm2Status st)
{
    switch (st) {
    case WM2_OK:            return "成功";
    case WM2_ERR_ARG:       return "参数非法";
    case WM2_ERR_CAPACITY:  return "容量不足（图太小或消息太长，可换更大的图）";
    case WM2_ERR_NOT_FOUND: return "未检测到 DCT 水印";
    case WM2_ERR_CORRUPT:   return "水印头部损坏或长度非法";
    case WM2_ERR_CRC:       return "CRC 校验失败：内容与嵌入时不一致（疑似被篡改）";
    default:                return "未知错误";
    }
}

/* ---------------- 嵌入 ---------------- */

Wm2Status wm2_embed(BmpImage *img, const uint8_t *data, size_t len, int verbose)
{
    if (!img || (!data && len > 0)) return WM2_ERR_ARG;
    if (len == 0 || len > 65535u) return WM2_ERR_ARG;

    size_t blocks = block_count(img);
    if (blocks == 0) return WM2_ERR_CAPACITY;

    size_t payload_bits = len * 8u;
    size_t need = (size_t)WM2_HEADER_BITS * WM2_REP_HEADER + payload_bits * WM2_REP_PAYLOAD;
    if (need > blocks) return WM2_ERR_CAPACITY;

    /* ① 拼头和 CRC */
    uint8_t hdr[WM2_HEADER_BYTES];
    memcpy(hdr, WM2_MAGIC, 4);
    hdr[4] = (uint8_t)WM2_VERSION;
    hdr[5] = (uint8_t)WM2_METHOD_DCT;
    hdr[6] = (uint8_t)(len & 0xFF);
    hdr[7] = (uint8_t)((len >> 8) & 0xFF);

    Crc16Ctx cc;
    crc16_init(&cc);
    crc16_update(&cc, hdr + 6, 2);          /* 长度也参与校验，防止改长度蒙混过关 */
    crc16_update(&cc, data, len);
    uint16_t crc = crc16_final(&cc);
    hdr[8] = (uint8_t)(crc & 0xFF);
    hdr[9] = (uint8_t)(crc >> 8);

    /* ② 逐块嵌入 */
    size_t head_blocks = (size_t)WM2_HEADER_BITS * WM2_REP_HEADER;
    double y_old[64], y_new[64], F[64];

    for (size_t bi = 0; bi < blocks; bi++) {
        int bit = -1;

        if (bi < head_blocks) {
            size_t j = bi % WM2_HEADER_BITS;                 /* 第 j 个头部比特 */
            bit = (hdr[j / 8] >> (7 - (j % 8))) & 1;
        } else {
            size_t k = bi - head_blocks;
            if (k < payload_bits * WM2_REP_PAYLOAD) {
                size_t p = k % payload_bits;                 /* 第 p 个正文比特 */
                bit = (data[p / 8] >> (7 - (p % 8))) & 1;
            }
        }
        if (bit < 0) continue;                               /* 没用到的块保持原样 */

        read_y_block(img, bi, y_old);
        dct8x8(y_old, F);
        force_bit(F, bit);
        idct8x8(F, y_new);
        write_y_block(img, bi, y_old, y_new);
    }

    /* ③ 哨兵 */
    wm2_write_sentinel(img, crc);

    if (verbose > 0) {
        printf("[dct] 图像 %dx%d → %zu 个 8x8 块（可用 %zu）\n",
               img->width, img->height, blocks, need);
        printf("[dct] 头部 %d bit × %d 份 + 正文 %zu bit × %d 份\n",
               WM2_HEADER_BITS, WM2_REP_HEADER, payload_bits, WM2_REP_PAYLOAD);
        printf("[dct] 系数对 (v=%d,u=%d)/(v=%d,u=%d)，阈值 T=%.0f，CRC16=0x%04X\n",
               CA_V, CA_U, CB_V, CB_U, WM2_THRESHOLD, crc);
        printf("[dct] LSB 哨兵已写入 4 字节（'SN' + CRC16）\n");
    }
    return WM2_OK;
}

/* ---------------- 提取 ---------------- */

/* 读出第 j 个比特的所有副本，软判决求和 */
static int decode_header_bit(const BmpImage *img, size_t j, double *strength_sum)
{
    double F[64], y[64], sum = 0.0;
    for (int t = 0; t < WM2_REP_HEADER; t++) {
        size_t bi = j + (size_t)t * WM2_HEADER_BITS;
        read_y_block(img, bi, y);
        dct8x8(y, F);
        sum += read_bit_strength(F);
    }
    if (strength_sum) *strength_sum = sum;
    return (sum > 0.0) ? 1 : 0;
}

static int decode_payload_bit(const BmpImage *img, size_t p, size_t payload_bits)
{
    double F[64], y[64], sum = 0.0;
    size_t base = (size_t)WM2_HEADER_BITS * WM2_REP_HEADER;
    for (int t = 0; t < WM2_REP_PAYLOAD; t++) {
        size_t bi = base + p + (size_t)t * payload_bits;
        read_y_block(img, bi, y);
        dct8x8(y, F);
        sum += read_bit_strength(F);
    }
    return (sum > 0.0) ? 1 : 0;
}

Wm2Status wm2_extract(const BmpImage *img, uint8_t *out, size_t out_cap,
                      size_t *out_len, uint16_t *hdr_crc, int verbose)
{
    if (!img || !out || !out_len) return WM2_ERR_ARG;
    if (hdr_crc) *hdr_crc = 0;
    *out_len = 0;

    size_t blocks = block_count(img);
    if (blocks < (size_t)WM2_HEADER_BITS * WM2_REP_HEADER) return WM2_ERR_NOT_FOUND;

    /* ① 读头（每个比特 5 份，求和取符号） */
    uint8_t hdr[WM2_HEADER_BYTES];
    memset(hdr, 0, sizeof(hdr));
    double min_strength = 1e18;

    for (size_t j = 0; j < WM2_HEADER_BITS; j++) {
        double s = 0.0;
        int bit = decode_header_bit(img, j, &s);
        if (fabs(s) < min_strength) min_strength = fabs(s);
        if (bit) hdr[j / 8] = (uint8_t)(hdr[j / 8] | (1u << (7 - (j % 8))));
    }

    if (memcmp(hdr, WM2_MAGIC, 4) != 0) return WM2_ERR_NOT_FOUND;
    if (hdr[4] != (uint8_t)WM2_VERSION) return WM2_ERR_CORRUPT;
    if (hdr[5] != (uint8_t)WM2_METHOD_DCT) return WM2_ERR_CORRUPT;

    size_t len = (size_t)hdr[6] | ((size_t)hdr[7] << 8);
    if (len == 0) return WM2_ERR_CORRUPT;

    size_t payload_bits = len * 8u;
    size_t need = (size_t)WM2_HEADER_BITS * WM2_REP_HEADER + payload_bits * WM2_REP_PAYLOAD;
    if (need > blocks) return WM2_ERR_CORRUPT;
    if (out_cap < len + 1u) return WM2_ERR_ARG;

    /* ② 读正文 */
    memset(out, 0, len);
    for (size_t p = 0; p < payload_bits; p++) {
        int bit = decode_payload_bit(img, p, payload_bits);
        if (bit) out[p / 8] = (uint8_t)(out[p / 8] | (1u << (7 - (p % 8))));
    }
    out[len] = '\0';
    *out_len = len;

    uint16_t stored = (uint16_t)(hdr[8] | ((uint16_t)hdr[9] << 8));
    if (hdr_crc) *hdr_crc = stored;

    /* ③ 验 CRC（长度两字节 + 正文） */
    Crc16Ctx cc;
    crc16_init(&cc);
    crc16_update(&cc, hdr + 6, 2);
    crc16_update(&cc, out, len);
    uint16_t calc = crc16_final(&cc);

    if (verbose > 0) {
        printf("[dct] 头部命中：msg %zu 字节，CRC 存储 0x%04X / 计算 0x%04X\n",
               len, stored, calc);
        printf("[dct] 头部最弱一份的判决余量 ≈ %.1f（越小越危险）\n", min_strength);
    }

    return (calc == stored) ? WM2_OK : WM2_ERR_CRC;
}

/* ---------------- LSB 哨兵 ---------------- */

static uint8_t *chan_at(const BmpImage *img, size_t idx)
{
    size_t per_row = (size_t)img->width * 3u;
    size_t y = idx / per_row;
    size_t off = idx % per_row;
    return img->pixels + y * img->stride + off;
}

/* 第 t 份哨兵、第 b 个比特用哪个"比特序号"（再由 lsb_bit_channel_index 映射到通道）
   用 t*1024 + b 这样一个稀疏序列，使 16 份均匀散布在整幅图上。 */
static size_t sentinel_k(int copy, int bit)
{
    return (size_t)copy * 1024u + (size_t)bit;
}

int wm2_write_sentinel(BmpImage *img, uint16_t crc)
{
    if (!img) return -1;
    uint8_t s[4];
    s[0] = 'S';
    s[1] = 'N';
    s[2] = (uint8_t)(crc & 0xFF);
    s[3] = (uint8_t)(crc >> 8);

    size_t total_ch = (size_t)img->width * (size_t)img->height * 3u;

    for (int t = 0; t < WM2_SENTINEL_COPIES; t++) {
        int b = 0;
        for (int i = 0; i < 4; i++) {
            for (int k = 7; k >= 0; k--) {
                size_t idx = sentinel_k(t, b++);
                if (idx >= total_ch) continue;          /* 小图就少写几份 */
                uint8_t *p = chan_at(img, lsb_bit_channel_index(img, idx));
                *p = (uint8_t)((*p & 0xFEu) | ((s[i] >> k) & 1u));
            }
        }
    }
    return 0;
}

/* 读第 t 份哨兵，成功把 CRC 写进 *out_crc */
static int read_copy(const BmpImage *img, int t, uint16_t *out_crc)
{
    uint8_t s[4] = { 0, 0, 0, 0 };
    size_t  total_ch = (size_t)img->width * (size_t)img->height * 3u;
    int     b = 0;

    for (int i = 0; i < 4; i++) {
        for (int k = 7; k >= 0; k--) {
            size_t idx = sentinel_k(t, b++);
            if (idx >= total_ch) return 0;              /* 这份没写全，视为无效 */
            uint8_t v = (uint8_t)(*chan_at(img, lsb_bit_channel_index(img, idx)) & 1u);
            s[i] = (uint8_t)(s[i] | (uint8_t)(v << k));
        }
    }
    if (s[0] != 'S' || s[1] != 'N') return 0;
    if (out_crc) *out_crc = (uint16_t)(s[2] | ((uint16_t)s[3] << 8));
    return 1;
}

int wm2_read_sentinel(const BmpImage *img, uint16_t *crc)
{
    if (!img) return 0;
    for (int t = 0; t < WM2_SENTINEL_COPIES; t++) {
        if (read_copy(img, t, crc)) return 1;
    }
    return 0;
}

int wm2_sentinel_check(const BmpImage *img, uint16_t crc, int *intact)
{
    if (intact) *intact = 0;
    if (!img) return 0;

    int good = 0;
    for (int t = 0; t < WM2_SENTINEL_COPIES; t++) {
        uint16_t c = 0;
        if (read_copy(img, t, &c) && c == crc) good++;
    }
    if (intact) *intact = good;
    return good > 0;
}
