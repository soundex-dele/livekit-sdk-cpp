// Local development viewer. API credentials stay in this Node process.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const web = path.join(root, 'examples/web_viewer');
const { values } = require('node:util').parseArgs({ options: {
  port: { type: 'string' }, room: { type: 'string' }, url: { type: 'string' },
} });
const port = Number(values.port || process.env.VIEWER_PORT || 8080);
const room = values.room || process.env.VIEWER_ROOM || 'test';
const url = values.url || process.env.LIVEKIT_URL || 'http://127.0.0.1:7880';
const cli = process.env.LIVEKIT_CLI || path.join(root, 'out/windows-x64/Release/lk.exe');
const sdk = path.join(web, 'node_modules/livekit-client/dist/livekit-client.umd.js');
if (!fs.existsSync(sdk)) throw new Error('Run npm ci --prefix examples/web_viewer first.');
const env = { ...process.env,
  LIVEKIT_API_KEY: process.env.LIVEKIT_API_KEY || 'devkey',
  LIVEKIT_API_SECRET: process.env.LIVEKIT_API_SECRET || 'local-dev-secret-change-me-32-chars',
};
const files = new Map([
  ['/', [path.join(web, 'index.html'), 'text/html; charset=utf-8']],
  ['/viewer.js', [path.join(web, 'viewer.js'), 'text/javascript; charset=utf-8']],
  ['/livekit-client.umd.js', [sdk, 'text/javascript; charset=utf-8']],
]);
const allowedHosts = new Set([`127.0.0.1:${port}`, `localhost:${port}`]);
const server = http.createServer((req, res) => {
  const origin = req.headers.origin;
  if (!allowedHosts.has(req.headers.host) ||
      (origin && origin !== `http://${req.headers.host}`)) {
    res.writeHead(403).end('Local same-origin requests only'); return;
  }
  if (req.method !== 'GET') { res.writeHead(405).end(); return; }
  res.setHeader('Cache-Control', 'no-store');
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('Referrer-Policy', 'no-referrer');
  const pathname = new URL(req.url, `http://${req.headers.host}`).pathname;
  try {
    if (pathname === '/session.json') {
      const token = execFileSync(cli, ['token', 'create', '--room', room,
        '--identity', `browser-${require('node:crypto').randomUUID()}`,
        '--no-publish', '--ttl', '3600'], { env, encoding: 'utf8', windowsHide: true, timeout: 10000 }).trim();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ url: url.replace(/^http/, 'ws'), room, token })); return;
    }
    const file = files.get(pathname);
    if (!file) { res.writeHead(404).end('Not found'); return; }
    res.writeHead(200, { 'Content-Type': file[1] });
    res.end(fs.readFileSync(file[0]));
  } catch (error) {
    console.error(error.message);
    res.writeHead(500, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Cannot create viewer session. Check CLI path and server credentials.' }));
  }
});
server.on('error', (error) => { console.error(error.message); process.exitCode = 1; });
server.listen(port, '127.0.0.1', () => {
  console.log(`Viewer: http://127.0.0.1:${port} | room: ${room} | server: ${url}`);
  console.log('Local development only. Ctrl+C to stop.');
});
