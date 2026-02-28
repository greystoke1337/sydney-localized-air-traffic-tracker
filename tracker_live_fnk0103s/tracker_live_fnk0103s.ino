/*
  OVERHEAD TRACKER — LIVE (Redesigned UI)
  FNK0103S — 4.0" 480x320 ST7796 (landscape)

  Libraries needed:
    - TFT_eSPI (Freenove pre-configured version)
    - ArduinoJson (install via Library Manager)
    - SD (built into Arduino ESP32 core — no install needed)
    - ArduinoOTA (built into Arduino ESP32 core — no install needed)
*/

#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <math.h>
#include <Preferences.h>
#include <time.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include "secrets.h"

TFT_eSPI   tft = TFT_eSPI();
WebServer  setupServer(80);
DNSServer  dnsServer;

// ─── WiFi (loaded from NVS on boot; defaults used on first flash) ─────────
char WIFI_SSID[64] = WIFI_SSID_DEFAULT;
char WIFI_PASS[64] = WIFI_PASS_DEFAULT;

// ─── Proxy ────────────────────────────────────────────
const char* PROXY_HOST = "192.168.86.24";
const int   PROXY_PORT = 3000;

// ─── SD pin ───────────────────────────────────────────
#define SD_CS 5

// ─── Refresh ──────────────────────────────────────────
const int REFRESH_SECS = 15;
const int CYCLE_SECS   = 8;

// ─── Screen (landscape: 480 wide x 320 tall) ──────────
#define W 480
#define H 320

// ─── Layout ───────────────────────────────────────────
#define HDR_H      28   // amber header bar
#define NAV_H      36   // navigation/controls bar below header
#define FOOT_H     20   // dim footer bar
#define CONTENT_Y  (HDR_H + NAV_H)                // 64
#define CONTENT_H  (H - HDR_H - NAV_H - FOOT_H)  // 236px

// ─── Colours (RGB565) ─────────────────────────────────
#define C_BG      0x0820
#define C_AMBER   0xFD00
#define C_DIM     0x7940
#define C_DIMMER  0x3900
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_CYAN    0x07FF
#define C_YELLOW  0xFFE0
#define C_ORANGE  0xFC60   // descending (aligned with web app #ff8844)
#define C_GOLD    0xFE68   // approach   (aligned with web app #ffaa00)

// ─── Location (defaults — overridden by NVS / config.txt) ────────────────
float HOME_LAT    = 0.0f;
float HOME_LON    = 0.0f;
float GEOFENCE_KM = 10.0f;
int   ALT_FLOOR_FT = 500;
char  LOCATION_NAME[32] = "NOT SET";
char  HOME_QUERY[128]   = "";   // plain-text search; cleared after geocoding
bool  needsGeocode      = false;

// ─── SD state ─────────────────────────────────────────
bool sdAvailable = false;

// ─── Geofence presets ─────────────────────────────────
const float GEO_PRESETS[] = {5.0f, 10.0f, 20.0f};
const int   GEO_COUNT     = 3;
int         geoIndex      = 1;  // default: 10km

// ─── Touch ────────────────────────────────────────────
uint16_t touchCalData[5] = {0};
bool     touchReady      = false;
uint32_t lastTouchMs     = 0;
#define  TOUCH_DEBOUNCE_MS  350
// Nav bar button zones (below header, right-aligned, 70px wide each)
#define  NAV_Y       HDR_H
#define  NAV_BTN_W   70
#define  NAV_BTN_H   (NAV_H - 4)   // 32px tall (2px top/bottom padding)
#define  NAV_BTN_GAP 4
#define  CFG_BTN_X1  (W - NAV_BTN_W)                          // 410
#define  GEO_BTN_X1  (CFG_BTN_X1 - NAV_BTN_GAP - NAV_BTN_W)  // 336
#define  WX_BTN_X1   (GEO_BTN_X1 - NAV_BTN_GAP - NAV_BTN_W)  // 262

// ─── Screen mode ──────────────────────────────────────
enum ScreenMode { SCREEN_FLIGHT, SCREEN_WEATHER };
ScreenMode currentScreen = SCREEN_FLIGHT;

// ─── Weather data ─────────────────────────────────────
struct WeatherData {
  float   temp;
  float   feels_like;
  int     humidity;
  char    condition[32];
  float   wind_speed;
  int     wind_dir;
  char    wind_cardinal[4];
  float   uv_index;
  int32_t utc_offset_secs;
};
WeatherData wxData;
bool        wxReady         = false;
int         wxCountdown     = 0;
int         lastMinute      = -1;  // for clock-only redraws
const int   WX_REFRESH_SECS = 900; // 15 minutes

// ─── Flight status ────────────────────────────────────
enum FlightStatus {
  STATUS_UNKNOWN,
  STATUS_TAKING_OFF,
  STATUS_CLIMBING,
  STATUS_CRUISING,
  STATUS_DESCENDING,
  STATUS_APPROACH,
  STATUS_LANDING,
  STATUS_OVERHEAD,
};

// ─── Flight struct ────────────────────────────────────
struct Flight {
  char         callsign[12];
  char         reg[12];
  char         type[8];
  char         dep[6];
  char         arr[6];
  float        lat, lon;
  int          alt;
  int          speed;
  int          vs;
  int          track;
  float        dist;
  char         squawk[6];
  FlightStatus status;
};

Flight flights[20];
Flight newFlights[20];

// global — keeps off the stack
int           flightCount  = 0;
int           flightIndex  = 0;
int           countdown    = REFRESH_SECS;
bool          isFetching   = false;
bool          usingCache   = false;
// Source: 0=proxy, 1=direct API, 2=cache
int           dataSource   = 0;
unsigned long lastTick     = 0;
unsigned long lastCycle    = 0;
time_t        cacheTimestamp = 0;  // Unix time when cache.json was last written

// ─── Direct API robustness ──────────────────────────────
int           directApiFailCount   = 0;
unsigned long directApiNextRetryMs = 0;
#define       DIRECT_API_MIN_HEAP  40000  // 40 KB minimum free heap for TLS
#define       DIRECT_API_TIMEOUT   8000   // reduced from 12 s (WDT is 30 s)

bool wifiOk() { return WiFi.status() == WL_CONNECTED; }

// ─── Session log (track unique callsigns to avoid duplicates) ─
#define MAX_LOGGED 200
char loggedCallsigns[MAX_LOGGED][12];
int  loggedCount = 0;

bool alreadyLogged(const char* cs) {
  for (int i = 0; i < loggedCount; i++)
    if (strcmp(loggedCallsigns[i], cs) == 0) return true;
  return false;
}

// ─── Airline lookup ───────────────────────────────────
struct Airline { const char* prefix; const char* name; };
const Airline AIRLINES[] = {
  {"QFA","QANTAS"},   {"VOZ","VIRGIN"},     {"JST","JETSTAR"},
  {"RXA","REX"},      {"UAE","EMIRATES"},   {"ETD","ETIHAD"},
  {"QTR","QATAR"},    {"SIA","SINGAPORE"},  {"ANZ","AIR NZ"},
  {"CPA","CATHAY"},   {"MAS","MALAYSIA"},   {"THA","THAI"},
  {"KAL","KOREAN"},   {"JAL","JAL"},        {"ANA","ANA"},
  {"AAL","AMERICAN"}, {"UAL","UNITED"},     {"BAW","BRITISH"},
  {"DAL","DELTA"},    {"AFR","AIR FRANCE"}, {"DLH","LUFTHANSA"},
  {"NWL","NETWORK"},  {"FJI","FIJI"},       {"EVA","EVA AIR"},
  {"CCA","AIR CHINA"},{"CSN","CHINA STH"},  {"CES","CHINA EST"},
  {"HAL","HAWAIIAN"},
};
const int AIRLINE_COUNT = sizeof(AIRLINES) / sizeof(AIRLINES[0]);

const char* getAirline(const char* cs) {
  for (int i = 0; i < AIRLINE_COUNT; i++)
    if (strncmp(cs, AIRLINES[i].prefix, 3) == 0) return AIRLINES[i].name;
  return "";
}

// ─── Aircraft type lookup ─────────────────────────────
struct AircraftType { const char* code; const char* name; };
const AircraftType AIRCRAFT_TYPES[] = {
  {"B737","B737-700"},  {"B738","B737-800"},  {"B739","B737-900"},  {"B73X","B737-900ER"},
  {"B37M","B737 MAX 7"},{"B38M","B737 MAX 8"},{"B39M","B737 MAX 9"},{"B3XM","B737 MAX 10"},
  {"B752","B757-200"},  {"B753","B757-300"},
  {"B762","B767-200"},  {"B763","B767-300"},  {"B764","B767-400"},
  {"B772","B777-200"},  {"B77L","B777-200LR"},{"B773","B777-300"},  {"B77W","B777-300ER"},
  {"B788","B787-8"},    {"B789","B787-9"},    {"B78X","B787-10"},
  {"B712","B717-200"},
  {"B741","B747-100"},  {"B742","B747-200"},  {"B743","B747-300"},  {"B744","B747-400"},  {"B748","B747-8"},
  {"A318","A318"},      {"A319","A319"},      {"A320","A320"},      {"A321","A321"},
  {"A19N","A319neo"},   {"A20N","A320neo"},   {"A21N","A321neo"},   {"A21X","A321XLR"},
  {"A332","A330-200"},  {"A333","A330-300"},  {"A338","A330-800neo"},{"A339","A330-900neo"},
  {"A342","A340-200"},  {"A343","A340-300"},  {"A345","A340-500"},  {"A346","A340-600"},
  {"A359","A350-900"},  {"A35K","A350-1000"},
  {"A380","A380"},      {"A388","A380-800"},
  {"E170","E170"},      {"E175","E175"},      {"E190","E190"},      {"E195","E195"},
  {"E290","E190-E2"},   {"E295","E195-E2"},
  {"CRJ2","CRJ-200"},   {"CRJ7","CRJ-700"},   {"CRJ9","CRJ-900"},   {"CRJX","CRJ-1000"},
  {"DH8A","Dash 8-100"},{"DH8B","Dash 8-200"},{"DH8C","Dash 8-300"},{"DH8D","Dash 8-400"},
  {"AT43","ATR 42-300"},{"AT45","ATR 42-500"},{"AT72","ATR 72-200"},{"AT75","ATR 72-500"},{"AT76","ATR 72-600"},
  {"BCS1","A220-100"},  {"BCS3","A220-300"},
  {"SF34","SAAB 340"},  {"SB20","SAAB 2000"}, {"JS41","Jetstream 41"},
  {"PC12","Pilatus PC-12"},{"C208","Cessna Caravan"},
  {"GL5T","G550"},      {"GLEX","Global Express"},{"GLF6","G650"},
  {"C25A","Citation CJ2"},{"C25B","Citation CJ3"},
  {"FA7X","Falcon 7X"}, {"FA8X","Falcon 8X"},
  {"C130","C-130 Hercules"},{"P8","P-8 Poseidon"},
  {"EC35","H135"},      {"EC45","H145"},       {"S76","Sikorsky S-76"},
  {"B06","Bell 206"},   {"B407","Bell 407"},
};
const int AIRCRAFT_TYPE_COUNT = sizeof(AIRCRAFT_TYPES) / sizeof(AIRCRAFT_TYPES[0]);

