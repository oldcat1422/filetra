// 生成 csrc/assets.h：把 web/ 的静态文件变成 C 字节数组
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url))); // tools/ 的上级 = 项目根

const files = [
  ['index.html', 'AS_INDEX'],
  ['app.js', 'AS_APPJS'],
  ['style.css', 'AS_CSS'],
];

let out = '/* 自动生成：web/ 内嵌资源 */\n';
for (const [file, sym] of files) {
  const b = fs.readFileSync(path.join(ROOT, 'web', file));
  out += `static const unsigned char ${sym}[] = {`;
  const lines = [];
  for (let i = 0; i < b.length; i += 20) {
    lines.push('\n\t' + Array.from(b.slice(i, i + 20), x => '0x' + x.toString(16).padStart(2, '0')).join(','));
  }
  out += lines.join(',') + '\n};\n';
  out += `static const unsigned int ${sym}_LEN = ${b.length};\n\n`;
}
fs.writeFileSync(path.join(ROOT, 'csrc', 'assets.h'), out);
console.log('assets.h OK');
