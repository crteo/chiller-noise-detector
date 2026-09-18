# SPD Offline Noise Monitor

This project runs without internet access.

The ESP32-S3:

1. Reads the INMP441 over I2S.
2. Calculates dBFS, dBFS(A), estimated dBA and a real 32-band spectrum.
3. Creates the Wi-Fi network `SPD_Noise_Monitor`.
4. Hosts the dashboard from LittleFS.
5. Serves current measurements from `http://192.168.4.1/data`.

## Project structure

```text
SPD_Noise_Monitor_Offline/
├── SPD_Noise_Monitor_Offline.ino
├── README.md
└── data/
    └── index.html
```

The folder and `.ino` filename must remain identical for Arduino IDE.

## Required software

- Arduino IDE 2.x or PlatformIO
- Espressif ESP32 board package
- `arduinoFFT` library
- A LittleFS upload tool if using Arduino IDE

`WiFi`, `WebServer`, `LittleFS` and the I2S driver come from the ESP32 board package.

## Wiring assumed by the sketch

| INMP441 | ESP32-S3 |
|---|---:|
| SCK / BCLK | GPIO 18 |
| WS / LRCLK | GPIO 16 |
| SD / DOUT | GPIO 36 |
| L/R | GND for left channel |
| VDD | 3.3 V |
| GND | GND |

Do not power the INMP441 from 5 V.

## Arduino IDE upload sequence

1. Open `SPD_Noise_Monitor_Offline.ino`.
2. Select the correct ESP32-S3 board and port.
3. Install `arduinoFFT` through Library Manager.
4. Set a partition scheme that provides sufficient filesystem space.
5. Upload the Arduino sketch normally.
6. Use the installed LittleFS data-upload command to upload the `data` folder.
7. Open Serial Monitor at 115200 baud.

The sketch will print:

```text
Network  : SPD_Noise_Monitor
Dashboard: http://192.168.4.1
```

If the tablet shows an error saying `index.html is missing`, the firmware was uploaded but the LittleFS `data` folder was not.

## PlatformIO upload sequence

Use this project structure and configure LittleFS in `platformio.ini`:

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
board_build.filesystem = littlefs
lib_deps = kosme/arduinoFFT
monitor_speed = 115200
```

Then upload firmware and filesystem:

```text
pio run --target upload
pio run --target uploadfs
```

## Connect the tablet

1. Open Wi-Fi settings.
2. Join `SPD_Noise_Monitor`.
3. Enter `NoiseMonitor123`.
4. Accept the warning that the network has no internet.
5. Keep the tablet connected to this network.
6. Open `http://192.168.4.1` in the browser.

The ESP32-S3 uses 2.4 GHz Wi-Fi.

## Settings to change before deployment

Change the access-point password:

```cpp
const char* AP_PASSWORD = "NoiseMonitor123";
```

Confirm the I2S GPIO assignments match the actual wiring:

```cpp
#define I2S_BCLK  18
#define I2S_LRCLK 16
#define I2S_DIN   36
```

After calibration, update:

```cpp
const double CALIB_CONST = 123.01;
```

Until calibration is performed with a suitable reference, treat the dashboard result as estimated dBA.

## Local endpoints

| Address | Purpose |
|---|---|
| `http://192.168.4.1/` | Dashboard |
| `http://192.168.4.1/data` | Latest measurement JSON |

## Dashboard dependencies

The dashboard uses only HTML, CSS, JavaScript and native canvas. It does not load Firebase, Chart.js, fonts or other resources from the internet.