const char* getAircraftTypeName(const char* code) {
  if (!code || !code[0]) return "---";
  for (int i = 0; i < AIRCRAFT_TYPE_COUNT; i++)
    if (strcmp(AIRCRAFT_TYPES[i].code, code) == 0) return AIRCRAFT_TYPES[i].name;
  return code; 
}

// ─── Airport name lookup ──────────────────────────────
struct Airport { const char* code; const char* city; };
const Airport AIRPORTS[] = {
  {"YSSY","Sydney"},   {"YMML","Melbourne"}, {"YBBN","Brisbane"},
  {"YPPH","Perth"},    {"YPAD","Adelaide"},  {"YSCB","Canberra"},
  {"YBCS","Cairns"},   {"YBHM","Hamilton Is"},{"YBTL","Townsville"},
  {"YBAS","Alice Spg"},{"YSNF","Norfolk Is"},
  {"SYD","Sydney"},    {"MEL","Melbourne"},  {"BNE","Brisbane"},
  {"PER","Perth"},     {"ADL","Adelaide"},   {"CBR","Canberra"},
  {"CNS","Cairns"},    {"OOL","Gold Coast"}, {"TSV","Townsville"},
  {"DRW","Darwin"},    {"HBA","Hobart"},     {"LST","Launceston"},
  {"MKY","Mackay"},    {"ROK","Rockhampton"},
  {"NZAA","Auckland"}, {"NZCH","Christchurch"},{"NZWN","Wellington"},{"NZQN","Queenstown"},
  {"AKL","Auckland"},  {"CHC","Christchurch"},{"WLG","Wellington"},  {"ZQN","Queenstown"},
  {"WSSS","Singapore"},{"SIN","Singapore"},
  {"VHHH","Hong Kong"},{"HKG","Hong Kong"},
  {"RJAA","Tokyo"},    {"RJTT","Tokyo"},     {"NRT","Tokyo"},       {"HND","Tokyo"},
  {"RJBB","Osaka"},    {"KIX","Osaka"},
  {"RKSI","Seoul"},    {"ICN","Seoul"},
  {"RCTP","Taipei"},   {"TPE","Taipei"},
  {"VTBS","Bangkok"},  {"BKK","Bangkok"},
  {"WMKK","K.Lumpur"}, {"KUL","K.Lumpur"},
  {"WADD","Bali"},     {"WIII","Jakarta"},   {"DPS","Bali"},        {"CGK","Jakarta"},
  {"RPLL","Manila"},   {"MNL","Manila"},
  {"VVTS","Ho Chi Minh"},{"SGN","Ho Chi Minh"},{"VVNB","Hanoi"},   {"HAN","Hanoi"},
  {"ZBAA","Beijing"},  {"ZSPD","Shanghai"},  {"PEK","Beijing"},     {"PVG","Shanghai"},
  {"ZGGG","Guangzhou"},{"CAN","Guangzhou"},
  {"OMDB","Dubai"},    {"DXB","Dubai"},
  {"OMAA","Abu Dhabi"},{"AUH","Abu Dhabi"},
  {"OTHH","Doha"},    {"DOH","Doha"},
  {"VIDP","Delhi"},    {"VABB","Mumbai"},    {"DEL","Delhi"},       {"BOM","Mumbai"},
  {"EGLL","London"},   {"EGKK","London"},   {"LHR","London"},      {"LGW","London"},
  {"LFPG","Paris"},    {"CDG","Paris"},
  {"EHAM","Amsterdam"},{"AMS","Amsterdam"},
  {"EDDF","Frankfurt"},{"FRA","Frankfurt"},
  {"EDDM","Munich"},   {"MUC","Munich"},
  {"LEMD","Madrid"},   {"MAD","Madrid"},
  {"LEBL","Barcelona"},{"BCN","Barcelona"},
  {"LIRF","Rome"},     {"FCO","Rome"},
  {"LTFM","Istanbul"}, {"IST","Istanbul"},
  {"KJFK","New York"}, {"JFK","New York"},
  {"KLAX","L.A."},     {"LAX","L.A."},
  {"KSFO","S.F."},     {"SFO","S.F."},
  {"KORD","Chicago"},  {"ORD","Chicago"},
  {"KATL","Atlanta"},  {"ATL","Atlanta"},
  {"KDFW","Dallas"},   {"DFW","Dallas"},
  {"KDEN","Denver"},   {"DEN","Denver"},
  {"KSEA","Seattle"},  {"SEA","Seattle"},
  {"CYYZ","Toronto"},  {"YYZ","Toronto"},
  {"CYVR","Vancouver"},{"YVR","Vancouver"},
  {"FAOR","J'burg"},  {"JNB","J'burg"},
  {"FACT","Cape Town"},{"CPT","Cape Town"},
  {"HECA","Cairo"},    {"CAI","Cairo"},
};
const int AIRPORT_COUNT = sizeof(AIRPORTS) / sizeof(AIRPORTS[0]);

const char* airportCity(const char* code) {
  if (!code || !code[0]) return nullptr;
  for (int i = 0; i < AIRPORT_COUNT; i++)
    if (strcmp(AIRPORTS[i].code, code) == 0) return AIRPORTS[i].city;
  return code;
}

bool formatRoute(const char* dep, const char* arr, char* buf, int len) {
  if (!dep[0] && !arr[0]) return false;
  const char* depName = dep[0] ? airportCity(dep) : "?";
  const char* arrName = arr[0] ? airportCity(arr) : "?";
  snprintf(buf, len, "%s > %s", depName, arrName);
  return true;
}

// ─── Flight status derivation ─────────────────────────
// Thresholds kept in sync with web app flightPhase().
FlightStatus deriveStatus(int alt, int vs, float dist) {
  if (alt <= 0) return STATUS_UNKNOWN;
  if (dist < 2.0f && alt < 8000) return STATUS_OVERHEAD;
  if (alt < 3000) {
    if (vs < -200) return STATUS_LANDING;
    if (vs >  200) return STATUS_TAKING_OFF;
    if (vs <  -50) return STATUS_APPROACH;
  }
  if (vs < -100) return STATUS_DESCENDING;
  if (vs >  100) return STATUS_CLIMBING;
  return STATUS_CRUISING;
}

const char* statusLabel(FlightStatus s) {
  switch (s) {
    case STATUS_TAKING_OFF:  return "TAKEOFF";
    case STATUS_CLIMBING:    return "CLIMBING";
    case STATUS_CRUISING:    return "CRUISING";
    case STATUS_DESCENDING:  return "DESCEND";
    case STATUS_APPROACH:    return "APPROACH";
    case STATUS_LANDING:     return "LANDING";
    case STATUS_OVERHEAD:    return "OVERHEAD";
    default:                 return "UNKNOWN";
  }
}

uint16_t statusColor(FlightStatus s) {
  switch (s) {
    case STATUS_TAKING_OFF:  return C_GREEN;
    case STATUS_CLIMBING:    return C_GREEN;
    case STATUS_CRUISING:    return C_AMBER;
    case STATUS_DESCENDING:  return C_ORANGE;
    case STATUS_APPROACH:    return C_GOLD;
    case STATUS_LANDING:     return C_RED;
    case STATUS_OVERHEAD:    return C_AMBER;
    default:                 return C_DIM;
  }
}

// ─── Helpers ──────────────────────────────────────────
float haversineKm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f;
  float dLat = (lat2 - lat1) * M_PI / 180.0f;
  float dLon = (lon2 - lon1) * M_PI / 180.0f;
  float a = sinf(dLat/2)*sinf(dLat/2) +
            cosf(lat1*M_PI/180)*cosf(lat2*M_PI/180)*sinf(dLon/2)*sinf(dLon/2);
  return R * 2.0f * atan2f(sqrtf(a), sqrtf(1-a));
}

int apiRadiusNm() {
  return (int)ceilf((GEOFENCE_KM / 1.852f) * 4.0f);
}

void formatAlt(int alt, char* buf, int len) {
  if (alt <= 0)          snprintf(buf, len, "---");
  else if (alt >= 10000) snprintf(buf, len, "FL%03d", alt / 100);
  else                   snprintf(buf, len, "%d FT", alt);
}

// ─── SD: Config ───────────────────────────────────────
void loadConfig() {
  if (!sdAvailable) return;
  File f = SD.open("/config.txt", FILE_READ);
  if (!f) {
    Serial.println("No config.txt — using defaults");
    return;
  }
  Serial.println("Reading config.txt...");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("#") || line.length() == 0) continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim(); val.trim();
    if      (key == "lat")       HOME_LAT     = val.toFloat();
    else if (key == "lon")       HOME_LON     = val.toFloat();
    else if (key == "geofence")  GEOFENCE_KM  = val.toFloat();
    else if (key == "alt_floor") ALT_FLOOR_FT = val.toInt();
    else if (key == "name") {
      val.toUpperCase();
      strlcpy(LOCATION_NAME, val.c_str(), sizeof(LOCATION_NAME));
    }
    Serial.printf("  %s = %s\n", key.c_str(), val.c_str());
  }
  f.close();
  Serial.println("Config loaded.");
}

// ─── SD: Cache write ──────────────────────────────────
void writeCache(const String& payload) {
  if (!sdAvailable) return;
  File f = SD.open("/cache.json", FILE_WRITE);
  if (!f) { Serial.println("Cache write failed"); return; }
  f.print(payload);
  f.close();
  // Write timestamp alongside the cache
  time_t now = time(NULL);
  if (now > 1000000000) {  // only write if NTP has synced (epoch > ~2001)
    File tf = SD.open("/cache_ts.txt", FILE_WRITE);
    if (tf) { tf.print((unsigned long)now); tf.close(); }
    cacheTimestamp = now;
  }
  Serial.printf("Cache written (%d bytes)\n", payload.length());
}

// ─── SD: Cache read ───────────────────────────────────
String readCache() {
  if (!sdAvailable) return "";
  File f = SD.open("/cache.json", FILE_READ);
  if (!f) return "";
  String payload = f.readString();
  f.close();
  // Read associated timestamp if present
  File tf = SD.open("/cache_ts.txt", FILE_READ);
  if (tf) {
    cacheTimestamp = (time_t)tf.readString().toInt();
    tf.close();
  }
  Serial.printf("Cache loaded (%d bytes, ts=%lu)\n", payload.length(), (unsigned long)cacheTimestamp);
  return payload;
}

