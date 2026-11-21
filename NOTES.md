# 学习笔记：术语表 + 原理解析

> 用途：自己复习 + 答辩前扫一遍。每条都对应代码里的具体位置。

---

# 第一部分 · 术语表

## 1. 项目与阶段

| 缩写 | 全称 | 是什么 | 在本项目的作用 |
|---|---|---|---|
| **AIGC** | Artificial Intelligence Generated Content | 人工智能生成内容 | 赛题对象 |
| **M1~M4** | Milestone 1~4 | 里程碑 | 四个递进阶段，不是难度等级 |

## 2. 图像与格式

| 缩写 | 全称 | 是什么 | 作用 |
|---|---|---|---|
| **BMP** | BitMaP | 无压缩位图 | M1 手写的载体格式；无损，LSB 才活得下来 |
| **JPEG** | Joint Photographic Experts Group | 有损压缩标准 | 我们的"敌人"：它一压，空域水印就死 |
| **PNG** | Portable Network Graphics | 无损压缩图 | 网页版素材 |
| **RGB / BGR** | Red Green Blue / Blue Green Red | 三原色 / 反序三原色 | **BMP 文件里像素是 BGR 顺序**，写反了画面蓝红互换 |
| **Y / Cb / Cr** | Luma / Chroma Blue / Chroma Red | 亮度 / 两个色差 | JPEG 内部色彩空间；我们只改 **Y** |
| **BT.601** | ITU-R BT.601 | 视频标准 | 定义 `Y = 0.299R+0.587G+0.114B` |
| **BI_RGB** | Bitmap Info: RGB | BMP 的"无压缩"标志 | 只接受 compression=0 |
| **RLE** | Run-Length Encoding | 游程编码 | BMP 的一种压缩，我们不支持 |
| **DPI** | Dots Per Inch | 每英寸点数 | BMP 头固定写 2835（≈72 DPI） |

## 3. 水印算法

| 缩写 | 全称 | 是什么 | 作用 |
|---|---|---|---|
| **LSB** | Least Significant Bit | **最低有效位** | 只改它 → 像素变 ±1，肉眼不可见 |
| **MSB** | Most Significant Bit | 最高有效位 | 写比特时"高位先写" |
| **DCT** | Discrete Cosine Transform | **离散余弦变换** | 把 8×8 块拆成 64 个频率系数 |
| **IDCT** | Inverse DCT | 逆变换 | 改完系数再变回像素 |
| **DC** | Direct Current（借词） | **第 0 号系数 = 平均亮度** | 不碰（改了整块明暗变化，最显眼） |
| **AC** | Alternating Current（借词） | 其余 63 个系数 | 改的是中频 AC |
| **T** | Threshold | 阈值 | 强制 `|A|-|B| ≥ T`，决定抗压到几档 |
| **R** | Repetition | 重复份数 | 每比特写几份（正文 3，头部 5） |
| **S/N** | 本项目自定义 | 哨兵魔数 `'S','N'` | 判断"这张图里有没有我们的哨兵"（**不是信噪比**） |

## 4. 校验与安全

| 缩写 | 全称 | 是什么 | 作用 |
|---|---|---|---|
| **CRC / CRC16** | Cyclic Redundancy Check | 循环冗余校验 | 判断内容有没有被改坏；占 2 字节 |
| **CCITT** | 国际电报电话咨询委员会 | 标准组织 | CRC16-CCITT 的多项式 0x1021 |
| **HMAC** | Hash-based Message Authentication Code | 带密钥的消息认证码 | **防伪造**：没密钥算不出来 |
| **SHA-256** | Secure Hash Algorithm | 安全哈希（32 字节） | HMAC 的底层 |
| **MD5** | Message-Digest 5 | 另一种哈希（16 字节） | 没用：太占 DCT 容量 |
| **RS** | Reed–Solomon | 里德-所罗门纠错码 | **原计划用，实际用"重复+软判决"替代** |

## 5. 指标

