#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <math.h>
#include <time.h>

// Local acquisition remains independent of the cloud. The laptop collector
// owns Firebase credentials; this ESP32 exposes only the private local API.
constexpr char FIRMWARE_VERSION[] = "stage7-master-v1.0.0";
constexpr size_t NODE_COUNT = 2, BAND_COUNT = 4, PEAK_COUNT = 3;
constexpr size_t MAX_BODY_BYTES = 4096;
constexpr uint64_t MIN_VALID_UNIX_MS = 1700000000000ULL;
const char* BAND_NAMES[BAND_COUNT] = {"60_200_hz", "200_1000_hz", "1000_5000_hz", "5000_15000_hz"};

struct MasterConfig {
  String masterID, apSSID, apPassword, upstreamSSID, upstreamPassword;
  String ntpServer1, ntpServer2, apiKey;
  uint8_t apChannel, apMaxConnections;
  bool upstreamEnabled;
  uint32_t captureIntervalMs, captureDurationMs, firstCaptureDelayMs, sensorStaleMs;
  String nodeID[NODE_COUNT], nodeName[NODE_COUNT], nodeRole[NODE_COUNT];
};
struct NodeState {
  bool hasReport = false;
  uint32_t captureID = 0, receivedAtMs = 0, sampleCount = 0, integrationMs = 0;
  int32_t wifiRSSIDBm = 0;
  double broadbandDBFS = NAN, estimatedDBZ = NAN, estimatedDBA = NAN, peakDBFS = NAN;
  double crestFactor = NAN, kurtosis = NAN, zeroCrossingRate = NAN, clippedFraction = NAN;
  double bandDBFS[BAND_COUNT] = {NAN, NAN, NAN, NAN};
  double peakHz[PEAK_COUNT] = {NAN, NAN, NAN};
  double peakSpectralDBFS[PEAK_COUNT] = {NAN, NAN, NAN};
};

MasterConfig config;
NodeState nodes[NODE_COUNT];
WebServer server(80);
IPAddress AP_IP(192, 168, 4, 1), AP_GATEWAY(192, 168, 4, 1), AP_SUBNET(255, 255, 255, 0);
uint32_t sessionID = 0, firstCaptureStartMs = 0, manualUptimeAtSyncMs = 0, nextUpstreamRetryAt = 0, nextNtpConfigureAt = 0;
uint64_t manualEpochAtSyncMs = 0;

