/*
 * wm_server.c —— 把 M1 水印引擎包成一个微型 Web 服务（纯 C，零第三方库）
 *
 * 为什么自己写 HTTP：比赛要求"只用标准库"，而 HTTP/1.1 的最小可用子集
 * （读请求头 → 读 Content-Length → 回一条响应 → 关连接）不到 100 行就能说清楚，
 * 放进答辩 PPT 也讲得明白；用 WinSock 是 Windows 系统自带，不算第三方库。
 *
 * 接口设计（刻意不用 multipart，避免边界字符串扫描这类易错代码）：
 *   GET  /              浏览器界面（读 web/index.html）
 *   GET  /api/health    健康检查，返回 ok
 *   POST /api/embed     body = [4 字节小端消息长度][UTF-8 消息][24 位 BMP]
 *                       返回：带水印的新 BMP（image/bmp），附 X-WM-* 头
 *   POST /api/extract   body = [24 位 BMP]
 *                       返回：text/plain，正文为消息；X-WM-Status 说明命中情况
 *
 * 编译： gcc -std=c99 -O2 -Wall -Wextra -o wm_server.exe wm_server.c bmp.c lsb.c -lws2_32
 * 运行： wm_server.exe   →  浏览器打开 http://localhost:8080
 *
 * 说明：单线程顺序处理（一次一个请求），足够答辩/队内演示；
 *       并发改造（线程池）留到 M4 交付阶段再做。
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bmp.h"
#include "lsb.h"

#define WM_PORT      8080
#define MAX_BODY     (64u * 1024u * 1024u)   /* 单请求最大 64MB */
#define MAX_HDR      16384                   /* 请求头最大 16KB */

/* ---------------- 小工具 ---------------- */

/* 把 4 字节读出小端 uint32（消息长度帧） */
static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* 忽略大小写的前缀比较（自己写，避免依赖 _strnicmp/strncasecmp 的平台差异） */
static int ci_prefix(const char *s, const char *prefix, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
        if (a == '\0') return 0;
    }
    return 1;
}

/* 从请求头里取 Content-Length；找不到返回 -1 */
static long get_content_length(const char *hdr)
{
    const char *p = hdr;
    for (;;) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) break;
        p = eol + 2;
        if (p[0] == '\r' && p[1] == '\n') break;          /* 头结束 */
        if (ci_prefix(p, "Content-Length:", 15)) return atol(p + 15);
    }
    return -1;
}

/* 读满 n 字节 */
static int recv_all(SOCKET s, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = recv(s, (char *)buf + got, (int)(n - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* 逐字节读到 \r\n\r\n，返回头部字节数；失败 -1。buf 需要 cap+1 字节 */
static int recv_headers(SOCKET s, char *buf, size_t cap)
{
    size_t n = 0;
    while (n < cap) {
        int r = recv(s, buf + n, 1, 0);
        if (r <= 0) return -1;
        n++;
        if (n >= 4 && buf[n - 4] == '\r' && buf[n - 3] == '\n' &&
                      buf[n - 2] == '\r' && buf[n - 1] == '\n') {
            buf[n] = '\0';
            return (int)n;
        }
    }
    return -1;
}

static void send_response(SOCKET s, int code, const char *reason, const char *ctype,
                          const uint8_t *body, size_t len, const char *extra_hdr)
{
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "%s"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, reason, ctype, len, extra_hdr ? extra_hdr : "");
    if (n > 0) send(s, hdr, n, 0);
    if (len > 0) {
        size_t sent = 0;
        while (sent < len) {
            int r = send(s, (const char *)body + sent, (int)(len - sent), 0);
            if (r <= 0) return;
            sent += (size_t)r;
        }
    }
}

/* 纯文本错误响应（UTF-8 中文） */
static void send_text(SOCKET s, int code, const char *reason, const char *msg)
{
    send_response(s, code, reason, "text/plain; charset=utf-8",
                  (const uint8_t *)msg, strlen(msg), NULL);
}

/* 读整个文件到内存（调用者 free）；成功 0 */
static int read_file(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return -1; }
    rewind(fp);

    uint8_t *buf = (uint8_t *)malloc((size_t)size + 1u);
    if (!buf) { fclose(fp); return -1; }
    size_t n = (size > 0) ? fread(buf, 1, (size_t)size, fp) : 0;
    fclose(fp);
    if (n != (size_t)size) { free(buf); return -1; }

    buf[size] = '\0';
    *out     = buf;
    *out_len = (size_t)size;
    return 0;
}

/* ---------------- 业务处理 ---------------- */

static void handle_embed(SOCKET c, const uint8_t *body, long clen)
{
    if (clen < 5) {
        send_text(c, 400, "Bad Request", "请求体太短：需要 [4 字节长度][消息][BMP]");
        return;
    }

    uint32_t tlen = rd_u32le(body);
    if (tlen == 0) {
        send_text(c, 400, "Bad Request", "消息为空，请先输入要藏的文字");
        return;
    }
    if ((long)(4 + tlen) >= clen) {
        send_text(c, 400, "Bad Request", "长度字段与请求体不一致（消息长度非法）");
        return;
    }

    const uint8_t *text = body + 4;
    const uint8_t *bmpd = body + 4 + tlen;
    size_t bmplen = (size_t)(clen - 4 - (long)tlen);

    BmpImage *img = bmp_load_mem(bmpd, bmplen);
    if (!img) {
        send_text(c, 400, "Bad Request",
                  "图片解析失败：需要 24 位未压缩 BMP（网页会自动把 JPG/PNG 转成 BMP）");
        return;
    }

    LsbStatus st = lsb_embed(img, text, tlen, 0);
    if (st != LSB_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "嵌入失败：%s（图片容量 %zu 字节，消息 %u 字节，至少需要 %u 字节，请换更大的图）",
                 lsb_status_str(st), lsb_capacity_bytes(img), (unsigned)tlen,
                 (unsigned)(tlen + LSB_HEADER_SIZE));
        bmp_free(img);
        send_text(c, 400, "Bad Request", msg);
        return;
    }

    uint8_t *out   = NULL;
    size_t   olen  = 0;
    if (bmp_save_mem(img, &out, &olen) != 0) {
        bmp_free(img);
        send_text(c, 500, "Internal Server Error", "生成输出图片失败");
        return;
    }

    char extra[256];
    snprintf(extra, sizeof(extra),
             "X-WM-MsgLen: %u\r\nX-WM-Capacity: %zu\r\nX-WM-Width: %d\r\nX-WM-Height: %d\r\n",
             (unsigned)tlen, lsb_capacity_bytes(img), img->width, img->height);

    printf("[embed] %dx%d，消息 %u 字节 → 输出 %zu 字节\n",
           img->width, img->height, (unsigned)tlen, olen);

    send_response(c, 200, "OK", "image/bmp", out, olen, extra);

    free(out);
    bmp_free(img);
}

