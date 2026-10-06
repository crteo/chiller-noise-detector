#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <math.h>
#include "config.h"

// =====================================================
// ACCESS POINT AND WEB SERVER
// =====================================================

IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);
WebServer server(80);

constexpr size_t MAX_NODES = 8;
constexpr size_t CONFIGURED_NODE_COUNT = 2;
constexpr size_t MAX_READING_BODY_BYTES = 1024;
constexpr uint8_t WIFI_BGN_PROTOCOLS =
  WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;

static_assert(sizeof(AP_PASSWORD) > 8, "AP password must be at least 8 characters");
static_assert(CONFIGURED_NODE_COUNT <= MAX_NODES, "Too many configured nodes");
static_assert(AP_CHANNEL >= 1 && AP_CHANNEL <= 11, "AP channel must be 1 through 11");

struct NodeState
{
  const char* id;
  const char* name;
  bool hasReading;
  bool online;
  uint32_t bootID;
  uint32_t sequence;
  uint32_t receivedAtMs;
  uint32_t sensorUptimeMs;
  uint32_t measuredAtMs;
  uint32_t integrationMs;
  uint32_t sampleRate;
  int32_t wifiRSSIDBm;
  double dba;
  double dbfsA;
  double dominantFrequency;
  double windowEnergySum;
  uint16_t windowSampleCount;
  double lastWindowDBA;
  bool contributedLastWindow;
};

NodeState nodes[MAX_NODES] = {
  {
    NODE_1_ID, NODE_1_NAME, false, false,
    0, 0, 0, 0, 0, 0, 0, 0, NAN, NAN, NAN, 0.0, 0, NAN, false
  },
  {
    NODE_2_ID, NODE_2_NAME, false, false,
    0, 0, 0, 0, 0, 0, 0, 0, NAN, NAN, NAN, 0.0, 0, NAN, false
  }
};

SemaphoreHandle_t stateMutex = nullptr;
bool aggregateHasValue = false;
bool aggregateHeld = false;
double aggregateDBA = NAN;
uint32_t aggregateResultID = 0;
uint32_t aggregateUpdatedAtMs = 0;
uint8_t activeNodeCount = 0;
uint8_t aggregateNodeCount = 0;
uint32_t activeNodeMask = 0;

// =====================================================
// NODE STATE AND AGGREGATION
// =====================================================

int findNodeIndex(const char* nodeID)
{
  for (size_t i = 0; i < CONFIGURED_NODE_COUNT; i++)
  {
    if (strcmp(nodes[i].id, nodeID) == 0)
    {
      return (int)i;
    }
  }

  return -1;
}

void updateFreshnessLocked(uint32_t now)
{
  uint8_t newActiveCount = 0;
  uint32_t newActiveMask = 0;

  for (size_t i = 0; i < CONFIGURED_NODE_COUNT; i++)
  {
    const bool isFresh = nodes[i].hasReading &&
      (uint32_t)(now - nodes[i].receivedAtMs) <= SENSOR_STALE_MS;

    nodes[i].online = isFresh;

    if (!isFresh)
    {
      continue;
    }

    newActiveCount++;
    newActiveMask |= (1UL << i);
  }

  activeNodeCount = newActiveCount;
  activeNodeMask = newActiveMask;

  if (activeNodeCount == 0)
  {
    aggregateHeld = aggregateHasValue;
  }
}

