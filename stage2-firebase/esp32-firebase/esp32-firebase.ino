#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

const char* ssid = "SPD_MESH";
const char* password = "Fivespd1234!";

const char* firebaseURL =
  "https://science-park-noise-monitoring-default-rtdb.asia-southeast1.firebasedatabase.app/current.json";

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to Wi-Fi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi connected");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
}

void loop() {

  if (WiFi.status() == WL_CONNECTED) {

    // ----- Generate fake measurement -----
    float t = millis() / 1000.0;

    float dbfs =
      -30.0 + 5.0 * sin(2.0 * PI * 0.1 * t);

    float rms = pow(10.0, dbfs / 20.0);

    // ----- Build JSON -----
    String json = "{";
    json += "\"dbfs\":";
    json += String(dbfs, 2);
    json += ",";

    json += "\"rms\":";
    json += String(rms, 6);
    json += ",";

    json += "\"uptime_ms\":";
    json += String(millis());

    json += "}";

    Serial.println();
    Serial.println("Sending:");
    Serial.println(json);

    // ----- HTTPS connection -----
    WiFiClientSecure client;

    // Prototype only:
    // skips certificate verification
    client.setInsecure();

    HTTPClient https;

    if (https.begin(client, firebaseURL)) {

      https.addHeader("Content-Type", "application/json");

      int httpCode = https.PUT(json);

      Serial.print("HTTP response: ");
      Serial.println(httpCode);

      if (httpCode > 0) {
        String response = https.getString();
        Serial.println("Firebase response:");
        Serial.println(response);
      } else {
        Serial.println("Request failed");
      }

      https.end();
    }

  } else {

    Serial.println("Wi-Fi disconnected");
  }

  delay(5000);
}