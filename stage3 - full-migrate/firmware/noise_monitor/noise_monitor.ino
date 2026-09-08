#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <time.h>

#include "config.h"
#include "dsp.h"

#define I2S_BCLK 18
#define I2S_LRCLK 16
#define I2S_DIN 36

const char *firebaseURL = FIREBASE_URL;

constexpr uint32_t SAMPLE_RATE = 48000;
constexpr size_t WINDOW_SAMPLES = 4800;
constexpr uint32_t WINDOW_MS = 100;
constexpr uint32_t UPLOAD_INTERVAL_MS = 200;
constexpr char FIRMWARE_VERSION[] = "2.0.0";

struct Measurement {
  DspResult dsp;
  uint32_t sequence;
  uint64_t measuredAt;
  uint32_t uptimeMs;
  uint32_t captureErrors;
  uint32_t droppedSamples;
  uint16_t blockSize;
  bool clockSynced;
};

static int32_t pcmWindow[WINDOW_SAMPLES];
static QueueHandle_t latestQueue;
static String bootId;
static String idToken;
static String refreshToken;
static uint32_t tokenExpiresAt = 0;

uint64_t unixMilliseconds() {
  struct timeval now;
  gettimeofday(&now, nullptr);
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL + now.tv_usec / 1000ULL;
}

bool clockIsSynced() { return time(nullptr) > 1700000000; }

String jsonString(const String &value) {
  String escaped = "\"";
  for (char c : value) {
    if (c == '\"' || c == '\\') escaped += '\\';
    escaped += c;
  }
  return escaped + "\"";
}

String jsonNumber(float value, uint8_t decimals = 6) {
  return isfinite(value) ? String(value, static_cast<unsigned int>(decimals)) : "null";
}

String formValue(const String &value) {
  String encoded;
  const char hex[] = "0123456789ABCDEF";
  for (uint8_t c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') encoded += static_cast<char>(c);
    else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
  }
  return encoded;
}

String jsonField(const String &body, const char *name) {
  const String needle = String("\"") + name + "\":\"";
  int start = body.indexOf(needle);
  if (start < 0) return "";
  start += needle.length();
  int end = body.indexOf('"', start);
  return end < 0 ? "" : body.substring(start, end);
}

bool authenticateFirebase() {
  if (idToken.length() && millis() < tokenExpiresAt) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure(); // Replace with setCACert() for managed production deployments.
  HTTPClient http;
  String url, body, contentType;
  if (refreshToken.length()) {
    url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;
    body = "grant_type=refresh_token&refresh_token=" + formValue(refreshToken);
    contentType = "application/x-www-form-urlencoded";
  } else {
    url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY;
    body = String("{\"email\":") + jsonString(FIREBASE_EMAIL) + ",\"password\":" + jsonString(FIREBASE_PASSWORD) + ",\"returnSecureToken\":true}";
    contentType = "application/json";
  }
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", contentType);
  int status = http.POST(body);
  String response = http.getString();
  http.end();
  if (status < 200 || status >= 300) {
    Serial.printf("Firebase authentication failed: HTTP %d\n", status);
    idToken = ""; refreshToken = "";
    return false;
  }
  idToken = jsonField(response, refreshToken.length() ? "id_token" : "idToken");
  String nextRefresh = jsonField(response, refreshToken.length() ? "refresh_token" : "refreshToken");
  if (nextRefresh.length()) refreshToken = nextRefresh;
  tokenExpiresAt = millis() + 55UL * 60UL * 1000UL;
  return idToken.length() > 0;
}

