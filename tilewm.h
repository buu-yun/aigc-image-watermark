/*
 * tilewm.h —— M3：分块冗余（Tile）水印，抗裁剪 + 篡改定位
 *
 * 与 M2 的 dctwm 的关系：
 *   dctwm (-m dct)   ：整幅图按顺序分配比特，容量大（1024×768 可放 495 字节），
 *                      但**裁一刀就全乱**（块序号一变，比特全错位）。
 *   tilewm (-m tile) ：把图切成 256×256 的瓦片，每个瓦片里存一份完整水印，
 *                      裁掉一部分，剩下的瓦片照样能读出整条消息。
 *                      代价：单条消息上限 54 字节（512 bit 帧 / 每 bit 2 份冗余）。
 *
 * 帧格式（固定 512 bit = 64 字节，不足补 0）：
 *   [0..3] 'A''I''G''C' | [4] ver=2 | [5] method=2 | [6..7] len(小端)
 *   [8..9] CRC16 | [10..] 正文 | 其余补 0
 *   帧长固定，是为了让"比特槽位"数量与消息长度解耦——解码端不必先知道长度。
 *
 * 瓦片内的比特分配用**局部坐标**哈希：同一个槽位在每个瓦片里落在相同的相对位置，
 * 所以解码端只要找到任意一个完整瓦片就能读出全部内容，不需要知道它是第几个瓦片。
 */
#ifndef TILEWM_H
#define TILEWM_H

#include <stddef.h>
#include <stdint.h>

#include "bmp.h"

#define WM3_TILE_BLOCKS  32                        /* 32×32 个 8x8 块 = 256×256 像素 */
#define WM3_TILE_PX      (WM3_TILE_BLOCKS * 8)
#define WM3_FRAME_BITS   512                       /* 固定帧长 */
#define WM3_FRAME_BYTES  (WM3_FRAME_BITS / 8)      /* 64 */
#define WM3_HEADER_BYTES 10
#define WM3_MAX_PAYLOAD  (WM3_FRAME_BYTES - WM3_HEADER_BYTES)   /* 54 字节 */

#define WM3_CELLS        4                         /* 定位用的网格：4×4 个格子 */

typedef enum {
    WM3_OK            = 0,
    WM3_ERR_ARG       = -1,
    WM3_ERR_CAPACITY  = -2,
    WM3_ERR_NOT_FOUND = -3,
    WM3_ERR_CORRUPT   = -4,
    WM3_ERR_CRC       = -5
} Wm3Status;

const char *wm3_status_str(Wm3Status st);

/* 单条消息上限（字节） */
size_t wm3_max_payload(void);

/* 能放几份完整水印（瓦片数），决定抗裁剪比例 */
int wm3_tile_count(const BmpImage *img);

Wm3Status wm3_embed(BmpImage *img, const uint8_t *data, size_t len, int verbose);

/*
 * 提取：成功返回 WM3_OK；CRC 不匹配返回 WM3_ERR_CRC（内容仍写进 out）。
 * 参数 resync 返回实际用到的重同步偏移（0,0 表示没裁剪/8 的倍数）；
 * heat[16] 返回 4×4 网格里每个格子的"反水率"（0..1），用于画篡改热力图。
 */
Wm3Status wm3_extract(const BmpImage *img, uint8_t *out, size_t out_cap,
                      size_t *out_len, int *resync_dx, int *resync_dy,
                      float heat[WM3_CELLS * WM3_CELLS], int verbose);

#endif /* TILEWM_H */