| 缩写 | 全称 | 是什么 | 作用 |
|---|---|---|---|
| **PSNR** | Peak Signal-to-Noise Ratio | 峰值信噪比（dB） | 衡量改完有多像原图；>38 dB 肉眼不可见 |
| **MSE** | Mean Squared Error | 均方误差 | `PSNR = 10·log10(255²/MSE)` |
| **SSIM** | Structural Similarity | 结构相似度 | 比 PSNR 更贴近人眼；未采用（够用即可） |
| **SNR** | Signal-to-Noise Ratio | 信噪比 | 解释"3 份求和为何抗噪"（√3 提升） |
| **BER** | Bit Error Rate | 误比特率 | 用 CRC 通过与否代替 |

## 6. 工具与平台

| 缩写 | 全称 | 作用 |
|---|---|---|
| **GCC** | GNU Compiler Collection | 编译 C |
| **GNU / MinGW / MSYS2** | GNU's Not Unix / Minimalist GNU for Windows / Minimal SYStem 2 | Windows 上的 GNU 工具链 |
| **LLVM** | Low Level Virtual Machine | clang 后端，wasm 链接器来源 |
| **WASM / WASI** | WebAssembly / WebAssembly System Interface | 浏览器里跑 C；本项目 wasm 51.5 KB |
| **HTTP / HTTPS / TLS** | HyperText Transfer Protocol / Secure / Transport Layer Security | 手写服务 + 公网加密 |
| **API** | Application Programming Interface | 对外接口（`wm.h`、`/api/embed`） |
| **CLI / GUI** | Command Line / Graphical User Interface | 命令行 / 图形界面 |
| **CDP** | Chrome DevTools Protocol | 驱动无头浏览器做自动验收 |
| **ACL / NTFS / errno** | Access Control List / New Technology File System / error number | 排错时用到的文件系统概念 |
| **UTF-8 / BOM / GBK / CP** | Unicode 8 位编码 / Byte Order Mark / 国标扩展 / Code Page | 编码问题：`.ps1` 无 BOM 会被按 ANSI 读 |
| **MIT** | Massachusetts Institute of Technology | 开源许可证 |

## 7. 最容易混的三组

| 易混 | 区别 |
|---|---|
| LSB = Bit 还是 Byte | 本项目一律指**最低位** |
| DC / AC | 不是直流交流电，是**第 0 号系数 / 其余系数** |
| S/N | 我们的**哨兵魔数**，不是信噪比（信噪比缩写是 SNR） |
| R 与 RS | R 是重复份数，RS 是纠错码 |

---

# 第二部分 · 原理笔记

## 1. 三层架构

```
③ 交互层   main.c（CLI）/ wm_server.c（网页）/ wm_api.c（库）
② 算法层   lsb.c（空域） dctwm.c（频域） tilewm.c（分块） + crc16.c / dct.c / crypto.c
① 载体层   bmp.c（24 位 BMP 读写）
```
**换载体只动 ①，换算法只动 ②** —— 这是能一直加功能而不推倒重来的原因。

## 2. 载体层：BMP 的四个坑

| 坑 | 后果 |
|---|---|
| 像素顺序是 **B,G,R** | 画面蓝红互换（但水印照样能取，最难发现） |
| 每行补到 **4 字节倍数** | 不管 padding，从第二行起全错位 |
| 默认**自下而上**存 | 图上下颠倒；负高度才是自上而下 |
| 全部**小端序** | 宽高读成天文数字 |

## 3. 空域（LSB）：为什么不可见，为什么要铺开

- 一个通道只改最低 1 位 → 像素值变 ±1 → 亮度变化 1/255，低于人眼分辨阈；
- 8 个通道的 LSB 拼成 1 字节：`p = (p & 0xFE) | bit`；
- **必须铺开**：顺序写会让 300 字节消息全挤在顶部两行，涂掉画面中部 11% 都伤不到它；
- 铺开公式：第 k 个比特写到通道 `(k × step) % total`，`step` 与 `total` **互质** ⇒ `k ↦ k·step mod total` 是双射，**不重不漏**。

## 4. 频域（DCT）：为什么能扛 JPEG

