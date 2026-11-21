/*
 * main.c —— M1 命令行工具：embed / extract / info / diff / selftest / bench
 *
 * 为什么要有 selftest：验收不能只靠肉眼「看着像没坏」，
 * 这里用内置测试用例把「能藏能取」变成一条命令给出的 PASS/FAIL，
 * 答辩时可以现场跑，也方便 M2/M3 回归。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bmp.h"
#include "crc16.h"
#include "dctwm.h"
#include "lsb.h"
#include "tilewm.h"

/* ==================== 通用工具 ==================== */

static void usage(void)
{
    printf(
        "AI 图像可信水印引擎  M1：LSB 隐写最小闭环\n"
        "\n"
        "用法:\n"
        "  wm embed    -i <载体.bmp> -o <含水印.bmp> -f <消息.txt> [-m dct|tile|lsb] [-q]\n"
        "  wm embed    -i <载体.bmp> -o <含水印.bmp> -t <文本>     [-q]\n"
        "  wm extract  -i <含水印.bmp> [-o <输出.txt>] [-q]\n"
        "  wm info     -i <图片.bmp>\n"
        "  wm verify   -i <待验.bmp> [-o <明文.txt>]     三态：真 / 可疑 / 假\n"
        "  wm heatmap  -i <待验.bmp> -o <热力图.bmp>       标出被改的格子（Tile 模式）\n"
        "  wm diff     -i <原图.bmp>   -j <含水印.bmp>\n"
        "  wm tamper   -i <图.bmp> -o <改后.bmp> -r <x,y,w,h> [-c B,G,R]\n"
        "  wm selftest\n"
        "  wm bench\n"
        "\n"
        "说明: 消息按字节原样嵌入。中文请用 UTF-8 编码的 txt 文件(-f 方式)，\n"
        "      避免 Windows 控制台代码页把参数转码成 GBK 导致取回内容不一致。\n");
}

/* 读整个文件到内存，末尾补 '\0'，方便当文本用；失败返回 NULL */
static uint8_t *read_all(const char *path, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[err] 打不开文件: %s\n", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return NULL; }
    rewind(fp);

    uint8_t *buf = (uint8_t *)malloc((size_t)size + 1u);
    if (!buf) {
        fprintf(stderr, "[err] 内存不足\n");
        fclose(fp);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "[err] 读取失败: %s\n", path);
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[size] = '\0';
    fclose(fp);
    *len_out = (size_t)size;
    return buf;
}

/* 统计两图差异：最大通道差、被改动的通道数 */
static void analyze_diff(const BmpImage *a, const BmpImage *b, int *maxd, size_t *changed,
                         double *sse_out)
{
    *maxd    = 0;
    *changed = 0;
    double sse = 0.0;
    size_t row_bytes = (size_t)a->width * 3u;

    for (int y = 0; y < a->height; y++) {
        const uint8_t *ra = a->pixels + (size_t)y * a->stride;
        const uint8_t *rb = b->pixels + (size_t)y * b->stride;
        for (size_t i = 0; i < row_bytes; i++) {
            int d = (int)ra[i] - (int)rb[i];
            if (d != 0) (*changed)++;
            sse += (double)d * (double)d;
            if (d < 0) d = -d;
            if (d > *maxd) *maxd = d;
        }
    }
    if (sse_out) *sse_out = sse;
}

/* ==================== embed ==================== */

