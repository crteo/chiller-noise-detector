#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <RTClib.h>
#include <WiFi.h>
#include <arduinoFFT.h>
#include <driver/i2s.h>
#include <esp_sleep.h>
#include <math.h>
#include "config.h"

static_assert(AP_CHANNEL == 11, "This bench test must use Wi-Fi channel 11");

constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr uint32_t SAMPLE_RATE_HZ = 32000;
constexpr size_t I2S_CHUNK_SAMPLES = 512;
constexpr uint16_t FFT_SIZE = 4096;
constexpr float PCM_FULL_SCALE = 2147483648.0f;
constexpr char FIRMWARE_VERSION[] = "battery-bench-dba-only-v2";

static_assert(
    CAPTURE_DURATION_MS >= (FFT_SIZE * 1000UL) / SAMPLE_RATE_HZ,
    "Capture duration must contain at least one complete FFT frame");

float fftReal[FFT_SIZE];
float fftImag[FFT_SIZE];
float hannWindow[FFT_SIZE];
float aWeightPowerGain[FFT_SIZE / 2 + 1];
float windowEnergyCorrection = 1.0f;
ArduinoFFT<float> fft(fftReal, fftImag, FFT_SIZE, SAMPLE_RATE_HZ);

RTC_DS3231 rtc;
RTC_DATA_ATTR uint32_t sequenceNumber = 0;

struct Measurement {
  uint32_t sampleCount;
  uint32_t integrationMs;
  uint32_t fftFrames;
  double aWeightedDBFS;
  double estimatedDBA;
  double peakDBFS;
  double dominantFrequencyHz;
};

float calculateAWeightingDB(float frequency) {
  if (frequency <= 0.0f) return -200.0f;
  constexpr float c1 = 20.6f;
  constexpr float c2 = 107.7f;
  constexpr float c3 = 737.9f;
  constexpr float c4 = 12194.0f;
  const float f2 = frequency * frequency;
  const float numerator = (c4 * c4) * f2 * f2;
  const float denominator =
      (f2 + c1 * c1) * sqrtf((f2 + c2 * c2) * (f2 + c3 * c3)) *
      (f2 + c4 * c4);
  const float response = numerator / denominator;
  return response > 0.0f ? 20.0f * log10f(response) + 2.0f : -200.0f;
}

void initializeDSP() {
  float windowSquareSum = 0.0f;
  for (uint16_t index = 0; index < FFT_SIZE; ++index) {
    const float window =
        0.5f * (1.0f - cosf(2.0f * PI * index / (FFT_SIZE - 1)));
    hannWindow[index] = window;
    windowSquareSum += window * window;
  }
  windowEnergyCorrection = windowSquareSum / FFT_SIZE;
  aWeightPowerGain[0] = 0.0f;
  for (uint16_t bin = 1; bin <= FFT_SIZE / 2; ++bin) {
    const float frequency = static_cast<float>(bin) * SAMPLE_RATE_HZ / FFT_SIZE;
    aWeightPowerGain[bin] =
        powf(10.0f, calculateAWeightingDB(frequency) / 10.0f);
  }
}

void processFFTFrame(double& weightedMeanSquare, double& dominantFrequencySum,
                     uint32_t& frameCount) {
  float mean = 0.0f;
  for (uint16_t index = 0; index < FFT_SIZE; ++index) mean += fftReal[index];
  mean /= FFT_SIZE;
  for (uint16_t index = 0; index < FFT_SIZE; ++index) {
    fftReal[index] = (fftReal[index] - mean) * hannWindow[index];
    fftImag[index] = 0.0f;
  }

  fft.compute(FFTDirection::Forward);
  double weightedEnergy = 0.0;
  float maximumBinEnergy = 0.0f;
  uint16_t maximumBin = 0;
  for (uint16_t bin = 0; bin <= FFT_SIZE / 2; ++bin) {
    const float re = fftReal[bin];
    const float im = fftImag[bin];
    float binEnergy = re * re + im * im;
    if (bin > 0 && bin < FFT_SIZE / 2) binEnergy *= 2.0f;
    if (bin > 0) {
      weightedEnergy += binEnergy * aWeightPowerGain[bin];
      if (bin < FFT_SIZE / 2 && binEnergy > maximumBinEnergy) {
        maximumBinEnergy = binEnergy;
        maximumBin = bin;
      }
    }
  }

  const double normalization =
      static_cast<double>(FFT_SIZE) * FFT_SIZE * windowEnergyCorrection;
  weightedMeanSquare += max(0.0, weightedEnergy / normalization);
  dominantFrequencySum +=
      static_cast<double>(maximumBin) * SAMPLE_RATE_HZ / FFT_SIZE;
  ++frameCount;
}

