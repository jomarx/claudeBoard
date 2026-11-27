// Reads Claude Code's local logs and (optionally) the real plan limits, and builds
// usage frames for the meter (protocol in ../../src/protocol.h).

const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { log } = require('./log');

const CLAUDE_DIR = process.env.CLAUDE_CONFIG_DIR || path.join(os.homedir(), '.claude');
const PROJECTS = path.join(CLAUDE_DIR, 'projects');

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
  // previous window ended, lasts five hours. Only used when limits are off.
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

const limitsState = { data: null, at: 0, error: '', plan: null };

function readLogin() {
  // Returns the access token, or a string starting with '!' that says why there is none.
  // Never logs the token itself.
  const file = path.join(CLAUDE_DIR, '.credentials.json');
  let j;
  try {
    j = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (e) {
    return `!can't read ${file} (${e.code || e.message})`;
  }
  const c = j && j.claudeAiOauth;
  if (!c) return `!no claudeAiOauth sign-in in .credentials.json (keys: ${Object.keys(j || {}).join(', ') || 'none'})`;
  const tier = `${c.rateLimitTier || ''} ${c.subscriptionType || ''}`.toLowerCase();
  limitsState.plan = /20x/.test(tier) ? 'Max 20x' : /5x/.test(tier) ? 'Max 5x' : /max/.test(tier) ? 'Max'
    : /team/.test(tier) ? 'Team' : /enterprise/.test(tier) ? 'Enterprise' : /pro/.test(tier) ? 'Pro' : null;
  if (!c.accessToken) return '!sign-in has no access token';
  if (c.expiresAt && c.expiresAt < Date.now()) {
    return `!sign-in expired ${new Date(c.expiresAt).toLocaleString()} - run \`claude\` in a terminal once to refresh it`;
  }
  return c.accessToken;
}

const toEpoch = (iso) => (iso ? Math.floor(Date.parse(iso) / 1000) || 0 : 0);
const pctOf = (o) => (o && typeof o.utilization === 'number' ? Math.round(o.utilization) : -1);

async function refreshLimits(cfg, force = false) {
  if (!cfg.limits) {
    limitsState.data = null;
    limitsState.error = '';
    return;
  }
  if (!force && Date.now() - limitsState.at < 120e3) return;
  limitsState.at = Date.now();
  const token = readLogin();
  try {
    if (token.startsWith('!')) throw new Error(token.slice(1));
    const r = await fetch('https://api.anthropic.com/api/oauth/usage', {
      headers: { Authorization: `Bearer ${token}`, 'anthropic-beta': 'oauth-2025-04-20' },
      signal: AbortSignal.timeout(10000),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}${r.status === 401 ? ' (open Claude Code once to refresh the sign-in)' : ''}`);
    const j = await r.json();
    limitsState.data = {
      session: { pct: pctOf(j.five_hour), reset: toEpoch(j.five_hour && j.five_hour.resets_at) },
      week: { pct: pctOf(j.seven_day), reset: toEpoch(j.seven_day && j.seven_day.resets_at) },
      opus: { pct: pctOf(j.seven_day_opus) },
    };
    if (limitsState.error) log('limits: working again');
    limitsState.error = '';
  } catch (err) {
    if (limitsState.error !== err.message) log('limits:', err.message);
    limitsState.error = err.message;
  }
}

// ---------- frames ----------

let seq = 0;

function buildFrame(cfg) {
  scanLogs();
  const s = summarize();
  if (cfg.limits && limitsState.plan === null) readLogin();   // fills in the plan name
  const f = {
    type: 'usage', seq: ++seq,
    now: Math.floor(Date.now() / 1000),
    tz: -new Date().getTimezoneOffset() * 60,
    plan: cfg.plan || limitsState.plan || 'Claude',
    today: s.today,
  };
  if (cfg.limits && limitsState.data) {
    Object.assign(f, limitsState.data);
  } else {
    const pct = cfg.sessionBudget > 0 && s.window.active
      ? Math.round((100 * s.window.tokens) / cfg.sessionBudget) : -1;
    f.session = { pct, reset: s.window.reset };
    f.week = { pct: -1, reset: 0 };
    f.opus = { pct: -1 };
  }
  return f;
}

module.exports = { buildFrame, refreshLimits, limitsState };
