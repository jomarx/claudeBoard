// The LAN listener boards connect to in Wi-Fi sync: ws://<this PC>:<port>/board.
//
// It speaks only the board protocol. The settings page and its API stay on 127.0.0.1
// (server.js); nothing here can change settings or read the config. A board's first message
// must be {"type":"auth","token":"..."} with the pairing token, within 5 s, or the socket
// closes; five bad tokens from one address in a minute lock that address out for a minute.
// It only listens while Sync is Wi-Fi.

const http = require('node:http');
const crypto = require('node:crypto');
const { WebSocketServer } = require('ws');
const { log } = require('./log');

const AUTH_MS = 5000;
const PING_MS = 15000;

function tokenOk(want, got) {
  const a = Buffer.from(String(want || '')), b = Buffer.from(String(got || ''));
  return a.length > 0 && a.length === b.length && crypto.timingSafeEqual(a, b);
}

function createLan({ getCfg, onBoard }) {
  let server = null, wss = null, pinger = null;
  const state = { listening: false, port: null, error: '' };
  const failures = new Map();   // ip -> { n, first, until }

  const blocked = (ip) => (failures.get(ip)?.until || 0) > Date.now();
  const fail = (ip) => {
    const now = Date.now();
    let f = failures.get(ip);
    if (!f || now - f.first > 60000) f = { n: 0, first: now, until: 0 };
    if (++f.n >= 5) f.until = now + 60000;
    failures.set(ip, f);
  };

  function start(port) {
    server = http.createServer((req, res) => { res.writeHead(404); res.end(); });
    wss = new WebSocketServer({ server, path: '/board', maxPayload: 16384 });
    wss.on('connection', (ws, req) => {
      const ip = String(req.socket.remoteAddress || '').replace(/^::ffff:/, '');
      if (blocked(ip)) { ws.close(4003, 'too many attempts'); return; }
      const timer = setTimeout(() => ws.close(4001, 'no token'), AUTH_MS);
      ws.once('message', (data, isBinary) => {
        clearTimeout(timer);
        let m = null;
        try { m = JSON.parse(String(data)); } catch { /* not JSON */ }
        if (isBinary || !m || m.type !== 'auth' || !tokenOk(getCfg().lan.token, m.token)) {
          fail(ip);
          log(`Wi-Fi: refused ${ip} (no or wrong pairing token)`);
          ws.close(4001, 'bad token');
          return;
        }
        failures.delete(ip);
        ws.isAlive = true;
        ws.on('pong', () => { ws.isAlive = true; });
        ws.send(JSON.stringify({ type: 'authok' }));
        log(`Wi-Fi: board connected from ${ip}`);
        // Synchronous, so the hello and cfg the board sends right after its token are
        // caught by the board's own listener.
        onBoard(ws, ip);
      });
    });
    // Drop boards that stop answering pings (power cut, Wi-Fi gone) instead of waiting for TCP.
    pinger = setInterval(() => {
      for (const ws of wss.clients) {
        if (ws.isAlive === undefined) continue;   // still authenticating
        if (!ws.isAlive) { ws.terminate(); continue; }
        ws.isAlive = false;
        ws.ping();
      }
    }, PING_MS);
    server.on('error', (e) => {
      state.listening = false;
      state.error = e.code === 'EADDRINUSE' ? `port ${port} is already in use` : e.message;
      log('Wi-Fi listener:', state.error);
    });
    server.listen(port, '0.0.0.0', () => {
      Object.assign(state, { listening: true, port, error: '' });
      log(`Wi-Fi: listening for boards on port ${port}`);
    });
  }

  function stop() {
    clearInterval(pinger);
    if (wss) for (const ws of wss.clients) ws.terminate();
    if (wss) wss.close();
    if (server) server.close();
    server = wss = pinger = null;
    Object.assign(state, { listening: false, port: null, error: '' });
  }

  // Listen when Sync is Wi-Fi, on the configured port; stop otherwise.
  function apply() {
    const cfg = getCfg();
    const want = cfg.board.sync === 'wifi' ? cfg.lan.port : null;
    if (want === state.port && (state.listening || state.error)) return;
    if (server) stop();
    if (want) start(want);
  }

  return { apply, stop, state };
}

module.exports = { createLan };
