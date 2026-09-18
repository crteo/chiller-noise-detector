#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <driver/i2s.h>
#include <arduinoFFT.h>
#include <math.h>

// =====================================================
// LOCAL WI-FI ACCESS POINT
// =====================================================

const char* AP_SSID = "SPD_Noise_Monitor";
const char* AP_PASSWORD = "NoiseMonitor123";

IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);

WebServer server(80);

// =====================================================
// INMP441 / I2S PINS
// =====================================================

#define I2S_BCLK   18
#define I2S_LRCLK  16
#define I2S_DIN    17
#define I2S_PORT   I2S_NUM_0

// =====================================================
// AUDIO SETTINGS
// =====================================================

const uint32_t SAMPLE_RATE = 48000;
const uint16_t FFT_SIZE = 4096;
const uint8_t FRAMES_PER_LEVEL = 3;

// Nominal INMP441 sensitivity-based calibration.
// Replace after comparison with a traceable reference.
const double CALIB_CONST = 123.01;

const uint32_t LEVEL_SAMPLE_COUNT =
  FFT_SIZE * FRAMES_PER_LEVEL;

const double LEVEL_DURATION_SECONDS =
  (double)LEVEL_SAMPLE_COUNT / SAMPLE_RATE;

// INMP441 signed 24-bit PCM in 32-bit I2S word.
const double PCM_FULL_SCALE = 8388608.0;  // 2^23

// =====================================================
// HISTORICAL LOGGING SETTINGS
// =====================================================

// Target duration of one historical CSV record.
const double LOG_INTERVAL_SECONDS = 10.0;

// Number of normal measurement results to combine
// into approximately one 10-second log entry.
const uint16_t RESULTS_PER_LOG =
  (uint16_t)round(
    LOG_INTERVAL_SECONDS / LEVEL_DURATION_SECONDS
  );

const char* LOG_FILE = "/acoustic_log.csv";

// =====================================================
// I2S AND FFT BUFFERS
// =====================================================

const int I2S_BUFFER_SIZE = 512;
int32_t i2sBuffer[I2S_BUFFER_SIZE];

double vReal[FFT_SIZE];
double vImag[FFT_SIZE];

ArduinoFFT<double> FFT(
  vReal,
  vImag,
  FFT_SIZE,
  SAMPLE_RATE
);

uint16_t fftIndex = 0;

// =====================================================
// REAL SPECTRUM OUTPUT FOR DASHBOARD
// 32 logarithmic bands from 20 Hz to 20 kHz.
// =====================================================

const uint8_t SPECTRUM_BANDS = 32;
const double SPECTRUM_MIN_HZ = 20.0;
const double SPECTRUM_MAX_HZ = 20000.0;
const double SPECTRUM_DYNAMIC_RANGE_DB = 60.0;

double accumulatedSpectrumEnergy[SPECTRUM_BANDS] =
  {0.0};

// =====================================================
// ASSET-MONITORING FREQUENCY BANDS
// These are independent from the 32 dashboard bands.
// =====================================================

const uint8_t ASSET_BANDS = 8;

const double ASSET_BAND_LOW[ASSET_BANDS] =
{
  60.0,
  120.0,
  250.0,
  500.0,
  1000.0,
  2000.0,
  4000.0,
  8000.0
};

const double ASSET_BAND_HIGH[ASSET_BANDS] =
{
  120.0,
  250.0,
  500.0,
  1000.0,
  2000.0,
  4000.0,
  8000.0,
  15000.0
};

// Accumulated mean-square energy across
// FRAMES_PER_LEVEL FFT frames.
double accumulatedAssetBandMS[ASSET_BANDS] =
  {0.0};

// =====================================================
// SHORT-TERM MEASUREMENT ACCUMULATORS
// =====================================================

double accumulatedUnweightedMS = 0.0;
double accumulatedWeightedMS = 0.0;
double accumulatedDominantFrequency = 0.0;

uint8_t accumulatedFrames = 0;

// =====================================================
// 10-SECOND LOGGING ACCUMULATORS
// Store energies rather than averaging dB directly.
// =====================================================

double logSumUnweightedMS = 0.0;
double logSumWeightedMS = 0.0;

double logSumAssetBandMS[ASSET_BANDS] =
  {0.0};

double logSumDominantFrequency = 0.0;

uint16_t logResultCount = 0;
unsigned long logRecordID = 0;

