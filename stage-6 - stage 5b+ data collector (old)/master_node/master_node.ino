#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <math.h>
#include "config.h"

static_assert(sizeof(AP_PASSWORD) > 8, "AP password must be at least 8 characters");
static_assert(CAPTURE_DURATION_MS < CAPTURE_INTERVAL_MS,
              "Capture duration must be shorter than the interval");

IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);
WebServer server(80);

constexpr size_t NODE_COUNT = 2;
constexpr size_t BAND_COUNT = 4;
constexpr size_t PEAK_COUNT = 3;
constexpr size_t MAX_BODY_BYTES = 4096;

const char* BAND_NAMES[BAND_COUNT] = {
  "60_200_hz", "200_1000_hz", "1000_5000_hz", "5000_15000_hz"
};

struct NodeState {
  const char* id;
  const char* name;
  const char* role;
  bool hasReport;
  uint32_t captureID;
  uint32_t receivedAtMs;
  uint32_t sampleCount;
  uint32_t integrationMs;
  int32_t wifiRSSIDBm;
  double broadbandDBFS;
  double estimatedDBZ;
  double estimatedDBA;
  double peakDBFS;
  double crestFactor;
  double kurtosis;
  double zeroCrossingRate;
  double clippedFraction;
  double bandDBFS[BAND_COUNT];
  double peakHz[PEAK_COUNT];
  double peakSpectralDBFS[PEAK_COUNT];
};

NodeState nodes[NODE_COUNT] = {
  {
    NEAR_NODE_ID, NEAR_NODE_NAME, "near", false, 0, 0, 0, 0, 0,
    NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN,
    {NAN, NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN}
  },
  {
    REFERENCE_NODE_ID, REFERENCE_NODE_NAME, "reference", false, 0, 0, 0, 0, 0,
    NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN,
    {NAN, NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN}
  }
};

uint32_t sessionID = 0;
uint32_t firstCaptureStartMs = 0;
bool clockSet = false;
uint64_t epochAtSyncMs = 0;
uint32_t uptimeAtSyncMs = 0;

