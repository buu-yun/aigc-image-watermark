/*
 * bmp.c —— 24 位 BMP 读写实现（M1 手写，答辩要能逐行讲）
 *
 * BMP 文件结构（24 位、无压缩）：
 *   [14 字节 文件头 BITMAPFILEHEADER]
 *      'B' 'M' | 文件总大小(4) | 保留(4) | 像素数据偏移(4)
 *   [40 字节 信息头 BITMAPINFOHEADER]
 *      头大小(4) | 宽(4) | 高(4) | 平面数(2) | 位深(2) | 压缩方式(4)
 *      图像大小(4) | X 分辨率(4) | Y 分辨率(4) | 调色板颜色数(4) | 重要颜色数(4)
 *   [像素数组]
 *      每像素 3 字节，顺序是 B,G,R（不是 RGB！）
 *      每行字节数 = 宽*3，然后向上补齐到 4 的整数倍（行填充 padding）
 *      高度为正：自下而上存；高度为负：自上而下存（本文件两种都能读）
 *
 * 全部整数均为小端序，所以这里手写 rd16/rd32/wr16/wr32。
 *
 * 结构上分两层：
 *   bmp_load_mem / bmp_save_mem —— 只认内存缓冲（Web 服务直接用这层，不落盘）
 *   bmp_load / bmp_save         —— 文件包装（CLI 用这层）
 */
#include "bmp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BMP_FILE_HEADER_SIZE 14
#define BMP_INFO_HEADER_SIZE 40
#define BMP_HEADER_TOTAL     (BMP_FILE_HEADER_SIZE + BMP_INFO_HEADER_SIZE) /* 54 */

/* 单边最大尺寸与最大像素缓冲，防止恶意/损坏文件把内存吃爆 */
#define BMP_MAX_DIM    20000
#define BMP_MAX_BYTES  (512u * 1024u * 1024u)

/* ---------------- 小端序读写 ---------------- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* 一行像素占用的字节数：width*3 向上对齐到 4 的倍数 */
static size_t row_stride(int width)
{
    return (((size_t)width * 3u) + 3u) & ~(size_t)3u;
}

/* 把 errno 翻成人话（Windows 上最常见的就是"只读/被占用"） */
static const char *os_reason(int e)
{
    switch (e) {
    case EACCES: return "拒绝访问：文件只读，或正被其它程序（看图软件/微信/杀软）占用";
    case ENOENT: return "路径不存在";
    case ENOSPC: return "磁盘空间不足";
    default:     return strerror(e);
    }
}

/* ---------------- 生命周期 ---------------- */

BmpImage *bmp_create(int width, int height)
{
    if (width <= 0 || height <= 0 || width > BMP_MAX_DIM || height > BMP_MAX_DIM) {
        fprintf(stderr, "[bmp] 非法尺寸 %dx%d\n", width, height);
        return NULL;
    }

    size_t stride = row_stride(width);
    size_t total  = stride * (size_t)height;
    if (total > BMP_MAX_BYTES) {
        fprintf(stderr, "[bmp] 图像过大 (%zu 字节)\n", total);
        return NULL;
    }

    BmpImage *img = (BmpImage *)malloc(sizeof(BmpImage));
    if (!img) {
        fprintf(stderr, "[bmp] 内存不足\n");
        return NULL;
    }

    img->pixels = (uint8_t *)calloc(total, 1); /* calloc: 行填充字节自然为 0 */
    if (!img->pixels) {
        free(img);
        fprintf(stderr, "[bmp] 内存不足\n");
        return NULL;
    }

    img->width  = width;
    img->height = height;
    img->stride = stride;
    return img;
}

void bmp_free(BmpImage *img)
{
    if (!img) return;
    free(img->pixels);
    free(img);
}

uint8_t *bmp_pixel(const BmpImage *img, int x, int y)
{
    if (!img || x < 0 || y < 0 || x >= img->width || y >= img->height) return NULL;
    return img->pixels + (size_t)y * img->stride + (size_t)x * 3u;
}

/* ---------------- 内存版：解析 ---------------- */

BmpImage *bmp_load_mem(const uint8_t *data, size_t len)
{
    if (!data || len < BMP_HEADER_TOTAL) {
        fprintf(stderr, "[bmp] 数据太短，不是合法 BMP\n");
        return NULL;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        fprintf(stderr, "[bmp] 缺少 'BM' 魔数，不是 BMP\n");
        return NULL;
    }

    uint32_t off_bits = rd32(data + 10);   /* 像素数据偏移 */
    const uint8_t *ih = data + BMP_FILE_HEADER_SIZE;

    uint32_t ih_size = rd32(ih + 0);
    if (ih_size < BMP_INFO_HEADER_SIZE) {
        fprintf(stderr, "[bmp] 仅支持 BITMAPINFOHEADER(40 字节)及以上的 BMP，当前 %u 字节\n",
                (unsigned)ih_size);
        return NULL;
    }

    int32_t  w         = (int32_t)rd32(ih + 4);
    int32_t  h_raw     = (int32_t)rd32(ih + 8);
    uint16_t planes    = rd16(ih + 12);
    uint16_t bit_count = rd16(ih + 14);
    uint32_t comp      = rd32(ih + 16);

    int top_down = 0;
    int height   = h_raw;
    if (h_raw < 0) {          /* 负高度 = 自上而下存储 */
        top_down = 1;
        height   = -h_raw;
    }

    if (planes != 1 || bit_count != 24 || comp != 0) {
        fprintf(stderr, "[bmp] 仅支持 24 位、无压缩(BI_RGB) BMP：planes=%u bpp=%u comp=%u\n",
                (unsigned)planes, (unsigned)bit_count, (unsigned)comp);
        return NULL;
    }
    if (w <= 0 || height <= 0 || w > BMP_MAX_DIM || height > BMP_MAX_DIM) {
        fprintf(stderr, "[bmp] 非法图像尺寸 %dx%d\n", (int)w, height);
        return NULL;
    }

    size_t src_stride = row_stride((int)w);
    size_t need       = (size_t)off_bits + src_stride * (size_t)height;
    if (off_bits < BMP_HEADER_TOTAL || need > len) {
        fprintf(stderr, "[bmp] 像素数据被截断（需要 %zu 字节，实际 %zu 字节）\n", need, len);
        return NULL;
    }

    BmpImage *img = bmp_create((int)w, height);
    if (!img) return NULL;

    for (int y = 0; y < height; y++) {
        const uint8_t *src = data + off_bits + (size_t)y * src_stride;
        int dst_y = top_down ? y : (height - 1 - y);
        memcpy(img->pixels + (size_t)dst_y * img->stride, src, (size_t)w * 3u);
    }
    return img;
}

