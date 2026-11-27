// A board connected over Wi-Fi: one authenticated WebSocket from lan.js. Same interface
// and events as the USB Board (both are BoardLinks), so the hub treats them alike.

const { BoardLink } = require('./board');

class NetBoard extends BoardLink {
  constructor(socket, ip, { verbose = false } = {}) {
    super({ verbose, label: `Wi-Fi board ${ip}` });
    this.socket = socket;
    this.ip = ip;
    this.since = Date.now();
    this.status({ path: `Wi-Fi ${ip}`, connected: true, ready: false });
    socket.on('message', (data, isBinary) => { if (!isBinary) this.onLine(String(data)); });
    socket.on('close', () => {
      clearTimeout(this.bootTimer);
      this.status({ connected: false, ready: false });
      this.emit('closed');
    });
    // The board sends hello right after its token; ask too, in case that one was missed.
    this.helloAskedAt = Date.now();
    this.send({ type: 'hello' });
    this.bootTimer = setTimeout(() => this.markReady(), 3000);
  }

  send(obj) {
    if (this.socket.readyState !== 1) return false;   // 1 = OPEN
    this.socket.send(JSON.stringify(obj));
    return true;
  }

  close(code = 1000, reason = '') {
    try { this.socket.close(code, reason); } catch { /* already gone */ }
  }
}

module.exports = { NetBoard };