static void handle_extract(SOCKET c, const uint8_t *body, long clen)
{
    if (clen <= 0) {
        send_text(c, 400, "Bad Request", "请求体为空，请先选择图片");
        return;
    }

    BmpImage *img = bmp_load_mem(body, (size_t)clen);
    if (!img) {
        send_text(c, 400, "Bad Request",
                  "图片解析失败：需要 24 位未压缩 BMP（网页会自动把 JPG/PNG 转成 BMP）");
        return;
    }

    size_t   cap = lsb_capacity_bytes(img);
    uint8_t *buf = (uint8_t *)malloc(cap + 2u);
    if (!buf) {
        bmp_free(img);
        send_text(c, 500, "Internal Server Error", "内存不足");
        return;
    }

    size_t    len = 0;
    LsbStatus st  = lsb_extract(img, buf, cap + 1u, &len, 0);

    if (st == LSB_OK) {
        char extra[160];
        snprintf(extra, sizeof(extra),
                 "X-WM-Status: OK\r\nX-WM-Len: %lu\r\nX-WM-Width: %d\r\nX-WM-Height: %d\r\n",
                 (unsigned long)len, img->width, img->height);
        printf("[extract] %dx%d，命中水印，消息 %lu 字节\n",
               img->width, img->height, (unsigned long)len);
        send_response(c, 200, "OK", "text/plain; charset=utf-8", buf, len, extra);
    } else {
        const char *status = (st == LSB_ERR_NOT_FOUND) ? "NOT_FOUND" : "CORRUPT";
        char extra[96];
        snprintf(extra, sizeof(extra), "X-WM-Status: %s\r\n", status);
        printf("[extract] %dx%d，未命中（%s）\n", img->width, img->height, status);
        send_response(c, 200, "OK", "text/plain; charset=utf-8", NULL, 0, extra);
    }

    free(buf);
    bmp_free(img);
}

/* ---------------- 静态资源（web/ 目录） ---------------- */

/*
 * 只放行极少数后缀，并拒绝 ".."、反斜杠、盘符 —— 防目录穿越。
 * 这是自己写 HTTP 服务必须补的一课：不校验 path，就能被人读走任意文件。
 */
static int is_safe_static(const char *p)
{
    if (p[0] != '/') return 0;
    if (strstr(p, "..")) return 0;
    if (strchr(p, '\\')) return 0;
    if (strchr(p, ':')) return 0;

    const char *dot = strrchr(p, '.');
    if (!dot) return 0;
    return !strcmp(dot, ".html") || !strcmp(dot, ".js") ||
           !strcmp(dot, ".css")  || !strcmp(dot, ".png") ||
           !strcmp(dot, ".jpg")  || !strcmp(dot, ".ico");
}

static const char *mime_of(const char *p)
{
    const char *dot = strrchr(p, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".jpg"))  return "image/jpeg";
    return "application/octet-stream";
}