void publishAggregateWindowLocked(uint32_t now)
{
  updateFreshnessLocked(now);

  double nodeMeanEnergySum = 0.0;
  uint8_t contributingNodes = 0;

  for (size_t i = 0; i < CONFIGURED_NODE_COUNT; i++)
  {
    nodes[i].contributedLastWindow = false;

    if (nodes[i].online && nodes[i].windowSampleCount > 0)
    {
      const double nodeMeanEnergy =
        nodes[i].windowEnergySum / nodes[i].windowSampleCount;
      nodeMeanEnergySum += nodeMeanEnergy;
      nodes[i].lastWindowDBA = 10.0 * log10(nodeMeanEnergy);
      nodes[i].contributedLastWindow = true;
      contributingNodes++;
    }

    // Every fixed window consumes its samples. Old measurements are never
    // reused in a later aggregate.
    nodes[i].windowEnergySum = 0.0;
    nodes[i].windowSampleCount = 0;
  }

  aggregateNodeCount = contributingNodes;

  if (contributingNodes == 0)
  {
    aggregateHeld = aggregateHasValue;
    return;
  }

  aggregateDBA =
    10.0 * log10(nodeMeanEnergySum / contributingNodes);
  aggregateHasValue = isfinite(aggregateDBA);
  aggregateHeld = false;
  aggregateUpdatedAtMs = now;

  if (aggregateHasValue)
  {
    aggregateResultID++;
    Serial.printf(
      "[AVERAGE] result=%lu active=%u/%u level=%.2f dBA\n",
      (unsigned long)aggregateResultID,
      aggregateNodeCount,
      (unsigned int)CONFIGURED_NODE_COUNT,
      aggregateDBA
    );
  }
}

void refreshFreshness(uint32_t now)
{
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) != pdTRUE)
  {
    return;
  }

  const uint32_t previousMask = activeNodeMask;
  updateFreshnessLocked(now);

  if (previousMask != activeNodeMask)
  {
    for (size_t i = 0; i < CONFIGURED_NODE_COUNT; i++)
    {
      const bool wasOnline = (previousMask & (1UL << i)) != 0;

      if (wasOnline != nodes[i].online)
      {
        Serial.printf(
          "[NODE] %s is now %s\n",
          nodes[i].id,
          nodes[i].online ? "connected" : "disconnected"
        );
      }
    }
  }

  xSemaphoreGive(stateMutex);
}

// =====================================================
// HTTP HANDLERS
// =====================================================

void sendJSONError(int statusCode, const char* error)
{
  String body = String("{\"error\":\"") + error + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(statusCode, "application/json", body);
}

void handleReadingPost()
{
  if (!server.hasHeader("X-API-Key") ||
      server.header("X-API-Key") != API_KEY)
  {
    sendJSONError(401, "unauthorized");
    return;
  }

  if (!server.hasArg("plain"))
  {
    sendJSONError(400, "missing_body");
    return;
  }

  const String& body = server.arg("plain");

  if (body.length() == 0 || body.length() > MAX_READING_BODY_BYTES)
  {
    sendJSONError(413, "invalid_body_size");
    return;
  }

  JsonDocument document;
  const DeserializationError jsonError = deserializeJson(document, body);

  if (jsonError)
  {
    sendJSONError(400, "invalid_json");
    return;
  }

  const char* nodeID = document["node_id"] | "";
  const int nodeIndex = findNodeIndex(nodeID);

  if (nodeIndex < 0)
  {
    sendJSONError(403, "unknown_node");
    return;
  }

  if (!document["boot_id"].is<uint32_t>() ||
      !document["sequence"].is<uint32_t>() ||
      !document["uptime_ms"].is<uint32_t>() ||
      !document["measured_at_ms"].is<uint32_t>() ||
      !document["estimated_dba"].is<double>())
  {
    sendJSONError(422, "missing_or_invalid_fields");
    return;
  }

  const uint32_t bootID = document["boot_id"].as<uint32_t>();
  const uint32_t sequence = document["sequence"].as<uint32_t>();
  const double dba = document["estimated_dba"].as<double>();

  if (!isfinite(dba) || dba < 0.0 || dba > 160.0 || sequence == 0)
  {
    sendJSONError(422, "reading_out_of_range");
    return;
  }

  const uint32_t now = millis();

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) != pdTRUE)
  {
    sendJSONError(503, "state_busy");
    return;
  }

  NodeState& node = nodes[nodeIndex];

  if (node.hasReading && node.bootID == bootID && sequence <= node.sequence)
  {
    xSemaphoreGive(stateMutex);
    sendJSONError(409, "old_sequence");
    return;
  }

  node.hasReading = true;
  node.bootID = bootID;
  node.sequence = sequence;
  node.receivedAtMs = now;
  node.sensorUptimeMs = document["uptime_ms"].as<uint32_t>();
  node.measuredAtMs = document["measured_at_ms"].as<uint32_t>();
  node.integrationMs = document["integration_ms"] | 0UL;
  node.sampleRate = document["sample_rate"] | 0UL;
  const int32_t reportedRSSI = document["wifi_rssi_dbm"] | 0;
  node.wifiRSSIDBm =
    reportedRSSI >= -127 && reportedRSSI <= 0 ? reportedRSSI : 0;
  node.dba = dba;
  node.dbfsA = document["dbfs_a"] | NAN;
  node.dominantFrequency = document["dominant_frequency"] | NAN;
  node.windowEnergySum += pow(10.0, dba / 10.0);
  node.windowSampleCount++;
  updateFreshnessLocked(now);
  xSemaphoreGive(stateMutex);

  Serial.printf(
    "[READING] %s boot=%lu seq=%lu level=%.2f dBA rssi=%ld dBm\n",
    node.id,
    (unsigned long)bootID,
    (unsigned long)sequence,
    dba,
    (long)node.wifiRSSIDBm
  );

  server.sendHeader("Cache-Control", "no-store");
  server.send(202, "application/json", "{\"accepted\":true}");
}

