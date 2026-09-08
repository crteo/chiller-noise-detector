#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

// ===== CHANGE THESE TO MATCH YOUR WIRING =====
#define I2S_BCLK  18 //SCK
#define I2S_LRCLK 16 //WS
#define I2S_DIN   36 //SD
// ============================================
#define I2S_PORT I2S_NUM_0

const char* firebaseURL =
  "https://science-park-noise-monitoring-default-rtdb.asia-southeast1.firebasedatabase.app/current.json";

// ======================================================
// INMP441 / I2S
// ======================================================

// ======================================================
// AUDIO SETTINGS
// ======================================================

const int SAMPLE_RATE = 48000;
const int BLOCK_SIZE = 512;

// Exactly 1 second
const uint32_t SAMPLES_PER_MEASUREMENT = 48000;

// 24-bit signed PCM
const double PCM_FULL_SCALE = 8388608.0; // 2^23

int32_t i2sBuffer[BLOCK_SIZE];

// ======================================================
// BIQUAD FILTER
// ======================================================
//
// Difference equation:
//
// y[n] = b0*x[n]
//      + b1*x[n-1]
//      + b2*x[n-2]
//      - a1*y[n-1]
//      - a2*y[n-2]
//
// ======================================================

struct Biquad {

  double b0;
  double b1;
  double b2;

  double a1;
  double a2;

  double x1 = 0.0;
  double x2 = 0.0;

  double y1 = 0.0;
  double y2 = 0.0;

  double process(double x) {

    double y =
      b0 * x +
      b1 * x1 +
      b2 * x2 -
      a1 * y1 -
      a2 * y2;

    x2 = x1;
    x1 = x;

    y2 = y1;
    y1 = y;

    return y;
  }

  void reset() {

    x1 = 0.0;
    x2 = 0.0;

    y1 = 0.0;
    y2 = 0.0;
  }
};

// ======================================================
// A-WEIGHTING FILTER — 48 kHz
// ======================================================
//
// Three cascaded biquads.
//
// These coefficients implement an A-weighting response
// for 48 kHz sampling.
//
// ======================================================

Biquad aWeightStage1 = {

  0.234183,
  0.468366,
  0.234183,

 -0.224558,
  0.012607
};

Biquad aWeightStage2 = {

  1.000000,
 -2.000000,
  1.000000,

 -1.893870,
  0.895160
};

Biquad aWeightStage3 = {

  1.000000,
 -2.000000,
  1.000000,

 -1.994614,
  0.994622
};

// ======================================================
// APPLY A-WEIGHTING
// ======================================================

double applyAWeighting(double x) {

  double y;

  y = aWeightStage1.process(x);

  y = aWeightStage2.process(y);

  y = aWeightStage3.process(y);

  return y;
}

// ======================================================
// ACCUMULATORS
// ======================================================

// Number of samples accumulated
uint32_t accumulatorCount = 0;

// -------- Unweighted --------

double sumRaw = 0.0;
double sumSquaresRaw = 0.0;

double peakRaw = 0.0;

// -------- A-weighted --------

double sumSquaresA = 0.0;

double peakA = 0.0;

// ======================================================
// MEASUREMENT STRUCTURE
// ======================================================

struct MeasurementData {

  double mean;

  double rms;
  double dbfs;

  double rmsA;
  double dbfsA;

  double peak;
  double peakA;

  uint32_t sampleCount;

  unsigned long measurementTime;
};

MeasurementData latestMeasurement;

SemaphoreHandle_t measurementMutex;

bool measurementAvailable = false;

// ======================================================
// WIFI
// ======================================================

void setupWiFi() {

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    ssid,
    password
  );

  Serial.print("Connecting to Wi-Fi");

  while (
    WiFi.status() != WL_CONNECTED
  ) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  Serial.println("Wi-Fi connected.");

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// ======================================================
// I2S
// ======================================================

void setupI2S() {

  i2s_config_t i2s_config = {

    .mode = (i2s_mode_t)(
      I2S_MODE_MASTER |
      I2S_MODE_RX
    ),

    .sample_rate = SAMPLE_RATE,

    .bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT,

    // L/R pin tied to GND
    .channel_format =
      I2S_CHANNEL_FMT_ONLY_LEFT,

    .communication_format =
      I2S_COMM_FORMAT_STAND_I2S,

    .intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count = 8,

    .dma_buf_len = 256,

    .use_apll = false,

    .tx_desc_auto_clear = false,

    .fixed_mclk = 0
  };


  i2s_pin_config_t pin_config = {

    .bck_io_num = I2S_BCLK,

    .ws_io_num = I2S_LRCLK,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num =
      I2S_DIN
  };


  esp_err_t err;


  err = i2s_driver_install(
    I2S_PORT,
    &i2s_config,
    0,
    NULL
  );


  if (err != ESP_OK) {

    Serial.print(
      "I2S driver install failed: "
    );

    Serial.println(err);

    while (true) {
      delay(1000);
    }
  }


  err = i2s_set_pin(
    I2S_PORT,
    &pin_config
  );


  if (err != ESP_OK) {

    Serial.print(
      "I2S pin setup failed: "
    );

    Serial.println(err);

    while (true) {
      delay(1000);
    }
  }


  Serial.println(
    "I2S initialized."
  );
}