// =====================================================
// SHARED RESULTS
// =====================================================

SemaphoreHandle_t resultMutex;

double latestDBFS = -120.0;
double latestDBFSA = -120.0;

double latestRMS = 0.0;
double latestRMSA = 0.0;

double latestACorrection = 0.0;
double latestDominantFrequency = 0.0;
double latestFFTError = 0.0;

double latestEstimatedDBA = 0.0;
double latestEstimatedDBZ = 0.0;

double latestSpectrum[SPECTRUM_BANDS] =
  {0.0};

unsigned long latestResultID = 0;

// =====================================================
// A-WEIGHTING
// =====================================================

double calculateAWeightingDB(double f)
{
  if (f <= 0.0)
  {
    return -200.0;
  }

  const double c1 = 20.6;
  const double c2 = 107.7;
  const double c3 = 737.9;
  const double c4 = 12194.0;

  const double f2 = f * f;

  const double c1_2 = c1 * c1;
  const double c2_2 = c2 * c2;
  const double c3_2 = c3 * c3;
  const double c4_2 = c4 * c4;

  const double numerator =
    c4_2 * f2 * f2;

  const double denominator =
    (f2 + c1_2) *
    sqrt(
      (f2 + c2_2) *
      (f2 + c3_2)
    ) *
    (f2 + c4_2);

  const double RA =
    numerator / denominator;

  if (RA <= 0.0)
  {
    return -200.0;
  }

  return
    20.0 * log10(RA) + 2.0;
}

double calculateAWeightPowerGain(double frequency)
{
  return pow(
    10.0,
    calculateAWeightingDB(frequency) / 10.0
  );
}

// =====================================================
// DASHBOARD SPECTRUM BAND MAPPING
// =====================================================

int spectrumBandForFrequency(double frequency)
{
  if (
    frequency < SPECTRUM_MIN_HZ ||
    frequency > SPECTRUM_MAX_HZ
  )
  {
    return -1;
  }

  const double position =
    log(
      frequency /
      SPECTRUM_MIN_HZ
    ) /
    log(
      SPECTRUM_MAX_HZ /
      SPECTRUM_MIN_HZ
    );

  int band =
    (int)floor(
      position * SPECTRUM_BANDS
    );

  if (band < 0)
  {
    band = 0;
  }

  if (band >= SPECTRUM_BANDS)
  {
    band =
      SPECTRUM_BANDS - 1;
  }

  return band;
}

// =====================================================
// CSV LOG FILE
// =====================================================

void initialiseLogFile()
{
  if (!LittleFS.exists(LOG_FILE))
  {
    File file =
      LittleFS.open(
        LOG_FILE,
        FILE_WRITE
      );

    if (!file)
    {
      Serial.println(
        "[LOG] Failed to create CSV."
      );
      return;
    }

    file.println(
      "record_id,"
      "uptime_ms,"
      "window_s,"
      "dba_est,"
      "dbfs_z,"
      "dbz_est,"
      "dominant_hz,"
      "b60_120,"
      "b120_250,"
      "b250_500,"
      "b500_1000,"
      "b1000_2000,"
      "b2000_4000,"
      "b4000_8000,"
      "b8000_15000"
    );

    file.close();

    Serial.println(
      "[LOG] CSV created."
    );
  }
  else
  {
    Serial.println(
      "[LOG] Existing CSV found."
    );
  }
}

// =====================================================
// WRITE ONE ~10 SECOND HISTORICAL RECORD
// =====================================================

