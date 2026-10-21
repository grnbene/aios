// Drive the real v8 client the way its users do: log in on one tenant, switch
// the board to another with the same selector the top-left menu uses, and
// read what the server said — never printing a key.
//
//   node drive-sandbox.mjs cards                 login, switch, read the OMS board
//   node drive-sandbox.mjs seed <truck> <n>      put the first n geocoded open orders on a truck column
//   node drive-sandbox.mjs recalc <truck>        re-sort that truck (exact through aios when small)
//   node drive-sandbox.mjs full <truck> <n>      cards → seed → recalc → cards → dashboard screenshot
//
// env: CLIENT_URL (http://127.0.0.1:4200)  V8_USER/V8_PASS (JACK/pudpud)
//      TENANT (moov_sandbox — the config to switch to after login; '' = stay)
//      OUT_DIR (/tmp) for screenshots
import { createRequire } from 'module';
const { chromium } = createRequire('/repos/v8/client/package.json')('playwright');

const BASE = process.env.CLIENT_URL || 'http://127.0.0.1:4200';
const USER = process.env.V8_USER || 'JACK';
const PASS = process.env.V8_PASS || 'pudpud';
const TENANT = process.env.TENANT === undefined ? 'moov_sandbox' : process.env.TENANT;
const OUT = process.env.OUT_DIR || '/tmp';

const [, , cmd = 'cards', arg1, arg2] = process.argv;

