# Sydney Airport Arrivals Tracker

A real-time flight tracker showing aircraft on approach to Sydney Kingsford Smith (YSSY), built as a single HTML file with no dependencies or backend required.

🔗 **Live:** https://greystoke1337.github.io/sydney-localized-air-traffic-tracker

---

## Features

- **Live ADS-B data** via [airplanes.live](https://airplanes.live) — free, no API key, refreshes every 15 seconds
- **Dot matrix display** — amber-on-black airport board aesthetic with scanline overlay
- **Flight info** — callsign, registration, aircraft type, airline, and estimated departure airport
- **Descent profile graph** — canvas-drawn glideslope chart showing aircraft position relative to the ideal 3° ILS approach, always normalized to keep the aircraft in view
- **Aircraft photo** — fetched automatically from [Planespotters.net](https://planespotters.net) by hex code
- **Bankstown filter** — excludes traffic near YSBK so only YSSY arrivals are shown
- **Keyboard navigation** — left/right arrow keys to browse flights
- **NOW button** — jumps to the closest aircraft instantly
- **Loading spinner** — animated indicator while fetching data
- **Auto retry** — if the API fails, retries up to 3 times with backoff before showing an error
- **Mobile responsive** — scales cleanly on phones and tablets

---

## How It Works

1. Queries airplanes.live for all aircraft within 50nm of YSSY
2. Filters to descending aircraft only, excluding traffic near Bankstown Airport
3. Sorts by distance to SYD — closest on approach first
4. Renders flight data, glideslope graph, and fetches a photo from Planespotters

---

## Usage

No server, no build step, no API keys needed. Just open `index.html` in a browser.

```bash
git clone https://github.com/greystoke1337/sydney-localized-air-traffic-tracker.git
cd sydney-localized-air-traffic-tracker
open index.html   # or double-click it
```

---

## Data Sources

| Source | Data | Key required |
|---|---|---|
| [airplanes.live](https://api.airplanes.live) | Live ADS-B positions | No |
| [Planespotters.net](https://planespotters.net) | Aircraft photos | No |

---

## Roadmap

- Sound alert when a new aircraft becomes #1 on approach
- Historic landing log
- Airline logo display
- Map view of approach path

---

## License

MIT — do whatever you want with it.