static int cmd_embed(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL, *msg_file = NULL, *msg_text = NULL;
    int quiet = 0;
    int method = 1;   /* 0=LSB，1=DCT（默认：抗压缩） */

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-i") && i + 1 < argc)      in_path  = argv[++i];
        else if (!strcmp(a, "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(a, "-f") && i + 1 < argc) msg_file = argv[++i];
        else if (!strcmp(a, "-t") && i + 1 < argc) msg_text = argv[++i];
        else if (!strcmp(a, "-m") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "lsb")) method = 0;
            else if (!strcmp(m, "dct")) method = 1;
            else if (!strcmp(m, "tile")) method = 2;
            else { fprintf(stderr, "[err] -m 只能是 dct / tile / lsb\n"); return 2; }
        }
        else if (!strcmp(a, "-q"))                 quiet    = 1;
        else { fprintf(stderr, "[err] 未知参数: %s\n", a); return 2; }
    }

    if (!in_path || !out_path || (!msg_file && !msg_text)) {
        fprintf(stderr, "[err] embed 需要 -i -o 以及 -f 或 -t\n");
        return 2;
    }

    /* 1) 准备消息字节 */
    uint8_t *msg_buf = NULL;
    const uint8_t *msg = NULL;
    size_t msg_len = 0;

    if (msg_file) {
        msg_buf = read_all(msg_file, &msg_len);
        if (!msg_buf) return 1;
        msg = msg_buf;
    } else {
        msg_len = strlen(msg_text);
        msg     = (const uint8_t *)msg_text;
    }

    if (msg_len == 0) {
        fprintf(stderr, "[err] 消息为空，拒绝嵌入（M1 只处理非空文本）\n");
        free(msg_buf);
        return 1;
    }

    /* 2) 读载体 */
    BmpImage *img = bmp_load(in_path);
    if (!img) { free(msg_buf); return 1; }

    if (!quiet) printf("[embed] 载体 %s  %dx%d\n", in_path, img->width, img->height);

    /* 3) 嵌入：DCT（默认，抗压缩）或 LSB（脆弱，用于演示篡改） */
    if (method == 0) {
        LsbStatus st = lsb_embed(img, msg, msg_len, quiet ? 0 : 1);
        if (st != LSB_OK) {
            fprintf(stderr, "[err] 嵌入失败: %s\n", lsb_status_str(st));
            fprintf(stderr, "[err] LSB 容量 %zu 字节，消息 %zu 字节（含 10 字节头需 %zu 字节）\n",
                    lsb_capacity_bytes(img), msg_len, msg_len + LSB_HEADER_SIZE);
            bmp_free(img);
            free(msg_buf);
            return 1;
        }
    } else if (method == 2) {
        Wm3Status st = wm3_embed(img, msg, msg_len, quiet ? 0 : 1);
        if (st != WM3_OK) {
            fprintf(stderr, "[err] 嵌入失败: %s\n", wm3_status_str(st));
            fprintf(stderr, "[err] Tile 模式单条上限 %zu 字节；这张图有 %d 份完整水印（瓦片数）\n",
                    wm3_max_payload(), wm3_tile_count(img));
            bmp_free(img);
            free(msg_buf);
            return 1;
        }
    } else {
        Wm2Status st = wm2_embed(img, msg, msg_len, quiet ? 0 : 1);
        if (st != WM2_OK) {
            fprintf(stderr, "[err] 嵌入失败: %s\n", wm2_status_str(st));
            fprintf(stderr, "[err] DCT 容量 %zu 字节（每块 1 bit、正文 3 份冗余），消息 %zu 字节\n",
                    wm2_capacity_bytes(img), msg_len);
            size_t need_side = (size_t)((msg_len * 8 * WM2_REP_PAYLOAD) +
                                        WM2_HEADER_BITS * WM2_REP_HEADER);
            fprintf(stderr, "[err] 这张图只有 %zu 个 8x8 块，本次需要 %zu 块；"
                            "按此消息至少要 %zu x %zu 的图\n",
                    (size_t)(img->width / 8) * (size_t)(img->height / 8), need_side,
                    (size_t)(need_side * 8 / (img->height / 8 > 0 ? img->height / 8 : 1) + 8),
                    (size_t)img->height);
            bmp_free(img);
            free(msg_buf);
            return 1;
        }
    }

    /* 4) 写文件 */
    if (bmp_save(img, out_path) != 0) {
        bmp_free(img);
        free(msg_buf);
        return 1;
    }

    if (!quiet)
        printf("[embed] 已生成: %s（%s，BMP 无损保存）\n", out_path,
               method == 0 ? "LSB 空域，转 JPG 即失效"
                           : (method == 2 ? "Tile 分块冗余，可抗裁剪" : "DCT 频域，可抗 JPEG 压缩"));

    bmp_free(img);
    free(msg_buf);
    return 0;
}

/* ==================== extract / verify 共用 ==================== */

/*
 * 依次尝试三种水印：Tile（抗裁剪）→ DCT（抗压缩）→ LSB（脆弱）。
 * 方法字节写在水印头里，不靠猜。
 * 返回 0=完整命中，1=命中但 CRC 失败，-1=都没找到。
 */
static int auto_extract(const BmpImage *img, uint8_t *buf, size_t cap, size_t *len,
                        int *method, float heat[WM3_CELLS * WM3_CELLS], int verbose)
{
    *len = 0;
    if (method) *method = -1;

    size_t tlen = 0;
    int dx = 0, dy = 0;
    Wm3Status s3 = wm3_extract(img, buf, cap, &tlen, &dx, &dy, heat, verbose);
    if (s3 == WM3_OK || s3 == WM3_ERR_CRC) {
        *len = tlen;
        if (method) *method = 2;
        if (verbose)
            printf("[extract] 命中方式: Tile 分块冗余（重同步偏移 %d,%d）\n", dx, dy);
        return (s3 == WM3_OK) ? 0 : 1;
    }

    uint16_t hc   = 0;
    size_t   wlen = 0;
    Wm2Status s2  = wm2_extract(img, buf, cap, &wlen, &hc, 0);
    if (s2 == WM2_OK || s2 == WM2_ERR_CRC) {
        *len = wlen;
        if (method) *method = 1;
        if (verbose) printf("[extract] 命中方式: DCT 频域（整幅顺序分配）\n");
        return (s2 == WM2_OK) ? 0 : 1;
    }

    size_t    llen = 0;
    size_t    lcap = lsb_capacity_bytes(img);
    LsbStatus s1   = lsb_extract(img, buf, (lcap + 1u < cap) ? lcap + 1u : cap, &llen, 0);
    if (s1 == LSB_OK) {
        *len = llen;
        if (method) *method = 0;
        if (verbose) printf("[extract] 命中方式: LSB 空域（脆弱）\n");
        return 0;
    }
    return -1;
}

/* 申请"够大"的缓冲：三种方法里最大的那个容量 */
static uint8_t *alloc_extract_buf(const BmpImage *img, size_t *cap)
{
    size_t c = wm2_capacity_bytes(img);
    size_t l = lsb_capacity_bytes(img);
    if (l > c) c = l;
    if (wm3_max_payload() > c) c = wm3_max_payload();
    c += 16u;
    uint8_t *buf = (uint8_t *)malloc(c);
    if (buf) *cap = c;
    return buf;
}

/* ==================== extract ==================== */

