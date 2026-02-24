# Pi Proxy Setup — Overhead Tracker

Complete record of how the Raspberry Pi proxy was configured.
Written: February 2026 — Updated: February 2026

---

## Hardware

- **Device:** Raspberry Pi 3B+
- **OS:** Raspberry Pi OS Lite 64-bit (headless)
- **Local IP:** 192.168.86.24
- **Hostname:** piproxy

---

## What it does

Acts as a caching proxy between the flight tracker clients and airplanes.live.
Caches each unique query for 10 seconds so multiple devices (web app + ESP32)
can refresh freely without hitting API rate limits (HTTP 429).

```
Browser / ESP32  →  Pi proxy (:3000)  →  airplanes.live
                         ↑
                  Cloudflare Tunnel
                  api.overheadtracker.com
                  dashboard.overheadtracker.com
```

---

## SD Card & OS Setup

Flashed via **Raspberry Pi Imager** on Windows with these settings:

- **Device:** Raspberry Pi 3
- **OS:** Raspberry Pi OS Lite (64-bit)
- **Hostname:** `piproxy`
- **SSH:** enabled, password authentication
- **Username:** `pi`
- **WiFi SSID:** chartreuse
- **WiFi country:** AU

---

## SSH Access

From any machine on the local network:

```bash
ssh pi@piproxy.local
# or
ssh pi@192.168.86.24
```

**Recommended:** Use **VS Code with the Remote SSH extension** for editing files.
Connect to `pi@piproxy.local`, then open `/home/pi/proxy` as the workspace.
Much easier than nano or scp.

---

## Software Installed

### Node.js 20

```bash
sudo apt update && sudo apt upgrade -y
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs
```

### Proxy server dependencies

```bash
mkdir ~/proxy && cd ~/proxy
npm init -y
npm install express node-fetch
```

### PM2 (process manager — keeps services alive across reboots)

```bash
sudo npm install -g pm2
```

### cloudflared (Cloudflare Tunnel client)

```bash
curl -L https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-arm64 -o cloudflared
chmod +x cloudflared
sudo mv cloudflared /usr/local/bin/
```

---

## File Structure

```
/home/pi/proxy/
├── server.js          # Main proxy server
├── dashboard.html     # Browser dashboard UI
├── display.py         # 3.5" TFT dashboard (framebuffer)
├── package.json
└── data/
    └── peak.json      # Persisted hourly traffic counts (auto-created)
```

---

## Proxy Server

**File:** `/home/pi/proxy/server.js`

Includes: caching, stats tracking, peak hour persistence, dashboard serving, and shutdown endpoint.

