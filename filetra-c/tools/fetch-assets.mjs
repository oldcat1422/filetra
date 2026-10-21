// 获取 zig 工具链 + qrcodegen（二维码库）源码
import fs from 'fs';
import https from 'https';
import path from 'path';
import { fileURLToPath } from 'url';

const BASE = path.dirname(path.dirname(fileURLToPath(import.meta.url))); // tools/ 的上级 = 项目根

function get(url, headers, onData) {
  return new Promise((res, rej) => {
    https.get(url, { headers }, r => {
      if (r.statusCode >= 300 && r.statusCode < 400 && r.headers.location) {
        get(r.headers.location, headers, onData).then(res, rej);
        return;
      }
      if (r.statusCode !== 200) {
        r.resume();
        rej(new Error('HTTP ' + r.statusCode + ' ' + url));
        return;
      }
      onData(r, res, rej);
    }).on('error', rej);
  });
}

const jsonGet = u => get(u, { 'User-Agent': 'filetra-fetch', 'Accept': 'application/vnd.github+json' },
  (r, res, rej) => { let d = ''; r.setEncoding('utf8'); r.on('data', c => d += c); r.on('end', () => { try { res(JSON.parse(d)); } catch (e) { rej(e); } }); });

const pipeTo = (u, dest) => get(u, { 'User-Agent': 'filetra-fetch' }, (r, res, rej) => {
  console.log('downloading:', u);
  const f = fs.createWriteStream(dest);
  r.pipe(f);
  f.on('finish', () => f.close(() => res()));
  f.on('error', rej);
});

const textGet = u => get(u, { 'User-Agent': 'filetra-fetch' }, (r, res, rej) => {
  let d = ''; r.setEncoding('utf8'); r.on('data', c => d += c); r.on('end', () => res(d));
});

fs.mkdirSync(BASE + '/csrc', { recursive: true });

// 1) zig：优先 0.13.0，否则取最新 release
try {
  const rels = await jsonGet('https://api.github.com/repos/ziglang/zig/releases?per_page=10');
  let pick = rels.find(r => r.tag_name === '0.13.0') || rels[0];
  const asset = (pick.assets || []).find(a =>
    /windows/i.test(a.name) && /x86_64/i.test(a.name) && a.name.endsWith('.zip'));
  if (!asset) throw new Error('tools: ' + (pick.assets || []).map(a => a.name).join(', '));
  console.log('zig release:', pick.tag_name, '->', asset.name, (asset.size / 1e6).toFixed(1) + 'MB');
  await pipeTo(asset.browser_download_url, BASE + '/tools/zig.zip');
  console.log('zig.zip OK');
} catch (e) {
  console.error('zig download failed:', e.message);
}

// 2) qrcodegen（小文件，失败不致命）
try {
  const h = await textGet('https://raw.githubusercontent.com/nayuki/QR-Code-generator/master/c/qrcodegen.h');
  const c = await textGet('https://raw.githubusercontent.com/nayuki/QR-Code-generator/master/c/qrcodegen.c');
  fs.writeFileSync(BASE + '/csrc/qrcodegen.h', h);
  fs.writeFileSync(BASE + '/csrc/qrcodegen.c', c);
  console.log('qrcodegen OK:', h.length, 'B header,', c.length, 'B source');
} catch (e) {
  console.error('qrcodegen failed (QR 可后续补):', e.message);
}

console.log('fetch-done');
