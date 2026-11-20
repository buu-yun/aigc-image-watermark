/*
 * dct.c —— 8×8 DCT 正/逆变换
 *
 * 关键点：
 *   1) 基函数矩阵 M[u][x] = 0.5·C(u)·cos((2x+1)uπ/16)，
 *      于是正变换 F = M · f · Mᵀ，逆变换 f = Mᵀ · F · M —— 逆变换只是把转置换了个位置，
 *      不需要真的"求逆矩阵"，这也是 DCT 好实现的原因。
 *   2) 基函数表只算一次（懒初始化），之后每次变换都是纯乘加。
 *   3) 输入先减 128 做中心化：DC 系数就代表"这块的平均亮度偏差"，不会出现几百上千的大数。
 */
#include "dct.h"

#include <math.h>

#define N 8

static double g_m[N][N];   /* 基函数矩阵 M[u][x] */
static int    g_ready = 0;

static void build_basis(void)
{
    for (int u = 0; u < N; u++) {
        double cu = (u == 0) ? (1.0 / sqrt(2.0)) : 1.0;
        for (int x = 0; x < N; x++) {
            g_m[u][x] = 0.5 * cu * cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0);
        }
    }
    g_ready = 1;
}

/* 一维正向：F = M · f */
static void fdct_1d(const double in[N], double out[N])
{
    for (int u = 0; u < N; u++) {
        double s = 0.0;
        for (int x = 0; x < N; x++) s += g_m[u][x] * in[x];
        out[u] = s;
    }
}

/* 一维逆向：f = Mᵀ · F */
static void idct_1d(const double in[N], double out[N])
{
    for (int x = 0; x < N; x++) {
        double s = 0.0;
        for (int u = 0; u < N; u++) s += g_m[u][x] * in[u];
        out[x] = s;
    }
}

void dct8x8(const double in[64], double out[64])
{
    if (!g_ready) build_basis();

    double tmp[64];

    /* ① 逐行变换 */
    for (int y = 0; y < N; y++) {
        double row_in[N], row_out[N];
        for (int x = 0; x < N; x++) row_in[x] = in[y * N + x] - 128.0;  /* 中心化 */
        fdct_1d(row_in, row_out);
        for (int u = 0; u < N; u++) tmp[y * N + u] = row_out[u];
    }

    /* ② 逐列变换（转置后再走一次一维） */
    for (int u = 0; u < N; u++) {
        double col_in[N], col_out[N];
        for (int y = 0; y < N; y++) col_in[y] = tmp[y * N + u];
        fdct_1d(col_in, col_out);
        for (int v = 0; v < N; v++) out[v * N + u] = col_out[v];
    }
}

void idct8x8(const double in[64], double out[64])
{
    if (!g_ready) build_basis();

    double tmp[64];

    /* ① 逐列逆变换 */
    for (int u = 0; u < N; u++) {
        double col_in[N], col_out[N];
        for (int v = 0; v < N; v++) col_in[v] = in[v * N + u];
        idct_1d(col_in, col_out);
        for (int y = 0; y < N; y++) tmp[y * N + u] = col_out[y];
    }

    /* ② 逐行逆变换，并 +128 还原 */
    for (int y = 0; y < N; y++) {
        double row_in[N], row_out[N];
        for (int u = 0; u < N; u++) row_in[u] = tmp[y * N + u];
        idct_1d(row_in, row_out);
        for (int x = 0; x < N; x++) out[y * N + x] = row_out[x] + 128.0;
    }
}