// ─── SD: Flight log ───────────────────────────────────
void logFlight(const Flight& f) {
  if (!sdAvailable) return;
  if (!f.callsign[0] || alreadyLogged(f.callsign)) return;
  bool isNew = !SD.exists("/flightlog.csv");
  File file = SD.open("/flightlog.csv", FILE_APPEND);
  if (!file) return;

  if (isNew) {
    file.println("callsign,reg,type,airline,dep,arr,status,dist_km");
  }

  char row[96];
  snprintf(row, sizeof(row), "%s,%s,%s,%s,%s,%s,%s,%.1f",
    f.callsign,
    f.reg,
    f.type,
    getAirline(f.callsign),
    f.dep[0] ? f.dep : "",
    f.arr[0] ? f.arr : "",
    statusLabel(f.status),
    f.dist
  );
  file.println(row);
  file.close();

  if (loggedCount < MAX_LOGGED) {
    strlcpy(loggedCallsigns[loggedCount++], f.callsign, 12);
  }
  Serial.printf("Logged: %s\n", f.callsign);
}

// ─── Touch calibration (NVS flash) ───────────────────
bool loadTouchCal() {
  Preferences p;
  p.begin("tracker", true);
  if (!p.isKey("tcal")) { p.end(); return false; }
  p.getBytes("tcal", touchCalData, sizeof(touchCalData));
  p.end();
  return true;
}

void saveTouchCal() {
  Preferences p;
  p.begin("tracker", false);
  p.putBytes("tcal", touchCalData, sizeof(touchCalData));
  p.end();
}

void initTouch() {
  if (loadTouchCal()) {
    tft.setTouch(touchCalData);
    touchReady = true;
    Serial.println("Touch cal loaded.");
    return;
  }
  // First boot — run on-screen calibration routine
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, W, HDR_H, C_AMBER);
  tft.setTextColor(C_BG, C_AMBER);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print("OVERHEAD TRACKER");
  tft.setTextColor(C_AMBER, C_BG);
  tft.setTextSize(2);
  tft.setCursor(16, H/2 - 24);
  tft.print("TOUCH CALIBRATION");
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextSize(1);
  tft.setCursor(16, H/2 + 4);
  tft.print("Tap each corner cross when it appears");
  delay(1200);
  tft.calibrateTouch(touchCalData, C_AMBER, C_BG, 15);
  saveTouchCal();
  touchReady = true;
  Serial.println("Touch calibrated and saved.");
}

// ─── NVS: WiFi + location + geofence config ───────────
bool loadWiFiConfig() {
  Preferences p;
  p.begin("tracker", true);
  if (!p.isKey("wifi_ssid")) { p.end(); return false; }
  strlcpy(WIFI_SSID, p.getString("wifi_ssid", "").c_str(), sizeof(WIFI_SSID));
  strlcpy(WIFI_PASS, p.getString("wifi_pass", "").c_str(), sizeof(WIFI_PASS));
  // Location: use saved lat/lon if available; otherwise schedule geocoding
  if (p.isKey("home_lat")) {
    HOME_LAT = p.getFloat("home_lat", HOME_LAT);
    HOME_LON = p.getFloat("home_lon", HOME_LON);
    String name = p.getString("home_name", "");
    if (name.length() > 0) {
      name.toUpperCase();
      strlcpy(LOCATION_NAME, name.c_str(), sizeof(LOCATION_NAME));
    }
    needsGeocode = false;
  } else if (p.isKey("home_query")) {
    strlcpy(HOME_QUERY, p.getString("home_query", "").c_str(), sizeof(HOME_QUERY));
    needsGeocode = true;
  }
  geoIndex = p.getInt("gfence_idx", 1);
  if (geoIndex < 0 || geoIndex >= GEO_COUNT) geoIndex = 1;
  GEOFENCE_KM = GEO_PRESETS[geoIndex];
  p.end();
  return true;
}

void saveWiFiConfig(const char* ssid, const char* pass, const char* query) {
  Preferences p;
  p.begin("tracker", false);
  p.putString("wifi_ssid",  ssid);
  p.putString("wifi_pass",  pass);
  p.putString("home_query", query);
  // Clear cached lat/lon so geocoding runs on next boot
  p.remove("home_lat");
  p.remove("home_lon");
  p.remove("home_name");
  p.end();
}

void saveGeoIndex() {
  Preferences p;
  p.begin("tracker", false);
  p.putInt("gfence_idx", geoIndex);
  p.end();
}

// ─── Geocoding (Nominatim) ────────────────────────────
bool geocodeLocation(const char* query) {
  // URL-encode: replace spaces with +
  char encoded[192];
  int j = 0;
  for (int i = 0; query[i] && j < (int)sizeof(encoded) - 1; i++) {
    if (query[i] == ' ') encoded[j++] = '+';
    else                  encoded[j++] = query[i];
  }
  encoded[j] = '\0';

  char url[320];
  snprintf(url, sizeof(url),
    "https://nominatim.openstreetmap.org/search?q=%s&format=json&limit=1",
    encoded);

  WiFiClientSecure client;
  client.setInsecure();  // OK for a one-time geocode request
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("User-Agent", "OverheadTracker/1.0");
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Geocode HTTP error: %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, body) != DeserializationError::Ok || !doc.is<JsonArray>()) {
    Serial.println("Geocode JSON parse error");
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() == 0) {
    Serial.println("Geocode: no results");
    return false;
  }
  HOME_LAT = arr[0]["lat"].as<const char*>() ? String(arr[0]["lat"].as<const char*>()).toFloat() : 0.0f;
  HOME_LON = arr[0]["lon"].as<const char*>() ? String(arr[0]["lon"].as<const char*>()).toFloat() : 0.0f;

  // Extract short name: first segment of display_name before the first comma
  String dispName = arr[0]["display_name"].as<String>();
  int comma = dispName.indexOf(',');
  String shortName = (comma > 0 && comma <= 30) ? dispName.substring(0, comma) : dispName.substring(0, 30);
  shortName.toUpperCase();
  strlcpy(LOCATION_NAME, shortName.c_str(), sizeof(LOCATION_NAME));

  Serial.printf("Geocoded: %s → %.4f, %.4f\n", LOCATION_NAME, HOME_LAT, HOME_LON);

  // Persist to NVS and clear the pending query
  Preferences p;
  p.begin("tracker", false);
  p.putFloat("home_lat",  HOME_LAT);
  p.putFloat("home_lon",  HOME_LON);
  p.putString("home_name", LOCATION_NAME);
  p.remove("home_query");
  p.end();
  HOME_QUERY[0] = '\0';
  needsGeocode = false;
  return true;
}

// ─── Captive portal ───────────────────────────────────

void handleSetupRoot() {
  // Use <b> for field labels — more reliable than <label> in captive portal WebViews.
  // placeholders serve as fallback if styling is stripped.
  const char* locDefault = HOME_QUERY[0] ? HOME_QUERY : LOCATION_NAME;
  static char page[2048];
  snprintf(page, sizeof(page),
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OVERHEAD SETUP</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{background:#041010;color:#fd0;font-family:monospace;padding:16px;max-width:480px;margin:auto}"
    "h2{font-size:1em;letter-spacing:3px;margin:0 0 20px;padding-bottom:8px;border-bottom:1px solid #3900}"
    "b{display:block;font-size:.8em;letter-spacing:1px;margin:16px 0 4px;color:#fd0}"
    "input{display:block;width:100%%;background:#0d1f1f;border:1px solid #fd0;"
    "color:#fd0;padding:10px;font-family:monospace;font-size:1em;margin-bottom:2px}"
    "button{display:block;width:100%%;margin-top:20px;padding:14px;background:#fd0;"
    "color:#041010;border:none;font-family:monospace;font-size:1em;font-weight:bold;letter-spacing:2px}"
    "</style></head><body>"
    "<h2>OVERHEAD TRACKER &mdash; SETUP</h2>"
    "<form method='POST' action='/save'>"
    "<b>WI-FI NETWORK</b>"
    "<input name='ssid' placeholder='Network name' value='%s' required>"
    "<b>WI-FI PASSWORD</b>"
    "<input name='pass' type='password' placeholder='Password'>"
    "<b>LOCATION</b>"
    "<input name='query' placeholder='e.g. Russell Lea, Sydney Airport' value='%s'>"
    "<button type='submit'>SAVE &amp; REBOOT</button>"
    "</form></body></html>",
    WIFI_SSID, locDefault);
  setupServer.send(200, "text/html", page);
}

void handleSetupSave() {
  String ssid  = setupServer.arg("ssid");
  String pass  = setupServer.arg("pass");
  String query = setupServer.arg("query");
  ssid.trim(); query.trim();
  saveWiFiConfig(ssid.c_str(), pass.c_str(), query.c_str());
  setupServer.send(200, "text/html",
    "<html><body style='background:#041010;color:#fd0;"
    "font-family:monospace;padding:20px'>"
    "<h2 style='color:#07e0'>SAVED</h2>"
    "<p>REBOOTING NOW...</p></body></html>");
  delay(1500);
  ESP.restart();
}

void startCaptivePortal() {
  // TFT setup screen
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, W, HDR_H, C_AMBER);
  tft.setTextColor(C_BG, C_AMBER);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print("OVERHEAD TRACKER");

  tft.setTextColor(C_AMBER, C_BG);
  tft.setTextSize(2);
  tft.setCursor(16, 50);
  tft.print("SETUP MODE");

  tft.setTextColor(C_DIM, C_BG);
  tft.setTextSize(1);
  tft.setCursor(16, 86);
  tft.print("ON YOUR PHONE:");
  tft.setCursor(16, 104);
  tft.print("1. CONNECT TO WI-FI:");
  tft.setTextColor(C_AMBER, C_BG);
  tft.setCursor(28, 120);
  tft.print("OVERHEAD-SETUP");
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(16, 144);
  tft.print("2. OPEN ANY BROWSER");
  tft.setCursor(28, 160);
  tft.print("(PAGE OPENS AUTOMATICALLY)");
  tft.setTextColor(C_DIMMER, C_BG);
  tft.setCursor(28, 180);
  tft.print("OR: 192.168.4.1");

  // Start AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP("OVERHEAD-SETUP");
  delay(100);

  // DNS redirect — all queries → 192.168.4.1 (captive portal)
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Web server routes
  setupServer.on("/", HTTP_GET, handleSetupRoot);
  setupServer.on("/save", HTTP_POST, handleSetupSave);
  // Captive portal detection endpoints (iOS, Android, Windows auto-popup)
  setupServer.onNotFound([]() {
    setupServer.sendHeader("Location", "http://192.168.4.1/");
    setupServer.send(302, "text/plain", "");
  });
  setupServer.begin();
  Serial.println("Captive portal active — AP: OVERHEAD-SETUP");

  // Block until form submitted (handleSetupSave restarts the device)
  while (true) {
    dnsServer.processNextRequest();
    setupServer.handleClient();
  }
}

