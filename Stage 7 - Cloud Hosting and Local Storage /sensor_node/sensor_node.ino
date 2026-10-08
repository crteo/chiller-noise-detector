#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <arduinoFFT.h>
#include <driver/i2s.h>
#include <esp32-hal-cpu.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <time.h>
constexpr char FIRMWARE_VERSION[] = "stage7-acoustic-v1.0.0";
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr uint32_t SAMPLE_RATE = 32000;
constexpr uint16_t FFT_SIZE = 4096;
constexpr uint16_t SPECTRUM_BINS = FFT_SIZE / 2 + 1;
constexpr uint32_t MAX_CAPTURE_MS = 15000;
constexpr size_t MAX_SAMPLES = (SAMPLE_RATE * MAX_CAPTURE_MS) / 1000;
constexpr size_t MAX_AUDIO_BYTES = MAX_SAMPLES * sizeof(int32_t);
constexpr size_t I2S_CHUNK_SAMPLES = 512;
constexpr uint32_t SCHEDULE_POLL_MS = 2000;
constexpr uint32_t WIFI_RETRY_MS = 5000;
constexpr uint32_t CPU_FREQUENCY_MHZ = 160;
constexpr size_t BAND_COUNT = 4;
constexpr size_t PEAK_COUNT = 3;
constexpr float MIN_ANALYSIS_HZ = 60.0f;
constexpr float MAX_ANALYSIS_HZ = 15000.0f;

struct SensorConfig {
  String nodeID, nodeName, role, apSSID, apPassword, masterHost;
  String collectorHost, apiKey;
  uint16_t collectorPort;
  bool rawUploadEnabled;
  double calibrationDB;
  int i2sBCLK, i2sLRCLK, i2sDIN;
};

SensorConfig config;

[[noreturn]] void fatal(const String& message) {
  Serial.println("[FATAL] " + message);
  while (true) delay(1000);
}

String requiredString(JsonVariantConst value, const char* name) {
  const char* text = value | "";
  if (!text[0]) fatal(String("Missing config field: ") + name);
  return String(text);
}

void loadConfig() {
  if (!LittleFS.begin(false)) fatal("LittleFS mount failed; upload data/config.json");
  File file = LittleFS.open("/config.json", "r");
  if (!file) fatal("Missing /config.json");
  JsonDocument doc;
  if (deserializeJson(doc, file)) fatal("Invalid /config.json");
  config.nodeID = requiredString(doc["device"]["id"], "device.id");
  config.nodeName = requiredString(doc["device"]["name"], "device.name");
  config.role = requiredString(doc["device"]["role"], "device.role");
  config.apSSID = requiredString(doc["local_network"]["ssid"], "local_network.ssid");
  config.apPassword = requiredString(doc["local_network"]["password"], "local_network.password");
  config.masterHost = doc["local_network"]["master_host"] | "192.168.4.1";
  config.collectorHost = requiredString(doc["collector"]["host"], "collector.host");
  config.collectorPort = doc["collector"]["port"] | 8080;
  config.rawUploadEnabled = doc["collector"]["raw_upload_enabled"] | true;
  config.apiKey = requiredString(doc["local_api"]["shared_key"], "local_api.shared_key");
  config.calibrationDB = doc["acoustics"]["calibration_offset_db"] | 0.0;
  config.i2sBCLK = doc["pins"]["i2s_bclk"] | 4;
  config.i2sLRCLK = doc["pins"]["i2s_lrclk"] | 5;
  config.i2sDIN = doc["pins"]["i2s_din"] | 6;
  if (config.apPassword.length() < 8) fatal("local_network.password must have at least 8 characters");
  if (config.role != "near" && config.role != "reference") fatal("device.role must be near or reference");
}

const float BAND_LOW[BAND_COUNT] = {60.0f, 200.0f, 1000.0f, 5000.0f};
const float BAND_HIGH[BAND_COUNT] = {200.0f, 1000.0f, 5000.0f, 15000.0f};

int32_t* rawSamples = nullptr;
float fftReal[FFT_SIZE];
float fftImag[FFT_SIZE];
float hannWindow[FFT_SIZE];
float aWeightPowerGain[SPECTRUM_BINS];
float spectrumPower[SPECTRUM_BINS];
float windowEnergyCorrection = 1.0f;
ArduinoFFT<float> FFT(fftReal, fftImag, FFT_SIZE, SAMPLE_RATE);