const char DASHBOARD[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CWP1-2 Acoustic Pilot</title><style>
body{margin:0;background:#101316;color:#e8edf1;font:15px system-ui,sans-serif}main{max-width:1050px;margin:auto;padding:24px}
h1{font-size:25px;margin:0 0 4px}.sub{color:#93a0aa;margin-bottom:20px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:14px}
.card{background:#1a2025;border:1px solid #2c353d;border-radius:14px;padding:17px}.role{color:#86c7ff;text-transform:uppercase;font-size:11px;letter-spacing:.12em}
.value{font-size:32px;font-weight:650;margin:8px 0}.muted{color:#93a0aa}.row{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid #273038}
.ok{color:#7ee2a8}.bad{color:#ff9898}.contrast{color:#ffd37e}table{width:100%;border-collapse:collapse;margin-top:8px}td,th{text-align:right;padding:6px}td:first-child,th:first-child{text-align:left}
</style></head><body><main><h1>CWP1-2 acoustic pilot</h1><div class="sub">Near-field and background channels remain independent.</div>
<div id="summary" class="card"></div><div id="nodes" class="grid" style="margin-top:14px"></div><div id="contrast" class="card" style="margin-top:14px"></div>
<script>
const f=(v,n=2)=>Number.isFinite(Number(v))?Number(v).toFixed(n):"--";
async function poll(){try{const r=await fetch('/api/v1/status',{cache:'no-store'}),d=await r.json();
document.querySelector('#summary').innerHTML=`<div class="row"><span>Master clock</span><span class="${d.clock_set?'ok':'bad'}">${d.clock_set?'synchronised':'waiting for collector'}</span></div><div class="row"><span>Session</span><span>${d.session_id}</span></div><div class="row"><span>Next capture</span><span>#${d.schedule.next_capture_id} · ${d.schedule.next_capture_utc||'UTC unavailable'}</span></div><div class="row"><span>Window</span><span>${d.schedule.capture_duration_ms/1000}s every ${d.schedule.capture_interval_ms/1000}s</span></div>`;
document.querySelector('#nodes').innerHTML=d.nodes.map(n=>`<section class="card"><div class="role">${n.role}</div><h2>${n.name}</h2><div class="value">${f(n.broadband_dbfs_unweighted)} <small>dBFS</small></div><div class="row"><span>Status</span><span class="${n.online?'ok':'bad'}">${n.online?'online':'stale'}</span></div><div class="row"><span>Capture</span><span>#${n.capture_id||'--'}</span></div><div class="row"><span>Estimated dB(Z)</span><span>${f(n.estimated_dbz)}</span></div><div class="row"><span>Estimated dB(A)</span><span>${f(n.estimated_dba)}</span></div><div class="row"><span>Crest / kurtosis</span><span>${f(n.crest_factor)} / ${f(n.kurtosis)}</span></div><div class="row"><span>RSSI</span><span>${n.wifi_rssi_dbm??'--'} dBm</span></div></section>`).join('');
const c=d.contrast;if(!c.available){document.querySelector('#contrast').innerHTML='<h2>Near/reference contrast</h2><div class="muted">Waiting for both nodes to report the same capture.</div>'}else{document.querySelector('#contrast').innerHTML=`<h2>Near/reference contrast · capture #${c.capture_id}</h2><div class="value contrast">${f(c.broadband_db_difference)} dB</div><table><tr><th>Band</th><th>Near − reference</th></tr>${Object.entries(c.band_db_difference).map(([k,v])=>`<tr><td>${k.replaceAll('_',' ')}</td><td>${f(v)} dB</td></tr>`).join('')}</table>`}}
catch(e){console.error(e)}}poll();setInterval(poll,2000);
</script></main></body></html>)HTML";

int findNode(const char* id) {
  for (size_t i = 0; i < NODE_COUNT; ++i) {
    if (strcmp(nodes[i].id, id) == 0) return static_cast<int>(i);
  }
  return -1;
}

uint64_t unixNowMs(uint32_t now) {
  if (!clockSet) return 0;
  return epochAtSyncMs + static_cast<uint32_t>(now - uptimeAtSyncMs);
}

void nextCapture(uint32_t now, uint32_t& captureID, uint32_t& startMs) {
  if (static_cast<int32_t>(now - firstCaptureStartMs) < 0) {
    captureID = 1;
    startMs = firstCaptureStartMs;
    return;
  }
  const uint32_t elapsed = now - firstCaptureStartMs;
  const uint32_t currentIndex = elapsed / CAPTURE_INTERVAL_MS;
  captureID = currentIndex + 2;
  startMs = firstCaptureStartMs + (currentIndex + 1) * CAPTURE_INTERVAL_MS;
}

void sendError(int status, const char* error) {
  JsonDocument doc;
  doc["error"] = error;
  String response;
  serializeJson(doc, response);
  server.send(status, "application/json", response);
}

bool authorized() {
  return server.hasHeader("X-API-Key") && server.header("X-API-Key") == API_KEY;
}

bool parseBody(JsonDocument& doc) {
  if (!server.hasArg("plain")) {
    sendError(400, "missing_body");
    return false;
  }
  const String& body = server.arg("plain");
  if (body.isEmpty() || body.length() > MAX_BODY_BYTES) {
    sendError(413, "invalid_body_size");
    return false;
  }
  if (deserializeJson(doc, body)) {
    sendError(400, "invalid_json");
    return false;
  }
  return true;
}

void handleTimeSync() {
  if (!authorized()) {
    sendError(401, "unauthorized");
    return;
  }
  JsonDocument doc;
  if (!parseBody(doc)) return;
  const uint64_t unixMs = doc["unix_ms"] | 0ULL;
  if (unixMs < 1700000000000ULL) {
    sendError(422, "invalid_unix_ms");
    return;
  }
  const uint32_t now = millis();
  epochAtSyncMs = unixMs;
  uptimeAtSyncMs = now;
  clockSet = true;
  Serial.printf("[CLOCK] UTC synchronized at uptime %lu ms\n", (unsigned long)now);
  server.send(200, "application/json", "{\"synchronized\":true}");
}

void handleSchedule() {
  const uint32_t now = millis();
  uint32_t captureID, startMs;
  nextCapture(now, captureID, startMs);
  JsonDocument doc;
  doc["session_id"] = sessionID;
  doc["master_uptime_ms"] = now;
  doc["clock_set"] = clockSet;
  doc["capture_interval_ms"] = CAPTURE_INTERVAL_MS;
  doc["capture_duration_ms"] = CAPTURE_DURATION_MS;
  doc["next_capture_id"] = captureID;
  doc["next_capture_start_ms"] = startMs;
  if (clockSet) {
    doc["next_capture_unix_ms"] = unixNowMs(now) + static_cast<uint32_t>(startMs - now);
  } else {
    doc["next_capture_unix_ms"] = nullptr;
  }
  String response;
  serializeJson(doc, response);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", response);
}

void handleFeaturePost() {
  if (!authorized()) {
    sendError(401, "unauthorized");
    return;
  }
  JsonDocument doc;
  if (!parseBody(doc)) return;

  const char* nodeID = doc["node_id"] | "";
  const int index = findNode(nodeID);
  if (index < 0) {
    sendError(403, "unknown_node");
    return;
  }
  if (!doc["session_id"].is<uint32_t>() ||
      !doc["capture_id"].is<uint32_t>() ||
      !doc["broadband_dbfs_unweighted"].is<double>()) {
    sendError(422, "missing_or_invalid_fields");
    return;
  }
  if (doc["session_id"].as<uint32_t>() != sessionID) {
    sendError(409, "wrong_session");
    return;
  }

  NodeState& node = nodes[index];
  const uint32_t captureID = doc["capture_id"].as<uint32_t>();
  if (node.hasReport && captureID <= node.captureID) {
    sendError(409, "old_capture");
    return;
  }

  const double broadband = doc["broadband_dbfs_unweighted"].as<double>();
  if (!isfinite(broadband) || broadband < -160.0 || broadband > 6.0) {
    sendError(422, "reading_out_of_range");
    return;
  }

  node.hasReport = true;
  node.captureID = captureID;
  node.receivedAtMs = millis();
  node.sampleCount = doc["sample_count"] | 0UL;
  node.integrationMs = doc["integration_ms"] | 0UL;
  node.wifiRSSIDBm = doc["wifi_rssi_dbm"] | 0;
  node.broadbandDBFS = broadband;
  node.estimatedDBZ = doc["estimated_dbz"] | NAN;
  node.estimatedDBA = doc["estimated_dba"] | NAN;
  node.peakDBFS = doc["peak_dbfs"] | NAN;
  node.crestFactor = doc["crest_factor"] | NAN;
  node.kurtosis = doc["kurtosis"] | NAN;
  node.zeroCrossingRate = doc["zero_crossing_rate"] | NAN;
  node.clippedFraction = doc["clipped_fraction"] | NAN;

  JsonArray bands = doc["band_dbfs"].as<JsonArray>();
  for (size_t i = 0; i < BAND_COUNT; ++i) {
    node.bandDBFS[i] = i < bands.size() ? bands[i].as<double>() : NAN;
  }
  JsonArray peaks = doc["spectral_peaks"].as<JsonArray>();
  for (size_t i = 0; i < PEAK_COUNT; ++i) {
    if (i < peaks.size()) {
      node.peakHz[i] = peaks[i]["hz"] | NAN;
      node.peakSpectralDBFS[i] = peaks[i]["dbfs"] | NAN;
    } else {
      node.peakHz[i] = NAN;
      node.peakSpectralDBFS[i] = NAN;
    }
  }

  Serial.printf("[FEATURE] %s capture=%lu broadband=%.2f dBFS\n",
                node.id, (unsigned long)captureID, broadband);
  server.send(202, "application/json", "{\"accepted\":true}");
}

void addNullable(JsonObject object, const char* key, double value) {
  if (isfinite(value)) object[key] = value;
  else object[key] = nullptr;
}

void handleStatus() {
  const uint32_t now = millis();
  uint32_t nextID, nextStart;
  nextCapture(now, nextID, nextStart);

  JsonDocument doc;
  doc["session_id"] = sessionID;
  doc["clock_set"] = clockSet;
  doc["master_uptime_ms"] = now;
  JsonObject schedule = doc["schedule"].to<JsonObject>();
  schedule["next_capture_id"] = nextID;
  schedule["next_capture_start_ms"] = nextStart;
  schedule["capture_interval_ms"] = CAPTURE_INTERVAL_MS;
  schedule["capture_duration_ms"] = CAPTURE_DURATION_MS;
  if (clockSet) {
    const uint64_t nextUnix = unixNowMs(now) + static_cast<uint32_t>(nextStart - now);
    schedule["next_capture_unix_ms"] = nextUnix;
    time_t seconds = static_cast<time_t>(nextUnix / 1000ULL);
    struct tm utc;
    gmtime_r(&seconds, &utc);
    char text[25];
    strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    schedule["next_capture_utc"] = text;
  } else {
    schedule["next_capture_unix_ms"] = nullptr;
    schedule["next_capture_utc"] = nullptr;
  }

  JsonArray nodeArray = doc["nodes"].to<JsonArray>();
  for (size_t n = 0; n < NODE_COUNT; ++n) {
    NodeState& node = nodes[n];
    JsonObject item = nodeArray.add<JsonObject>();
    item["id"] = node.id;
    item["name"] = node.name;
    item["role"] = node.role;
    item["has_report"] = node.hasReport;
    item["online"] = node.hasReport &&
      static_cast<uint32_t>(now - node.receivedAtMs) <= SENSOR_STALE_MS;
    item["capture_id"] = node.captureID;
    item["age_ms"] = node.hasReport ? now - node.receivedAtMs : 0;
    item["sample_count"] = node.sampleCount;
    item["integration_ms"] = node.integrationMs;
    item["wifi_rssi_dbm"] = node.wifiRSSIDBm;
    addNullable(item, "broadband_dbfs_unweighted", node.broadbandDBFS);
    addNullable(item, "estimated_dbz", node.estimatedDBZ);
    addNullable(item, "estimated_dba", node.estimatedDBA);
    addNullable(item, "peak_dbfs", node.peakDBFS);
    addNullable(item, "crest_factor", node.crestFactor);
    addNullable(item, "kurtosis", node.kurtosis);
    addNullable(item, "zero_crossing_rate", node.zeroCrossingRate);
    addNullable(item, "clipped_fraction", node.clippedFraction);
    JsonObject bands = item["band_dbfs"].to<JsonObject>();
    for (size_t i = 0; i < BAND_COUNT; ++i) addNullable(bands, BAND_NAMES[i], node.bandDBFS[i]);
    JsonArray peaks = item["spectral_peaks"].to<JsonArray>();
    for (size_t i = 0; i < PEAK_COUNT; ++i) {
      JsonObject peak = peaks.add<JsonObject>();
      addNullable(peak, "hz", node.peakHz[i]);
      addNullable(peak, "dbfs", node.peakSpectralDBFS[i]);
    }
  }

  JsonObject contrast = doc["contrast"].to<JsonObject>();
  const bool comparable = nodes[0].hasReport && nodes[1].hasReport &&
    nodes[0].captureID == nodes[1].captureID;
  contrast["available"] = comparable;
  if (comparable) {
    contrast["capture_id"] = nodes[0].captureID;
    contrast["broadband_db_difference"] =
      nodes[0].broadbandDBFS - nodes[1].broadbandDBFS;
    JsonObject bandDifference = contrast["band_db_difference"].to<JsonObject>();
    for (size_t i = 0; i < BAND_COUNT; ++i) {
      if (isfinite(nodes[0].bandDBFS[i]) && isfinite(nodes[1].bandDBFS[i])) {
        bandDifference[BAND_NAMES[i]] = nodes[0].bandDBFS[i] - nodes[1].bandDBFS[i];
      } else {
        bandDifference[BAND_NAMES[i]] = nullptr;
      }
    }
  }

  String response;
  response.reserve(5000);
  serializeJson(doc, response);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", response);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  sessionID = esp_random();
  firstCaptureStartMs = millis() + FIRST_CAPTURE_DELAY_MS;

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET) ||
      !WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS)) {
    Serial.println("[FATAL] Access point setup failed.");
    while (true) delay(1000);
  }

  const char* headers[] = {"X-API-Key"};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", DASHBOARD); });
  server.on("/api/v1/schedule", HTTP_GET, handleSchedule);
  server.on("/api/v1/features", HTTP_POST, handleFeaturePost);
  server.on("/api/v1/time", HTTP_POST, handleTimeSync);
  server.on("/api/v1/status", HTTP_GET, handleStatus);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();

  Serial.println("\n========================================");
  Serial.println("CWP1-2 STAGE 6 ACOUSTIC MASTER");
  Serial.printf("Session   : %lu\n", (unsigned long)sessionID);
  Serial.printf("Network   : %s\n", AP_SSID);
  Serial.printf("Dashboard : http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("Waiting for collector clock synchronization.");
  Serial.println("========================================");
}

void loop() {
  server.handleClient();
  delay(2);
}
