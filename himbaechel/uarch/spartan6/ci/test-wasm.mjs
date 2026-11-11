// The WebAssembly build of nextpnr-himbaechel-spartan6 on the test netlists (tests/expected.txt, tests/pack.txt), run in Node by
// YoWASP's runtime: no error, every net routed, the maximum frequency the one recorded there (the same code, the same
// seed). The device's chip database is fetched alone (the package's chipdb()) and given with --chipdb.
//   node test-wasm.mjs <package dir>
import fs from 'node:fs';
import path from 'node:path';

const pkg = path.resolve(process.argv[2]);
const tests = path.join(path.dirname(new URL(import.meta.url).pathname), '..', 'tests');
const { runNextpnrHimbaechelSpartan6, Exit, chipdb } = await import(path.join(pkg, 'gen', 'bundle.js'));
let failed = false;
const db = await chipdb('xc6slx16csg324-3');
console.log(`chip database xc6slx16: ${db.length} bytes`);
for (const line of fs.readFileSync(path.join(tests, 'expected.txt'), 'utf8').split('\n')) {
  if (!line.trim() || line.startsWith('#')) continue;
  const [netlist, seed, floor, native, ...extra] = line.trim().split(/\s+/);
  let log = '';
  const out = bytes => { if (bytes) log += new TextDecoder().decode(bytes); };
  const t0 = Date.now();
  try {
    await runNextpnrHimbaechelSpartan6(
      ['--device', 'xc6slx16csg324-3', '--chipdb', 'chipdb.bin', '--json', 'design.json', '--write', 'routed.json', '--freq', '50', '--seed', seed, ...extra],
      { 'design.json': fs.readFileSync(path.join(tests, netlist), 'utf8'), 'chipdb.bin': db }, { stdout: out, stderr: out });
  } catch (e) {
    console.log(`FAIL ${netlist}: ${e instanceof Exit ? `exit ${e.code}` : e.message}`);
    console.log(log.split('\n').filter(l => /ERROR/.test(l)).slice(0, 5).join('\n'));
    failed = true;
    continue;
  }
  const fmax = [...log.matchAll(/Max frequency for clock .*?: ([0-9.]+) MHz/g)].at(-1)?.[1];
  if (!/Program finished normally/.test(log)) { console.log(`FAIL ${netlist}: not routed`); failed = true; }
  else if (!(+fmax >= +floor)) { console.log(`FAIL ${netlist}: ${fmax} MHz, below ${floor} MHz`); failed = true; }
  else if (fmax !== native) { console.log(`FAIL ${netlist}: ${fmax} MHz, ${native} MHz expected`); failed = true; }
  else console.log(`ok   ${netlist}: ${fmax} MHz, as expected (${((Date.now() - t0) / 1000).toFixed(1)} s)`);
}
// the pads whose path the chip database got from Silinx's held PIPs (tests/pads.txt): placed and routed on their
// device, no warning
for (const line of fs.readFileSync(path.join(tests, 'pads.txt'), 'utf8').split('\n')) {
  if (!line.trim() || line.startsWith('#')) continue;
  const [netlist, device] = line.trim().split(/\s+/);
  let log = '';
  const out = bytes => { if (bytes) log += new TextDecoder().decode(bytes); };
  try {
    await runNextpnrHimbaechelSpartan6(['--device', device, '--chipdb', 'chipdb.bin', '--json', 'design.json', '--write', 'routed.json', '--freq', '50', '--seed', '1'],
      { 'design.json': fs.readFileSync(path.join(tests, netlist), 'utf8'), 'chipdb.bin': await chipdb(device) }, { stdout: out, stderr: out });
  } catch (e) {
    console.log(`FAIL ${netlist}: ${e instanceof Exit ? `exit ${e.code}` : e.message}`);
    console.log(log.split('\n').filter(l => /ERROR/.test(l)).slice(0, 5).join('\n'));
    failed = true;
    continue;
  }
  const warnings = log.split('\n').filter(l => /^Warning:/.test(l));
  if (!/Program finished normally/.test(log)) { console.log(`FAIL ${netlist}: not routed`); failed = true; }
  else if (warnings.length) { console.log(`FAIL ${netlist}: ${warnings.slice(0, 5).join('\n')}`); failed = true; }
  else console.log(`ok   ${netlist}: routed on ${device}, no warning`);
}
// the packer and the pads' rules (tests/pack.txt, its --pack-only netlists): each one's Info: Packed, Warning: and
// ERROR: lines exactly those listed
const packs = new Map();
for (const line of fs.readFileSync(path.join(tests, 'pack.txt'), 'utf8').split('\n')) {
  if (!line.trim() || line.startsWith('#')) continue;
  const [, netlist, mode, want] = /^(\S+)\s+(\S+)\s+(.*)$/.exec(line.trim());
  if (mode === 'pack') (packs.get(netlist) ?? packs.set(netlist, []).get(netlist)).push(want);
}
for (const [netlist, want] of packs) {
  let log = '';
  const out = bytes => { if (bytes) log += new TextDecoder().decode(bytes); };
  let stopped = false;
  try {
    await runNextpnrHimbaechelSpartan6(['--device', 'xc6slx16csg324-3', '--chipdb', 'chipdb.bin', '--json', 'design.json', '--pack-only'],
      { 'design.json': fs.readFileSync(path.join(tests, netlist), 'utf8'), 'chipdb.bin': db }, { stdout: out, stderr: out });
  } catch (e) {
    stopped = true;
  }
  const got = log.split('\n').filter(l => /^(Info: Packed|Warning:|ERROR:)/.test(l));
  if (JSON.stringify(got) !== JSON.stringify(want)) { console.log(`FAIL ${netlist}:\n  want ${want.join('\n       ')}\n  got  ${got.join('\n       ')}`); failed = true; }
  else if (stopped !== want.some(l => l.startsWith('ERROR:'))) { console.log(`FAIL ${netlist}: ${stopped ? 'stopped' : 'did not stop'}`); failed = true; }
  else console.log(`ok   ${netlist}: packed as expected`);
}
process.exit(failed ? 1 : 0);
