// The browser test in headless Chromium (Playwright): serve.mjs serves index.html, the package and the test netlists;
// the page's worker runs nextpnr and window.result says whether every netlist gave the native frequency.
//   node test-browser.mjs <package dir>      (Playwright installed beside this script: npm install playwright)
import { spawn } from 'node:child_process';
import path from 'node:path';
import { chromium } from 'playwright';

const here = path.dirname(new URL(import.meta.url).pathname);
const port = 8790 + Math.floor(Math.random() * 1000);
const server = spawn(process.execPath, [path.join(here, 'serve.mjs'), path.resolve(process.argv[2]), String(port)], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise(resolve => server.stdout.once('data', resolve));
const browser = await chromium.launch();
let ok = false;
try {
  const page = await browser.newPage();
  page.on('pageerror', e => console.log(`page error: ${e.message}`));
  await page.goto(`http://localhost:${port}/`);
  await page.waitForFunction(() => window.result?.done, null, { timeout: 180000 });
  const result = await page.evaluate(() => window.result);
  if (result.chipdb) console.log(`chip database ${result.chipdb.device}: ${(result.chipdb.gz / 1e6).toFixed(1)} MB fetched, ${(result.chipdb.bytes / 1e6).toFixed(1)} MB unpacked, ${result.chipdb.s} s`);
  for (const r of result.rows) console.log(`${r.ok ? 'ok  ' : 'FAIL'} ${r.netlist}: ${r.message} (native ${r.native} MHz, ${r.s} s)`);
  if (result.error) console.log(`FAIL ${result.error}`);
  ok = result.ok && result.rows.length > 0;
} finally {
  await browser.close();
  server.kill();
}
process.exit(ok ? 0 : 1);