- JPEG = 8×8 分块 DCT + 逐系数 ÷ 量化步长取整 → **毁掉"值"**；
- 我们的做法：比较两个中频系数的**大小关系**，藏"关系"→ 量化同时缩放两者，**关系不变**；
- 选频段：低频改了看得见，高频被量化表清零，**中频最稳**；本项目取 (v3,u2) 与 (v2,u3)，量化步长 22～29（q50）；
- 写比特：`bit=1 ⇒ |A|-|B| ≥ T`，只抬高较小的那个（能量变化最小）；
- 阈值 T：必须大于量化噪声。M2 固定 24；M3 自适应 `clamp(0.6(|A|+|B|)+14, 20, 40)`；
- 冗余 + 软判决：每比特 3 份，提取时**把 3 份差值求和再取符号**（比投票更耐噪，独立噪声下 SNR 提升 √3）。

## 5. 分块（Tile）：为什么能扛裁剪

- 裁剪让"块序号"整体平移 → 按序号分配的水印全乱；
- 解法：**256×256 瓦片，每片存一份完整水印**；瓦片内只用**局部坐标**分配比特；
- 槽位映射 `(i × 397) % 512`：397 与 512 互质 ⇒ 双射，**每槽恰好 2 块**；
- **哈希分配会翻车**：1024 块 → 512 槽，`(1-1/512)^1024 ≈ e^-2 ≈ 0.135`，约 **69 个槽是空的**，
  空槽的比特只能猜 0 → 实测整帧错 2 bit 作废；
- 重同步：解码先试 (0,0)，失败再试 64 种像素偏移（救"裁剪不是 8 的倍数"）。

## 6. 校验与安全

- **CRC16**：多项式 0x1021，2 字节，抓偶然损坏；**公开算法，可被重算 → 不抗伪造**；
- **哨兵**：4 字节 `'SN'+CRC` **复制 16 份铺满全图**；只采样 32 个通道会漏掉 25% 的涂改（踩过）；
- **HMAC-SHA256**：`tag = HMAC(口令, 原文)` 前 4 字节，封在载荷里 `"WMK1"+tag+原文`；
  错误口令/BAD_KEY、无口令/NEED_KEY，**没有密钥伪造不出来**。

## 7. 数字速记

| 量 | 公式 / 值 |
|---|---|
| LSB 容量 | `W·H·3/8`（512×512 → 98 304 B） |
| DCT 容量 | `(块数 − 80×5) / 24`（1024×768 → 495 B） |
| Tile 容量 | `64 − 10 = 54 B`（带口令 46 B） |
| PSNR | `10·log10(255²/MSE)`；LSB 77~87 dB，Tile 39.5 dB |
| 实测抗压 | DCT/Tile 到 **q30**，q20 失守 |
| 实测抗裁 | 右裁 **75%** / 下裁 66% / 非 8 倍数 100px 全部通过；**左/上裁失败** |

## 8. 答辩 8 问（速答）

| 问题 | 一句话 |
|---|---|
| 为什么改最低位看不出来 | 亮度只变 1/255，PSNR 77 dB |
| 为什么要写 10 字节头 | 告诉提取端"有没有、多长、什么方法" |
| 为什么跳着写 | 顺序写会挤在顶部两行，涂中间都不坏 |
| step 为什么要互质 | 否则只在部分通道打转，留下永远碰不到的区域 |
| 容量多少 | LSB 98KB / DCT 495B / Tile 54B，按鲁棒性需求选 |
| 抗压缩吗 | LSB 不抗；DCT/Tile 抗到 q30 |
| 改一个像素能查出来吗 | LSB 哨兵能报"动过"，DCT 定位能指出大致格子 |
| 为什么不用现成库 | 只用标准库；BMP 手写为讲清格式，密码学手写为可审计 |

## 9. 学习路线（亲测有效）

1. 跑 `demo.bat` → 对照第 3、4 节看输出；
2. 读 `lsb.c` 的三个函数：`write_bits`（96 行）→ `stride_step`（64 行）→ `bit_channel_index`（78 行）；
3. 做 4 个实验（改完记得 `git checkout`）：
   - T 改 8 → PSNR 升高但 q75 就挂（画质与鲁棒性的兑换）
   - 冗余改 1 → q90 就开始劈叉（冗余是抗噪的唯一货币）
   - `slot_of` 换回哈希 → 立刻"差几个比特"（生日悖论的后果）
   - 哨兵改 1 份 → 涂 25% 仍判【真】（采样密度决定灵敏度）
4. 看 `tilewm.c` 的 `decode_at` → 理解"重同步"和"反水率定位"。
