#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <arduinoFFT.h>
#include <esp_system.h>
#include <math.h>
#include "config.h"

static_assert(sizeof(AP_PASSWORD) > 8, "AP password must be at least 8 characters");
static_assert(sizeof(NODE_ID) > 1, "NODE_ID must not be empty");

// =====================================================
// AUDIO CONFIGURATION
// =====================================================

constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint16_t FFT_SIZE = 4096;
constexpr uint8_t FRAMES_PER_LEVEL = 3;
constexpr uint32_t LEVEL_SAMPLE_COUNT = FFT_SIZE * FRAMES_PER_LEVEL;
constexpr uint32_t INTEGRATION_MS =
  (LEVEL_SAMPLE_COUNT * 1000UL) / SAMPLE_RATE;
constexpr double PCM_FULL_SCALE = 8388608.0;  // 2^23

constexpr size_t I2S_BUFFER_SIZE = 512;
int32_t i2sBuffer[I2S_BUFFER_SIZE];
double vReal[FFT_SIZE];
double vImag[FFT_SIZE];
ArduinoFFT<double> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

uint16_t fftIndex = 0;
double accumulatedUnweightedMS = 0.0;
double accumulatedWeightedMS = 0.0;
double accumulatedDominantFrequency = 0.0;
uint8_t accumulatedFrames = 0;
uint32_t sequenceNumber = 0;
uint32_t bootID = 0;

struct MeasurementReport
{
  uint32_t sequence;
  uint32_t measuredAtMs;
  double estimatedDBA;
  double dbfsA;
  double dominantFrequency;
};

QueueHandle_t reportQueue = nullptr;

// =====================================================
// A-WEIGHTING
// =====================================================

double calculateAWeightingDB(double frequency)
{
  if (frequency <= 0.0)
  {
    return -200.0;
  }

  constexpr double c1 = 20.6;
  constexpr double c2 = 107.7;
  constexpr double c3 = 737.9;
  constexpr double c4 = 12194.0;

  const double f2 = frequency * frequency;
  const double numerator = (c4 * c4) * f2 * f2;
  const double denominator =
    (f2 + c1 * c1) *
    sqrt((f2 + c2 * c2) * (f2 + c3 * c3)) *
    (f2 + c4 * c4);
  const double response = numerator / denominator;

  return response > 0.0 ? 20.0 * log10(response) + 2.0 : -200.0;
}

double calculateAWeightPowerGain(double frequency)
{
  return pow(10.0, calculateAWeightingDB(frequency) / 10.0);
}

// =====================================================
// I2S AND DSP
// =====================================================

void setupI2S()
{
  const i2s_config_t i2sConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
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

  const i2s_pin_config_t pinConfig = {
    .bck_io_num = SENSOR_I2S_BCLK,
    .ws_io_num = SENSOR_I2S_LRCLK,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = SENSOR_I2S_DIN
  };

  esp_err_t result = i2s_driver_install(I2S_PORT, &i2sConfig, 0, nullptr);

  if (result == ESP_OK)
  {
    result = i2s_set_pin(I2S_PORT, &pinConfig);
  }

  if (result != ESP_OK)
  {
    Serial.printf("[I2S] Initialisation failed: %d\n", result);
    while (true)
    {
      delay(1000);
    }
  }

  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("[I2S] Initialised.");
}

