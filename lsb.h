/*
 * lsb.h —— LSB（最低有效位）隐写：把字节流藏进像素最低位
 *
 * M1 的定位：先打通「能藏能取」的最小闭环，把 BMP 读写的坑踩完，
 * 为 M2 的 DCT + CRC 提供同一套 CLI / 校验框架。
 *
 * 载体格式（大端比特序，先写高位再写低位）：
 *   [magic 'A''I''G''C'] [版本 1B] [方法 1B] [消息长度 4B 小端] [消息体 len B]
 *   = 10 字节头 + len 字节消息
 *
 * 写入位置（重点）：不是顺序写，而是「跳着铺开」——
 *   第 k 个比特落在通道号 (k * step) % total 上，
 *   step 取与通道总数互质的常数（从 7919 起，不互质就 +1），
 *   于是 k 遍历 0..total-1 时恰好走遍所有通道，水印均匀铺满整幅图。
 *   顺序写的致命问题：324 字节消息只占前 2672 个通道 ≈ 顶部 2 行像素，
 *   涂掉画面中部 11% 都伤不到水印，抗篡改就成了摆设。
 * 每个通道只改 1 个最低位，肉眼不可见（差分恒为 0 或 1）。
 */
#ifndef LSB_H
#define LSB_H

#include <stddef.h>
#include <stdint.h>

#include "bmp.h"

#define LSB_MAGIC       "AIGC"
#define LSB_MAGIC_LEN   4
#define LSB_VERSION     1
#define LSB_METHOD_LSB  0          /* 方法字节：0=LSB，M2 会加 1=DCT */
#define LSB_HEADER_SIZE 10         /* 4 + 1 + 1 + 4 */
#define LSB_STRIDE_BASE 7919u      /* 铺开步长的起始质数 */

typedef enum {
    LSB_OK            = 0,
    LSB_ERR_ARG       = -1,   /* 参数不合法（缓冲区太小 / 消息超长等） */
    LSB_ERR_CAPACITY  = -2,   /* 载体容量不足 */
    LSB_ERR_NOT_FOUND = -3,   /* 图里没有水印（魔数不匹配） */
    LSB_ERR_CORRUPT   = -4    /* 找到疑似水印但头部非法（长度越界 / 版本不符） */
} LsbStatus;

/* 可用容量（字节）。= 宽*高*3/8，其中 10 字节要被头部占用 */
size_t lsb_capacity_bytes(const BmpImage *img);

/* 把状态码翻译成中文说明，用于日志打印 */
const char *lsb_status_str(LsbStatus st);

/*
 * 嵌入：data 是原始字节（不要求是字符串），len 是字节数。
 * verbose>0 时向 stdout 打印容量占用情况。
 */
LsbStatus lsb_embed(BmpImage *img, const uint8_t *data, size_t len, int verbose);

/*
 * 提取：out 至少 out_cap 字节，函数会在 out[len] 处补 '\0'（所以 out_cap >= len+1）。
 * 成功时 *out_len = 实际消息长度。
 */
LsbStatus lsb_extract(const BmpImage *img, uint8_t *out, size_t out_cap,
                      size_t *out_len, int verbose);

/* 只读文件头，不取消息体：用于 info 命令判断「这张图有没有水印、多长」 */
LsbStatus lsb_peek(const BmpImage *img, uint32_t *msg_len);

/*
 * 第 k 个比特落在哪个绝对通道号（0 起，按 y*w*3 + x*3 + 通道 计）。
 * 供自检、篡改定位、以及 M3 的冗余/分块实验复用。
 */
size_t lsb_bit_channel_index(const BmpImage *img, size_t k);

#endif /* LSB_H */
