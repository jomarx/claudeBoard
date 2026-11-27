// Log to the console (when there is one), a file next to the config, and a ring
// buffer the settings page shows.

const fs = require('node:fs');
const path = require('node:path');
const { APP_DIR } = require('./config');

const FILE = path.join(APP_DIR, 'companion.log');
const MAX_BYTES = 1024 * 1024;
const recent = [];
let toFile = true;

function write(line) {
  if (!toFile) return;
  try {
    fs.mkdirSync(APP_DIR, { recursive: true });
    try {
      if (fs.statSync(FILE).size > MAX_BYTES) fs.renameSync(FILE, FILE + '.1');
    } catch { /* no file yet */ }
    fs.appendFileSync(FILE, line + '\n');
  } catch {
    toFile = false;   // read-only profile or similar: keep going without a file
  }
}

function log(...args) {
  const msg = args.map((a) => (typeof a === 'string' ? a : a instanceof Error ? a.message : JSON.stringify(a))).join(' ');
  const line = `${new Date().toLocaleString()}  ${msg}`;
  recent.push(line);
  if (recent.length > 200) recent.shift();
  try { process.stdout.write(`${new Date().toLocaleTimeString()} ${msg}\n`); } catch { /* no console */ }
  write(line);
}

module.exports = { log, recent, FILE };
