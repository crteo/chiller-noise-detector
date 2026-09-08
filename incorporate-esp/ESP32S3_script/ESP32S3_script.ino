#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

const char* plantSsid = "MaggieGuest";
const char* plantPassword = "Lai&PlayWithMe!";

const char* apSsid = "Chiller-Noise-Monitor";
const char* apPassword = "12345678";

WebServer server(80);

// --------------------------------
// Synthetic signal configuration
// --------------------------------

const float SAMPLE_RATE = 48000.0;
const float SIGNAL_FREQUENCY = 1000.0;
const int NUM_SAMPLES = 4096;

float signalAmplitude = 0.1;
float rmsValue = 0.0;
float dbfsValue = 0.0;

unsigned long lastUpdate = 0;


// --------------------------------
// Synthetic DSP
// --------------------------------

void calculateSyntheticSignal() {

  // Slowly vary the amplitude so the dashboard looks "live"
  float t = millis() / 1000.0;

  signalAmplitude =
    0.08 +
    0.04 * sin(2.0 * PI * 0.1 * t);

  double sumSquares = 0.0;

  for (int n = 0; n < NUM_SAMPLES; n++) {

    float sample =
      signalAmplitude *
      sin(
        2.0 * PI *
        SIGNAL_FREQUENCY *
        n /
        SAMPLE_RATE
      );

    sumSquares += sample * sample;
  }

  rmsValue = sqrt(sumSquares / NUM_SAMPLES);

  if (rmsValue > 0.0) {
    dbfsValue = 20.0 * log10(rmsValue);
  } else {
    dbfsValue = -120.0;
  }
}


// --------------------------------
// Dashboard page
// --------------------------------

void handleRoot() {

  String html = R"rawliteral(
<!DOCTYPE html>
<html>

<head>

<meta name="viewport" content="width=device-width, initial-scale=1">

<title>ESP32 Noise Monitor</title>

<style>

body {
  font-family: Arial, sans-serif;
  text-align: center;
  margin: 40px;
  background: #f5f5f5;
}

h1 {
  margin-bottom: 10px;
}

.subtitle {
  color: #666;
  margin-bottom: 30px;
}

.card {
  background: white;
  border-radius: 14px;
  padding: 25px;
  margin: 20px auto;
  max-width: 420px;
  box-shadow: 0 2px 8px rgba(0,0,0,0.1);
}

.value {
  font-size: 46px;
  font-weight: bold;
}

.small {
  color: #666;
}

</style>

</head>

<body>

<h1>ESP32 Noise Monitor</h1>

<div class="subtitle">
Synthetic 1 kHz test signal
</div>

<div class="card">
  <h2>RMS</h2>
  <div id="rms" class="value">—</div>
</div>

<div class="card">
  <h2>dBFS</h2>
  <div id="dbfs" class="value">—</div>
</div>

<div class="card">

  <h2>Signal</h2>

  <p>
    Amplitude:
    <span id="amplitude">—</span>
  </p>

  <p>
    Frequency:
    <span id="frequency">—</span> Hz
  </p>

  <p>
    Sample rate:
    <span id="sampleRate">—</span> Hz
  </p>

  <p class="small">
    Dashboard updates every 500 ms
  </p>

</div>


<script>

async function updateData() {

  try {

    const response = await fetch('/data');

    const data = await response.json();

    document.getElementById('rms').textContent =
      data.rms.toFixed(5);

    document.getElementById('dbfs').textContent =
      data.dbfs.toFixed(2) + ' dBFS';

    document.getElementById('amplitude').textContent =
      data.amplitude.toFixed(4);

    document.getElementById('frequency').textContent =
      data.frequency.toFixed(0);

    document.getElementById('sampleRate').textContent =
      data.sampleRate.toFixed(0);

  }

  catch (error) {

    console.log('Data update failed:', error);

  }

}

updateData();

setInterval(updateData, 500);

</script>


</body>

</html>
)rawliteral";

  server.send(200, "text/html", html);
}


// --------------------------------
// JSON data endpoint
// --------------------------------

void handleData() {

  String json = "{";

  json += "\"rms\":";
  json += String(rmsValue, 6);

  json += ",\"dbfs\":";
  json += String(dbfsValue, 3);

  json += ",\"amplitude\":";
  json += String(signalAmplitude, 6);

  json += ",\"frequency\":";
  json += String(SIGNAL_FREQUENCY, 1);

  json += ",\"sampleRate\":";
  json += String(SAMPLE_RATE, 1);

  json += "}";

  server.send(200, "application/json", json);
}


// --------------------------------
// Setup
// --------------------------------

void setup() {

  Serial.begin(115200);

  delay(1000);

  WiFi.mode(WIFI_AP_STA);


  // -------------------------
  // Connect to plant Wi-Fi
  // -------------------------

  WiFi.begin(plantSsid, plantPassword);

  Serial.print("Connecting to plant Wi-Fi");

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  Serial.println("Connected to plant Wi-Fi");

  Serial.print("Plant-side IP: ");
  Serial.println(WiFi.localIP());


  // -------------------------
  // Create ESP32 access point
  // -------------------------

  WiFi.softAP(apSsid, apPassword);

  Serial.print("ESP32 AP IP: ");
  Serial.println(WiFi.softAPIP());


  // -------------------------
  // Web server routes
  // -------------------------

  server.on("/", HTTP_GET, handleRoot);

  server.on("/data", HTTP_GET, handleData);

  server.begin();

  Serial.println("Web server started");

  Serial.println();
  Serial.println("Connect iPad to:");
  Serial.println(apSsid);

  Serial.println();

  Serial.print("Then open: http://");
  Serial.println(WiFi.softAPIP());
}


// --------------------------------
// Main loop
// --------------------------------

void loop() {

  server.handleClient();

  if (millis() - lastUpdate >= 100) {

    lastUpdate = millis();

    calculateSyntheticSignal();

  }
}