static int cmd_extract(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL;
    int quiet = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-i") && i + 1 < argc)      in_path  = argv[++i];
        else if (!strcmp(a, "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(a, "-q"))                 quiet    = 1;
        else { fprintf(stderr, "[err] 未知参数: %s\n", a); return 2; }
    }
    if (!in_path) { fprintf(stderr, "[err] extract 需要 -i\n"); return 2; }

    BmpImage *img = bmp_load(in_path);
    if (!img) return 1;

    size_t   cap = 0;
    uint8_t *buf = alloc_extract_buf(img, &cap);
    if (!buf) { fprintf(stderr, "[err] 内存不足\n"); bmp_free(img); return 1; }

    size_t len = 0;
    int    method = -1;
    int    rc = auto_extract(img, buf, cap, &len, &method, NULL, quiet ? 0 : 1);
    bmp_free(img);

    if (rc < 0) {
        fprintf(stderr, "[extract] 失败: 三种水印都没找到（Tile / DCT / LSB）\n");
        free(buf);
        return 1;
    }
    if (rc == 1)
        fprintf(stderr, "[extract] 警告: CRC 校验失败，取回的内容可能已被篡改\n");

    fwrite(buf, 1, len, stdout);
    fputc('\n', stdout);

    if (out_path) {
        FILE *fp = fopen(out_path, "wb");
        if (!fp) {
            fprintf(stderr, "[err] 无法写入: %s\n", out_path);
            free(buf);
            return 1;
        }
        size_t n = fwrite(buf, 1, len, fp);
        fclose(fp);
        if (n != len) {
            fprintf(stderr, "[err] 写文件不完整: %s\n", out_path);
            free(buf);
            return 1;
        }
        if (!quiet)
            printf("[extract] 已写出明文: %s（%lu 字节）\n", out_path, (unsigned long)len);
    }

    free(buf);
    return 0;
}
/* ==================== verify ==================== */

/*
 * 三态判定（M2 引入，M3 增加"篡改定位"）：
 *   DCT/Tile 的 CRC 通过 = 内容完整（哪怕被压缩/裁剪过）
 *   LSB 哨兵 16 份是否完好 = 像素有没有被重编码
 * 组合出：【真】/【可疑】/【假】/【无水印】
 */
static int cmd_verify(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL;
    int quiet = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-i") && i + 1 < argc)      in_path  = argv[++i];
        else if (!strcmp(a, "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(a, "-q"))                 quiet    = 1;
        else { fprintf(stderr, "[err] 未知参数: %s\n", a); return 2; }
    }
    if (!in_path) { fprintf(stderr, "[err] verify 需要 -i\n"); return 2; }

    BmpImage *img = bmp_load(in_path);
    if (!img) return 1;

    size_t   cap = 0;
    uint8_t *buf = alloc_extract_buf(img, &cap);
    if (!buf) { bmp_free(img); return 1; }

    float heat[WM3_CELLS * WM3_CELLS];
    for (int i = 0; i < WM3_CELLS * WM3_CELLS; i++) heat[i] = 0.0f;

    size_t len = 0;
    int    method = -1;
    int    rc = auto_extract(img, buf, cap, &len, &method, heat, quiet ? 0 : 1);

    int      intact = 0;
    uint16_t scap   = 0;
    (void)wm2_sentinel_scan(img, &intact, &scap);

    int bw = img->width, bh = img->height;
    bmp_free(img);

    static const char *kMethodName[] = { "LSB（空域）", "DCT（频域）", "Tile（分块冗余）" };

    printf("\n文件: %s\n", in_path);
    if (rc < 0) {
        printf("水印     : 未找到（Tile / DCT / LSB 都没有）\n");
        printf("结论     : 【无水印】未找到可解析的水印\n");
        free(buf);
        return 4;
    }
    printf("水印     : 命中，方式 %s，内容 %lu 字节\n",
           (method >= 0 && method <= 2) ? kMethodName[method] : "未知",
           (unsigned long)len);
    printf("CRC      : %s\n", rc == 0 ? "通过（内容完整）" : "失败（内容被改过）");
    printf("LSB 哨兵 : %d/%d 份完好%s\n", intact, WM2_SENTINEL_COPIES,
           intact == WM2_SENTINEL_COPIES ? "（像素未被改动）" : "（有区域被动过）");

    /* M3：篡改定位（只有 Tile 模式能给出格子级结果） */
    if (method == 2 && (rc == 0 || rc == 1)) {
        int  worst = 0;
        for (int i = 1; i < WM3_CELLS * WM3_CELLS; i++)
            if (heat[i] > heat[worst]) worst = i;
        printf("篡改定位 : 最高反水格 第 %d 行第 %d 列，%d%%\n",
               worst / WM3_CELLS + 1, worst % WM3_CELLS + 1,
               (int)(heat[worst] * 100.0f));
        if (heat[worst] > 0.15f) {
            printf("           该格有超过 15%% 的块与多数派相反 → 很可能被局部编辑过\n");
        } else {
            printf("           各格反水率都低，看不出局部编辑\n");
        }
    }

    int code;
    if (rc == 0 && intact == WM2_SENTINEL_COPIES) {
        printf("结论     : 【真】内容完整，且像素未被改动\n");
        code = 0;
    } else if (rc == 0) {
        printf("结论     : 【可疑】内容可读且 CRC 通过，但像素被改动过\n");
        printf("           可能是重编码/压缩/裁剪/加滤镜，或局部编辑；内容本身仍可信\n");
        code = 2;
    } else {
        printf("结论     : 【假】CRC 校验失败，内容与嵌入时不一致（已被篡改）\n");
        code = 3;
    }

    printf("取回内容 : %lu 字节\n", (unsigned long)len);
    fwrite(buf, 1, len, stdout);
    fputc('\n', stdout);

    if (out_path) {
        FILE *fp = fopen(out_path, "wb");
        if (fp) { fwrite(buf, 1, len, fp); fclose(fp); }
    }

    free(buf);
    (void)bw; (void)bh;
    return code;
}

/* ==================== heatmap ==================== */

/* 把 4×4 网格里反水率 > 30% 的格子用红框标出来，输出一张新图给人看 */
static int cmd_heatmap(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL;
    int quiet = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-i") && i + 1 < argc)      in_path  = argv[++i];
        else if (!strcmp(a, "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(a, "-q"))                 quiet    = 1;
        else { fprintf(stderr, "[err] 未知参数: %s\n", a); return 2; }
    }
    if (!in_path || !out_path) { fprintf(stderr, "[err] heatmap 需要 -i 与 -o\n"); return 2; }

    BmpImage *img = bmp_load(in_path);
    if (!img) return 1;

    size_t   cap = 0;
    uint8_t *buf = alloc_extract_buf(img, &cap);
    if (!buf) { bmp_free(img); return 1; }

    float heat[WM3_CELLS * WM3_CELLS];
    size_t len = 0;
    int method = -1;
    int rc = auto_extract(img, buf, cap, &len, &method, heat, 0);
    free(buf);

    if (rc < 0 || method != 2) {
        fprintf(stderr, "[heatmap] 这张图不是 Tile 模式水印，无法定位"
                        "（先 wm.exe embed -m tile）\n");
        bmp_free(img);
        return 1;
    }

    int bw = img->width / 8, bh = img->height / 8;
    int gx = bw / WM3_CELLS > 0 ? bw / WM3_CELLS : 1;
    int gy = bh / WM3_CELLS > 0 ? bh / WM3_CELLS : 1;
    int marked = 0;

    for (int cy = 0; cy < WM3_CELLS; cy++) {
        for (int cx = 0; cx < WM3_CELLS; cx++) {
            float h = heat[cy * WM3_CELLS + cx];
            if (h <= 0.15f) continue;
            marked++;

            int x0 = cx * gx * 8, y0 = cy * gy * 8;
            int x1 = x0 + gx * 8, y1 = y0 + gy * 8;
            if (x1 > img->width)  x1 = img->width;
            if (y1 > img->height) y1 = img->height;

            /* 画 3 像素宽的红框 */
            for (int t = 0; t < 3; t++) {
                for (int x = x0; x < x1; x++) {
                    uint8_t *p1 = bmp_pixel(img, x, y0 + t);
                    uint8_t *p2 = bmp_pixel(img, x, y1 - 1 - t);
                    if (p1) { p1[0] = 0; p1[1] = 0; p1[2] = 255; }
                    if (p2) { p2[0] = 0; p2[1] = 0; p2[2] = 255; }
                }
                for (int y = y0; y < y1; y++) {
                    uint8_t *p1 = bmp_pixel(img, x0 + t, y);
                    uint8_t *p2 = bmp_pixel(img, x1 - 1 - t, y);
                    if (p1) { p1[0] = 0; p1[1] = 0; p1[2] = 255; }
                    if (p2) { p2[0] = 0; p2[1] = 0; p2[2] = 255; }
                }
            }
            if (!quiet)
                printf("[heatmap] 标记 第 %d 行第 %d 列（反水率 %d%%）\n",
                       cy + 1, cx + 1, (int)(h * 100.0f));
        }
    }

    if (bmp_save(img, out_path) != 0) { bmp_free(img); return 1; }
    printf("[heatmap] 已输出: %s（标记 %d 个格子）\n", out_path, marked);
    bmp_free(img);
    return 0;
}
/* ==================== info ==================== */

static int cmd_info(int argc, char **argv)
{
    const char *in_path = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) in_path = argv[++i];
        else { fprintf(stderr, "[err] 未知参数: %s\n", argv[i]); return 2; }
    }
    if (!in_path) { fprintf(stderr, "[err] info 需要 -i\n"); return 2; }

    BmpImage *img = bmp_load(in_path);
    if (!img) return 1;

    size_t cap = lsb_capacity_bytes(img);
    printf("文件      : %s\n", in_path);
    printf("尺寸      : %dx%d（%d 万像素）\n", img->width, img->height,
           (img->width * img->height) / 10000);
    printf("格式      : 24 位 BMP，行跨度 %zu 字节\n", img->stride);
    printf("像素缓冲  : %zu 字节\n", img->stride * (size_t)img->height);
    printf("LSB 容量  : %zu 字节（每像素 3 bit，脆弱，用作篡改哨兵）\n", cap);
    printf("DCT 容量  : %zu 字节（8x8 块每块 1 bit，正文 3 份冗余，抗 JPEG）\n",
           wm2_capacity_bytes(img));

    uint32_t  mlen = 0;
    LsbStatus st   = lsb_peek(img, &mlen);
    if (st == LSB_OK) {
        printf("水印      : 检测到 LSB 水印，消息 %lu 字节，占容量 %.4f%%\n",
               (unsigned long)mlen, (double)(mlen + LSB_HEADER_SIZE) * 100.0 / (double)cap);
    } else {
        printf("水印      : 未检测到（%s）\n",
               st == LSB_ERR_NOT_FOUND ? "魔数不匹配" : "头部损坏或长度非法");
    }

    bmp_free(img);
    return 0;
}

/* ==================== diff ==================== */

static int cmd_diff(int argc, char **argv)
{
    const char *a_path = NULL, *b_path = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc)      a_path = argv[++i];
        else if (!strcmp(argv[i], "-j") && i + 1 < argc) b_path = argv[++i];
        else { fprintf(stderr, "[err] 未知参数: %s\n", argv[i]); return 2; }
    }
    if (!a_path || !b_path) { fprintf(stderr, "[err] diff 需要 -i 与 -j\n"); return 2; }

    BmpImage *a = bmp_load(a_path);
    BmpImage *b = bmp_load(b_path);
    if (!a || !b) { bmp_free(a); bmp_free(b); return 1; }

    if (a->width != b->width || a->height != b->height) {
        fprintf(stderr, "[err] 尺寸不一致: %dx%d vs %dx%d\n",
                a->width, a->height, b->width, b->height);
        bmp_free(a); bmp_free(b);
        return 1;
    }

    int    maxd    = 0;
    size_t changed = 0;
    double sse     = 0.0;
    analyze_diff(a, b, &maxd, &changed, &sse);

    size_t total = (size_t)a->width * (size_t)a->height * 3u;
    double mse   = total ? sse / (double)total : 0.0;
    double psnr  = (mse > 0.0) ? (10.0 * log10(255.0 * 255.0 / mse)) : 999.0;
    if (psnr > 99.0) psnr = 99.0;

    printf("最大通道差 : %d  (0=完全相同, 1=只有最低位变了)\n", maxd);
    printf("PSNR      : %.2f dB  （>40 dB 很安全，>38 dB 通常肉眼不可见；越高越接近原图）\n", psnr);
    printf("改动通道数 : %zu / %zu (%.4f%%)\n", changed, total,
           total ? (double)changed * 100.0 / (double)total : 0.0);
    printf("结论       : %s\n",
           (maxd <= 1) ? "仅最低位变化，视觉不可见（符合 LSB 预期）"
                       : "存在大于 1 的像素改动，可能不只做了 LSB 嵌入");

    bmp_free(a);
    bmp_free(b);
    return 0;
}

