# Overhead // Live Aircraft Tracker

Shows which aircraft are flying directly above any location in the world. Single HTML file, no dependencies, no build step.

Live: [overheadtracker.com](https://www.overheadtracker.com/)

---

## Features

- Worldwide location search with adjustable geofence radius (2–20 km) and altitude floor (200–5,000 ft)
- Settings persist across sessions; share any location via `?location=` URL param
- Live ADS-B data via [airplanes.live](https://airplanes.live), routed through a self-hosted proxy — no API key needed
- 15-second auto-refresh with manual override
- Flight phase detection: LANDING, TAKING OFF, APPROACH, DESCENDING, CLIMBING, OVERHEAD (ESP32 adds CRUISING and UNKNOWN)
- Airline name from ICAO callsign prefix (passenger, cargo, and specialty operators including RFDS, Cobham, NetJets); full aircraft type names (B789 → B787-9, A20N → A320neo, etc.)
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
git clone https://github.com/greystoke1337/localized-air-traffic-tracker.git
cd localized-air-traffic-tracker
open index.html
```

The app calls `api.overheadtracker.com` (flight data), `nominatim.openstreetmap.org` (geocoding), and `api.planespotters.net` (photos). All HTTPS, all CORS-enabled.

---

## ESP32 hardware display

Standalone physical tracker on the **Freenove FNK0103S** (ESP32 + 4" 480×320 ST7796 touchscreen). Polls the local proxy directly over LAN, no browser needed.

**Hardware:** Freenove FNK0103S, optional 3D-printed enclosure (STL/STEP in [`tracker_live_fnk0103s/enclosure/`](tracker_live_fnk0103s/enclosure/))

**What it shows:** Header bar, nav bar with touch buttons (WX / GEO / CFG), flight card (callsign, airline, aircraft type, route), and a 4-column dashboard: PHASE | ALT (with vertical rate) | SPEED | DIST. Cycles through overhead flights every 8 seconds. Each of the 8 flight phases (TAKING OFF, CLIMBING, CRUISING, DESCENDING, APPROACH, LANDING, OVERHEAD, UNKNOWN) has its own colour in the dashboard.

**Nav bar controls:**

- **WX** — weather screen showing temperature, humidity, wind, and conditions
- **GEO** — cycles geofence radius: 5 km / 10 km / 50 km
- **CFG** — launches the captive portal for Wi-Fi and location configuration

**Route display:** departure and arrival with airport city names from a built-in lookup table.

**Emergency squawk handling:** 7700 / 7600 / 7500 triggers a flashing red banner (MAYDAY, NORDO, or HIJACK). The layout compacts automatically to prevent overlap.

**Libraries** (Arduino Library Manager): `TFT_eSPI` (Freenove version), `ArduinoJson`, `ArduinoOTA`, `SD`

Edit the top of [`tracker_live_fnk0103s/tracker_live_fnk0103s.ino`](tracker_live_fnk0103s/tracker_live_fnk0103s.ino) before flashing:

```cpp
const char* WIFI_SSID = "your-network";
const char* WIFI_PASS = "your-password";
const char* PROXY_HOST = "192.168.x.x";  // IP of your local proxy
```

```bash
./build.sh            # compile + upload via USB
./build.sh compile    # compile only
./build.sh upload     # upload last build via USB
./build.sh ota        # compile + upload over Wi-Fi (OTA)
./build.sh monitor    # serial monitor
```

**Over-the-air updates:** after the first USB flash, the device advertises itself as `overhead-tracker.local` on the local network via mDNS. Run `./build.sh ota` (or press **Ctrl+Shift+B** in VS Code) to compile and upload wirelessly. The TFT displays a green progress bar during the update.

### TFT preview tool

`tft-preview.html` is a browser-based simulator of the ESP32 display. Open it to preview layout changes before flashing.

It mirrors the firmware's rendering logic: same pixel coordinates, same RGB565 colour palette, same lookup tables (airlines, aircraft types, airports). Interactive controls let you test every combination of flight phase, squawk code, route length, and altitude.

```bash
open tft-preview.html   # or just double-click
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

- [ ] Push notification when a specific flight appears overhead

---

## License

MIT — do whatever you want with it.
