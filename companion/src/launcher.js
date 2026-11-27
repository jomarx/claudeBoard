// Opens a launcher tile's target on this PC when it is tapped on the board.
// Programs (.exe / .com) start directly with their arguments, no shell in between.
// Anything else (a URL, document, folder or shortcut) goes to the Windows shell, the
// same as double-clicking it; arguments don't apply there.

const path = require('node:path');
const { spawn } = require('node:child_process');
const { log } = require('./log');

// Splits an argument string the way a command line reads: spaces separate, double
// quotes group ("C:\My Files\a.txt" stays one argument).
function splitArgs(s) {
  const out = [];
  let cur = '', quoted = false, started = false;
  for (const ch of s) {
    if (ch === '"') { quoted = !quoted; started = true; continue; }
    if (!quoted && /\s/.test(ch)) {
      if (started) out.push(cur);
      cur = ''; started = false;
      continue;
    }
    cur += ch; started = true;
  }
  if (started) out.push(cur);
  return out;
}

function isProgram(target) {
  return /\.(exe|com)$/i.test(target);
}

// Programs that only hand a target to something else; their own window isn't the app's.
const HANDOFF = new Set(['explorer', 'rundll32', 'cmd', 'powershell', 'pwsh', 'wscript']);

// Process names whose window should come to the front after a launch: the program's own
// name (chrome for chrome.exe), and the tile name for apps started through a handoff,
// e.g. a Store app via explorer.exe shell:AppsFolder (the Claude tile -> claude.exe).
function frontNames(app) {
  const names = [];
  if (isProgram(app.path)) {
    const base = path.basename(app.path).replace(/\.(exe|com)$/i, '');
    if (!HANDOFF.has(base.toLowerCase())) names.push(base);
  }
  names.push(app.name.replace(/\s+/g, ''));
  return names;
}

function launch(app) {
  const target = app.path;
  let cmd, args, cwd;
  if (isProgram(target)) {
    cmd = target;
    args = splitArgs(app.args || '');
    // Start in the program's own folder, like a shortcut does.
    if (path.isAbsolute(target)) cwd = path.dirname(target);
  } else {
    cmd = 'rundll32.exe';
    args = ['url.dll,FileProtocolHandler', target];
  }
  try {
    const child = spawn(cmd, args, { cwd, detached: true, stdio: 'ignore' });
    child.on('error', (e) => log(`can't open ${app.name}: ${e.message}`));
    child.unref();
    log(`opening ${app.name}`);
  } catch (e) {
    log(`can't open ${app.name}: ${e.message}`);
  }
}

module.exports = { launch, frontNames, splitArgs, isProgram };
