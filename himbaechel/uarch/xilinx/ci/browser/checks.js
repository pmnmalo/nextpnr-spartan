// The checks of a test netlist's run (tests/expected.txt), as ci/build-and-test.sh makes them natively, for the
// WebAssembly package in Node (ci/test-wasm.mjs) and in a browser (worker.js): routed, the carry chains' feed-through
// LUTs and the LUT cells as expected, the LUTs as Vivado counts them at most 15 % over Vivado's, no warning, the maximum
// frequency at or above its floor (the native build's shown beside it).

export const part = 'xc7a35tcpg236-1';

/** tests/expected.txt's lines: netlist, seed, feed-throughs, DI's, LUT cells, Vivado's LUTs, floor, native (MHz). */
export function parseExpected(text) {
    return text.split('\n').filter(l => l.trim() && !l.startsWith('#')).map(l => {
        const [name, seed, feeds, feedsDi, luts, vivado, floor, native] = l.trim().split(/\s+/);
        return { name, seed, feeds: +feeds, feedsDi: +feedsDi, luts: +luts, vivado: +vivado, floor: +floor, native };
    });
}

/** nextpnr's arguments for a test netlist: design.json and design.xdc in, design.fasm and routed.json out, router 1. */
export const args = t => ['--device', part, '--chipdb', 'chipdb.bin', '--json', 'design.json', '-o', 'xdc=design.xdc',
    '-o', 'fasm=design.fasm', '--write', 'routed.json', '--router', 'router1', '--seed', t.seed];

/**
 * The LUTs of a routed design (its --write JSON) as Vivado's report_utilization counts them (ci/lut-count.py): a LUT
 * site (a 6LUT and its 5LUT) holding logic once, a site holding only feed-throughs (route-thrus) not at all.
 */
export function lutCount(routed) {
    const mod = Object.values(routed.modules).reduce((a, m) => Object.keys(m.cells).length > Object.keys(a.cells).length ? m : a);
    const sites = new Map();
    let cells = 0, feeds = 0;
    for (const [name, c] of Object.entries(mod.cells)) {
        const bel = c.attributes?.NEXTPNR_BEL ?? '';
        if (c.type !== 'SLICE_LUTX' || !bel) continue;
        const feed = /\$LUT\d+$/.test(name) || name.endsWith('$feed$zero');
        const [tile, b] = bel.split('.');
        const site = `${tile}.${b[0]}`;
        sites.set(site, (sites.get(site) ?? false) || !feed);
        cells++;
        feeds += feed;
    }
    return { cells, feeds, sites: [...sites.values()].filter(Boolean).length, allSites: sites.size };
}

/** A run's result: { ok, message, fmax }, from its log and its routed design (undefined when nextpnr failed). */
export function check(t, log, routedText) {
    if (!/Program finished normally/.test(log) || routedText === undefined) return { ok: false, message: 'not routed' };
    const feeds = +(/Created (\d+) feed-through LUTs from carry chains/.exec(log)?.[1] ?? 0);
    const feedsDi = +(/from carry chains \(.*; DI (\d+)\)/.exec(log)?.[1] ?? 0);
    const luts = +[...log.matchAll(/SLICE_LUTX: *(\d+)\//g)].at(-1)?.[1];
    const { sites } = lutCount(JSON.parse(routedText));
    const fmax = [...log.matchAll(/Max frequency for clock .*?: ([0-9.]+) MHz/g)].at(-1)?.[1];
    const warnings = log.split('\n').filter(l => /^Warning/.test(l));
    const line = `feed-throughs ${feeds} (DI ${feedsDi}), ${luts} LUT cells, ${sites} LUTs as Vivado counts them (Vivado ${t.vivado}), ${fmax} MHz, ${warnings.length} warnings`;
    const fail = why => ({ ok: false, message: `${line}: ${why}`, fmax });
    if (feeds !== t.feeds || feedsDi !== t.feedsDi || luts !== t.luts)
        return fail(`expected feed-throughs ${t.feeds} (DI ${t.feedsDi}), ${t.luts} LUT cells`);
    if (sites * 100 > t.vivado * 115) return fail("more than 15 % over Vivado's");
    if (!(+fmax >= t.floor)) return fail(`below ${t.floor} MHz`);
    if (warnings.length) return fail(warnings.slice(0, 5).join('; '));
    // (the frequency is not compared with the native build's: WebAssembly's placement differs from it with the same
    // seed, blinky 287.85 against 280.19 MHz; its floor holds)
    return { ok: true, message: `${line} (native ${t.native} MHz)`, fmax };
}
