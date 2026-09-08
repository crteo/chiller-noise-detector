#pragma once

// Copy this file to config.h and fill in the values. config.h is git-ignored.
#define WIFI_SSID "SPD_MESH"
#define WIFI_PASSWORD "Fivespd1234!"

// Firebase Authentication user created specifically for this ESP32.
#define FIREBASE_API_KEY "AIzaSyCEvhwXiOmkB2dMYiSu2QLCs3zvQqcSuYM"
#define FIREBASE_EMAIL "noise-monitor-spd@example.com"
#define FIREBASE_PASSWORD "123456"

#define FIREBASE_URL "https://science-park-noise-monitoring-default-rtdb.asia-southeast1.firebasedatabase.app/current.json"
#define DEVICE_ID "chiller-room-1"

// Set true only after calibration against a reference sound-level meter.
#define CALIBRATION_ENABLED false
#define CALIBRATION_CONSTANT_DB 120.0f

// INMP441 L/R pin: GND selects left, 3V3 selects right.
#define MICROPHONE_RIGHT_CHANNEL false

