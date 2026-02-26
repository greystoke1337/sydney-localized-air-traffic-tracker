# Overhead Tracker — Product Specification

## What It Is

Overhead Tracker is a real-time aircraft tracking system that answers one question: **"What planes are flying above me right now?"** Given a location anywhere in the world, it fetches live ADS-B transponder data, filters to aircraft within a user-defined radius and altitude floor, and presents them sorted nearest-first with flight details, a live map, and phase classification.

---

## Why It Exists

Commercial flight tracking apps (FlightRadar24, etc.) are world-map oriented — they show everything everywhere and require you to find your location. This project is **location-first and overhead-first**: you set one location and the system only surfaces aircraft directly above it, cycling through them automatically. The secondary motivation is a physical hardware display that requires no phone or browser — just a self-contained device on a shelf.

---

## System Architecture

The project has three deployed components:

```
┌─────────────────────┐     HTTPS      ┌────────────────────────────────┐
│  Web app            │ ──────────────▶│ api.overheadtracker.com         │
│  (GitHub Pages)     │                │ (Cloudflare Tunnel →            │
│  index.html         │                │  Raspberry Pi proxy :3000)      │
└─────────────────────┘                └──────────────┬─────────────────┘
                                                       │ HTTPS
┌─────────────────────┐     HTTP LAN   │               ▼
│  ESP32 TFT display  │ ──────────────▶│        airplanes.live ADS-B API
│  (Freenove FNK0103S)│                │
└─────────────────────┘     ◀──────────┘ cached (10s TTL)
```

### Component 1: Web app (`index.html`)

A single self-contained HTML file. No build step, no framework, no backend required to run locally. Deployed to GitHub Pages at `overheadtracker.com` by pushing to `main`.

### Component 2: Raspberry Pi proxy (`server.js`)

A Node.js/Express server on a Pi 3B+ at `192.168.x.x:3000`. Its sole job is to cache airplanes.live API responses for 10 seconds so multiple clients (web app + ESP32) can refresh at 15-second intervals without triggering API rate limits. Exposed publicly over a Cloudflare Tunnel as `api.overheadtracker.com`. The Pi also runs a `display.py` process that renders a status dashboard to a local 3.5" TFT over framebuffer. Managed by PM2 for auto-restart.

### Component 3: ESP32 TFT display (`.ino` firmware)

A standalone physical device (Freenove FNK0103S, 4" 480×320 ST7796 touchscreen). Runs entirely independently of the web app — polls the local proxy directly over LAN. Configured on first boot via a captive portal Wi-Fi form. Displays one flight at a time, cycling every 8 seconds, with a touchscreen button to change the geofence radius.

---

## Data Pipeline

1. **Geocoding** — user's location string is resolved to lat/lon via Nominatim (OpenStreetMap). No API key needed.
2. **Fetch** — proxy is queried with `lat`, `lon`, `radius` (4× the geofence in nautical miles, to cast a wider net than the actual filter).
3. **Filter** — results are reduced to aircraft within the geofence radius *and* above the altitude floor.
4. **Sort** — remaining aircraft sorted by haversine distance, closest first.
5. **Render** — flight info, map, photo, altitude bar, and phase colour rendered for the current aircraft.
6. **Repeat** — every 15 seconds automatically; any in-flight request is aborted before starting the next.

---

## Feature Set (Web App)

| Category | Features |
|---|---|
| **Location** | Worldwide search, persisted in `localStorage`, shareable via `?location=` URL param |
| **Filtering** | Geofence radius slider (2–20 km), altitude floor slider (200–5,000 ft) |
| **Flight data** | Callsign, airline name (from ICAO prefix), full aircraft type name, altitude (FL or QNH ft), ground speed, heading, vertical rate |
| **Flight phase** | TAKING OFF / CLIMBING / CRUISING / DESCENDING / APPROACH / LANDING / OVERHEAD — derived from altitude + vertical speed |
| **Map** | Leaflet with dark CartoDB tiles, geofence circle, aircraft dot, dashed line to location, speed-scaled heading vector with chevron |
| **Photo** | Aircraft registration photo from Planespotters.net, with halftone overlay |
| **Alerts** | Emergency squawk highlighting (7700/7600/7500), optional radar ping sound on new #1 aircraft |
| **Navigation** | Arrow key browsing, NEAREST button, session flight log |
| **UX** | CRT scanline aesthetic, phase colour bleed on info block border, altitude bar, mobile-responsive |

## Feature Set (ESP32 Display)

| Category | Features |
|---|---|
| **Config** | Captive portal on first boot — set Wi-Fi SSID/password and location; geocodes via Nominatim and stores to NVS |
| **Display** | Callsign, airline, aircraft type, altitude, distance, flight phase on a 480×320 TFT at 15s refresh / 8s cycle |
| **Touch** | GEO pill cycles geofence (5K / 10K / 50K); CFG pill opens captive portal |
| **Resilience** | Falls back from proxy → direct API → SD card cache (`cache.json`) |

---

## External Dependencies

| Service | Used for | Auth |
|---|---|---|
| airplanes.live | Live ADS-B transponder positions | None |
| Nominatim / OpenStreetMap | Location geocoding | None |
| Planespotters.net | Aircraft registration photos | None |
| CartoDB / OpenStreetMap | Map tiles | None |
| Cloudflare Tunnel | Public HTTPS ingress to Pi | Tunnel token |

---

## Deployment

- **Web app** — `git push` to `main` → GitHub Pages auto-deploys within ~60 seconds
- **Proxy** — SSH to `pi@piproxy.local`, edit `/home/pi/proxy/server.js`, `pm2 restart proxy`
- **ESP32** — `./build.sh` (compiles with `arduino-cli` and uploads to COM4)
