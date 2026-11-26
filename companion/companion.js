#!/usr/bin/env node
// claudeBoard companion: reads Claude Code's local logs on this PC and pushes
// usage frames to the meter over USB serial (protocol in ../src/protocol.h).
//
//   node companion.js                 auto-detect the board (CH340) and run
//   node companion.js --port COM8     pick the port
//   node companion.js --limits        also show your real plan limits (see README)
//   node companion.js --dry-run --once   print one frame, no board needed
//
// Nothing here is sent anywhere except the serial port, unless --limits is on.

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

// ---------- options ----------

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf(name);
  return i >= 0 && argv[i + 1] && !argv[i + 1].startsWith('--') ? argv[i + 1] : def;
};
const flag = (name) => argv.includes(name);

if (flag('--help') || flag('-h')) {
  console.log(`claudeBoard companion
  --port COMx          serial port (default: first CH340 found)
  --interval N         seconds between updates (default 20)
  --limits             read real 5-hour / weekly limits from Claude's servers
                       using Claude Code's own sign-in (undocumented endpoint)
  --session-budget N   without --limits: tokens you treat as 100% of a 5-hour window
  --plan NAME          plan label shown on the meter (default from sign-in, or "Claude")
  --brightness N       0-255, sent to the board at start
  --dry-run            print frames instead of sending them
  --once               send one frame and exit
  --verbose            print everything the board says`);
  process.exit(0);
}

const INTERVAL_S = Math.max(5, Number(opt('--interval', 20)));
const USE_LIMITS = flag('--limits');
const SESSION_BUDGET = Number(opt('--session-budget', 0));
const DRY = flag('--dry-run');
const ONCE = flag('--once');
const VERBOSE = flag('--verbose');
const BRIGHTNESS = opt('--brightness', null);
const CLAUDE_DIR = process.env.CLAUDE_CONFIG_DIR || path.join(os.homedir(), '.claude');
const PROJECTS = path.join(CLAUDE_DIR, 'projects');

const log = (...a) => console.log(new Date().toLocaleTimeString(), ...a);

// ---------- pricing (API-equivalent estimate, $ per million tokens) ----------
// [input, output, cache read]; cache writes are 1.25x input (5 min) or 2x (1 hour).
const PRICES = [
  [/fable-5-2|mythos-5-2/, 8, 40, 0.2],
  [/fable|mythos/, 10, 50, 1.0],
  [/opus-5-5/, 4, 20, 0.2],
  [/opus/, 5, 25, 0.5],
  [/sonnet-5-5/, 2, 10, 0.1],
  [/sonnet-5/, 2, 10, 0.2],
  [/sonnet/, 3, 15, 0.3],
  [/haiku-5-5/, 0.1, 0.5, 0.01],
  [/haiku/, 1, 5, 0.1],
];

function costOf(e) {
  const p = PRICES.find(([re]) => re.test(e.model));
  if (!p) return 0;
  const [, inp, out, read] = p;
  return (e.inp * inp + e.out * out + e.cr * read + e.cw5 * inp * 1.25 + e.cw1h * inp * 2) / 1e6;
}

function modelLabel(id) {
  const m = /claude-([a-z]+)-(\d+)(?:-(\d{1,2}))?(?!\d)/.exec(id || '');
  if (!m) return null;
  const fam = m[1][0].toUpperCase() + m[1].slice(1);
  return `${fam} ${m[2]}${m[3] ? '.' + m[3] : ''}`.slice(0, 15);
}

// ---------- log reading ----------
// Each assistant turn in ~/.claude/projects/**/*.jsonl carries message.usage.
// Streaming can write the same message more than once, so entries are keyed by message id.

const entries = new Map();          // message id -> entry
const fileState = new Map();        // path -> { offset, rest }
const KEEP_MS = 8 * 24 * 3600e3;

