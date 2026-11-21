/*
 * crypto.h —— SHA-256 + HMAC-SHA256（纯 C99 手写，不依赖 OpenSSL）
 *
 * 为什么 M4 必须加这一层：
 *   CRC 是**公开算法、无密钥**。攻击者改完内容，自己重算一个 CRC 填回去，
 *   校验照样通过 —— 那叫"防损坏"，不叫"防伪造"。
 *   HMAC 把密钥混进哈希过程里：没有密钥就算不出标签，伪造不了。
 *
 * 为什么自己写而不引第三方库：
 *   项目约定"只用标准库"；SHA-256 只有六十来行，还能在答辩里逐行讲。
 */
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_BLOCK  64
#define SHA256_DIGEST 32

typedef struct {
    uint32_t h[8];
    uint64_t bits;              /* 已处理的总位数 */
    uint8_t  buf[SHA256_BLOCK];
    size_t   buf_len;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t len);
void sha256_final(Sha256 *s, uint8_t out[SHA256_DIGEST]);
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST]);

/* HMAC-SHA256：out 是完整的 32 字节 */
void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t out[SHA256_DIGEST]);

/*
 * 取 4 字节标签：HMAC 的前 4 字节。
 * 32 位意味着随机伪造成功率约 1/43 亿；比赛够用，生产环境应保留 128 位。
 */
void hmac_tag4(const void *key, size_t key_len,
               const void *data, size_t data_len,
               uint8_t out[4]);

#endif /* CRYPTO_H */