void publishLevelMeasurement()
{
  if (accumulatedFrames == 0)
  {
    return;
  }

  const double averageWeightedMS =
    max(0.0, accumulatedWeightedMS / accumulatedFrames);
  const double rmsA = sqrt(averageWeightedMS);
  const double dbfsA = rmsA > 1e-12 ? 20.0 * log10(rmsA) : -120.0;
  const double estimatedDBA = dbfsA + SENSOR_CALIBRATION_DB;

  MeasurementReport report = {
    .sequence = ++sequenceNumber,
    .measuredAtMs = millis(),
    .estimatedDBA = estimatedDBA,
    .dbfsA = dbfsA,
    .dominantFrequency = accumulatedDominantFrequency / accumulatedFrames
  };

  if (isfinite(report.estimatedDBA) && report.estimatedDBA >= 0.0 &&
      report.estimatedDBA <= 160.0)
  {
    xQueueOverwrite(reportQueue, &report);
    Serial.printf(
      "[LEVEL] seq=%lu level=%.2f dBA dbfsA=%.2f dominant=%.1f Hz\n",
      (unsigned long)report.sequence,
      report.estimatedDBA,
      report.dbfsA,
      report.dominantFrequency
    );
  }
  else
  {
    Serial.printf("[LEVEL] Rejected invalid value: %.2f dBA\n", estimatedDBA);
  }

  accumulatedUnweightedMS = 0.0;
  accumulatedWeightedMS = 0.0;
  accumulatedDominantFrequency = 0.0;
  accumulatedFrames = 0;
}

void processFFTFrame()
{
  double mean = 0.0;

  for (uint16_t i = 0; i < FFT_SIZE; i++)
  {
    mean += vReal[i];
  }

  mean /= FFT_SIZE;

  double windowSquareSum = 0.0;

  for (uint16_t i = 0; i < FFT_SIZE; i++)
  {
    const double window =
      0.5 * (1.0 - cos(2.0 * PI * i / (FFT_SIZE - 1)));
    vReal[i] = (vReal[i] - mean) * window;
    vImag[i] = 0.0;
    windowSquareSum += window * window;
  }

  const double windowEnergyCorrection = windowSquareSum / FFT_SIZE;
  FFT.compute(FFTDirection::Forward);

  double spectralEnergy = 0.0;
  double weightedEnergy = 0.0;
  double maximumBinEnergy = 0.0;
  uint16_t maximumBin = 0;

  for (uint16_t bin = 0; bin <= FFT_SIZE / 2; bin++)
  {
    const double re = vReal[bin];
    const double im = vImag[bin];
    double binEnergy = re * re + im * im;

    if (bin > 0 && bin < FFT_SIZE / 2)
    {
      binEnergy *= 2.0;
    }

    spectralEnergy += binEnergy;

    if (bin > 0)
    {
      const double frequency = (double)bin * SAMPLE_RATE / FFT_SIZE;
      weightedEnergy += binEnergy * calculateAWeightPowerGain(frequency);

      if (bin < FFT_SIZE / 2 && binEnergy > maximumBinEnergy)
      {
        maximumBinEnergy = binEnergy;
        maximumBin = bin;
      }
    }
  }

  const double normalization =
    (double)FFT_SIZE * (double)FFT_SIZE * windowEnergyCorrection;
  accumulatedUnweightedMS += max(0.0, spectralEnergy / normalization);
  accumulatedWeightedMS += max(0.0, weightedEnergy / normalization);
  accumulatedDominantFrequency +=
    (double)maximumBin * SAMPLE_RATE / FFT_SIZE;
  accumulatedFrames++;

  if (accumulatedFrames >= FRAMES_PER_LEVEL)
  {
    publishLevelMeasurement();
  }
}

void audioTask(void* parameter)
{
  while (true)
  {
    size_t bytesRead = 0;
    const esp_err_t result = i2s_read(
      I2S_PORT,
      i2sBuffer,
      sizeof(i2sBuffer),
      &bytesRead,
      portMAX_DELAY
    );

    if (result != ESP_OK)
    {
      Serial.printf("[AUDIO] I2S read error: %d\n", result);
      continue;
    }

    const size_t samplesReceived = bytesRead / sizeof(int32_t);

    for (size_t i = 0; i < samplesReceived; i++)
    {
      const int32_t pcm = i2sBuffer[i] >> 8;
      vReal[fftIndex] = (double)pcm / PCM_FULL_SCALE;
      vImag[fftIndex] = 0.0;
      fftIndex++;

      if (fftIndex >= FFT_SIZE)
      {
        processFFTFrame();
        fftIndex = 0;
      }
    }
  }
}

// =====================================================
// WI-FI AND REPORTING
// =====================================================