void writeHistoricalLog()
{
  if (logResultCount == 0)
  {
    return;
  }

  const double averageUnweightedMS =
    logSumUnweightedMS /
    logResultCount;

  const double averageWeightedMS =
    logSumWeightedMS /
    logResultCount;

  double dbfsZ = -120.0;
  double dbfsA = -120.0;

  if (averageUnweightedMS > 1e-20)
  {
    dbfsZ =
      10.0 *
      log10(
        averageUnweightedMS
      );
  }

  if (averageWeightedMS > 1e-20)
  {
    dbfsA =
      10.0 *
      log10(
        averageWeightedMS
      );
  }

  const double estimatedDBZ =
    dbfsZ + CALIB_CONST;

  const double estimatedDBA =
    dbfsA + CALIB_CONST;

  const double averageDominantFrequency =
    logSumDominantFrequency /
    logResultCount;

  double bandDB[ASSET_BANDS];

  for (
    uint8_t b = 0;
    b < ASSET_BANDS;
    b++
  )
  {
    const double bandMS =
      logSumAssetBandMS[b] /
      logResultCount;

    if (bandMS > 1e-20)
    {
      bandDB[b] =
        10.0 * log10(bandMS);
    }
    else
    {
      bandDB[b] = -120.0;
    }
  }

  logRecordID++;

  File file =
    LittleFS.open(
      LOG_FILE,
      FILE_APPEND
    );

  if (!file)
  {
    Serial.println(
      "[LOG] Failed to open CSV."
    );
  }
  else
  {
    file.print(logRecordID);
    file.print(",");

    file.print(millis());
    file.print(",");

    file.print(
      logResultCount *
      LEVEL_DURATION_SECONDS,
      3
    );
    file.print(",");

    file.print(
      estimatedDBA,
      2
    );
    file.print(",");

    file.print(
      dbfsZ,
      2
    );
    file.print(",");

    file.print(
      estimatedDBZ,
      2
    );
    file.print(",");

    file.print(
      averageDominantFrequency,
      2
    );

    for (
      uint8_t b = 0;
      b < ASSET_BANDS;
      b++
    )
    {
      file.print(",");
      file.print(
        bandDB[b],
        2
      );
    }

    file.println();
    file.close();

    // Also emit machine-readable line
    // to Serial for debugging.
    Serial.print("LOG,");
    Serial.print(logRecordID);
    Serial.print(",");
    Serial.print(millis());
    Serial.print(",");
    Serial.print(
      estimatedDBA,
      2
    );
    Serial.print(",");
    Serial.print(
      dbfsZ,
      2
    );
    Serial.print(",");
    Serial.print(
      estimatedDBZ,
      2
    );
    Serial.print(",");
    Serial.print(
      averageDominantFrequency,
      2
    );

    for (
      uint8_t b = 0;
      b < ASSET_BANDS;
      b++
    )
    {
      Serial.print(",");
      Serial.print(
        bandDB[b],
        2
      );
    }

    Serial.println();
  }

  // Reset historical accumulators.
  logSumUnweightedMS = 0.0;
  logSumWeightedMS = 0.0;
  logSumDominantFrequency = 0.0;

  for (
    uint8_t b = 0;
    b < ASSET_BANDS;
    b++
  )
  {
    logSumAssetBandMS[b] = 0.0;
  }

  logResultCount = 0;
}

// =====================================================
// WI-FI ACCESS POINT
// =====================================================

void setupAccessPoint()
{
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  if (
    !WiFi.softAPConfig(
      AP_IP,
      AP_GATEWAY,
      AP_SUBNET
    )
  )
  {
    Serial.println(
      "[AP] Static IP configuration failed."
    );
  }

  const bool started =
    WiFi.softAP(
      AP_SSID,
      AP_PASSWORD,
      1,
      false,
      4
    );

  if (!started)
  {
    Serial.println(
      "[AP] Failed to start access point."
    );

    while (true)
    {
      delay(1000);
    }
  }

  Serial.println();
  Serial.println(
    "=========================================="
  );
  Serial.println(
    "LOCAL WI-FI ACCESS POINT READY"
  );

  Serial.print("Network  : ");
  Serial.println(AP_SSID);

  Serial.print("Dashboard: http://");
  Serial.println(WiFi.softAPIP());

  Serial.print("CSV log  : http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/log.csv");

  Serial.println(
    "=========================================="
  );
}

// =====================================================
// WEB SERVER
// =====================================================

void sendIndexPage()
{
  File file =
    LittleFS.open(
      "/index.html",
      "r"
    );

  if (!file)
  {
    server.send(
      500,
      "text/plain",
      "index.html is missing. Upload the data folder to LittleFS."
    );

    return;
  }

  server.streamFile(
    file,
    "text/html"
  );

  file.close();
}

// =====================================================
// CSV DOWNLOAD ENDPOINT
// =====================================================

void sendLogFile()
{
  File file =
    LittleFS.open(
      LOG_FILE,
      "r"
    );

  if (!file)
  {
    server.send(
      404,
      "text/plain",
      "acoustic_log.csv not found"
    );

    return;
  }

  server.sendHeader(
    "Cache-Control",
    "no-store"
  );

  server.sendHeader(
    "Content-Disposition",
    "attachment; filename=acoustic_log.csv"
  );

  server.streamFile(
    file,
    "text/csv"
  );

  file.close();
}

