// nextpnr-spartan's browser test (index.html): the package (pkg/) in a module worker, the XC3S250E's chip database
// fetched over HTTP by the package's chipdb(), each test netlist (tests/expected.txt) placed and routed, its maximum
// frequency compared with the native build's.
const post = m => self.postMessage(m);
try {
  const { runNextpnrHimbaechelSpartan3e, chipdb, chipdbUrl } = await import('./pkg/gen/bundle.js');
  const t0 = performance.now();
  const gz = (await fetch(chipdbUrl('xc3s250ecp132-4'), { method: 'HEAD' })).headers.get('content-length');
  const db = await chipdb('xc3s250ecp132-4');
  const chipdbInfo = { device: 'xc3s250e', gz: +gz, bytes: db.length, s: ((performance.now() - t0) / 1000).toFixed(1) };
  post({ chipdb: chipdbInfo });
  let ok = true;
  const expected = (await (await fetch('tests/expected.txt')).text()).split('\n').filter(l => l.trim() && !l.startsWith('#'));
  for (const line of expected) {
    const [netlist, seed, floor, native] = line.trim().split(/\s+/);
    let log = '';
    const out = bytes => { if (bytes) log += new TextDecoder().decode(bytes); };
    const t1 = performance.now();
    let message, good = false;
    try {
      await runNextpnrHimbaechelSpartan3e(
        ['--device', 'xc3s250ecp132-4', '--chipdb', 'chipdb.bin', '--json', 'design.json', '--write', 'routed.json', '--freq', '50', '--seed', seed],
        { 'design.json': await (await fetch(`tests/${netlist}`)).text(), 'chipdb.bin': db }, { stdout: out, stderr: out });
      const fmax = [...log.matchAll(/Max frequency for clock .*?: ([0-9.]+) MHz/g)].at(-1)?.[1];
      good = /Routing complete/.test(log) && +fmax >= +floor && fmax === native;
      message = `${fmax} MHz${good ? ', as native' : ''}`;
    } catch (e) {
      message = `error: ${e.message || e}`;
      post({ log: log.split('\n').filter(l => /ERROR/.test(l)).join('\n') + '\n' });
    }
    ok &&= good;
    post({ row: { netlist, ok: good, message, native, s: ((performance.now() - t1) / 1000).toFixed(1) } });
  }
  post({ done: true, ok, chipdbInfo });
} catch (e) {
  post({ done: true, ok: false, error: String(e.stack || e) });
}