// ─── Drawing ──────────────────────────────────────────

// ── Header bar (title + location only — buttons moved to nav bar) ──
void drawHeader() {
  tft.fillRect(0, 0, W, HDR_H, C_AMBER);
  tft.setTextColor(C_BG, C_AMBER);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print("OVERHEAD TRACKER");
  int locW = strlen(LOCATION_NAME) * 12;
  tft.setCursor(W - locW - 8, 6);
  tft.print(LOCATION_NAME);
}

// ── Navigation bar (WX · GEO · CFG buttons, flight index indicator) ──
void drawNavBar() {
  tft.fillRect(0, NAV_Y, W, NAV_H, C_BG);
  tft.drawFastHLine(0, NAV_Y, W, C_DIMMER);

  // Flight index indicator (left side)
  if (currentScreen == SCREEN_FLIGHT && flightCount > 1) {
    char navBuf[16];
    snprintf(navBuf, sizeof(navBuf), "< %d/%d >", flightIndex + 1, flightCount);
    tft.setTextSize(2);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(8, NAV_Y + 10);
    tft.print(navBuf);
  }

  // WX button
  uint16_t wxBg = (currentScreen == SCREEN_WEATHER) ? C_CYAN : C_DIMMER;
  uint16_t wxFg = (currentScreen == SCREEN_WEATHER) ? C_BG   : C_AMBER;
  tft.fillRect(WX_BTN_X1, NAV_Y + 2, NAV_BTN_W, NAV_BTN_H, wxBg);
  tft.setTextColor(wxFg, wxBg);
  tft.setTextSize(2);
  tft.setCursor(WX_BTN_X1 + (NAV_BTN_W - 24) / 2, NAV_Y + 10);
  tft.print("WX");

  // GEO button
  const char* geoLabels[] = {"5km", "10km", "20km"};
  tft.fillRect(GEO_BTN_X1, NAV_Y + 2, NAV_BTN_W, NAV_BTN_H, C_DIMMER);
  tft.setTextColor(C_AMBER, C_DIMMER);
  tft.setTextSize(2);
  int geoW = strlen(geoLabels[geoIndex]) * 12;
  tft.setCursor(GEO_BTN_X1 + (NAV_BTN_W - geoW) / 2, NAV_Y + 10);
  tft.print(geoLabels[geoIndex]);

  // CFG button
  uint16_t cfgBg = isFetching ? C_RED : C_DIMMER;
  const char* cfgLbl = isFetching ? "..." : "CFG";
  tft.fillRect(CFG_BTN_X1, NAV_Y + 2, NAV_BTN_W, NAV_BTN_H, cfgBg);
  tft.setTextColor(C_AMBER, cfgBg);
  tft.setTextSize(2);
  int cfgW = strlen(cfgLbl) * 12;
  tft.setCursor(CFG_BTN_X1 + (NAV_BTN_W - cfgW) / 2, NAV_Y + 10);
  tft.print(cfgLbl);
}

// ── Footer bar ─────────────────────────────────────────
void drawStatusBar() {
  int y = H - FOOT_H;
  tft.fillRect(0, y, W, FOOT_H, C_BG);
  tft.drawFastHLine(0, y, W, C_DIMMER);
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextSize(1);
  char buf[80];
  if (isFetching) {
    snprintf(buf, sizeof(buf), "  SCANNING AIRSPACE...");
  } else if (dataSource == 2 && cacheTimestamp > 0) {
    time_t now = time(NULL);
    long ageSec = (now > cacheTimestamp) ? (long)(now - cacheTimestamp) : 0;
    if (ageSec < 3600)
      snprintf(buf, sizeof(buf), "  AC %d/%d   SRC:CACHE(%ldm%lds)   NEXT:%ds",
               flightIndex+1, flightCount, ageSec/60, ageSec%60, countdown);
    else
      snprintf(buf, sizeof(buf), "  AC %d/%d   SRC:CACHE(%ldh%ldm)   NEXT:%ds",
               flightIndex+1, flightCount, ageSec/3600, (ageSec%3600)/60, countdown);
  } else {
    const char* src = dataSource==2 ? "CACHE" : dataSource==1 ? "DIRECT" : "PROXY";
    snprintf(buf, sizeof(buf), "  AC %d/%d   SRC:%s   NEXT:%ds",
             flightIndex+1, flightCount, src, countdown);
  }
  tft.setCursor(6, y + 6);
  tft.print(buf);
}


void renderMessage(const char* line1, const char* line2);

// ─── Touch handler ────────────────────────────────────
void handleTouch(uint16_t tx, uint16_t ty) {
  uint32_t now = millis();
  if (now - lastTouchMs < TOUCH_DEBOUNCE_MS) return;
  lastTouchMs = now;

  // ── Nav bar buttons ──
  if (ty >= NAV_Y && ty < NAV_Y + NAV_H) {

    // WX button
    if (tx >= WX_BTN_X1 && tx < WX_BTN_X1 + NAV_BTN_W) {
      tft.fillRect(WX_BTN_X1, NAV_Y + 2, NAV_BTN_W, NAV_BTN_H, C_AMBER);
      tft.setTextColor(C_BG, C_AMBER);
      tft.setTextSize(2);
      tft.setCursor(WX_BTN_X1 + (NAV_BTN_W - 24) / 2, NAV_Y + 10);
      tft.print("WX");
      delay(100);
      if (currentScreen == SCREEN_WEATHER) {
        currentScreen = SCREEN_FLIGHT;
        if (flightCount > 0) renderFlight(flights[flightIndex]);
        else renderMessage("NO AIRCRAFT", "IN RANGE");
      } else {
        currentScreen = SCREEN_WEATHER;
        renderWeather();
      }
      return;
    }

    // GEO button
    if (tx >= GEO_BTN_X1 && tx < GEO_BTN_X1 + NAV_BTN_W) {
      tft.fillRect(GEO_BTN_X1, NAV_Y + 2, NAV_BTN_W, NAV_BTN_H, C_AMBER);
      tft.setTextColor(C_BG, C_AMBER);
      tft.setTextSize(2);
      tft.setCursor(GEO_BTN_X1 + 8, NAV_Y + 10);
      tft.print(geoIndex == 0 ? "5km" : geoIndex == 1 ? "10km" : "20km");
      delay(100);
      geoIndex = (geoIndex + 1) % GEO_COUNT;
      GEOFENCE_KM = GEO_PRESETS[geoIndex];
      saveGeoIndex();
      Serial.printf("Geofence: %.0f km\n", GEOFENCE_KM);
      drawNavBar();
      if (!isFetching) {
        flightCount = 0;
        flightIndex = 0;
        fetchFlights();
        countdown = REFRESH_SECS;
        lastCycle  = millis();
      }
      return;
    }

    // CFG button (two-tap confirmation — reboot is destructive)
    if (tx >= CFG_BTN_X1 && tx < CFG_BTN_X1 + NAV_BTN_W) {
      if (isFetching) return;
      tft.fillRect(CFG_BTN_X1, NAV_Y + 2, NAV_BTN_W, NAV_BTN_H, C_RED);
      tft.setTextColor(C_BG, C_RED);
      tft.setTextSize(1);
      tft.setCursor(CFG_BTN_X1 + 5, NAV_Y + 6);
      tft.print("REBOOT?");
      tft.setCursor(CFG_BTN_X1 + 2, NAV_Y + 20);
      tft.print("TAP AGAIN");
      uint32_t confirmDeadline = millis() + 3000;
      bool confirmed = false;
      while (millis() < confirmDeadline) {
        uint16_t cx, cy;
        if (tft.getTouch(&cx, &cy)) {
          if (cx >= CFG_BTN_X1 && cy >= NAV_Y && cy < NAV_Y + NAV_H) {
            confirmed = true;
            break;
          } else {
            break;
          }
        }
        delay(30);
      }
      if (confirmed) {
        startCaptivePortal();
      } else {
        drawNavBar();
      }
      return;
    }
    return;
  }

  // ── Content area: tap left/right to cycle flights ──
  if (ty >= CONTENT_Y && ty < (H - FOOT_H) &&
      currentScreen == SCREEN_FLIGHT && flightCount > 1) {
    if (tx < W / 2) {
      flightIndex = (flightIndex - 1 + flightCount) % flightCount;
    } else {
      flightIndex = (flightIndex + 1) % flightCount;
    }
    lastCycle = millis();
    renderFlight(flights[flightIndex]);
    return;
  }
}