// =====================================================
// CURRENT MEASUREMENT JSON
// Existing frontend remains compatible.
// =====================================================

void sendMeasurementData()
{
  double dbfs;
  double dbfsA;
  double rms;
  double rmsA;
  double correction;
  double dominantFrequency;
  double fftError;
  double estimatedDBA;
  double estimatedDBZ;

  double spectrum[SPECTRUM_BANDS];

  unsigned long resultID;

  if (
    xSemaphoreTake(
      resultMutex,
      pdMS_TO_TICKS(50)
    ) != pdTRUE
  )
  {
    server.send(
      503,
      "application/json",
      "{\"error\":\"measurement_busy\"}"
    );

    return;
  }

  dbfs =
    latestDBFS;

  dbfsA =
    latestDBFSA;

  rms =
    latestRMS;

  rmsA =
    latestRMSA;

  correction =
    latestACorrection;

  dominantFrequency =
    latestDominantFrequency;

  fftError =
    latestFFTError;

  estimatedDBA =
    latestEstimatedDBA;

  estimatedDBZ =
    latestEstimatedDBZ;

  resultID =
    latestResultID;

  for (
    uint8_t i = 0;
    i < SPECTRUM_BANDS;
    i++
  )
  {
    spectrum[i] =
      latestSpectrum[i];
  }

  xSemaphoreGive(resultMutex);

  String json;
  json.reserve(1500);

  json += "{";

  json += "\"ready\":";
  json +=
    (resultID > 0) ?
    "true" :
    "false";

  json +=
    ",\"result_id\":" +
    String(resultID);

  json +=
    ",\"dbfs\":" +
    String(dbfs, 2);

  json +=
    ",\"dbfsA\":" +
    String(dbfsA, 2);

  json +=
    ",\"estimated_dba\":" +
    String(estimatedDBA, 2);

  // Added, but existing frontend
  // does not need to use it.
  json +=
    ",\"estimated_dbz\":" +
    String(estimatedDBZ, 2);

  json +=
    ",\"rms\":" +
    String(rms, 8);

  json +=
    ",\"rmsA\":" +
    String(rmsA, 8);

  json +=
    ",\"a_correction\":" +
    String(correction, 2);

  json +=
    ",\"dominant_frequency\":" +
    String(dominantFrequency, 2);

  json +=
    ",\"fft_error_db\":" +
    String(fftError, 4);

  json +=
    ",\"fft_size\":" +
    String(FFT_SIZE);

  json +=
    ",\"frames_per_level\":" +
    String(FRAMES_PER_LEVEL);

  json +=
    ",\"measurement_duration_s\":" +
    String(
      LEVEL_DURATION_SECONDS,
      3
    );

  json +=
    ",\"sample_rate\":" +
    String(SAMPLE_RATE);

  json +=
    ",\"connected_clients\":" +
    String(
      WiFi.softAPgetStationNum()
    );

  json +=
    ",\"uptime_ms\":" +
    String(millis());

  json +=
    ",\"spectrum\":[";

  for (
    uint8_t i = 0;
    i < SPECTRUM_BANDS;
    i++
  )
  {
    if (i > 0)
    {
      json += ",";
    }

    json +=
      String(
        spectrum[i],
        4
      );
  }

  json += "]}";

  server.sendHeader(
    "Cache-Control",
    "no-store, no-cache, must-revalidate"
  );

  server.send(
    200,
    "application/json",
    json
  );
}

void setupWebServer()
{
  if (!LittleFS.begin(true))
  {
    Serial.println(
      "[LittleFS] Mount failed."
    );

    while (true)
    {
      delay(1000);
    }
  }

  Serial.println(
    "[LittleFS] Mounted."
  );

  if (
    !LittleFS.exists(
      "/index.html"
    )
  )
  {
    Serial.println(
      "[LittleFS] WARNING: /index.html not found."
    );
  }

  initialiseLogFile();

  server.on(
    "/",
    HTTP_GET,
    sendIndexPage
  );

  server.on(
    "/data",
    HTTP_GET,
    sendMeasurementData
  );

  // New CSV endpoint.
  server.on(
    "/log.csv",
    HTTP_GET,
    sendLogFile
  );

  server.on(
    "/favicon.ico",
    HTTP_GET,
    []()
    {
      server.send(
        204,
        "text/plain",
        ""
      );
    }
  );

  server.onNotFound(
    []()
    {
      server.send(
        404,
        "text/plain",
        "Not found"
      );
    }
  );

  server.begin();

  Serial.println(
    "[WEB] Local web server started."
  );
}

