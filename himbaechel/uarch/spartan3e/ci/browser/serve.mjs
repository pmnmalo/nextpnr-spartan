// A static server for the browser test (no dependencies): / is this folder, /pkg the package, /tests the test netlists.
//   node serve.mjs <package dir> [port]
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';

const here = path.dirname(new URL(import.meta.url).pathname);
const roots = { pkg: path.resolve(process.argv[2]), tests: path.resolve(here, '..', '..', 'tests') };
const port = +(process.argv[3] || 8790);
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json', '.txt': 'text/plain', '.tar': 'application/x-tar', '.gz': 'application/gzip' };
http.createServer((req, res) => {
  const url = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const [, top, ...rest] = url.split('/');
  const file = roots[top] ? path.join(roots[top], ...rest) : path.join(here, url === '/' ? 'index.html' : url);
  if (![here, ...Object.values(roots)].some(r => file.startsWith(r)) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404).end('not found');
    return;
  }
  res.writeHead(200, { 'content-type': types[path.extname(file)] || 'application/octet-stream', 'content-length': fs.statSync(file).size });
  if (req.method === 'HEAD') res.end(); else fs.createReadStream(file).pipe(res);
}).listen(port, () => console.log(`http://localhost:${port}/`));
