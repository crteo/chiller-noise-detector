#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

const char* apSSID = "SPD_Noise_Monitor";
const char* apPassword = "NoiseMonitor123";

WebServer server(80);

void handleHome()
{
  server.send(
    200,
    "text/html",
    R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">

  <meta
    name="viewport"
    content="width=device-width, initial-scale=1.0"
  >

  <title>ESP32 Noise Monitor</title>

  <style>
    body {
      margin: 0;
      font-family: Arial, sans-serif;
      background: #eef3f8;
      text-align: center;
      color: #17324d;
    }

    .card {
      width: min(600px, 85%);
      margin: 60px auto;
      padding: 40px;
      background: white;
      border-radius: 24px;
      box-shadow: 0 12px 30px rgba(0,0,0,0.1);
    }

    #level {
      font-size: 100px;
      font-weight: bold;
      color: #1479c9;
    }

    .unit {
      font-size: 32px;
    }

    #status {
      margin-top: 24px;
      font-size: 22px;
    }
  </style>
</head>

<body>
  <div class="card">
    <h1>1 SPD Chiller Plant Room</h1>

    <div>
      <span id="level">--.-</span>
      <span class="unit">dB(A)</span>
    </div>

    <div id="status">
      Waiting for ESP32 data
    </div>
  </div>

  <script>
    async function getMeasurement()
    {
      try
      {
        const response = await fetch(
          "/data",
          {
            cache: "no-store"
          }
        );

        if (!response.ok)
        {
          throw new Error(
            "HTTP " + response.status
          );
        }

        const data =
          await response.json();

        const level =
          Number(data.estimated_dba);

        if (!Number.isFinite(level))
        {
          throw new Error(
            "Invalid estimated_dba"
          );
        }

        document.getElementById(
          "level"
        ).textContent =
          level.toFixed(1);

        document.getElementById(
          "status"
        ).textContent =
          "Connected to ESP32";

      }
      catch (error)
      {
        document.getElementById(
          "status"
        ).textContent =
          "ESP32 data unavailable";
      }
    }

    getMeasurement();

    setInterval(
      getMeasurement,
      1000
    );
  </script>
</body>
</html>
    )rawliteral"
  );
}

void handleData()
{
  String json = "{";

  json += "\"estimated_dba\":";
  json += String(76.5, 1);

  json += ",\"dbfsA\":";
  json += String(-46.51, 2);

  json += ",\"dominant_frequency\":";
  json += String(996.1, 1);

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_AP);

  bool started = WiFi.softAP(
    apSSID,
    apPassword
  );

  if (!started)
  {
    Serial.println("Access point failed.");

    while (true)
    {
      delay(1000);
    }
  }

  Serial.println("Access point started.");

  Serial.print("Network name: ");
  Serial.println(apSSID);

  Serial.print("Dashboard address: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleHome);
  server.on("/data", handleData);

  server.begin();

  Serial.println("Web server started.");

  
}

void loop()
{
  server.handleClient();
  delay(2);
}