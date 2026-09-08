#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <arduinoFFT.h>
#include <math.h>

// ======================================================
// WIFI
// ======================================================

const char* ssid = "Teo Family";
const char* password = "XegKvUFFQsEexRu";

// ======================================================
// FIREBASE
// ======================================================

const char* firebaseURL =
  "https://science-park-noise-monitoring-default-rtdb.asia-southeast1.firebasedatabase.app/current.json";

// ======================================================
// INMP441 / I2S PINS
// Keep your existing working wiring
// ======================================================

#define I2S_BCLK   18
#define I2S_LRCLK  16
#define I2S_DIN    36

#define I2S_PORT I2S_NUM_0

// ======================================================
// AUDIO / FFT SETTINGS
// ======================================================

const uint32_t SAMPLE_RATE = 48000;

const uint16_t FFT_SIZE = 4096;

// 24-bit signed PCM full-scale
const double PCM_FULL_SCALE = 8388608.0;  // 2^23

// I2S block buffer
const int I2S_BUFFER_SIZE = 512;

int32_t i2sBuffer[I2S_BUFFER_SIZE];

// FFT arrays
double vReal[FFT_SIZE];
double vImag[FFT_SIZE];

// FFT object
ArduinoFFT<double> FFT(
  vReal,
  vImag,
  FFT_SIZE,
  SAMPLE_RATE
);

// ======================================================
// FFT FRAME STATE
// ======================================================

uint16_t fftIndex = 0;

// ======================================================
// SHARED RESULTS
// ======================================================

SemaphoreHandle_t resultMutex;

double latestDBFS = -120.0;
double latestDBFSFFT = -120.0;
double latestDBFSA = -120.0;

double latestRMS = 0.0;
double latestRMSA = 0.0;

double latestACorrection = 0.0;

double latestFFTError = 0.0;

double latestDominantFrequency = 0.0;

unsigned long latestFrameID = 0;

// ======================================================
// A-WEIGHTING EQUATION
// ======================================================
//
// Standard A-weighting relationship:
//
// RA(f) =
//      12194² f⁴
// ------------------------------------------------
// (f²+20.6²)
// × sqrt[(f²+107.7²)(f²+737.9²)]
// × (f²+12194²)
//
// A(f) = 20 log10(RA) + 2.00
//
// Returns A-weighting in dB.
//
// ======================================================

double calculateAWeightingDB(double f)
{
  if (f <= 0.0)
  {
    // DC should effectively contribute nothing
    return -200.0;
  }

  double f2 = f * f;

  double c1 = 20.6;
  double c2 = 107.7;
  double c3 = 737.9;
  double c4 = 12194.0;

  double c1_2 = c1 * c1;
  double c2_2 = c2 * c2;
  double c3_2 = c3 * c3;
  double c4_2 = c4 * c4;

  double numerator =
    c4_2 *
    f2 *
    f2;

  double denominator =
    (f2 + c1_2) *
    sqrt(
      (f2 + c2_2) *
      (f2 + c3_2)
    ) *
    (f2 + c4_2);

  double RA =
    numerator /
    denominator;

  if (RA <= 0.0)
  {
    return -200.0;
  }

  return
    20.0 * log10(RA)
    + 2.0;
}

// ======================================================
// CONVERT A-WEIGHTING dB TO ENERGY MULTIPLIER
// ======================================================
//
// A(f) describes amplitude weighting:
//
// amplitude gain = 10^(A/20)
//
// But FFT energy is |X|².
//
// Therefore:
//
// energy gain = 10^(A/10)
//
// ======================================================

double aWeightingPowerGain(double frequency)
{
  double A =
    calculateAWeightingDB(frequency);

  return
    pow(
      10.0,
      A / 10.0
    );
}

// ======================================================
// WI-FI
// ======================================================

void setupWiFi()
{
  Serial.println();
  Serial.print("Connecting to Wi-Fi");

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    ssid,
    password
  );

  while (
    WiFi.status() != WL_CONNECTED
  )
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi connected.");

  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
}

// ======================================================
// I2S
// ======================================================