bool initializeRtc() {
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  if (!rtc.begin()) return false;
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  rtc.disable32K();
  rtc.writeSqwPinMode(DS3231_OFF);
  rtc.disableAlarm(2);
  rtc.clearAlarm(1);
  rtc.clearAlarm(2);
  return true;
}

bool initializeI2s() {
  const i2s_config_t config = {
    .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE_HZ,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
  };
  const i2s_pin_config_t pins = {
    .bck_io_num = SENSOR_I2S_BCLK,
    .ws_io_num = SENSOR_I2S_LRCLK,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = SENSOR_I2S_DIN,
  };
  esp_err_t result = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
  if (result == ESP_OK) result = i2s_set_pin(I2S_PORT, &pins);
  if (result == ESP_OK) result = i2s_zero_dma_buffer(I2S_PORT);
  return result == ESP_OK;
}

Measurement captureMeasurement() {
  Measurement output{};
  const uint32_t wantedSamples =
      SAMPLE_RATE_HZ * CAPTURE_DURATION_MS / 1000UL;
  int32_t samples[I2S_CHUNK_SAMPLES];
  size_t bytesRead = 0;

  // Discard one block so the microphone and I2S data path can settle.
  i2s_read(I2S_PORT, samples, sizeof(samples), &bytesRead, pdMS_TO_TICKS(1000));

  double minimum = 1.0;
  double maximum = -1.0;
  double weightedMeanSquare = 0.0;
  double dominantFrequencySum = 0.0;
  uint32_t frameCount = 0;
  uint16_t fftIndex = 0;
  uint32_t received = 0;
  while (received < wantedSamples) {
    const size_t count = min(
        static_cast<size_t>(wantedSamples - received), I2S_CHUNK_SAMPLES);
    bytesRead = 0;
    if (i2s_read(I2S_PORT, samples, count * sizeof(int32_t), &bytesRead,
                 pdMS_TO_TICKS(1000)) != ESP_OK || bytesRead == 0) {
      break;
    }
    const size_t sampleCount = bytesRead / sizeof(int32_t);
    for (size_t index = 0; index < sampleCount; ++index) {
      const float value = static_cast<float>(samples[index]) / PCM_FULL_SCALE;
      if (value < minimum) minimum = value;
      if (value > maximum) maximum = value;
      fftReal[fftIndex++] = value;
      if (fftIndex == FFT_SIZE) {
        processFFTFrame(weightedMeanSquare, dominantFrequencySum, frameCount);
        fftIndex = 0;
      }
    }
    received += sampleCount;
  }

  output.sampleCount = received;
  output.fftFrames = frameCount;
  output.integrationMs = static_cast<uint32_t>(
      static_cast<uint64_t>(frameCount) * FFT_SIZE * 1000ULL / SAMPLE_RATE_HZ);
  if (received == 0 || frameCount == 0) {
    output.aWeightedDBFS = output.estimatedDBA = output.peakDBFS = -200.0;
    return output;
  }
  const double rmsA = sqrt(max(0.0, weightedMeanSquare / frameCount));
  const double peak = max(0.0, (maximum - minimum) / 2.0);
  output.aWeightedDBFS = rmsA > 1e-12 ? 20.0 * log10(rmsA) : -200.0;
  output.peakDBFS = peak > 1e-12 ? 20.0 * log10(peak) : -200.0;
  output.estimatedDBA = output.aWeightedDBFS + SENSOR_CALIBRATION_DB;
  output.dominantFrequencyHz = dominantFrequencySum / frameCount;
  return output;
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  // Supplying the fixed AP channel avoids a full channel scan on every wake.
  WiFi.begin(AP_SSID, AP_PASSWORD, AP_CHANNEL);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool postMeasurement(const Measurement& measurement, uint32_t rtcUnix,
                     uint32_t awakeMs) {
  if (WiFi.status() != WL_CONNECTED) return false;
  JsonDocument document;
  document["node_id"] = NODE_ID;
  document["power_source"] = POWER_SOURCE_LABEL;
  document["firmware_version"] = FIRMWARE_VERSION;
  document["sequence"] = sequenceNumber;
  document["rtc_unix"] = rtcUnix;
  document["a_weighted_dbfs"] = measurement.aWeightedDBFS;
  document["estimated_dba"] = measurement.estimatedDBA;
  document["peak_dbfs"] = measurement.peakDBFS;
  document["sample_count"] = measurement.sampleCount;
  document["integration_ms"] = measurement.integrationMs;
  document["fft_frames"] = measurement.fftFrames;
  document["dominant_frequency_hz"] = measurement.dominantFrequencyHz;
  document["capture_ms"] = CAPTURE_DURATION_MS;
  document["awake_ms"] = awakeMs;
  document["wifi_rssi_dbm"] = WiFi.RSSI();

  String body;
  body.reserve(420);
  serializeJson(document, body);
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  const String url = String("http://") + MASTER_HOST + "/measurement";
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  const int status = http.POST(body);
  http.end();
  return status == 202;
}

bool scheduleNextAlarm() {
  const DateTime now = rtc.now();
  const uint32_t interval = REPORT_INTERVAL_SECONDS;
  const uint32_t nextUnix = ((now.unixtime() / interval) + 1) * interval;
  rtc.clearAlarm(1);
  return rtc.setAlarm1(DateTime(nextUnix), DS3231_A1_Date);
}

void enterDeepSleep() {
  if (!scheduleNextAlarm()) {
    Serial.println("[FATAL] Failed to program DS3231 Alarm 1.");
    delay(1000);
    ESP.restart();
  }
  pinMode(RTC_INTERRUPT_PIN, INPUT_PULLUP);
  if (digitalRead(RTC_INTERRUPT_PIN) == LOW) {
    Serial.println("[FATAL] RTC interrupt is still LOW; check alarm flags/pull-up.");
    delay(1000);
    ESP.restart();
  }
  esp_sleep_enable_ext0_wakeup(
      static_cast<gpio_num_t>(RTC_INTERRUPT_PIN), 0);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  const uint32_t awakeStarted = millis();
  Serial.begin(115200);
  delay(100);
  ++sequenceNumber;

  if (!initializeRtc()) {
    Serial.println("[FATAL] DS3231 not found.");
    delay(1000);
    ESP.restart();
  }
  if (!initializeI2s()) {
    Serial.println("[FATAL] I2S initialization failed.");
    delay(1000);
    ESP.restart();
  }

  initializeDSP();

  const Measurement measurement = captureMeasurement();
  i2s_driver_uninstall(I2S_PORT);
  const uint32_t rtcUnix = rtc.now().unixtime();

  bool sent = false;
  if (connectWifi()) {
    sent = postMeasurement(measurement, rtcUnix, millis() - awakeStarted);
  }
  Serial.printf(
      "[RESULT] sequence=%lu weighted=%.2f dBFS estimated=%.2f dBA "
      "peak=%.2f samples=%lu integration_ms=%lu frames=%lu dominant=%.1f Hz "
      "sent=%s awake_ms=%lu\n",
      static_cast<unsigned long>(sequenceNumber), measurement.aWeightedDBFS,
      measurement.estimatedDBA, measurement.peakDBFS,
      static_cast<unsigned long>(measurement.sampleCount),
      static_cast<unsigned long>(measurement.integrationMs),
      static_cast<unsigned long>(measurement.fftFrames),
      measurement.dominantFrequencyHz,
      sent ? "yes" : "no",
      static_cast<unsigned long>(millis() - awakeStarted));

  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  enterDeepSleep();
}

void loop() {}
