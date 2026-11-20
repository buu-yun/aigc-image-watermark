// wasm 端到端自检：纯 node，不需要浏览器
// 用法: node wasm/test_wasm.mjs
import fs from 'node:fs';

const wasmBytes = fs.readFileSync('wasm/wm.wasm');
const wasi = {
  fd_write: () => 0,   // 日志丢弃
  fd_close: () => 0,
  fd_seek: () => 0,
};

const { instance } = await WebAssembly.instantiate(wasmBytes, { wasi_snapshot_preview1: wasi });
const ex = instance.exports;
if (ex._initialize) ex._initialize();   // reactor 模型：先跑构造器

// 内存可能增长导致 buffer 失效，所以每次都重新取视图
const mem = () => new Uint8Array(ex.memory.buffer);
const IN = ex.wm_in_ptr();
const OUT = ex.wm_out_ptr();

const bmp = fs.readFileSync('test_image.bmp');
const text = Buffer.from(
  'wasm 测试：这份文字由 C 代码在浏览器/Node 的 WebAssembly 沙箱里嵌入。作者=隐写溯源小队',
  'utf8'
);

let pass = 0, fail = 0;
const check = (ok, name, extra = '') => {
  console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${extra ? '  ' + extra : ''}`);
  ok ? pass++ : fail++;
};

// 1) 容量
mem().set(bmp, IN);
const cap = ex.wm_capacity(bmp.length);
check(cap === 98304, '容量查询 = 98304', `实际 ${cap}`);

// 2) 嵌入
mem().set(bmp, IN);
mem().set(text, IN + bmp.length);
const outLen = ex.wm_embed(bmp.length, text.length);
check(outLen > 0, '嵌入返回输出字节数', `${outLen} 字节`);
const outBmp = Buffer.from(ex.memory.buffer, OUT, outLen).slice();
fs.writeFileSync('wasm_out.bmp', outBmp);
check(outBmp[0] === 0x42 && outBmp[1] === 0x4d, '输出是合法 BMP（BM 魔数）');

// 3) 提取（把刚生成的图塞回去）
mem().set(outBmp, IN);
const exLen = ex.wm_extract(outBmp.length);
check(exLen === text.length, '提取长度与原文一致', `${exLen} vs ${text.length}`);
const got = Buffer.from(ex.memory.buffer, OUT, Math.max(exLen, 0)).slice();
check(got.equals(text), '提取内容逐字节一致');

// 4) 未嵌入的图
mem().set(bmp, IN);
const nf = ex.wm_extract(bmp.length);
check(nf === -3, '未嵌入图返回 NOT_FOUND(-3)', `实际 ${nf}`);

// 5) 容量不足
const tiny = Buffer.alloc(300);   // 不是 BMP → 应返回 -101
mem().set(tiny, IN);
const bad = ex.wm_embed(300, 5000);
check(bad === -101 || bad === -2, '非法输入返回错误码而不是崩溃', `实际 ${bad}`);

console.log(`\n结果: ${pass}/${pass + fail} ${fail === 0 ? '全部通过' : '存在失败'}`);
process.exit(fail === 0 ? 0 : 1);