/* ---------------- 内存版：序列化 ---------------- */

int bmp_save_mem(const BmpImage *img, uint8_t **out, size_t *out_len)
{
    if (!img || !out || !out_len) return -1;

    size_t   pixels_bytes = img->stride * (size_t)img->height;
    size_t   total        = BMP_HEADER_TOTAL + pixels_bytes;
    uint32_t file_size    = (uint32_t)total; /* 本工程图像远小于 4GB */

    uint8_t *buf = (uint8_t *)calloc(total, 1); /* 行填充字节保持 0 */
    if (!buf) {
        fprintf(stderr, "[bmp] 内存不足\n");
        return -1;
    }

    /* 14 字节文件头 */
    buf[0] = 'B';
    buf[1] = 'M';
    wr32(buf + 2,  file_size);
    wr32(buf + 6,  0);                 /* 保留 */
    wr32(buf + 10, BMP_HEADER_TOTAL);  /* 像素数据偏移 = 54 */

    /* 40 字节信息头 */
    wr32(buf + 14, BMP_INFO_HEADER_SIZE);
    wr32(buf + 18, (uint32_t)img->width);
    wr32(buf + 22, (uint32_t)img->height); /* 正数 = 自下而上存储 */
    wr16(buf + 26, 1);                     /* planes */
    wr16(buf + 28, 24);                    /* 位深 */
    wr32(buf + 30, 0);                     /* BI_RGB 无压缩 */
    wr32(buf + 34, (uint32_t)pixels_bytes);
    wr32(buf + 38, 2835); /* 水平分辨率 ≈72 DPI */
    wr32(buf + 42, 2835); /* 垂直分辨率 */
    wr32(buf + 46, 0);    /* 调色板颜色数 */
    wr32(buf + 50, 0);    /* 重要颜色数 */

    /* 自下而上写：先写内存最后一行 */
    for (int y = img->height - 1; y >= 0; y--) {
        const uint8_t *row = img->pixels + (size_t)y * img->stride;
        uint8_t *dst = buf + BMP_HEADER_TOTAL + (size_t)(img->height - 1 - y) * img->stride;
        memcpy(dst, row, img->stride);
    }

    *out     = buf;
    *out_len = total;
    return 0;
}

/* ---------------- 文件包装 ---------------- */

BmpImage *bmp_load(const char *path)
{
    if (!path) return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[bmp] 打不开文件: %s\n", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return NULL; }
    rewind(fp);

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fprintf(stderr, "[bmp] 内存不足\n");
        fclose(fp);
        return NULL;
    }
    if (size > 0 && fread(data, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "[bmp] 读文件失败: %s\n", path);
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    BmpImage *img = bmp_load_mem(data, (size_t)size);
    free(data);
    return img;
}

int bmp_save(const BmpImage *img, const char *path)
{
    uint8_t *buf = NULL;
    size_t   len = 0;
    if (bmp_save_mem(img, &buf, &len) != 0) return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        /* 把系统原因也打出来：Windows 上多半是文件被占用/只读，或权限状态异常 */
        fprintf(stderr, "[bmp] 无法创建文件: %s（%s）\n", path, os_reason(errno));
        fprintf(stderr, "[bmp] 提示: 若该文件正在被看图软件/微信打开，请关闭后重试；"
                        "或先删除它，或换一个输出文件名。\n");
        free(buf);
        return -1;
    }
    size_t n = fwrite(buf, 1, len, fp);
    int ok = (n == len) && (fclose(fp) == 0);
    free(buf);

    if (!ok) {
        fprintf(stderr, "[bmp] 写文件失败: %s\n", path);
        return -1;
    }
    return 0;
}

/* ---------------- 辅助 ---------------- */

int bmp_diff_max(const BmpImage *a, const BmpImage *b)
{
    if (!a || !b || a->width != b->width || a->height != b->height) return -1;

    int maxd = 0;
    size_t row_bytes = (size_t)a->width * 3u;
    for (int y = 0; y < a->height; y++) {
        const uint8_t *ra = a->pixels + (size_t)y * a->stride;
        const uint8_t *rb = b->pixels + (size_t)y * b->stride;
        for (size_t i = 0; i < row_bytes; i++) {
            int d = (int)ra[i] - (int)rb[i];
            if (d < 0) d = -d;
            if (d > maxd) maxd = d;
        }
    }
    return maxd;
}