void sendStatus()
{
  refreshFreshness(millis());

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) != pdTRUE)
  {
    sendJSONError(503, "state_busy");
    return;
  }

  const uint32_t now = millis();
  JsonDocument document;
  document["has_value"] = aggregateHasValue;
  document["held"] = aggregateHeld;
  document["result_id"] = aggregateResultID;

  if (aggregateHasValue)
  {
    document["overall_dba"] = aggregateDBA;
    document["aggregate_age_ms"] = now - aggregateUpdatedAtMs;
  }
  else
  {
    document["overall_dba"] = nullptr;
    document["aggregate_age_ms"] = nullptr;
  }

  document["aggregation"] = "equal_energy_mean";
  document["aggregation_interval_ms"] = AGGREGATION_INTERVAL_MS;
  document["aggregate_node_count"] = aggregateNodeCount;
  document["active_node_count"] = activeNodeCount;
  document["configured_node_count"] = CONFIGURED_NODE_COUNT;
  document["connected_stations"] = WiFi.softAPgetStationNum();
  document["uptime_ms"] = now;
  JsonArray nodeArray = document["nodes"].to<JsonArray>();

  for (size_t i = 0; i < CONFIGURED_NODE_COUNT; i++)
  {
    JsonObject item = nodeArray.add<JsonObject>();
    item["id"] = nodes[i].id;
    item["name"] = nodes[i].name;
    item["online"] = nodes[i].online;
    item["has_reading"] = nodes[i].hasReading;

    if (nodes[i].hasReading)
    {
      item["dba"] = nodes[i].dba;
      item["age_ms"] = now - nodes[i].receivedAtMs;
      item["sequence"] = nodes[i].sequence;
      item["boot_id"] = nodes[i].bootID;
      if (nodes[i].wifiRSSIDBm != 0)
      {
        item["wifi_rssi_dbm"] = nodes[i].wifiRSSIDBm;
      }
      else
      {
        item["wifi_rssi_dbm"] = nullptr;
      }
      if (nodes[i].contributedLastWindow)
      {
        item["window_dba"] = nodes[i].lastWindowDBA;
      }
      else
      {
        item["window_dba"] = nullptr;
      }

      item["contributed"] = nodes[i].contributedLastWindow;
    }
    else
    {
      item["dba"] = nullptr;
      item["age_ms"] = nullptr;
      item["sequence"] = 0;
      item["boot_id"] = 0;
      item["wifi_rssi_dbm"] = nullptr;
      item["window_dba"] = nullptr;
      item["contributed"] = false;
    }
  }

  String response;
  response.reserve(1800);
  serializeJson(document, response);
  xSemaphoreGive(stateMutex);

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", response);
}