function walk(dir, out) {
  let list;
  try { list = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
  for (const d of list) {
    const p = path.join(dir, d.name);
    if (d.isDirectory()) walk(p, out);
    else if (d.name.endsWith('.jsonl')) out.push(p);
  }
}

function parseLine(line) {
  if (!line.includes('"usage"')) return;
  let o;
  try { o = JSON.parse(line); } catch { return; }
  const msg = o.message;
  const u = msg && msg.usage;
  if (!u || o.type !== 'assistant' || !msg.model || msg.model.startsWith('<')) return;
  const ts = Date.parse(o.timestamp);
  if (!ts) return;
  const cc = u.cache_creation || {};
  const cw = u.cache_creation_input_tokens || 0;
  const cw1h = cc.ephemeral_1h_input_tokens || 0;
  const e = {
    ts, model: msg.model, session: o.sessionId || '',
    inp: u.input_tokens || 0, out: u.output_tokens || 0,
    cr: u.cache_read_input_tokens || 0,
    cw, cw1h, cw5: Math.max(0, cw - cw1h),
  };
  const key = msg.id || o.requestId || o.uuid;
  const prev = entries.get(key);
  if (!prev || e.out >= prev.out) entries.set(key, e);
}

function scanLogs() {
  const files = [];
  walk(PROJECTS, files);
  const cutoff = Date.now() - KEEP_MS;
  for (const f of files) {
    let st;
    try { st = fs.statSync(f); } catch { continue; }
    if (st.mtimeMs < cutoff) continue;
    let s = fileState.get(f);
    if (!s || st.size < s.offset) s = { offset: 0, rest: '' };   // new or truncated
    if (st.size > s.offset) {
      const fd = fs.openSync(f, 'r');
      const buf = Buffer.alloc(st.size - s.offset);
      fs.readSync(fd, buf, 0, buf.length, s.offset);
      fs.closeSync(fd);
      s.offset = st.size;
      const lines = (s.rest + buf.toString('utf8')).split('\n');
      s.rest = lines.pop();
      for (const l of lines) parseLine(l);
    }
    fileState.set(f, s);
  }
  for (const [k, e] of entries) if (e.ts < cutoff) entries.delete(k);
}

// ---------- numbers for the meter ----------

const tokensOf = (e) => e.inp + e.out + e.cr + e.cw;

function summarize() {
  const now = Date.now();
  const midnight = new Date(); midnight.setHours(0, 0, 0, 0);
  const all = [...entries.values()].sort((a, b) => a.ts - b.ts);
  const today = all.filter((e) => e.ts >= midnight.getTime());

  const hourly = new Array(24).fill(0);
  const byModel = new Map();
  const sessions = new Set();
  let tokens = 0, cost = 0, cacheRead = 0, inputSide = 0;
  for (const e of today) {
    const t = tokensOf(e);
    tokens += t;
    cost += costOf(e);
    cacheRead += e.cr;
    inputSide += e.inp + e.cr + e.cw;
    hourly[new Date(e.ts).getHours()] += t;
    sessions.add(e.session);
    const name = modelLabel(e.model);
    if (name) byModel.set(name, (byModel.get(name) || 0) + t);
  }
  const models = [...byModel.entries()]
    .sort((a, b) => b[1] - a[1]).slice(0, 4)
    .map(([n, t]) => ({ n, p: tokens ? Math.round((100 * t) / tokens) : 0 }));

  // Estimated 5-hour window: starts at the hour of the first message after the
  // previous window ended, lasts five hours. Only used when --limits is off.
  let winStart = 0, winEnd = 0, winTokens = 0;
  for (const e of all) {
    if (e.ts >= winEnd) {
      winStart = Math.floor(e.ts / 3600e3) * 3600e3;
      winEnd = winStart + 5 * 3600e3;
      winTokens = 0;
    }
    winTokens += e.inp + e.out + e.cw;   // cache reads left out: they barely count
  }
  const active = now < winEnd;

  return {
    today: {
      tokens, cost: Math.round(cost * 100) / 100, sessions: sessions.size,
      cache: inputSide ? Math.round((100 * cacheRead) / inputSide) : 0,
      hourly: hourly.map((v) => Math.min(v, 0xffffffff)), models,
    },
    window: { active, reset: active ? Math.floor(winEnd / 1000) : 0, tokens: active ? winTokens : 0 },
  };
}

// ---------- real limits (opt-in) ----------
// Uses the sign-in Claude Code keeps on this PC and an endpoint Claude Code's
// /usage screen relies on. It is not a public API and may change or stop working.

let limits = null, limitsAt = 0, limitsErr = '', planFromLogin = null;

function readLogin() {
  // Returns the access token, or a string starting with '!' that says why there is none.
  // Never prints the token itself.
  let j;
  try {
    j = JSON.parse(fs.readFileSync(path.join(CLAUDE_DIR, '.credentials.json'), 'utf8'));
  } catch (e) {
    return `!can't read ${path.join(CLAUDE_DIR, '.credentials.json')} (${e.code || e.message})`;
  }
  const c = j && j.claudeAiOauth;
  if (!c) return `!no claudeAiOauth sign-in in .credentials.json (keys: ${Object.keys(j || {}).join(', ') || 'none'})`;
  const tier = `${c.rateLimitTier || ''} ${c.subscriptionType || ''}`.toLowerCase();
  planFromLogin = /20x/.test(tier) ? 'Max 20x' : /5x/.test(tier) ? 'Max 5x' : /max/.test(tier) ? 'Max'
    : /team/.test(tier) ? 'Team' : /enterprise/.test(tier) ? 'Enterprise' : /pro/.test(tier) ? 'Pro' : null;
  if (!c.accessToken) return '!sign-in has no access token';
  if (c.expiresAt && c.expiresAt < Date.now()) {
    return `!sign-in expired ${new Date(c.expiresAt).toLocaleString()} - run \`claude\` in a terminal once to refresh it`;
  }
  return c.accessToken;
}

const toEpoch = (iso) => (iso ? Math.floor(Date.parse(iso) / 1000) || 0 : 0);
const pctOf = (o) => (o && typeof o.utilization === 'number' ? Math.round(o.utilization) : -1);

async function refreshLimits() {
  if (!USE_LIMITS || Date.now() - limitsAt < 120e3) return;
  limitsAt = Date.now();
  const token = readLogin();
  try {
    if (token.startsWith('!')) throw new Error(token.slice(1));
    const r = await fetch('https://api.anthropic.com/api/oauth/usage', {
      headers: { Authorization: `Bearer ${token}`, 'anthropic-beta': 'oauth-2025-04-20' },
      signal: AbortSignal.timeout(10000),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}${r.status === 401 ? ' (open Claude Code once to refresh the sign-in)' : ''}`);
    const j = await r.json();
    limits = {
      session: { pct: pctOf(j.five_hour), reset: toEpoch(j.five_hour?.resets_at) },
      week: { pct: pctOf(j.seven_day), reset: toEpoch(j.seven_day?.resets_at) },
      opus: { pct: pctOf(j.seven_day_opus) },
    };
    if (limitsErr) log('limits: working again');
    limitsErr = '';
  } catch (err) {
    if (limitsErr !== err.message) log('limits:', err.message);
    limitsErr = err.message;
  }
}

// ---------- frames ----------

let seq = 0;

function buildFrame() {
  scanLogs();
  const s = summarize();
  const f = {
    type: 'usage', seq: ++seq,
    now: Math.floor(Date.now() / 1000),
    tz: -new Date().getTimezoneOffset() * 60,
    plan: opt('--plan', null) || planFromLogin || 'Claude',
    today: s.today,
  };
  if (limits) {
    Object.assign(f, limits);
  } else {
    const pct = SESSION_BUDGET > 0 && s.window.active ? Math.round((100 * s.window.tokens) / SESSION_BUDGET) : -1;
    f.session = { pct, reset: s.window.reset };
    f.week = { pct: -1, reset: 0 };
    f.opus = { pct: -1 };
  }
  return f;
}

// ---------- serial ----------

async function findPort() {
  const { SerialPort } = await import('serialport');
  const ports = await SerialPort.list();
  const ch340 = ports.find((p) => (p.vendorId || '').toLowerCase() === '1a86');
  return (ch340 || ports[0] || {}).path;
}

async function main() {
  if (USE_LIMITS) readLogin();
  await refreshLimits();

  if (DRY) {
    const show = () => console.log(JSON.stringify(buildFrame()));
    show();
    if (ONCE) return;
    setInterval(async () => { await refreshLimits(); show(); }, INTERVAL_S * 1000);
    return;
  }

  const { SerialPort } = await import('serialport');
  let port = null, ready = false, timer = null, lastPush = 0;

  const send = (obj) => {
    if (!port || !port.isOpen) return;
    port.write(JSON.stringify(obj) + '\n');
  };
  const push = async () => {
    await refreshLimits();
    lastPush = Date.now();
    const f = buildFrame();
    send(f);
    log(`sent: today ${(f.today.tokens / 1e6).toFixed(2)}M tokens, ~$${f.today.cost}, ` +
        `session ${f.session.pct < 0 ? '?' : f.session.pct + '%'}` +
        (f.week.pct >= 0 ? `, week ${f.week.pct}%` : ''));
    if (ONCE) setTimeout(() => process.exit(0), 500);
  };
  const onReady = () => {
    if (ready) return;
    ready = true;
    if (BRIGHTNESS !== null) send({ type: 'cfg', brightness: Number(BRIGHTNESS) });
    push();
    clearInterval(timer);
    if (!ONCE) timer = setInterval(push, INTERVAL_S * 1000);
  };

  const connect = async () => {
    const p = opt('--port', null) || (await findPort());
    if (!p) { log('no serial port found, retrying'); return setTimeout(connect, 5000); }
    port = new SerialPort({ path: p, baudRate: 115200 }, (err) => {
      if (err) { log(`can't open ${p}: ${err.message}`); port = null; return setTimeout(connect, 5000); }
      log(`connected to ${p}`);
      // Node leaves DTR asserted, which the board's auto-reset circuit turns into
      // BOOT (GPIO0) held low = a fake button press. Release both lines.
      port.set({ dtr: false, rts: false }, () => {});
      ready = false;
      // Opening the port resets the board; give it time to boot, then start anyway.
      setTimeout(onReady, 3000);
    });
    let buf = '';
    port.on('data', (d) => {
      buf += d.toString('utf8');
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, i).trim();
        buf = buf.slice(i + 1);
        if (!line.startsWith('{')) continue;
        let m;
        try { m = JSON.parse(line); } catch { continue; }
        if (VERBOSE) log('board:', line);
        if (m.type === 'hello') {
          log(`board firmware ${m.fw}`);
          // A hello after we started means it rebooted and lost its data.
          if (!ready) onReady(); else if (Date.now() - lastPush > 3000) push();
        } else if (m.type === 'event') {
          log(`board: ${m.name} ${m.value}`);
        } else if (m.type === 'err') {
          log(`board error: ${m.msg}`);
        }
      }
    });
    port.on('close', () => {
      log('disconnected, waiting for the board');
      clearInterval(timer);
      ready = false;
      port = null;
      setTimeout(connect, 3000);
    });
    port.on('error', (e) => log('serial error:', e.message));
  };
  connect();
}

main().catch((e) => { console.error(e); process.exit(1); });