void setupI2S()
{
  i2s_config_t i2s_config = {

    .mode =
      (i2s_mode_t)(
        I2S_MODE_MASTER |
        I2S_MODE_RX
      ),

    .sample_rate =
      SAMPLE_RATE,

    .bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT,

    // Assumes INMP441 L/R pin tied to GND
    .channel_format =
      I2S_CHANNEL_FMT_ONLY_LEFT,

    .communication_format =
      I2S_COMM_FORMAT_STAND_I2S,

    .intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1,

    // Larger DMA reserve because FFT processing
    // temporarily occupies CPU time.
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


  i2s_pin_config_t pin_config = {

    .bck_io_num =
      I2S_BCLK,

    .ws_io_num =
      I2S_LRCLK,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num =
      I2S_DIN
  };


  esp_err_t result;


  result =
    i2s_driver_install(
      I2S_PORT,
      &i2s_config,
      0,
      NULL
    );


  if (result != ESP_OK)
  {
    Serial.print(
      "I2S install error: "
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
      &pin_config
    );


  if (result != ESP_OK)
  {
    Serial.print(
      "I2S pin error: "
    );

    Serial.println(result);

    while (true)
    {
      delay(1000);
    }
  }


  Serial.println(
    "I2S initialized."
  );
}

// ======================================================
// PROCESS ONE 4096-SAMPLE FFT FRAME
// ======================================================

void processFFTFrame()
{
  // ====================================================
  // STEP 1 — REMOVE FRAME MEAN
  // ====================================================

  double mean = 0.0;


  for (
    uint16_t i = 0;
    i < FFT_SIZE;
    i++
  )
  {
    mean +=
      vReal[i];
  }


  mean /=
    (double)FFT_SIZE;


  for (
    uint16_t i = 0;
    i < FFT_SIZE;
    i++
  )
  {
    vReal[i] -=
      mean;
  }

  // ====================================================
  // STEP 2 — RAW TIME-DOMAIN RMS
  // ====================================================

  double rawSquareSum = 0.0;


  for (
    uint16_t i = 0;
    i < FFT_SIZE;
    i++
  )
  {
    rawSquareSum +=
      vReal[i] *
      vReal[i];
  }


  double rawRMS =
    sqrt(
      rawSquareSum /
      FFT_SIZE
    );


  double rawDBFS =
    -120.0;


  if (rawRMS > 1e-12)
  {
    rawDBFS =
      20.0 *
      log10(rawRMS);
  }

  // ====================================================
  // STEP 3 — APPLY HANN WINDOW MANUALLY
  // ====================================================
  //
  // w[n] =
  // 0.5 * (1 - cos(2πn/(N-1)))
  //
  // Also calculate:
  //
  // U = (1/N) Σw²
  //
  // For a large Hann window U ≈ 0.375
  //
  // ====================================================

  double windowSquareSum =
    0.0;

  double windowedSignalSquareSum =
    0.0;


  for (
    uint16_t i = 0;
    i < FFT_SIZE;
    i++
  )
  {
    double w =
      0.5 *
      (
        1.0 -
        cos(
          (2.0 * PI * i) /
          (FFT_SIZE - 1)
        )
      );


    vReal[i] *=
      w;


    vImag[i] =
      0.0;


    windowSquareSum +=
      w * w;


    windowedSignalSquareSum +=
      vReal[i] *
      vReal[i];
  }


  double U =
    windowSquareSum /
    FFT_SIZE;

  // ====================================================
  // STEP 4 — TIME-DOMAIN HANN-CORRECTED RMS
  // ====================================================
  //
  // This is our best apples-to-apples comparison
  // with the FFT reconstruction.
  //
  // ====================================================

  double timeCorrectedMeanSquare =
    windowedSignalSquareSum /
    (
      FFT_SIZE *
      U
    );


  double timeCorrectedRMS =
    sqrt(
      timeCorrectedMeanSquare
    );


  double timeCorrectedDBFS =
    -120.0;


  if (
    timeCorrectedRMS >
    1e-12
  )
  {
    timeCorrectedDBFS =
      20.0 *
      log10(
        timeCorrectedRMS
      );
  }

  // ====================================================
  // STEP 5 — FFT
  // ====================================================

  FFT.compute(
    FFTDirection::Forward
  );

  // IMPORTANT:
  //
  // DO NOT call complexToMagnitude().
  //
  // We need both real and imaginary values so that:
  //
  // |X[k]|² =
  // Re[k]² + Im[k]²
  //
  // ====================================================

  // ====================================================
  // STEP 6 — CALCULATE SPECTRAL ENERGY
  // ====================================================

  double unweightedSpectralEnergy =
    0.0;

  double weightedSpectralEnergy =
    0.0;


  double largestBinEnergy =
    0.0;

  uint16_t largestBin =
    0;


  for (
    uint16_t k = 0;
    k <= FFT_SIZE / 2;
    k++
  )
  {
    double re =
      vReal[k];

    double im =
      vImag[k];


    double magnitudeSquared =
      re * re +
      im * im;


    // ----------------------------------------------
    // One-sided FFT correction
    // ----------------------------------------------
    //
    // Positive and negative frequency bins contain
    // matching energy for a real-valued signal.
    //
    // Therefore all bins except:
    //
    // k = 0     DC
    // k = N/2   Nyquist
    //
    // must be doubled.
    //
    // ----------------------------------------------

    double oneSidedEnergy =
      magnitudeSquared;


    if (
      k > 0 &&
      k < FFT_SIZE / 2
    )
    {
      oneSidedEnergy *=
        2.0;
    }


    // ----------------------------------------------
    // Unweighted total energy
    // ----------------------------------------------

    unweightedSpectralEnergy +=
      oneSidedEnergy;


    // ----------------------------------------------
    // Frequency represented by this bin
    // ----------------------------------------------

    double frequency =
      (
        (double)k *
        SAMPLE_RATE
      ) /
      FFT_SIZE;


    // ----------------------------------------------
    // A-weighting
    // ----------------------------------------------

    if (k > 0)
    {
      double powerGain =
        aWeightingPowerGain(
          frequency
        );


      weightedSpectralEnergy +=
        oneSidedEnergy *
        powerGain;
    }


    // ----------------------------------------------
    // Dominant frequency
    // ----------------------------------------------

    if (
      k > 0 &&
      k < FFT_SIZE / 2
    )
    {
      if (
        oneSidedEnergy >
        largestBinEnergy
      )
      {
        largestBinEnergy =
          oneSidedEnergy;

        largestBin =
          k;
      }
    }
  }

  // ====================================================
  // STEP 7 — PARSEVAL / FFT NORMALIZATION
  // ====================================================
  //
  // ArduinoFFT performs an unnormalised forward DFT.
  //
  // Parseval:
  //
  // Σx² = (1/N) Σ|X|²
  //
  // Mean-square:
  //
  // MS = Σ|X|² / N²
  //
  // Because we applied a Hann window:
  //
  // MS corrected =
  // spectral energy / (N² × U)
  //
  // ====================================================

  double normalization =
    (
      (double)FFT_SIZE *
      (double)FFT_SIZE *
      U
    );


  double fftMeanSquare =
    unweightedSpectralEnergy /
    normalization;


  double weightedMeanSquare =
    weightedSpectralEnergy /
    normalization;


  if (fftMeanSquare < 0.0)
  {
    fftMeanSquare = 0.0;
  }


  if (weightedMeanSquare < 0.0)
  {
    weightedMeanSquare = 0.0;
  }


  double fftRMS =
    sqrt(
      fftMeanSquare
    );


  double rmsA =
    sqrt(
      weightedMeanSquare
    );

  // ====================================================
  // STEP 8 — CONVERT TO dBFS
  // ====================================================

  double fftDBFS =
    -120.0;


  if (fftRMS > 1e-12)
  {
    fftDBFS =
      20.0 *
      log10(
        fftRMS
      );
  }


  double dbfsA =
    -120.0;


  if (rmsA > 1e-12)
  {
    dbfsA =
      20.0 *
      log10(
        rmsA
      );
  }

  // ====================================================
  // STEP 9 — VALIDATION VALUES
  // ====================================================

  double fftError =
    fftDBFS -
    timeCorrectedDBFS;


  double aCorrection =
    dbfsA -
    fftDBFS;


  double dominantFrequency =
    (
      (double)largestBin *
      SAMPLE_RATE
    ) /
    FFT_SIZE;

  // ====================================================
  // SERIAL OUTPUT
  // ====================================================

  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    "        FFT A-WEIGHTING ANALYSIS"
  );

  Serial.println(
    "=========================================="
  );


  Serial.print(
    "Frame length             : "
  );

  Serial.print(
    (1000.0 * FFT_SIZE) /
    SAMPLE_RATE,
    2
  );

  Serial.println(
    " ms"
  );


  Serial.print(
    "FFT bin spacing          : "
  );

  Serial.print(
    (double)SAMPLE_RATE /
    FFT_SIZE,
    5
  );

  Serial.println(
    " Hz"
  );


  Serial.print(
    "Hann power factor U      : "
  );

  Serial.println(
    U,
    6
  );


  Serial.println(
    "------------------------------------------"
  );


  Serial.print(
    "Raw time-domain dBFS     : "
  );

  Serial.print(
    rawDBFS,
    2
  );

  Serial.println(
    " dBFS"
  );


  Serial.print(
    "Hann corrected time dBFS : "
  );

  Serial.print(
    timeCorrectedDBFS,
    2
  );

  Serial.println(
    " dBFS"
  );


  Serial.print(
    "FFT reconstructed dBFS   : "
  );

  Serial.print(
    fftDBFS,
    2
  );

  Serial.println(
    " dBFS"
  );


  Serial.print(
    "FFT validation error     : "
  );

  Serial.print(
    fftError,
    4
  );

  Serial.println(
    " dB"
  );


  Serial.println(
    "------------------------------------------"
  );


  Serial.print(
    "A-weighted dBFS          : "
  );

  Serial.print(
    dbfsA,
    2
  );

  Serial.println(
    " dBFS(A)"
  );


  Serial.print(
    "A-weighting difference   : "
  );

  Serial.print(
    aCorrection,
    2
  );

  Serial.println(
    " dB"
  );


  Serial.println(
    "------------------------------------------"
  );


  Serial.print(
    "Dominant frequency       : "
  );

  Serial.print(
    dominantFrequency,
    2
  );

  Serial.println(
    " Hz"
  );


  Serial.println(
    "=========================================="
  );

  // ====================================================
  // SHARE RESULTS WITH FIREBASE TASK
  // ====================================================

  if (
    xSemaphoreTake(
      resultMutex,
      portMAX_DELAY
    ) == pdTRUE
  )
  {
    latestDBFS =
      timeCorrectedDBFS;

    latestDBFSFFT =
      fftDBFS;

    latestDBFSA =
      dbfsA;

    latestRMS =
      fftRMS;

    latestRMSA =
      rmsA;

    latestACorrection =
      aCorrection;

    latestFFTError =
      fftError;

    latestDominantFrequency =
      dominantFrequency;

    latestFrameID++;

    xSemaphoreGive(
      resultMutex
    );
  }
}

// ======================================================
// AUDIO TASK
// ======================================================

void audioTask(
  void* parameter
)
{
  while (true)
  {
    size_t bytesRead =
      0;


    esp_err_t result =
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


    int samplesReceived =
      bytesRead /
      sizeof(int32_t);


    for (
      int i = 0;
      i < samplesReceived;
      i++
    )
    {
      // ----------------------------------------------
      // INMP441 sends 24 meaningful bits
      // inside the 32-bit I2S slot.
      // ----------------------------------------------

      int32_t pcm =
        i2sBuffer[i] >> 8;


      // Normalize approximately to:
      //
      // -1.0 ... +1.0

      double normalized =
        (double)pcm /
        PCM_FULL_SCALE;


      vReal[fftIndex] =
        normalized;


      vImag[fftIndex] =
        0.0;


      fftIndex++;


      // ----------------------------------------------
      // 4096 samples collected
      // ----------------------------------------------

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

// ======================================================
// FIREBASE UPLOAD
// ======================================================

void uploadFirebase(
  double dbfs,
  double fftDbfs,
  double dbfsA,
  double rms,
  double rmsA,
  double aCorrection,
  double fftError,
  double dominantFrequency
)
{
  if (
    WiFi.status() != WL_CONNECTED
  )
  {
    Serial.println(
      "[UPLOAD] Wi-Fi disconnected."
    );

    return;
  }


  WiFiClientSecure client;

  // Prototype only.
  client.setInsecure();


  HTTPClient https;


  if (
    !https.begin(
      client,
      firebaseURL
    )
  )
  {
    Serial.println(
      "[UPLOAD] HTTPS begin failed."
    );

    return;
  }


  https.addHeader(
    "Content-Type",
    "application/json"
  );


  String json =
    "{";


  json +=
    "\"dbfs\":" +
    String(dbfs, 2);


  json +=
    ",\"dbfs_fft\":" +
    String(fftDbfs, 2);


  json +=
    ",\"dbfsA\":" +
    String(dbfsA, 2);


  json +=
    ",\"rms\":" +
    String(rms, 8);


  json +=
    ",\"rmsA\":" +
    String(rmsA, 8);


  json +=
    ",\"a_correction\":" +
    String(aCorrection, 2);


  json +=
    ",\"fft_error_db\":" +
    String(fftError, 4);


  json +=
    ",\"dominant_frequency\":" +
    String(
      dominantFrequency,
      2
    );


  json +=
    ",\"fft_size\":" +
    String(FFT_SIZE);


  json +=
    ",\"sample_rate\":" +
    String(SAMPLE_RATE);


  json +=
    ",\"uptime_ms\":" +
    String(millis());


  json +=
    "}";


  unsigned long start =
    millis();


  int httpCode =
    https.PUT(
      json
    );


  unsigned long duration =
    millis() -
    start;


  Serial.print(
    "[UPLOAD] HTTP "
  );

  Serial.print(
    httpCode
  );


  Serial.print(
    " | "
  );

  Serial.print(
    duration
  );

  Serial.println(
    " ms"
  );


  https.end();
}

// ======================================================
// FIREBASE TASK
// ======================================================
//
// Upload roughly once per second.
//
// FFT/audio continues independently.
//
// ======================================================

void firebaseTask(
  void* parameter
)
{
  unsigned long previousUpload =
    0;

  unsigned long previousFrameID =
    0;


  while (true)
  {
    if (
      millis() -
      previousUpload >=
      1000
    )
    {
      double dbfs;
      double fftDbfs;
      double dbfsA;

      double rms;
      double rmsA;

      double aCorrection;
      double fftError;

      double dominantFrequency;

      unsigned long frameID;


      if (
        xSemaphoreTake(
          resultMutex,
          portMAX_DELAY
        ) == pdTRUE
      )
      {
        dbfs =
          latestDBFS;

        fftDbfs =
          latestDBFSFFT;

        dbfsA =
          latestDBFSA;

        rms =
          latestRMS;

        rmsA =
          latestRMSA;

        aCorrection =
          latestACorrection;

        fftError =
          latestFFTError;

        dominantFrequency =
          latestDominantFrequency;

        frameID =
          latestFrameID;


        xSemaphoreGive(
          resultMutex
        );
      }


      if (
        frameID != 0 &&
        frameID != previousFrameID
      )
      {
        uploadFirebase(
          dbfs,
          fftDbfs,
          dbfsA,
          rms,
          rmsA,
          aCorrection,
          fftError,
          dominantFrequency
        );


        previousFrameID =
          frameID;
      }


      previousUpload =
        millis();
    }


    vTaskDelay(
      pdMS_TO_TICKS(100)
    );
  }
}

// ======================================================
// SETUP
// ======================================================

void setup()
{
  Serial.begin(
    115200
  );


  delay(
    1500
  );


  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    "INMP441 FFT-BASED A-WEIGHTING"
  );

  Serial.println(
    "4096 FFT | 48 kHz"
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

  setupWiFi();


  // Audio: higher priority, Core 1

  xTaskCreatePinnedToCore(
    audioTask,
    "AudioTask",
    12288,
    NULL,
    2,
    NULL,
    1
  );


  // Firebase: lower priority, Core 0

  xTaskCreatePinnedToCore(
    firebaseTask,
    "FirebaseTask",
    12288,
    NULL,
    1,
    NULL,
    0
  );


  Serial.println();
  Serial.println(
    "System ready."
  );
}

// ======================================================
// LOOP
// ======================================================

void loop()
{
  delay(
    1000
  );
}