// ─── Main Content Display (Redesigned) ─────────────────
void renderFlight(const Flight& f) {
  drawHeader();
  drawNavBar();

  // Clear the main content area
  tft.fillRect(0, CONTENT_Y, W, CONTENT_H, C_BG);

  // Emergency squawk banner (full-width red alert at top of content)
  bool hasEmergency = strcmp(f.squawk,"7700")==0 || strcmp(f.squawk,"7600")==0 || strcmp(f.squawk,"7500")==0;
  int emergOffset = 0;
  if (hasEmergency) {
    const char* emergLabel = strcmp(f.squawk,"7700")==0 ? "EMERGENCY - MAYDAY" :
                             strcmp(f.squawk,"7600")==0 ? "EMERGENCY - NORDO"  :
                                                          "EMERGENCY - HIJACK";
    tft.fillRect(0, CONTENT_Y, W, 24, C_RED);
    tft.setTextColor(C_BG, C_RED);
    tft.setTextSize(2);
    int lblW = strlen(emergLabel) * 12;
    tft.setCursor((W - lblW) / 2, CONTENT_Y + 4);
    tft.print(emergLabel);
    emergOffset = 24;
  }

  int x = 15;
  int y = CONTENT_Y + emergOffset + 4;

  // 1. PRIMARY IDENTITY (Flight & Airline)
  int csSize = hasEmergency ? 3 : 4;
  tft.setTextSize(csSize);
  tft.setTextColor(C_AMBER, C_BG);
  tft.setCursor(x, y);
  tft.print(f.callsign[0] ? f.callsign : "SEARCHING");

  y += csSize * 8;
  if (!hasEmergency) {
    const char* al = getAirline(f.callsign);
    tft.setTextSize(2);
    tft.setTextColor(al[0] ? C_AMBER : C_DIM, C_BG);
    tft.setCursor(x, y);
    tft.print(al[0] ? al : "UNKNOWN AIRLINE");
    y += 20;
  } else {
    y += 8;
  }

  // 2. AIRCRAFT TYPE & REG
  tft.drawFastHLine(10, y, W - 20, C_DIMMER);
  y += 8;

  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(x, y);
  tft.print("AIRCRAFT TYPE");
  tft.setCursor(W/2 + 20, y);
  tft.print("REGISTRATION");
  
  y += 10;
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN, C_BG);
  tft.setCursor(x, y);
  tft.print(getAircraftTypeName(f.type));
  
  tft.setTextColor(C_AMBER, C_BG);
  tft.setCursor(W/2 + 20, y);
  tft.print(f.reg[0] ? f.reg : "---");

  // 3. ROUTE SECTION
  y += 20;
  tft.drawFastHLine(10, y, W - 20, C_DIMMER);
  y += 8;
  
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(x, y);
  tft.print("ROUTE");
  
  y += 10;
  char routeBuf[64];
  if (formatRoute(f.dep, f.arr, routeBuf, sizeof(routeBuf))) {
    tft.setTextSize(2);
    tft.setTextColor(C_YELLOW, C_BG);
    tft.setCursor(x, y);
    tft.print(routeBuf);
  } else {
    tft.setTextSize(2);
    tft.setTextColor(C_DIMMER, C_BG);
    tft.setCursor(x, y);
    tft.print("NO ROUTE DATA");
  }

  // 4. DASHBOARD: PHASE | ALT | SPEED | DIST (4 columns at 120px each)
  int dashY = H - FOOT_H - 75;
  tft.drawFastHLine(0, dashY, W, C_DIM);

  // Phase Block (0-120)
  uint16_t sCol = statusColor(f.status);
  tft.fillRect(0, dashY + 1, 4, 74, sCol);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(12, dashY + 8);
  tft.print("PHASE");
  tft.setTextSize(2);
  tft.setTextColor(sCol, C_BG);
  tft.setCursor(12, dashY + 24);
  tft.print(statusLabel(f.status));

  // Squawk (below phase)
  bool emerg = strcmp(f.squawk,"7700")==0 || strcmp(f.squawk,"7600")==0 || strcmp(f.squawk,"7500")==0;
  const char* sqLabel = strcmp(f.squawk,"7700")==0 ? "MAYDAY" : strcmp(f.squawk,"7600")==0 ? "NORDO" : strcmp(f.squawk,"7500")==0 ? "HIJACK" : f.squawk;
  tft.setTextColor(emerg ? C_RED : C_DIM, C_BG);
  tft.setTextSize(1);
  tft.setCursor(12, dashY + 44);
  tft.print("SQK ");
  tft.print(sqLabel);

  // Altitude Block (120-240)
  char altBuf[20];
  formatAlt(f.alt, altBuf, sizeof(altBuf));
  tft.drawFastVLine(120, dashY + 5, 65, C_DIMMER);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(132, dashY + 8);
  tft.print("ALTITUDE");
  tft.setTextSize(2);
  tft.setTextColor(C_AMBER, C_BG);
  tft.setCursor(132, dashY + 24);
  tft.print(altBuf);
  // Vertical rate indicator
  tft.setTextSize(1);
  if (abs(f.vs) >= 50) {
    char vsBuf[16];
    if (f.vs > 0) {
      snprintf(vsBuf, sizeof(vsBuf), "+%d FPM", f.vs);
      tft.setTextColor(C_GREEN, C_BG);
    } else {
      snprintf(vsBuf, sizeof(vsBuf), "%d FPM", f.vs);
      tft.setTextColor(C_RED, C_BG);
    }
    tft.setCursor(132, dashY + 44);
    tft.print(vsBuf);
  } else {
    tft.setTextColor(C_AMBER, C_BG);
    tft.setCursor(132, dashY + 44);
    tft.print("LEVEL");
  }

  // Speed Block (240-360)
  tft.drawFastVLine(240, dashY + 5, 65, C_DIMMER);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(252, dashY + 8);
  tft.print("SPEED");
  tft.setTextSize(2);
  tft.setTextColor(C_AMBER, C_BG);
  tft.setCursor(252, dashY + 24);
  if (f.speed > 0) {
    char spdBuf[16];
    snprintf(spdBuf, sizeof(spdBuf), "%d", f.speed);
    tft.print(spdBuf);
    tft.setTextSize(1);
    tft.print(" KT");
  } else {
    tft.print("---");
  }

  // Distance Block (360-480)
  tft.drawFastVLine(360, dashY + 5, 65, C_DIMMER);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(372, dashY + 8);
  tft.print("DISTANCE");
  tft.setTextSize(2);
  tft.setTextColor(C_AMBER, C_BG);
  tft.setCursor(372, dashY + 24);
  if (f.dist > 0) {
    char distBuf[16];
    if (f.dist >= 10.0f) snprintf(distBuf, sizeof(distBuf), "%.0f", f.dist);
    else                  snprintf(distBuf, sizeof(distBuf), "%.1f", f.dist);
    tft.print(distBuf);
    tft.setTextSize(1);
    tft.print(" KM");
  } else {
    tft.print("---");
  }

  drawStatusBar();
}


// ─── Fetch weather from Pi proxy (Open-Meteo) ─────────
void fetchWeather() {
  esp_task_wdt_reset();
  if (!wifiOk()) { Serial.println("[WX] WiFi not connected"); return; }
  char url[160];
  snprintf(url, sizeof(url),
    "http://%s:%d/weather?lat=%.4f&lon=%.4f",
    PROXY_HOST, PROXY_PORT, HOME_LAT, HOME_LON);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[WX] Fetch failed (%d)\n", code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    Serial.println("[WX] JSON parse error");
    return;
  }
  wxData.temp            = doc["temp"]            | 0.0f;
  wxData.feels_like      = doc["feels_like"]      | 0.0f;
  wxData.humidity        = doc["humidity"]        | 0;
  wxData.wind_speed      = doc["wind_speed"]      | 0.0f;
  wxData.wind_dir        = doc["wind_dir"]        | 0;
  wxData.uv_index        = doc["uv_index"]        | 0.0f;
  wxData.utc_offset_secs = doc["utc_offset_secs"] | 0;
  const char* cond = doc["condition"] | "---";
  strlcpy(wxData.condition, cond, sizeof(wxData.condition));
  const char* wc = doc["wind_cardinal"] | "?";
  strlcpy(wxData.wind_cardinal, wc, sizeof(wxData.wind_cardinal));
  wxReady = true;
  Serial.printf("[WX] %.1f C  %s  UV %.1f\n", wxData.temp, wxData.condition, wxData.uv_index);
}