/* ==================== tamper ==================== */

/*
 * 模拟篡改：把指定矩形涂成指定颜色（默认纯白）。
 * 用途：答辩现场演示「图被改过 → 取不回原文」，无需依赖任何图像编辑软件。
 * 坐标 (x,y) 以图像左上角为原点，与 BMP 文件里的自下而上存储无关（这里走 bmp_pixel）。
 */
static int cmd_tamper(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL, *rect = NULL;
    int cb = 255, cg = 255, cr = 255;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-i") && i + 1 < argc)      in_path  = argv[++i];
        else if (!strcmp(a, "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(a, "-r") && i + 1 < argc) rect     = argv[++i];
        else if (!strcmp(a, "-c") && i + 1 < argc) {
            int b = 255, g = 255, r = 255;
            if (sscanf(argv[++i], "%d,%d,%d", &b, &g, &r) != 3) {
                fprintf(stderr, "[err] -c 格式应为 B,G,R（例如 -c 0,0,255 表示红）\n");
                return 2;
            }
            cb = b; cg = g; cr = r;
        }
        else { fprintf(stderr, "[err] 未知参数: %s\n", a); return 2; }
    }

    int x = 0, y = 0, w = 0, h = 0;
    if (!in_path || !out_path || !rect) {
        fprintf(stderr, "[err] tamper 需要 -i -o -r x,y,w,h\n");
        return 2;
    }
    if (sscanf(rect, "%d,%d,%d,%d", &x, &y, &w, &h) != 4 || w <= 0 || h <= 0) {
        fprintf(stderr, "[err] -r 格式应为 x,y,w,h（例如 -r 100,100,60,60）\n");
        return 2;
    }

    BmpImage *img = bmp_load(in_path);
    if (!img) return 1;

    /* 颜色越界一律夹到 0..255，避免演示脚本手滑传错值 */
    cb = cb < 0 ? 0 : (cb > 255 ? 255 : cb);
    cg = cg < 0 ? 0 : (cg > 255 ? 255 : cg);
    cr = cr < 0 ? 0 : (cr > 255 ? 255 : cr);

    int touched = 0;
    for (int yy = y; yy < y + h && yy < img->height; yy++) {
        for (int xx = x; xx < x + w && xx < img->width; xx++) {
            uint8_t *p = bmp_pixel(img, xx, yy);
            if (!p) continue;
            p[0] = (uint8_t)cb;
            p[1] = (uint8_t)cg;
            p[2] = (uint8_t)cr;
            touched++;
        }
    }

    if (bmp_save(img, out_path) != 0) {
        bmp_free(img);
        return 1;
    }

    printf("[tamper] 已涂改 (%d,%d) 起 %dx%d 区域，共 %d 像素，颜色 BGR(%d,%d,%d)\n",
           x, y, w, h, touched, cb, cg, cr);
    printf("[tamper] 输出: %s\n", out_path);

    bmp_free(img);
    return 0;
}

/* ==================== selftest ==================== */

static int g_fail = 0;

static void check(int cond, const char *name)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail++;
}