const char DASHBOARD[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Stage 7 Master</title><style>body{margin:0;background:#101316;color:#edf2f5;font:15px system-ui}main{max-width:1000px;margin:auto;padding:24px}.card{background:#192026;border:1px solid #303b43;border-radius:14px;padding:16px;margin:12px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:12px}.row{display:flex;justify-content:space-between;padding:5px;border-bottom:1px solid #29333a}.value{font-size:32px;font-weight:650}.ok{color:#7ee2a8}.bad{color:#ff9898}.muted{color:#99a4ad}</style></head><body><main><h1>CWP1-2 Stage 7 local master</h1><p class=muted>Cloud presentation is hosted separately. This page verifies the local path.</p><div id=summary class=card></div><div id=nodes class=grid></div><div id=contrast class=card></div><script>const f=(x,n=2)=>Number.isFinite(Number(x))?Number(x).toFixed(n):'--';async function p(){try{const d=await(await fetch('/api/v1/status',{cache:'no-store'})).json();summary.innerHTML=`<div class=row><span>Clock</span><span class=${d.clock_set?'ok':'bad'}>${d.clock_set?'UTC ready':'not set'}</span></div><div class=row><span>Upstream</span><span class=${d.upstream.connected?'ok':'bad'}>${d.upstream.connected?d.upstream.ip:'offline'}</span></div><div class=row><span>Session</span><span>${d.session_id}</span></div><div class=row><span>Next capture</span><span>#${d.schedule.next_capture_id} ${d.schedule.next_capture_utc||''}</span></div>`;nodes.innerHTML=d.nodes.map(n=>`<section class=card><b>${n.name}</b><div class=value>${f(n.broadband_dbfs_unweighted)} dBFS</div><div class=row><span>Role</span><span>${n.role}</span></div><div class=row><span>Capture</span><span>#${n.capture_id||'--'}</span></div><div class=row><span>Status</span><span class=${n.online?'ok':'bad'}>${n.online?'online':'stale'}</span></div></section>`).join('');contrast.innerHTML=d.contrast.available?`<b>Near − reference</b><div class=value>${f(d.contrast.broadband_db_difference)} dB</div>`:'<b>Near − reference</b><p class=muted>Waiting for matching capture IDs.</p>'}catch(e){console.error(e)}}p();setInterval(p,2000)</script></main></body></html>)HTML";

[[noreturn]] void fatal(const String& message) { Serial.println("[FATAL] " + message); while (true) delay(1000); }
String requiredString(JsonVariantConst value, const char* name) {
  const char* text = value | ""; if (!text[0]) fatal(String("Missing config field: ") + name); return String(text);
}
void loadConfig() {
  if (!LittleFS.begin(false)) fatal("LittleFS mount failed; upload data/config.json");
  File file = LittleFS.open("/config.json", "r"); if (!file) fatal("Missing /config.json");
  JsonDocument doc; if (deserializeJson(doc, file)) fatal("Invalid /config.json");
  config.masterID = requiredString(doc["device"]["id"], "device.id");
  config.apSSID = requiredString(doc["local_ap"]["ssid"], "local_ap.ssid");
  config.apPassword = requiredString(doc["local_ap"]["password"], "local_ap.password");
  config.apChannel = doc["local_ap"]["channel"] | 1; config.apMaxConnections = doc["local_ap"]["max_connections"] | 6;
  config.upstreamEnabled = doc["upstream_wifi"]["enabled"] | false;
  config.upstreamSSID = doc["upstream_wifi"]["ssid"] | ""; config.upstreamPassword = doc["upstream_wifi"]["password"] | "";
  config.ntpServer1 = doc["upstream_wifi"]["ntp_server_1"] | "pool.ntp.org"; config.ntpServer2 = doc["upstream_wifi"]["ntp_server_2"] | "time.google.com";
  config.apiKey = requiredString(doc["local_api"]["shared_key"], "local_api.shared_key");
  config.captureIntervalMs = doc["capture"]["interval_ms"] | 300000UL; config.captureDurationMs = doc["capture"]["duration_ms"] | 10000UL;
  config.firstCaptureDelayMs = doc["capture"]["first_capture_delay_ms"] | 20000UL; config.sensorStaleMs = doc["capture"]["sensor_stale_ms"] | 600000UL;
  JsonArrayConst list = doc["nodes"].as<JsonArrayConst>(); if (list.size() != NODE_COUNT) fatal("Exactly two nodes must be configured");
  for (size_t i = 0; i < NODE_COUNT; ++i) { config.nodeID[i] = requiredString(list[i]["id"], "nodes[].id"); config.nodeName[i] = requiredString(list[i]["name"], "nodes[].name"); config.nodeRole[i] = requiredString(list[i]["role"], "nodes[].role"); }
  if (config.apPassword.length() < 8) fatal("AP password must have at least 8 characters");
  if (config.captureDurationMs >= config.captureIntervalMs) fatal("Capture duration must be shorter than interval");
}

bool systemClockValid() { return static_cast<uint64_t>(time(nullptr)) * 1000ULL >= MIN_VALID_UNIX_MS; }
bool clockSet() { return systemClockValid() || manualEpochAtSyncMs >= MIN_VALID_UNIX_MS; }
uint64_t unixNowMs(uint32_t now) { if (systemClockValid()) return static_cast<uint64_t>(time(nullptr)) * 1000ULL; return manualEpochAtSyncMs ? manualEpochAtSyncMs + static_cast<uint32_t>(now - manualUptimeAtSyncMs) : 0; }
void nextCapture(uint32_t now, uint32_t& id, uint32_t& start) {
  if (static_cast<int32_t>(now - firstCaptureStartMs) < 0) { id = 1; start = firstCaptureStartMs; return; }
  const uint32_t index = (now - firstCaptureStartMs) / config.captureIntervalMs; id = index + 2; start = firstCaptureStartMs + (index + 1) * config.captureIntervalMs;
}
void sendError(int status, const char* error) { server.send(status, "application/json", String("{\"error\":\"") + error + "\"}"); }
bool authorized() { return server.hasHeader("X-API-Key") && server.header("X-API-Key") == config.apiKey; }
bool parseBody(JsonDocument& doc) {
  if (!server.hasArg("plain")) { sendError(400, "missing_body"); return false; } const String& body = server.arg("plain");
  if (!body.length() || body.length() > MAX_BODY_BYTES) { sendError(413, "invalid_body_size"); return false; }
  if (deserializeJson(doc, body)) { sendError(400, "invalid_json"); return false; } return true;
}
int findNode(const char* id) { for (size_t i = 0; i < NODE_COUNT; ++i) if (config.nodeID[i] == id) return static_cast<int>(i); return -1; }
void addNullable(JsonObject object, const char* key, double value) { if (isfinite(value)) object[key] = value; else object[key] = nullptr; }

void handleTimeSync() {
  if (!authorized()) { sendError(401, "unauthorized"); return; } JsonDocument doc; if (!parseBody(doc)) return;
  const uint64_t value = doc["unix_ms"] | 0ULL; if (value < MIN_VALID_UNIX_MS) { sendError(422, "invalid_unix_ms"); return; }
  manualEpochAtSyncMs = value; manualUptimeAtSyncMs = millis(); server.send(200, "application/json", "{\"synchronized\":true}");
}
void handleSchedule() {
  const uint32_t now = millis(); uint32_t id, start; nextCapture(now, id, start); JsonDocument doc;
  doc["session_id"] = sessionID; doc["master_uptime_ms"] = now; doc["clock_set"] = clockSet(); doc["capture_interval_ms"] = config.captureIntervalMs;
  doc["capture_duration_ms"] = config.captureDurationMs; doc["next_capture_id"] = id; doc["next_capture_start_ms"] = start;
  if (clockSet()) doc["next_capture_unix_ms"] = unixNowMs(now) + static_cast<uint32_t>(start - now); else doc["next_capture_unix_ms"] = nullptr;
  String response; serializeJson(doc, response); server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", response);
}
void handleFeaturePost() {
  if (!authorized()) { sendError(401, "unauthorized"); return; } JsonDocument doc; if (!parseBody(doc)) return;
  const int index = findNode(doc["node_id"] | ""); if (index < 0) { sendError(403, "unknown_node"); return; }
  if ((doc["session_id"] | 0UL) != sessionID) { sendError(409, "wrong_session"); return; }
  const uint32_t captureID = doc["capture_id"] | 0UL; const double broadband = doc["broadband_dbfs_unweighted"] | NAN;
  if (!captureID || !isfinite(broadband) || broadband < -160.0 || broadband > 6.0) { sendError(422, "invalid_reading"); return; }
  NodeState& n = nodes[index]; if (n.hasReport && captureID <= n.captureID) { sendError(409, "old_capture"); return; }
  n.hasReport = true; n.captureID = captureID; n.receivedAtMs = millis(); n.sampleCount = doc["sample_count"] | 0UL; n.integrationMs = doc["integration_ms"] | 0UL;
  n.wifiRSSIDBm = doc["wifi_rssi_dbm"] | 0; n.broadbandDBFS = broadband; n.estimatedDBZ = doc["estimated_dbz"] | NAN; n.estimatedDBA = doc["estimated_dba"] | NAN;
  n.peakDBFS = doc["peak_dbfs"] | NAN; n.crestFactor = doc["crest_factor"] | NAN; n.kurtosis = doc["kurtosis"] | NAN;
  n.zeroCrossingRate = doc["zero_crossing_rate"] | NAN; n.clippedFraction = doc["clipped_fraction"] | NAN;
  JsonArray bands = doc["band_dbfs"].as<JsonArray>(); for (size_t i = 0; i < BAND_COUNT; ++i) n.bandDBFS[i] = i < bands.size() ? bands[i].as<double>() : NAN;
  JsonArray peaks = doc["spectral_peaks"].as<JsonArray>(); for (size_t i = 0; i < PEAK_COUNT; ++i) { n.peakHz[i] = i < peaks.size() ? (peaks[i]["hz"] | NAN) : NAN; n.peakSpectralDBFS[i] = i < peaks.size() ? (peaks[i]["dbfs"] | NAN) : NAN; }
  Serial.printf("[FEATURE] %s capture=%lu %.2f dBFS\n", config.nodeID[index].c_str(), (unsigned long)captureID, broadband); server.send(202, "application/json", "{\"accepted\":true}");
}
void appendNode(JsonArray array, size_t i, uint32_t now) {
  NodeState& n = nodes[i]; JsonObject item = array.add<JsonObject>(); item["id"] = config.nodeID[i]; item["name"] = config.nodeName[i]; item["role"] = config.nodeRole[i];
  item["has_report"] = n.hasReport; item["online"] = n.hasReport && now - n.receivedAtMs <= config.sensorStaleMs; item["capture_id"] = n.captureID;
  item["age_ms"] = n.hasReport ? now - n.receivedAtMs : 0; item["sample_count"] = n.sampleCount; item["integration_ms"] = n.integrationMs; item["wifi_rssi_dbm"] = n.wifiRSSIDBm;
  addNullable(item, "broadband_dbfs_unweighted", n.broadbandDBFS); addNullable(item, "estimated_dbz", n.estimatedDBZ); addNullable(item, "estimated_dba", n.estimatedDBA);
  addNullable(item, "peak_dbfs", n.peakDBFS); addNullable(item, "crest_factor", n.crestFactor); addNullable(item, "kurtosis", n.kurtosis);
  addNullable(item, "zero_crossing_rate", n.zeroCrossingRate); addNullable(item, "clipped_fraction", n.clippedFraction);
  JsonObject bandObject = item["band_dbfs"].to<JsonObject>(); for (size_t b = 0; b < BAND_COUNT; ++b) addNullable(bandObject, BAND_NAMES[b], n.bandDBFS[b]);
  JsonArray peakArray = item["spectral_peaks"].to<JsonArray>(); for (size_t p = 0; p < PEAK_COUNT; ++p) { JsonObject peak = peakArray.add<JsonObject>(); addNullable(peak, "hz", n.peakHz[p]); addNullable(peak, "dbfs", n.peakSpectralDBFS[p]); }
}
void handleStatus() {
  const uint32_t now = millis(); uint32_t nextID, nextStart; nextCapture(now, nextID, nextStart); JsonDocument doc;
  doc["firmware_version"] = FIRMWARE_VERSION; doc["master_id"] = config.masterID; doc["session_id"] = sessionID; doc["clock_set"] = clockSet(); doc["master_uptime_ms"] = now;
  JsonObject upstream = doc["upstream"].to<JsonObject>(); upstream["enabled"] = config.upstreamEnabled; upstream["connected"] = WiFi.status() == WL_CONNECTED;
  upstream["ssid"] = config.upstreamSSID; upstream["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : ""; upstream["rssi_dbm"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  JsonObject schedule = doc["schedule"].to<JsonObject>(); schedule["next_capture_id"] = nextID; schedule["next_capture_start_ms"] = nextStart;
  schedule["capture_interval_ms"] = config.captureIntervalMs; schedule["capture_duration_ms"] = config.captureDurationMs;
  if (clockSet()) { const uint64_t stamp = unixNowMs(now) + static_cast<uint32_t>(nextStart - now); schedule["next_capture_unix_ms"] = stamp; time_t seconds = stamp / 1000ULL; struct tm utc; gmtime_r(&seconds, &utc); char text[25]; strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc); schedule["next_capture_utc"] = text; } else { schedule["next_capture_unix_ms"] = nullptr; schedule["next_capture_utc"] = nullptr; }
  JsonArray list = doc["nodes"].to<JsonArray>(); for (size_t i = 0; i < NODE_COUNT; ++i) appendNode(list, i, now);
  JsonObject contrast = doc["contrast"].to<JsonObject>(); const bool comparable = nodes[0].hasReport && nodes[1].hasReport && nodes[0].captureID == nodes[1].captureID; contrast["available"] = comparable;
  if (comparable) { contrast["capture_id"] = nodes[0].captureID; contrast["broadband_db_difference"] = nodes[0].broadbandDBFS - nodes[1].broadbandDBFS; JsonObject differences = contrast["band_db_difference"].to<JsonObject>(); for (size_t b = 0; b < BAND_COUNT; ++b) { if (isfinite(nodes[0].bandDBFS[b]) && isfinite(nodes[1].bandDBFS[b])) differences[BAND_NAMES[b]] = nodes[0].bandDBFS[b] - nodes[1].bandDBFS[b]; else differences[BAND_NAMES[b]] = nullptr; } }
  String response; response.reserve(6000); serializeJson(doc, response); server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", response);
}
void connectUpstream() {
  if (!config.upstreamEnabled || !config.upstreamSSID.length() || WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[UPSTREAM] Connecting to %s\n", config.upstreamSSID.c_str()); WiFi.begin(config.upstreamSSID.c_str(), config.upstreamPassword.c_str()); nextUpstreamRetryAt = millis() + 15000;
}
void setup() {
  Serial.begin(115200); delay(1000); loadConfig(); sessionID = esp_random(); if (!sessionID) sessionID = 1; firstCaptureStartMs = millis() + config.firstCaptureDelayMs;
  WiFi.mode(config.upstreamEnabled ? WIFI_AP_STA : WIFI_AP); WiFi.setSleep(false);
  if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET) || !WiFi.softAP(config.apSSID.c_str(), config.apPassword.c_str(), config.apChannel, false, config.apMaxConnections)) fatal("Access point setup failed");
  connectUpstream(); const char* headers[] = {"X-API-Key"}; server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", DASHBOARD); }); server.on("/api/v1/schedule", HTTP_GET, handleSchedule);
  server.on("/api/v1/features", HTTP_POST, handleFeaturePost); server.on("/api/v1/time", HTTP_POST, handleTimeSync); server.on("/api/v1/status", HTTP_GET, handleStatus);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); }); server.begin();
  Serial.printf("\n%s ready; local dashboard http://%s\n", FIRMWARE_VERSION, WiFi.softAPIP().toString().c_str());
}
void loop() {
  server.handleClient(); const uint32_t now = millis();
  if (config.upstreamEnabled && WiFi.status() != WL_CONNECTED && static_cast<int32_t>(now - nextUpstreamRetryAt) >= 0) connectUpstream();
  if (config.upstreamEnabled && WiFi.status() == WL_CONNECTED && !systemClockValid() && static_cast<int32_t>(now - nextNtpConfigureAt) >= 0) { configTime(0, 0, config.ntpServer1.c_str(), config.ntpServer2.c_str()); nextNtpConfigureAt = now + 60000; }
  delay(2);
}