// ─── Weather + clock screen ────────────────────────────
void renderWeather() {
  drawHeader();
  drawNavBar();
  tft.fillRect(0, CONTENT_Y, W, CONTENT_H, C_BG);

  // ── Clock ──
  time_t utcNow  = time(NULL);
  bool   ntpOk   = utcNow > 1000000000UL;
  time_t localNow = (ntpOk && wxReady && wxData.utc_offset_secs != 0)
                      ? utcNow + wxData.utc_offset_secs : utcNow;
  struct tm* t   = gmtime(&localNow);

  int cy = CONTENT_Y + 12;
  char timeBuf[8];
  if (ntpOk) snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", t->tm_hour, t->tm_min);
  else       strlcpy(timeBuf, "--:--", sizeof(timeBuf));

  // HH:MM centred — size 6: 36px/char, 5 chars = 180px
  tft.setTextSize(6);
  tft.setTextColor(C_AMBER, C_BG);
  tft.setCursor((W - 180) / 2, cy);
  tft.print(timeBuf);

  cy += 52;
  if (ntpOk) {
    const char* dayNames[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    const char* monNames[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
    char dateBuf[20];
    snprintf(dateBuf, sizeof(dateBuf), "%s %d %s",
             dayNames[t->tm_wday], t->tm_mday, monNames[t->tm_mon]);
    int dateW = strlen(dateBuf) * 12;
    tft.setTextSize(2);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor((W - dateW) / 2, cy);
    tft.print(dateBuf);
  }
  cy += 22;

  tft.drawFastHLine(10, cy, W - 20, C_DIMMER);
  cy += 8;

  if (!wxReady) {
    tft.setTextSize(1);
    tft.setTextColor(C_DIMMER, C_BG);
    tft.setCursor(15, cy);
    tft.print("WEATHER LOADING...");
    drawStatusBar();
    return;
  }

  // ── Weather grid ──
  char buf[32];

  // Row 1: Temperature | Feels Like
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(15, cy);      tft.print("TEMPERATURE");
  tft.setCursor(W/2 + 15, cy); tft.print("FEELS LIKE");
  cy += 10;
  tft.setTextSize(2);
  tft.setTextColor(C_AMBER, C_BG);
  snprintf(buf, sizeof(buf), "%.1f C", wxData.temp);
  tft.setCursor(15, cy); tft.print(buf);
  snprintf(buf, sizeof(buf), "%.1f C", wxData.feels_like);
  tft.setCursor(W/2 + 15, cy); tft.print(buf);
  cy += 20;

  tft.drawFastHLine(10, cy, W - 20, C_DIMMER); cy += 6;

  // Row 2: Condition
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(15, cy); tft.print("CONDITIONS");
  cy += 10;
  tft.setTextSize(2);
  tft.setTextColor(C_YELLOW, C_BG);
  tft.setCursor(15, cy); tft.print(wxData.condition);
  cy += 20;

  tft.drawFastHLine(10, cy, W - 20, C_DIMMER); cy += 6;

  // Row 3: Humidity | Wind
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(15, cy);       tft.print("HUMIDITY");
  tft.setCursor(W/2 + 15, cy); tft.print("WIND");
  cy += 10;
  tft.setTextSize(2);
  tft.setTextColor(C_AMBER, C_BG);
  snprintf(buf, sizeof(buf), "%d%%", wxData.humidity);
  tft.setCursor(15, cy); tft.print(buf);
  tft.setTextColor(C_AMBER, C_BG);
  snprintf(buf, sizeof(buf), "%.0f KM/H %s", wxData.wind_speed, wxData.wind_cardinal);
  tft.setCursor(W/2 + 15, cy); tft.print(buf);
  cy += 20;

  tft.drawFastHLine(10, cy, W - 20, C_DIMMER); cy += 6;

  // Row 4: UV Index (colour-coded by level)
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(15, cy); tft.print("UV INDEX");
  cy += 10;
  uint16_t uvCol = wxData.uv_index < 3.0f ? C_GREEN :
                   wxData.uv_index < 6.0f ? C_YELLOW :
                   wxData.uv_index < 8.0f ? C_AMBER  : C_RED;
  tft.setTextSize(2);
  tft.setTextColor(uvCol, C_BG);
  snprintf(buf, sizeof(buf), "%.1f", wxData.uv_index);
  tft.setCursor(15, cy); tft.print(buf);

  drawStatusBar();
}

// ── Boot sequence ───────────────────────────────────────
static int bootLineY = 56;

void bootLine(const char* label, const char* result, uint16_t col, int pauseMs) {
  tft.setTextColor(C_DIMMER, C_BG);
  tft.setTextSize(1);
  tft.setCursor(10, bootLineY);
  tft.print(label);
  int dotX = 10 + strlen(label) * 6;
  while (dotX < 212) { tft.setCursor(dotX, bootLineY); tft.print("."); dotX += 6; }
  delay(pauseMs);
  tft.setTextColor(col, C_BG);
  tft.setCursor(214, bootLineY);
  tft.print(result);
  bootLineY += 14;
  delay(10);
}

void bootSequence() {
  tft.fillScreen(C_BG);
  bootLineY = 56;
  for (int y = 0; y < H; y += 2) {
    tft.drawFastHLine(0, y, W, C_DIMMER);
    delayMicroseconds(200);
  }
  delay(30);
  tft.fillScreen(C_BG);
  delay(20);

  tft.setTextColor(C_AMBER, C_BG);
  tft.setTextSize(2);
  tft.setCursor(10, 12);
  tft.print("OVERHEAD TRACKER");
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextSize(1);
  tft.setCursor(10, 34);
  tft.print("ADS-B AIRSPACE SURVEILLANCE  REV 3.2");
  tft.drawFastHLine(0, 47, W, C_DIM);
  delay(100);
  char buf[40];
  snprintf(buf, sizeof(buf), "240 MHz  DUAL CORE");
  bootLine("CPU",            buf,                    C_GREEN,  30);
  snprintf(buf, sizeof(buf), "%d KB FREE", ESP.getFreeHeap() / 1024);
  bootLine("HEAP MEMORY",    buf,                    C_GREEN,  35);
  snprintf(buf, sizeof(buf), "%d KB",      ESP.getFlashChipSize() / 1024);
  bootLine("FLASH SIZE",     buf,                    C_AMBER,  25);
  snprintf(buf, sizeof(buf), "%s", ESP.getSdkVersion());
  bootLine("ESP-IDF SDK",    buf,                    C_DIM,    20);
  bootLine("SPI BUS",        "CLK 40MHz  OK",         C_GREEN,  25);
  bootLine("DISPLAY",        "ST7796 480x320 16BIT",  C_GREEN,  30);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  bootLine("WIFI MAC",       buf,                    C_AMBER,  30);
  bootLine("WIFI MODE",      "STA  802.11 B/G/N",    C_AMBER,  20);
  bootLine("SD CARD",        "SEARCHING...",         C_YELLOW, 80);
  snprintf(buf, sizeof(buf), "%s:%d", PROXY_HOST, PROXY_PORT);
  bootLine("PROXY TARGET",   buf,                    C_AMBER,  25);
  snprintf(buf, sizeof(buf), "%.4f, %.4f", HOME_LAT, HOME_LON);
  bootLine("HOME COORDS",    buf,                    C_AMBER,  25);
  snprintf(buf, sizeof(buf), "%.0f KM RADIUS", GEOFENCE_KM);
  bootLine("GEOFENCE",       buf,                    C_AMBER,  20);
  bootLine("ADS-B PIPELINE", "DECODER READY",        C_GREEN,  35);
  snprintf(buf, sizeof(buf), "%d SEC AUTO-REFRESH", REFRESH_SECS);
  bootLine("POLL INTERVAL",  buf,                    C_GREEN,  25);
  tft.drawFastHLine(0, bootLineY + 4, W, C_DIM);
  int flashY = bootLineY + 8;

  tft.fillRect(8, flashY, W - 16, 24, C_GREEN);
  tft.setTextColor(C_BG, C_GREEN);
  tft.setTextSize(2);
  int textX = (W - 13*12) / 2;
  tft.setCursor(textX, flashY + 4);
  tft.print("SYSTEM ONLINE");
  delay(120);
  tft.fillRect(8, flashY, W - 16, 24, C_BG);
  delay(80);
  tft.fillRect(8, flashY, W - 16, 24, C_GREEN);
  tft.setTextColor(C_BG, C_GREEN);
  tft.setTextSize(2);
  tft.setCursor(textX, flashY + 4);
  tft.print("SYSTEM ONLINE");
  delay(400);
}

// ── Error / status message screen ──────────────────────
void renderMessage(const char* line1, const char* line2 = nullptr) {
  tft.fillScreen(C_BG);
  drawHeader();
  drawNavBar();
  tft.setTextColor(C_AMBER, C_BG);
  tft.setTextSize(2);
  tft.setCursor(16, H/2 - 16);
  tft.print(line1);
  if (line2) {
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextSize(1);
    tft.setCursor(16, H/2 + 12);
    tft.print(line2);
  }
}

// ─── Parse a String payload (proxy or cache) ──────────
int extractFlights(DynamicJsonDocument& doc);
int parsePayload(String& payload) {
  StaticJsonDocument<512> filter;
  JsonObject af = filter["ac"].createNestedObject();
  af["flight"] = af["r"] = af["t"] = af["lat"] = af["lon"] =
  af["alt_baro"] = af["gs"] = af["baro_rate"] = af["track"] =
  af["squawk"] = af["dep"] = af["arr"] = af["orig_iata"] = af["dest_iata"] = true;
  Serial.printf("[MEM] Before JSON alloc: %d free\n", ESP.getFreeHeap());
  DynamicJsonDocument doc(16384);
  // In-situ parse: ArduinoJSON modifies the buffer in-place and stores string
  // pointers into it rather than copying — doc memory usage is ~half of copy mode.
  DeserializationError err = deserializeJson(doc, &payload[0], payload.length(), DeserializationOption::Filter(filter));
  Serial.printf("[MEM] After JSON parse: %d free\n", ESP.getFreeHeap());
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return -1;
  }
  int result = extractFlights(doc);
  // Free doc before returning, then free the source buffer in caller
  doc.clear();
  payload = String();  // release the source buffer now that doc is done with it
  return result;
}

// ─── Fetch: proxy (returns small String) ──────────────
String fetchFromProxy() {
  if (!wifiOk()) { Serial.println("[PROXY] WiFi not connected"); return ""; }
  char url[160];
  snprintf(url, sizeof(url),
    "http://%s:%d/flights?lat=%.4f&lon=%.4f&radius=%d",
    PROXY_HOST, PROXY_PORT, HOME_LAT, HOME_LON, apiRadiusNm());
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String p = http.getString();
    http.end();
    Serial.printf("Proxy OK, payload=%d bytes\n", p.length());
    return p;
  }
  http.end();
  Serial.printf("Proxy failed (%d)\n", code);
  return "";
}

// ─── Root CA for api.airplanes.live (ISRG Root X1 / Let's Encrypt) ──
static const char AIRPLANES_LIVE_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoBggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

// ─── Fetch: direct API — zero-allocation stream scanner ──
static bool readQuotedString(WiFiClient* s, char* buf, int maxLen) {
  int i = 0;
  int c;
  while ((c = s->read()) != -1) {
    if (c == '"') { buf[i] = 0; return true; }
    if (c == '\\') s->read();  
    if (i < maxLen - 1) buf[i++] = (char)c;
  }
  return false;
}

static int readNumber(WiFiClient* s, char* buf, int maxLen) {
  int i = 0;
  int c = s->peek();
  while (c == ' ' || c == '\n' || c == '\r') { s->read(); c = s->peek(); }
  while (i < maxLen - 1) {
    c = s->peek();
    if (c == -1 || c == ',' || c == '}' || c == ']' || c == ' ') break;
    buf[i++] = (char)s->read();
  }
  buf[i] = 0;
  return i;
}

int fetchAndParseDirectAPI() {
  if (!wifiOk()) {
    Serial.println("[DIRECT] WiFi not connected, skipping");
    return -1;
  }
  int freeHeap = ESP.getFreeHeap();
  Serial.printf("[DIRECT] Free heap: %d\n", freeHeap);
  if (freeHeap < DIRECT_API_MIN_HEAP) {
    Serial.println("[DIRECT] Insufficient heap for TLS, skipping");
    return -1;
  }
  if (directApiFailCount > 0 && millis() < directApiNextRetryMs) {
    Serial.printf("[DIRECT] Backoff active, %lu ms remaining\n",
                  directApiNextRetryMs - millis());
    return -1;
  }

  char url[160];
  snprintf(url, sizeof(url),
    "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
    HOME_LAT, HOME_LON, apiRadiusNm());

  WiFiClientSecure tlsClient;
  tlsClient.setCACert(AIRPLANES_LIVE_CA);
  tlsClient.setTimeout(DIRECT_API_TIMEOUT / 1000);
  HTTPClient http;
  http.begin(tlsClient, url);
  http.setTimeout(DIRECT_API_TIMEOUT);
  int code = http.GET();
  if (code != 200) {
    http.end();
    Serial.printf("Direct API failed (%d)\n", code);
    directApiFailCount++;
    unsigned long backoffMs = min(120000UL, 15000UL * (1UL << min(directApiFailCount - 1, 3)));
    directApiNextRetryMs = millis() + backoffMs;
    Serial.printf("[DIRECT] Backoff set to %lu ms (fail #%d)\n", backoffMs, directApiFailCount);
    return -1;
  }
  directApiFailCount = 0;

  Serial.printf("[MEM] Direct stream scan start: %d free\n", ESP.getFreeHeap());

  WiFiClient* s = http.getStreamPtr();
  int newCount = 0;
  int depth = 0;          
  bool inString = false;
  char key[16] = "";
  char val[32] = "";
  bool readingKey = false;
  bool readingVal = false;
  char ac_callsign[12]={}, ac_reg[12]={}, ac_type[8]={};
  char ac_dep[6]={},       ac_arr[6]={},  ac_squawk[6]={};
  float ac_lat=0, ac_lon=0;
  int   ac_alt=0, ac_speed=0, ac_vs=0, ac_track=-1;

  auto commitAircraft = [&]() {
    if (newCount >= 20) return;
    if (ac_alt < ALT_FLOOR_FT || ac_lat == 0.0f) return;
    float dist = haversineKm(HOME_LAT, HOME_LON, ac_lat, ac_lon);
    if (dist > GEOFENCE_KM) return;
    Flight& f = newFlights[newCount];
    strlcpy(f.callsign, ac_callsign, sizeof(f.callsign));
    for (int i = strlen(f.callsign)-1; i >= 0 && f.callsign[i] == ' '; i--) f.callsign[i] = 0;
    strlcpy(f.reg,    ac_reg,    sizeof(f.reg));
    strlcpy(f.type,   ac_type,   sizeof(f.type));
    strlcpy(f.squawk, ac_squawk[0] ? ac_squawk : "----", sizeof(f.squawk));
    strlcpy(f.dep,    ac_dep,    sizeof(f.dep));
    strlcpy(f.arr,    ac_arr,    sizeof(f.arr));
    f.lat=ac_lat; f.lon=ac_lon; f.alt=ac_alt;
    f.speed=ac_speed; f.vs=ac_vs; f.track=ac_track; f.dist=dist;
    f.status = deriveStatus(ac_alt, ac_vs, dist);
    for (int i=0;f.callsign[i];i++) f.callsign[i]=toupper(f.callsign[i]);
    for (int i=0;f.reg[i];i++)      f.reg[i]=toupper(f.reg[i]);
    for (int i=0;f.type[i];i++)     f.type[i]=toupper(f.type[i]);
    for (int i=0;f.dep[i];i++)      f.dep[i]=toupper(f.dep[i]);
    for (int i=0;f.arr[i];i++)      f.arr[i]=toupper(f.arr[i]);
    newCount++;
  };
  auto applyKV = [&]() {
    if      (strcmp(key,"flight")==0)    strlcpy(ac_callsign, val, sizeof(ac_callsign));
    else if (strcmp(key,"r")==0)         strlcpy(ac_reg,      val, sizeof(ac_reg));
    else if (strcmp(key,"t")==0)         strlcpy(ac_type,     val, sizeof(ac_type));
    else if (strcmp(key,"squawk")==0)    strlcpy(ac_squawk,   val, sizeof(ac_squawk));
    else if (strcmp(key,"dep")==0)       strlcpy(ac_dep,      val, sizeof(ac_dep));
    else if (strcmp(key,"arr")==0)       strlcpy(ac_arr,      val, sizeof(ac_arr));
    else if (strcmp(key,"orig_iata")==0 && !ac_dep[0])  strlcpy(ac_dep, val, sizeof(ac_dep));
    else if (strcmp(key,"dest_iata")==0 && !ac_arr[0])  strlcpy(ac_arr, val, sizeof(ac_arr));
    else if (strcmp(key,"lat")==0)       ac_lat   = atof(val);
    else if (strcmp(key,"lon")==0)       ac_lon   = atof(val);
    else if (strcmp(key,"alt_baro")==0)  ac_alt   = atoi(val);
    else if (strcmp(key,"gs")==0)        ac_speed = (int)atof(val);
    else if (strcmp(key,"baro_rate")==0) ac_vs    = atoi(val);
    else if (strcmp(key,"track")==0)     ac_track = (int)atof(val);
    key[0] = 0; val[0] = 0;
  };

  unsigned long deadline = millis() + DIRECT_API_TIMEOUT;
  int c;
  while (millis() < deadline) {
    if (!wifiOk()) { Serial.println("[DIRECT] WiFi lost during stream"); break; }
    if (!s->available()) { delay(5); continue; }
    c = s->read();
    if (c == -1) break;
    if (c == '"' && depth == 2) {
      readQuotedString(s, key, sizeof(key));
      while (s->available() && s->peek() != ':' && s->peek() != '"') s->read();
      if (s->peek() == ':') s->read();
      while (s->available() && (s->peek()==' '||s->peek()=='\t')) s->read();
      int nxt = s->peek();
      if (nxt == '"') {
        s->read();  
        readQuotedString(s, val, sizeof(val));
        applyKV();
      } else if (nxt == '-' || (nxt >= '0' && nxt <= '9')) {
        readNumber(s, val, sizeof(val));
        applyKV();
      }
      key[0] = 0;
      continue;
    }

    if (c == '{') {
      depth++;
      if (depth == 2) {
        ac_callsign[0]=ac_reg[0]=ac_type[0]=0;
        ac_dep[0]=ac_arr[0]=ac_squawk[0]=0;
        ac_lat=ac_lon=0; ac_alt=ac_speed=ac_vs=0; ac_track=-1;
      }
    } else if (c == '}') {
       if (depth == 2) commitAircraft();
      if (depth > 0) depth--;
    }
  }

  http.end();
  Serial.printf("[MEM] Direct scan done: %d free, found %d\n", ESP.getFreeHeap(), newCount);

  for (int i = 0; i < newCount-1; i++)
    for (int j = 0; j < newCount-i-1; j++)
      if (newFlights[j].dist > newFlights[j+1].dist)
        { Flight tmp=newFlights[j]; newFlights[j]=newFlights[j+1]; newFlights[j+1]=tmp; }

  return newCount;
}

