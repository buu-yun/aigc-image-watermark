/*
 * crc16.c —— CRC16-CCITT 实现（查表法）
 *
 * 原理：把数据看成一个很长的二进制多项式，除以生成多项式 0x11021（截断成 0x1021），
 *       余数就是 CRC。任何一位翻转都会让余数变化，所以能发现错误。
 * 查表法：一次处理 1 字节，把 8 次移位异或预先算进 256 项表里。
 */
#include "crc16.h"

#define CRC16_POLY 0x1021u

static uint16_t g_table[256];
static int      g_ready = 0;

static void build_table(void)
{
    for (unsigned i = 0; i < 256u; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (int b = 0; b < 8; b++) {
            if (c & 0x8000u) c = (uint16_t)((c << 1) ^ CRC16_POLY);
            else             c = (uint16_t)(c << 1);
        }
        g_table[i] = c;
    }
    g_ready = 1;
}

void crc16_init(Crc16Ctx *ctx)
{
    if (!ctx) return;
    if (!g_ready) build_table();
    ctx->crc = 0xFFFFu;      /* CCITT 初值 */
}

void crc16_update(Crc16Ctx *ctx, const uint8_t *data, size_t len)
{
    if (!ctx || !data) return;
    if (!g_ready) build_table();

    uint16_t c = ctx->crc;
    for (size_t i = 0; i < len; i++) {
        c = (uint16_t)((c << 8) ^ g_table[((c >> 8) ^ data[i]) & 0xFFu]);
    }
    ctx->crc = c;
}

uint16_t crc16_final(const Crc16Ctx *ctx)
{
    return ctx ? ctx->crc : 0;
}

uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    Crc16Ctx ctx;
    crc16_init(&ctx);
    crc16_update(&ctx, data, len);
    return crc16_final(&ctx);
}
