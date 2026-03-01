#!/usr/bin/env node
// Mock proxy server for testing ESP32 firmware resilience.
// Zero dependencies — uses only Node.js built-in modules.
//
// Usage:  node tools/mock-proxy.js [mode] [port]
//
// Modes:
//   normal   — valid flight + weather JSON (default)
//   timeout  — accepts TCP, never responds
//   error503 — returns 503 Service Unavailable
//   error502 — returns 502 Bad Gateway
//   corrupt  — returns 200 with broken JSON
//   partial  — returns 200, drops connection mid-body
//   slow     — waits 4s before valid response

const http = require('http');
const os = require('os');

const mode = process.argv[2] || 'normal';
const port = parseInt(process.argv[3]) || 3000;

const MODES = ['normal', 'timeout', 'error503', 'error502', 'corrupt', 'partial', 'slow'];
if (!MODES.includes(mode)) {
  console.error(`Unknown mode: "${mode}"\nAvailable: ${MODES.join(', ')}`);
  process.exit(1);
}

const SAMPLE_FLIGHTS = JSON.stringify({
  ac: [
    { flight: 'QFA1    ', r: 'VH-OQA', t: 'A388', lat: -33.87, lon: 151.21,
      alt_baro: 35000, gs: 480, baro_rate: 0, track: 180, squawk: '1234',
      dep: 'YMML', arr: 'YSSY' },
    { flight: 'VOZ456  ', r: 'VH-YIA', t: 'B738', lat: -33.88, lon: 151.20,
      alt_baro: 12000, gs: 280, baro_rate: -1200, track: 90, squawk: '2345',
      dep: 'YBBN', arr: 'YSSY' }
  ],
  total: 2, now: Date.now() / 1000
});

const SAMPLE_WEATHER = JSON.stringify({
  temp: 22.5, feels_like: 21.0, humidity: 65,
  condition: 'Partly Cloudy', weather_code: 2,
  wind_speed: 15.0, wind_dir: 180, wind_cardinal: 'S',
  uv_index: 5.0, utc_offset_secs: 36000
});

function getLocalIP() {
  const nets = os.networkInterfaces();
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) return net.address;
    }
  }
  return '127.0.0.1';
}

function timestamp() {
  return new Date().toISOString().slice(11, 23);
}

function logReq(req, note) {
  console.log(`  [${timestamp()}] ${req.method} ${req.url} -> ${note}`);
}

const handlers = {
  normal(req, res) {
    if (req.url.startsWith('/flights')) {
      logReq(req, '200 flights');
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(SAMPLE_FLIGHTS);
    } else if (req.url.startsWith('/weather')) {
      logReq(req, '200 weather');
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(SAMPLE_WEATHER);
    } else {
      logReq(req, '404');
      res.writeHead(404);
      res.end('Not Found');
    }
  },

  timeout(req, res) {
    logReq(req, 'HANG (no response)');
    // Accept connection but never respond — tests HTTP read timeout
  },

  error503(req, res) {
    logReq(req, '503');
    res.writeHead(503, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Proxy disabled' }));
  },

  error502(req, res) {
    logReq(req, '502');
    res.writeHead(502, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Upstream timeout' }));
  },

  corrupt(req, res) {
    logReq(req, '200 corrupt JSON');
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end('{"ac":[{"flight":"QFA1","lat":-33.87,"lon":151.21,"alt_baro":BROKEN');
  },

  partial(req, res) {
    logReq(req, '200 partial (will drop)');
    res.writeHead(200, { 'Content-Type': 'application/json' });
    const half = SAMPLE_FLIGHTS.slice(0, Math.floor(SAMPLE_FLIGHTS.length / 2));
    res.write(half);
    setTimeout(() => res.destroy(), 500);
  },

  slow(req, res) {
    logReq(req, 'SLOW (4s delay)');
    setTimeout(() => {
      if (req.url.startsWith('/flights')) {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(SAMPLE_FLIGHTS);
        console.log(`  [${timestamp()}]   -> sent response`);
      } else if (req.url.startsWith('/weather')) {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(SAMPLE_WEATHER);
        console.log(`  [${timestamp()}]   -> sent response`);
      } else {
        res.writeHead(404);
        res.end();
      }
    }, 4000);
  }
};

const expectations = {
  normal:   'ESP32 shows flights normally. Serial: [PROXY] OK',
  timeout:  'ESP32 HTTP read timeout after 5s. Serial: [PROXY] HTTP -1\n  Falls back to direct API.',
  error503: 'ESP32 gets non-200, returns empty. Serial: [PROXY] HTTP 503\n  Falls back to direct API.',
  error502: 'ESP32 gets non-200, returns empty. Serial: [PROXY] HTTP 502\n  Falls back to direct API.',
  corrupt:  'ESP32 gets 200 but JSON parse fails. Serial: JSON parse error\n  Falls back to SD cache.',
  partial:  'ESP32 gets 200, connection drops mid-read. Serial: JSON parse error or partial data\n  Falls back to SD cache.',
  slow:     'ESP32 waits ~4s then gets valid response. Serial: [PROXY] OK ... (slow)'
};

const server = http.createServer(handlers[mode]);
server.listen(port, () => {
  const ip = getLocalIP();
  console.log(`\n=== MOCK PROXY: ${mode} mode on port ${port} ===\n`);
  console.log(`Local IP: ${ip}`);
  console.log(`Set PROXY_HOST in tracker_live_fnk0103s.ino line 37 to "${ip}"`);
  console.log(`Then:  ./build.sh compile && ./build.sh upload COM4`);
  console.log(`       ./build.sh monitor COM4  (in another terminal)\n`);
  console.log(`Expected ESP32 behavior:`);
  console.log(`  ${expectations[mode]}\n`);
  console.log('Waiting for requests... (Ctrl-C to stop)\n');
});
