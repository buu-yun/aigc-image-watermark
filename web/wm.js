/*
 * wm.js —— 前端公共逻辑（被 index.html 和 selftest.html 共用）
 *
 * 为什么要有这个文件：网页端唯一"有技术含量"的地方，就是把用户随手丢进来的
 * JPG/PNG 统一成 24 位 BMP。这段逻辑必须有且只有一份，否则自检页测的就不是线上跑的那份代码了。
 */
window.WM = (function () {
  'use strict';

  /* 读 BMP 头拿宽高（严格校验在服务端做，这里只为显示容量） */
  function readBmpInfo(buf) {
    const dv = new DataView(buf);
    return { w: dv.getInt32(18, true), h: Math.abs(dv.getInt32(22, true)) };
  }

  /* 容量 = 宽*高*3/8 字节（每像素 3 bit） */
  function capacityOf(w, h) { return Math.floor(w * h * 3 / 8); }

  /*
   * 把 canvas 的 RGBA 像素打包成 24 位 BMP：
   *   ① 自下而上存（BMP 的历史包袱）
   *   ② 每像素 B,G,R（不是 RGB）
   *   ③ 每行补齐到 4 字节的倍数
   * 这三条与服务端 bmp.c 的解析规则必须完全一致。
   */
  function packFromImageData(px, w, h) {
    const stride = (w * 3 + 3) & ~3;
    const total = 54 + stride * h;
    const out = new Uint8Array(total);            // 全 0 初始化，行填充天然为 0
    const dv = new DataView(out.buffer);

    out[0] = 0x42; out[1] = 0x4D;                 // 'BM'
    dv.setUint32(2, total, true);                 // 文件大小
    dv.setUint32(10, 54, true);                   // 像素数据偏移
    dv.setUint32(14, 40, true);                   // 信息头大小
    dv.setInt32(18, w, true);                     // 宽
    dv.setInt32(22, h, true);                     // 正高度 = 自下而上
    dv.setUint16(26, 1, true);                    // planes
    dv.setUint16(28, 24, true);                   // 24 位
    dv.setUint32(30, 0, true);                    // BI_RGB 无压缩
    dv.setUint32(34, stride * h, true);
    dv.setUint32(38, 2835, true);                 // ≈72 DPI
    dv.setUint32(42, 2835, true);

    for (let r = 0; r < h; r++) {                 // r = 文件中的行号（自下而上）
      const srcY = h - 1 - r;
      let o = 54 + r * stride;
      let s = srcY * w * 4;
      for (let x = 0; x < w; x++) {
        out[o++] = px[s + 2];                     // B
        out[o++] = px[s + 1];                     // G
        out[o++] = px[s];                         // R
        s += 4;
      }
    }
    return out.buffer;
  }

  /* 已解码的 Image → {bmp, w, h} */
  function rasterToBmp(img) {
    const w = img.naturalWidth, h = img.naturalHeight;
    if (!w || !h) throw new Error('图片尺寸为 0，无法处理');
    if (w * h > 16e6) throw new Error('图片太大（超过 1600 万像素），请先缩到 2000px 以内');

    const cv = document.createElement('canvas');
    cv.width = w; cv.height = h;
    const ctx = cv.getContext('2d', { willReadFrequently: true });
    ctx.drawImage(img, 0, 0);
    const px = ctx.getImageData(0, 0, w, h).data;  // RGBA，自上而下
    return { bmp: packFromImageData(px, w, h), w: w, h: h };
  }

  /* File/Blob → {bmp, w, h}：本来就是 BMP 就原样送，否则光栅化后再打包 */
  async function fileToBmp(file) {
    const raw = await file.arrayBuffer();
    if (raw.byteLength >= 54) {
      const h2 = new Uint8Array(raw, 0, 2);
      if (h2[0] === 0x42 && h2[1] === 0x4D) {
        const i = readBmpInfo(raw);
        return { bmp: raw, w: i.w, h: i.h };
      }
    }
    const url = URL.createObjectURL(file);
    try {
      const img = new Image();
      img.src = url;
      await img.decode();
      return rasterToBmp(img);
    } finally {
      URL.revokeObjectURL(url);
    }
  }

  /* 请求体 = [4 字节小端消息长度][UTF-8 消息][24 位 BMP]，避开 multipart 边界解析 */
  function buildEmbedBody(textBytes, bmp) {
    const buf = new ArrayBuffer(4 + textBytes.length + bmp.byteLength);
    new DataView(buf).setUint32(0, textBytes.length, true);
    new Uint8Array(buf, 4, textBytes.length).set(textBytes);
    new Uint8Array(buf, 4 + textBytes.length).set(new Uint8Array(bmp));
    return buf;
  }

  /* 嵌入：返回 {blob, msgLen, capacity}；失败抛出带中文原因的错误 */
  async function postEmbed(bmp, text) {
    const textBytes = new TextEncoder().encode(text);
    const r = await fetch('/api/embed', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: buildEmbedBody(textBytes, bmp)
    });
    if (!r.ok) throw new Error((await r.text()) || ('服务返回 ' + r.status));
    return {
      blob: await r.blob(),
      msgLen: r.headers.get('X-WM-MsgLen'),
      capacity: r.headers.get('X-WM-Capacity')
    };
  }

  /* 提取：返回 {status, text}，status ∈ {OK, NOT_FOUND, CORRUPT} */
  async function postExtract(bmp) {
    const r = await fetch('/api/extract', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: bmp
    });
    if (!r.ok) throw new Error((await r.text()) || ('服务返回 ' + r.status));
    return { status: r.headers.get('X-WM-Status') || '', text: await r.text() };
  }

  return { readBmpInfo, capacityOf, rasterToBmp, fileToBmp, buildEmbedBody, postEmbed, postExtract };
})();
