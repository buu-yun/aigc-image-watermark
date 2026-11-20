/*
 * bmp.h —— 24 位 BMP 读写（M1 手写实现，不依赖任何第三方库）
 *
 * 设计要点：
 *   1) 内存中的像素统一为「自上而下、BGR、每行 stride 字节」布局，
 *      与 BMP 文件的「自下而上」解耦，后续算法只需关心 (x,y) 坐标。
 *   2) stride 已按 BMP 规范做 4 字节对齐，写文件时可整行直出。
 *   3) 只支持 24 位、BI_RGB(无压缩) —— 这是隐写最常用的载体格式。
 */
#ifndef BMP_H
#define BMP_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      width;    /* 宽（像素） */
    int      height;   /* 高（像素，恒为正数；文件里的负高度已归一化） */
    size_t   stride;   /* 每行字节数 = 4 字节对齐后的 width*3 */
    uint8_t *pixels;   /* 像素缓冲，长度 = stride * height，自上而下 */
} BmpImage;

/* 申请一张全黑的 24 位图像；失败返回 NULL */
BmpImage *bmp_create(int width, int height);
void      bmp_free(BmpImage *img);

/* 读/写 24 位未压缩 BMP；失败返回 NULL / -1，并在 stderr 打印原因 */
BmpImage *bmp_load(const char *path);
int       bmp_save(const BmpImage *img, const char *path);

/*
 * 内存版读写：Web 服务里图片是从 HTTP body 来的，不落盘。
 * bmp_save_mem 成功后 *out 由调用者 free()，*out_len 是字节数。
 */
BmpImage *bmp_load_mem(const uint8_t *data, size_t len);
int       bmp_save_mem(const BmpImage *img, uint8_t **out, size_t *out_len);

/* 取像素指针，返回 3 字节 B,G,R；越界返回 NULL */
uint8_t *bmp_pixel(const BmpImage *img, int x, int y);

/* 比较两张图，返回最大通道差值(0..255)；尺寸不同返回 -1 */
int bmp_diff_max(const BmpImage *a, const BmpImage *b);

#endif /* BMP_H */
