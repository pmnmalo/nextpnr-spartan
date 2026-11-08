// Spike (Silinx #2305): one design placed and routed by the WebAssembly build, in Node, by YoWASP's runtime (as the
// spartan3e package's test-wasm.mjs): its time, the process's peak memory, its maximum frequency, its FASM.
//   node run-wasm.mjs <package dir> <design.json.gz> <design.xdc> <part> <router> <out.fasm>
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const [pkg, json, xdc, part, router, out] = process.argv.slice(2);
const { runNextpnrHimbaechelXilinx, Exit, chipdb } = await import(path.join(path.resolve(pkg), 'gen', 'bundle.js'));
const t0db = Date.now();
const db = await chipdb(part);
console.log(`chip database for ${part}: ${db.length} bytes (${((Date.now() - t0db) / 1000).toFixed(1)} s to read and decompress)`);
let log = '';
const echo = bytes => { if (bytes) log += new TextDecoder().decode(bytes); };
const t0 = Date.now();
let files;
try {
  files = await runNextpnrHimbaechelXilinx(
    ['--device', part, '--chipdb', 'chipdb.bin', '--json', 'design.json', '-o', 'xdc=design.xdc', '-o', 'fasm=design.fasm', '--router', router, '--seed', '1'],
    { 'design.json': zlib.gunzipSync(fs.readFileSync(json)).toString(), 'design.xdc': fs.readFileSync(xdc, 'utf8'), 'chipdb.bin': db },
    { stdout: echo, stderr: echo });
} catch (e) {
  console.log(`FAIL ${e instanceof Exit ? `exit ${e.code}` : e.message}`);
  console.log(log.split('\n').filter(l => /ERROR|error/.test(l)).slice(0, 10).join('\n'));
  process.exit(1);
}
const s = ((Date.now() - t0) / 1000).toFixed(1);
const fmax = [...log.matchAll(/Max frequency for clock .*?: ([0-9.]+) MHz/g)].at(-1)?.[1];
fs.writeFileSync(out, files['design.fasm']);
fs.writeFileSync(out.replace(/\.fasm$/, '.log'), log);
const warnings = log.split('\n').filter(l => /^Warning/.test(l));
console.log(`${path.basename(json)} ${part} ${router}: ${s} s, ${fmax} MHz, peak rss ${(process.resourceUsage().maxRSS / 1024).toFixed(0)} MiB, ${warnings.length} warnings`);
for (const w of warnings) console.log(`  ${w}`);
