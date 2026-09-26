# Two-Sensor Offline Noise Monitor

This repository contains the implemented distributed monitor and retains the
original single-board prototype in `SPD_Noise_Monitor_Offline_Glass/` for
reference. The design decisions are recorded in
[`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md).

The target installation uses two ESP32-S3 + INMP441 sensor nodes and one
ESP32-S3 master. The master creates the local Wi-Fi network, receives both
measurements, calculates an equal-energy dBA average across the currently
connected nodes once per master-controlled one-second interval, and hosts the
dashboard for the tablet.

## Project structure

```text
.
├── sensor_node/
│   ├── sensor_node.ino
│   ├── config.h
│   ├── config.h.example
│   └── platformio.ini
├── master_node/
│   ├── master_node.ino
│   ├── config.h
│   ├── config.h.example
│   ├── platformio.ini
│   └── data/
│       ├── index.html
│       └── logo.png
└── SPD_Noise_Monitor_Offline_Glass/  # original prototype
```

## Hardware

- 3 × ESP32-S3 boards: two sensors and one master
- 2 × INMP441 I2S microphones
- Stable 3.3 V-compatible power and USB cables
- 1 × tablet with 2.4 GHz Wi-Fi

Do not power an INMP441 from 5 V.

### Sensor wiring

The current sketch defines the following wiring:

| INMP441 | ESP32-S3 |
|---|---:|
| SCK / BCLK | GPIO 18 |
| WS / LRCLK | GPIO 16 |
| SD / DOUT | GPIO 17 |
| L/R | GND (left channel) |
| VDD | 3.3 V |
| GND | GND |

The implemented sensor sketch uses GPIO 17, matching the original sketch.
Confirm these pins against the physical boards before uploading. The old
prototype README lists GPIO 36 for SD; update `SENSOR_I2S_DIN` if the physical
wiring actually uses that pin.

## Required software

- Arduino IDE 2.x
- Espressif ESP32 board package
- `arduinoFFT` from Arduino Library Manager (sensor sketches only)
- `ArduinoJson` 7.x from Arduino Library Manager (master sketch only)
- An ESP32 LittleFS data-upload tool for Arduino IDE

`WiFi`, `HTTPClient`, `WebServer`, `LittleFS`, and the I2S driver are supplied
by the ESP32 Arduino board package.

## Prepare configuration

Ready-to-run `config.h` files are included with the existing prototype's Wi-Fi
credentials. Before deployment:

1. Change the Wi-Fi password and API key in `master_node/config.h`.
2. Put the identical Wi-Fi password and API key in `sensor_node/config.h`.
3. Set `NODE_ID`, `NODE_NAME`, and `SENSOR_CALIBRATION_DB` for Sensor 1 and
   upload it.
4. Change those three values for Sensor 2 and upload it.

The `.example` files provide clean templates if configuration needs to be
reset.

The master aggregation cadence is configured here:

```cpp
#define AGGREGATION_INTERVAL_MS 1000UL
```

Keep it at 1000 ms for the intended one update per second.

Use a Wi-Fi password of at least eight characters. The system is offline, but
the access point should still use WPA2 and the sensor reporting endpoint should
check the shared API key.

## Upload with Arduino IDE

### 1. Upload Sensor 1

1. Connect only the first sensor ESP32-S3 by USB.
2. Open `sensor_node/sensor_node.ino`.
3. In **Tools**, select the exact ESP32-S3 board model and its serial port.
4. Confirm `config.h` contains `NODE_ID "sensor-1"` and Sensor 1's calibration.
5. Click **Upload**.
6. Open Serial Monitor at 115200 baud and confirm it repeatedly attempts to
   join `SPD_Noise_Monitor`. It cannot connect until the master is running.

### 2. Upload Sensor 2

1. Disconnect Sensor 1 and connect Sensor 2.
2. Change `NODE_ID` to `sensor-2`, `NODE_NAME` to `Sensor 2`, and use Sensor
   2's calibration constant.
3. Upload the same sensor sketch.
4. Open Serial Monitor at 115200 baud and verify the printed node ID is
   `sensor-2`.

Do not leave both boards configured with the same node ID; one will continually
overwrite the other's state on the master.

### 3. Upload the master firmware

1. Connect the master ESP32-S3 by USB.
2. Open `master_node/master_node.ino`.
3. Select the exact board model and serial port.
4. Choose a partition scheme with enough filesystem space for the contents of
   `master_node/data/`.
5. Click **Upload** to install the firmware.

### 4. Upload the master dashboard

Firmware upload and filesystem upload are separate operations.

1. Keep the master board connected and the `master_node` sketch open.
2. Run the ESP32 LittleFS data-upload command supplied by the installed upload
   tool. This uploads the complete `master_node/data/` folder.
3. Reset the master.
4. Open Serial Monitor at 115200 baud.

Expected master output includes:

```text
Network   : SPD_Noise_Monitor
Dashboard : http://192.168.4.1
Nodes     : sensor-1, sensor-2
```

If the browser reports that `index.html` is missing, repeat the LittleFS upload;
uploading firmware alone does not install dashboard files.

## PlatformIO upload

Each firmware folder contains its own `platformio.ini`. Change the `board`
setting if the hardware is not an ESP32-S3 DevKitC-1.

For each sensor, edit its identity/calibration and run:

```sh
cd sensor_node
pio run -e esp32-s3-sensor -t upload
pio device monitor -b 115200
```

For the master, run both firmware and filesystem uploads:

```sh
cd master_node
pio run -e esp32-s3-master -t upload
pio run -e esp32-s3-master -t uploadfs
pio device monitor -b 115200
```

## Start the system

Power-up order is not critical because the sensors retry automatically, but
this order makes commissioning easier:

1. Power the master and wait for `SPD NOISE MONITOR MASTER READY` in Serial
   Monitor.
2. Power Sensor 1 and verify the master logs an accepted `sensor-1` reading.
3. Power Sensor 2 and verify the master logs an accepted `sensor-2` reading.
4. On the tablet, join `SPD_Noise_Monitor` using the configured password.
5. Accept the tablet warning that the Wi-Fi network has no internet.
6. Browse to `http://192.168.4.1`.
7. Confirm both sensor rows show connected and the main panel changes from
   `--.-` to the overall dB(A) value.

The main reading is calculated across `N` connected nodes as:

```text
10 × log10((Σ 10^(sensor_dBA/10)) / N)
```

Disconnected nodes are excluded. With one connected node, the overall value is
the same as that node's value. With no connected nodes, the dashboard holds the
last displayed overall value and clearly labels it as held/offline; before the
first valid reading it displays `--.-`.

Sensor reports are not published directly to the dashboard. During each fixed
one-second master window, every node's dBA reports are converted to linear
energy and time-averaged. The master then averages those per-node energies and
publishes exactly one new `result_id`. Different sensor startup phases therefore
do not increase the dashboard update frequency.

## Runtime endpoints

| Method and address | Purpose |
|---|---|
| `GET http://192.168.4.1/` | Offline dashboard |
| `POST http://192.168.4.1/api/v1/readings` | Sensor-to-master report |
| `GET http://192.168.4.1/api/v1/status` | Aggregate and node-health JSON |

## Preliminary noise map

The dashboard opens with the overall sound level in the left 25% of the upper
row and the noise map in the remaining 75%. Individual sensor readings appear
in a full-width card below. Select **Trend** in the map card to inspect the
latest 60 one-second averages. The map uses the synchronized one-second reading
from each contributing sensor and performs inverse-distance interpolation in
linear acoustic-energy space. Chillers are drawn only as neutral floor-plan
context; they are not treated as sound sources. It is an indicative
visualization, not a validated acoustic simulation or regulatory exposure map.

The heat field requires at least two connected sensors that contributed to the
same one-second window. With zero or one contributor, the heat field is turned
off and the dashboard shows **Insufficient sensors available** below the
neutral floor plan. A single sensor is not used to invent a radial decay model.

All editable map inputs and assumptions are intentionally kept together in
`master_node/data/index.html` under `NOISE_MAP_CONFIG`. No `.h` file needs to
be changed. That block contains:

- Room dimensions: 52.3 m × 26.4 m
- Four chiller centre coordinates and footprints used for visual context only
- Sensor 1 at (11.1, 10.0) m
- Sensor 2 at (45.0, 10.0) m
- Assumed measurement height, interpolation power, sensor anchor radius,
  display range, and rendering resolution

After changing map geometry or assumptions, upload the `master_node/data/`
filesystem again. A master firmware upload is unnecessary unless the API or
firmware also changed.

## Commissioning checks

- Place both microphones side by side and expose them to the same steady source.
- Confirm each sensor reports independently and their calibrated levels are
  within the chosen tolerance.
- Stop Sensor 1; within two seconds the dashboard should show Sensor 1 offline
  and the overall value should equal Sensor 2.
- Restart Sensor 1; the system should recover without resetting the master.
- Repeat for Sensor 2.
- Stop both sensors; the last overall value should remain displayed, a held
  state should be visible, and the history should stop advancing.
- With test inputs of 70.0 dBA and 80.0 dBA, confirm the master returns 77.4
  dBA, not 75.0 dBA.
- Leave the complete system running for at least eight hours before deployment.

## Troubleshooting

### A sensor never appears on the master

- Confirm the node IDs are unique and exactly `sensor-1` and `sensor-2`.
- Confirm SSID, Wi-Fi password, and API key match the master configuration.
- Check that the master was powered and its access point started.
- Move the node closer during commissioning and inspect its 115200-baud log.

### Nodes connect but no overall value appears

- Check whether every reported node is older than the two-second freshness
  limit. One fresh node is sufficient for a live overall value.
- Confirm payloads contain finite `estimated_dba` values.
- Inspect the master log for rejected API keys, invalid IDs, old sequences, or
  invalid numeric values.

### Dashboard does not load

- Browse directly to `http://192.168.4.1`, not an HTTPS address.
- Keep the tablet on the no-internet Wi-Fi network.
- Upload the `master_node/data/` folder to LittleFS and reset the master.

### Readings disagree

- Confirm identical INMP441 orientation, mounting, and L/R wiring.
- Calibrate the two complete sensor assemblies independently.
- Verify the configured I2S data pin matches the actual wiring.

## Calibration note

Until both assembled sensors are calibrated against a suitable acoustic
calibrator or traceable reference, treat the values as estimated dBA. Do not
reuse one sensor's calibration constant for the other without verification.
