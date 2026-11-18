// 本地拼装自解压安装器：build-ota.sh 的 heredoc 头部 + payload tar.gz。
// 用途：本机所有盘是 noacl（chmod/test -x 无效），build-ota.sh 的 `[ -x $SRC/aka-capp ]`
// 检查必然失败，跑不了整脚本时用这个按字节拼。
//
// 用法: node scripts/assemble-ota.mjs <payload.tgz> <输出文件>
// 字节保真：按 latin1 读写，UTF-8 中文头部的字节原样拷贝（不要再转编码，
// 否则 0x0A 换行字节可能被改写——踩过坑）。
import { readFileSync, writeFileSync, statSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const [payloadArg, outArg] = process.argv.slice(2);
if (!payloadArg || !outArg) {
  console.error('用法: node scripts/assemble-ota.mjs <payload.tgz> <输出文件>');
  process.exit(1);
}
const cpp = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const scriptPath = path.join(cpp, 'scripts/build-ota.sh');

const script = readFileSync(scriptPath, 'latin1').replace(/\r\n/g, '\n');
const lines = script.split('\n');

const startMarker = `cat > "$TMP_OUT" <<'HEADER'`;
const start = lines.findIndex((l) => l.trim() === startMarker);
if (start < 0) throw new Error('header start not found: ' + startMarker);
let end = -1;
for (let i = start + 1; i < lines.length; i++) {
  if (lines[i] === 'HEADER') { end = i; break; }
}
if (end < 0) throw new Error('header end not found');
const body = lines.slice(start + 1, end).join('\n') + '\n';
if (!body.includes('#__PAYLOAD_BELOW__')) throw new Error('missing #__PAYLOAD_BELOW__');
if (!body.includes('PAYLOAD_OFFSET=0000000')) throw new Error('missing PAYLOAD_OFFSET placeholder');

const placeholderLen = Buffer.from(body, 'latin1').length;
const offset = String(placeholderLen + 1).padStart(7, '0');
const header = Buffer.from(body.replace('PAYLOAD_OFFSET=0000000', 'PAYLOAD_OFFSET=' + offset), 'latin1');
if (header.length !== placeholderLen) throw new Error('offset replacement changed header length');

const payload = readFileSync(payloadArg);
const outPath = path.resolve(outArg);
writeFileSync(outPath, Buffer.concat([header, payload]), { mode: 0o755 });

console.log(JSON.stringify({
  headerSize: header.length,
  offset,
  payloadSize: payload.length,
  totalSize: statSync(outPath).size,
  sha256: createHash('sha256').update(readFileSync(outPath)).digest('hex'),
  output: outPath,
}, null, 2));
