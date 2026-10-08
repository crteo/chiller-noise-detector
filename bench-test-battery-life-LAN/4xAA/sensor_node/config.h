#pragma once

#define NODE_ID "bench-aa-node"
#define POWER_SOURCE_LABEL "4xAA-LM2596"

#define AP_SSID "BatteryBench"
#define AP_PASSWORD "replace-with-at-least-8-characters"
#define AP_CHANNEL 11
#define API_KEY "replace-with-a-private-shared-key"
#define MASTER_HOST "192.168.4.1"

#define REPORT_INTERVAL_SECONDS 600UL
#define CAPTURE_DURATION_MS 2000UL
#define SENSOR_CALIBRATION_DB 123.01

#define SENSOR_I2S_BCLK 18
#define SENSOR_I2S_LRCLK 16
#define SENSOR_I2S_DIN 17

#define RTC_SDA_PIN 8
#define RTC_SCL_PIN 9
#define RTC_INTERRUPT_PIN 4

#define WIFI_CONNECT_TIMEOUT_MS 8000UL
#define HTTP_TIMEOUT_MS 3000UL