void webServerTask(void* parameter)
{
  while (true)
  {
    server.handleClient();

    vTaskDelay(
      pdMS_TO_TICKS(2)
    );
  }
}

// =====================================================
// I2S
// =====================================================

void setupI2S()
{
  i2s_config_t i2sConfig =
  {
    .mode =
      (i2s_mode_t)(
        I2S_MODE_MASTER |
        I2S_MODE_RX
      ),

    .sample_rate =
      SAMPLE_RATE,

    .bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT,

    .channel_format =
      I2S_CHANNEL_FMT_ONLY_LEFT,

    .communication_format =
      I2S_COMM_FORMAT_STAND_I2S,

    .intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count =
      16,

    .dma_buf_len =
      256,

    .use_apll =
      false,

    .tx_desc_auto_clear =
      false,

    .fixed_mclk =
      0
  };

  i2s_pin_config_t pinConfig =
  {
    .bck_io_num =
      I2S_BCLK,

    .ws_io_num =
      I2S_LRCLK,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num =
      I2S_DIN
  };

  esp_err_t result =
    i2s_driver_install(
      I2S_PORT,
      &i2sConfig,
      0,
      NULL
    );

  if (result != ESP_OK)
  {
    Serial.print(
      "[I2S] Driver installation failed: "
    );

    Serial.println(result);

    while (true)
    {
      delay(1000);
    }
  }

  result =
    i2s_set_pin(
      I2S_PORT,
      &pinConfig
    );

  if (result != ESP_OK)
  {
    Serial.print(
      "[I2S] Pin configuration failed: "
    );

    Serial.println(result);

    while (true)
    {
      delay(1000);
    }
  }

  Serial.println(
    "[I2S] Initialised."
  );
}

// =====================================================
// FINALISE ONE BASE MEASUREMENT
// Current configuration = about 0.256 seconds.
// =====================================================