// ─── Extract flights from a parsed JSON doc ────────────
int extractFlights(DynamicJsonDocument& doc) {
  JsonArray ac = doc["ac"].as<JsonArray>();
  int newCount = 0;
  for (JsonObject a : ac) {
    if (newCount >= 20) break;
    float lat = a["lat"] | 0.0f;
    float lon = a["lon"] | 0.0f;
    int   alt = a["alt_baro"] | 0;
    if (alt < ALT_FLOOR_FT || lat == 0.0f) continue;
    float dist = haversineKm(HOME_LAT, HOME_LON, lat, lon);
    if (dist > GEOFENCE_KM) continue;

    Flight& f = newFlights[newCount];
    const char* cs = a["flight"] | "";
    strlcpy(f.callsign, cs, sizeof(f.callsign));
    for (int i = strlen(f.callsign)-1; i >= 0 && f.callsign[i] == ' '; i--) f.callsign[i] = 0;
    strlcpy(f.reg,    a["r"]      | "",     sizeof(f.reg));
    strlcpy(f.type,   a["t"]      | "",     sizeof(f.type));
    strlcpy(f.squawk, a["squawk"] | "----", sizeof(f.squawk));
    { const char* d = a["dep"] | ""; strlcpy(f.dep, d[0] ? d : (a["orig_iata"] | ""), sizeof(f.dep)); }
    { const char* a2 = a["arr"] | ""; strlcpy(f.arr, a2[0] ? a2 : (a["dest_iata"] | ""), sizeof(f.arr)); }
    f.lat   = lat;
    f.lon = lon; f.alt = alt;
    f.speed = (int)(a["gs"]      | 0.0f);
    f.vs    = a["baro_rate"]     | 0;
    f.track = (int)(a["track"]   | -1.0f);
    f.dist  = dist;
    f.status = deriveStatus(alt, f.vs, dist);

    for (int i = 0; f.callsign[i]; i++) f.callsign[i] = toupper(f.callsign[i]);
    for (int i = 0; f.reg[i]; i++)      f.reg[i]      = toupper(f.reg[i]);
    for (int i = 0; f.type[i]; i++)     f.type[i]     = toupper(f.type[i]);
    for (int i = 0; f.dep[i]; i++)      f.dep[i]      = toupper(f.dep[i]);
    for (int i = 0; f.arr[i]; i++)      f.arr[i]      = toupper(f.arr[i]);
    newCount++;
  }

  for (int i = 0; i < newCount-1; i++)
    for (int j = 0; j < newCount-i-1; j++)
      if (newFlights[j].dist > newFlights[j+1].dist)
        { Flight tmp = newFlights[j]; newFlights[j] = newFlights[j+1]; newFlights[j+1] = tmp; }

  return newCount;
}

// ─── Main fetch ───────────────────────────────────────
void fetchFlights() {
  Serial.printf("[MEM] fetchFlights start: %d bytes free\n", ESP.getFreeHeap());
  isFetching = true;
  if (flightCount > 0) {
    drawHeader();
    drawNavBar();
    drawStatusBar();
  } else {
    renderMessage("FETCHING...");
  }

  int newCount = -1;
  bool fromCache = false;

  if (wifiOk()) {
    String payload = fetchFromProxy();
    if (!payload.isEmpty()) {
      writeCache(payload);
      newCount = parsePayload(payload);
      payload = String();
      dataSource = 0;
    } else {
      Serial.println("Trying direct API (stream)...");
      esp_task_wdt_reset();
      newCount = fetchAndParseDirectAPI();
      if (newCount >= 0) dataSource = 1;
    }
  } else {
    Serial.println("[FETCH] WiFi not connected, skipping network fetches");
  }

  if (newCount < 0) {
    Serial.println("Network failed, loading SD cache...");
    String payload = readCache();
    if (!payload.isEmpty()) {
      newCount = parsePayload(payload);
      payload = String();
      fromCache = true;
      dataSource = 2;
      Serial.println("Using cached data.");
    } else {
      renderMessage("NO DATA", "ALL SOURCES FAILED");
      isFetching = false;
      return;
    }
  }

  if (newCount < 0) {
    renderMessage("JSON ERROR", fromCache ? "CACHE CORRUPT" : "BAD RESPONSE");
    isFetching = false;
    return;
  }

  memcpy(flights, newFlights, sizeof(Flight) * newCount);
  flightCount = newCount;
  flightIndex = 0;
  isFetching  = false;
  usingCache  = fromCache;

  for (int i = 0; i < flightCount; i++) logFlight(flights[i]);
  if (flightCount == 0) {
    // Show transition message before switching to weather
    tft.fillRect(0, CONTENT_Y, W, CONTENT_H, C_BG);
    tft.setTextSize(3);
    tft.setTextColor(C_AMBER, C_BG);
    int msgW = 11 * 18;
    tft.setCursor((W - msgW) / 2, CONTENT_Y + CONTENT_H / 2 - 24);
    tft.print("CLEAR SKIES");
    tft.setTextSize(1);
    tft.setTextColor(C_DIM, C_BG);
    int subW = 20 * 6;
    tft.setCursor((W - subW) / 2, CONTENT_Y + CONTENT_H / 2 + 8);
    tft.print("NO AIRCRAFT IN RANGE");
    drawStatusBar();
    delay(2500);
    currentScreen = SCREEN_WEATHER;
    renderWeather();
  } else {
    currentScreen = SCREEN_FLIGHT;
    renderFlight(flights[0]);
  }
}

// ─── OTA progress overlay ─────────────────────────────
void drawOtaProgress(int pct) {
  static bool first = true;
  if (first) {
    first = false;
    tft.fillScreen(C_BG);
    tft.setTextColor(C_AMBER, C_BG);
    tft.setTextSize(3);
    tft.setCursor(100, 110);
    tft.print("OTA UPDATE");
    tft.setTextSize(2);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(80, 155);
    tft.print("Do not power off");
  }
  const int BX = 40, BY = 210, BW = 400, BH = 24;
  tft.drawRect(BX, BY, BW, BH, C_AMBER);
  tft.fillRect(BX + 1, BY + 1, (BW - 2) * pct / 100, BH - 2, C_GREEN);
}