struct CaptureSchedule {
  bool valid;
  uint32_t sessionID;
  uint32_t captureID;
  uint32_t localStartMs;
  uint32_t durationMs;
  uint64_t unixStartMs;
};

struct AcousticFeatures {
  uint32_t sampleCount;
  uint32_t integrationMs;
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

CaptureSchedule pending = {false, 0, 0, 0, 0, 0};
uint32_t lastCompletedCaptureID = 0;
uint32_t currentSessionID = 0;
uint32_t nextSchedulePollAt = 0;
uint32_t nextWiFiRetryAt = 0;

float aWeightingDB(float frequency) {
  if (frequency <= 0.0f) return -200.0f;
  constexpr float c1 = 20.6f, c2 = 107.7f, c3 = 737.9f, c4 = 12194.0f;
  const float f2 = frequency * frequency;
  const float numerator = (c4 * c4) * f2 * f2;
  const float denominator = (f2 + c1 * c1) *
    sqrtf((f2 + c2 * c2) * (f2 + c3 * c3)) * (f2 + c4 * c4);
  const float response = numerator / denominator;
  return response > 0.0f ? 20.0f * log10f(response) + 2.0f : -200.0f;
}

void setupDSP() {
  float squareSum = 0.0f;
  for (uint16_t i = 0; i < FFT_SIZE; ++i) {
    hannWindow[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SIZE - 1)));
    squareSum += hannWindow[i] * hannWindow[i];
  }
  windowEnergyCorrection = squareSum / FFT_SIZE;
  aWeightPowerGain[0] = 0.0f;
  for (uint16_t bin = 1; bin < SPECTRUM_BINS; ++bin) {
    const float hz = static_cast<float>(bin) * SAMPLE_RATE / FFT_SIZE;
    aWeightPowerGain[bin] = powf(10.0f, aWeightingDB(hz) / 10.0f);
  }
}

void setupI2S() {
  const i2s_config_t config = {
    .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 16,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  const i2s_pin_config_t pins = {
    .bck_io_num = config.i2sBCLK,
    .ws_io_num = config.i2sLRCLK,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = config.i2sDIN
  };
  esp_err_t result = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
  if (result == ESP_OK) result = i2s_set_pin(I2S_PORT, &pins);
  if (result == ESP_OK) result = i2s_zero_dma_buffer(I2S_PORT);
  if (result == ESP_OK) result = i2s_stop(I2S_PORT);
  if (result != ESP_OK) {
    Serial.printf("[FATAL] I2S setup failed: %d\n", result);
    while (true) delay(1000);
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WIFI] Connecting to %s\n", config.apSSID.c_str());
  WiFi.disconnect(false);
  WiFi.begin(config.apSSID.c_str(), config.apPassword.c_str());
  nextWiFiRetryAt = millis() + WIFI_RETRY_MS;
}

bool fetchSchedule() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  const String url = String("http://") + config.masterHost + "/api/v1/schedule";
  const uint32_t requestStarted = millis();
  http.setConnectTimeout(800);
  http.setTimeout(1500);
  if (!http.begin(url)) return false;
  const int status = http.GET();
  const uint32_t requestFinished = millis();
  if (status != 200) {
    Serial.printf("[SCHEDULE] HTTP %d\n", status);
    http.end();
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, http.getString());
  http.end();
  if (error || !(doc["clock_set"] | false)) return false;

  const uint32_t sessionID = doc["session_id"] | 0UL;
  const uint32_t captureID = doc["next_capture_id"] | 0UL;
  const uint32_t masterNow = doc["master_uptime_ms"] | 0UL;
  const uint32_t masterStart = doc["next_capture_start_ms"] | 0UL;
  const uint32_t durationMs = doc["capture_duration_ms"] | 0UL;
  const uint64_t unixStartMs = doc["next_capture_unix_ms"] | 0ULL;
  if (sessionID == 0 || captureID == 0 || unixStartMs == 0 ||
      durationMs == 0 || durationMs > MAX_CAPTURE_MS) return false;

  if (currentSessionID != sessionID) {
    currentSessionID = sessionID;
    lastCompletedCaptureID = 0;
    pending.valid = false;
    Serial.printf("[SCHEDULE] New master session %lu\n", (unsigned long)sessionID);
  }
  if (captureID <= lastCompletedCaptureID) return true;

  const uint32_t localMidpoint = requestStarted + (requestFinished - requestStarted) / 2;
  pending.valid = true;
  pending.sessionID = sessionID;
  pending.captureID = captureID;
  pending.localStartMs = localMidpoint + static_cast<uint32_t>(masterStart - masterNow);
  pending.durationMs = durationMs;
  pending.unixStartMs = unixStartMs;
  return true;
}