static void fill_payload(uint8_t *buf, size_t n, unsigned seed)
{
    unsigned s = seed * 2654435761u + 1u;
    for (size_t i = 0; i < n; i++) {
        s      = s * 1103515245u + 12345u;         /* 简易 LCG，够用且可复现 */
        buf[i] = (uint8_t)((s >> 16) & 0xFFu);
    }
}

static void fill_test_image(BmpImage *img)
{
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            uint8_t *p = bmp_pixel(img, x, y);
            p[0] = (uint8_t)((x * 7 + y * 3) & 0xFF);   /* B */
            p[1] = (uint8_t)((y * 5) & 0xFF);           /* G */
            p[2] = (uint8_t)((x ^ y) & 0xFF);           /* R */
        }
    }
}

/* 按绝对通道号写值（通道号 → 行、列、通道的换算在这里，与 lsb.c 的映射保持一致） */
static void put_channel(BmpImage *img, size_t idx, uint8_t v)
{
    size_t per_row = (size_t)img->width * 3u;
    size_t y       = idx / per_row;
    size_t off     = idx % per_row;
    img->pixels[y * img->stride + off] = v;
}

/* 在 (x0,y0) 起 rw*rh 的矩形区域涂成纯白，模拟「贴图/涂改」式篡改 */
static void whiten_rect(BmpImage *img, int x0, int y0, int rw, int rh)
{
    for (int y = y0; y < y0 + rh && y < img->height; y++) {
        for (int x = x0; x < x0 + rw && x < img->width; x++) {
            uint8_t *p = bmp_pixel(img, x, y);
            if (!p) continue;
            p[0] = p[1] = p[2] = 255;
        }
    }
}

