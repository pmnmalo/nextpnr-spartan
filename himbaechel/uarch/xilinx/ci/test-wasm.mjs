// The WebAssembly build of nextpnr-himbaechel-xilinx on the test netlists (tests/expected.txt), run in Node by YoWASP's
// runtime, with the native build's checks (ci/browser/checks.js): routed, the carry chains' feed-through LUTs and the LUT
// cells as expected, the LUTs as Vivado counts them at most 5 % over Vivado's, no warning, the maximum frequency at or
// above its floor (the native build's beside it). Then the package's devices: each chip database read and decompressed,
// deviceOf's parts.
//   node test-wasm.mjs <package dir>
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { part, parseExpected, args, check } from './browser/checks.js';

const pkg = path.resolve(process.argv[2]);
const tests = path.join(path.dirname(new URL(import.meta.url).pathname), '..', 'tests');
const { runNextpnrHimbaechelXilinx, Exit, chipdb, devices, deviceOf, version } = await import(path.join(pkg, 'gen', 'bundle.js'));
let failed = false;
console.log(`@silinx/nextpnr-himbaechel-xc7 ${version}`);
const t0db = Date.now();
const db = await chipdb(part);
console.log(`chip database ${deviceOf(part)} for ${part}: ${db.length} bytes (${((Date.now() - t0db) / 1000).toFixed(1)} s)`);
for (const t of parseExpected(fs.readFileSync(path.join(tests, 'expected.txt'), 'utf8'))) {
  let log = '';
  const out = bytes => { if (bytes) log += new TextDecoder().decode(bytes); };
  const t0 = Date.now();
  let files;
  try {
    files = await runNextpnrHimbaechelXilinx(args(t), {
      'design.json': zlib.gunzipSync(fs.readFileSync(path.join(tests, `${t.name}.json.gz`))).toString(),
      'design.xdc': fs.readFileSync(path.join(tests, `${t.name}.xdc`), 'utf8'), 'chipdb.bin': db }, { stdout: out, stderr: out });
  } catch (e) {
    console.log(`FAIL ${t.name}: ${e instanceof Exit ? `exit ${e.code}` : e.message}`);
    console.log(log.split('\n').filter(l => /ERROR/.test(l)).slice(0, 5).join('\n'));
    failed = true;
    continue;
  }
  const r = check(t, log, files['routed.json']);
  if (r.ok && !(files['design.fasm'] ?? '').trim()) Object.assign(r, { ok: false, message: `${r.message}: no FASM` });
  console.log(`${r.ok ? 'ok  ' : 'FAIL'} ${t.name}: ${r.message} (${((Date.now() - t0) / 1000).toFixed(1)} s, peak rss ${(process.resourceUsage().maxRSS / 1024).toFixed(0)} MiB)`);
  failed ||= !r.ok;
}
// every device's chip database there, read and decompressed; the parts deviceOf maps
for (const d of devices) {
  const bytes = await chipdb(`${d}csg324-1`);
  const ok = bytes.length > 1e6;
  console.log(`${ok ? 'ok  ' : 'FAIL'} chip database ${d}: ${bytes.length} bytes`);
  failed ||= !ok;
}
const parts = { 'xc7a35tcpg236-1': 'xc7a50t', 'xc7a35tcsg324-1': 'xc7a50t', 'xc7a50tcsg324-1': 'xc7a50t', 'XC7A100TCSG324-1': 'xc7a100t' };
for (const [p, d] of Object.entries(parts)) {
  const got = deviceOf(p);
  console.log(`${got === d ? 'ok  ' : 'FAIL'} deviceOf('${p}') = ${got}`);
  failed ||= got !== d;
}
for (const p of ['xc7a200tfbg484-1', 'xc3s250ecp132-4', '']) {
  let threw = false;
  try { deviceOf(p); } catch { threw = true; }
  console.log(`${threw ? 'ok  ' : 'FAIL'} deviceOf('${p}') throws`);
  failed ||= !threw;
}
process.exit(failed ? 1 : 0);