bool captureAudio(uint32_t sampleCount) {
  if (sampleCount > MAX_SAMPLES) return false;
  esp_err_t result = i2s_start(I2S_PORT);
  if (result != ESP_OK) return false;

  int32_t settling[I2S_CHUNK_SAMPLES];
  size_t bytesRead = 0;
  result = i2s_read(I2S_PORT, settling, sizeof(settling), &bytesRead, portMAX_DELAY);
  if (result != ESP_OK) {
    i2s_stop(I2S_PORT);
    return false;
  }

  size_t received = 0;
  while (received < sampleCount) {
    const size_t wanted = min(I2S_CHUNK_SAMPLES, sampleCount - received);
    bytesRead = 0;
    result = i2s_read(I2S_PORT, rawSamples + received,
                      wanted * sizeof(int32_t), &bytesRead, portMAX_DELAY);
    if (result != ESP_OK || bytesRead == 0) {
      i2s_stop(I2S_PORT);
      return false;
    }
    received += bytesRead / sizeof(int32_t);
  }
  i2s_stop(I2S_PORT);
  return received == sampleCount;
}

double powerToDB(double power) {
  return power > 1e-20 ? 10.0 * log10(power) : -200.0;
}

AcousticFeatures calculateFeatures(uint32_t sampleCount, uint32_t durationMs) {
  AcousticFeatures out{};
  out.sampleCount = sampleCount;
  out.integrationMs = durationMs;

  double mean = 0.0;
  for (uint32_t i = 0; i < sampleCount; ++i) {
    mean += static_cast<double>(rawSamples[i]) / 2147483648.0;
  }
  mean /= sampleCount;

  double sum2 = 0.0, sum4 = 0.0, peak = 0.0;
  uint32_t clipped = 0, zeroCrossings = 0;
  double previous = static_cast<double>(rawSamples[0]) / 2147483648.0 - mean;
  for (uint32_t i = 0; i < sampleCount; ++i) {
    const double original = static_cast<double>(rawSamples[i]) / 2147483648.0;
    const double value = original - mean;
    const double square = value * value;
    sum2 += square;
    sum4 += square * square;
    peak = max(peak, fabs(value));
    if (fabs(original) >= 0.99) ++clipped;
    if (i > 0 && ((value >= 0.0) != (previous >= 0.0))) ++zeroCrossings;
    previous = value;
  }
  const double variance = sum2 / sampleCount;
  const double rms = sqrt(variance);
  out.peakDBFS = peak > 1e-10 ? 20.0 * log10(peak) : -200.0;
  out.crestFactor = rms > 1e-10 ? peak / rms : 0.0;
  out.kurtosis = variance > 1e-20 ? (sum4 / sampleCount) / (variance * variance) : 0.0;
  out.zeroCrossingRate = static_cast<double>(zeroCrossings) / max(1UL, sampleCount - 1);
  out.clippedFraction = static_cast<double>(clipped) / sampleCount;

  memset(spectrumPower, 0, sizeof(spectrumPower));
  const uint32_t frameCount = sampleCount / FFT_SIZE;
  const double normalization = static_cast<double>(FFT_SIZE) * FFT_SIZE *
    windowEnergyCorrection;
  for (uint32_t frame = 0; frame < frameCount; ++frame) {
    const uint32_t offset = frame * FFT_SIZE;
    double frameMean = 0.0;
    for (uint16_t i = 0; i < FFT_SIZE; ++i) {
      frameMean += static_cast<double>(rawSamples[offset + i]) / 2147483648.0;
    }
    frameMean /= FFT_SIZE;
    for (uint16_t i = 0; i < FFT_SIZE; ++i) {
      const float value = static_cast<float>(
        static_cast<double>(rawSamples[offset + i]) / 2147483648.0 - frameMean);
      fftReal[i] = value * hannWindow[i];
      fftImag[i] = 0.0f;
    }
    FFT.compute(FFTDirection::Forward);
    for (uint16_t bin = 1; bin < SPECTRUM_BINS; ++bin) {
      double energy = static_cast<double>(fftReal[bin]) * fftReal[bin] +
        static_cast<double>(fftImag[bin]) * fftImag[bin];
      if (bin < FFT_SIZE / 2) energy *= 2.0;
      spectrumPower[bin] += static_cast<float>(energy / normalization);
    }
  }

  double broadbandPower = 0.0, aWeightedPower = 0.0;
  double bandPower[BAND_COUNT] = {0.0, 0.0, 0.0, 0.0};
  for (uint16_t bin = 1; bin < SPECTRUM_BINS; ++bin) {
    spectrumPower[bin] = frameCount ? spectrumPower[bin] / frameCount : 0.0f;
    const double hz = static_cast<double>(bin) * SAMPLE_RATE / FFT_SIZE;
    const double power = spectrumPower[bin];
    if (hz >= MIN_ANALYSIS_HZ && hz <= MAX_ANALYSIS_HZ) broadbandPower += power;
    aWeightedPower += power * aWeightPowerGain[bin];
    for (size_t band = 0; band < BAND_COUNT; ++band) {
      const bool inBand = hz >= BAND_LOW[band] &&
        (band == BAND_COUNT - 1 ? hz <= BAND_HIGH[band] : hz < BAND_HIGH[band]);
      if (inBand) bandPower[band] += power;
    }
  }
  out.broadbandDBFS = powerToDB(broadbandPower);
  out.estimatedDBZ = out.broadbandDBFS + config.calibrationDB;
  out.estimatedDBA = powerToDB(aWeightedPower) + config.calibrationDB;
  for (size_t band = 0; band < BAND_COUNT; ++band) {
    out.bandDBFS[band] = powerToDB(bandPower[band]);
  }

  for (size_t selected = 0; selected < PEAK_COUNT; ++selected) {
    uint16_t bestBin = 0;
    float bestPower = 0.0f;
    for (uint16_t bin = 2; bin + 1 < SPECTRUM_BINS; ++bin) {
      const float hz = static_cast<float>(bin) * SAMPLE_RATE / FFT_SIZE;
      if (hz < MIN_ANALYSIS_HZ || hz > MAX_ANALYSIS_HZ ||
          spectrumPower[bin] < spectrumPower[bin - 1] ||
          spectrumPower[bin] < spectrumPower[bin + 1]) continue;
      bool separated = true;
      for (size_t prior = 0; prior < selected; ++prior) {
        if (fabs(hz - out.peakHz[prior]) < 40.0) separated = false;
      }
      if (separated && spectrumPower[bin] > bestPower) {
        bestPower = spectrumPower[bin];
        bestBin = bin;
      }
    }
    out.peakHz[selected] = static_cast<double>(bestBin) * SAMPLE_RATE / FFT_SIZE;
    out.peakSpectralDBFS[selected] = powerToDB(bestPower);
  }
  return out;
}

