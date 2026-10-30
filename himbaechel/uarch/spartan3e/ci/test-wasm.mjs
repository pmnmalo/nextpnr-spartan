// The WebAssembly build of nextpnr-himbaechel-spartan3e on the test netlists (tests/expected.txt), run in Node by
// YoWASP's runtime: no error, every net routed, the maximum frequency at or above its floor and equal to the native
// build's (the same code, the same seed).
//   node test-wasm.mjs <package dir>
import fs from 'node:fs';
import path from 'node:path';

const pkg = path.resolve(process.argv[2]);
const tests = path.join(path.dirname(new URL(import.meta.url).pathname), '..', 'tests');
const { runNextpnrHimbaechelSpartan3e, Exit } = await import(path.join(pkg, 'gen', 'bundle.js'));
let failed = false;
for (const line of fs.readFileSync(path.join(tests, 'expected.txt'), 'utf8').split('\n')) {
  if (!line.trim() || line.startsWith('#')) continue;
  const [netlist, seed, floor, native] = line.trim().split(/\s+/);
  let log = '';
  const out = bytes => { if (bytes) log += new TextDecoder().decode(bytes); };
  const t0 = Date.now();
  try {
    await runNextpnrHimbaechelSpartan3e(
      ['--device', 'xc3s250ecp132-4', '--json', 'design.json', '--write', 'routed.json', '--freq', '50', '--seed', seed],
      { 'design.json': fs.readFileSync(path.join(tests, netlist), 'utf8') }, { stdout: out, stderr: out });
  } catch (e) {
    console.log(`FAIL ${netlist}: ${e instanceof Exit ? `exit ${e.code}` : e.message}`);
    console.log(log.split('\n').filter(l => /ERROR/.test(l)).slice(0, 5).join('\n'));
    failed = true;
    continue;
  }
  const fmax = [...log.matchAll(/Max frequency for clock .*?: ([0-9.]+) MHz/g)].at(-1)?.[1];
  const s = ((Date.now() - t0) / 1000).toFixed(1);
  if (!/Routing complete/.test(log)) { console.log(`FAIL ${netlist}: not routed`); failed = true; }
  else if (!(+fmax >= +floor)) { console.log(`FAIL ${netlist}: ${fmax} MHz, below ${floor} MHz`); failed = true; }
  else if (fmax !== native) { console.log(`FAIL ${netlist}: ${fmax} MHz, the native build reports ${native} MHz`); failed = true; }
  else console.log(`ok   ${netlist}: ${fmax} MHz, as native (${s} s)`);
}
process.exit(failed ? 1 : 0);
