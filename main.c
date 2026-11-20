/*
 * main.c —— M1 命令行工具：embed / extract / info / diff / selftest / bench
 *
 * 为什么要有 selftest：验收不能只靠肉眼「看着像没坏」，
 * 这里用内置测试用例把「能藏能取」变成一条命令给出的 PASS/FAIL，
 * 答辩时可以现场跑，也方便 M2/M3 回归。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bmp.h"
#include "lsb.h"

/* ==================== 通用工具 ==================== */

static void usage(void)
{
    printf(
        "AI 图像可信水印引擎  M1：LSB 隐写最小闭环\n"
        "\n"
        "用法:\n"
        "  wm embed    -i <载体.bmp> -o <含水印.bmp> -f <消息.txt> [-q]\n"
        "  wm embed    -i <载体.bmp> -o <含水印.bmp> -t <文本>     [-q]\n"
        "  wm extract  -i <含水印.bmp> [-o <输出.txt>] [-q]\n"
        "  wm info     -i <图片.bmp>\n"
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
static void analyze_diff(const BmpImage *a, const BmpImage *b, int *maxd, size_t *changed)
{
    *maxd    = 0;
    *changed = 0;
    size_t row_bytes = (size_t)a->width * 3u;

    for (int y = 0; y < a->height; y++) {
        const uint8_t *ra = a->pixels + (size_t)y * a->stride;
        const uint8_t *rb = b->pixels + (size_t)y * b->stride;
        for (size_t i = 0; i < row_bytes; i++) {
            int d = (int)ra[i] - (int)rb[i];
            if (d != 0) (*changed)++;
            if (d < 0) d = -d;
            if (d > *maxd) *maxd = d;
        }
    }
}

/* ==================== embed ==================== */

static int cmd_embed(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL, *msg_file = NULL, *msg_text = NULL;
    int quiet = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-i") && i + 1 < argc)      in_path  = argv[++i];
        else if (!strcmp(a, "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(a, "-f") && i + 1 < argc) msg_file = argv[++i];
        else if (!strcmp(a, "-t") && i + 1 < argc) msg_text = argv[++i];
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

    /* 3) 嵌入 */
    LsbStatus st = lsb_embed(img, msg, msg_len, quiet ? 0 : 1);
    if (st != LSB_OK) {
        fprintf(stderr, "[err] 嵌入失败: %s\n", lsb_status_str(st));
        fprintf(stderr, "[err] 当前图片容量 %zu 字节，消息 %zu 字节（含 10 字节头需 %zu 字节）\n",
                lsb_capacity_bytes(img), msg_len, msg_len + LSB_HEADER_SIZE);
        bmp_free(img);
        free(msg_buf);
        return 1;
    }

    /* 4) 写文件 */
    if (bmp_save(img, out_path) != 0) {
        bmp_free(img);
        free(msg_buf);
        return 1;
    }

    if (!quiet) printf("[embed] 已生成: %s（BMP 无损保存，LSB 不会被重新编码破坏）\n", out_path);

    bmp_free(img);
    free(msg_buf);
    return 0;
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

    size_t   cap = lsb_capacity_bytes(img);
    uint8_t *buf = (uint8_t *)malloc(cap + 2u);
    if (!buf) {
        fprintf(stderr, "[err] 内存不足\n");
        bmp_free(img);
        return 1;
    }

    size_t    len = 0;
    LsbStatus st  = lsb_extract(img, buf, cap + 1u, &len, quiet ? 0 : 1);
    bmp_free(img);

    if (st != LSB_OK) {
        fprintf(stderr, "[extract] 失败: %s\n", lsb_status_str(st));
        free(buf);
        return 1;
    }

    /* 原样输出字节（不做编码转换，UTF-8 进来就 UTF-8 出去） */
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
    printf("LSB 容量  : %zu 字节（每像素 3 bit）\n", cap);

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
    analyze_diff(a, b, &maxd, &changed);

    size_t total = (size_t)a->width * (size_t)a->height * 3u;
    printf("最大通道差 : %d  (0=完全相同, 1=只有最低位变了)\n", maxd);
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
        analyze_diff(orig, steg, &maxd, &changed);

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

    printf("=== 结果: %s（失败 %d 项）===\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* ==================== bench ==================== */

/* 进程内计时，排除进程启动开销；M3 的性能表会在此基础上扩展 */
static int cmd_bench(void)
{
    const int W = 512, H = 512, N = 200;
    BmpImage *img = bmp_create(W, H);
    if (!img) return 1;
    fill_test_image(img);

    uint8_t msg[324];
    fill_payload(msg, sizeof(msg), 5);

    printf("=== 性能基准：%dx%d，消息 %zu 字节，各 %d 次 ===\n",
           W, H, sizeof(msg), N);

    clock_t t0 = clock();
    for (int i = 0; i < N; i++) {
        if (lsb_embed(img, msg, sizeof(msg), 0) != LSB_OK) {
            fprintf(stderr, "[bench] 嵌入失败\n");
            bmp_free(img);
            return 1;
        }
    }
    clock_t t1 = clock();

    uint8_t out[512];
    size_t  got = 0;
    for (int i = 0; i < N; i++) {
        if (lsb_extract(img, out, sizeof(out), &got, 0) != LSB_OK) {
            fprintf(stderr, "[bench] 提取失败\n");
            bmp_free(img);
            return 1;
        }
    }
    clock_t t2 = clock();

    double ms_embed   = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC / N;
    double ms_extract = (double)(t2 - t1) * 1000.0 / CLOCKS_PER_SEC / N;

    printf("embed   : %.4f ms/次  → 约 %ld 次/秒\n", ms_embed,
           (long)(ms_embed > 0 ? 1000.0 / ms_embed : 0));
    printf("extract : %.4f ms/次  → 约 %ld 次/秒\n", ms_extract,
           (long)(ms_extract > 0 ? 1000.0 / ms_extract : 0));
    printf("说明    : LSB 只访问水印用到的 %zu 个通道（不扫描全图），\n",
           (sizeof(msg) + LSB_HEADER_SIZE) * 8u);
    printf("          所以耗时与消息长度成正比，与图片大小几乎无关（同一张图=同一组通道）。\n");

    bmp_free(img);
    return 0;
}

/* ==================== 入口 ==================== */

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "embed"))    return cmd_embed(argc, argv);
    if (!strcmp(cmd, "extract"))  return cmd_extract(argc, argv);
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
