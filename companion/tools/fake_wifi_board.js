// A pretend Wi-Fi board, for testing the companion's LAN side without hardware.
//
//   node tools/fake_wifi_board.js <host> <port> <token> [seconds] [event name:value ...]
//
// Connects to ws://host:port/board like the firmware does (token, then hello and cfg),
// prints every message it receives, sends the given events after a second, and exits after
// [seconds] (default 6). Exit code 0 = it was accepted and got frames.

const WebSocket = require('ws');

const [host = '127.0.0.1', port = '47821', token = '', secs = '6', ...events] = process.argv.slice(2);
const ws = new WebSocket(`ws://${host}:${port}/board`);
const got = [];

ws.on('open', () => {
  ws.send(JSON.stringify({ type: 'auth', token }));
  ws.send(JSON.stringify({ type: 'hello', fw: '0.5.0', w: 240, h: 320, cfg: true, apps: true, net: true, link: 'wifi' }));
  ws.send(JSON.stringify({ type: 'cfg', brightness: 200, sync: 'wifi' }));
  setTimeout(() => {
    for (const e of events) {
      const [name, value] = e.split(':');
      ws.send(JSON.stringify({ type: 'event', name, value: Number(value) }));
      console.log('sent event', name, value);
    }
  }, 1000);
});
ws.on('message', (data) => {
  const m = JSON.parse(String(data));
  got.push(m.type);
  // A real board answers frames: an ack for usage, its settings for cfg.
  if (m.type === 'usage') ws.send(JSON.stringify({ type: 'ack', seq: m.seq }));
  if (m.type === 'hello') ws.send(JSON.stringify({ type: 'hello', fw: '0.5.0', cfg: true, apps: true, net: true, link: 'wifi' }));
  const s = JSON.stringify(m);
  console.log('got', s.length > 110 ? s.slice(0, 110) + '...' : s);
});
ws.on('close', (code, reason) => {
  console.log(`closed ${code} ${String(reason)} | received: ${[...new Set(got)].join(', ') || 'nothing'}`);
  process.exit(got.includes('authok') && got.length > 1 ? 0 : 1);
});
ws.on('error', (e) => { console.log('error', e.message); });
setTimeout(() => ws.close(1000, 'test done'), Number(secs) * 1000);
