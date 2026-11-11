// nextpnr-spartan's browser test of the xilinx package (index.html): the package (pkg/) in a module worker, the
// xc7a35t's chip database (the xc7a50t's) fetched over HTTP by the package's chipdb(), each test netlist
// (tests/expected.txt, gzipped) placed and routed, with the native build's checks (checks.js).
import { part, parseExpected, args, check } from './checks.js';

const post = m => self.postMessage(m);
const gunzip = async response => new Response(response.body.pipeThrough(new DecompressionStream('gzip'))).text();
try {
  const { runNextpnrHimbaechelXilinx, chipdb, chipdbUrl, deviceOf } = await import('./pkg/gen/bundle.js');
  const t0 = performance.now();
  const gz = (await fetch(chipdbUrl(part), { method: 'HEAD' })).headers.get('content-length');
  const db = await chipdb(part);
  const chipdbInfo = { device: deviceOf(part), gz: +gz, bytes: db.length, s: ((performance.now() - t0) / 1000).toFixed(1) };
  post({ chipdb: chipdbInfo });
  let ok = true;
  for (const t of parseExpected(await (await fetch('tests/expected.txt')).text())) {
    let log = '';
    const out = bytes => { if (bytes) log += new TextDecoder().decode(bytes); };
    const t1 = performance.now();
    let message, good = false;
    try {
      const files = await runNextpnrHimbaechelXilinx(args(t), {
        'design.json': await gunzip(await fetch(`tests/${t.name}.json.gz`)),
        'design.xdc': await (await fetch(`tests/${t.name}.xdc`)).text(), 'chipdb.bin': db }, { stdout: out, stderr: out });
      ({ ok: good, message } = check(t, log, files['routed.json']));
    } catch (e) {
      message = `error: ${e.message || e}`;
      post({ log: log.split('\n').filter(l => /ERROR/.test(l)).join('\n') + '\n' });
    }
    ok &&= good;
    post({ row: { netlist: t.name, ok: good, message, native: t.native, s: ((performance.now() - t1) / 1000).toFixed(1) } });
  }
  post({ done: true, ok, chipdbInfo });
} catch (e) {
  post({ done: true, ok: false, error: String(e.stack || e) });
}
