/*
 * dct.h —— 8×8 二维 DCT 正变换 / 逆变换（纯整数输入，浮点中间量）
 *
 * 公式（标准 DCT-II）：
 *   F(u,v) = (1/4)·C(u)·C(v)·ΣxΣy f(x,y)·cos((2x+1)uπ/16)·cos((2y+1)vπ/16)
 *   C(0) = 1/√2，其余 C(k) = 1
 *
 * 实现方式：拆成两次一维变换（先按行、再按列），把 8×8=64 次乘加降成 2×8 组一维变换，
 * 并且把基函数预先算成一张 8×8 矩阵 M，正变换就是 F = M·f·Mᵀ，逆变换是 f = Mᵀ·F·M。
 */
#ifndef DCT_H
#define DCT_H

/* 输入：64 个像素（行优先，已减去 128 做中心化）；输出：64 个系数，out[0] 是直流 DC */
void dct8x8(const double in[64], double out[64]);

/* 逆变换：输入 64 个系数，输出 64 个像素值（含 +128 还原） */
void idct8x8(const double in[64], double out[64]);

#endif /* DCT_H */
