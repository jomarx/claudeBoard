// Settings file: %APPDATA%\claudeBoard\config.json (or ~/.config/claudeBoard elsewhere).
// "board" holds what is sent to the meter in a cfg frame; the rest is for this PC.

const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');

const APP_DIR = process.env.CLAUDEBOARD_DIR ||
  path.join(process.env.APPDATA || path.join(os.homedir(), '.config'), 'claudeBoard');
const FILE = path.join(APP_DIR, 'config.json');

const DEFAULTS = {
  port: 'auto',           // 'auto' or a port name like 'COM12'
  interval: 20,           // seconds between usage frames
  limits: false,          // ask Claude's servers for real plan limits (undocumented endpoint)
  sessionBudget: 0,       // without limits: tokens that count as 100% of a 5-hour window
  plan: '',               // label on the meter; empty = from the sign-in
  apps: [],               // launcher tiles: { name, path, args }, at most MAX_APPS
  startup: false,         // start with Windows (mirrors the registry Run value)
  board: {
    brightness: 200,      // 5-255
    alert1: 80,           // first alert, % of the 5-hour limit
    alert2: 90,
    h24: true,
    rot: 0,               // 0 portrait, 1 landscape, 2 portrait flipped, 3 landscape flipped
    cycle: 0,             // seconds between automatic screen changes, 0 = off
    fx: 'none',           // 'none' | 'wipe' | 'fade'
    dim: 120,             // idle seconds before dimming, 0 = never
    clock: 'digital',     // clock screen face, one of CLOCK_STYLES
  },
};

const clampInt = (v, lo, hi, def) => {
  const n = Math.round(Number(v));
  return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : def;
};

const MAX_APPS = 8;   // the board's launcher grid (MAX_APPS in src/model.h)
const CLOCK_STYLES = ['digital', 'analog', 'rings', 'flip', 'words', 'binary'];   // CLOCK_NAMES in src/protocol.cpp

// One launcher tile, or null if it has no name or nothing to open. The name is what the
// board shows (15 chars, its fonts are ASCII); path is a program, file, folder or URL.
function normalizeApp(a) {
  if (!a || typeof a !== 'object') return null;
  const str = (v, n) => (typeof v === 'string' ? v.trim().slice(0, n) : '');
  // Explorer's "Copy as path" wraps the path in quotes; the quotes aren't part of it.
  const path = str(a.path, 514).replace(/^"(.*)"$/, '$1').trim().slice(0, 512);
  const name = str(a.name, 15), args = str(a.args, 512);
  return name && path ? { name, path, args } : null;
}

// Turns anything (file contents, a POST body) into a complete, valid config.
function normalize(input, base = DEFAULTS) {
  const i = input && typeof input === 'object' ? input : {};
  const b = i.board && typeof i.board === 'object' ? i.board : {};
  const bb = base.board;
  const port = typeof i.port === 'string' && /^(auto|[A-Za-z0-9_./\\-]{1,64})$/.test(i.port.trim())
    ? i.port.trim() : base.port;
  return {
    port,
    interval: clampInt(i.interval, 5, 3600, base.interval),
    limits: typeof i.limits === 'boolean' ? i.limits : base.limits,
    sessionBudget: clampInt(i.sessionBudget, 0, 1e12, base.sessionBudget),
    plan: typeof i.plan === 'string' ? i.plan.slice(0, 15) : base.plan,
    apps: Array.isArray(i.apps) ? i.apps.map(normalizeApp).filter(Boolean).slice(0, MAX_APPS) : base.apps,
    startup: typeof i.startup === 'boolean' ? i.startup : base.startup,
    board: {
      brightness: clampInt(b.brightness, 5, 255, bb.brightness),
      alert1: clampInt(b.alert1, 1, 100, bb.alert1),
      alert2: clampInt(b.alert2, 1, 100, bb.alert2),
      h24: typeof b.h24 === 'boolean' ? b.h24 : bb.h24,
      rot: b.rot === undefined ? bb.rot : [0, 1, 2, 3].includes(Number(b.rot)) ? Number(b.rot) : 0,
      cycle: clampInt(b.cycle, 0, 3600, bb.cycle),
      fx: ['none', 'wipe', 'fade'].includes(b.fx) ? b.fx : bb.fx,
      dim: clampInt(b.dim, 0, 3600, bb.dim),
      clock: CLOCK_STYLES.includes(b.clock) ? b.clock : bb.clock,
    },
  };
}

function load() {
  try {
    return normalize(JSON.parse(fs.readFileSync(FILE, 'utf8')));
  } catch {
    return normalize({});
  }
}

function save(cfg) {
  fs.mkdirSync(APP_DIR, { recursive: true });
  const tmp = FILE + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(cfg, null, 2));
  fs.renameSync(tmp, FILE);
}

module.exports = { APP_DIR, FILE, DEFAULTS, MAX_APPS, normalize, load, save };
