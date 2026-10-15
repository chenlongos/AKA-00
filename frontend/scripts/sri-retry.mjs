// 构建后为 SPA 入口注入「SRI + 失败重试」加载器（治标规避）。
//
// 背景：板子 wlan0 对多段 TCP 载荷存在非确定性静默损坏（~80KB 级 15–40% 概率传坏，
// 且损坏发生在板侧 TCP 校验和之前 → 客户端 TCP 无法察觉）。压缩能减少字节数、降低触发
// 概率，但救不了正确性：浏览器可能拿到「长度对、校验和合法、内容损坏」的 JS 而解析失败
// 黑屏。Subresource Integrity(SRI) 按解码后内容校验，损坏即被浏览器丢弃并触发 onerror，
// 于是我们换 URL 重取，直到某次传输干净（每次独立连接、损坏随机 → 多试必成）。
//
// 做法：读原始 static/assets/index.{js,css} 计算 sha384，替换 vite 注入的静态
// <script>/<link> 为一段内联加载器（内联脚本随极小的 index.html 传输，不在易损坏的大
// 载荷里，故可靠）。仅影响生产产物，dev（vite serve）不受影响。
//
// 用法：node scripts/sri-retry.mjs [静态目录]  —— 默认 ../../static（相对本脚本）
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';
import crypto from 'node:crypto';

const here = path.dirname(fileURLToPath(import.meta.url));
const target = process.argv[2]
  ? path.resolve(process.argv[2])
  : path.resolve(here, '..', '..', 'static');

const htmlPath = path.join(target, 'index.html');
if (!fs.existsSync(htmlPath)) {
  console.error(`[sri-retry] 缺 index.html: ${htmlPath}`);
  process.exit(1);
}

function integrityOf(file) {
  const buf = fs.readFileSync(file);
  return 'sha384-' + crypto.createHash('sha384').update(buf).digest('base64');
}

const jsFile = path.join(target, 'assets', 'index.js');
const cssFile = path.join(target, 'assets', 'index.css');
if (!fs.existsSync(jsFile)) {
  console.warn('[sri-retry] 未找到 assets/index.js，跳过（前端未构建？）');
  process.exit(0);
}
const jsSri = integrityOf(jsFile);
const cssSri = fs.existsSync(cssFile) ? integrityOf(cssFile) : '';

let html = fs.readFileSync(htmlPath, 'utf8');

// 幂等：已注入加载器则不重复处理。
if (html.includes('/*SRI-RETRY*/')) {
  console.log('[sri-retry] 已注入过，跳过');
  process.exit(0);
}

// 移除 vite 注入的入口 script 与 stylesheet link（属性顺序/引号风格容错）。
const before = html;
html = html.replace(/<script[^>]*\ssrc=["']\/assets\/index\.js["'][^>]*>\s*<\/script>/i, '');
html = html.replace(/<link[^>]*\shref=["']\/assets\/index\.css["'][^>]*>/i, '');
if (html === before) {
  console.warn('[sri-retry] 未匹配到入口 <script>/<link>，可能 vite 输出格式变了；请检查 index.html');
}

// 注：幂等标记 `/*SRI-RETRY*/` 必须放在 <script> 内部（当 JS 注释）。
// 若裸露在 head/body 会被 HTML 解析器当作文本节点渲染出来（页面左上角可见）。
const loader = `
    <script>/*SRI-RETRY*/
    (function () {
      // 每次 onerror 换新 URL(retry=n) 触发全新连接重取；损坏随机 → 多试必成。
      var MAX_RETRY = 10;
      function load(url, sri, make, done) {
        var n = 0;
        (function attempt() {
          var sep = url.indexOf('?') >= 0 ? '&' : '?';
          var el = make(url + sep + 'retry=' + n, sri);
          var settled = false;
          el.onload = function () { if (settled) return; settled = true; done(true); };
          el.onerror = function () {
            if (settled) return; settled = true;
            el.parentNode && el.parentNode.removeChild(el);
            if (++n < MAX_RETRY) attempt(); else done(false);
          };
          (document.head || document.documentElement).appendChild(el);
        })();
      }
      function showErr() {
        if (document.getElementById('boot-err')) return;
        var d = document.createElement('div');
        d.id = 'boot-err';
        d.textContent = '资源加载失败（链路不稳定），请重试';
        d.style.cssText = 'position:fixed;left:12px;right:12px;bottom:12px;padding:12px 14px;background:#b91c1c;color:#fff;border-radius:8px;z-index:2147483647;font:14px/1.4 system-ui,sans-serif;text-align:center';
        (document.body || document.documentElement).appendChild(d);
      }
      load('/assets/index.css', ${JSON.stringify(cssSri)}, function (u, s) {
        var l = document.createElement('link');
        l.rel = 'stylesheet'; l.href = u; l.crossOrigin = 'anonymous';
        if (s) l.integrity = s;
        return l;
      }, function () {});
      load('/assets/index.js', ${JSON.stringify(jsSri)}, function (u, s) {
        var t = document.createElement('script');
        t.type = 'module'; t.src = u; t.crossOrigin = 'anonymous';
        if (s) t.integrity = s;
        return t;
      }, function (ok) { if (!ok) showErr(); });
    })();
    </script>`;

// 插到 </head> 前（模块脚本本就在 head，时序等价）。
if (/<\/head>/i.test(html)) {
  html = html.replace(/<\/head>/i, loader + '\n  </head>');
} else {
  html = html.replace(/<body/i, loader + '\n  <body');
}

fs.writeFileSync(htmlPath, html);
console.log(`[sri-retry] 已注入 SRI+重试加载器 → ${htmlPath}`);
console.log(`  js  ${jsSri}`);
console.log(`  css ${cssSri || '(无 index.css)'}`);