void finishLevelMeasurement()
{
  if (accumulatedFrames == 0)
  {
    return;
  }

  double averageMS =
    accumulatedUnweightedMS /
    accumulatedFrames;

  double averageMSA =
    accumulatedWeightedMS /
    accumulatedFrames;

  averageMS =
    max(0.0, averageMS);

  averageMSA =
    max(0.0, averageMSA);

  const double rms =
    sqrt(averageMS);

  const double rmsA =
    sqrt(averageMSA);

  double dbfs = -120.0;
  double dbfsA = -120.0;

  if (rms > 1e-12)
  {
    dbfs =
      20.0 *
      log10(rms);
  }

  if (rmsA > 1e-12)
  {
    dbfsA =
      20.0 *
      log10(rmsA);
  }

  const double aCorrection =
    dbfsA - dbfs;

  const double dominantFrequency =
    accumulatedDominantFrequency /
    accumulatedFrames;

  const double estimatedDBA =
    dbfsA + CALIB_CONST;

  const double estimatedDBZ =
    dbfs + CALIB_CONST;

  // ===================================================
  // ASSET BAND LEVELS FOR THIS BASE MEASUREMENT
  // ===================================================

  double assetBandMS[ASSET_BANDS];
  double assetBandDB[ASSET_BANDS];

  for (
    uint8_t b = 0;
    b < ASSET_BANDS;
    b++
  )
  {
    assetBandMS[b] =
      accumulatedAssetBandMS[b] /
      accumulatedFrames;

    if (assetBandMS[b] > 1e-20)
    {
      assetBandDB[b] =
        10.0 *
        log10(
          assetBandMS[b]
        );
    }
    else
    {
      assetBandDB[b] =
        -120.0;
    }
  }

  // ===================================================
  // DASHBOARD SPECTRUM
  // ===================================================

  double spectrumDisplay[
    SPECTRUM_BANDS
  ];

  double maximumBandEnergy =
    0.0;

  for (
    uint8_t i = 0;
    i < SPECTRUM_BANDS;
    i++
  )
  {
    const double averageBandEnergy =
      accumulatedSpectrumEnergy[i] /
      accumulatedFrames;

    if (
      averageBandEnergy >
      maximumBandEnergy
    )
    {
      maximumBandEnergy =
        averageBandEnergy;
    }
  }

  for (
    uint8_t i = 0;
    i < SPECTRUM_BANDS;
    i++
  )
  {
    const double averageBandEnergy =
      accumulatedSpectrumEnergy[i] /
      accumulatedFrames;

    if (
      maximumBandEnergy <= 1e-30 ||
      averageBandEnergy <= 1e-30
    )
    {
      spectrumDisplay[i] =
        0.0;

      continue;
    }

    const double relativeDB =
      10.0 *
      log10(
        averageBandEnergy /
        maximumBandEnergy
      );

    spectrumDisplay[i] =
      constrain(
        (
          relativeDB +
          SPECTRUM_DYNAMIC_RANGE_DB
        ) /
        SPECTRUM_DYNAMIC_RANGE_DB,
        0.0,
        1.0
      );
  }

  // ===================================================
  // UPDATE SHARED DASHBOARD RESULTS
  // ===================================================

  if (
    xSemaphoreTake(
      resultMutex,
      portMAX_DELAY
    ) == pdTRUE
  )
  {
    latestDBFS =
      dbfs;

    latestDBFSA =
      dbfsA;

    latestRMS =
      rms;

    latestRMSA =
      rmsA;

    latestACorrection =
      aCorrection;

    latestDominantFrequency =
      dominantFrequency;

    latestEstimatedDBA =
      estimatedDBA;

    latestEstimatedDBZ =
      estimatedDBZ;

    for (
      uint8_t i = 0;
      i < SPECTRUM_BANDS;
      i++
    )
    {
      latestSpectrum[i] =
        spectrumDisplay[i];
    }

    latestResultID++;

    xSemaphoreGive(
      resultMutex
    );
  }

  // ===================================================
  // ADD BASE MEASUREMENT INTO 10-SECOND LOG WINDOW
  // ===================================================

  logSumUnweightedMS +=
    averageMS;

  logSumWeightedMS +=
    averageMSA;

  logSumDominantFrequency +=
    dominantFrequency;

  for (
    uint8_t b = 0;
    b < ASSET_BANDS;
    b++
  )
  {
    logSumAssetBandMS[b] +=
      assetBandMS[b];
  }

  logResultCount++;

  if (
    logResultCount >=
    RESULTS_PER_LOG
  )
  {
    writeHistoricalLog();
  }

  // ===================================================
  // SERIAL DIAGNOSTICS
  // ===================================================

  Serial.println();

  Serial.println(
    "=========================================="
  );

  Serial.print(
    "Result ID        : "
  );
  Serial.println(
    latestResultID
  );

  Serial.print(
    "Integration time : "
  );
  Serial.print(
    LEVEL_DURATION_SECONDS,
    3
  );
  Serial.println(" s");

  Serial.print(
    "Unweighted level : "
  );
  Serial.print(
    dbfs,
    2
  );
  Serial.println(" dBFS-Z");

  Serial.print(
    "Estimated Z      : "
  );
  Serial.print(
    estimatedDBZ,
    2
  );
  Serial.println(" dBZ");

  Serial.print(
    "A-weighted level : "
  );
  Serial.print(
    dbfsA,
    2
  );
  Serial.println(" dBFS(A)");

  Serial.print(
    "Estimated A      : "
  );
  Serial.print(
    estimatedDBA,
    2
  );
  Serial.println(" dBA");

  Serial.print(
    "Dominant freq.   : "
  );
  Serial.print(
    dominantFrequency,
    1
  );
  Serial.println(" Hz");

  Serial.print(
    "Log progress     : "
  );
  Serial.print(
    logResultCount
  );
  Serial.print("/");
  Serial.println(
    RESULTS_PER_LOG
  );

  Serial.println(
    "=========================================="
  );

  // ===================================================
  // RESET BASE MEASUREMENT ACCUMULATORS
  // ===================================================

  accumulatedUnweightedMS =
    0.0;

  accumulatedWeightedMS =
    0.0;

  accumulatedDominantFrequency =
    0.0;

  accumulatedFrames =
    0;

  for (
    uint8_t i = 0;
    i < SPECTRUM_BANDS;
    i++
  )
  {
    accumulatedSpectrumEnergy[i] =
      0.0;
  }

  for (
    uint8_t b = 0;
    b < ASSET_BANDS;
    b++
  )
  {
    accumulatedAssetBandMS[b] =
      0.0;
  }
}

// =====================================================
// PROCESS ONE 4096-SAMPLE FFT FRAME
// =====================================================

