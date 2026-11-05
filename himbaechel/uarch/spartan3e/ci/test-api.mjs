// The built package's public interface (npm/README.md, "The interface"), frozen since 1.0.0: the module's exports are
// those lib/api.d.ts declares, no more and no fewer, of the kinds it declares; the version is package.json's; the
// devices and their chip databases' files; deviceOf, chipdbUrl and chipdb; and nextpnr run with the arguments Silinx
// passes (--device --chipdb --json --write --seed --freq --sdc --timing-allow-fail) on a test netlist: the routed
// design as Silinx reads it (each cell's NEXTPNR_BEL, each net's ROUTING), the log's lines, a failure's Exit.
//   node test-api.mjs <package dir>
import fs from 'node:fs';
import path from 'node:path';

const pkg = path.resolve(process.argv[2]);
const tests = path.join(path.dirname(new URL(import.meta.url).pathname), '..', 'tests');
const api = await import(path.join(pkg, 'gen', 'bundle.js'));
let failed = false;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed = true; };
const throws = (f, re) => { try { f(); return false; } catch (e) { return re.test(e.message); } };

// the exports: the values lib/api.d.ts declares (its types are not values), each of its kind
const dts = fs.readFileSync(path.join(pkg, 'lib', 'api.d.ts'), 'utf8');
const declared = new Map([...dts.matchAll(/^export (const|function|class) (\w+)/gm)].map(m => [m[2], m[1]]));
const exported = Object.keys(api).sort(), want = [...declared.keys()].sort();
check(JSON.stringify(exported) === JSON.stringify(want), `the exports are lib/api.d.ts's: ${exported.join(', ')}` +
  (JSON.stringify(exported) === JSON.stringify(want) ? '' : ` (declared: ${want.join(', ')})`));
for (const [name, kind] of declared) {
  const v = api[name];
  const ok = kind === 'const' ? v !== undefined : typeof v === 'function';
  check(ok, `${name}: a ${kind === 'const' ? 'value' : kind}`);
}
check(Object.prototype.isPrototypeOf.call(Error.prototype, api.Exit.prototype), 'Exit: an Error');

// the version and the package's metadata
const meta = JSON.parse(fs.readFileSync(path.join(pkg, 'package.json'), 'utf8'));
check(api.version === meta.version, `version ${api.version}: package.json's (${meta.version})`);
check(/^\d+\.\d+\.\d+(-[\w.]+)?$/.test(api.version), 'version: semantic');
check(meta.exports?.types === './lib/api.d.ts' && meta.exports?.default === './gen/bundle.js', 'package.json exports: lib/api.d.ts, gen/bundle.js');
check(fs.readFileSync(path.join(pkg, 'CHANGELOG.md'), 'utf8').includes(`\n## ${api.version.replace(/-.*/, '')}\n`), `CHANGELOG.md: a section ## ${api.version.replace(/-.*/, '')}`);

// the devices, their chip databases' files, deviceOf, chipdbUrl
const DEVICES = ['xc3s100e', 'xc3s250e', 'xc3s500e', 'xc3s1200e'];
check(DEVICES.every(d => api.devices.includes(d)), `devices: ${api.devices.join(', ')} (1.0.0's at least)`);
for (const d of api.devices) check(fs.existsSync(path.join(pkg, 'chipdb', `chipdb-${d}.bin.gz`)), `chipdb/chipdb-${d}.bin.gz`);
check(api.deviceOf('xc3s250ecp132-4') === 'xc3s250e' && api.deviceOf('XC3S1200EFG320-5') === 'xc3s1200e', 'deviceOf: a part\'s device, in lower case');
check(throws(() => api.deviceOf('xc6slx9tqg144-2'), /no chip database/), 'deviceOf: a device without a chip database throws');
const url = api.chipdbUrl('xc3s500efg320-4');
check(url instanceof URL && url.protocol === 'file:' && fs.realpathSync(url) === fs.realpathSync(path.join(pkg, 'chipdb', 'chipdb-xc3s500e.bin.gz')),
  'chipdbUrl: the package\'s chipdb/chipdb-<device>.bin.gz');
check(typeof api.commands['nextpnr-himbaechel-spartan3e'] === 'function' && Object.keys(api.commands).length === 1, 'commands: nextpnr-himbaechel-spartan3e');

// nextpnr with Silinx's arguments
const part = 'xc3s250ecp132-4';
const db = await api.chipdb(part);
check(db instanceof Uint8Array && db.length > 1 << 20, `chipdb(${part}): ${db.length} bytes`);
const run = async (args, files) => {
  let log = '';
  const out = bytes => { if (bytes) log += new TextDecoder().decode(bytes); };
  try { return { files: await api.runNextpnrHimbaechelSpartan3e(args, files, { stdout: out, stderr: out, decodeASCII: false, fetchProgress: () => {} }), log }; }
  catch (e) { return { error: e, log }; }
};
const netlist = fs.readFileSync(path.join(tests, 'blink_carry.json'), 'utf8');
const clock = Object.entries(Object.values(JSON.parse(netlist).modules)[0].netnames).find(([n]) => /^clk/i.test(n))?.[0];
check(!!clock, `the test netlist's clock: ${clock}`);
const r = await run(['--device', part, '--chipdb', 'chipdb.bin', '--json', 'design.json', '--write', 'routed.json', '--seed', '1',
  '--freq', '50.000', '--sdc', 'design.sdc', '--timing-allow-fail'],
  { 'chipdb.bin': db, 'design.json': netlist, 'design.sdc': `create_clock -period 20 [get_nets {${clock}}]\n` });
check(!r.error, `Silinx's arguments: nextpnr ran${r.error ? ` (${r.error.message}: ${r.log.split('\n').filter(l => /^ERROR/.test(l)).join(' ')})` : ''}`);
if (!r.error) {
  const routed = JSON.parse(new TextDecoder().decode(r.files['routed.json']));
  const mod = Object.values(routed.modules)[0];
  const cells = Object.values(mod.cells);
  check(cells.length > 0 && cells.every(c => /^[^/]+\/S\d+_\w+$/.test(c.attributes?.NEXTPNR_BEL ?? '')), 'routed.json: every cell\'s NEXTPNR_BEL, <tile>/S<site>_<BEL>');
  const routes = Object.values(mod.netnames).map(n => n.attributes?.ROUTING?.trim()).filter(Boolean);   // (a net nothing routes: ' ')
  check(routes.length > 0 && routes.every(s => s.split(';').length >= 3 && /^[^/;]+\/[^/;]+;/.test(s)), 'routed.json: the nets\' ROUTING, <wire>;<pip>;<strength>;…');
  check(routes.some(s => /;[^/;]+\/[^/;]+\/[^/;]+;/.test(s)), 'routed.json: a PIP <tile>/<to>/<from>');
  check(/Max frequency for clock '[^']+': [0-9.]+ MHz/.test(r.log), 'the log: Max frequency for clock \'<clock>\': <f> MHz');
}
const bad = await run(['--device', part, '--chipdb', 'chipdb.bin', '--json', 'design.json', '--write', 'routed.json'],
  { 'chipdb.bin': db, 'design.json': '{ "modules": {' });
// (an exit code other than 0 throws Exit; an error nextpnr stops on, here, may come out as WebAssembly's exception)
check(!!bad.error && /^ERROR: Failed to parse JSON/m.test(bad.log), 'a bad netlist: thrown, an ERROR: line says why');

process.exit(failed ? 1 : 0);
