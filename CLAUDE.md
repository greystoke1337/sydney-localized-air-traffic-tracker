# Claude Code Instructions

## Project Overview

**Overhead Tracker** is a real-time aircraft tracking system.
It answers: *"What planes are flying directly above me right now?"*

Three components:

| Component | Tech | Hosted at |
|-----------|------|-----------|
| Web app | Single-file HTML + vanilla JS | GitHub Pages (auto-deploy on push to `main`) |
| Pi proxy | Node.js / Express on Raspberry Pi 3B+ | api.overheadtracker.com (Cloudflare Tunnel) |
| ESP32 firmware | Arduino C++ | Physical device (USB upload via `build.sh`) |

Live URL: https://greystoke1337.github.io/sydney-localized-air-traffic-tracker/
Custom domain: https://overheadtracker.com

---

## Repository Layout

```
index.html                  # Entire web app (single file, ~118 KB)
build.sh                    # ESP32 compile + upload helper (arduino-cli)
PI_PROXY_SETUP.md           # Raspberry Pi proxy setup guide + full server source
SPEC.md                     # Product specification and feature matrix
README.md                   # User-facing documentation
CNAME                       # GitHub Pages custom domain
tracker_live_fnk0103s/      # ESP32 hardware project
  tracker_live_fnk0103s.ino # Arduino firmware (~59 KB)
  enclosure/                # 3D-printable case files (STL/STEP)
.claude/agents/             # Specialist sub-agents
  backend-specialist.md
  technical-writer.md
  ux-designer.md
```

---

## Branch Policy

Always develop and push directly to **`main`**.
Do **not** create feature branches.
Pushes to `main` deploy automatically to GitHub Pages within ~60 seconds.

---

## Pending Deployments — Complete when home

> **Pi proxy** (server.js updated in repo but not yet on the Pi):
> ```bash
> ssh pi@192.168.86.24
> # Open PI_PROXY_SETUP.md in VS Code Remote SSH, copy the server.js block to /home/pi/proxy/server.js, then:
> pm2 restart proxy
> pm2 logs proxy --lines 10   # confirm no errors; look for [FETCH] weather:...
> ```

> **ESP32** (firmware updated in repo but not yet flashed):
> ```bash
> ./build.sh
> ```

---

## External APIs Used by the Web App

| Service | Purpose | Auth |
|---------|---------|------|
| airplanes.live | Live ADS-B flight data | None (via Pi proxy cache) |
| Open-Meteo | Current weather conditions | None (via Pi proxy cache) |
| Nominatim / OpenStreetMap | Location geocoding | None |
| Planespotters.net | Aircraft photos by registration | None |
| CartoDB | Dark map tiles (Leaflet) | None |

The Pi proxy at `api.overheadtracker.com` caches airplanes.live responses for 10 s and Open-Meteo weather for 10 min.

---

## Key Concepts

- **Geofence**: User-configurable radius (2–20 km) around a chosen location. Only aircraft inside the fence are shown.
- **Altitude floor**: Filters out aircraft below a configurable altitude (200–5 000 ft AGL).
- **Flight phase detection**: LANDING / TAKING OFF / APPROACH / DESCENDING / CLIMBING / OVERHEAD — derived from speed, altitude, and vertical rate.
- **Weather**: Current conditions fetched from Open-Meteo via the Pi proxy (`/weather?lat=&lon=`). Proxy caches for 10 min. Web app refreshes every 15 min; ESP32 every 15 min. Fields: temp, feels_like, humidity, condition (WMO code → text), wind_speed (km/h), wind_cardinal, uv_index, utc_offset_secs.
- **ESP32 weather screen**: Shown automatically when no aircraft are in range. Shows NTP local-time clock (HH:MM + date) plus weather grid. Tap the **WX** pill in the header to toggle it at any time.
- **No build step**: `index.html` is deployed as-is; never introduce a bundler or external dependency that requires a build pipeline.
- **No framework**: The web app uses vanilla JS and the browser's built-in APIs only. Do not add React, Vue, or similar.

---

## Common Tasks

### Web app change
1. Edit `index.html`.
2. Test by opening the file in a browser (`file://` works — no server needed).
3. `git add index.html && git commit -m "..." && git push origin main`
4. Verify at the live URL after ~60 s.

### Pi proxy change
- The full server source and setup instructions are in `PI_PROXY_SETUP.md`.
- The Pi lives at `192.168.86.24` (local network only; SSH in then `pm2 restart proxy`).
- Tunnel is managed by `cloudflared` running as a systemd service.

### ESP32 firmware change
1. Edit `tracker_live_fnk0103s/tracker_live_fnk0103s.ino`.
2. Run `./build.sh` to compile and upload (requires Arduino IDE + `arduino-cli` on Windows).
3. Serial monitor: `./build.sh monitor`.

---

## Code Style

- **No comments** on self-evident code; add comments only when logic is non-obvious.
- **No docstrings / JSDoc** unless already present in that section.
- **Minimal abstractions**: prefer three clear lines over a premature helper function.
- **Security**: never embed API keys, credentials, or private IPs in committed code.
- **Accessibility**: maintain keyboard navigation support (arrow keys for table rows).

---

## Specialist Agents

Use these sub-agents for domain-specific tasks:

| Agent | File | When to use |
|-------|------|-------------|
| Backend Specialist | `.claude/agents/backend-specialist.md` | Pi proxy, ESP32 firmware, Cloudflare Tunnel, API integrations |
| Technical Writer | `.claude/agents/technical-writer.md` | README, PI_PROXY_SETUP.md, inline comments, documentation |
| UX Designer | `.claude/agents/ux-designer.md` | UI layout, CRT aesthetic, accessibility, mobile responsiveness |
