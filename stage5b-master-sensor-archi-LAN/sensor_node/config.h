#pragma once

// Change these two values before uploading the second sensor board.
#define NODE_ID "sensor-1"
#define NODE_NAME "Sensor 1"

#define AP_SSID "SPD_Noise_Monitor"
#define AP_PASSWORD "NoiseMonitor123"
#define API_KEY "SPDNoiseNodes2026"
#define MASTER_HOST "192.168.4.1"

// Calibrate each complete microphone assembly independently.
#define SENSOR_CALIBRATION_DB 123.01

// INMP441 wiring. L/R is assumed to be connected to GND (left channel).
#define SENSOR_I2S_BCLK 18
#define SENSOR_I2S_LRCLK 16
#define SENSOR_I2S_DIN 17
