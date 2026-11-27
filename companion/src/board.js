// Serial link to the meter: finds the port, opens it, reconnects, parses what the
// board says. Emits 'ready' (board booted, send config + data), 'cfg' (the board's
// current settings), 'event' (touch/button events) and 'status' (anything changed).

const { EventEmitter } = require('node:events');
const { SerialPort } = require('serialport');
const { log } = require('./log');

// Known USB-serial chips on ESP32 boards. The meter uses a CH340 (1a86:7523).
function chipOf(p) {
  const v = (p.vendorId || '').toLowerCase(), id = (p.productId || '').toLowerCase();
  if (v === '1a86' && id === '7523') return 'CH340';
  if (v === '1a86') return 'CH34x/CH910x';
  if (v === '10c4') return 'CP210x';
  if (v === '303a') return 'ESP32 USB';
  return null;
}

async function listPorts() {
  let ports = [];
  try { ports = await SerialPort.list(); } catch (e) { log('port list failed:', e.message); }
  return ports
    .map((p) => ({ path: p.path, chip: chipOf(p), name: p.friendlyName || p.manufacturer || '' }))
    .sort((a, b) => a.path.localeCompare(b.path, undefined, { numeric: true }));
}

// Auto-detect takes only the meter's own chip, a CH340 (1a86:7523). Other USB-serial
// chips, even the CH9102 from the same vendor, are some other device: writing frames to
// one would be sending junk to whatever it is. Pick such a port by hand if needed.
async function autoPort() {
  const ports = await listPorts();
  const board = ports.find((p) => p.chip === 'CH340');
  return board ? board.path : null;
}

class Board extends EventEmitter {
  constructor(getCfg, { verbose = false } = {}) {
    super();
    this.getCfg = getCfg;
    this.verbose = verbose;
    this.port = null;
    this.retry = null;
    this.bootTimer = null;
    this.stopped = false;
    this.state = {
      path: null, connected: false, ready: false, fw: null, cfgCapable: false, screens: false,
      boardCfg: null, lastAckAt: 0, error: '',
    };
  }

  status(patch) {
    Object.assign(this.state, patch);
    this.emit('status', this.state);
  }

  start() {
    this.stopped = false;
    this.connect();
  }

  stop() {
    this.stopped = true;
    clearTimeout(this.retry);
    clearTimeout(this.bootTimer);
    if (this.port && this.port.isOpen) this.port.close(() => {});
  }

  // Close and open again, e.g. after the port setting changed.
  reconnect() {
    clearTimeout(this.retry);
    clearTimeout(this.bootTimer);
    const p = this.port;
    this.port = null;
    if (p && p.isOpen) {
      p.removeAllListeners('close');
      p.close(() => this.connect());
    } else {
      this.connect();
    }
  }

  later(ms) {
    clearTimeout(this.retry);
    if (!this.stopped) this.retry = setTimeout(() => this.connect(), ms);
  }

  send(obj) {
    if (!this.port || !this.port.isOpen) return false;
    this.port.write(JSON.stringify(obj) + '\n');
    return true;
  }

  async connect() {
    if (this.stopped || this.port) return;
    const want = this.getCfg().port;
    const p = want && want !== 'auto' ? want : await autoPort();
    if (this.stopped || this.port) return;
    if (!p) {
      this.status({ path: null, connected: false, ready: false, error: 'no board found (looking for a CH340)' });
      return this.later(5000);
    }
    const port = new SerialPort({ path: p, baudRate: 115200, autoOpen: false });
    this.port = port;
    port.open((err) => {
      if (this.port !== port) return;
      if (err) {
        this.port = null;
        const msg = /Access denied|busy/i.test(err.message)
          ? `${p} is in use by another program` : `can't open ${p}: ${err.message}`;
        if (this.state.error !== msg) log(msg);
        this.status({ path: p, connected: false, ready: false, error: msg });
        return this.later(5000);
      }
      log(`connected to ${p}`);
      // Node leaves DTR asserted, which the board's auto-reset circuit turns into
      // BOOT (GPIO0) held low = a fake button press. Release both lines.
      port.set({ dtr: false, rts: false }, () => {
        // Releasing the lines usually doesn't reset the board, so it won't say hello
        // by itself; ask. If it did reset, its boot hello arrives anyway.
        this.helloAskedAt = Date.now();
        this.send({ type: 'hello' });
      });
      this.status({ path: p, connected: true, ready: false, fw: null, cfgCapable: false, screens: false, boardCfg: null, error: '' });
      // Opening the port resets the board; give it time to boot, then start anyway.
      this.bootTimer = setTimeout(() => this.markReady(), 3000);
    });

    let buf = '';
    port.on('data', (d) => {
      buf += d.toString('utf8');
      if (buf.length > 65536) buf = buf.slice(-4096);
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, i).trim();
        buf = buf.slice(i + 1);
        if (line.startsWith('{')) this.onLine(line);
      }
    });
    port.on('close', () => {
      if (this.port !== port) return;
      log('disconnected, waiting for the board');
      this.port = null;
      clearTimeout(this.bootTimer);
      this.status({ connected: false, ready: false });
      this.later(3000);
    });
    port.on('error', (e) => log('serial error:', e.message));
  }

  markReady() {
    clearTimeout(this.bootTimer);
    if (this.state.ready || !this.state.connected) return;
    this.status({ ready: true });
    this.emit('ready');
  }

  onLine(line) {
    let m;
    try { m = JSON.parse(line); } catch { return; }
    if (this.verbose && m.type !== 'touch') log('board:', line);
    switch (m.type) {
      case 'hello': {
        log(`board firmware ${m.fw}`);
        const wasReady = this.state.ready;
        const isReply = Date.now() - (this.helloAskedAt || 0) < 4000;
        // "apps" in hello = firmware 0.3+: five screens, relative steps, launcher, rotation 0-3.
        this.status({ fw: m.fw, cfgCapable: !!m.cfg, screens: !!m.apps });
        // An unasked hello after we started means it rebooted and lost its data.
        if (!wasReady) this.markReady();
        else if (!isReply) this.emit('ready');
        break;
      }
      case 'cfg': {
        const { type, ...rest } = m;
        this.status({ boardCfg: rest, cfgCapable: true });
        this.emit('cfg', rest);
        break;
      }
      case 'ack':
        if (m.seq !== undefined) this.status({ lastAckAt: Date.now() });
        break;
      case 'event':
        log(`board: ${m.name} ${m.value}`);
        this.emit('event', m);
        break;
      case 'err':
        log(`board error: ${m.msg}`);
        break;
      default:
        break;
    }
  }
}

module.exports = { Board, listPorts, autoPort };
