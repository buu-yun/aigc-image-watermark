/*
 * lsb.c —— LSB 隐写核心实现
 *
 * 关键设计（答辩三句话）：
 *   1) 为什么挑最低位：像素值只动 ±1，人眼与显示器都分不出来；
 *      但任何有损压缩、滤波、重编码都会把它抹掉 —— 所以 LSB 天然适合「一改就坏」的篡改检测。
 *   2) 为什么按 B→G→R 循环、并且要铺开：人眼对蓝光最不敏感，视觉代价最低；
 *      更重要的是「均匀铺开」——若顺序写入，几百字节的水印会全挤在图像顶部几行，
 *      裁掉顶部或只改画面中部都伤不到水印，检测就形同虚设。这里用与通道总数互质的固定步长
 *      跳着写，让水印均匀铺满整幅图，任何局部改动都会命中一部分比特。
 *   3) 为什么要有 10 字节头：把「有没有水印 / 多长 / 用什么方法」写在最前面，
 *      提取端才能安全地按长度取数据；版本+方法字节为 M2/M4 扩展预留。
 *
 * 注意：M1 还没有完整性校验。故意改坏 1 个 bit 依然可能「成功」取出错误内容 ——
 *       这正是 M2 要补 CRC 的原因（先暴露问题，再解决问题，答辩更有说服力）。
 */
#include "lsb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 通道级访问 ---------------- */

/* 参与隐写的通道总数：每个像素 3 个通道(B,G,R) */
static size_t channel_total(const BmpImage *img)
{
    return (size_t)img->width * (size_t)img->height * 3u;
}

/* 一行内的有效字节数（不含行填充），保证不会把 padding 当载体 */
static size_t row_bytes(const BmpImage *img)
{
    return (size_t)img->width * 3u;
}

/* 第 idx 个通道的指针。自然扫描顺序：(y=0..h-1) × (x=0..w-1) × (B,G,R) */
static uint8_t *channel_at(const BmpImage *img, size_t idx)
{
    size_t per_row = row_bytes(img);
    size_t y       = idx / per_row;
    size_t off     = idx % per_row;
    return img->pixels + y * img->stride + off;
}

/* ---------------- 铺开映射（关键） ---------------- */

static size_t gcd_size(size_t a, size_t b)
{
    while (b != 0) {
        size_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/*
 * 计算铺开步长。
 * 要求 step 与通道总数互质，这样 k*step mod total 在 k ∈ [0, total) 上恰好遍历所有通道，
 * 既不会重复覆盖同一个通道，也不会漏掉整片区域。
 * 从质数 7919 起步，不互质就 +1：同一张图两端算出同一个值，提取端必然复现。
 */
static size_t stride_step(size_t total)
{
    size_t step = LSB_STRIDE_BASE;
    if (total == 0) return 1;
    step %= total;
    if (step == 0) step = 1;
    while (gcd_size(step, total) != 1) {
        step++;
        if (step >= total) step = 1;
    }
    return step;
}

/* 第 k 个比特落在哪个通道号上 */
static size_t bit_channel_index(size_t k, size_t total, size_t step)
{
    unsigned long long pos = (unsigned long long)k * (unsigned long long)step;
    return (size_t)(pos % (unsigned long long)total);
}

/* 对外暴露给自检/篡改定位用 */
size_t lsb_bit_channel_index(const BmpImage *img, size_t k)
{
    if (!img) return 0;
    size_t total = channel_total(img);
    if (total == 0) return 0;
    return bit_channel_index(k, total, stride_step(total));
}

/* ---------------- 比特级读写 ---------------- */

/* 从第 k 个比特位置开始，按「字节高位在前」把 len 字节逐位写进最低位 */
static void write_bits(BmpImage *img, size_t k_start, const uint8_t *data, size_t len)
{
    size_t total = channel_total(img);
    size_t step  = stride_step(total);
    size_t k     = k_start;

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int b = 7; b >= 0; b--) {
            uint8_t *p = channel_at(img, bit_channel_index(k++, total, step));
            *p = (uint8_t)((*p & 0xFEu) | ((byte >> b) & 1u)); /* 清最低位再写入 */
        }
    }
}

/* 从第 k 个比特位置开始读回 len 字节 */
static void read_bits(const BmpImage *img, size_t k_start, uint8_t *out, size_t len)
{
    size_t total = channel_total(img);
    size_t step  = stride_step(total);
    size_t k     = k_start;

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = 0;
        for (int b = 7; b >= 0; b--) {
            uint8_t v = (uint8_t)(*channel_at(img, bit_channel_index(k++, total, step)) & 1u);
            byte = (uint8_t)((byte << 1) | v);
        }
        out[i] = byte;
    }
}

/* 小端 32 位读写（头部长度的编码方式） */
static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ---------------- 对外接口 ---------------- */

size_t lsb_capacity_bytes(const BmpImage *img)
{
    if (!img) return 0;
    return channel_total(img) / 8u; /* 每 8 个通道承载 1 字节 */
}

const char *lsb_status_str(LsbStatus st)
{
    switch (st) {
    case LSB_OK:            return "成功";
    case LSB_ERR_ARG:       return "参数非法（缓冲区不足或数据过长）";
    case LSB_ERR_CAPACITY:  return "载体容量不足，请换更大图片或更短消息";
    case LSB_ERR_NOT_FOUND: return "未检测到 LSB 水印（魔数不匹配）";
    case LSB_ERR_CORRUPT:   return "水印头部损坏或长度非法（图像可能已被篡改/重编码）";
    default:                return "未知错误";
    }
}