bool postFeatures(const CaptureSchedule& schedule, const AcousticFeatures& f) {
  if (WiFi.status() != WL_CONNECTED) return false;
  JsonDocument doc;
  doc["node_id"] = config.nodeID;
  doc["node_name"] = config.nodeName;
  doc["role"] = config.role;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["session_id"] = schedule.sessionID;
  doc["capture_id"] = schedule.captureID;
  doc["capture_start_unix_ms"] = schedule.unixStartMs;
  doc["sample_rate_hz"] = SAMPLE_RATE;
  doc["sample_count"] = f.sampleCount;
  doc["integration_ms"] = f.integrationMs;
  doc["wifi_rssi_dbm"] = WiFi.RSSI();
  doc["broadband_dbfs_unweighted"] = f.broadbandDBFS;
  doc["estimated_dbz"] = f.estimatedDBZ;
  doc["estimated_dba"] = f.estimatedDBA;
  doc["peak_dbfs"] = f.peakDBFS;
  doc["crest_factor"] = f.crestFactor;
  doc["kurtosis"] = f.kurtosis;
  doc["zero_crossing_rate"] = f.zeroCrossingRate;
  doc["clipped_fraction"] = f.clippedFraction;
  JsonArray bands = doc["band_dbfs"].to<JsonArray>();
  for (double value : f.bandDBFS) bands.add(value);
  JsonArray peaks = doc["spectral_peaks"].to<JsonArray>();
  for (size_t i = 0; i < PEAK_COUNT; ++i) {
    JsonObject peak = peaks.add<JsonObject>();
    peak["hz"] = f.peakHz[i];
    peak["dbfs"] = f.peakSpectralDBFS[i];
  }

  String payload;
  payload.reserve(1800);
  serializeJson(doc, payload);
  HTTPClient http;
  const String url = String("http://") + config.masterHost + "/api/v1/features";
  http.setConnectTimeout(1000);
  http.setTimeout(2500);
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", config.apiKey);
  const int status = http.POST(payload);
  http.end();
  Serial.printf("[FEATURE] POST result=%d\n", status);
  return status == 202;
}