// ======================================================
// RESET ACCUMULATORS
// ======================================================

void resetAccumulator() {

  accumulatorCount = 0;

  sumRaw = 0.0;

  sumSquaresRaw = 0.0;

  sumSquaresA = 0.0;

  peakRaw = 0.0;

  peakA = 0.0;
}

// ======================================================
// FINISH 1-SECOND MEASUREMENT
// ======================================================

void finishMeasurement() {

  if (
    accumulatorCount == 0
  ) {

    return;
  }

  // ====================================================
  // UNWEIGHTED
  // ====================================================

  double mean =
    sumRaw /
    accumulatorCount;


  double meanSquare =
    sumSquaresRaw /
    accumulatorCount;


  // Remove DC contribution:
  //
  // variance = E[x²] - E[x]²

  double variance =
    meanSquare -
    mean * mean;


  if (
    variance < 0.0
  ) {

    variance = 0.0;
  }


  double rms =
    sqrt(variance);


  double dbfs =
    -INFINITY;


  if (
    rms > 0.0
  ) {

    dbfs =
      20.0 *
      log10(rms);
  }

  // ====================================================
  // A-WEIGHTED
  // ====================================================
  //
  // A-weighting already strongly suppresses DC and
  // very-low-frequency content.
  //
  // Therefore RMS can be calculated directly from
  // the filtered output energy.
  // ====================================================

  double meanSquareA =
    sumSquaresA /
    accumulatorCount;


  double rmsA =
    sqrt(meanSquareA);


  double dbfsA =
    -INFINITY;


  if (
    rmsA > 0.0
  ) {

    dbfsA =
      20.0 *
      log10(rmsA);
  }

  // ====================================================
  // CREATE RESULT
  // ====================================================

  MeasurementData measurement;


  measurement.mean =
    mean;


  measurement.rms =
    rms;


  measurement.dbfs =
    dbfs;


  measurement.rmsA =
    rmsA;


  measurement.dbfsA =
    dbfsA;


  measurement.peak =
    peakRaw;


  measurement.peakA =
    peakA;


  measurement.sampleCount =
    accumulatorCount;


  measurement.measurementTime =
    millis();

  // ====================================================
  // SHARE WITH FIREBASE TASK
  // ====================================================

  if (
    xSemaphoreTake(
      measurementMutex,
      portMAX_DELAY
    ) == pdTRUE
  ) {

    latestMeasurement =
      measurement;

    measurementAvailable =
      true;

    xSemaphoreGive(
      measurementMutex
    );
  }

  // ====================================================
  // SERIAL OUTPUT
  // ====================================================

  Serial.println();

  Serial.println(
    "========== 1 SECOND =========="
  );


  Serial.print(
    "Samples: "
  );

  Serial.println(
    measurement.sampleCount
  );


  Serial.print(
    "Mean: "
  );

  Serial.println(
    measurement.mean,
    8
  );


  Serial.print(
    "RMS: "
  );

  Serial.println(
    measurement.rms,
    8
  );


  Serial.print(
    "dBFS: "
  );

  Serial.print(
    measurement.dbfs,
    2
  );

  Serial.println(
    " dBFS"
  );


  Serial.print(
    "A-weighted RMS: "
  );

  Serial.println(
    measurement.rmsA,
    8
  );


  Serial.print(
    "dBFS(A): "
  );

  Serial.print(
    measurement.dbfsA,
    2
  );

  Serial.println(
    " dBFS(A)"
  );


  Serial.print(
    "A correction: "
  );

  Serial.print(
    measurement.dbfsA -
    measurement.dbfs,
    2
  );

  Serial.println(
    " dB"
  );


  Serial.print(
    "Peak: "
  );

  Serial.println(
    measurement.peak,
    6
  );


  Serial.println(
    "=============================="
  );
}

// ======================================================
// AUDIO TASK
// ======================================================

