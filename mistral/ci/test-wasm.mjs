// The WebAssembly build of nextpnr-mistral on the test designs (tests/*.v, synthesised by YoWASP's Yosys,
// synth_intel_alm -family cyclonev) on each board's device: the DE0-CV's 5CEBA4F23C7 (the e50f), the DE1-SoC's
// 5CSEMA5F31C6 and the DE10-Nano's 5CSEBA6U23I7 (the sx120f), the pins of each board's clock, switches and LEDs in a QSF.
// Each run: no ERROR, a .rbf written, and, given the native nextpnr-mistral (built from the same nextpnr and Mistral,
// Mistral's data linked in as upstream links them), the same .rbf byte for byte with the same seed, or else the same placement,
// every net routed, the same maximum frequency and the WebAssembly build's own runs the same (wasm32's routing order is not
// always x86-64's: some nets routed through other wires). Then the package's
// dies and dieOf. pmnmalo/Silinx#5377.
//   node test-wasm.mjs <package dir> <yosys package dir> [native nextpnr-mistral]
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const [pkg, yosysPkg, native] = process.argv.slice(2).map(p => p && path.resolve(p));
const tests = path.join(path.dirname(new URL(import.meta.url).pathname), 'tests');
const { runNextpnrMistral, Exit, data, dies, dieOf, version } = await import(path.join(pkg, 'gen', 'bundle.js'));
const { runYosys } = await import(path.join(yosysPkg, 'gen', 'bundle.js'));
let failed = false;
console.log(`@silinx/nextpnr-mistral ${version}; dies ${dies.join(', ')}`);

const BOARDS = {
  'de0-cv': { part: '5CEBA4F23C7', clk: 'PIN_M9', sw: ['PIN_U13', 'PIN_V13', 'PIN_T13', 'PIN_T12'], led: ['PIN_AA2', 'PIN_AA1', 'PIN_W2', 'PIN_Y3'] },
  'de1-soc': { part: '5CSEMA5F31C6', clk: 'PIN_AF14', sw: ['PIN_AB12', 'PIN_AC12', 'PIN_AF9', 'PIN_AF10'], led: ['PIN_V16', 'PIN_W16', 'PIN_V17', 'PIN_V18'] },
  'de10-nano': { part: '5CSEBA6U23I7', clk: 'PIN_V11', sw: ['PIN_Y24', 'PIN_W24', 'PIN_W21', 'PIN_W20'], led: ['PIN_W15', 'PIN_AA24', 'PIN_V16', 'PIN_V15'] },
};
const qsf = (b, src) => [['clk', b.clk], ...b.led.map((p, i) => [`led[${i}]`, p]), ...(/\bsw\b/.test(src) ? b.sw.map((p, i) => [`sw[${i}]`, p]) : [])]
  .map(([n, p]) => `set_location_assignment ${p} -to ${n}\nset_instance_assignment -name IO_STANDARD "3.3-V LVTTL" -to ${n}\n`).join('');

