/*
 * wm.h —— 公开 API（M4 交付物）
 *
 * 设计目标：让别人**不用读你的实现**也能用。
 *   - 只暴露 5 个函数，参数只用 unsigned char* / size_t / char*，不泄任何内部结构体；
 *   - 内存接口（推荐，适合嵌到别的程序）+ 文件接口（适合脚本/命令行）；
 *   - 三种方法（LSB / DCT / Tile）由 WmMethod 选择，提取时自动识别；
 *   - 可选口令：给了口令就用 HMAC-SHA256 生成密钥标签，没有口令伪造不出来。
 *
 * 典型用法（三行）：
 *     WmEmbedOptions opt = { WM_METHOD_TILE, "my-key", 6 };
 *     unsigned char *out; size_t out_len;
 *     wm_embed_bmp(bmp, bmp_len, (const unsigned char*)"hello", 5, &opt, &out, &out_len);
 */
#ifndef WM_H
#define WM_H

#include <stddef.h>

#define WM_VERSION_STRING "1.0.0-m4"

/* 嵌入方法 */
typedef enum {
    WM_METHOD_AUTO = -1,   /* 只用于"提取/验证"：依次尝试三种 */
    WM_METHOD_LSB  = 0,    /* 空域，容量最大，怕压缩 */
    WM_METHOD_DCT  = 1,    /* 频域，抗 JPEG，容量中等 */
    WM_METHOD_TILE = 2     /* 分块冗余，抗裁剪，容量最小 */
} WmMethod;

/* 统一返回码（负数都是错误） */
typedef enum {
    WM_OK               = 0,
    WM_ERR_ARG          = -1,   /* 参数非法 */
    WM_ERR_IO           = -2,   /* 文件读写失败 */
    WM_ERR_FORMAT       = -3,   /* 不是 24 位 BMP / 头部损坏 */
    WM_ERR_CAPACITY     = -4,   /* 载体容量不足 */
    WM_ERR_NO_WATERMARK = -5,   /* 三种方法都没找到水印 */
    WM_ERR_TAMPERED     = -6,   /* 校验失败：内容与嵌入时不一致 */
    WM_ERR_NEED_KEY     = -7,   /* 图里是带密钥的水印，但调用方没给口令 */
    WM_ERR_BAD_KEY      = -8    /* 口令不对：HMAC 标签验证失败 */
} WmStatus;

/* 验证结果（供调用方判断"真/假/可疑"） */
typedef struct {
    WmMethod method;        /* 实际命中的方法 */
    int      keyed;         /* 1 = 水印带密钥标签 */
    int      authenticated; /* 1 = 密钥标签验证通过（没有密钥时恒为 0） */
    int      crc_ok;        /* 1 = 内容校验通过 */
    size_t   msg_len;       /* 取回的消息长度 */
    char     method_name[16];
} WmVerifyInfo;

/* 嵌入选项 */
typedef struct {
    WmMethod    method;     /* WM_METHOD_LSB / DCT / TILE，默认 TILE */
    const char *key;        /* 口令，可为 NULL（不带密钥，只能防损坏） */
    size_t      key_len;    /* 口令长度（字节） */
} WmEmbedOptions;

/* ---------------- 内存接口 ---------------- */

/* 成功时 *bmp_out 由库内 malloc，调用方负责 wm_free() */
WmStatus wm_embed_bmp(const unsigned char *bmp_in, size_t bmp_len,
                      const unsigned char *msg, size_t msg_len,
                      const WmEmbedOptions *opt,
                      unsigned char **bmp_out, size_t *bmp_out_len);

/* 成功时 *msg_out 是 NUL 结尾的 UTF-8 字节，调用方负责 wm_free() */
WmStatus wm_extract_bmp(const unsigned char *bmp_in, size_t bmp_len,
                        const char *key, size_t key_len,
                        char **msg_out, size_t *msg_len,
                        WmVerifyInfo *info);

WmStatus wm_verify_bmp(const unsigned char *bmp_in, size_t bmp_len,
                       const char *key, size_t key_len,
                       WmVerifyInfo *info);

/* ---------------- 文件接口 ---------------- */

WmStatus wm_embed_file(const char *in_path, const char *out_path,
                       const char *msg_path, const WmEmbedOptions *opt);

WmStatus wm_verify_file(const char *path, const char *key, WmVerifyInfo *info);

/* 提取并输出明文（*msg_out 由库内 malloc，调用方 wm_free） */
WmStatus wm_extract_file(const char *path, const char *key,
                         char **msg_out, size_t *msg_len, WmVerifyInfo *info);

/* ---------------- 杂项 ---------------- */

void        wm_free(void *p);
const char *wm_status_str(WmStatus st);
const char *wm_method_name(WmMethod m);
const char *wm_version(void);

#endif /* WM_H */