void writeLE16(uint8_t* dst, uint16_t value) {
  dst[0] = value & 0xff;
  dst[1] = value >> 8;
}

void writeLE32(uint8_t* dst, uint32_t value) {
  dst[0] = value & 0xff;
  dst[1] = (value >> 8) & 0xff;
  dst[2] = (value >> 16) & 0xff;
  dst[3] = value >> 24;
}

void makeWavHeader(uint8_t* header, uint32_t dataBytes) {
  memset(header, 0, 44);
  memcpy(header, "RIFF", 4);
  writeLE32(header + 4, 36 + dataBytes);
  memcpy(header + 8, "WAVEfmt ", 8);
  writeLE32(header + 16, 16);
  writeLE16(header + 20, 1);
  writeLE16(header + 22, 1);
  writeLE32(header + 24, SAMPLE_RATE);
  writeLE32(header + 28, SAMPLE_RATE * 4);
  writeLE16(header + 32, 4);
  writeLE16(header + 34, 32);
  memcpy(header + 36, "data", 4);
  writeLE32(header + 40, dataBytes);
}

bool writeAll(WiFiClient& client, const uint8_t* data, size_t length) {
  size_t written = 0;
  uint32_t lastProgress = millis();
  while (written < length) {
    if (!client.connected()) return false;
    const size_t count = client.write(data + written, length - written);
    if (count) {
      written += count;
      lastProgress = millis();
    } else if (millis() - lastProgress > 10000) {
      return false;
    } else {
      delay(1);
    }
  }
  return true;
}

String utcCompact(uint64_t unixMs) {
  time_t seconds = static_cast<time_t>(unixMs / 1000ULL);
  struct tm utc;
  gmtime_r(&seconds, &utc);
  char text[20];
  strftime(text, sizeof(text), "%Y%m%dT%H%M%SZ", &utc);
  return String(text);
}

String utcISO(uint64_t unixMs) {
  time_t seconds = static_cast<time_t>(unixMs / 1000ULL);
  struct tm utc;
  gmtime_r(&seconds, &utc);
  char text[25];
  strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(text);
}

bool uploadWav(const CaptureSchedule& schedule, uint32_t sampleCount, const AcousticFeatures& features) {
  if (!config.rawUploadEnabled || WiFi.status() != WL_CONNECTED) return !config.rawUploadEnabled;
  WiFiClient client;
  client.setTimeout(10);
  if (!client.connect(config.collectorHost.c_str(), config.collectorPort)) {
    Serial.println("[WAV] Collector connection failed.");
    return false;
  }

  const uint32_t dataBytes = sampleCount * sizeof(int32_t);
  const String filename = config.nodeID + "_" + utcCompact(schedule.unixStartMs) +
    "_capture-" + String(schedule.captureID) + "_sr32000_s32.wav";
  client.print("POST /upload HTTP/1.1\r\n");
  client.printf("Host: %s:%u\r\n", config.collectorHost.c_str(), config.collectorPort);
  client.print("Content-Type: audio/wav\r\n");
  client.printf("Content-Length: %lu\r\n", (unsigned long)(44 + dataBytes));
  client.printf("X-API-Key: %s\r\n", config.apiKey.c_str());
  client.printf("X-Filename: %s\r\n", filename.c_str());
  client.printf("X-Device-Id: %s\r\n", config.nodeID.c_str());
  client.printf("X-Role: %s\r\n", config.role.c_str());
  client.printf("X-Session-Id: %lu\r\n", (unsigned long)schedule.sessionID);
  client.printf("X-Capture-Id: %lu\r\n", (unsigned long)schedule.captureID);
  client.printf("X-Capture-Start-Utc: %s\r\n", utcISO(schedule.unixStartMs).c_str());
  client.printf("X-Sample-Rate: %lu\r\n", (unsigned long)SAMPLE_RATE);
  client.print("X-Bits-Per-Sample: 32\r\nX-Channels: 1\r\n");
  client.printf("X-Firmware-Version: %s\r\n", FIRMWARE_VERSION);
  client.printf("X-Wifi-Rssi-Dbm: %ld\r\n", (long)WiFi.RSSI());
  client.printf("X-Broadband-Dbfs: %.4f\r\n", features.broadbandDBFS);
  client.printf("X-Estimated-Dbz: %.4f\r\n", features.estimatedDBZ);
  client.printf("X-Estimated-Dba: %.4f\r\n", features.estimatedDBA);
  client.printf("X-Crest-Factor: %.5f\r\n", features.crestFactor);
  client.printf("X-Kurtosis: %.5f\r\n", features.kurtosis);
  client.printf("X-Zero-Crossing-Rate: %.7f\r\n", features.zeroCrossingRate);
  client.printf("X-Clipped-Fraction: %.7f\r\n", features.clippedFraction);
  client.print("Connection: close\r\n\r\n");

  uint8_t header[44];
  makeWavHeader(header, dataBytes);
  bool ok = writeAll(client, header, sizeof(header));
  if (ok) ok = writeAll(client, reinterpret_cast<const uint8_t*>(rawSamples), dataBytes);
  const uint32_t responseStart = millis();
  while (ok && !client.available() && client.connected() && millis() - responseStart < 10000) delay(10);
  const String statusLine = client.readStringUntil('\n');
  client.stop();
  ok = ok && (statusLine.startsWith("HTTP/1.1 201") || statusLine.startsWith("HTTP/1.0 201"));
  Serial.printf("[WAV] upload=%s %s\n", ok ? "success" : "failed", filename.c_str());
  return ok;
}