const text = bytes => (bytes ? new TextDecoder().decode(bytes) : '');
const diffBytes = (a, b) => { let n = Math.abs(a.length - b.length); for (let i = 0; i < Math.min(a.length, b.length); i++) n += a[i] !== b[i]; return n; };
// two routed designs (--write): the cells on other bels (their NEXTPNR_BEL), the nets routed otherwise (their ROUTING), the nets
// with no route in the second
function compareRouted(nativeJson, wasmJson) {
  const top = j => Object.values(JSON.parse(typeof j === 'string' ? j : text(j)).modules)[0];
  const a = top(nativeJson), b = top(wasmJson);
  const cells = Object.keys(a.cells), nets = Object.keys(a.netnames).filter(n => a.netnames[n].attributes?.ROUTING);
  return { cells: cells.length, moved: cells.filter(n => a.cells[n].attributes?.NEXTPNR_BEL !== b.cells[n]?.attributes?.NEXTPNR_BEL).length,
    nets: nets.length, rerouted: nets.filter(n => a.netnames[n].attributes.ROUTING !== b.netnames[n]?.attributes?.ROUTING).length,
    unrouted: nets.filter(n => !b.netnames[n]?.attributes?.ROUTING).length };
}
for (const file of fs.readdirSync(tests).filter(f => f.endsWith('.v')).sort()) {
  const name = path.basename(file, '.v'), src = fs.readFileSync(path.join(tests, file), 'utf8');
  const t0y = Date.now();
  const syn = await runYosys(['-q', '-p', 'read_verilog design.v; synth_intel_alm -family cyclonev -top top; write_json design.json'],
    { 'design.v': src }, { stdout: null, stderr: null });
  const netlist = syn['design.json'];
  console.log(`${name}: synthesised (${((Date.now() - t0y) / 1000).toFixed(1)} s)`);
  for (const [board, b] of Object.entries(BOARDS)) {
    const what = `${name} on ${board} (${b.part})`, constraints = qsf(b, src);
    const args = ['--device', b.part, '--json', 'design.json', '--qsf', 'design.qsf', '--rbf', 'design.rbf', '--write', 'routed.json', '--seed', '1'];
    let log = '';
    const out = bytes => { log += text(bytes); };
    const t0 = Date.now();
    let files;
    try {
      files = await runNextpnrMistral(args, { ...await data(b.part), 'design.json': netlist, 'design.qsf': constraints },
        { stdout: out, stderr: out, decodeASCII: false });
    } catch (e) {
      console.log(`FAIL ${what}: ${e instanceof Exit ? `exit ${e.code}` : e.message}`);
      console.log(log.split('\n').filter(l => /ERROR/.test(l)).slice(0, 5).join('\n') || log.split('\n').slice(-15).join('\n'));
      failed = true;
      continue;
    }
    const rbf = files['design.rbf'], fmax = [...log.matchAll(/Max frequency for clock +'([^']+)': ([\d.]+) MHz/g)].pop();
    const secs = ((Date.now() - t0) / 1000).toFixed(1), rss = (process.resourceUsage().maxRSS / 1024).toFixed(0);
    let ok = rbf instanceof Uint8Array && rbf.length > 1000 && !/ERROR/.test(log), message = `rbf ${rbf?.length ?? 'none'} bytes`;
    if (fmax) message += `, ${fmax[2]} MHz`;
    if (ok && native) {
      const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mistral-'));
      fs.writeFileSync(path.join(dir, 'design.json'), netlist);
      fs.writeFileSync(path.join(dir, 'design.qsf'), constraints);
      try {
        // (nextpnr logs on its standard error)
        const run = spawnSync(native, args, { cwd: dir, maxBuffer: 1 << 28, encoding: 'utf8' });
        if (run.status !== 0) throw Object.assign(new Error(`exit ${run.status}`), { stderr: run.stderr });
        const natLog = `${run.stdout}${run.stderr}`;
        const nat = fs.readFileSync(path.join(dir, 'design.rbf')), same = Buffer.compare(nat, Buffer.from(rbf)) === 0;
        if (same) message += ', the native build\'s .rbf';
        else {
          // (the same placement and the same maximum frequency, its routes another order's: wasm32's hashes and iterations
          // are not x86-64's; and the WebAssembly build's own runs the same)
          const nativeJson = fs.readFileSync(path.join(dir, 'routed.json'), 'utf8'), cmp = compareRouted(nativeJson, files['routed.json']);
          const natFmax = [...natLog.matchAll(/Max frequency for clock +'([^']+)': ([\d.]+) MHz/g)].pop()?.[2];
          const again = await runNextpnrMistral(args, { ...await data(b.part), 'design.json': netlist, 'design.qsf': constraints }, { stdout: null, stderr: null, decodeASCII: false });
          const stable = Buffer.compare(Buffer.from(again['design.rbf']), Buffer.from(rbf)) === 0;
          ok = cmp.moved === 0 && natFmax === fmax?.[2] && cmp.unrouted === 0 && stable;
          message += `, not the native build's .rbf (${diffBytes(nat, rbf)} bytes differ): ${cmp.moved} of ${cmp.cells} cells placed elsewhere, ${cmp.rerouted} of ${cmp.nets} nets routed otherwise, `
            + `${cmp.unrouted} unrouted, the native build's ${natFmax} MHz, the WebAssembly build ${stable ? 'the same twice' : 'NOT the same twice'}`;
        }
      } catch (e) {
        ok = false;
        message += `, the native build failed: ${String(e.stderr ?? e.message).split('\n').filter(l => /ERROR/.test(l)).join(' ')}`;
      }
    }
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}: ${message} (${secs} s, peak rss ${rss} MiB)`);
    if (!ok) console.log(log.split('\n').filter(l => /ERROR|Warning/.test(l)).slice(0, 10).join('\n'));
    failed ||= !ok;
  }
}
// the package's dies, each file there; dieOf's parts
for (const d of dies) {
  const part = Object.values(BOARDS).find(b => dieOf(b.part) === d)?.part;
  const bytes = part && Object.values(await data(part))[0];
  const ok = !!bytes && bytes.length > 1e6;
  console.log(`${ok ? 'ok  ' : 'FAIL'} die ${d}: ${bytes?.length ?? 'no part'} bytes`);
  failed ||= !ok;
}
const parts = { '5CEBA4F23C7': 'e50f', '5ceba2f17c7': 'e50f', '5CSEMA5F31C6': 'sx120f', '5CSEBA6U23I7': 'sx120f' };
for (const [p, d] of Object.entries(parts)) {
  const got = dieOf(p);
  console.log(`${got === d ? 'ok  ' : 'FAIL'} dieOf('${p}') = ${got}`);
  failed ||= got !== d;
}
for (const p of ['5CGXFC7C7F23C8', 'EP4CE22F17C6', '']) {
  let threw = false;
  try { dieOf(p); } catch { threw = true; }
  console.log(`${threw ? 'ok  ' : 'FAIL'} dieOf('${p}') throws`);
  failed ||= !threw;
}
process.exit(failed ? 1 : 0);
