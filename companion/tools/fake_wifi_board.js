// A pretend Wi-Fi board, for testing the companion's LAN side without hardware.
//
//   node tools/fake_wifi_board.js <host> <port> <token> [seconds] [event name:value ...]
//   node tools/fake_wifi_board.js <host> <port> --replay <hmac>   (answers with a captured hmac)
//
// Connects to ws://host:port/board like the firmware does: answers the server's challenge
// with HMAC-SHA256(token, nonce), then sends hello and cfg after authok, prints every message
// it receives, sends the given events after a second, and exits after [seconds] (default 6).
// Prints the hmac it sent, so a replay can be tried. Exit code 0 = accepted and got frames.

const crypto = require('node:crypto');
const WebSocket = require('ws');

const [host = '127.0.0.1', port = '47821', token = '', secs = '6', ...events] = process.argv.slice(2);
const replay = token === '--replay' ? secs : null;
const ws = new WebSocket(`ws://${host}:${port}/board`);
const got = [];

ws.on('message', (data) => {
  const m = JSON.parse(String(data));
  got.push(m.type);
  if (m.type === 'challenge') {
    const hmac = replay || crypto.createHmac('sha256', token).update(m.nonce).digest('hex');
    console.log('answered challenge with hmac', hmac);
    ws.send(JSON.stringify({ type: 'auth', hmac }));
    return;
  }
  if (m.type === 'authok') {
    ws.send(JSON.stringify({ type: 'hello', fw: '0.5.0', w: 240, h: 320, cfg: true, apps: true, net: true, link: 'wifi' }));
    ws.send(JSON.stringify({ type: 'cfg', brightness: 200, sync: 'wifi' }));
    setTimeout(() => {
      for (const e of events) {
        const [name, value] = e.split(':');
        ws.send(JSON.stringify({ type: 'event', name, value: Number(value) }));
        console.log('sent event', name, value);
      }
    }, 1000);
  }
  // A real board answers frames: an ack for usage, hello when asked.
  if (m.type === 'usage') ws.send(JSON.stringify({ type: 'ack', seq: m.seq }));
  if (m.type === 'hello') ws.send(JSON.stringify({ type: 'hello', fw: '0.5.0', cfg: true, apps: true, net: true, link: 'wifi' }));
  const s = JSON.stringify(m);
  console.log('got', s.length > 110 ? s.slice(0, 110) + '...' : s);
});
ws.on('close', (code, reason) => {
  console.log(`closed ${code} ${String(reason)} | received: ${[...new Set(got)].join(', ') || 'nothing'}`);
  process.exit(got.includes('authok') && got.length > 2 ? 0 : 1);
});
ws.on('error', (e) => { console.log('error', e.message); });
setTimeout(() => ws.close(1000, 'test done'), Number(replay ? 3 : secs) * 1000);
