/*
 * tilewm.c —— M3 主体：Tile 分块冗余 + 重同步 + 自适应阈值 + 篡改定位
 *
 * 一次嵌入（照着读）：
 *   ① 拼固定 512 bit 帧（头 10 字节 + 正文 + 补 0）
 *   ② 遍历整幅图的每个 8x8 块；用**块在瓦片内的局部坐标**哈希到 0..511 的槽位
 *   ③ 把该槽位的比特写进这个块（自适应阈值）
 *   因为哈希只用局部坐标，所以每个瓦片里是同一份完整水印；
 *   裁剪后只要还有完整瓦片，解码端用同样的局部哈希就能读出来。
 *
 * 提取：
 *   ① 先按 (0,0) 对齐解一遍
 *   ② 不行就试 64 种 (dx,dy) 像素偏移（裁剪不是 8 的倍数时会错半个块）
 *   ③ 解出来的每个比特，统计"有多少块投了反对票"，按 4×4 网格汇总 → 热力图
 */
#include "tilewm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc16.h"
#include "dct.h"
#include "dctwm.h"

#define BLK 8
#define CA_V 3
#define CA_U 2
#define CB_V 2
#define CB_U 3
#define WM3_T_MIN 20.0   /* 阈值下限：压到 q45 也还稳 */
#define WM3_T_MAX 40.0

/* ---------------- 工具 ---------------- */

static int clamp255(double v)
{
    int i = (int)(v + 0.5);
    if (i < 0) return 0;
    if (i > 255) return 255;
    return i;
}

/*
 * 槽位映射：只用"块在瓦片内的局部索引" i = ly*32 + lx ∈ [0,1024)。
 *
 * 为什么不用哈希：哈希 1024→512 会让某些槽位一个块都分不到（生日悖论），
 * 那些比特解码时只能猜 0 —— 实测就错了 2 个比特，整帧作废。
 *
 * 这里用 (i * 397) % 512：397 与 512 互质，所以是一个双射，
 * 每个槽位**恰好**分到 1024/512 = 2 个块，既不漏也不重；乘数又起到了打散作用。
 */
static unsigned slot_of(int lx, int ly)
{
    unsigned i = (unsigned)(ly * WM3_TILE_BLOCKS + lx);
    return (i * 397u) % WM3_FRAME_BITS;
}

/* 取一个 8x8 块的亮度 Y；origin 为像素级偏移（重同步用） */
static void read_y(const BmpImage *img, int px, int py, double y[64])
{
    for (int r = 0; r < BLK; r++) {
        const uint8_t *row = img->pixels + (size_t)(py + r) * img->stride
                           + (size_t)px * 3u;
        for (int c = 0; c < BLK; c++) {
            double b = row[c * 3 + 0];
            double g = row[c * 3 + 1];
            double rr = row[c * 3 + 2];
            y[r * BLK + c] = 0.299 * rr + 0.587 * g + 0.114 * b;
        }
    }
}

static void write_y(BmpImage *img, int px, int py, const double y_old[64], const double y_new[64])
{
    for (int r = 0; r < BLK; r++) {
        uint8_t *row = img->pixels + (size_t)(py + r) * img->stride + (size_t)px * 3u;
        for (int c = 0; c < BLK; c++) {
            double d = y_new[r * BLK + c] - y_old[r * BLK + c];
            row[c * 3 + 0] = (uint8_t)clamp255((double)row[c * 3 + 0] + d);
            row[c * 3 + 1] = (uint8_t)clamp255((double)row[c * 3 + 1] + d);
            row[c * 3 + 2] = (uint8_t)clamp255((double)row[c * 3 + 2] + d);
        }
    }
}

/* 自适应阈值：纹理越强的块给越大余量（抗量化），平坦块给小余量（不显脏） */
static double adaptive_threshold(double a, double b)
{
    double t = 0.6 * (a + b) + 14.0;
    if (t < WM3_T_MIN) t = WM3_T_MIN;      /* 下限决定"能扛多低的 JPEG" */
    if (t > WM3_T_MAX) t = WM3_T_MAX;
    return t;
}

