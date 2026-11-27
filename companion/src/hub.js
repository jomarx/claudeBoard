// Every board link the companion has: the USB board (always kept, it carries Wi-Fi setup)
// and any Wi-Fi boards. Sync picks the active side: data goes only there, and only events
// from there act on this PC. Settings (cfg) go to every link, so a board always learns a
// Sync change, whichever link it is on.
//
// It stands where the single Board used to: send, state, start/stop/reconnect, and the
// 'ready', 'event', 'status' events.

const { EventEmitter } = require('node:events');
const { NetBoard } = require('./netboard');

class Hub extends EventEmitter {
  constructor(usb, getCfg, { verbose = false } = {}) {
    super();
    this.usb = usb;
    this.getCfg = getCfg;
    this.verbose = verbose;
    this.wifi = new Set();
    this.wire(usb);
  }

  mode() { return this.getCfg().board.sync === 'wifi' ? 'wifi' : 'usb'; }

  isActive(link) { return this.mode() === 'usb' ? link === this.usb : link !== this.usb; }

  wire(link) {
    link.on('ready', () => {
      if (this.isActive(link)) this.emit('ready', link);
      else if (link === this.usb) this.emit('setupReady');   // USB in Wi-Fi mode: settings only
    });
    link.on('event', (m) => { if (this.isActive(link)) this.emit('event', m); });
    link.on('status', () => this.emit('status', this.state));
    link.on('net', (n) => this.emit('net', n, link));
    link.on('boardError', (msg) => this.emit('boardError', msg, link));
  }

  // A socket that passed the token check in lan.js.
  addWifi(socket, ip) {
    const b = new NetBoard(socket, ip, { verbose: this.verbose });
    this.wifi.add(b);
    this.wire(b);
    b.on('closed', () => { this.wifi.delete(b); this.emit('status', this.state); });
    this.emit('status', this.state);
  }

  closeWifi(reason) { for (const b of this.wifi) b.close(4000, reason); }

  // Data: the active side's ready boards. True if at least one got it.
  send(obj) {
    let sent = false;
    for (const link of this.mode() === 'usb' ? [this.usb] : this.wifi) {
      if (link.state.ready && link.send(obj)) sent = true;
    }
    return sent;
  }

  // Settings: every connected link.
  sendAll(obj) {
    let sent = false;
    for (const link of [this.usb, ...this.wifi]) if (link.state.connected && link.send(obj)) sent = true;
    return sent;
  }

  // What the tray and the page show as "the board": the USB one in USB mode; in Wi-Fi mode
  // the first ready Wi-Fi board, or a summary when there is none.
  get state() {
    if (this.mode() === 'usb') return { ...this.usb.state, link: 'usb' };
    const boards = [...this.wifi];
    const b = boards.find((x) => x.state.ready) || boards[0];
    if (b) return { ...b.state, link: 'wifi', count: boards.length };
    return {
      path: null, connected: false, ready: false, fw: null, cfgCapable: false, screens: false,
      netCapable: false, net: null, boardCfg: null, lastAckAt: 0, link: 'wifi', count: 0,
      error: 'waiting for a board over Wi-Fi',
    };
  }

  // Every link, for the settings page.
  list() {
    const one = (b, link) => ({
      link, path: b.state.path, connected: b.state.connected, ready: b.state.ready, fw: b.state.fw,
      rssi: b.state.net ? b.state.net.rssi : null, since: b.since || null,
      net: b.state.net,   // the board's own report: no password, no token
    });
    return [one(this.usb, 'usb'), ...[...this.wifi].map((b) => one(b, 'wifi'))];
  }

  start() { this.usb.start(); }
  stop() { this.closeWifi('companion quitting'); this.usb.stop(); }
  reconnect() { this.usb.reconnect(); }
}

module.exports = { Hub };
