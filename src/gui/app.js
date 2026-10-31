'use strict';
// PHYLKA wallet UI. Plain JavaScript, no libraries, no network access except the same-origin JSON API.
// The session token comes in the URL fragment (never sent to the server by the browser) and lives only in this closure.
(() => {
const TOKEN = ((location.hash.match(/[#&]t=([0-9a-f]{64})/) || [])[1]) || '';
const IDLE_MS = 10 * 60 * 1000;
const DISCLAIMER_URL = 'https://github.com/a9ish1/PHYLKA/blob/main/DISCLAIMER.md';
const $app = document.getElementById('app');

// ------------------------------------------------------------------ helpers

function h(tag, props, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v === true) e.setAttribute(k, '');
    else if (v !== false && v != null) e.setAttribute(k, v);
  }
  for (const c of kids.flat(2)) {
    if (c === null || c === undefined || c === false) continue;
    e.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return e;
}
const clear = (el) => { while (el.firstChild) el.removeChild(el.firstChild); return el; };
// Empties `el`; its append() takes nested arrays and skips null/false, like h()
const fresh = (el) => ({
  append(...kids) {
    clear(el);
    for (const c of kids.flat(3)) if (c !== null && c !== undefined && c !== false) el.append(c.nodeType ? c : document.createTextNode(String(c)));
    return el;
  },
});

async function api(method, path, body) {
  const res = await fetch(path, {
    method,
    headers: Object.assign({ Authorization: 'Bearer ' + TOKEN }, body ? { 'Content-Type': 'application/json' } : {}),
    body: body ? JSON.stringify(body) : undefined,
    cache: 'no-store',
    credentials: 'omit',
    referrerPolicy: 'no-referrer',
  });
  let j = {};
  try { j = await res.json(); } catch (_) { /* not JSON */ }
  if (!res.ok) {
    const e = new Error(j.error || 'Request failed (' + res.status + ')');
    e.code = j.code || '';
    e.status = res.status;
    throw e;
  }
  return j;
}
const get = (p) => api('GET', p);
const post = (p, b) => api('POST', p, b || {});

const fmt = (f) => {
  const w = Math.floor(f / 1e8), fr = String(Math.abs(f) % 1e8).padStart(8, '0').replace(/0+$/, '');
  return w.toLocaleString('en-US') + (fr ? '.' + fr : '');
};
const plk = (f) => fmt(f) + ' PLK';
const when = (t) => (t ? new Date(t * 1000).toLocaleString() : 'pending');
const short = (s, a = 8, b = 6) => (s.length > a + b + 1 ? s.slice(0, a) + '…' + s.slice(-b) : s);
const ageText = (s) => (s < 90 ? s + ' s' : s < 5400 ? Math.round(s / 60) + ' min' : s < 172800 ? Math.round(s / 3600) + ' h' : Math.round(s / 86400) + ' days');
const safeExplorer = (u) => (typeof u === 'string' && /^http:\/\/127\.0\.0\.1:\d+\/$/.test(u) ? u : null);

async function copyText(text, btn) {
  const old = btn.textContent;
  try { await navigator.clipboard.writeText(text); btn.textContent = 'Copied'; }
  catch (_) { btn.textContent = 'Copy failed - select the text'; }
  setTimeout(() => { btn.textContent = old; }, 1600);
}
const copyBtn = (text, label) => { const b = h('button', { class: 'btn small', type: 'button' }, label || 'Copy'); b.addEventListener('click', () => copyText(text, b)); return b; };

async function busy(btn, fn, errBox) {
  const old = btn.textContent;
  btn.disabled = true;
  btn.textContent = '';
  btn.append(h('span', { class: 'spin' }), 'Working…');
  if (errBox) errBox.textContent = '';
  try { return await fn(); }
  catch (e) { if (errBox) errBox.textContent = e.message; else throw e; }
  finally { btn.disabled = false; btn.textContent = old; }
}

// ------------------------------------------------------------------ QR code (byte mode, error correction M, versions 1-6)
// Written for this wallet after ISO/IEC 18004; renders offline so the address never leaves the machine.

const QR = (() => {
  const DATA_CW = [0, 16, 28, 44, 64, 86, 108];                       // data codewords at level M
  const ECC_PER_BLOCK = [0, 10, 16, 26, 18, 24, 16];
  const BLOCKS = [0, 1, 1, 1, 2, 2, 4];
  const ALIGN = [[], [], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34]];
  const mulGf = (x, y) => { let z = 0; for (let i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >>> 7) * 0x11D); z ^= ((y >>> i) & 1) * x; } return z; };
  function rsDivisor(deg) {
    const r = new Array(deg).fill(0); r[deg - 1] = 1;
    let root = 1;
    for (let i = 0; i < deg; i++) {
      for (let j = 0; j < r.length; j++) { r[j] = mulGf(r[j], root); if (j + 1 < r.length) r[j] ^= r[j + 1]; }
      root = mulGf(root, 2);
    }
    return r;
  }
  function rsRemainder(data, div) {
    const r = new Array(div.length).fill(0);
    for (const b of data) { const f = b ^ r.shift(); r.push(0); div.forEach((c, i) => { r[i] ^= mulGf(c, f); }); }
    return r;
  }
  const rawModules = (v) => { let r = (16 * v + 128) * v + 64; if (v >= 2) { const n = Math.floor(v / 7) + 2; r -= (25 * n - 10) * n - 55; } return r; };

  function codewords(bytes, ver) {
    const bits = [];
    const put = (val, n) => { for (let i = n - 1; i >= 0; i--) bits.push((val >>> i) & 1); };
    put(4, 4); put(bytes.length, 8); bytes.forEach((b) => put(b, 8));
    const cap = DATA_CW[ver] * 8;
    put(0, Math.min(4, cap - bits.length));
    while (bits.length % 8) bits.push(0);
    for (let pad = 0xEC; bits.length < cap; pad ^= 0xEC ^ 0x11) put(pad, 8);
    const data = [];
    for (let i = 0; i < bits.length; i += 8) data.push(parseInt(bits.slice(i, i + 8).join(''), 2));
    const nb = BLOCKS[ver], eb = ECC_PER_BLOCK[ver], raw = Math.floor(rawModules(ver) / 8);
    const shortN = nb - (raw % nb), shortLen = Math.floor(raw / nb), div = rsDivisor(eb);
    const blocks = [];
    for (let i = 0, k = 0; i < nb; i++) {
      const dat = data.slice(k, k + shortLen - eb + (i < shortN ? 0 : 1)); k += dat.length;
      const ecc = rsRemainder(dat, div);
      if (i < shortN) dat.push(0);
      blocks.push(dat.concat(ecc));
    }
    const out = [];
    for (let i = 0; i < blocks[0].length; i++) blocks.forEach((b, j) => { if (i !== shortLen - eb || j >= shortN) out.push(b[i]); });
    return out;
  }

  function build(cw, ver) {
    const size = ver * 4 + 17;
    const m = Array.from({ length: size }, () => new Array(size).fill(false));
    const fn = Array.from({ length: size }, () => new Array(size).fill(false));
    const set = (x, y, d) => { m[y][x] = d; fn[y][x] = true; };
    for (let i = 0; i < size; i++) { set(6, i, i % 2 === 0); set(i, 6, i % 2 === 0); }
    for (const [cx, cy] of [[3, 3], [size - 4, 3], [3, size - 4]])
      for (let dy = -4; dy <= 4; dy++) for (let dx = -4; dx <= 4; dx++) {
        const x = cx + dx, y = cy + dy, d = Math.max(Math.abs(dx), Math.abs(dy));
        if (x >= 0 && x < size && y >= 0 && y < size) set(x, y, d !== 2 && d !== 4);
      }
    const al = ALIGN[ver];
    al.forEach((ax, i) => al.forEach((ay, j) => {
      if ((i === 0 && j === 0) || (i === 0 && j === al.length - 1) || (i === al.length - 1 && j === 0)) return;
      for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) set(ax + dx, ay + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);
    }));
    drawFormat(m, fn, size, 0);   // reserve the format areas
    let i = 0;
    for (let right = size - 1; right >= 1; right -= 2) {
      if (right === 6) right = 5;
      for (let vert = 0; vert < size; vert++) for (let j = 0; j < 2; j++) {
        const x = right - j, up = ((right + 1) & 2) === 0, y = up ? size - 1 - vert : vert;
        if (!fn[y][x] && i < cw.length * 8) { m[y][x] = ((cw[i >>> 3] >>> (7 - (i & 7))) & 1) !== 0; i++; }
      }
    }
    return { m, fn, size };
  }
  function drawFormat(m, fn, size, mask) {
    const data = (0 << 3) | mask;   // level M = 0b00
    let rem = data;
    for (let i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
    const bits = ((data << 10) | rem) ^ 0x5412;
    const bit = (i) => ((bits >>> i) & 1) !== 0;
    const set = (x, y, d) => { m[y][x] = d; fn[y][x] = true; };
    for (let i = 0; i <= 5; i++) set(8, i, bit(i));
    set(8, 7, bit(6)); set(8, 8, bit(7)); set(7, 8, bit(8));
    for (let i = 9; i < 15; i++) set(14 - i, 8, bit(i));
    for (let i = 0; i < 8; i++) set(size - 1 - i, 8, bit(i));
    for (let i = 8; i < 15; i++) set(8, size - 15 + i, bit(i));
    set(8, size - 8, true);
  }
  const MASKS = [
    (x, y) => (x + y) % 2 === 0, (x, y) => y % 2 === 0, (x) => x % 3 === 0, (x, y) => (x + y) % 3 === 0,
    (x, y) => (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0, (x, y) => ((x * y) % 2) + ((x * y) % 3) === 0,
    (x, y) => (((x * y) % 2) + ((x * y) % 3)) % 2 === 0, (x, y) => (((x + y) % 2) + ((x * y) % 3)) % 2 === 0,
  ];
  function penalty(m, size) {
    let p = 0, dark = 0;
    const lines = [];
    for (let a = 0; a < size; a++) { lines.push(m[a].slice()); lines.push(m.map((row) => row[a])); }
    for (const line of lines) {
      let run = 1;
      for (let i = 1; i <= size; i++) {
        if (i < size && line[i] === line[i - 1]) run++;
        else { if (run >= 5) p += 3 + run - 5; run = 1; }
      }
      const s = line.map((v) => (v ? '1' : '0')).join('');
      for (const pat of ['10111010000', '00001011101']) for (let k = s.indexOf(pat); k >= 0; k = s.indexOf(pat, k + 1)) p += 40;
    }
    for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
      if (m[y][x]) dark++;
      if (x < size - 1 && y < size - 1 && m[y][x] === m[y][x + 1] && m[y][x] === m[y + 1][x] && m[y][x] === m[y + 1][x + 1]) p += 3;
    }
    const total = size * size;
    p += (Math.ceil(Math.abs(dark * 20 - total * 10) / total) - 1) * 10;
    return p;
  }
  function encode(text) {
    const bytes = Array.from(new TextEncoder().encode(text));
    let ver = 1;
    while (ver <= 6 && bytes.length + 2 > DATA_CW[ver]) ver++;
    if (ver > 6) throw new Error('text too long for the QR code');
    const base = build(codewords(bytes, ver), ver);
    let best = null, bestP = Infinity;
    for (let mask = 0; mask < 8; mask++) {
      const m = base.m.map((r) => r.slice());
      for (let y = 0; y < base.size; y++) for (let x = 0; x < base.size; x++) if (!base.fn[y][x] && MASKS[mask](x, y)) m[y][x] = !m[y][x];
      drawFormat(m, base.fn.map((r) => r.slice()), base.size, mask);
      const p = penalty(m, base.size);
      if (p < bestP) { bestP = p; best = m; }
    }
    return best;
  }
  function draw(canvas, text) {
    const m = encode(text), n = m.length, quiet = 2, scale = 6;
    canvas.width = canvas.height = (n + 2 * quiet) * scale;
    const c = canvas.getContext('2d');
    c.fillStyle = '#fff'; c.fillRect(0, 0, canvas.width, canvas.height);
    c.fillStyle = '#000';
    m.forEach((row, y) => row.forEach((d, x) => { if (d) c.fillRect((x + quiet) * scale, (y + quiet) * scale, scale, scale); }));
  }
  return { encode, draw };
})();

// ------------------------------------------------------------------ app state

let state = null;       // /api/state
let ov = null;          // latest /api/overview
let view = 'dashboard';
let page = null;        // { refresh(ov), dispose() } of the open page
let mainEl = null, bannerEl = null, navEl = null;
let pollTimer = null, idleTimer = null, locked = false;

const NAV = [
  ['dashboard', 'Overview', '▦'], ['send', 'Send', '↗'], ['receive', 'Receive', '↙'],
  ['history', 'History', '☰'], ['staking', 'Staking', '◈'], ['settings', 'Settings', '⚙'],
];

function errBox() { return h('div', { class: 'err', role: 'alert' }); }
function note(kind, ...kids) { return h('div', { class: 'banner ' + kind }, kids); }
const netBadge = () => h('span', { class: 'badge ' + (state.network === 'dev' ? 'dev' : '') }, state.network);

// ------------------------------------------------------------------ onboarding

function onboarding() {
  stopTimers();
  const wrap = h('div', { class: 'onb' });
  fresh($app).append(wrap);
  const logo = () => h('div', { class: 'logo' }, 'PHYL', h('span', null, 'KA'));
  const pwFields = () => [h('input', { type: 'password', id: 'p1', autocomplete: 'new-password' }), h('input', { type: 'password', id: 'p2', autocomplete: 'new-password' })];
  const pwCheck = (p1, p2, err) => {
    if (p1.value.length < state.min_password) { err.textContent = 'The password must have at least ' + state.min_password + ' characters.'; return false; }
    if (p1.value !== p2.value) { err.textContent = 'The two passwords do not match.'; return false; }
    return true;
  };

  function choose() {
    fresh(wrap).append(logo(), h('h1', { class: 'big' }, 'Welcome'),
      h('p', { class: 'dim' }, 'Your wallet is an encrypted file on this computer: ', h('span', { class: 'mono' }, state.wallet_path)),
      h('p', null, 'Network: ', netBadge()),
      note('warn', h('b', null, 'Beta software. '), 'PHYLKA is a public beta. Use it only with amounts you can afford to lose.'),
      h('div', { class: 'card' }, h('h2', null, 'Create a new wallet'), h('p', { class: 'dim' }, 'You will get 24 recovery words. They are the only way to get your coins back.'),
        h('button', { class: 'btn primary', onclick: createIntro }, 'Create new wallet')),
      h('div', { class: 'card' }, h('h2', null, 'Restore from 24 words'), h('p', { class: 'dim' }, 'Use the recovery phrase of an existing wallet.'),
        h('button', { class: 'btn', onclick: restore }, 'Restore wallet')));
  }

  function createIntro() {
    const err = errBox();
    const go = h('button', { class: 'btn primary' }, 'Show my recovery words');
    go.addEventListener('click', () => busy(go, async () => { createWords(await post('/api/wallet/generate')); }, err));
    fresh(wrap).append(logo(), h('h1', { class: 'big' }, 'Before you start'),
      note('bad', h('b', null, 'Write the words on paper. '), 'Anyone who sees your 24 words can take all your coins, and nobody can restore them for you if you lose them. Do not take a screenshot, do not photograph them and do not store them in a cloud.'),
      h('p', null, 'Make sure nobody is watching your screen.'), h('div', { class: 'actions' }, go, h('button', { class: 'btn', onclick: choose }, 'Back')), err);
  }

  function createWords(g) {
    const words = g.words.slice();
    const sure = h('input', { type: 'checkbox', id: 'sure' });
    const next = h('button', { class: 'btn primary', disabled: true }, 'Continue');
    sure.addEventListener('change', () => { next.disabled = !sure.checked; });
    next.addEventListener('click', () => createConfirm(g, words));
    fresh(wrap).append(logo(), h('h1', { class: 'big' }, 'Your recovery words'),
      note('bad', h('b', null, 'Write these 24 words on paper, in this order. '), 'They are shown only once. Never share them and never type them into any website.'),
      h('ol', { class: 'words' }, words.map((w) => h('li', null, w))),
      h('label', { class: 'check', for: 'sure' }, sure, 'I have written all 24 words on paper and stored them safely.'),
      h('div', { class: 'actions' }, next, h('button', { class: 'btn', onclick: () => { words.fill(''); choose(); } }, 'Cancel')));
  }

  function createConfirm(g, words) {
    const err = errBox();
    const ans = g.challenge.map((n) => h('input', { type: 'text', autocomplete: 'off', spellcheck: 'false', autocapitalize: 'none', 'aria-label': 'Word ' + n }));
    const [p1, p2] = pwFields();
    const go = h('button', { class: 'btn primary' }, 'Create wallet');
    go.addEventListener('click', () => {
      if (!pwCheck(p1, p2, err)) return;
      busy(go, async () => {
        const r = await post('/api/wallet/create', { password: p1.value, answers: ans.map((a) => a.value.trim().toLowerCase()) });
        words.fill(''); p1.value = p2.value = ''; ans.forEach((a) => { a.value = ''; });
        state.wallet_exists = true;
        finished(r.address);
      }, err);
    });
    fresh(wrap).append(logo(), h('h1', { class: 'big' }, 'Check your paper copy'),
      h('p', { class: 'dim' }, 'Type these words from your paper copy so we know you wrote them down correctly.'),
      g.challenge.map((n, i) => [h('label', null, 'Word #' + n), ans[i]]),
      h('h2', null, 'Choose a password'),
      h('p', { class: 'dim' }, 'It encrypts the wallet file on this computer and is asked for every payment. It cannot be recovered - your 24 words can.'),
      h('label', { for: 'p1' }, 'Password (at least ' + state.min_password + ' characters)'), p1, h('label', { for: 'p2' }, 'Repeat password'), p2, err,
      h('div', { class: 'actions' }, go, h('button', { class: 'btn', onclick: () => createWords(g) }, 'Back to the words')));
  }

  function restore() {
    const err = errBox();
    const phrase = h('textarea', { id: 'ph', autocomplete: 'off', spellcheck: 'false', placeholder: 'word1 word2 word3 … word24' });
    const pass = h('input', { type: 'password', id: 'bp', autocomplete: 'off' });
    const [p1, p2] = pwFields();
    const go = h('button', { class: 'btn primary' }, 'Restore wallet');
    go.addEventListener('click', () => {
      if (!pwCheck(p1, p2, err)) return;
      busy(go, async () => {
        const r = await post('/api/wallet/restore', { mnemonic: phrase.value, passphrase: pass.value, password: p1.value });
        phrase.value = pass.value = p1.value = p2.value = '';
        state.wallet_exists = true;
        finished(r.address);
      }, err);
    });
    fresh(wrap).append(logo(), h('h1', { class: 'big' }, 'Restore a wallet'),
      h('label', { for: 'ph' }, 'Recovery phrase (24 words)'), phrase,
      h('label', { for: 'bp' }, 'BIP39 passphrase (optional)'), pass, h('div', { class: 'hint' }, 'Leave empty unless you set one when you created the wallet.'),
      h('h2', null, 'Choose a password'), h('div', { class: 'hint' }, 'Encrypts the wallet file on this computer.'),
      h('label', { for: 'p1' }, 'Password (at least ' + state.min_password + ' characters)'), p1, h('label', { for: 'p2' }, 'Repeat password'), p2, err,
      h('div', { class: 'actions' }, go, h('button', { class: 'btn', onclick: choose }, 'Back')));
  }

  function finished(address) {
    fresh(wrap).append(logo(), h('h1', { class: 'big' }, 'Wallet ready'), h('p', null, 'Your first receiving address:'), h('div', { class: 'addr' }, address),
      h('div', { class: 'actions' }, h('button', { class: 'btn primary', onclick: startApp }, 'Open wallet')));
  }
  choose();
}

// ------------------------------------------------------------------ shell

function stopTimers() { clearInterval(pollTimer); clearTimeout(idleTimer); pollTimer = idleTimer = null; }

function startApp() {
  clear($app);
  mainEl = h('main', { class: 'main', id: 'main' });
  bannerEl = h('div', { id: 'banner' });
  navEl = h('nav', { class: 'nav', 'aria-label': 'Wallet sections' }, NAV.map(([id, label, icon]) =>
    h('button', { type: 'button', 'data-view': id, onclick: () => go(id) }, h('span', { 'aria-hidden': 'true' }, icon), label)));
  const lockBtn = h('button', { class: 'btn small lockbtn', type: 'button', onclick: lock }, 'Lock');
  $app.append(h('div', { class: 'shell' },
    h('aside', { class: 'side' }, h('div', { class: 'logo' }, 'PHYL', h('span', null, 'KA')), navEl, h('div', { class: 'spacer' }), lockBtn),
    h('div', null, mainEl)));
  for (const ev of ['mousemove', 'keydown', 'click', 'touchstart', 'scroll']) window.addEventListener(ev, bumpIdle, { passive: true });
  bumpIdle();
  poll().then(() => go(view));
  pollTimer = setInterval(poll, 5000);
}

async function poll() {
  if (document.hidden || locked) return;
  try { ov = await get('/api/overview'); } catch (e) { ov = { node: { running: false, code: 'ui', message: e.message }, wallet: { exists: true } }; }
  renderBanner();
  if (page && page.refresh) page.refresh(ov);
}

function renderBanner() {
  if (!bannerEl) return;
  clear(bannerEl);
  const n = ov && ov.node;
  if (!n) return;
  const cmd = 'phylka node' + (state.network === 'dev' ? ' --dev' : '') + (state.rpc_port ? ' --rpc ' + state.rpc_port : '') + ' ...';
  if (!n.running && n.code === 'node_down')
    bannerEl.append(note('bad', h('b', null, 'The node is not running. '), 'Balances and payments need it. Start it with: ', h('code', null, cmd)));
  else if (!n.running)
    bannerEl.append(note('bad', h('b', null, 'Node problem: '), n.message || 'unknown error'));
  else if (!n.synced)
    bannerEl.append(note('warn', h('b', null, 'The node may still be syncing. '), 'Its newest block is ' + ageText(n.tip_age) + ' old, so balances can be out of date.'));
}

function go(v) {
  if (page && page.dispose) page.dispose();
  page = null;
  view = v;
  for (const b of navEl.children) { if (b.dataset.view === v) b.setAttribute('aria-current', 'page'); else b.removeAttribute('aria-current'); }
  clear(mainEl);
  const title = NAV.find((x) => x[0] === v)[1];
  mainEl.append(h('div', { class: 'top' }, h('h1', null, title), netBadge()), bannerEl);
  const body = h('div');
  mainEl.append(body, h('div', { class: 'foot' }, 'PHYLKA is public beta software. ', h('a', { href: DISCLAIMER_URL, target: '_blank', rel: 'noopener noreferrer' }, 'Read the disclaimer'), '.'));
  page = PAGES[v](body) || null;
  if (page && page.refresh && ov) page.refresh(ov);
  window.scrollTo(0, 0);
}

// ------------------------------------------------------------------ idle lock

function bumpIdle() {
  if (locked) return;
  clearTimeout(idleTimer);
  idleTimer = setTimeout(lock, IDLE_MS);
}

function lock() {
  if (locked) return;
  locked = true;
  clearTimeout(idleTimer);
  if (page && page.dispose) page.dispose();   // hides anything sensitive (for instance a shown recovery phrase)
  document.body.classList.add('locked');
  const pw = h('input', { type: 'password', id: 'lockpw', autocomplete: 'off', 'aria-label': 'Wallet password' });
  const err = errBox();
  const btn = h('button', { class: 'btn primary', type: 'submit' }, 'Unlock');
  const form = h('form', null, h('h2', null, 'Wallet locked'), h('p', { class: 'dim' }, 'Locked after 10 minutes without activity (or by you). Enter your password to continue.'), pw, err, h('div', { class: 'actions' }, btn));
  const overlay = h('div', { class: 'lock', role: 'dialog', 'aria-modal': 'true', 'aria-label': 'Wallet locked' }, h('div', { class: 'card' }, form));
  form.addEventListener('submit', (e) => {
    e.preventDefault();
    busy(btn, async () => {
      await post('/api/unlock', { password: pw.value });
      pw.value = '';
      overlay.remove();
      document.body.classList.remove('locked');
      locked = false;
      bumpIdle();
      go(view);
      poll();
    }, err);
  });
  document.body.append(overlay);
  pw.focus();
}

// ------------------------------------------------------------------ pages

const PAGES = {};

PAGES.dashboard = (root) => {
  const cards = h('div', { class: 'grid' }), info = h('div', { class: 'card' });
  root.append(cards, info);
  const stat = (k, v, s, cls) => h('div', { class: 'card stat ' + (cls || '') }, h('div', { class: 'k' }, k), h('div', { class: 'v' }, v), s ? h('div', { class: 's' }, s) : null);
  return {
    refresh(o) {
      clear(cards); clear(info);
      const b = o.wallet && o.wallet.balance;
      cards.append(
        stat('Available', b ? plk(b.spendable) : '—', 'Ready to spend', 'hero'),
        stat('Maturing', b ? plk(b.immature) : '—', 'Mined coins wait ' + state.coinbase_maturity + ' blocks'),
        stat('Staked', b ? plk(b.staked) : '—', b && b.staked ? 'Withdrawable now: ' + plk(b.unlockable) : 'Locked as validator stake'));
      const n = o.node;
      info.append(h('h2', null, 'Node'));
      if (!n.running) { info.append(h('p', { class: 'dim' }, 'No data - the node is not reachable.')); return; }
      info.append(h('dl', { class: 'kv' },
        h('dt', null, 'Network'), h('dd', null, n.network),
        h('dt', null, 'Block height'), h('dd', null, n.height.toLocaleString('en-US')),
        h('dt', null, 'Peers'), h('dd', null, String(n.peers)),
        h('dt', null, 'Synced'), h('dd', null, n.synced ? 'Yes' : 'No - newest block is ' + ageText(n.tip_age) + ' old'),
        h('dt', null, 'Finalized height'), h('dd', null, n.finalized ? n.finalized.toLocaleString('en-US') : 'none yet'),
        h('dt', null, 'Mempool'), h('dd', null, n.mempool + ' transaction(s)')));
    },
  };
};

PAGES.receive = (root) => {
  const box = h('div');
  root.append(box);
  const err = errBox();
  let last = '';
  function draw(o) {
    const w = o.wallet;
    if (!w || !w.exists) return;
    const sig = w.addresses.map((a) => a.address + ':' + a.balance).join('|');
    if (sig === last) return;
    last = sig;
    const canvas = h('canvas', { 'aria-label': 'QR code of your address', role: 'img' });
    try { QR.draw(canvas, w.address); } catch (_) { canvas.remove(); }
    const pw = h('input', { type: 'password', id: 'npw', autocomplete: 'off', placeholder: 'Wallet password', 'aria-label': 'Wallet password' });
    const nb = h('button', { class: 'btn' }, 'New address');
    nb.addEventListener('click', () => busy(nb, async () => { await post('/api/address/new', { password: pw.value }); pw.value = ''; last = ''; await poll(); }, err));
    fresh(box).append(
      h('div', { class: 'card' }, h('div', { class: 'recv' }, h('div', { class: 'qr' }, canvas),
        h('div', null, h('h2', null, 'Your receiving address'), h('div', { class: 'addr' }, w.address),
          h('div', { class: 'actions' }, copyBtn(w.address, 'Copy address')),
          h('p', { class: 'hint' }, 'Only send ' + state.network + '-network PLK to this address. Addresses start with ' + state.hrp + '1.')))),
      h('div', { class: 'card' }, h('h2', null, 'Need a fresh address?'), h('p', { class: 'dim' }, 'Using a new address for each payer improves privacy. Old addresses keep working. Creating one needs your password.'),
        h('div', { class: 'row' }, pw, nb), err),
      h('div', { class: 'card' }, h('h2', null, 'All addresses'), h('table', { class: 'tbl' }, h('thead', null, h('tr', null, h('th', null, '#'), h('th', null, 'Address'), h('th', { class: 'num' }, 'Balance'))),
        h('tbody', null, w.addresses.map((a) => h('tr', null, h('td', null, String(a.index)), h('td', { class: 'mono' }, a.address), h('td', { class: 'num' }, a.balance == null ? '—' : plk(a.balance))))))));
  }
  return { refresh: draw };
};

function reviewTable(p, toLabel) {
  return h('div', { class: 'review' },
    h('div', { class: 'to' }, h('span', { class: 'dim' }, toLabel || 'To'), h('span', { class: 'mono' }, p.to)),
    h('div', null, h('span', null, 'Amount'), h('b', null, plk(p.amount))),
    h('div', null, h('span', null, 'Network fee (' + p.inputs + ' input' + (p.inputs === 1 ? '' : 's') + ')'), h('span', null, plk(p.fee))),
    h('div', { class: 'total' }, h('span', null, p.kind === 'unstake' ? 'Spent from stake' : 'Total deducted'), h('span', null, plk(p.total))));
}

function txSent(holder, r, title, again) {
  const link = safeExplorer(r.explorer_url);
  fresh(holder).append(h('div', { class: 'card' }, h('h2', { class: 'pos' }, title), h('p', { class: 'dim' }, 'Transaction ID'), h('div', { class: 'addr' }, r.txid),
    h('p', { class: 'hint' }, 'It is in the mempool now and confirms when the next block is mined.'),
    h('div', { class: 'actions' }, copyBtn(r.txid, 'Copy transaction ID'),
      link ? h('a', { class: 'btn', href: link + '#/tx/' + r.txid, target: '_blank', rel: 'noopener noreferrer' }, 'Open in explorer') : null,
      h('button', { class: 'btn', onclick: again }, 'Done'))));
}

PAGES.send = (root) => {
  const holder = h('div');
  root.append(holder);
  let avail = null, max = false, valid = false, chkTimer = null;
  const to = h('input', { type: 'text', id: 'to', autocomplete: 'off', spellcheck: 'false', placeholder: state.hrp + '1…' });
  const amount = h('input', { type: 'text', id: 'amt', inputmode: 'decimal', autocomplete: 'off', placeholder: '0.0' });
  const toMsg = h('div', { class: 'hint' }), err = errBox(), availEl = h('div', { class: 'hint' });

  to.addEventListener('input', () => {
    clearTimeout(chkTimer);
    valid = false;
    toMsg.textContent = '';
    toMsg.className = 'hint';
    if (!to.value.trim()) return;
    chkTimer = setTimeout(async () => {
      try {
        const r = await get('/api/check?addr=' + encodeURIComponent(to.value.trim()));
        valid = r.valid;
        toMsg.textContent = r.valid ? 'Valid ' + state.network + ' address' : r.reason;
        toMsg.className = 'hint ' + (r.valid ? 'okmsg' : 'neg');
      } catch (_) { /* node errors are shown elsewhere */ }
    }, 250);
  });
  amount.addEventListener('input', () => { max = false; });
  const maxBtn = h('button', { class: 'btn small', type: 'button', onclick: () => { max = true; amount.value = ''; amount.placeholder = 'Everything available, minus the fee'; } }, 'Max');
  const review = h('button', { class: 'btn primary' }, 'Review payment');
  review.addEventListener('click', () => busy(review, async () => {
    const chk = await get('/api/check?addr=' + encodeURIComponent(to.value.trim()));
    valid = chk.valid;
    if (!chk.valid) throw new Error(chk.reason);
    const body = { kind: 'send', to: to.value.trim(), max };
    if (!max) body.amount = amount.value.trim();
    confirmView(body, await post('/api/tx/preview', body));
  }, err));

  function formView() {
    fresh(holder).append(h('div', { class: 'card' },
      h('label', { for: 'to' }, 'Recipient address'), to, toMsg,
      h('label', { for: 'amt' }, 'Amount (PLK)'), h('div', { class: 'row' }, amount, maxBtn), availEl,
      h('div', { class: 'hint' }, 'The network fee is added on top and shown before you confirm.'), err,
      h('div', { class: 'actions' }, review)));
  }

  function confirmView(body, p) {
    const pw = h('input', { type: 'password', id: 'spw', autocomplete: 'off' });
    const e2 = errBox();
    const sendBtn = h('button', { class: 'btn primary' }, 'Sign and send');
    sendBtn.addEventListener('click', () => busy(sendBtn, async () => {
      const r = await post('/api/tx/send', Object.assign({}, body, { password: pw.value, expect_amount: p.amount, expect_fee: p.fee }));
      pw.value = '';
      txSent(holder, r, 'Payment sent', () => { to.value = amount.value = ''; valid = false; max = false; toMsg.textContent = ''; formView(); poll(); });
    }, e2));
    fresh(holder).append(h('div', { class: 'card' }, h('h2', null, 'Confirm payment'), reviewTable(p),
      h('p', { class: 'hint' }, 'Check the address carefully - payments cannot be reversed.'),
      h('label', { for: 'spw' }, 'Wallet password'), pw, e2,
      h('div', { class: 'actions' }, sendBtn, h('button', { class: 'btn', onclick: formView }, 'Back'))));
    pw.focus();
  }

  formView();
  return { refresh(o) { const b = o.wallet && o.wallet.balance; avail = b ? b.spendable : null; availEl.textContent = avail != null ? 'Available: ' + plk(avail) : ''; } };
};

PAGES.staking = (root) => {
  const holder = h('div');
  root.append(holder);
  let data = null, mode = 'view';

  async function load() {
    try { data = await get('/api/stakes'); } catch (e) { data = null; fresh(holder).append(note('warn', e.message)); return; }
    if (mode === 'view') render();
  }

  function render() {
    const err = errBox();
    const amount = h('input', { type: 'text', id: 'samt', inputmode: 'decimal', autocomplete: 'off', placeholder: fmt(data.min_stake) });
    const stakeBtn = h('button', { class: 'btn primary' }, 'Review stake');
    stakeBtn.addEventListener('click', () => busy(stakeBtn, async () => {
      const body = { kind: 'stake', amount: amount.value.trim() };
      confirmView(body, await post('/api/tx/preview', body), 'Stake coins');
    }, err));
    const out = data.unlockable > 0;
    const unBtn = h('button', { class: 'btn', disabled: !out }, 'Unstake ' + (out ? plk(data.unlockable) : ''));
    unBtn.addEventListener('click', () => busy(unBtn, async () => {
      const body = { kind: 'unstake' };
      confirmView(body, await post('/api/tx/preview', body), 'Unstake coins');
    }, err));
    fresh(holder).append(
      h('div', { class: 'grid' },
        h('div', { class: 'card stat' }, h('div', { class: 'k' }, 'Available'), h('div', { class: 'v' }, plk(data.spendable))),
        h('div', { class: 'card stat' }, h('div', { class: 'k' }, 'Staked'), h('div', { class: 'v' }, plk(data.staked))),
        h('div', { class: 'card stat' }, h('div', { class: 'k' }, 'Withdrawable now'), h('div', { class: 'v' }, plk(data.unlockable)))),
      h('div', { class: 'card' }, h('h2', null, 'Stake coins'),
        h('p', { class: 'dim' }, 'Staked coins back the network’s finality votes. They are locked for ' + data.stake_lock.toLocaleString('en-US') + ' blocks and count after ' + data.stake_activation.toLocaleString('en-US') +
          ' blocks. To vote, run your node with ', h('code', { class: 'code' }, '--validator'), '. Minimum: ' + plk(data.min_stake) + '.'),
        h('label', { for: 'samt' }, 'Amount (PLK)'), amount, err, h('div', { class: 'actions' }, stakeBtn)),
      h('div', { class: 'card' }, h('h2', null, 'Locked stakes'),
        data.stakes.length === 0 ? h('div', { class: 'empty' }, 'You have no stakes.') :
          h('table', { class: 'tbl' }, h('thead', null, h('tr', null, h('th', { class: 'num' }, 'Amount'), h('th', null, 'Unlocks at block'), h('th', null, 'Status'))),
            h('tbody', null, data.stakes.map((s) => h('tr', null, h('td', { class: 'num' }, plk(s.value)), h('td', null, s.unlock_height.toLocaleString('en-US')),
              h('td', null, s.unlockable ? h('span', { class: 'pos' }, 'Can be withdrawn') : 'Locked, ' + s.blocks_left.toLocaleString('en-US') + ' blocks left'))))),
        h('div', { class: 'actions' }, unBtn)));
  }

  function confirmView(body, p, title) {
    mode = 'confirm';
    const pw = h('input', { type: 'password', id: 'kpw', autocomplete: 'off' });
    const e2 = errBox();
    const ok = h('button', { class: 'btn primary' }, 'Sign and send');
    ok.addEventListener('click', () => busy(ok, async () => {
      const r = await post('/api/tx/send', Object.assign({}, body, { password: pw.value, expect_amount: p.amount, expect_fee: p.fee }));
      pw.value = '';
      mode = 'done';
      txSent(holder, r, 'Transaction sent', () => { mode = 'view'; load(); poll(); });
    }, e2));
    fresh(holder).append(h('div', { class: 'card' }, h('h2', null, title), reviewTable(p, body.kind === 'stake' ? 'Locked at your stake address' : 'Returned to'),
      h('label', { for: 'kpw' }, 'Wallet password'), pw, e2,
      h('div', { class: 'actions' }, ok, h('button', { class: 'btn', onclick: () => { mode = 'view'; render(); } }, 'Back'))));
    pw.focus();
  }

  load();
  return { refresh() { if (mode === 'view') load(); } };
};

PAGES.history = (root) => {
  const holder = h('div', { class: 'card' });
  root.append(holder);
  let busyNow = false;
  async function load() {
    if (busyNow || document.hidden || locked) return;
    busyNow = true;
    try {
      const d = await get('/api/history');
      const link = safeExplorer(d.explorer_url);
      clear(holder);
      if (!d.rows.length) { holder.append(h('div', { class: 'empty' }, 'No transactions yet.')); return; }
      holder.append(h('table', { class: 'tbl' },
        h('thead', null, h('tr', null, h('th', null, 'Type'), h('th', { class: 'num' }, 'Amount'), h('th', { class: 'hide-s' }, 'Time'), h('th', null, 'Block'), h('th', { class: 'hide-s' }, 'Confirmations'), h('th', null, 'Transaction'))),
        h('tbody', null, d.rows.map((r) => {
          const kind = r.mined ? 'Mined' : r.delta > 0 ? 'Received' : 'Sent';
          const tx = link ? h('a', { class: 'mono', href: link + '#/tx/' + r.txid, target: '_blank', rel: 'noopener noreferrer' }, short(r.txid)) : h('span', { class: 'mono' }, short(r.txid));
          return h('tr', null, h('td', null, kind), h('td', { class: 'num ' + (r.delta >= 0 ? 'pos' : 'neg') }, (r.delta > 0 ? '+' : r.delta < 0 ? '-' : '') + plk(Math.abs(r.delta))),
            h('td', { class: 'hide-s dim' }, r.pending ? 'pending' : when(r.time)), h('td', null, r.pending ? '—' : r.height.toLocaleString('en-US')),
            h('td', { class: 'hide-s' }, r.pending ? 'unconfirmed' : r.confirmations.toLocaleString('en-US')), h('td', null, tx));
        }))));
      holder.append(h('p', { class: 'hint' }, 'Sent amounts include the network fee. Moving coins to or from your stake shows as a small fee-sized outgoing entry.'));
    } catch (e) {
      fresh(holder).append(note('warn', e.message));
    } finally { busyNow = false; }
  }
  load();
  const timer = setInterval(load, 15000);
  return { dispose() { clearInterval(timer); } };
};

PAGES.settings = (root) => {
  const seedBox = h('div'), err = errBox();
  let hideTimer = null;
  const pw = h('input', { type: 'password', id: 'seedpw', autocomplete: 'off', placeholder: 'Wallet password', 'aria-label': 'Wallet password' });
  const show = h('button', { class: 'btn danger' }, 'Show recovery phrase');
  function hide() { clearTimeout(hideTimer); clear(seedBox); }
  show.addEventListener('click', () => busy(show, async () => {
    const r = await post('/api/seed', { password: pw.value });
    pw.value = '';
    fresh(seedBox).append(h('ol', { class: 'words' }, r.mnemonic.split(' ').map((w) => h('li', null, w))),
      r.passphrase_set ? note('warn', 'This wallet also uses a BIP39 passphrase. It is not shown - you need it for a restore as well.') : null,
      h('div', { class: 'actions' }, h('button', { class: 'btn', onclick: hide }, 'Hide')), h('p', { class: 'hint' }, 'Hidden automatically after one minute.'));
    r.mnemonic = '';
    hideTimer = setTimeout(hide, 60000);
  }, err));
  root.append(
    h('div', { class: 'card' }, h('h2', null, 'Wallet'), h('dl', { class: 'kv' },
      h('dt', null, 'Network'), h('dd', null, state.network + ' (addresses start with ' + state.hrp + '1)'),
      h('dt', null, 'Wallet file'), h('dd', { class: 'mono' }, state.wallet_path),
      h('dt', null, 'Node RPC'), h('dd', { class: 'mono' }, '127.0.0.1:' + state.rpc_port),
      h('dt', null, 'Explorer'), h('dd', null, safeExplorer(state.explorer_url) ? h('a', { href: state.explorer_url, target: '_blank', rel: 'noopener noreferrer' }, state.explorer_url) : '—'))),
    h('div', { class: 'card' }, h('h2', null, 'Recovery phrase'),
      note('bad', h('b', null, 'Danger. '), 'Anyone who sees these 24 words can take all your coins. Show them only in private, never type them into a website or send them to anyone - nobody from PHYLKA will ever ask for them.'),
      h('div', { class: 'row' }, pw, show), err, seedBox),
    h('div', { class: 'card' }, h('h2', null, 'Security'), h('p', { class: 'dim' }, 'The wallet locks itself after 10 minutes without activity. Your password is never stored; it is asked for every payment and when showing the phrase. Payments are signed on this computer - the node only receives the finished transaction.'),
      h('button', { class: 'btn', onclick: lock }, 'Lock now')),
    h('div', { class: 'card' }, h('h2', null, 'About'), h('dl', { class: 'kv' },
      h('dt', null, 'Software'), h('dd', null, 'PHYLKA GUI wallet ' + state.version),
      h('dt', null, 'Status'), h('dd', null, 'Public beta - use only amounts you can afford to lose. ', h('a', { href: DISCLAIMER_URL, target: '_blank', rel: 'noopener noreferrer' }, 'Disclaimer')))));
  return { dispose: hide };
};

// ------------------------------------------------------------------ start

async function main() {
  if (!TOKEN) {
    fresh($app).append(h('div', { class: 'onb' }, h('div', { class: 'logo' }, 'PHYL', h('span', null, 'KA')),
      note('warn', h('b', null, 'No session token. '), 'Open the full link printed by ', h('code', null, 'phylka gui'), ' in your terminal - it ends with ', h('code', null, '#t=...'), '.')));
    return;
  }
  try { state = await get('/api/state'); }
  catch (e) { fresh($app).append(h('div', { class: 'onb' }, note('bad', 'Cannot reach the wallet: ' + e.message + '. If you restarted ', h('code', null, 'phylka gui'), ', open the new link from the terminal.'))); return; }
  if (!state.wallet_exists) onboarding(); else startApp();
}
main();
})();