```javascript
const express = require('express');
const fetch = (...args) => import('node-fetch').then(({default: f}) => f(...args));
const fs = require('fs');
const path = require('path');

const app       = express();
const PORT      = 3000;
const CACHE_MS  = 10000;
const DATA_DIR  = path.join(__dirname, 'data');
const PEAK_FILE = path.join(DATA_DIR, 'peak.json');

// Ensure data directory exists
if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR);

// Load persisted peak data or start fresh
function loadPeak() {
  try {
    if (fs.existsSync(PEAK_FILE)) {
      const raw = JSON.parse(fs.readFileSync(PEAK_FILE, 'utf8'));
      if (Array.isArray(raw) && raw.length === 24) return raw;
    }
  } catch(e) { console.error('[PEAK] Load error:', e.message); }
  return new Array(24).fill(0);
}

function savePeak() {
  try { fs.writeFileSync(PEAK_FILE, JSON.stringify(stats.peakHour)); }
  catch(e) { console.error('[PEAK] Save error:', e.message); }
}

const cache = new Map();

const startTime = Date.now();
const stats = {
  totalRequests: 0,
  cacheHits:     0,
  cacheMisses:   0,
  errors:        0,
  peakHour:      loadPeak(),
  uniqueClients: new Set(),
};

app.use((req, res, next) => {
  res.header('Access-Control-Allow-Origin', '*');
  next();
});

// Dashboard HTML
app.get('/', (req, res) => {
  res.sendFile(__dirname + '/dashboard.html');
});

// Stats endpoint
app.get('/stats', (req, res) => {
  const uptimeSec = Math.floor((Date.now() - startTime) / 1000);
  const days  = Math.floor(uptimeSec / 86400);
  const hours = Math.floor((uptimeSec % 86400) / 3600);
  const mins  = Math.floor((uptimeSec % 3600) / 60);
  const hitRate = stats.totalRequests > 0
    ? ((stats.cacheHits / stats.totalRequests) * 100).toFixed(1)
    : '0.0';

  res.json({
    uptime:        `${days}d ${hours}h ${mins}m`,
    totalRequests: stats.totalRequests,
    cacheHits:     stats.cacheHits,
    cacheMisses:   stats.cacheMisses,
    cacheHitRate:  hitRate + '%',
    errors:        stats.errors,
    uniqueClients: stats.uniqueClients.size,
    cacheEntries:  cache.size,
    peakHour:      stats.peakHour,
  });
});

// Peak hour breakdown endpoint
app.get('/peak', (req, res) => {
  const total = stats.peakHour.reduce((a, b) => a + b, 0);
  const max   = Math.max(...stats.peakHour, 1);
  const now   = new Date().getHours();

  const hours = stats.peakHour.map((count, i) => ({
    hour:    i,
    label:   String(i).padStart(2, '0') + ':00',
    count,
    pct:     total > 0 ? ((count / total) * 100).toFixed(1) : '0.0',
    bar:     Math.round((count / max) * 20),
    current: i === now,
  }));

  const peakIdx = stats.peakHour.indexOf(max);

  res.json({
    hours,
    total,
    peakHour:    peakIdx,
    peakLabel:   String(peakIdx).padStart(2, '0') + ':00',
    peakCount:   max,
    currentHour: now,
  });
});

// Shutdown endpoint (PM2 will auto-restart)
app.get('/shutdown', (req, res) => {
  savePeak();
  res.json({ message: 'Proxy shutting down...' });
  setTimeout(() => process.exit(0), 500);
});

// Flights endpoint
app.get('/flights', async (req, res) => {
  const { lat, lon, radius } = req.query;
  if (!lat || !lon || !radius) return res.status(400).json({ error: 'lat, lon, radius required' });

  stats.totalRequests++;
  stats.peakHour[new Date().getHours()]++;
  savePeak();
  const ip = req.headers['x-forwarded-for'] || req.socket.remoteAddress || 'unknown';
  stats.uniqueClients.add(ip);

  const key = `${lat},${lon},${radius}`;
  const now  = Date.now();
  const hit  = cache.get(key);

  if (hit && (now - hit.timestamp) < CACHE_MS) {
    console.log(`[CACHE HIT] ${key}`);
    stats.cacheHits++;
    return res.json(hit.data);
  }

  try {
    console.log(`[FETCH] ${key}`);
    stats.cacheMisses++;
    const url      = `https://api.airplanes.live/v2/point/${lat}/${lon}/${radius}`;
    const response = await fetch(url, { signal: AbortSignal.timeout(8000) });
    if (!response.ok) throw new Error(`API returned ${response.status}`);
    const data = await response.json();
    cache.set(key, { data, timestamp: now });
    res.json(data);
  } catch(e) {
    console.error(`[ERROR] ${e.message}`);
    stats.errors++;
    res.status(502).json({ error: e.message });
  }
});

// Save peak every 5 minutes as safety net
setInterval(savePeak, 5 * 60 * 1000);

app.listen(PORT, '0.0.0.0', () => {
  console.log(`Proxy running on port ${PORT}`);
});
```

---

## Dashboard

**File:** `/home/pi/proxy/dashboard.html`

Served at `dashboard.overheadtracker.com` (and at `/` on port 3000).

Features:
- Live proxy status (online/offline indicator)
- Uptime, total requests, cache hits/misses, errors, unique clients
- Cache hit rate with progress bar
- Peak hour bar chart (24h) — amber = current hour, green = all-time peak
- Refresh button and stop proxy button (with confirmation dialog)
- Auto-refreshes every 10 seconds

---

## API Endpoints

| Endpoint     | Description |
|--------------|-------------|
| `GET /`      | Serves dashboard.html |
| `GET /flights?lat=&lon=&radius=` | Proxied + cached flight data from airplanes.live |
| `GET /stats` | JSON — uptime, request counts, cache stats, peak hour array |
| `GET /peak`  | JSON — full 24-hour breakdown with labels and percentages |
| `GET /shutdown` | Gracefully stops the process (PM2 restarts it automatically) |

---

## Cloudflare Tunnel

### Account
- Cloudflare account linked to **overheadtracker.com**
- Tunnel name: **overhead-tracker**
- Tunnel ID: `eca4af72-74e4-49db-9fbe-66dfb50c586d`
- Public URLs:
  - **https://api.overheadtracker.com** — proxy API
  - **https://dashboard.overheadtracker.com** — dashboard UI

### Config file
**File:** `/home/pi/.cloudflared/config.yml`

```yaml
tunnel: eca4af72-74e4-49db-9fbe-66dfb50c586d
credentials-file: /home/pi/.cloudflared/eca4af72-74e4-49db-9fbe-66dfb50c586d.json

