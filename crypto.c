/*
 * crypto.c —— SHA-256（FIPS 180-4）+ HMAC（RFC 2104），纯 C99
 *
 * SHA-256 的骨架只有四步：
 *   ① 填充：末尾补 0x80，再补 0 到 56 mod 64，最后 8 字节写原始比特长度（大端）
 *   ② 分组：每 64 字节一组，展开成 64 个 32 位字 W[0..63]
 *   ③ 压缩：做 64 轮，每轮用不同的常数 K[i] 和轮换函数（Ch/Maj/Σ）
 *   ④ 输出：8 个 32 位状态量按大端拼成 32 字节摘要
 *
 * HMAC 的骨架也只有四步：K 补零到 64 字节 → 与 ipad 异或 → 哈希 → 与 opad 异或再哈希。
 */
#include "crypto.h"

#include <string.h>

/* ---------------- 常量表 ---------------- */

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

/* ---------------- 压缩函数 ---------------- */

static void compress(uint32_t h[8], const uint8_t blk[SHA256_BLOCK])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | (uint32_t)blk[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + BSIG1(e) + CH(e, f, g) + K256[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        hh = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ---------------- 公开接口 ---------------- */

void sha256_init(Sha256 *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(s->h, iv, sizeof(iv));
    s->bits = 0;
    s->buf_len = 0;
}

void sha256_update(Sha256 *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    s->bits += (uint64_t)len * 8u;

    while (len > 0) {
        size_t need = SHA256_BLOCK - s->buf_len;
        size_t take = (len < need) ? len : need;
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += take;
        p += take;
        len -= take;

        if (s->buf_len == SHA256_BLOCK) {
            compress(s->h, s->buf);
            s->buf_len = 0;
        }
    }
}

void sha256_final(Sha256 *s, uint8_t out[SHA256_DIGEST])
{
    uint64_t bits = s->bits;

    /* 填充：0x80 + 若干个 0，让 (buf_len + 1 + k) % 64 == 56 */
    uint8_t pad[SHA256_BLOCK + 8];
    size_t pad_len = 0;
    pad[pad_len++] = 0x80;
    size_t rem = (s->buf_len + 1) % SHA256_BLOCK;
    size_t zeros = (rem <= 56) ? (56 - rem) : (120 - rem);
    memset(pad + pad_len, 0, zeros);
    pad_len += zeros;

    for (size_t i = 0; i < 8; i++) {
        pad[pad_len + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    pad_len += 8;

    sha256_update(s, pad, pad_len);   /* 注意：这会改动 s->bits，所以前面先存了 */

    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST])
{
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}

void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t out[SHA256_DIGEST])
{
    uint8_t k[SHA256_BLOCK];
    memset(k, 0, sizeof(k));

    if (key_len > SHA256_BLOCK) {
        sha256(key, key_len, k);           /* 长密钥先哈希 */
    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }

    uint8_t ipad[SHA256_BLOCK], opad[SHA256_BLOCK];
    for (int i = 0; i < SHA256_BLOCK; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }

    uint8_t inner[SHA256_DIGEST];
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, ipad, sizeof(ipad));
    sha256_update(&s, data, data_len);
    sha256_final(&s, inner);

    sha256_init(&s);
    sha256_update(&s, opad, sizeof(opad));
    sha256_update(&s, inner, sizeof(inner));
    sha256_final(&s, out);
}

void hmac_tag4(const void *key, size_t key_len,
               const void *data, size_t data_len,
               uint8_t out[4])
{
    uint8_t full[SHA256_DIGEST];
    hmac_sha256(key, key_len, data, data_len, full);
    memcpy(out, full, 4);
}
