// Settings page on http://127.0.0.1:<port>. Localhost only; the Host check stops
// DNS-rebinding pages, and requiring a JSON body makes browsers preflight any
// cross-site POST (which is never answered), so other websites can't change settings.

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');

const PAGE = path.join(__dirname, '..', 'public', 'index.html');

function createServer(app, port) {
  const allowedHosts = new Set([`127.0.0.1:${port}`, `localhost:${port}`]);

  const json = (res, code, obj) => {
    const body = JSON.stringify(obj);
    res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    res.end(body);
  };

  const readBody = (req) => new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (c) => {
      size += c.length;
      if (size > 16384) { reject(new Error('body too large')); req.destroy(); return; }
      chunks.push(c);
    });
    req.on('end', () => {
      try { resolve(JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}')); }
      catch { reject(new Error('bad JSON')); }
    });
    req.on('error', reject);
  });

  return http.createServer(async (req, res) => {
    if (!allowedHosts.has(req.headers.host || '')) {
      res.writeHead(403); return res.end('forbidden');
    }
    const url = new URL(req.url, `http://${req.headers.host}`);
    try {
      if (req.method === 'GET' && url.pathname === '/') {
        res.writeHead(200, {
          'Content-Type': 'text/html; charset=utf-8',
          'Cache-Control': 'no-store',
          'Content-Security-Policy': "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'",
          'X-Frame-Options': 'DENY',
        });
        return res.end(fs.readFileSync(PAGE));
      }
      if (req.method === 'GET' && url.pathname === '/api/state') return json(res, 200, await app.state());
      if (req.method === 'GET' && url.pathname === '/api/ports') return json(res, 200, await app.ports());

      if (req.method === 'POST') {
        if (!(req.headers['content-type'] || '').startsWith('application/json')) {
          return json(res, 415, { error: 'JSON only' });
        }
        const body = await readBody(req);
        if (url.pathname === '/api/config') return json(res, 200, await app.updateConfig(body));
        if (url.pathname === '/api/action') return json(res, 200, await app.action(String(body.action || ''), body));
      }
      json(res, 404, { error: 'not found' });
    } catch (e) {
      json(res, 400, { error: e.message });
    }
  });
}

module.exports = { createServer };
