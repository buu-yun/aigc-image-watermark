/*
 * crc16.h —— CRC16-CCITT（多项式 0x1021，初值 0xFFFF，不反转）
 *
 * 为什么用 CRC16 而不是 MD5/SHA：
 *   DCT 水印的容量只有百来字节，MD5 要 16 字节、SHA256 要 32 字节，太贵。
 *   CRC16 只占 2 字节，能抓出绝大多数比特错误，够用来判断"内容是否还完整"。
 *
 * 已知局限（要写进报告）：
 *   CRC 只能防"偶然损坏"，防不住"主动伪造"——算法公开，攻击者可以重算 CRC。
 *   真正的防伪需要密钥（HMAC），留到 M4。
 */
#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

uint16_t crc16_ccitt(const uint8_t *data, size_t len);

/* 分两段算：先传头部 2 字节（长度），再传正文，省掉一次拼接 */
typedef struct {
    uint16_t crc;
} Crc16Ctx;

void     crc16_init(Crc16Ctx *ctx);
void     crc16_update(Crc16Ctx *ctx, const uint8_t *data, size_t len);
uint16_t crc16_final(const Crc16Ctx *ctx);

#endif /* CRC16_H */
