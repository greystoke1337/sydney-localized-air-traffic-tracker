# Overhead // Live Aircraft Tracker

Shows which aircraft are flying directly above any location in the world. Single HTML file, no dependencies, no build step.

Live: [overheadtracker.com](https://www.overheadtracker.com/)

---

## Features

- Worldwide location search with adjustable geofence radius (2–20 km) and altitude floor (200–5,000 ft)
- Settings persist across sessions; share any location via `?location=` URL param
- Live ADS-B data via [airplanes.live](https://airplanes.live), routed through a self-hosted proxy — no API key needed
- 15-second auto-refresh with manual override
- Flight phase detection: LANDING, TAKING OFF, APPROACH, DESCENDING, CLIMBING, OVERHEAD
- Airline name from ICAO callsign prefix; full aircraft type names (B789 → B787-9, A20N → A320neo, etc.)
- Emergency squawk highlighting — 7700 / 7600 / 7500 shown in red with a warning
- Leaflet map with geofence circle, aircraft dot, and speed-scaled heading vector
- Aircraft photo from [Planespotters.net](https://planespotters.net) by registration
- Phase colour bleed on the info block, altitude bar, CRT scanline aesthetic
- Keyboard navigation, NEAREST button, session flight log, optional radar ping sound
- Mobile-responsive

---

## How it works

Geocodes the entered location via Nominatim, then queries the Pi proxy for aircraft within a radius 4x the geofence size. Filters down to aircraft inside the geofence and above the altitude floor, sorts by distance, and renders. Repeats every 15 seconds; any in-flight request is aborted before starting the next.

---

## Usage

No server, no build step, no API keys.

```bash
git clone https://github.com/greystoke1337/sydney-localized-air-traffic-tracker.git
cd sydney-localized-air-traffic-tracker
open index.html
```

The app calls `api.overheadtracker.com` (flight data), `nominatim.openstreetmap.org` (geocoding), and `api.planespotters.net` (photos). All HTTPS, all CORS-enabled.

---

## ESP32 hardware display

Standalone physical tracker on the **Freenove FNK0103S** (ESP32 + 4" 480×320 ST7796 touchscreen). Polls the local proxy directly over LAN — no browser needed.

**Hardware:** Freenove FNK0103S, optional 3D-printed enclosure (STL/STEP in [`tracker_live_fnk0103s/enclosure/`](tracker_live_fnk0103s/enclosure/))

**What it shows:** callsign, airline, aircraft type, altitude, distance, flight phase. Cycles through overhead flights every 8 seconds. Tap the header to switch between location presets.

**Libraries** (Arduino Library Manager): `TFT_eSPI` (Freenove version), `ArduinoJson`, `SD`

Edit the top of [`tracker_live_fnk0103s/tracker_live_fnk0103s.ino`](tracker_live_fnk0103s/tracker_live_fnk0103s.ino) before flashing:

```cpp
const char* WIFI_SSID = "your-network";
const char* WIFI_PASS = "your-password";
const char* PROXY_HOST = "192.168.x.x";  // IP of your local proxy
```

```bash
./build.sh            # compile + upload
./build.sh compile    # compile only
./build.sh upload     # upload last build
./build.sh monitor    # serial monitor
```

---

## Data sources

| Source | Data | Key required |
|---|---|---|
| [api.overheadtracker.com](https://api.overheadtracker.com) | Flight data proxy (Raspberry Pi + Cloudflare Tunnel) | No |
| [airplanes.live](https://api.airplanes.live) | Live ADS-B positions (via proxy) | No |
| [Nominatim / OpenStreetMap](https://nominatim.openstreetmap.org) | Location geocoding | No |
| [Planespotters.net](https://planespotters.net) | Aircraft photos | No |
| [Carto](https://carto.com) / OpenStreetMap | Map tiles | No |

---

## Roadmap

- [ ] Airline logo display
- [ ] Push notification when a specific flight appears overhead

---

## License

MIT — do whatever you want with it.