void processFFTFrame()
{
  // ---------------------------------------------------
  // 1. Remove frame mean / DC component
  // ---------------------------------------------------

  double mean = 0.0;

  for (
    uint16_t i = 0;
    i < FFT_SIZE;
    i++
  )
  {
    mean += vReal[i];
  }

  mean /= FFT_SIZE;

  for (
    uint16_t i = 0;
    i < FFT_SIZE;
    i++
  )
  {
    vReal[i] -= mean;
  }

  // ---------------------------------------------------
  // 2. Apply Hann window
  // ---------------------------------------------------

  double windowSquareSum = 0.0;
  double windowedSquareSum = 0.0;

  for (
    uint16_t i = 0;
    i < FFT_SIZE;
    i++
  )
  {
    const double w =
      0.5 *
      (
        1.0 -
        cos(
          2.0 *
          PI *
          i /
          (FFT_SIZE - 1)
        )
      );

    vReal[i] *= w;
    vImag[i] = 0.0;

    windowSquareSum +=
      w * w;

    windowedSquareSum +=
      vReal[i] *
      vReal[i];
  }

  const double U =
    windowSquareSum /
    FFT_SIZE;

  const double timeMeanSquare =
    windowedSquareSum /
    (
      FFT_SIZE *
      U
    );

  const double timeRMS =
    sqrt(
      max(
        0.0,
        timeMeanSquare
      )
    );

  const double timeDBFS =
    (timeRMS > 1e-12)
      ?
      20.0 *
      log10(timeRMS)
      :
      -120.0;

  // ---------------------------------------------------
  // 3. FFT
  // ---------------------------------------------------

  FFT.compute(
    FFTDirection::Forward
  );

  // ---------------------------------------------------
  // 4. Spectrum energy
  // ---------------------------------------------------

  double spectralEnergy = 0.0;
  double weightedEnergy = 0.0;

  double maxBinEnergy = 0.0;
  uint16_t maxBin = 0;

  double assetBandRawEnergy[
    ASSET_BANDS
  ] =
  {0.0};

  for (
    uint16_t k = 0;
    k <= FFT_SIZE / 2;
    k++
  )
  {
    const double re =
      vReal[k];

    const double im =
      vImag[k];

    const double magnitudeSquared =
      re * re +
      im * im;

    double binEnergy =
      magnitudeSquared;

    if (
      k > 0 &&
      k < FFT_SIZE / 2
    )
    {
      binEnergy *= 2.0;
    }

    spectralEnergy +=
      binEnergy;

    if (k > 0)
    {
      const double frequency =
        (
          (double)k *
          SAMPLE_RATE
        ) /
        FFT_SIZE;

      weightedEnergy +=
        binEnergy *
        calculateAWeightPowerGain(
          frequency
        );

      // Existing dashboard spectrum
      const int spectrumBand =
        spectrumBandForFrequency(
          frequency
        );

      if (
        spectrumBand >= 0
      )
      {
        accumulatedSpectrumEnergy[
          spectrumBand
        ] +=
          binEnergy;
      }

      // New asset-monitoring bands
      for (
        uint8_t b = 0;
        b < ASSET_BANDS;
        b++
      )
      {
        if (
          frequency >=
            ASSET_BAND_LOW[b]
          &&
          frequency <
            ASSET_BAND_HIGH[b]
        )
        {
          assetBandRawEnergy[b] +=
            binEnergy;

          break;
        }
      }

      if (
        k < FFT_SIZE / 2 &&
        binEnergy >
        maxBinEnergy
      )
      {
        maxBinEnergy =
          binEnergy;

        maxBin =
          k;
      }
    }
  }

  // ---------------------------------------------------
  // 5. Parseval + Hann normalisation
  // ---------------------------------------------------

  const double normalization =
    (double)FFT_SIZE *
    (double)FFT_SIZE *
    U;

  const double fftMeanSquare =
    max(
      0.0,
      spectralEnergy /
      normalization
    );

  const double weightedMeanSquare =
    max(
      0.0,
      weightedEnergy /
      normalization
    );

  // Normalise each asset-monitoring band.
  for (
    uint8_t b = 0;
    b < ASSET_BANDS;
    b++
  )
  {
    accumulatedAssetBandMS[b] +=
      assetBandRawEnergy[b] /
      normalization;
  }

  const double fftRMS =
    sqrt(
      fftMeanSquare
    );

  const double fftDBFS =
    (fftRMS > 1e-12)
      ?
      20.0 *
      log10(fftRMS)
      :
      -120.0;

  const double fftError =
    fftDBFS -
    timeDBFS;

  const double dominantFrequency =
    (
      (double)maxBin *
      SAMPLE_RATE
    ) /
    FFT_SIZE;

  accumulatedUnweightedMS +=
    fftMeanSquare;

  accumulatedWeightedMS +=
    weightedMeanSquare;

  accumulatedDominantFrequency +=
    dominantFrequency;

  accumulatedFrames++;

  if (
    xSemaphoreTake(
      resultMutex,
      portMAX_DELAY
    ) == pdTRUE
  )
  {
    latestFFTError =
      fftError;

    xSemaphoreGive(
      resultMutex
    );
  }

  Serial.print(
    "[FFT] frame "
  );

  Serial.print(
    accumulatedFrames
  );

  Serial.print("/");

  Serial.print(
    FRAMES_PER_LEVEL
  );

  Serial.print(
    " | f_dom="
  );

  Serial.print(
    dominantFrequency,
    1
  );

  Serial.print(
    " Hz | error="
  );

  Serial.print(
    fftError,
    4
  );

  Serial.println(" dB");

  if (
    accumulatedFrames >=
    FRAMES_PER_LEVEL
  )
  {
    finishLevelMeasurement();
  }
}