/* 把 1 个比特写进系数对：只抬高较小的那个，保证 |A|-|B| 的符号，差值 ≥ T */
static void force_bit(double F[64], int bit)
{
    double *pa = &F[CA_V * BLK + CA_U];
    double *pb = &F[CB_V * BLK + CB_U];
    double a  = fabs(*pa);
    double b  = fabs(*pb);
    double sa = (*pa < 0.0) ? -1.0 : 1.0;
    double sb = (*pb < 0.0) ? -1.0 : 1.0;
    double T  = adaptive_threshold(a, b);

    if (bit) {
        if (a - b < T) a = b + T;
    } else {
        if (b - a < T) b = a + T;
    }
    *pa = sa * a;
    *pb = sb * b;
}

/* 判决证据：|A|-|B|，正数倾向 1，负数倾向 0 */
static double strength(const double F[64])
{
    return fabs(F[CA_V * BLK + CA_U]) - fabs(F[CB_V * BLK + CB_U]);
}

static int frame_bit(const uint8_t *frame, unsigned slot)
{
    return (frame[slot / 8] >> (7 - (slot % 8))) & 1;
}

/* ---------------- 对外：说明 / 容量 ---------------- */

const char *wm3_status_str(Wm3Status st)
{
    switch (st) {
    case WM3_OK:            return "成功";
    case WM3_ERR_ARG:       return "参数非法";
    case WM3_ERR_CAPACITY:  return "消息太长（Tile 模式单条上限 54 字节）";
    case WM3_ERR_NOT_FOUND: return "未检测到 Tile 水印";
    case WM3_ERR_CORRUPT:   return "Tile 水印头部损坏";
    case WM3_ERR_CRC:       return "CRC 校验失败：内容被改过";
    default:                return "未知错误";
    }
}

size_t wm3_max_payload(void) { return WM3_MAX_PAYLOAD; }

int wm3_tile_count(const BmpImage *img)
{
    if (!img) return 0;
    int bw = img->width / BLK, bh = img->height / BLK;
    int tx = (bw + WM3_TILE_BLOCKS - 1) / WM3_TILE_BLOCKS;
    int ty = (bh + WM3_TILE_BLOCKS - 1) / WM3_TILE_BLOCKS;
    return tx * ty;
}

/* ---------------- 嵌入 ---------------- */

Wm3Status wm3_embed(BmpImage *img, const uint8_t *data, size_t len, int verbose)
{
    if (!img || (!data && len > 0)) return WM3_ERR_ARG;
    if (len == 0 || len > WM3_MAX_PAYLOAD) return WM3_ERR_CAPACITY;

    int bw = img->width / BLK;
    int bh = img->height / BLK;
    if (bw < WM3_TILE_BLOCKS || bh < WM3_TILE_BLOCKS) return WM3_ERR_CAPACITY;

    /* ① 拼帧 */
    uint8_t frame[WM3_FRAME_BYTES];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, "AIGC", 4);
    frame[4] = 2;                       /* 版本 2 = Tile 模式 */
    frame[5] = 2;                       /* 方法 2 */
    frame[6] = (uint8_t)(len & 0xFF);
    frame[7] = (uint8_t)((len >> 8) & 0xFF);

    Crc16Ctx cc;
    crc16_init(&cc);
    crc16_update(&cc, frame + 6, 2);
    crc16_update(&cc, data, len);
    uint16_t crc = crc16_final(&cc);
    frame[8] = (uint8_t)(crc & 0xFF);
    frame[9] = (uint8_t)(crc >> 8);
    memcpy(frame + WM3_HEADER_BYTES, data, len);

    /* ② 逐块写 */
    double y_old[64], y_new[64], F[64];
    int used = 0;

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int lx = bx % WM3_TILE_BLOCKS;
            int ly = by % WM3_TILE_BLOCKS;
            unsigned slot = slot_of(lx, ly);
            int bit = frame_bit(frame, slot);

            int px = bx * BLK, py = by * BLK;
            read_y(img, px, py, y_old);
            dct8x8(y_old, F);
            force_bit(F, bit);
            idct8x8(F, y_new);
            write_y(img, px, py, y_old, y_new);
            used++;
        }
    }

    /* 顺手写 LSB 哨兵：让 verify 的"像素有没有被动过"对 Tile 模式同样有效 */
    (void)wm2_write_sentinel(img, crc);

    if (verbose > 0) {
        printf("[tile] 瓦片 %d×%d 像素，本图共 %d 份完整水印\n",
               WM3_TILE_PX, WM3_TILE_PX, wm3_tile_count(img));
        printf("[tile] 帧长固定 %d bit，消息 %zu 字节，CRC16=0x%04X\n",
               WM3_FRAME_BITS, len, crc);
        printf("[tile] 用块 %d 个（每 bit 约 %.1f 份冗余），自适应阈值 T∈[12,40]\n",
               used, (double)used / (double)WM3_FRAME_BITS);
    }
    return WM3_OK;
}

