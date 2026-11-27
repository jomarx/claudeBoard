// claudeBoard companion: tray app + settings page + serial feed to the meter.

const fs = require('node:fs');
const { spawn } = require('node:child_process');
const config = require('./config');
const { log, recent, FILE: LOG_FILE } = require('./log');
const { buildFrame, refreshLimits, limitsState } = require('./usage');
const { Board, listPorts } = require('./board');
const { Tray } = require('./tray');
const { createServer } = require('./server');
const startup = require('./startup');
const { launch, frontNames } = require('./launcher');
const { refreshWeather, weatherFrame, weatherState, geocode } = require('./weather');
const VERSION = require('../package.json').version;

// ---------- command line ----------

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf(name);
  return i >= 0 && argv[i + 1] !== undefined && !argv[i + 1].startsWith('--') ? argv[i + 1] : def;
};
const flag = (name) => argv.includes(name);

const HELP = `claudeBoard companion ${VERSION}
Runs in the tray and serves a settings page; settings are saved in
${config.FILE}

  --port COMx          serial port (default: auto, first CH340 found)
  --interval N         seconds between updates
  --limits             read real 5-hour / weekly limits from Claude's servers
  --session-budget N   without --limits: tokens that count as 100% of a 5-hour window
  --plan NAME          plan label shown on the meter
  --brightness N       5-255
  --ui-port N          settings page port (default 47820)
  --no-tray            no tray icon
  --no-browser         don't open the settings page on first run
  --dry-run            print frames instead of sending them (no board, no UI)
  --once               send one frame and exit
  --verbose            log everything the board says
Command-line values win over the saved settings until changed on the page.`;

const UI_PORT = Number(opt('--ui-port', 47820));
const DRY = flag('--dry-run');
const ONCE = flag('--once');
const VERBOSE = flag('--verbose');
const NO_TRAY = flag('--no-tray') || ONCE;
const NO_BROWSER = flag('--no-browser');

// Command-line overrides, kept in memory only.
const overrides = {};
const ovBoard = {};
if (opt('--port')) overrides.port = opt('--port');
if (opt('--interval')) overrides.interval = Number(opt('--interval'));
if (flag('--limits')) overrides.limits = true;
if (opt('--session-budget')) overrides.sessionBudget = Number(opt('--session-budget'));
if (opt('--plan')) overrides.plan = opt('--plan');
if (opt('--brightness')) ovBoard.brightness = Number(opt('--brightness'));

const firstRun = !fs.existsSync(config.FILE);
let saved = config.load();

function effective() {
  return config.normalize({ ...saved, ...overrides, board: { ...saved.board, ...ovBoard } });
}

// ---------- helpers ----------

function openBrowser(url) {
  if (process.platform !== 'win32') return;
  spawn('rundll32.exe', ['url.dll,FileProtocolHandler', url], { detached: true, stdio: 'ignore', windowsHide: true }).unref();
}

const sameJSON = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// ---------- dry run: frames to stdout, nothing else ----------

async function dryRun() {
  const cfg = effective();
  const show = async () => {
    await refreshLimits(cfg);
    console.log(JSON.stringify(buildFrame(cfg)));
  };
  await show();
  if (!ONCE) setInterval(show, cfg.interval * 1000);
}

// ---------- the app ----------