void audioTask(
  void* parameter
) {

  while (true) {

    size_t bytesRead = 0;


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
    ) {

      Serial.println(
        "[AUDIO] I2S read error"
      );

      continue;
    }


    int sampleCount =
      bytesRead /
      sizeof(int32_t);


    // ==================================================
    // PROCESS EVERY SAMPLE
    // ==================================================

    for (
      int i = 0;
      i < sampleCount;
      i++
    ) {

      // ------------------------------------------------
      // Extract INMP441 24-bit PCM
      // ------------------------------------------------

      int32_t pcm =
        i2sBuffer[i] >> 8;


      // ------------------------------------------------
      // Normalize to approximately [-1,+1]
      // ------------------------------------------------

      double x =
        ((double) pcm) /
        PCM_FULL_SCALE;


      // =================================================
      // PATH 1:
      // UNWEIGHTED
      // =================================================

      sumRaw +=
        x;


      sumSquaresRaw +=
        x * x;


      double absRaw =
        fabs(x);


      if (
        absRaw > peakRaw
      ) {

        peakRaw =
          absRaw;
      }


      // =================================================
      // PATH 2:
      // A-WEIGHTING FILTER
      // =================================================

      double weighted =
        applyAWeighting(x);


      sumSquaresA +=
        weighted *
        weighted;


      double absWeighted =
        fabs(weighted);


      if (
        absWeighted > peakA
      ) {

        peakA =
          absWeighted;
      }


      accumulatorCount++;


      // =================================================
      // EXACTLY 48000 SAMPLES
      // =================================================

      if (
        accumulatorCount >=
        SAMPLES_PER_MEASUREMENT
      ) {

        finishMeasurement();

        resetAccumulator();
      }
    }
  }
}

// ======================================================
// FIREBASE UPLOAD
// ======================================================

void uploadMeasurement(
  const MeasurementData &m
) {

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    Serial.println(
      "[UPLOAD] Wi-Fi disconnected."
    );

    return;
  }


  WiFiClientSecure client;

  // Prototype only!
  client.setInsecure();


  HTTPClient https;


  if (
    !https.begin(
      client,
      firebaseURL
    )
  ) {

    Serial.println(
      "[UPLOAD] HTTPS begin failed."
    );

    return;
  }


  https.addHeader(
    "Content-Type",
    "application/json"
  );

  // ====================================================
  // JSON
  // ====================================================

  String json = "{";


  json += "\"dbfs\":";
  json += String(
    m.dbfs,
    2
  );


  json += ",";


  json += "\"dbfsA\":";
  json += String(
    m.dbfsA,
    2
  );


  json += ",";


  json += "\"rms\":";
  json += String(
    m.rms,
    8
  );


  json += ",";


  json += "\"rmsA\":";
  json += String(
    m.rmsA,
    8
  );


  json += ",";


  json += "\"peak\":";
  json += String(
    m.peak,
    6
  );


  json += ",";


  json += "\"peakA\":";
  json += String(
    m.peakA,
    6
  );


  json += ",";


  json += "\"sample_count\":";
  json += String(
    m.sampleCount
  );


  json += ",";


  json += "\"sample_rate\":";
  json += String(
    SAMPLE_RATE
  );


  json += ",";


  json += "\"measurement_time_ms\":";
  json += String(
    m.measurementTime
  );


  json += ",";


  json += "\"uptime_ms\":";
  json += String(
    millis()
  );


  json += "}";


  Serial.println();

  Serial.print(
    "[UPLOAD] "
  );

  Serial.println(
    json
  );


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

void firebaseTask(
  void* parameter
) {

  unsigned long lastUploaded =
    0;


  while (true) {

    MeasurementData snapshot;

    bool newMeasurement =
      false;


    if (
      xSemaphoreTake(
        measurementMutex,
        portMAX_DELAY
      ) == pdTRUE
    ) {

      if (
        measurementAvailable &&
        latestMeasurement.measurementTime
          != lastUploaded
      ) {

        snapshot =
          latestMeasurement;

        newMeasurement =
          true;
      }


      xSemaphoreGive(
        measurementMutex
      );
    }


    if (
      newMeasurement
    ) {

      uploadMeasurement(
        snapshot
      );


      lastUploaded =
        snapshot.measurementTime;
    }


    vTaskDelay(
      pdMS_TO_TICKS(100)
    );
  }
}

// ======================================================
// SETUP
// ======================================================

void setup() {

  Serial.begin(
    115200
  );


  delay(
    1500
  );


  Serial.println();

  Serial.println(
    "===================================="
  );

  Serial.println(
    "INMP441 A-Weighted Noise Monitor"
  );

  Serial.println(
    "===================================="
  );


  setupI2S();

  setupWiFi();


  measurementMutex =
    xSemaphoreCreateMutex();


  if (
    measurementMutex == NULL
  ) {

    Serial.println(
      "Mutex creation failed."
    );

    while (true) {

      delay(
        1000
      );
    }
  }


  // ====================================================
  // AUDIO TASK
  // ====================================================

  xTaskCreatePinnedToCore(

    audioTask,

    "Audio Task",

    8192,

    NULL,

    2,

    NULL,

    1
  );


  // ====================================================
  // NETWORK TASK
  // ====================================================

  xTaskCreatePinnedToCore(

    firebaseTask,

    "Firebase Task",

    12288,

    NULL,

    1,

    NULL,

    0
  );


  Serial.println();

  Serial.println(
    "System running."
  );
}

// ======================================================
// LOOP
// ======================================================

void loop() {

  delay(
    1000
  );
}