/* ---------------- 提取 ---------------- */

/*
 * 按给定像素偏移解一帧。
 * 返回 1 表示帧头合法（magic 对），0 表示不像水印。
 * votes 数组用于定位：记录每个槽位的票数与反水票数。
 */
static int decode_at(const BmpImage *img, int dx, int dy,
                     uint8_t frame[WM3_FRAME_BYTES],
                     double *sum, float heat[WM3_CELLS * WM3_CELLS],
                     int collect_heat)
{
    int bw = (img->width - dx) / BLK;
    int bh = (img->height - dy) / BLK;
    if (bw <= 0 || bh <= 0) return 0;

    for (int i = 0; i < WM3_FRAME_BITS; i++) sum[i] = 0.0;

    /* 网格用于定位：按"块在整幅图里的相对位置"分到 4×4 个格子 */
    int gx = bw / WM3_CELLS > 0 ? bw / WM3_CELLS : 1;
    int gy = bh / WM3_CELLS > 0 ? bh / WM3_CELLS : 1;
    int cell_cnt[WM3_CELLS * WM3_CELLS] = { 0 };

    double y[64], F[64];
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int lx = bx % WM3_TILE_BLOCKS;
            int ly = by % WM3_TILE_BLOCKS;
            unsigned slot = slot_of(lx, ly);

            read_y(img, dx + bx * BLK, dy + by * BLK, y);
            dct8x8(y, F);
            double d = strength(F);
            /* 同一个槽位在整幅图里会出现很多次，这里先累加证据 */
            sum[slot] += d;

            if (collect_heat) {
                int cx = bx / gx; if (cx >= WM3_CELLS) cx = WM3_CELLS - 1;
                int cy = by / gy; if (cy >= WM3_CELLS) cy = WM3_CELLS - 1;
                cell_cnt[cy * WM3_CELLS + cx]++;
            }
        }
    }

    /* 投票出比特 */
    memset(frame, 0, WM3_FRAME_BYTES);
    for (int i = 0; i < WM3_FRAME_BITS; i++) {
        if (sum[i] > 0.0) frame[i / 8] = (uint8_t)(frame[i / 8] | (1u << (7 - (i % 8))));
    }

    if (memcmp(frame, "AIGC", 4) != 0) return 0;

    /* 第二次遍历：统计每格里"投反对票"的块比例 → 反水率（用于定位篡改） */
    if (collect_heat && heat) {
        for (int i = 0; i < WM3_CELLS * WM3_CELLS; i++) heat[i] = 0.0f;

        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                int lx = bx % WM3_TILE_BLOCKS;
                int ly = by % WM3_TILE_BLOCKS;
                unsigned slot = slot_of(lx, ly);
                int want = frame_bit(frame, slot) ? 1 : 0;

                read_y(img, dx + bx * BLK, dy + by * BLK, y);
                dct8x8(y, F);
                int got = (strength(F) > 0.0) ? 1 : 0;

                int cx = bx / gx; if (cx >= WM3_CELLS) cx = WM3_CELLS - 1;
                int cy = by / gy; if (cy >= WM3_CELLS) cy = WM3_CELLS - 1;
                if (got != want) heat[cy * WM3_CELLS + cx] += 1.0f;
            }
        }
        for (int cy = 0; cy < WM3_CELLS; cy++) {
            for (int cx = 0; cx < WM3_CELLS; cx++) {
                int idx = cy * WM3_CELLS + cx;
                if (cell_cnt[idx] > 0) heat[idx] /= (float)cell_cnt[idx];
            }
        }
    }
    return 1;
}