async function run() {
  const url = `http://127.0.0.1:${UI_PORT}/`;
  const board = new Board(effective, { verbose: VERBOSE });
  const tray = NO_TRAY ? null : new Tray();
  // Watch for the app's window before starting it, so Windows' focus rules don't leave it behind.
  const open = (a) => { if (tray) tray.front(frontNames(a)); launch(a); };
  let pushTimer = null, lastFrame = null, lastFrameAt = 0;
  let startupOn = false;

  const sendBoardCfg = () => board.send({ type: 'cfg', ...effective().board });
  // The board only gets the tile names; paths stay here. Its fonts are ASCII only.
  const sendApps = () => board.send({
    type: 'apps',
    list: effective().apps.map((a) => a.name.replace(/[^\x20-\x7e]/g, '?')),
  });

  // Weather goes out only when there is something new (weatherState.seq moved) or the
  // board came back; -1 forces the next push to send it.
  let weatherSent = -1;
  const sendWeather = (cfg) => {
    if (weatherState.seq === weatherSent) return;
    if (board.send(weatherFrame(cfg))) weatherSent = weatherState.seq;
  };

  const push = async () => {
    const cfg = effective();
    await Promise.all([refreshLimits(cfg), refreshWeather(cfg)]);
    if (!board.state.ready) return;
    sendWeather(cfg);
    const f = buildFrame(cfg);
    if (board.send(f)) {
      lastFrame = f;
      lastFrameAt = Date.now();
      if (VERBOSE || ONCE) {
        log(`sent: today ${(f.today.tokens / 1e6).toFixed(2)}M tokens, ~$${f.today.cost}, ` +
            `session ${f.session.pct < 0 ? '?' : f.session.pct + '%'}` +
            (f.week.pct >= 0 ? `, week ${f.week.pct}%` : ''));
      }
      if (ONCE) setTimeout(() => process.exit(0), 500);
    }
  };

  const schedule = () => {
    clearInterval(pushTimer);
    if (!ONCE) pushTimer = setInterval(push, effective().interval * 1000);
  };

  board.on('ready', () => {
    sendBoardCfg();
    sendApps();
    weatherSent = -1;   // a (re)booted board has no weather
    push();
    schedule();
  });
  board.on('event', (m) => {
    if (m.name === 'launch') {
      const a = effective().apps[m.value];
      if (a) open(a);
      else log(`board asked for app ${m.value}, which isn't set`);
    }
    if (m.name === 'brightness' && Number.isFinite(m.value)) {
      // Long press on the board changed brightness: remember it.
      delete ovBoard.brightness;
      saved = config.normalize({ ...saved, board: { ...saved.board, brightness: m.value } });
      config.save(saved);
    }
  });

  // ---------- tray status ----------

  const updateTray = () => {
    if (!tray) return;
    const s = board.state;
    const cfg = effective();
    const fresh = lastFrameAt && Date.now() - lastFrameAt < cfg.interval * 2500;
    let color = '#888780', line;
    if (!s.connected) line = s.error || 'board not connected';
    else if (!s.ready) { line = `${s.path}: waiting for the board`; color = '#ef9f27'; }
    else {
      color = cfg.limits && limitsState.error ? '#ef9f27' : fresh ? '#5dcaa5' : '#ef9f27';
      const sess = lastFrame && lastFrame.session.pct >= 0 ? `, session ${lastFrame.session.pct}%` : '';
      line = `${s.path}${s.fw ? ' · fw ' + s.fw : ''}${sess}`;
      if (cfg.limits && limitsState.error) line += ' · limits: ' + limitsState.error.slice(0, 40);
    }
    tray.setStatus(color, `claudeBoard - ${line}`, line);
  };
  board.on('status', updateTray);
  setInterval(updateTray, 2000);

  // ---------- what the settings page calls ----------

  const app = {
    async state() {
      const cfg = effective();
      return {
        version: VERSION,
        config: cfg,
        overridden: [...Object.keys(overrides), ...Object.keys(ovBoard).map((k) => 'board.' + k)],
        board: board.state,
        limits: { enabled: cfg.limits, ok: !!limitsState.data && !limitsState.error, error: limitsState.error, at: limitsState.at },
        weather: { set: cfg.weather.lat !== null, data: weatherState.data, error: weatherState.error, at: weatherState.at },
        lastFrame: lastFrame && {
          at: lastFrameAt, plan: lastFrame.plan, tokens: lastFrame.today.tokens, cost: lastFrame.today.cost,
          session: lastFrame.session.pct, week: lastFrame.week.pct,
        },
        startup: startupOn,
        files: { config: config.FILE, log: LOG_FILE },
        log: recent.slice(-80),
      };
    },
    ports: () => listPorts(),
    geocode: (q) => geocode(q),
    async updateConfig(body) {
      const before = effective();
      // Anything set on the page stops being overridden by the command line.
      for (const k of Object.keys(body)) if (k !== 'board') delete overrides[k];
      for (const k of Object.keys(body.board || {})) delete ovBoard[k];
      saved = config.normalize({ ...saved, ...body, board: { ...saved.board, ...(body.board || {}) } });
      config.save(saved);
      const after = effective();

      if (after.port !== before.port) { log(`port set to ${after.port}`); board.reconnect(); }
      if (!sameJSON(after.board, before.board)) sendBoardCfg();
      if (!sameJSON(after.apps, before.apps)) sendApps();
      if (!sameJSON(after.weather, before.weather)) {
        await refreshWeather(after, true);
        sendWeather(after);
      }
      if (after.interval !== before.interval) schedule();
      if (after.limits !== before.limits) await refreshLimits(after, true);
      if (after.limits !== before.limits || after.plan !== before.plan || after.sessionBudget !== before.sessionBudget) push();
      if (after.startup !== startupOn) {
        startupOn = await startup.setEnabled(after.startup);
        if (tray) tray.setStartup(startupOn);
      }
      updateTray();
      return app.state();
    },
    async action(name, body = {}) {
      switch (name) {
        // A relative step lets the board wrap around however many screens it has.
        // Firmware before 0.3 reads a step as "screen 0", so don't send it one.
        case 'next':
        case 'prev':
          if (board.state.ready && !board.state.screens) throw new Error('the board firmware is older than 0.3.0; flash it to page from here');
          board.send({ type: 'screen', step: name === 'next' ? 1 : -1 });
          break;
        case 'launch': {
          const a = effective().apps[Number(body.index)];
          if (!a) throw new Error('no such app');
          open(a);
          break;
        }
        case 'alert': board.send({ type: 'preview', view: 'alert' }); break;
        case 'back': board.send({ type: 'preview', view: 'back' }); break;
        case 'reconnect': board.reconnect(); break;
        case 'weather': await refreshWeather(effective(), true); sendWeather(effective()); break;
        case 'push': await refreshLimits(effective(), true); await push(); break;
        default: throw new Error('unknown action');
      }
      return app.state();
    },
  };

  // ---------- single instance: the settings port doubles as the lock ----------

  const server = createServer(app, UI_PORT);
  const listening = await new Promise((resolve) => {
    server.once('error', (e) => resolve(e.code === 'EADDRINUSE' ? 'busy' : e));
    server.listen(UI_PORT, '127.0.0.1', () => resolve(true));
  });
  if (listening === 'busy') {
    log(`already running, opening ${url}`);
    openBrowser(url);
    setTimeout(() => process.exit(0), 300);
    return;
  }
  if (listening !== true) throw listening;
  log(`claudeBoard companion ${VERSION}, settings at ${url}`);

  startupOn = await startup.isEnabled();
  if (startupOn !== saved.startup) { saved = { ...saved, startup: startupOn }; config.save(saved); }

  const quit = () => {
    log('quitting');
    if (tray) tray.stop();
    board.stop();
    server.close();
    setTimeout(() => process.exit(0), 300);
  };

  if (tray) {
    tray.on('command', async (cmd) => {
      if (cmd === 'open') openBrowser(url);
      else if (cmd === 'reconnect') board.reconnect();
      else if (cmd === 'quit') quit();
      else if (cmd === 'startup') await app.updateConfig({ startup: !startupOn });
    });
    tray.start();
    tray.setStartup(startupOn);
    updateTray();
  }

  if (firstRun) config.save(saved);
  if (firstRun && !NO_BROWSER && !ONCE) openBrowser(url);

  process.on('SIGINT', quit);
  process.on('SIGTERM', quit);
  board.start();
}

// ---------- entry ----------

function main() {
  if (flag('--help') || flag('-h')) {
    console.log(HELP);
    return;
  }
  process.on('uncaughtException', (e) => log('error:', e.stack || e.message));
  process.on('unhandledRejection', (e) => log('error:', (e && e.stack) || String(e)));
  (DRY ? dryRun() : run()).catch((e) => { log('fatal:', e.stack || e.message); process.exit(1); });
}

module.exports = { main };
