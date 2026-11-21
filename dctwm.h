/*
 * dctwm.h —— M2：频域（DCT）鲁棒水印 + CRC 校验 + LSB 篡改哨兵
 *
 * 与 M1 的 LSB 分工：
 *   DCT 负责「扛」：发微信、转 JPG 之后还能把文字取回来（溯源）
 *   LSB 负责「灵敏」：像素被动过就立刻报警（验真）
 *
 * 载荷格式（10 字节头，与 LSB 方法保持一致的思路）：
 *   [0..3] 'A''I''G''C'   魔数
 *   [4]    version = 1
 *   [5]    method  = 1    （0=LSB，1=DCT）
 *   [6..7] len（小端，16 位：≤65535 字节够用）
 *   [8..9] CRC16-CCITT（对 len 两字节 + 正文一起算）
 *
 * 嵌入方式（每块只藏 1 bit，把容量全换成鲁棒性）：
 *   取一对中频系数 A=(v=3,u=2)、B=(v=2,u=3)，比较 |A| 与 |B|：
 *     bit=1 → 强制 |A| - |B| ≥ T
 *     bit=0 → 强制 |B| - |A| ≥ T
 *   只抬高较小的那个系数，能量变化最小；T 是抗压缩余量（默认 24）。
 *   JPEG 会把两个系数同时缩放，但"谁大谁小"基本不变 —— 这就是抗压缩的根。
 *
 * 冗余：
 *   头部每个比特写 5 份，正文每个比特写 3 份，落在间隔 80 / N 的块上；
 *   提取时把 3 份的差值**求和再取符号**（软判决，比投票更耐噪）。
 */
#ifndef DCTWM_H
#define DCTWM_H

#include <stddef.h>
#include <stdint.h>

#include "bmp.h"

#define WM2_MAGIC         "AIGC"
#define WM2_VERSION       1
#define WM2_METHOD_DCT    1
#define WM2_HEADER_BYTES  10
#define WM2_HEADER_BITS   80
#define WM2_REP_HEADER    5     /* 头部每比特重复 5 份 */
#define WM2_REP_PAYLOAD   3     /* 正文每比特重复 3 份 */
#define WM2_THRESHOLD     24.0  /* 抗压缩余量 T */

typedef enum {
    WM2_OK            = 0,
    WM2_ERR_ARG       = -1,
    WM2_ERR_CAPACITY  = -2,   /* 图太小 / 消息太长 */
    WM2_ERR_NOT_FOUND = -3,   /* 没有 DCT 水印 */
    WM2_ERR_CORRUPT   = -4,   /* 头部非法（方法/长度不对） */
    WM2_ERR_CRC       = -5    /* CRC 不匹配 → 内容被改过 */
} Wm2Status;

/* 可藏正文的字节数 = (块数 - 头部占用) / (8 × 冗余份数) */
size_t wm2_capacity_bytes(const BmpImage *img);

const char *wm2_status_str(Wm2Status st);

/* 嵌入：成功后像素已就地修改 */
Wm2Status wm2_embed(BmpImage *img, const uint8_t *data, size_t len, int verbose);

/*
 * 提取：成功返回 WM2_OK；CRC 不匹配返回 WM2_ERR_CRC（此时 out/out_len 仍然有效，
 * 调用方可以把内容显示出来但提示"已被篡改"）。
 * hdr_crc 返回头部里记录的 CRC，用来和 LSB 哨兵比对。
 */
Wm2Status wm2_extract(const BmpImage *img, uint8_t *out, size_t out_cap,
                      size_t *out_len, uint16_t *hdr_crc, int verbose);

/*
 * LSB 篡改哨兵
 *   内容只有 4 字节（'S' 'N' + CRC16），但**复制 16 份**、均匀铺满全图。
 *   为什么必须多份：只写一份等于"只抽查 32 个通道"，
 *   画面别处被涂掉它根本看不见（实测 25% 涂改漏报）。
 *   16 份铺开后，任何一块区域被动过，都会有若干份坏掉 →
 *   "完好的份数 < 总份数"就说明像素被改过。
 */
#define WM2_SENTINEL_COPIES 16

int wm2_write_sentinel(BmpImage *img, uint16_t crc);
int wm2_read_sentinel(const BmpImage *img, uint16_t *crc);   /* 1=命中，0=没有 */

/*
 * 统计 16 份哨兵里有多少份完好（魔数 'S' 'N' 还在，且 CRC 与头部一致）。
 * intact 返回完好份数；返回值 1 表示至少有 1 份可用。
 */
/* 只数"魔数完好"的份数（不比对 CRC），并回传其中一份的 CRC —— verify 用 */
int wm2_sentinel_scan(const BmpImage *img, int *intact, uint16_t *crc_out);

int wm2_sentinel_check(const BmpImage *img, uint16_t crc, int *intact);

#endif /* DCTWM_H */