void runCapture(const CaptureSchedule schedule) {
  const uint32_t sampleCount = (SAMPLE_RATE * schedule.durationMs) / 1000;
  Serial.printf("[CAPTURE] id=%lu samples=%lu\n",
                (unsigned long)schedule.captureID, (unsigned long)sampleCount);
  if (!captureAudio(sampleCount)) {
    Serial.println("[CAPTURE] Audio acquisition failed.");
    lastCompletedCaptureID = schedule.captureID;
    return;
  }
  const AcousticFeatures features = calculateFeatures(sampleCount, schedule.durationMs);
  Serial.printf("[FEATURE] broadband=%.2f dBFS dBZ=%.2f dBA=%.2f crest=%.2f kurtosis=%.2f\n",
                features.broadbandDBFS, features.estimatedDBZ, features.estimatedDBA,
                features.crestFactor, features.kurtosis);
  postFeatures(schedule, features);
  uploadWav(schedule, sampleCount, features);
  lastCompletedCaptureID = schedule.captureID;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  loadConfig();
  setCpuFrequencyMhz(CPU_FREQUENCY_MHZ);
  if (!psramFound()) {
    Serial.println("[FATAL] PSRAM is required for the Stage 7 raw capture buffer.");
    while (true) delay(1000);
  }
  rawSamples = static_cast<int32_t*>(heap_caps_malloc(
    MAX_AUDIO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!rawSamples) {
    Serial.println("[FATAL] Could not allocate PSRAM audio buffer.");
    while (true) delay(1000);
  }

  setupDSP();
  setupI2S();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  connectWiFi();

  Serial.println("\n========================================");
  Serial.println("CWP1-2 STAGE 7 ACOUSTIC SENSOR");
  Serial.printf("Node       : %s\n", config.nodeID.c_str());
  Serial.printf("Role       : %s\n", config.role.c_str());
  Serial.printf("PSRAM buf  : %lu bytes\n", (unsigned long)MAX_AUDIO_BYTES);
  Serial.println("========================================");
}

void loop() {
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(now - nextWiFiRetryAt) >= 0) {
    connectWiFi();
  }
  if (WiFi.status() == WL_CONNECTED && static_cast<int32_t>(now - nextSchedulePollAt) >= 0) {
    fetchSchedule();
    nextSchedulePollAt = now + SCHEDULE_POLL_MS;
  }
  if (pending.valid && pending.captureID > lastCompletedCaptureID &&
      static_cast<int32_t>(millis() - pending.localStartMs) >= 0) {
    const CaptureSchedule schedule = pending;
    pending.valid = false;
    runCapture(schedule);
    nextSchedulePollAt = millis();
  }
  delay(20);
}