static int cmd_selftest(void)
{
    printf("=== M1 自检：LSB 隐写闭环 ===\n");

    /* ---- 用例 1：多种长度往返 ---- */
    {
        printf("[用例1] 不同消息长度的往返一致性\n");
        BmpImage *img = bmp_create(256, 256);
        if (!img) { printf("  [FAIL] 无法创建测试图\n"); g_fail++; return 1; }
        fill_test_image(img);

        const size_t lens[] = { 1, 5, 17, 64, 255, 1000 };
        for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
            size_t   len = lens[k];
            uint8_t *src = (uint8_t *)malloc(len);
            uint8_t *dst = (uint8_t *)malloc(len + 1);
            fill_payload(src, len, (unsigned)len);

            int ok = 0;
            if (src && dst && lsb_embed(img, src, len, 0) == LSB_OK) {
                size_t got = 0;
                if (lsb_extract(img, dst, len + 1, &got, 0) == LSB_OK &&
                    got == len && memcmp(src, dst, len) == 0) {
                    ok = 1;
                }
            }
            char name[96];
            snprintf(name, sizeof(name), "%4zu 字节消息往返一致", len);
            check(ok, name);

            free(src);
            free(dst);
        }
        bmp_free(img);
    }

    /* ---- 用例 2：容量边界 ---- */
    {
        printf("[用例2] 容量边界（刚好塞满 / 多 1 字节）\n");
        BmpImage *img = bmp_create(256, 256);
        size_t cap    = lsb_capacity_bytes(img);
        size_t maxlen = cap - LSB_HEADER_SIZE;

        uint8_t *src = (uint8_t *)malloc(maxlen);
        uint8_t *dst = (uint8_t *)malloc(maxlen + 1);
        fill_payload(src, maxlen, 99);

        int ok = 0;
        if (lsb_embed(img, src, maxlen, 0) == LSB_OK) {
            size_t got = 0;
            ok = (lsb_extract(img, dst, maxlen + 1, &got, 0) == LSB_OK) &&
                 got == maxlen && memcmp(src, dst, maxlen) == 0;
        }
        char name[128];
        snprintf(name, sizeof(name), "容量上界 %zu 字节可完整塞入并取回", maxlen);
        check(ok, name);

        check(lsb_embed(img, src, maxlen + 1, 0) == LSB_ERR_CAPACITY,
              "超出容量 1 字节时报 LSB_ERR_CAPACITY");

        free(src);
        free(dst);
        bmp_free(img);
    }

    /* ---- 用例 3：未水印图必须判「无水印」 ---- */
    {
        printf("[用例3] 无水印图像识别\n");
        BmpImage *blank = bmp_create(64, 64);
        uint32_t  mlen  = 0;
        check(lsb_peek(blank, &mlen) == LSB_ERR_NOT_FOUND, "空白图 lsb_peek 返回 NOT_FOUND");

        uint8_t buf[64];
        size_t  got = 0;
        check(lsb_extract(blank, buf, sizeof(buf), &got, 0) == LSB_ERR_NOT_FOUND,
              "空白图 lsb_extract 返回 NOT_FOUND");
        bmp_free(blank);
    }

    /* ---- 用例 4：视觉不可见（差分 <= 1） ---- */
    {
        printf("[用例4] 视觉不可见性\n");
        BmpImage *orig = bmp_create(512, 512);
        BmpImage *steg = bmp_create(512, 512);
        fill_test_image(orig);
        memcpy(steg->pixels, orig->pixels, orig->stride * (size_t)orig->height);

        const char *text = "AIGC-TRUST-WATERMARK";
        size_t      tlen = strlen(text);
        lsb_embed(steg, (const uint8_t *)text, tlen, 0);

        int    maxd    = 0;
        size_t changed = 0;
        analyze_diff(orig, steg, &maxd, &changed, NULL);

        check(maxd <= 1, "嵌入后最大通道差 <= 1（肉眼不可见）");
        check(changed > 0, "确实改动了像素（不是空操作）");
        printf("         统计: 最大差 %d，改动 %zu / %zu 通道\n", maxd, changed,
               (size_t)orig->width * orig->height * 3u);

        bmp_free(orig);
        bmp_free(steg);
    }

    /* ---- 用例 5：二次嵌入覆盖 ---- */
    {
        printf("[用例5] 重复嵌入（后写覆盖先写）\n");
        BmpImage *img = bmp_create(128, 128);
        fill_test_image(img);

        const char *t1 = "FIRST-PAYLOAD";
        const char *t2 = "SECOND";
        lsb_embed(img, (const uint8_t *)t1, strlen(t1), 0);
        lsb_embed(img, (const uint8_t *)t2, strlen(t2), 0);

        uint8_t out[64];
        size_t  got = 0;
        int ok = (lsb_extract(img, out, sizeof(out), &got, 0) == LSB_OK) &&
                 got == strlen(t2) && memcmp(out, t2, got) == 0;
        check(ok, "二次嵌入后取回的是最后一次的内容");
        bmp_free(img);
    }

    /* ---- 用例 6：篡改演示（暴露 LSB 的脆弱性，M2 的 CRC 来兜底） ---- */
    {
        printf("[用例6] 翻转 1 个水印比特\n");
        BmpImage *img = bmp_create(64, 64);
        fill_test_image(img);

        const char *text = "TRUSTED-CONTENT-2025";
        size_t      tlen = strlen(text);
        lsb_embed(img, (const uint8_t *)text, tlen, 0);

        /* 第一个消息比特所在通道（头部占前 80 个比特位置） */
        size_t  ch  = lsb_bit_channel_index(img, (size_t)LSB_HEADER_SIZE * 8u);
        size_t  per = (size_t)img->width * 3u;
        uint8_t cur = img->pixels[(ch / per) * img->stride + (ch % per)];
        put_channel(img, ch, (uint8_t)(cur ^ 0x01u)); /* 只翻最低位 */

        uint8_t   out[64];
        size_t    got = 0;
        LsbStatus st  = lsb_extract(img, out, sizeof(out), &got, 0);
        int changed = (st != LSB_OK) || (got != tlen) || (memcmp(out, text, tlen) != 0);
        check(changed, "翻转 1 bit 后取回内容与原文不同（M1 无法自证清白 → M2 上 CRC）");
        bmp_free(img);
    }

    /* ---- 用例 7：区域涂改（验证「铺开」是否真的覆盖全图） ---- */
    {
        printf("[用例7] 涂白画面中部一块区域\n");
        BmpImage *img = bmp_create(512, 512);
        fill_test_image(img);

        /* 300 字节消息：若顺序写入只占顶部 3 行，涂中部必然「安然无恙」 */
        uint8_t src[300];
        fill_payload(src, sizeof(src), 7);
        lsb_embed(img, src, sizeof(src), 0);

        int rw = 60, rh = 60;
        whiten_rect(img, 100, 100, rw, rh); /* 只涂 60x60 像素 */

        uint8_t   out[512];
        size_t    got = 0;
        LsbStatus st  = lsb_extract(img, out, sizeof(out), &got, 0);
        int changed = (st != LSB_OK) || (got != sizeof(src)) ||
                      (memcmp(out, src, sizeof(src)) != 0);

        size_t touched = (size_t)rw * rh * 3u;
        size_t total   = (size_t)512 * 512 * 3u;
        printf("         涂改覆盖 %zu / %zu 通道（%.2f%%）\n",
               touched, total, (double)touched * 100.0 / (double)total);
        check(changed, "局部涂改后无法原样取回（铺开映射生效）");
        bmp_free(img);
    }

    /* ---- 用例 8：DCT 往返 + CRC + 哨兵 ---- */
    {
        printf("[用例8] DCT 频域往返（M2 核心）\n");
        BmpImage *img = bmp_create(512, 512);
        fill_test_image(img);

        const char *text = "DCT-ROUNDTRIP 频域水印往返测试 ✓";
        size_t      tlen = strlen(text);

        check(wm2_capacity_bytes(img) >= tlen, "512x512 的 DCT 容量够放这条消息");
        check(wm2_embed(img, (const uint8_t *)text, tlen, 0) == WM2_OK, "DCT 嵌入成功");

        uint8_t  out[512];
        size_t   got = 0;
        uint16_t hc  = 0;
        Wm2Status st = wm2_extract(img, out, sizeof(out), &got, &hc, 0);
        check(st == WM2_OK && got == tlen && memcmp(out, text, tlen) == 0,
              "DCT 提取与原文逐字节一致且 CRC 通过");

        int intact = 0;
        (void)wm2_sentinel_check(img, hc, &intact);
        check(intact == WM2_SENTINEL_COPIES, "16/16 份哨兵完好且 CRC 与头部一致");
        bmp_free(img);
    }

    /* ---- 用例 9：动 1 个像素 → 内容仍可读，但哨兵崩（三态里的「可疑」）---- */
    {
        printf("[用例9] 动 1 个像素的最低位\n");
        BmpImage *img = bmp_create(256, 256);
        fill_test_image(img);

        const char *text = "SENTINEL";
        size_t      tlen = strlen(text);
        wm2_embed(img, (const uint8_t *)text, tlen, 0);

        uint8_t  out[64];
        size_t   got = 0;
        uint16_t hc  = 0;
        wm2_extract(img, out, sizeof(out), &got, &hc, 0);

        img->pixels[0] ^= 1u;                     /* 只翻一个最低位 */

        int intact = 0;
        (void)wm2_sentinel_check(img, hc, &intact);
        check(intact < WM2_SENTINEL_COPIES, "哨兵份数下降到 15/16 → 能报出「像素被动过」");
        bmp_free(img);
    }

    /* ---- 用例 10：大面积涂改 → CRC 失败（三态里的「假」）---- */
    {
        printf("[用例10] 涂掉 40%% 画面\n");
        BmpImage *img = bmp_create(512, 512);
        fill_test_image(img);

        uint8_t msg[60];
        fill_payload(msg, sizeof(msg), 3);
        wm2_embed(img, msg, sizeof(msg), 0);

        whiten_rect(img, 0, 0, 320, 240);

        uint8_t  out[128];
        size_t   got = 0;
        uint16_t hc  = 0;
        check(wm2_extract(img, out, sizeof(out), &got, &hc, 0) == WM2_ERR_CRC,
              "CRC 失败 → 判定「已被篡改」");
        bmp_free(img);
    }
    printf("=== 结果: %s（失败 %d 项）===\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* ==================== bench ==================== */

/* 三种方法在三种分辨率下的耗时与画质（进程内计时，排除进程启动开销） */
static int cmd_bench(void)
{
    static const struct { int w, h; } sizes[] = { { 512, 512 }, { 1024, 768 }, { 1920, 1080 } };
    const int NL = 20, ND = 10, NT = 5;      /* 各方法重复次数 */

    uint8_t msg_lsb[200];
    uint8_t msg_dct[100];   /* 100 字节，512x512 也放得下（DCT 容量 154） */
    uint8_t msg_tile[40];
    fill_payload(msg_lsb,  sizeof(msg_lsb),  11);
    fill_payload(msg_dct,  sizeof(msg_dct),  12);
    fill_payload(msg_tile, sizeof(msg_tile), 13);

    printf("=== 性能基准（进程内计时，-O2，本机 CPU）===\n");
    printf("%-11s | %-14s | %-14s | %-14s\n", "分辨率", "LSB", "DCT", "Tile");
    printf("------------|----------------|----------------|----------------\n");

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int W = sizes[s].w, H = sizes[s].h;
        BmpImage *img  = bmp_create(W, H);
        BmpImage *orig = bmp_create(W, H);
        if (!img || !orig) { bmp_free(img); bmp_free(orig); return 1; }
        fill_test_image(img);
        memcpy(orig->pixels, img->pixels, orig->stride * (size_t)H);

        char cells[3][64];

        /* LSB */
        clock_t t0 = clock();
        for (int i = 0; i < NL; i++) {
            memcpy(img->pixels, orig->pixels, orig->stride * (size_t)H);
            lsb_embed(img, msg_lsb, sizeof(msg_lsb), 0);
        }
        double ms_lsb = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / NL;
        double lg = 0, cnt = 0;
        {
            int maxd = 0; size_t changed = 0; double sse = 0;
            analyze_diff(orig, img, &maxd, &changed, &sse);
            double mse = sse / ((double)W * H * 3.0);
            lg = (mse > 0) ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0;
            cnt = changed;
        }
        snprintf(cells[0], sizeof(cells[0]), "%.2f ms  PSNR %.1f", ms_lsb, lg);
        (void)cnt;

        /* DCT */
        memcpy(img->pixels, orig->pixels, orig->stride * (size_t)H);
        t0 = clock();
        for (int i = 0; i < ND; i++) {
            BmpImage *tmp = bmp_create(W, H);
            memcpy(tmp->pixels, orig->pixels, orig->stride * (size_t)H);
            wm2_embed(tmp, msg_dct, sizeof(msg_dct), 0);
            if (i == ND - 1) {
                memcpy(img->pixels, tmp->pixels, tmp->stride * (size_t)H);
            }
            bmp_free(tmp);
        }
        double ms_dct = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / ND;
        {
            int maxd = 0; size_t changed = 0; double sse = 0;
            analyze_diff(orig, img, &maxd, &changed, &sse);
            double mse = sse / ((double)W * H * 3.0);
            lg = (mse > 0) ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0;
        }
        snprintf(cells[1], sizeof(cells[1]), "%.2f ms  PSNR %.1f", ms_dct, lg);

        /* Tile */
        memcpy(img->pixels, orig->pixels, orig->stride * (size_t)H);
        t0 = clock();
        int tile_ok = 1;
        for (int i = 0; i < NT; i++) {
            BmpImage *tmp = bmp_create(W, H);
            memcpy(tmp->pixels, orig->pixels, orig->stride * (size_t)H);
            if (wm3_embed(tmp, msg_tile, sizeof(msg_tile), 0) != WM3_OK) tile_ok = 0;
            if (i == NT - 1) memcpy(img->pixels, tmp->pixels, tmp->stride * (size_t)H);
            bmp_free(tmp);
        }
        double ms_tile = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / NT;
        {
            int maxd = 0; size_t changed = 0; double sse = 0;
            analyze_diff(orig, img, &maxd, &changed, &sse);
            double mse = sse / ((double)W * H * 3.0);
            lg = (mse > 0) ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0;
        }
        if (tile_ok)
            snprintf(cells[2], sizeof(cells[2]), "%.2f ms  PSNR %.1f", ms_tile, lg);
        else
            snprintf(cells[2], sizeof(cells[2]), "图太小");

        printf("%-11s | %-14s | %-14s | %-14s\n",
               (s == 0) ? "512x512" : (s == 1) ? "1024x768" : "1920x1080",
               cells[0], cells[1], cells[2]);

        bmp_free(img);
        bmp_free(orig);
    }

    printf("\n说明：LSB 20 次 / DCT 10 次 / Tile 5 次取平均；PSNR 越高越接近原图（>38 dB 肉眼不可见）。\n");
    printf("Tile 模式单条消息上限 %zu 字节，但抗裁剪（裁掉 75%% 仍可恢复）。\n", wm3_max_payload());
    return 0;
}
/* ==================== 入口 ==================== */

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "embed"))    return cmd_embed(argc, argv);
    if (!strcmp(cmd, "extract"))  return cmd_extract(argc, argv);
    if (!strcmp(cmd, "verify"))   return cmd_verify(argc, argv);
    if (!strcmp(cmd, "heatmap"))  return cmd_heatmap(argc, argv);
    if (!strcmp(cmd, "info"))     return cmd_info(argc, argv);
    if (!strcmp(cmd, "diff"))     return cmd_diff(argc, argv);
    if (!strcmp(cmd, "tamper"))   return cmd_tamper(argc, argv);
    if (!strcmp(cmd, "selftest")) return cmd_selftest();
    if (!strcmp(cmd, "bench"))    return cmd_bench();
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage();
        return 0;
    }

    fprintf(stderr, "[err] 未知命令: %s\n", cmd);
    usage();
    return 2;
}
