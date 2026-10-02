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
constexpr uint32_t REPORT_INTERVAL_MS = 1000;
constexpr float PCM_FULL_SCALE = 8388608.0f;  // 2^23

constexpr size_t I2S_BUFFER_SIZE = 512;
int32_t i2sBuffer[I2S_BUFFER_SIZE];
float vReal[FFT_SIZE];
float vImag[FFT_SIZE];
float hannWindow[FFT_SIZE];
float aWeightPowerGain[FFT_SIZE / 2 + 1];
float windowEnergyCorrection = 1.0f;
ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

uint16_t fftIndex = 0;
float accumulatedWeightedMS = 0.0f;
float accumulatedDominantFrequency = 0.0f;
uint32_t accumulatedFrames = 0;
uint32_t sequenceNumber = 0;
uint32_t bootID = 0;

struct MeasurementReport
{
  uint32_t sequence;
  uint32_t measuredAtMs;
  uint32_t integrationMs;
  float estimatedDBA;
  float dbfsA;
  float dominantFrequency;
};

QueueHandle_t reportQueue = nullptr;
SemaphoreHandle_t dspMutex = nullptr;

// =====================================================
// A-WEIGHTING
// =====================================================

float calculateAWeightingDB(float frequency)
{
  if (frequency <= 0.0f)
  {
    return -200.0f;
  }

  constexpr float c1 = 20.6f;
  constexpr float c2 = 107.7f;
  constexpr float c3 = 737.9f;
  constexpr float c4 = 12194.0f;

  const float f2 = frequency * frequency;
  const float numerator = (c4 * c4) * f2 * f2;
  const float denominator =
    (f2 + c1 * c1) *
    sqrtf((f2 + c2 * c2) * (f2 + c3 * c3)) *
    (f2 + c4 * c4);
  const float response = numerator / denominator;

  return response > 0.0f ? 20.0f * log10f(response) + 2.0f : -200.0f;
}

float calculateAWeightPowerGain(float frequency)
{
  return powf(10.0f, calculateAWeightingDB(frequency) / 10.0f);
}

void setupDSPTables()
{
  float windowSquareSum = 0.0f;

  for (uint16_t i = 0; i < FFT_SIZE; i++)
  {
    const float window =
      0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SIZE - 1)));
    hannWindow[i] = window;
    windowSquareSum += window * window;
  }

  windowEnergyCorrection = windowSquareSum / FFT_SIZE;
  aWeightPowerGain[0] = 0.0f;

  for (uint16_t bin = 1; bin <= FFT_SIZE / 2; bin++)
  {
    const float frequency = (float)bin * SAMPLE_RATE / FFT_SIZE;
    aWeightPowerGain[bin] = calculateAWeightPowerGain(frequency);
  }

  Serial.println("[DSP] Window and A-weighting tables initialised.");
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
  if (xSemaphoreTake(dspMutex, pdMS_TO_TICKS(100)) != pdTRUE)
  {
    Serial.println("[DSP] Could not snapshot measurement window.");
    return;
  }

  if (accumulatedFrames == 0)
  {
    xSemaphoreGive(dspMutex);
    return;
  }

  const uint32_t completedFrames = accumulatedFrames;
  const float completedWeightedMS = accumulatedWeightedMS;
  const float completedDominantFrequency = accumulatedDominantFrequency;

  accumulatedWeightedMS = 0.0f;
  accumulatedDominantFrequency = 0.0f;
  accumulatedFrames = 0;
  xSemaphoreGive(dspMutex);

  const float averageWeightedMS =
    max(0.0f, completedWeightedMS / completedFrames);
  const float rmsA = sqrtf(averageWeightedMS);
  const float dbfsA = rmsA > 1e-12f ? 20.0f * log10f(rmsA) : -120.0f;
  const float estimatedDBA = dbfsA + SENSOR_CALIBRATION_DB;

  MeasurementReport report = {
    .sequence = ++sequenceNumber,
    .measuredAtMs = millis(),
    .integrationMs =
      (uint32_t)(((uint64_t)completedFrames * FFT_SIZE * 1000ULL) /
                 SAMPLE_RATE),
    .estimatedDBA = estimatedDBA,
    .dbfsA = dbfsA,
    .dominantFrequency = completedDominantFrequency / completedFrames
  };

  if (isfinite(report.estimatedDBA) && report.estimatedDBA >= 0.0f &&
      report.estimatedDBA <= 160.0f)
  {
    xQueueOverwrite(reportQueue, &report);
    Serial.printf(
      "[LEVEL] seq=%lu level=%.2f dBA integration=%lu ms "
      "dominant=%.1f Hz\n",
      (unsigned long)report.sequence,
      report.estimatedDBA,
      (unsigned long)report.integrationMs,
      report.dominantFrequency
    );
  }
  else
  {
    Serial.printf("[LEVEL] Rejected invalid value: %.2f dBA\n", estimatedDBA);
  }

}

