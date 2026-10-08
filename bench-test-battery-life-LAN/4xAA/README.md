# Four-AA + LM2596 battery-life bench test

## Purpose

This test measures an ESP32-S3 + INMP441 node powered from four series AA
cells through an LM2596 adjusted to 3.30 V. A DS3231 wakes the sensor every ten
minutes. The node calculates dB values locally, transmits one compact JSON
message to the supplied master firmware, and returns to deep sleep. It never
creates or transmits a WAV file.

## Expected runtime

The planning model assumes approximately 75 mAh/day at the 3.3 V sensor rail,
80% conversion efficiency, a 5.5 V average pack voltage, and the LM2596's
roughly 5 mA operating quiescent current.

| Cells | Nominal capacity used | Expected bench runtime |
| --- | ---: | ---: |
| Four alkaline AA | 2,500 mAh | about 10-16 days |
| Four low-self-discharge NiMH AA | 2,000 mAh | about 8-13 days |
| Four lithium FeS2 AA | 3,500 mAh | about 15-22 days |

The LM2596's idle current dominates, so extending the report interval produces
only a modest improvement. Measure the real pack current and use
`tools/runtime_calculator.py` for the final estimate.

## Required equipment

- two ESP32-S3 DevKitC-compatible boards: one sensor and one mains/USB-powered
  master;
- one INMP441 breakout;
- one DS3231 module with a suitable backup coin cell;
- one four-AA holder and four matched cells;
- one adjustable LM2596 module;
- one 750 mA to 1 A fuse or resettable polyfuse;
- one 100 uF, 25 V electrolytic capacitor;
- one 10 kohm resistor for the DS3231 interrupt pull-up;
- breadboard/perfboard, short jumper wires and a multimeter; and
- a computer with PlatformIO. A current logger is strongly recommended.

The 25 V capacitor rating is safe at 3.3 V. The marked negative lead goes to
ground. It buffers short radio-current transients; it does not increase battery
capacity.

## Wiring

### Power

```text
AA holder + -> fuse -> LM2596 IN+
AA holder - --------> LM2596 IN-

LM2596 OUT+ -> ESP32 3V3, DS3231 VCC, INMP441 VDD
LM2596 OUT- -> ESP32 GND, DS3231 GND, INMP441 GND

100 uF capacitor + -> ESP32 3V3
100 uF capacitor - -> ESP32 GND
```

Adjust the disconnected LM2596 to 3.30 V before attaching the electronics.
Do not connect USB to the sensor while the LM2596 is driving its 3V3 pin.

### Signals

| Device signal | ESP32-S3 pin |
| --- | ---: |
| INMP441 BCLK | GPIO18 |
| INMP441 WS | GPIO16 |
| INMP441 SD | GPIO17 |
| INMP441 L/R | GND |
| DS3231 SDA | GPIO8 |
| DS3231 SCL | GPIO9 |
| DS3231 INT/SQW | GPIO4 |

Connect a 10 kohm pull-up from INT/SQW to 3V3 unless the RTC module already
provides one. The firmware expects the active-low alarm on GPIO4.

If the module contains a charging circuit, confirm that its coin cell is the
correct rechargeable type. Do not allow a module to charge a CR2032.

## Prepare the firmware

1. Copy `sensor_node/config.h.example` to `sensor_node/config.h`.
2. Copy `master_node/config.h.example` to `master_node/config.h`.
3. Put the same SSID, password and API key in both files.
4. Leave `AP_CHANNEL` set to `11` in both files. The firmware deliberately
   fails to compile if another channel is selected.
5. Set `SENSOR_CALIBRATION_DB` to the calibration offset for this complete
   microphone assembly. Leave it unchanged if only relative dB changes matter.
6. For the accelerated wake test, temporarily set
   `REPORT_INTERVAL_SECONDS` to `60UL`. Restore `600UL` for the endurance run.

The sensor applies the Stage 5b Hann-windowed FFT and A-weighting curve before
adding `SENSOR_CALIBRATION_DB`. It transmits `estimated_dba`,
`a_weighted_dbfs`, unweighted `peak_dbfs`, integration time, FFT-frame count
and dominant frequency. The calculation uses the existing 32 kHz, two-second
capture and remains prototype instrumentation until validated against a trusted
sound-level reference.

At 32 kHz, the two-second capture contains 64,000 samples. Fifteen complete
4,096-sample FFT frames contribute 1,920 ms to the reported dB(A); the remaining
partial frame is excluded from weighted integration. FFT processing increases
awake time slightly, so use the reported `awake_ms` and measured battery current
when refining the runtime estimate.

## Flash and run

Flash the master first:

```bash
cd bench-test-battery-life/4xAA/master_node
pio run --target upload
pio run --target uploadfs
pio device monitor
```

Flash the sensor while it is disconnected from the AA supply:

```bash
cd bench-test-battery-life/4xAA/sensor_node
pio run --target upload
```

Disconnect the sensor's USB cable, verify the LM2596 still reads 3.30 V, and
then switch on the AA supply. Keep the master connected to USB power.

## Record the results

Install the serial logger dependency:

```bash
python3 -m pip install -r tools/requirements.txt
```

Run the logger against the master's serial port:

```bash
python3 tools/serial_logger.py /dev/cu.usbmodemXXXX aa_measurements.csv
```

Join the computer or phone to the `BatteryBench` access point and open
`http://192.168.4.1/`. The Stage-5b-style local dashboard shows the latest
estimated dB(A), node metadata, radio signal, timing, accepted-message count and
a trend accumulated while that browser page is open. It requires no internet
connection or cloud hosting. Raw JSON remains available at
`http://192.168.4.1/status`.

The master and sensor are fixed to Wi-Fi channel 11. The sensor supplies this
known channel to `WiFi.begin()` so it can avoid a full scan on each wake.

## Test stages and acceptance criteria

### 1. Regulator stability

- Run several immediate measurements while watching the 3.3 V rail.
- Confirm there are no brownout resets when Wi-Fi starts.
- Confirm the LM2596 and cells do not become hot.

### 2. Accelerated alarm test

- Set a 60-second interval and run for two hours.
- Expect 120 messages, allowing one initial setup message.
- Confirm the sequence increases and RTC timestamps are monotonic.

### 3. Endurance test

- Restore the 600-second interval and run for 72 hours.
- Expect approximately 432 messages plus the initial setup message.
- Record start/end pack voltage, missed sequences, resets and average pack
  current.

The test passes when the alarm wakes reliably, no WAV traffic is present, the
3.3 V rail remains stable, and at least 99% of scheduled feature messages reach
the master.

## Runtime calculation

If the measured average at the AA holder is 7.3 mA with 2,500 mAh cells:

```bash
python3 tools/runtime_calculator.py \
  --capacity-mah 2500 \
  --average-current-ma 7.3 \
  --usable-fraction 0.85
```

Four cells in series remain a 2,500 mAh pack; their capacities do not add.