// =====================================================
// AUDIO CAPTURE TASK
// =====================================================

void audioTask(void* parameter)
{
  while (true)
  {
    size_t bytesRead = 0;

    const esp_err_t result =
      i2s_read(
        I2S_PORT,
        i2sBuffer,
        sizeof(i2sBuffer),
        &bytesRead,
        portMAX_DELAY
      );

    if (
      result != ESP_OK
    )
    {
      Serial.println(
        "[AUDIO] I2S read error."
      );

      continue;
    }

    const int samplesReceived =
      bytesRead /
      sizeof(int32_t);

    for (
      int i = 0;
      i < samplesReceived;
      i++
    )
    {
      const int32_t pcm =
        i2sBuffer[i] >> 8;

      const double normalized =
        (double)pcm /
        PCM_FULL_SCALE;

      vReal[fftIndex] =
        normalized;

      vImag[fftIndex] =
        0.0;

      fftIndex++;

      if (
        fftIndex >= FFT_SIZE
      )
      {
        processFFTFrame();

        fftIndex = 0;
      }
    }
  }
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);
  delay(1500);

  Serial.println();

  Serial.println(
    "=========================================="
  );

  Serial.println(
    "OFFLINE INMP441 NOISE + ACOUSTIC LOGGER"
  );

  Serial.print(
    "Sample rate       : "
  );
  Serial.println(
    SAMPLE_RATE
  );

  Serial.print(
    "FFT size          : "
  );
  Serial.println(
    FFT_SIZE
  );

  Serial.print(
    "Frames per result : "
  );
  Serial.println(
    FRAMES_PER_LEVEL
  );

  Serial.print(
    "Base measurement  : "
  );
  Serial.print(
    LEVEL_DURATION_SECONDS,
    3
  );
  Serial.println(" s");

  Serial.print(
    "Log target        : "
  );
  Serial.print(
    LOG_INTERVAL_SECONDS,
    1
  );
  Serial.println(" s");

  Serial.print(
    "Results per log   : "
  );
  Serial.println(
    RESULTS_PER_LOG
  );

  Serial.print(
    "Actual log window : "
  );
  Serial.print(
    RESULTS_PER_LOG *
    LEVEL_DURATION_SECONDS,
    3
  );
  Serial.println(" s");

  Serial.print(
    "Calibration const.: "
  );
  Serial.println(
    CALIB_CONST,
    2
  );

  Serial.println(
    "=========================================="
  );

  resultMutex =
    xSemaphoreCreateMutex();

  if (
    resultMutex == NULL
  )
  {
    Serial.println(
      "ERROR: Mutex creation failed."
    );

    while (true)
    {
      delay(1000);
    }
  }

  setupI2S();
  setupAccessPoint();
  setupWebServer();

  xTaskCreatePinnedToCore(
    audioTask,
    "AudioTask",
    12288,
    NULL,
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    webServerTask,
    "WebServerTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  Serial.println(
    "System ready."
  );
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  delay(1000);
}
