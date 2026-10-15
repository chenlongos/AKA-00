// 构建后为**大**静态资源生成预压缩的 .br / .gz 兄弟文件。
//
// 板上 aka-capp 按 Accept-Encoding 依次优先下发 `<file>.br`、`<file>.gz`，请求期
// 不做压缩。只有体积 ≥ MIN_SIZE 的文件才预压缩：大文件（如 317KB 的 index.js）才会
// 被 wlan0 多段静默损坏、也会因体积拖垮加载，压缩能同时减少字节数与在途段数；
// 小文件（css/html/json/svg 等）本就单段可靠、压缩无收益，还容易留下过期产物，
// 故不压缩（并在发现旧的 .br/.gz 时清理，回退为直接下发原始）。
//
// 用法：node scripts/gzip-assets.mjs [静态目录]  —— 默认 ../static（相对本脚本）
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';
import zlib from 'node:zlib';

const here = path.dirname(fileURLToPath(import.meta.url));
// frontend/scripts -> repo 根 -> static/
const target = process.argv[2]
  ? path.resolve(process.argv[2])
  : path.resolve(here, '..', '..', 'static');

// 与 cpp/capp/src/http/router.cpp 的 compressible() 扩展名集合保持一致。
const COMPRESSIBLE = new Set([
  '.js', '.mjs', '.css', '.html', '.htm', '.json', '.svg', '.map', '.txt',
]);

// 只有不小于此字节数的可压缩资源才预压缩（约 2 个 MSS 以上才有损坏/拖垮风险）。
// 可用环境变量覆盖：MIN_COMPRESS_BYTES=16384 node scripts/gzip-assets.mjs
const MIN_SIZE = Number(process.env.MIN_COMPRESS_BYTES || 8192);

function walk(dir) {
  let out = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) out = out.concat(walk(full));
    else if (entry.isFile()) out.push(full);
  }
  return out;
}

if (!fs.existsSync(target)) {
  console.error(`[gzip-assets] 目录不存在: ${target}`);
  process.exit(1);
}

let made = 0;
let saved = 0;
let cleaned = 0;
for (const file of walk(target)) {
  if (file.endsWith('.gz') || file.endsWith('.br')) continue;
  if (!COMPRESSIBLE.has(path.extname(file).toLowerCase())) continue;
  const raw = fs.readFileSync(file);

  // 小文件不压缩：直接下发原始（单段可靠），并清理可能残留的旧 .br/.gz。
  if (raw.length < MIN_SIZE) {
    for (const suffix of ['.br', '.gz']) {
      if (fs.existsSync(`${file}${suffix}`)) {
        fs.rmSync(`${file}${suffix}`);
        cleaned++;
        console.log(`  − ${path.relative(target, file)}${suffix}  (小文件不压缩)`);
      }
    }
    continue;
  }

  // 两种预压缩变体都产出：br 更小（现代浏览器首选），gz 兜底（不支持 br 的客户端）。
  // 只有确实比原始小才写，避免给已压缩资源徒增文件。
  const variants = [
    ['.br', zlib.brotliCompressSync(raw, {
      params: { [zlib.constants.BROTLI_PARAM_QUALITY]: 11 },
    })],
    ['.gz', zlib.gzipSync(raw, { level: 9 })],
  ];
  for (const [suffix, buf] of variants) {
    if (buf.length >= raw.length) continue;
    fs.writeFileSync(`${file}${suffix}`, buf);
    made++;
    saved += raw.length - buf.length;
    console.log(`  + ${path.relative(target, file)}${suffix}  ${raw.length} -> ${buf.length} B`);
  }
}
console.log(`[gzip-assets] ${made} 个预压缩（阈值 ${MIN_SIZE}B），省 ${saved} B，清理 ${cleaned} 个旧的过压缩产物 (目录: ${target})`);