const browser = await chromium.launch({ headless: true });
const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
page.on('pageerror', e => console.log('pageerror:', String(e).slice(0, 160)));
page.on('response', async r => {
  if (/route=apir\/login/.test(r.url())) { let t = ''; try { t = (await r.text()).slice(0, 200); } catch {} console.log('login response', r.status(), t.replace(/"(key|apikey|token|password)":"[^"]*"/g, '"$1":"…"')); }
});

async function login() {
  await page.goto(BASE + '/', { waitUntil: 'domcontentloaded', timeout: 120000 });
  await page.waitForSelector('#username', { timeout: 120000 });
  await page.fill('#username', USER);
  await page.fill('#password', PASS);
  await Promise.all([
    page.waitForFunction(() => !!localStorage.getItem('user'), null, { timeout: 60000 }).catch(() => {}),
    page.click('button[type=submit]'),
  ]);
  const who = await page.evaluate(() => { try { return JSON.parse(localStorage.getItem('user') || 'null')?.user?.userid || null; } catch { return null; } });
  if (!who) {
    const txt = (await page.textContent('body').catch(() => '')) || '';
    await page.screenshot({ path: `${OUT}/login-failed.png` });
    throw new Error('login did not produce a user; page says: ' + txt.replace(/\s+/g, ' ').slice(0, 200));
  }
  const client = await page.evaluate(() => { try { return JSON.parse(localStorage.getItem('v8ConfigOverride') || 'null')?.env?.client || '(build default)'; } catch { return '?'; } });
  console.log('logged in as', who, 'on tenant', client);
}

// What the top-left selector does: store the chosen config and its name, reload.
async function switchTenant(name) {
  if (!name) return;
  await page.evaluate(async (name) => {
    const cfg = await (await fetch('/assets/config/config.' + name + '.json')).json();
    localStorage.setItem('v8ConfigOverride', JSON.stringify(cfg));
    localStorage.setItem('v8ActiveSetupKey', name);
  }, name);
  await page.reload({ waitUntil: 'domcontentloaded' });
  await page.waitForTimeout(1500);
  const client = await page.evaluate(() => JSON.parse(localStorage.getItem('v8ConfigOverride')).env.client);
  const still = await page.evaluate(() => !!localStorage.getItem('user'));
  console.log('switched tenant to', client, still ? '(session kept)' : '(session LOST)');
}

// Call the API exactly as the app does — same base, same headers, same key —
// from inside the page so nothing secret leaves the browser.
async function api(method, route, body) {
  return await page.evaluate(async ({ method, route, body }) => {
    const cfg = JSON.parse(localStorage.getItem('v8ConfigOverride'));
    const user = JSON.parse(localStorage.getItem('user'));
    const key = cfg.apiCredentials.clientId;
    const url = cfg.apiServer.endpoint + route + '&apikey=' + encodeURIComponent(key) + '&userid=' + encodeURIComponent(user.user.userid);
    const headers = { 'Authorization': 'Bearer ' + key + '_' + user.user.userid, 'X-Device-UUID': 'drive-' + Math.random().toString(16).slice(2) };
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    const r = await fetch(url, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
    const text = await r.text();
    let json = null; try { json = JSON.parse(text); } catch {}
    return { status: r.status, json, text: json ? null : text.slice(0, 300) };
  }, { method, route, body });
}

function summarizeCards(res) {
  const j = res.json || {};
  const orders = Array.isArray(j.orders) ? j.orders : Object.values(j.orders || {});
  const withCoords = orders.filter(o => Number(o.lat) !== 0 || Number(o.lng) !== 0);
  console.log('oms/cards', res.status, '| open orders in pool:', orders.length, '| geocoded:', withCoords.length);
  console.log('aios (pool):', JSON.stringify(j.aios || null));
  const cols = Array.isArray(j.columns) ? j.columns : Object.values(j.columns || {});
  console.log('truck columns:', JSON.stringify(cols.map(c => ({ id: c.id ?? c.truck_id, name: c.name ?? c.truck_name })).slice(0, 12)));
  for (const [col, rows] of Object.entries(j.current || {})) if (Array.isArray(rows) && rows.length) console.log(`  column ${col}: ${rows.length} card(s)`);
  if (!res.json) console.log('raw:', res.text);
  return { orders: withCoords, columns: cols, current: j.current || {} };
}

async function seed(column, n) {
  const { orders } = summarizeCards(await api('GET', 'oms/cards'));
  const rows = orders.slice(0, n);
  console.log(`seeding truck ${column} with ${rows.length} orders:`, rows.map(r => r.order ?? r.orderno).join(','));
  const userid = await page.evaluate(() => JSON.parse(localStorage.getItem('user')).user.userid);
  // The same request the socket relay forwards for a drop on a truck column.
  const r = await api('POST', 'oms/cardaction', { cards: { [column]: rows }, user: userid, devices: {} });
  console.log('cardaction', r.status, JSON.stringify(r.json ?? r.text).slice(0, 200));
}

async function recalc(column) {
  const t0 = Date.now();
  const r = await api('GET', 'oms/recalc&column=' + encodeURIComponent(column));
  const rows = Array.isArray(r.json) ? r.json : [];
  console.log(`oms/recalc column=${column}`, r.status, rows.length, 'rows in', Date.now() - t0, 'ms');
  for (const row of rows) console.log(`  #${row.sort_value ?? row.sort}  order=${row.order ?? row.orderno}  engine=${row.engine ?? '-'}  ${String(row.address || '').replace(/<br\s*\/?>/g, ' | ').slice(0, 64)}`);
  if (!rows.length) console.log('raw:', r.text || JSON.stringify(r.json).slice(0, 300));
}

async function board(name) {
  await page.goto(BASE + '/dispatches/dashboard', { waitUntil: 'domcontentloaded', timeout: 120000 }).catch(() => {});
  await page.waitForTimeout(6000);
  const out = `${OUT}/${name}`;
  await page.screenshot({ path: out, fullPage: false });
  console.log('screenshot', out);
}

try {
  await login();
  await switchTenant(TENANT);
  if (cmd === 'cards') summarizeCards(await api('GET', 'oms/cards'));
  else if (cmd === 'seed') await seed(String(arg1), Number(arg2 || 10));
  else if (cmd === 'recalc') await recalc(String(arg1));
  else if (cmd === 'full') {
    await seed(String(arg1), Number(arg2 || 10));
    await recalc(String(arg1));
    summarizeCards(await api('GET', 'oms/cards'));
    await board('dashboard.png');
  }
} catch (e) {
  console.log('DRIVE FAILED:', e.message);
  process.exitCode = 1;
} finally {
  await browser.close();
}