void processFFTFrame()
{
  float mean = 0.0f;

  for (uint16_t i = 0; i < FFT_SIZE; i++)
  {
    mean += vReal[i];
  }

  mean /= FFT_SIZE;

  for (uint16_t i = 0; i < FFT_SIZE; i++)
  {
    vReal[i] = (vReal[i] - mean) * hannWindow[i];
    vImag[i] = 0.0f;
  }

  FFT.compute(FFTDirection::Forward);

  float weightedEnergy = 0.0f;
  float maximumBinEnergy = 0.0f;
  uint16_t maximumBin = 0;

  for (uint16_t bin = 0; bin <= FFT_SIZE / 2; bin++)
  {
    const float re = vReal[bin];
    const float im = vImag[bin];
    float binEnergy = re * re + im * im;

    if (bin > 0 && bin < FFT_SIZE / 2)
    {
      binEnergy *= 2.0f;
    }

    if (bin > 0)
    {
      weightedEnergy += binEnergy * aWeightPowerGain[bin];

      if (bin < FFT_SIZE / 2 && binEnergy > maximumBinEnergy)
      {
        maximumBinEnergy = binEnergy;
        maximumBin = bin;
      }
    }
  }

  const float normalization =
    (float)FFT_SIZE * (float)FFT_SIZE * windowEnergyCorrection;

  if (xSemaphoreTake(dspMutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    accumulatedWeightedMS += max(0.0f, weightedEnergy / normalization);
    accumulatedDominantFrequency +=
      (float)maximumBin * SAMPLE_RATE / FFT_SIZE;
    accumulatedFrames++;
    xSemaphoreGive(dspMutex);
  }
  else
  {
    Serial.println("[DSP] Dropped processed FFT frame: accumulator busy.");
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
      vReal[fftIndex] = (float)pcm / PCM_FULL_SCALE;
      vImag[fftIndex] = 0.0f;
      fftIndex++;

      if (fftIndex >= FFT_SIZE)
      {
        processFFTFrame();
        fftIndex = 0;
      }
    }
  }
}

void reportingTask(void* parameter)
{
  TickType_t nextWake = xTaskGetTickCount();

  while (true)
  {
    vTaskDelayUntil(&nextWake, pdMS_TO_TICKS(REPORT_INTERVAL_MS));
    publishLevelMeasurement();
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
    payload += ",\"integration_ms\":" + String(report.integrationMs);
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
  Serial.printf("Report interval  : %lu ms\n", (unsigned long)REPORT_INTERVAL_MS);
  Serial.printf("Calibration      : %.2f dB\n", SENSOR_CALIBRATION_DB);
  Serial.println("==========================================");

  reportQueue = xQueueCreate(1, sizeof(MeasurementReport));
  dspMutex = xSemaphoreCreateMutex();

  if (reportQueue == nullptr || dspMutex == nullptr)
  {
    Serial.println("[FATAL] Could not create queue or DSP mutex.");
    while (true)
    {
      delay(1000);
    }
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  beginWiFiConnection();
  setupDSPTables();
  setupI2S();

  xTaskCreatePinnedToCore(
    audioTask, "AudioTask", 12288, nullptr, 2, nullptr, 1
  );
  xTaskCreatePinnedToCore(
    networkTask, "NetworkTask", 8192, nullptr, 1, nullptr, 0
  );
  xTaskCreatePinnedToCore(
    reportingTask, "ReportingTask", 4096, nullptr, 1, nullptr, 0
  );
}

void loop()
{
  delay(1000);
}