Wm3Status wm3_extract(const BmpImage *img, uint8_t *out, size_t out_cap,
                      size_t *out_len, int *resync_dx, int *resync_dy,
                      float heat[WM3_CELLS * WM3_CELLS], int verbose)
{
    if (!img || !out || !out_len) return WM3_ERR_ARG;
    *out_len = 0;
    if (resync_dx) *resync_dx = 0;
    if (resync_dy) *resync_dy = 0;
    if (heat) for (int i = 0; i < WM3_CELLS * WM3_CELLS; i++) heat[i] = 0.0f;

    if (img->width < WM3_TILE_PX || img->height < WM3_TILE_PX) return WM3_ERR_CAPACITY;

    double *sum = (double *)malloc(sizeof(double) * WM3_FRAME_BITS);
    uint8_t frame[WM3_FRAME_BYTES];
    if (!sum) return WM3_ERR_ARG;

    int found = 0, used_dx = 0, used_dy = 0;

    /* ① 先按 (0,0) 试；② 不行就 64 种偏移重同步（裁剪不是 8 的倍数时用） */
    for (int pass = 0; pass < 65 && !found; pass++) {
        int dx = 0, dy = 0;
        if (pass > 0) {
            dx = pass % 8;
            dy = pass / 8;
        }
        if (decode_at(img, dx, dy, frame, sum, NULL, 0)) {
            found = 1;
            used_dx = dx;
            used_dy = dy;
        }
    }

    if (!found) {
        if (verbose > 0) {
            uint8_t dbg[WM3_FRAME_BYTES];
            double  dsum[WM3_FRAME_BITS];
            (void)decode_at(img, 0, 0, dbg, dsum, NULL, 0);
            fprintf(stderr, "[tile][debug] frame[0..11] = ");
            for (int i = 0; i < 12; i++) fprintf(stderr, "%02X ", dbg[i]);
            fprintf(stderr, "  (expect 41 49 47 43 = AIGC)\n");
        }
        free(sum);
        return WM3_ERR_NOT_FOUND;
    }

    if (frame[4] != 2 || frame[5] != 2) {
        free(sum);
        return WM3_ERR_CORRUPT;
    }

    size_t len = (size_t)frame[6] | ((size_t)frame[7] << 8);
    if (len == 0 || len > WM3_MAX_PAYLOAD) {
        free(sum);
        return WM3_ERR_CORRUPT;
    }
    if (out_cap < len + 1u) {
        free(sum);
        return WM3_ERR_ARG;
    }

    memcpy(out, frame + WM3_HEADER_BYTES, len);
    out[len] = '\0';
    *out_len = len;

    /* 重算一遍热力图（用最终确认的偏移） */
    if (heat) {
        float local[WM3_CELLS * WM3_CELLS];
        decode_at(img, used_dx, used_dy, frame, sum, local, 1);
        memcpy(heat, local, sizeof(local));
    }

    if (resync_dx) *resync_dx = used_dx;
    if (resync_dy) *resync_dy = used_dy;

    Crc16Ctx cc;
    crc16_init(&cc);
    crc16_update(&cc, frame + 6, 2);
    crc16_update(&cc, out, len);
    uint16_t calc = crc16_final(&cc);
    uint16_t stored = (uint16_t)(frame[8] | ((uint16_t)frame[9] << 8));

    if (verbose > 0) {
        printf("[tile] 命中：重同步偏移 (%d,%d)，消息 %zu 字节，CRC 存储 0x%04X / 计算 0x%04X\n",
               used_dx, used_dy, len, stored, calc);
    }

    free(sum);
    return (calc == stored) ? WM3_OK : WM3_ERR_CRC;
}
