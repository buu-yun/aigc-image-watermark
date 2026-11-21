# 使用手册（MANUAL）

> AIGC 图像可信水印引擎 · C99 · 零第三方库
> 版本：1.0.0-m4（M1~M4 全部交付）

---

## 1. 三十秒上手

```bat
cd /d 项目目录
build.bat
demo.bat
```

看到 `DEMO PASSED` 就说明环境正常。

---

## 2. 三种用法，选一个

| 场景 | 命令 | 说明 |
|---|---|---|
| **看效果** | `demo.bat` | 自动跑完：编译 → 造图 → 自检 → 嵌入 → 提取 → 比对 → 篡改 |
| **用浏览器** | `serve.bat` | 起服务，打开 `http://localhost:8080`；队友同 WiFi 可用 |
| **写程序调用** | 链 `libwm.a` | 见第 5 节，三行代码 |

---

## 3. 命令行速查

### 3.1 嵌入

```bat
wm.exe embed -i 原图.bmp -o 输出.bmp -f 消息.txt -m tile
```

| 参数 | 含义 |
|---|---|
| `-i` | 输入载体（**必须是 24 位未压缩 BMP**） |
| `-o` | 输出文件 |
| `-f` | 消息文件（UTF-8，推荐方式） |
| `-t` | 直接给文本（仅 ASCII 安全） |
| `-m` | `tile`（默认，抗裁剪）/ `dct`（抗压缩，容量大）/ `lsb`（容量最大，怕压缩） |
| `-q` | 安静模式 |

### 3.2 提取 / 验证

```bat
wm.exe extract -i 图.bmp [-o 明文.txt]   :: 自动识别三种方法
wm.exe verify  -i 图.bmp                 :: 三态判定 + 篡改定位
wm.exe info    -i 图.bmp                 :: 尺寸、两种容量、有无水印
wm.exe diff    -i 原图.bmp -j 带水印.bmp  :: 最大像素差 + PSNR
wm.exe tamper  -i 原图.bmp -o 改后.bmp -r 100,100,60,60   :: 模拟涂改
wm.exe heatmap -i 图.bmp -o 热力图.bmp    :: 把被改的格子画红框
wm.exe selftest                          :: 23 项自检
wm.exe bench                             :: 三种方法 × 三种分辨率的耗时与 PSNR
```

`verify` 的退出码可直接给脚本用：

| 退出码 | 含义 |
|---|---|
| 0 | 【真】内容完整且像素未改 |
| 2 | 【可疑】内容可信，但像素被重编码/裁剪/局部编辑 |
| 3 | 【假】内容与嵌入时不一致 |
| 4 | 【无水印】 |

---

## 4. 三种模式怎么选

| 模式 | 容量（1024×768） | 抗 JPEG | 抗裁剪 | 用在哪 |
|---|---|---|---|---|
| `-m lsb` | 294 912 字节 | ❌ 转 JPG 即失效 | ❌ | 只做篡改哨兵、实验室演示 |
| `-m dct` | 495 字节 | ✅ q30 | ❌ | 要写一段完整的溯源信息 |
| `-m tile` | **54 字节**（带口令 46） | ✅ q30 | ✅ 裁 75% | 短水印 + 要被剪切/二次编辑 |

> 水印只有几十字节就够用时，**优先选 tile**——鲁棒性换容量是划算的。

---

## 5. 在程序里调用（库模式）

```c
#include "wm.h"

WmEmbedOptions opt = { WM_METHOD_TILE, "my-key", 6 };   /* 口令可为 NULL */
unsigned char *out; size_t out_len;
WmStatus st = wm_embed_bmp(bmp, bmp_len, (const unsigned char *)"hello", 5,
                           &opt, &out, &out_len);
if (st != WM_OK) { /* wm_status_str(st) 给出中文原因 */ }
wm_free(out);
```

编译：

```bat
gcc app.c -I. -L. -lwm -lm -o app.exe
```

验证：

```c
WmVerifyInfo info;
WmStatus st = wm_verify_bmp(bmp, bmp_len, "my-key", 6, &info);
/* st == WM_OK          → 真（且密钥标签通过）
   st == WM_ERR_TAMPERED→ 假
   st == WM_ERR_NEED_KEY→ 图里带密钥标签，但没给口令
   st == WM_ERR_BAD_KEY → 口令不对（伪造/篡改）
   info.crc_ok / info.authenticated / info.method_name 给出细节 */
```

---

## 6. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `无法创建文件 …（拒绝访问）` | 文件只读/被看图软件占用/权限残留 | 先 `del` 掉旧产物，或换输出名 |
| `仅支持 24 位、无压缩(BI_RGB) BMP` | 图是 8/32 位或 RLE 压缩 | 用画图另存为 24 位 BMP |
| `缺少 'BM' 魔数` | 其实是 PNG/JPG 改了后缀 | 扩展名不作数，先转 BMP |
| `消息太长` | tile 模式上限 54 字节 | 换 `-m dct`，或缩短内容 |
| 提取出乱码 | 图被截图/微信转发（重编码） | 用 tile/dct 模式；lsb 必然失效 |
| 汉语乱码但文件正常 | 控制台代码页 | `chcp 65001`，或看 `-o` 写出的文件 |
| 左/上方向裁剪后读不出 | 已知局限（瓦片原点移位） | 见技术方案第 6 节 |

---

## 7. 验收命令一览

```bat
wm.exe selftest                               :: 23 项内置断言
powershell -File run_m1_acceptance.ps1        :: 14 项
powershell -File run_m2_acceptance.ps1        :: 10 项（含 JPEG q95→q10 全表）
powershell -File run_m3_acceptance.ps1        :: 18 项（含裁剪、定位）
powershell -File run_m4_acceptance.ps1        :: 10 项（库、口令、密码学向量）
```

全部打印 `SUMMARY: ALL PASSED` 才算通过。