String serializeMeasurement(const Measurement &m) {
  String json;
  json.reserve(5500);
  json += "{\"version\":2,\"deviceId\":" + jsonString(DEVICE_ID);
  json += ",\"bootId\":" + jsonString(bootId) + ",\"firmwareVersion\":\"" + FIRMWARE_VERSION + "\"";
  json += ",\"sequence\":" + String(m.sequence) + ",\"measuredAt\":" + String(static_cast<uint64_t>(m.measuredAt));
  json += ",\"uploadedAt\":{\".sv\":\"timestamp\"},\"uptimeMs\":" + String(m.uptimeMs) + ",\"windowMs\":" + String(WINDOW_MS);
  json += ",\"values\":{\"dbfs\":" + jsonNumber(m.dsp.dbfs) + ",\"aWeightedDbfs\":" + jsonNumber(m.dsp.aWeightedDbfs);
#if CALIBRATION_ENABLED
  json += ",\"calibrationConstant\":" + String(CALIBRATION_CONSTANT_DB, 3);
  json += ",\"dbSpl\":" + jsonNumber(m.dsp.dbfs + CALIBRATION_CONSTANT_DB);
  json += ",\"dbSplA\":" + jsonNumber(m.dsp.aWeightedDbfs + CALIBRATION_CONSTANT_DB) + "}";
#else
  json += ",\"calibrationConstant\":null,\"dbSpl\":null,\"dbSplA\":null}";
#endif
  json += ",\"status\":{\"capture\":\"running\",\"clockSynced\":" + String(m.clockSynced ? "true" : "false");
  json += ",\"sampleRate\":" + String(SAMPLE_RATE) + ",\"clippingFraction\":" + jsonNumber(m.dsp.clippingFraction) + "}";
  json += ",\"diagnostics\":{\"rms\":" + jsonNumber(m.dsp.rms) + ",\"peak\":" + jsonNumber(m.dsp.peak);
  json += ",\"mean\":" + jsonNumber(m.dsp.mean) + ",\"latestSample\":" + jsonNumber(m.dsp.latestSample);
  json += ",\"blockSize\":" + String(m.blockSize) + ",\"droppedSamples\":" + String(m.droppedSamples) + ",\"captureErrors\":" + String(m.captureErrors) + "}";
  json += ",\"spectrum\":{\"fftSize\":4096,\"maxHz\":24000,\"measuredAt\":" + String(static_cast<uint64_t>(m.measuredAt));
  json += ",\"dominantHz\":" + jsonNumber(m.dsp.dominantHz, 2) + ",\"bands\":[";
  for (size_t i = 0; i < DSP_BAND_COUNT; ++i) { if (i) json += ','; json += String(m.dsp.bands[i]); }
  json += "]},\"waveform\":[";
  for (size_t i = 0; i < DSP_WAVEFORM_SIZE; ++i) { if (i) json += ','; json += String(m.dsp.waveform[i] / 32767.0f, 5); }
  json += "]}";
  return json;
}

void captureTask(void *) {
  uint32_t sequence = 0, captureErrors = 0, droppedSamples = 0;
  for (;;) {
    size_t offset = 0, lastBlock = 0;
    while (offset < WINDOW_SAMPLES) {
      size_t bytesRead = 0;
      size_t wanted = min(static_cast<size_t>(256), WINDOW_SAMPLES - offset);
      esp_err_t error = i2s_read(I2S_NUM_0, pcmWindow + offset, wanted * sizeof(int32_t), &bytesRead, pdMS_TO_TICKS(250));
      if (error != ESP_OK || bytesRead == 0) { ++captureErrors; continue; }
      lastBlock = bytesRead / sizeof(int32_t);
      offset += lastBlock;
    }
    Measurement m{};
    m.sequence = sequence++; m.uptimeMs = millis(); m.clockSynced = clockIsSynced();
    m.measuredAt = m.clockSynced ? unixMilliseconds() : m.uptimeMs;
    m.captureErrors = captureErrors; m.droppedSamples = droppedSamples; m.blockSize = lastBlock;
    if (processPcm(pcmWindow, WINDOW_SAMPLES, SAMPLE_RATE, m.dsp)) {
      if (uxQueueMessagesWaiting(latestQueue) > 0) ++droppedSamples;
      m.droppedSamples = droppedSamples;
      xQueueOverwrite(latestQueue, &m);
    }
    else ++captureErrors;
  }
}

void uploadTask(void *) {
  Measurement m{};
  for (;;) {
    if (xQueueReceive(latestQueue, &m, portMAX_DELAY) != pdTRUE) continue;
    vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
    while (xQueueReceive(latestQueue, &m, 0) == pdTRUE) {}
    if (!authenticateFirebase()) continue;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = String(firebaseURL) + "?auth=" + formValue(idToken);
    if (!http.begin(client, url)) continue;
    http.addHeader("Content-Type", "application/json");
    int status = http.PUT(serializeMeasurement(m));
    http.end();
    if (status == 401) { idToken = ""; tokenExpiresAt = 0; }
    if (status < 200 || status >= 300) Serial.printf("Firebase upload failed: HTTP %d\n", status);
  }
}

void setup() {
  Serial.begin(115200);
  uint64_t chip = ESP.getEfuseMac();
  bootId = String(static_cast<uint32_t>(chip >> 32), HEX) + String(static_cast<uint32_t>(chip), HEX) + "-" + String(esp_random(), HEX);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = MICROPHONE_RIGHT_CHANNEL ? I2S_CHANNEL_FMT_ONLY_RIGHT : I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8; config.dma_buf_len = 512;
  config.use_apll = false; config.tx_desc_auto_clear = false; config.fixed_mclk = 0;
  i2s_pin_config_t pins = {I2S_BCLK, I2S_LRCLK, I2S_PIN_NO_CHANGE, I2S_DIN};
  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &config, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));

  latestQueue = xQueueCreate(1, sizeof(Measurement));
  xTaskCreatePinnedToCore(captureTask, "capture-dsp", 8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(uploadTask, "firebase-upload", 12288, nullptr, 1, nullptr, 1);
  Serial.println("\nNoise monitor running");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  delay(1000);
}
