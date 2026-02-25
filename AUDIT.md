# Data Flow Audit — Localized Air Traffic Tracker

*Completed: 2026-02-25*

---

## System Overview

```
airplanes.live API
        ↓
  Pi Proxy (server.js, Node.js, port 3000)
  ├── 10-second in-memory cache (Map keyed by "lat,lon,radius")
  ├── Cloudflare Tunnel → api.overheadtracker.com
  └── Direct local access 192.168.x.x:3000
        ↓
  ┌─────────────────────┬──────────────────────┐
  │   Web App           │   ESP32              │
  │   index.html        │   .ino firmware      │
  └─────────────────────┴──────────────────────┘
```

---

## Layer 1 — Data Fetching

### Web app (`index.html:1537–1606`)

- Builds the API URL with a **4× geofence buffer**: `apiRadiusNm() = ceil((geofenceKm / 1.852) * 4)`. Intentional over-fetch; precise filtering runs client-side via Haversine after.
- Tries proxy first with a 6-second timeout (`AbortSignal.any`), falls back to the direct API on timeout or any error.
- `AbortController` cancels any in-flight request before a new one starts — correct stale-request pattern.
- **Fallback abort edge case**: the proxy catch block checks `proxyErr.name === 'AbortError' && !signal.aborted` to distinguish a timeout from a manual abort. In the degenerate case where the timeout fires in the same tick as a manual abort, the first `if` branch wins and falls through to the direct API instead of propagating the abort. JS single-threaded event loop makes this practically impossible to hit, but the logic is order-dependent.

### ESP32 (`.ino:698–858`)

- Tries proxy (5-second timeout), writes the raw payload to SD card on success.
- Falls back to a **zero-allocation streaming JSON parser** against the direct HTTPS API (12-second deadline). Scans character-by-character; avoids buffering the full document in RAM.
- Falls back to the SD cache when all network paths fail.
- Three-tier fallback is well-structured and robust.

---

## Layer 2 — Pi Proxy

*(Server code lives on the Pi at `/home/pi/proxy/server.js`; not tracked in this repo. The following is based on `PI_PROXY_SETUP.md` and inferred behaviour.)*

### Cache

- **Key format**: `"lat,lon,radius"` — exact string match. Both clients send 4 decimal places so they share cache entries correctly.
- **TTL**: 10 seconds. Stale entries are overwritten on next miss but are never explicitly deleted.
- **Memory leak (minor)**: `Map` entries for coordinates that are never queried again accumulate indefinitely. For a home proxy with a fixed handful of locations this is negligible, but structurally there is no eviction path.

### Security / access

- `Access-Control-Allow-Origin: *` is intentional (required for the GitHub Pages web client).
- **`/shutdown` endpoint has no authentication** — anyone who can reach `api.overheadtracker.com/shutdown` can restart the proxy process.
- No API-key or rate-limiting on the proxy's own endpoints.

---

## Layer 3 — Client-side Data Handling

### Web App

#### Filtering pipeline (`index.html:1574–1582`)

```javascript
(data.ac || []).filter(f =>
  typeof f.alt_baro === 'number' && f.alt_baro >= altFloorFt &&
  f.lat && f.lon &&                                             // ← see Bug #3
  haversineKm(...) <= geofenceKm
)
```

Good overall. Issues noted below.

---

#### Bug 1 — Falsy checks on numeric fields cause valid data to display as `'---'`

`index.html:1326–1328`

```javascript
const vs  = f.baro_rate ? ... + ' FPM' : '---';
const hdg = f.track     ? ... + '°'   : '---';
const spd = f.gs        ? ... + ' KT'  : '---';
```

`f.baro_rate === 0` (exactly level flight) and `f.track === 0` (heading due North) both evaluate as falsy. An aircraft cruising level at heading 0° shows `'---'` for both VS and HDG despite having valid data. `track === 0` is a realistic value.

---

#### Bug 2 — Flight selection resets to #1 on every auto-refresh

`index.html:1592`

```javascript
flights = ac; flightIndex = 0;
```

Every 15-second refresh resets `flightIndex` to 0 (closest aircraft). A user browsing aircraft #4 is snapped back to #1 on the next refresh with no recovery. Fixing this requires searching the new `flights` array for the previously selected `f.hex` after each fetch and restoring the index if found.

---

#### Bug 3 — Falsy check excludes aircraft at lat=0 or lon=0

`index.html:1577`

```javascript
f.lat && f.lon
```

Any aircraft at latitude 0 or longitude 0 (e.g. crossing the equator or the prime meridian) is filtered out. The correct guard is `f.lat != null && f.lon != null` or `typeof f.lat === 'number'`. Practically harmless for use around Australia, but technically incorrect.

---

#### Bug 4 — Route lookup errors are cached permanently; no retry

`index.html:1198–1200`

```javascript
} catch(e) {
  routeCache.set(callsign, null);  // ← permanently null this session
  return null;
}
```