ingress:
  - hostname: api.overheadtracker.com
    service: http://localhost:3000
  - hostname: dashboard.overheadtracker.com
    service: http://localhost:3000
  - service: http_status:404
```

### DNS records
Added automatically by:
```bash
cloudflared tunnel route dns overhead-tracker api.overheadtracker.com
cloudflared tunnel route dns overhead-tracker dashboard.overheadtracker.com
```

---

## 3.5" TFT Display

**Hardware:** Generic MPI3501 clone (ILI9486 SPI, 480×320, resistive touch)
**Driver:** `tft35a` dtoverlay (installed via goodtft/LCD-show `LCD35-show`)

### How it works

`display.py` uses pygame with `SDL_VIDEODRIVER=offscreen` to render in memory,
then converts the surface to RGB565 and writes directly to `/dev/fb1`.
No X server or desktop environment needed.

Dependencies: `python3-pygame`, `python3-numpy`, `requests`

### Relevant boot config (`/boot/firmware/config.txt`)

```
dtparam=spi=on
dtoverlay=tft35a:rotate=270
```

### Troubleshooting

| Symptom | Check |
|---|---|
| Black screen after reboot | `ls /dev/fb1` — if missing, driver didn't load; check config.txt |
| `pm2 logs display` shows errors | Check for numpy/pygame import errors |
| Display shows "proxy unreachable" | proxy PM2 service is down; `pm2 restart proxy` |

---

## PM2 Services

| Name    | Command                                        | Purpose               |
|---------|------------------------------------------------|-----------------------|
| proxy   | `node /home/pi/proxy/server.js`               | Flight data proxy     |
| tunnel  | `cloudflared tunnel run overhead-tracker`     | Cloudflare Tunnel     |
| display | `python3 /home/pi/proxy/display.py`           | 3.5" TFT dashboard    |

### Setup commands
```bash
pm2 start server.js --name proxy
pm2 start "cloudflared tunnel run overhead-tracker" --name tunnel
pm2 start "python3 /home/pi/proxy/display.py" --name display
pm2 save
pm2 startup  # then run the printed sudo command
```

### Useful PM2 commands
```bash
pm2 status              # check all 3 services are online
pm2 logs proxy          # see [FETCH] and [CACHE HIT] activity
pm2 logs tunnel         # check tunnel connection status
pm2 logs display        # check display rendering errors
pm2 restart proxy       # restart proxy after code changes
pm2 restart tunnel      # restart if tunnel drops
pm2 restart display     # restart after display.py changes
```

---

## Domain — overheadtracker.com

Registered through Cloudflare Registrar.

### DNS records

| Type  | Name        | Value                           | Proxy |
|-------|-------------|---------------------------------|-------|
| CNAME | `@`         | `greystoke1337.github.io`      | On    |
| CNAME | `www`       | `greystoke1337.github.io`      | On    |
| CNAME | `api`       | `eca4af72-...cfargotunnel.com` | On    |
| CNAME | `dashboard` | `eca4af72-...cfargotunnel.com` | On    |

---

## Client Configuration

### Web app (index.html)
```
https://api.overheadtracker.com/flights?lat=LAT&lon=LON&radius=RADIUS
```

### ESP32 (.ino)
```
http://192.168.86.24:3000/flights?lat=LAT&lon=LON&radius=RADIUS
```

---

## If the Pi reboots

Both `proxy` and `tunnel` start automatically via PM2.
Peak hour data is restored from `~/proxy/data/peak.json` automatically.
No manual intervention needed.

```bash
pm2 status  # verify both are online after reboot
```

---

## If something breaks

| Symptom | Check |
|---|---|
| `Cannot GET /` on dashboard | Add root route to server.js, restart proxy |
| dashboard.overheadtracker.com not loading | Check Cloudflare tunnel config has dashboard hostname in ingress |
| Stats all zero after restart | Normal — peak.json restores hourly data but session counts reset |
| Website shows fetch error | `pm2 status` — is proxy online? |
| api.overheadtracker.com unreachable | `pm2 logs tunnel` — is tunnel connected? |
| ESP32 shows HTTP ERR | Is Pi powered on? ping 192.168.86.24 |
| 429 errors from airplanes.live | Cache may have been disabled — check server.js |
| Tunnel restart count very high | `pm2 logs tunnel --lines 20` — check for auth or memory errors |

---

## Features Planned / In Progress

- [ ] Aircraft of the day (fastest, highest, rarest type — resets midnight)
- [ ] Filtered `/rare` endpoint (military, bizjets, uncommon types)
- [ ] Traffic heatmap (position accumulation over time → JSON for web overlay)