void beginWiFiConnection()
{
  Serial.printf("[WIFI] Connecting to %s...\n", AP_SSID);
  WiFi.begin(AP_SSID, AP_PASSWORD);
}

void networkTask(void* parameter)
{
  MeasurementReport report;
  uint32_t nextReconnectAt = millis() + 1000;
  uint32_t reconnectDelayMs = 1000;
  bool wasConnected = false;

  while (true)
  {
    const uint32_t now = millis();
    const bool connected = WiFi.status() == WL_CONNECTED;

    if (connected && !wasConnected)
    {
      Serial.print("[WIFI] Connected. Sensor IP: ");
      Serial.println(WiFi.localIP());
      reconnectDelayMs = 1000;
    }
    else if (!connected && wasConnected)
    {
      Serial.println("[WIFI] Connection lost.");
      nextReconnectAt = now;
    }

    wasConnected = connected;

    if (!connected && (int32_t)(now - nextReconnectAt) >= 0)
    {
      WiFi.disconnect(false);
      beginWiFiConnection();
      nextReconnectAt = now + reconnectDelayMs;
      reconnectDelayMs = min(reconnectDelayMs * 2UL, 10000UL);
    }

    if (xQueueReceive(reportQueue, &report, pdMS_TO_TICKS(250)) != pdTRUE)
    {
      continue;
    }

    if (WiFi.status() != WL_CONNECTED)
    {
      continue;
    }

    String url = String("http://") + MASTER_HOST + "/api/v1/readings";
    HTTPClient http;
    http.setConnectTimeout(750);
    http.setTimeout(1200);

    if (!http.begin(url))
    {
      Serial.println("[HTTP] Could not initialise request.");
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", API_KEY);

    String payload;
    payload.reserve(280);
    payload += "{\"node_id\":\"" + String(NODE_ID) + "\"";
    payload += ",\"node_name\":\"" + String(NODE_NAME) + "\"";
    payload += ",\"boot_id\":" + String(bootID);
    payload += ",\"sequence\":" + String(report.sequence);
    payload += ",\"uptime_ms\":" + String(millis());
    payload += ",\"measured_at_ms\":" + String(report.measuredAtMs);
    payload += ",\"estimated_dba\":" + String(report.estimatedDBA, 3);
    payload += ",\"dbfs_a\":" + String(report.dbfsA, 3);
    payload += ",\"dominant_frequency\":" +
      String(report.dominantFrequency, 2);
    payload += ",\"integration_ms\":" + String(INTEGRATION_MS);
    payload += ",\"sample_rate\":" + String(SAMPLE_RATE);
    payload += "}";

    const int responseCode = http.POST(payload);

    if (responseCode != 202)
    {
      Serial.printf("[HTTP] Report failed: %d\n", responseCode);
    }

    http.end();
  }
}

// =====================================================
// ARDUINO ENTRY POINTS
// =====================================================

void setup()
{
  Serial.begin(115200);
  delay(1200);

  bootID = esp_random();
  Serial.println();
  Serial.println("==========================================");
  Serial.println("SPD DISTRIBUTED NOISE SENSOR");
  Serial.printf("Node             : %s (%s)\n", NODE_NAME, NODE_ID);
  Serial.printf("Boot ID          : %lu\n", (unsigned long)bootID);
  Serial.printf("Integration time : %lu ms\n", (unsigned long)INTEGRATION_MS);
  Serial.printf("Calibration      : %.2f dB\n", SENSOR_CALIBRATION_DB);
  Serial.println("==========================================");

  reportQueue = xQueueCreate(1, sizeof(MeasurementReport));

  if (reportQueue == nullptr)
  {
    Serial.println("[FATAL] Could not create report queue.");
    while (true)
    {
      delay(1000);
    }
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  beginWiFiConnection();
  setupI2S();

  xTaskCreatePinnedToCore(
    audioTask, "AudioTask", 12288, nullptr, 2, nullptr, 1
  );
  xTaskCreatePinnedToCore(
    networkTask, "NetworkTask", 8192, nullptr, 1, nullptr, 0
  );
}

void loop()
{
  delay(1000);
}
