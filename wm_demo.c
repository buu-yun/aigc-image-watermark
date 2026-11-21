/*
 * wm_demo.c —— 只用"库"写出来的示例程序（M4 交付验收用）
 *
 * 它故意不 include 任何内部头文件，只 include "wm.h"：
 * 如果这个文件能编过，就证明"别人不用读实现也能用"。
 *
 * 用法：
 *   wm_demo embed  in.bmp out.bmp msg.txt [口令] [tile|dct|lsb]
 *   wm_demo extract in.bmp [口令]
 *   wm_demo verify  in.bmp [口令]
 *   wm_demo --crypto-test         :: 用 RFC 4231 / FIPS 180-4 的标准向量验证 SHA-256 与 HMAC
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "wm.h"

static void usage(void)
{
    printf("wm_demo %s —— AIGC 图像可信水印引擎 · 库示例\n\n", wm_version());
    printf("用法:\n");
    printf("  wm_demo embed   in.bmp out.bmp msg.txt [口令] [tile|dct|lsb]\n");
    printf("  wm_demo extract in.bmp [口令]\n");
    printf("  wm_demo verify  in.bmp [口令]\n");
    printf("  wm_demo --crypto-test\n");
}

static void print_info(const WmVerifyInfo *i)
{
    printf("  方法      : %s\n", i->method_name);
    printf("  带密钥    : %s\n", i->keyed ? "是" : "否（只能防损坏，不能防伪造）");
    if (i->keyed)
        printf("  密钥标签  : %s\n", i->authenticated ? "验证通过" : "验证失败");
    printf("  内容校验  : %s\n", i->crc_ok ? "通过" : "失败");
    printf("  消息长度  : %zu 字节\n", i->msg_len);
}

/* ---------------- 密码学自检（标准向量） ---------------- */

static int crypto_test(void)
{
    int fail = 0;

    /* FIPS 180-4：SHA-256("abc") */
    uint8_t d[32];
    sha256("abc", 3, d);
    const uint8_t want1[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    int ok1 = memcmp(d, want1, 32) == 0;
    printf("  [%s] SHA-256(\"abc\")\n", ok1 ? "PASS" : "FAIL");
    if (!ok1) fail++;

    /* RFC 4231 第 1 组：key = 0x0b × 20，data = "Hi There" */
    uint8_t k1[20];
    memset(k1, 0x0b, sizeof(k1));
    const uint8_t want2[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7
    };
    hmac_sha256(k1, sizeof(k1), "Hi There", 8, d);
    int ok2 = memcmp(d, want2, 32) == 0;
    printf("  [%s] HMAC-SHA256(RFC 4231 #1)\n", ok2 ? "PASS" : "FAIL");
    if (!ok2) fail++;

    /* RFC 4231 第 2 组：key = "Jefe"，data = "what do ya want for nothing?" */
    const uint8_t want3[32] = {
        0x5b,0xdc,0xc1,0x46,0xbf,0x60,0x75,0x4e,0x6a,0x04,0x24,0x26,0x08,0x95,0x75,0xc7,
        0x5a,0x00,0x3f,0x08,0x9d,0x27,0x39,0x83,0x9d,0xec,0x58,0xb9,0x64,0xec,0x38,0x43
    };
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    int ok3 = memcmp(d, want3, 32) == 0;
    printf("  [%s] HMAC-SHA256(RFC 4231 #2)\n", ok3 ? "PASS" : "FAIL");
    if (!ok3) fail++;

    printf("\n结果: %d 项失败\n", fail);
    return fail == 0 ? 0 : 1;
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }

    if (!strcmp(argv[1], "--crypto-test")) return crypto_test();

    if (!strcmp(argv[1], "embed")) {
        if (argc < 5) { usage(); return 2; }

        WmEmbedOptions opt;
        opt.method  = WM_METHOD_TILE;
        opt.key     = NULL;
        opt.key_len = 0;

        char keybuf[128] = { 0 };          /* 用固定缓冲，避免 strdup（POSIX，非 C99） */
        if (argc > 5) {
            strncpy(keybuf, argv[5], sizeof(keybuf) - 1);
            opt.key = keybuf;
            opt.key_len = strlen(keybuf);
        }
        if (argc > 6) {
            opt.method = (!strcmp(argv[6], "lsb")) ? WM_METHOD_LSB
                       : (!strcmp(argv[6], "dct")) ? WM_METHOD_DCT
                                                   : WM_METHOD_TILE;
        }

        WmStatus st = wm_embed_file(argv[2], argv[3], argv[4], &opt);
        printf("[demo] embed: %s（方法 %s%s）\n", wm_status_str(st),
               wm_method_name(opt.method), opt.key ? "，带口令" : "");
        return st == WM_OK ? 0 : 1;
    }

    if (!strcmp(argv[1], "extract")) {
        if (argc < 3) { usage(); return 2; }
        const char *key = (argc > 3) ? argv[3] : NULL;

        char         *msg = NULL;
        size_t        mlen = 0;
        WmVerifyInfo  info;
        memset(&info, 0, sizeof(info));

        WmStatus st = wm_extract_file(argv[2], key, &msg, &mlen, &info);
        printf("[demo] extract: %s\n", wm_status_str(st));
        print_info(&info);
        if (msg) {
            printf("  内容      : %s\n", msg);
            wm_free(msg);
        }
        return (st == WM_OK) ? 0 : 1;
    }

    if (!strcmp(argv[1], "verify")) {
        if (argc < 3) { usage(); return 2; }
        const char *key = (argc > 3) ? argv[3] : NULL;

        WmVerifyInfo info;
        memset(&info, 0, sizeof(info));
        WmStatus st = wm_verify_file(argv[2], key, &info);

        printf("[demo] verify: %s\n", wm_status_str(st));
        print_info(&info);
        return (st == WM_OK) ? 0 : 1;
    }

    usage();
    return 2;
}