// ─── Setup ────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  bootSequence();
  if (SD.begin(SD_CS)) {
    sdAvailable = true;
    Serial.println("SD card ready");
    loadConfig();
  } else {
    Serial.println("SD card not found — continuing without");
  }
  initTouch();

  // Load WiFi credentials and geofence from NVS; enter portal on first boot
  if (!loadWiFiConfig()) {
    startCaptivePortal();  // blocks until saved; restarts device
  }

  // ── Animated WiFi connection screen ──────────────────
  // Draws a scanning bar, dot progress, and attempt counter.
  // Everything is drawn inside the wait loop so it animates live.

  // Draw the static parts once
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, W, HDR_H, C_AMBER);
  tft.setTextColor(C_BG, C_AMBER);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print("OVERHEAD TRACKER");

  // Title
  tft.setTextColor(C_AMBER, C_BG);
  tft.setTextSize(2);
  tft.setCursor(16, 60);
  tft.print("CONNECTING TO WIFI");

  // SSID
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextSize(1);
  tft.setCursor(16, 88);
  tft.print("NETWORK: ");
  tft.setTextColor(C_AMBER, C_BG);
  tft.print(WIFI_SSID);

  // Static bar track (empty bar outline)
  const int BAR_X = 16;
  const int BAR_Y = 120;
  const int BAR_W = W - 32;
  const int BAR_H = 6;
  tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, C_DIMMER);

  // Dot row area — 20 dots across, one per attempt
  const int DOT_Y   = 148;
  const int DOT_SPACING = (BAR_W) / 20;

  // Attempt counter label
  tft.setTextColor(C_DIMMER, C_BG);
  tft.setTextSize(1);
  tft.setCursor(16, 174);
  tft.print("ATTEMPTING...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;

  // Scanning bar position — animates left-to-right and back (ping-pong)
  int scanPos   = 0;
  int scanDir   = 1;
  const int SCAN_W = 48;  // width of the lit segment

  while (WiFi.status() != WL_CONNECTED && attempts < 40) {

    // ── Erase old scan segment ──
    tft.fillRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, C_BG);

    // ── Draw new scan segment ──
    int segX = BAR_X + 1 + scanPos;
    int segMaxW = BAR_W - 2 - scanPos;
    int segW = min(SCAN_W, segMaxW);
    if (segW > 0) tft.fillRect(segX, BAR_Y + 1, segW, BAR_H - 2, C_AMBER);

    // Advance ping-pong
    scanPos += scanDir * 4;
    if (scanPos + SCAN_W >= BAR_W - 2) { scanPos = BAR_W - 2 - SCAN_W; scanDir = -1; }
    if (scanPos <= 0)                   { scanPos = 0;                   scanDir =  1; }

    // ── Dot progress (one dot per attempt, max 20) ──
    int dotIdx = attempts % 20;
    int dotX   = BAR_X + dotIdx * DOT_SPACING + DOT_SPACING / 2;
    // Reset row if we wrap
    if (dotIdx == 0 && attempts > 0) {
      tft.fillRect(BAR_X, DOT_Y, BAR_W, 8, C_BG);
    }
    tft.fillCircle(dotX, DOT_Y + 4, 2, C_DIM);

    // ── Attempt counter ──
    char countBuf[24];
    snprintf(countBuf, sizeof(countBuf), "ATTEMPT %d / 40", attempts + 1);
    tft.fillRect(16, 174, 200, 10, C_BG);
    tft.setTextColor(C_DIMMER, C_BG);
    tft.setTextSize(1);
    tft.setCursor(16, 174);
    tft.print(countBuf);

    delay(500);
    attempts++;
  }

  // ── Connection result flash ───────────────────────────
  if (WiFi.status() == WL_CONNECTED) {
    // Fill the scan bar solid green and show CONNECTED
    tft.fillRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, C_GREEN);
    tft.fillRect(16, 174, W - 32, 10, C_BG);
    tft.setTextColor(C_GREEN, C_BG);
    tft.setTextSize(1);
    tft.setCursor(16, 174);
    tft.print("CONNECTED");
    // Flash the whole bar 3x
    for (int i = 0; i < 3; i++) {
      tft.fillRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, C_BG);
      delay(100);
      tft.fillRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, C_GREEN);
      delay(120);
    }
    delay(300);
    // Sync NTP so cache timestamps are meaningful
    configTime(0, 0, "pool.ntp.org");
    Serial.println("NTP sync started");
    ArduinoOTA.setHostname("overhead-tracker");
    ArduinoOTA.onStart([]() {
      drawOtaProgress(0);
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      drawOtaProgress(progress * 100 / total);
    });
    ArduinoOTA.onEnd([]() {
      tft.setTextColor(C_GREEN, C_BG);
      tft.setTextSize(2);
      tft.setCursor(160, 250);
      tft.print("Restarting...");
    });
    ArduinoOTA.onError([](ota_error_t error) {
      tft.setTextColor(TFT_RED, C_BG);
      tft.setTextSize(2);
      tft.setCursor(120, 250);
      tft.printf("OTA Error [%u]", error);
      delay(3000);
      ESP.restart();
    });
    ArduinoOTA.begin();
    Serial.println("OTA ready — overhead-tracker.local");
  }

  // Hardware watchdog: reboot if loop() stalls for > 30 s (core 3.x struct API)
  const esp_task_wdt_config_t wdt_cfg = { .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_init(&wdt_cfg);
  esp_task_wdt_add(NULL);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi failed, attempting cache...");
    String cached = readCache();
    if (!cached.isEmpty()) {
      int n = parsePayload(cached);
      if (n > 0) {
        memcpy(flights, newFlights, sizeof(Flight) * n);
        flightCount = n;
        usingCache  = true;
        dataSource  = 2;
        renderFlight(flights[0]);
        countdown = REFRESH_SECS;
        return;
      }
    }
    // WiFi failed — show RECONFIGURE / RETRY options
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, W, HDR_H, C_AMBER);
    tft.setTextColor(C_BG, C_AMBER);
    tft.setTextSize(2);
    tft.setCursor(8, 6);
    tft.print("OVERHEAD TRACKER");
    tft.setTextColor(C_RED, C_BG);
    tft.setTextSize(2);
    tft.setCursor(16, 58);
    tft.print("WIFI FAILED");
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextSize(1);
    tft.setCursor(16, 88);
    tft.print("Could not connect to: ");
    tft.setTextColor(C_AMBER, C_BG);
    tft.print(WIFI_SSID);
    // RECONFIGURE button (left)
    tft.fillRect(16, 112, 200, 44, C_DIMMER);
    tft.setTextColor(C_AMBER, C_DIMMER);
    tft.setTextSize(1);
    tft.setCursor(28, 124);
    tft.print("RECONFIGURE");
    tft.setCursor(28, 138);
    tft.print("Change WiFi/location");
    // RETRY button (right)
    tft.fillRect(260, 112, 200, 44, C_DIMMER);
    tft.setTextColor(C_AMBER, C_DIMMER);
    tft.setTextSize(1);
    tft.setCursor(272, 124);
    tft.print("RETRY");
    tft.setCursor(272, 138);
    tft.print("Reboot and try again");
    // Wait for tap
    while (true) {
      if (touchReady) {
        uint16_t tx, ty;
        if (tft.getTouch(&tx, &ty)) {
          if (ty >= 112 && ty <= 156) {
            if (tx >= 16 && tx <= 216) {
              // RECONFIGURE: clear saved SSID so portal triggers on next boot
              Preferences p;
              p.begin("tracker", false);
              p.remove("wifi_ssid");
              p.end();
              startCaptivePortal();
            } else if (tx >= 260 && tx <= 460) {
              ESP.restart();
            }
          }
        }
      }
      delay(50);
    }
  }

  // ── Geocode location name if needed ─────────────────
  if (needsGeocode && HOME_QUERY[0]) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, W, HDR_H, C_AMBER);
    tft.setTextColor(C_BG, C_AMBER);
    tft.setTextSize(2);
    tft.setCursor(8, 6);
    tft.print("OVERHEAD TRACKER");
    tft.setTextColor(C_AMBER, C_BG);
    tft.setTextSize(2);
    tft.setCursor(16, 60);
    tft.print("LOCATING...");
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextSize(1);
    tft.setCursor(16, 90);
    tft.print(HOME_QUERY);

    if (!geocodeLocation(HOME_QUERY)) {
      // Geocode failed — show error with RECONFIGURE option
      tft.fillScreen(C_BG);
      tft.fillRect(0, 0, W, HDR_H, C_AMBER);
      tft.setTextColor(C_BG, C_AMBER);
      tft.setTextSize(2);
      tft.setCursor(8, 6);
      tft.print("OVERHEAD TRACKER");
      tft.setTextColor(C_RED, C_BG);
      tft.setTextSize(2);
      tft.setCursor(16, 60);
      tft.print("LOCATION NOT FOUND");
      tft.setTextColor(C_DIM, C_BG);
      tft.setTextSize(1);
      tft.setCursor(16, 90);
      tft.print(HOME_QUERY);
      tft.fillRect(16, 120, 200, 44, C_DIMMER);
      tft.setTextColor(C_AMBER, C_DIMMER);
      tft.setTextSize(1);
      tft.setCursor(28, 132);
      tft.print("RECONFIGURE");
      tft.setCursor(28, 146);
      tft.print("Change WiFi/location");
      tft.fillRect(260, 120, 200, 44, C_DIMMER);
      tft.setTextColor(C_AMBER, C_DIMMER);
      tft.setCursor(272, 132);
      tft.print("CONTINUE");
      tft.setCursor(272, 146);
      tft.print("Use default location");
      while (true) {
        if (touchReady) {
          uint16_t tx, ty;
          if (tft.getTouch(&tx, &ty)) {
            if (ty >= 120 && ty <= 164) {
              if (tx >= 16 && tx <= 216) {
                Preferences p; p.begin("tracker", false); p.remove("wifi_ssid"); p.end();
                startCaptivePortal();
              } else if (tx >= 260) {
                break;  // continue with hardcoded defaults
              }
            }
          }
        }
        delay(50);
      }
    }
  }

  fetchFlights();
  countdown = REFRESH_SECS;
  fetchWeather();
  wxCountdown = WX_REFRESH_SECS;
}

// ─── Loop ─────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();
  ArduinoOTA.handle();

  // ── Touch polling ──────────────────────────────────
  if (touchReady) {
    uint16_t tx, ty;
    if (tft.getTouch(&tx, &ty)) {
      handleTouch(tx, ty);
    }
  }

  if (now - lastTick >= 1000) {
    lastTick = now;
    countdown--;
    wxCountdown--;

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }

    if (currentScreen == SCREEN_FLIGHT && flightCount > 0) drawStatusBar();

    // Update clock on weather screen when the minute changes
    if (currentScreen == SCREEN_WEATHER) {
      time_t utcNow   = time(NULL);
      time_t localNow = (wxReady && wxData.utc_offset_secs != 0)
                          ? utcNow + wxData.utc_offset_secs : utcNow;
      struct tm* t = gmtime(&localNow);
      int curMin = t->tm_hour * 60 + t->tm_min;
      if (curMin != lastMinute) {
        lastMinute = curMin;
        renderWeather();
      }
    }

    if (countdown <= 0) {
      fetchFlights();
      countdown = REFRESH_SECS;
      lastCycle = millis();
    }

    if (wxCountdown <= 0) {
      fetchWeather();
      wxCountdown = WX_REFRESH_SECS;
      if (currentScreen == SCREEN_WEATHER) renderWeather();
    }
  }

  if (flightCount > 1 && currentScreen == SCREEN_FLIGHT &&
      now - lastCycle >= (unsigned long)CYCLE_SECS * 1000) {
    lastCycle = now;
    flightIndex = (flightIndex + 1) % flightCount;
    renderFlight(flights[flightIndex]);
  }
}