A transient adsbdb.com network error writes a permanent `null` entry for that callsign. Subsequent renders for the same flight skip the lookup entirely and show no route for the rest of the session. The cache should distinguish "not found" from "error" and retry on error.

---

#### Bug 5 — Military aircraft also flagged as PRIVATE/CHARTER

`index.html:1229–1237`

```javascript
if (reg && !call.match(/^[A-Z]{3}\d/)) flags.push('★ PRIVATE / CHARTER');
// ...
// Bizjet cleanup removes the private flag — but military callsigns do not
```

Military callsigns (e.g. `RAAF12`, `DUKE31`) don't match the `[A-Z]{3}\d` pattern, so they receive both `★ MILITARY CALLSIGN` and `★ PRIVATE / CHARTER`. The bizjet dedup splice removes the private flag for known bizjet types but there's no equivalent for military callsigns.

---

### ESP32 Firmware

#### Bug 6 — Session log deduplication fails after 50 callsigns

`.ino:106–115`, `388–390`

```c
#define MAX_LOGGED 50
// ...
if (loggedCount < MAX_LOGGED) {
  strlcpy(loggedCallsigns[loggedCount++], f.callsign, 12);
}
```

After 50 unique callsigns fill the in-memory array, `loggedCount` stops incrementing. `alreadyLogged()` then returns `false` for every subsequent callsign, so each flight after entry #50 gets appended to `flightlog.csv` on every 15-second refresh — producing duplicate rows. Fix: either grow the array, or wrap with a circular buffer / hash set.

---

#### Inconsistency 7 — Flight phase thresholds diverge between web and ESP32

| Condition | Web app | ESP32 |
|---|---|---|
| Low-alt boundary | 3 000 ft | 1 000 ft |
| TAKING OFF vspd | > 200 fpm | > 200 fpm (same) |
| LANDING vspd | < -200 fpm | < -200 fpm (same) |
| APPROACH alt/vspd | < 3 000 ft, vs < -50 | < 4–6 000 ft, vs < -300–500 |
| CLIMBING vspd | > 100 fpm | > 400 fpm |
| DESCENDING vspd | < -100 fpm | < -400 fpm |
| OVERHEAD | not present | dist < 2 km && alt < 8 000 ft |

The ESP32's climb/descent thresholds are 4× stricter (±400 vs ±100 fpm), so a gently climbing aircraft reads CRUISING on the device and CLIMBING in the browser. The low-altitude phase boundary is 3× lower on the ESP32.

---

#### Issue 8 — SD cache has no age / TTL

`.ino:341–360`

`/cache.json` is written on every successful proxy fetch but carries no timestamp metadata. If all network sources fail for an extended period, the ESP32 loads arbitrarily old flight data. The only indicator is the "CACHED" source tag in the header bar. Adding a write timestamp to the cache and displaying data age on screen would improve transparency.

---

#### Issue 9 — Hardcoded 500 ft altitude floor

`.ino:776`, `870`

```c
if (alt < 500 || lat == 0.0f) continue;
```

Both the streaming parser and `extractFlights()` hardcode 500 ft as the minimum altitude, with no way to adjust it. The web app's configurable `altFloorFt` slider (200–5 000 ft) has no equivalent on the device.

---

#### Issue 10 — HTTPS certificate not verified

`.ino:749–751`

`HTTPClient::begin(url)` for the direct `https://api.airplanes.live` URL does not configure a root CA certificate. The TLS handshake succeeds but the server certificate is not verified, leaving the connection open to MITM on untrusted networks.

---

## Summary

| # | Severity | Layer | Issue |
|---|---|---|---|
| 1 | Bug | Web | `track=0` (due North) and `baro_rate=0` (level flight) display `'---'` due to falsy checks |
| 2 | UX | Web | `flightIndex` resets to 0 on every auto-refresh, discarding the user's selection |
| 3 | Bug | Web | `f.lat && f.lon` falsy check incorrectly excludes aircraft at lat=0 or lon=0 |
| 4 | Logic | Web | Route lookup network errors cached as permanent `null`; no retry this session |
| 5 | Logic | Web | Military aircraft also flagged as `PRIVATE / CHARTER` |
| 6 | Bug | ESP32 | Session log CSV deduplication fails after 50 unique callsigns; duplicates written on every refresh |
| 7 | Inconsistency | Both | Flight phase thresholds differ significantly between web and ESP32 |
| 8 | Data quality | ESP32 | No TTL on SD cache; stale data served indefinitely when offline with no age display |
| 9 | Config | ESP32 | Altitude floor hardcoded at 500 ft; not user-configurable |
| 10 | Security | ESP32 | HTTPS certificate not verified on direct API fallback |
| 11 | Memory | Proxy | Cache `Map` entries for unseen coordinates never evicted |
| 12 | Security | Proxy | `/shutdown` endpoint unauthenticated |
