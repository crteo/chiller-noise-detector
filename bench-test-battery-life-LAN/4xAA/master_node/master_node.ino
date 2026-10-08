#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include "config.h"

static_assert(AP_CHANNEL == 11, "This bench test must use Wi-Fi channel 11");

WebServer server(80);
uint32_t acceptedMeasurements = 0;

struct LatestMeasurement {
  bool available = false;
  String nodeId;
  String powerSource;
  uint32_t sequence = 0;
  uint32_t rtcUnix = 0;
  float aWeightedDBFS = NAN;
  float estimatedDBA = NAN;
  float peakDBFS = NAN;
  uint32_t sampleCount = 0;
  uint32_t integrationMs = 0;
  uint32_t fftFrames = 0;
  float dominantFrequencyHz = NAN;
  uint32_t captureMs = 0;
  uint32_t awakeMs = 0;
  int32_t wifiRSSIDBm = 0;
  uint32_t receivedAtMs = 0;
} latest;

bool authorized() {
  return server.hasHeader("X-API-Key") &&
         server.header("X-API-Key") == API_KEY;
}

void handleMeasurement() {
  if (!authorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  JsonDocument document;
  const auto error = deserializeJson(document, server.arg("plain"));
  if (error || !document["node_id"].is<const char*>() ||
      !document["sequence"].is<uint32_t>() ||
      !document["a_weighted_dbfs"].is<double>()) {
    server.send(422, "application/json", "{\"error\":\"invalid_payload\"}");
    return;
  }
  ++acceptedMeasurements;
  latest.available = true;
  latest.nodeId = document["node_id"] | "";
  latest.powerSource = document["power_source"] | "";
  latest.sequence = document["sequence"] | 0UL;
  latest.rtcUnix = document["rtc_unix"] | 0UL;
  latest.aWeightedDBFS = document["a_weighted_dbfs"] | NAN;
  latest.estimatedDBA = document["estimated_dba"] | NAN;
  latest.peakDBFS = document["peak_dbfs"] | NAN;
  latest.sampleCount = document["sample_count"] | 0UL;
  latest.integrationMs = document["integration_ms"] | 0UL;
  latest.fftFrames = document["fft_frames"] | 0UL;
  latest.dominantFrequencyHz = document["dominant_frequency_hz"] | NAN;
  latest.captureMs = document["capture_ms"] | 0UL;
  latest.awakeMs = document["awake_ms"] | 0UL;
  latest.wifiRSSIDBm = document["wifi_rssi_dbm"] | 0;
  latest.receivedAtMs = millis();
  Serial.printf(
      "CSV,%s,%s,%lu,%lu,%.3f,%.3f,%.3f,%lu,%lu,%lu,%.1f,%lu,%lu,%ld\n",
      document["node_id"] | "", document["power_source"] | "",
      static_cast<unsigned long>(document["sequence"] | 0UL),
      static_cast<unsigned long>(document["rtc_unix"] | 0UL),
      document["a_weighted_dbfs"] | NAN, document["estimated_dba"] | NAN,
      document["peak_dbfs"] | NAN,
      static_cast<unsigned long>(document["sample_count"] | 0UL),
      static_cast<unsigned long>(document["integration_ms"] | 0UL),
      static_cast<unsigned long>(document["fft_frames"] | 0UL),
      document["dominant_frequency_hz"] | NAN,
      static_cast<unsigned long>(document["capture_ms"] | 0UL),
      static_cast<unsigned long>(document["awake_ms"] | 0UL),
      static_cast<long>(document["wifi_rssi_dbm"] | 0));
  server.send(202, "application/json", "{\"accepted\":true}");
}

void handleStatus() {
  JsonDocument document;
  document["accepted_measurements"] = acceptedMeasurements;
  document["uptime_ms"] = millis();
  document["ip"] = WiFi.softAPIP().toString();
  document["channel"] = AP_CHANNEL;
  document["connected_clients"] = WiFi.softAPgetStationNum();
  document["has_measurement"] = latest.available;
  if (latest.available) {
    document["node_id"] = latest.nodeId;
    document["power_source"] = latest.powerSource;
    document["sequence"] = latest.sequence;
    document["rtc_unix"] = latest.rtcUnix;
    document["a_weighted_dbfs"] = latest.aWeightedDBFS;
    document["estimated_dba"] = latest.estimatedDBA;
    document["peak_dbfs"] = latest.peakDBFS;
    document["sample_count"] = latest.sampleCount;
    document["integration_ms"] = latest.integrationMs;
    document["fft_frames"] = latest.fftFrames;
    document["dominant_frequency_hz"] = latest.dominantFrequencyHz;
    document["capture_ms"] = latest.captureMs;
    document["awake_ms"] = latest.awakeMs;
    document["wifi_rssi_dbm"] = latest.wifiRSSIDBm;
    document["age_ms"] = millis() - latest.receivedAtMs;
  }
  String response;
  serializeJson(document, response);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", response);
}

void sendDashboard() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    server.send(503, "text/plain",
                "Dashboard missing. Run: pio run --target uploadfs");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  if (!LittleFS.begin(true)) {
    Serial.println("[FATAL] Could not mount LittleFS.");
    while (true) delay(1000);
  }
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false,
                   AP_MAX_CONNECTIONS)) {
    Serial.println("[FATAL] Could not start access point.");
    while (true) delay(1000);
  }
  const char* headers[] = {"X-API-Key"};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, sendDashboard);
  server.on("/measurement", HTTP_POST, handleMeasurement);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/favicon.ico", HTTP_GET,
            []() { server.send(204, "text/plain", ""); });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.printf("[READY] SSID=%s IP=%s channel=%u dashboard=http://%s/\n",
                AP_SSID, WiFi.softAPIP().toString().c_str(), AP_CHANNEL,
                WiFi.softAPIP().toString().c_str());
}

void loop() {
  server.handleClient();
  delay(2);
}