static void serve_static(SOCKET c, const char *path)
{
    if (!is_safe_static(path)) {
        send_text(c, 404, "Not Found", "没有这个页面");
        return;
    }

    uint8_t *buf = NULL;
    size_t   len = 0;
    char     full[512];

    snprintf(full, sizeof(full), "web%s", path);          /* 首选 web/ 子目录 */
    if (read_file(full, &buf, &len) != 0) {
        snprintf(full, sizeof(full), "%s", path + 1);     /* 退路：与 exe 同目录 */
        if (read_file(full, &buf, &len) != 0) {
            send_text(c, 404, "Not Found",
                      "找不到网页文件。请在 E:\\dswork 目录下运行 wm_server.exe，"
                      "并保证 web/index.html、web/wm.js 存在。");
            return;
        }
    }

    send_response(c, 200, "OK", mime_of(path), buf, len, NULL);
    free(buf);
}

/* ---------------- 请求分发 ---------------- */

static void handle_client(SOCKET c)
{
    char hdr[MAX_HDR + 1];
    int  hlen = recv_headers(c, hdr, MAX_HDR);
    if (hlen <= 0) return;

    char method[8] = { 0 };
    char path[256] = { 0 };
    if (sscanf(hdr, "%7s %255s", method, path) != 2) return;

    /* 只取路径部分，去掉 query string */
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    uint8_t *body = NULL;
    long     clen = 0;

    if (strcmp(method, "POST") == 0) {
        clen = get_content_length(hdr);
        if (clen <= 0) {
            send_text(c, 411, "Length Required", "缺少 Content-Length");
            return;
        }
        if ((unsigned long)clen > MAX_BODY) {
            send_text(c, 413, "Payload Too Large", "图片太大（上限 64MB）");
            return;
        }
        body = (uint8_t *)malloc((size_t)clen);
        if (!body) {
            send_text(c, 500, "Internal Server Error", "内存不足");
            return;
        }
        if (recv_all(c, body, (size_t)clen) != 0) {
            free(body);
            return;                       /* 客户端断开，静默退出 */
        }
    }

    if (strcmp(method, "GET") == 0) {
        if (!strcmp(path, "/api/health")) {
            send_text(c, 200, "OK", "ok");
        } else if (!strcmp(path, "/")) {
            serve_static(c, "/index.html");
        } else {
            serve_static(c, path);
        }
    } else if (strcmp(method, "POST") == 0) {
        if (!strcmp(path, "/api/embed"))        handle_embed(c, body, clen);
        else if (!strcmp(path, "/api/extract")) handle_extract(c, body, clen);
        else send_text(c, 404, "Not Found", "没有这个接口");
    } else {
        send_text(c, 405, "Method Not Allowed", "只支持 GET / POST");
    }

    free(body);
}

/* ---------------- 启动 ---------------- */

/* 打印本机可用的 IPv4 地址，方便队友在同一 WiFi 下访问 */
static void print_lan_urls(int port)
{
    char host[256] = { 0 };
    if (gethostname(host, sizeof(host) - 1) != 0) return;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return;

    int printed = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        struct sockaddr_in *sa = (struct sockaddr_in *)p->ai_addr;
        char ip[64] = { 0 };
        if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) {
            if (!strcmp(ip, "127.0.0.1")) continue;
            printf("        http://%s:%d      (同一 WiFi 的队友用这个)\n", ip, port);
            printed++;
        }
    }
    freeaddrinfo(res);
    if (printed == 0)
        printf("        （没发现局域网地址，只能本机访问）\n");
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : WM_PORT;
    if (port <= 0 || port > 65535) port = WM_PORT;

    SetConsoleOutputCP(CP_UTF8);   /* 控制台按 UTF-8 显示中文日志 */
    SetConsoleCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[server] WSAStartup 失败\n");
        return 1;
    }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        fprintf(stderr, "[server] socket 创建失败\n");
        WSACleanup();
        return 1;
    }

    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 0.0.0.0：局域网内都能连 */

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[server] 端口 %d 被占用或绑定失败（换个端口: wm_server.exe 8081）\n",
                port);
        closesocket(srv);
        WSACleanup();
        return 1;
    }
    if (listen(srv, 8) != 0) {
        fprintf(stderr, "[server] listen 失败\n");
        closesocket(srv);
        WSACleanup();
        return 1;
    }

    printf("============================================================\n");
    printf("  AIGC 图像可信水印引擎 · Web 演示服务（M1：LSB 隐写）\n");
    printf("============================================================\n");
    printf("  本机访问:  http://localhost:%d\n", port);
    print_lan_urls(port);
    printf("------------------------------------------------------------\n");
    printf("  按 Ctrl+C 停止服务。首次运行如弹出防火墙提示，请允许“专用网络”。\n");
    printf("============================================================\n\n");

    for (;;) {
        struct sockaddr_in cli;
        int cli_len = (int)sizeof(cli);
        SOCKET c = accept(srv, (struct sockaddr *)&cli, &cli_len);
        if (c == INVALID_SOCKET) continue;

        /* 客户端慢吞吞时别把服务卡死：读写超时 15 秒 */
        DWORD tv = 15000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

        handle_client(c);
        closesocket(c);
    }

    closesocket(srv);
    WSACleanup();
    return 0;
}
