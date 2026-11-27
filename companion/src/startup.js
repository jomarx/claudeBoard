// "Start with Windows": a value under HKCU\...\Run, so no admin rights are needed.

const { execFile } = require('node:child_process');
const path = require('node:path');

const KEY = 'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run';
const NAME = 'claudeBoard';

function command() {
  // Packaged exe: run it directly. From source: node + this script.
  if (process.pkg) return `"${process.execPath}"`;
  return `"${process.execPath}" "${path.resolve(__dirname, '..', 'companion.js')}"`;
}

const reg = (args) => new Promise((resolve) => {
  execFile('reg.exe', args, { windowsHide: true }, (err, stdout) => resolve({ ok: !err, stdout: stdout || '' }));
});

async function isEnabled() {
  if (process.platform !== 'win32') return false;
  const r = await reg(['query', KEY, '/v', NAME]);
  return r.ok && r.stdout.includes(NAME);
}

async function setEnabled(on) {
  if (process.platform !== 'win32') return false;
  if (on) await reg(['add', KEY, '/v', NAME, '/t', 'REG_SZ', '/d', command(), '/f']);
  else await reg(['delete', KEY, '/v', NAME, '/f']);
  return isEnabled();
}

module.exports = { isEnabled, setEnabled };