LsbStatus lsb_embed(BmpImage *img, const uint8_t *data, size_t len, int verbose)
{
    if (!img || (!data && len > 0)) return LSB_ERR_ARG;
    if (len > 0xFFFFFFFFull) return LSB_ERR_ARG;   /* 长度字段只有 4 字节 */

    size_t cap      = lsb_capacity_bytes(img);
    size_t need     = LSB_HEADER_SIZE + len;
    size_t ch_total = channel_total(img);

    /* 精确判断：头 + 消息共 need 字节，需要 need*8 个通道 */
    if (need > cap || need * 8u > ch_total) return LSB_ERR_CAPACITY;

    uint8_t hdr[LSB_HEADER_SIZE];
    memcpy(hdr, LSB_MAGIC, LSB_MAGIC_LEN);
    hdr[4] = (uint8_t)LSB_VERSION;
    hdr[5] = (uint8_t)LSB_METHOD_LSB;
    put_u32le(hdr + 6, (uint32_t)len);

    write_bits(img, 0, hdr, LSB_HEADER_SIZE);
    if (len > 0) write_bits(img, (size_t)LSB_HEADER_SIZE * 8u, data, len);

    if (verbose > 0) {
        double usage = cap > 0 ? (double)need * 100.0 / (double)cap : 0.0;
        printf("[lsb] 载体 %dx%d，可用容量 %zu 字节\n", img->width, img->height, cap);
        printf("[lsb] 本次占用 %zu 字节（头 %d + 消息 %zu），使用率 %.2f%%\n",
               need, LSB_HEADER_SIZE, len, usage);
        printf("[lsb] 步长 %zu（与通道总数 %zu 互质），水印均匀铺满全图\n",
               stride_step(ch_total), ch_total);
    }
    return LSB_OK;
}

LsbStatus lsb_extract(const BmpImage *img, uint8_t *out, size_t out_cap,
                      size_t *out_len, int verbose)
{
    if (!img || !out || !out_len) return LSB_ERR_ARG;
    if (channel_total(img) < (size_t)LSB_HEADER_SIZE * 8u) return LSB_ERR_CORRUPT;

    uint8_t hdr[LSB_HEADER_SIZE];
    read_bits(img, 0, hdr, LSB_HEADER_SIZE);

    if (memcmp(hdr, LSB_MAGIC, LSB_MAGIC_LEN) != 0) return LSB_ERR_NOT_FOUND;
    if (hdr[4] != (uint8_t)LSB_VERSION) {
        fprintf(stderr, "[lsb] 版本不符：期望 %d，实际 %u\n", LSB_VERSION, (unsigned)hdr[4]);
        return LSB_ERR_CORRUPT;
    }
    if (hdr[5] != (uint8_t)LSB_METHOD_LSB) {
        fprintf(stderr, "[lsb] 方法不符：该图不是 LSB 载体（method=%u）\n", (unsigned)hdr[5]);
        return LSB_ERR_CORRUPT;
    }

    uint32_t len = get_u32le(hdr + 6);

    /* 关键防御：声称的长度必须真的放得下，否则是损坏/伪造 */
    size_t avail_bits = channel_total(img) - (size_t)LSB_HEADER_SIZE * 8u;
    if ((uint64_t)len * 8u > (uint64_t)avail_bits) {
        fprintf(stderr, "[lsb] 头部长度 %lu 超出载体可容纳范围，判定为损坏\n",
                (unsigned long)len);
        return LSB_ERR_CORRUPT;
    }
    if (out_cap < (size_t)len + 1u) {
        fprintf(stderr, "[lsb] 输出缓冲区不足：需要 %lu 字节\n",
                (unsigned long)len + 1u);
        return LSB_ERR_ARG;
    }

    if (len > 0) read_bits(img, (size_t)LSB_HEADER_SIZE * 8u, out, len);
    out[len] = '\0';
    *out_len = (size_t)len;

    if (verbose > 0) {
        printf("[lsb] 命中水印：版本 %u，方法 %u(LSB)，消息长度 %lu 字节\n",
               (unsigned)hdr[4], (unsigned)hdr[5], (unsigned long)len);
    }
    return LSB_OK;
}

LsbStatus lsb_peek(const BmpImage *img, uint32_t *msg_len)
{
    if (!img) return LSB_ERR_ARG;
    if (channel_total(img) < (size_t)LSB_HEADER_SIZE * 8u) return LSB_ERR_CORRUPT;

    uint8_t hdr[LSB_HEADER_SIZE];
    read_bits(img, 0, hdr, LSB_HEADER_SIZE);

    if (memcmp(hdr, LSB_MAGIC, LSB_MAGIC_LEN) != 0) return LSB_ERR_NOT_FOUND;
    if (hdr[4] != (uint8_t)LSB_VERSION || hdr[5] != (uint8_t)LSB_METHOD_LSB)
        return LSB_ERR_CORRUPT;

    uint32_t len   = get_u32le(hdr + 6);
    size_t   avail = channel_total(img) - (size_t)LSB_HEADER_SIZE * 8u;
    if ((uint64_t)len * 8u > (uint64_t)avail) return LSB_ERR_CORRUPT;

    if (msg_len) *msg_len = len;
    return LSB_OK;
}