void sendFile(const char* path, const char* contentType)
{
  File file = LittleFS.open(path, "r");

  if (!file)
  {
    server.send(404, "text/plain", "File not found in LittleFS");
    return;
  }

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.streamFile(file, contentType);
  file.close();
}

void setupWebServer()
{
  if (!LittleFS.begin(true))
  {
    Serial.println("[FATAL] LittleFS mount failed.");
    while (true)
    {
      delay(1000);
    }
  }

  if (!LittleFS.exists("/index.html"))
  {
    Serial.println("[LittleFS] WARNING: upload master_node/data.");
  }

  const char* headerKeys[] = {"X-API-Key"};
  server.collectHeaders(headerKeys, 1);
  server.on("/", HTTP_GET, []() { sendFile("/index.html", "text/html"); });
  server.on("/map-config.json", HTTP_GET, []() { sendFile("/map-config.json", "application/json"); });
  server.on("/logo.png", HTTP_GET, []() { sendFile("/logo.png", "image/png"); });
  server.on("/api/v1/readings", HTTP_POST, handleReadingPost);
  server.on("/api/v1/status", HTTP_GET, sendStatus);
  server.on("/data", HTTP_GET, sendStatus);
  server.on("/favicon.ico", HTTP_GET, []() {
    server.send(204, "text/plain", "");
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println("[WEB] Server started.");
}

void setupAccessPoint()
{
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET))
  {
    Serial.println("[FATAL] Access-point IP configuration failed.");
    while (true)
    {
      delay(1000);
    }
  }

  if (!WiFi.softAP(
        AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS))
  {
    Serial.println("[FATAL] Access point failed to start.");
    while (true)
    {
      delay(1000);
    }
  }

  const esp_err_t protocolResult =
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_BGN_PROTOCOLS);

  if (protocolResult != ESP_OK)
  {
    Serial.printf(
      "[FATAL] Could not enable B/G/N on AP: %s\n",
      esp_err_to_name(protocolResult)
    );
    while (true)
    {
      delay(1000);
    }
  }

  Serial.println();
  Serial.println("==========================================");
  Serial.println("SPD NOISE MONITOR MASTER READY");
  Serial.printf("Network   : %s\n", AP_SSID);
  Serial.printf("Radio     : B/G/N on channel %u\n", AP_CHANNEL);
  Serial.print("Dashboard : http://");
  Serial.println(WiFi.softAPIP());
  Serial.printf("Nodes     : %s, %s\n", NODE_1_ID, NODE_2_ID);
  Serial.println("==========================================");
}

void webServerTask(void* parameter)
{
  uint32_t nextFreshnessCheck = 0;
  uint32_t nextAggregateAt = millis() + AGGREGATION_INTERVAL_MS;

  while (true)
  {
    server.handleClient();
    const uint32_t now = millis();

    if ((int32_t)(now - nextFreshnessCheck) >= 0)
    {
      refreshFreshness(now);
      nextFreshnessCheck = now + 100;
    }

    if ((int32_t)(now - nextAggregateAt) >= 0)
    {
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
        publishAggregateWindowLocked(now);
        xSemaphoreGive(stateMutex);
      }

      // Preserve the master's fixed cadence without producing catch-up
      // aggregates from empty windows after an unusually long delay.
      nextAggregateAt += AGGREGATION_INTERVAL_MS;

      if ((int32_t)(now - nextAggregateAt) >= 0)
      {
        nextAggregateAt = now + AGGREGATION_INTERVAL_MS;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// =====================================================
// ARDUINO ENTRY POINTS
// =====================================================

void setup()
{
  Serial.begin(115200);
  delay(1200);

  stateMutex = xSemaphoreCreateMutex();

  if (stateMutex == nullptr)
  {
    Serial.println("[FATAL] Could not create state mutex.");
    while (true)
    {
      delay(1000);
    }
  }

  setupAccessPoint();
  setupWebServer();

  xTaskCreatePinnedToCore(
    webServerTask, "WebServerTask", 12288, nullptr, 1, nullptr, 0
  );
}

void loop()
{
  delay(1000);
}
