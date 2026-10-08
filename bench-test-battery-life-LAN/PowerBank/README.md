# 20,000 mAh power-bank battery-life bench test

## Purpose

This test determines whether a 20,000 mAh USB power bank can keep its output
enabled while an ESP32-S3 + INMP441 node spends most of its time in deep sleep.
A DS3231 wakes the sensor every ten minutes. The node calculates dB values
locally, sends one compact JSON message to the supplied master firmware, and
returns to deep sleep. It never creates or transmits a WAV file.

The power bank's advertised "trickle" or low-current mode must be an output
mode for wearables. Some banks automatically leave this mode after a fixed
time; that is a failed test even if the bank remains mostly charged.

## Expected runtime

A 20,000 mAh bank contains about 74 Wh at its internal cell voltage. Allowing
for conversion and cutoff leaves approximately 55-65 Wh usable. For the
defined two-second/ten-minute workload:

- conservative expectation: 4-6 weeks;
- likely range if low-current mode persists: 6-13 weeks;
- optimistic result with unusually low standby overhead: 3-4 months; and
- if USB output times out: the test ends at that timeout.

The bank's own converter overhead and the ESP32 development board's sleep
current are likely to dominate. Measure actual Wh consumption with an inline
USB meter and use `tools/runtime_calculator.py`.

## Required equipment

- two ESP32-S3 DevKitC-compatible boards: one sensor and one mains/USB-powered
  master;
- one INMP441 breakout;
- one DS3231 module with a suitable backup coin cell;
- one fully charged 20,000 mAh power bank with a documented low-current mode;
- one data-capable USB cable suitable for powering the sensor;
- one inline USB meter that accumulates Wh;
- one 100 uF, 25 V electrolytic capacitor;
- one 10 kohm resistor for the DS3231 interrupt pull-up;
- breadboard and short jumpers; and
- a computer with PlatformIO.

The 25 V capacitor rating is safe at 3.3 V. Its marked negative lead goes to
ground. It buffers short radio-current transients; it does not keep the power
bank awake.

## Wiring

### Power

```text
Power bank USB output -> inline USB Wh meter -> ESP32-S3 USB input

ESP32 3V3 -> DS3231 VCC and INMP441 VDD
ESP32 GND -> DS3231 GND and INMP441 GND

100 uF capacitor + -> ESP32 3V3
100 uF capacitor - -> ESP32 GND
```

Do not connect the capacitor across the DS3231 coin cell. The coin cell keeps
time if USB power disappears, but it cannot normally restart a power bank that
has disabled its USB output.

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

Connect a 10 kohm pull-up from INT/SQW to 3V3 unless the module already has
one. The firmware expects the active-low alarm on GPIO4.

If the module contains a charging circuit, confirm that its coin cell is the
correct rechargeable type. Do not allow a module to charge a CR2032.

## Prepare the firmware

1. Copy `sensor_node/config.h.example` to `sensor_node/config.h`.
2. Copy `master_node/config.h.example` to `master_node/config.h`.
3. Put the same SSID, password and API key in both files.
4. Leave `AP_CHANNEL` set to `11` in both files. The firmware deliberately
   fails to compile if another channel is selected.
5. Set `SENSOR_CALIBRATION_DB` to the calibration offset for this microphone
   assembly. Leave it unchanged if only relative changes matter.
6. For the accelerated test, temporarily change `REPORT_INTERVAL_SECONDS` to
   `60UL`. Restore `600UL` for the endurance run.

The sensor applies the Stage 5b Hann-windowed FFT and A-weighting curve before
adding `SENSOR_CALIBRATION_DB`. It transmits `estimated_dba`,
`a_weighted_dbfs`, unweighted `peak_dbfs`, integration time, FFT-frame count
and dominant frequency. The calculation uses the existing 32 kHz, two-second
capture and remains prototype instrumentation until validated against a trusted
sound-level reference.

At 32 kHz, the two-second capture contains 64,000 samples. Fifteen complete
4,096-sample FFT frames contribute 1,920 ms to the reported dB(A); the remaining
partial frame is excluded from weighted integration. FFT processing increases
awake time slightly, so use the reported `awake_ms` and measured USB energy when
refining the runtime estimate.

## Flash and run

Flash and monitor the master:

```bash
cd bench-test-battery-life/PowerBank/master_node
pio run --target upload
pio run --target uploadfs
pio device monitor
```

Flash the sensor from the computer:

```bash
cd bench-test-battery-life/PowerBank/sensor_node
pio run --target upload
```

Disconnect the sensor from the computer. Connect it through the inline USB Wh
meter to the fully charged power bank, then enable the bank's low-current mode
using its documented button sequence. Keep the master on independent USB
power.

## Record the results

Install the serial logger dependency:

```bash
python3 -m pip install -r tools/requirements.txt
```

Log the master's CSV rows:

```bash
python3 tools/serial_logger.py /dev/cu.usbmodemXXXX powerbank_measurements.csv
```

Join the computer or phone to the `BatteryBench` access point and open
`http://192.168.4.1/`. The Stage-5b-style local dashboard shows the latest
estimated dB(A), node metadata, radio signal, timing, accepted-message count and
a trend accumulated while that browser page is open. It requires no internet
connection or cloud hosting. Raw JSON remains available at
`http://192.168.4.1/status`.

The master and sensor are fixed to Wi-Fi channel 11. The sensor supplies this
known channel to `WiFi.begin()` so it can avoid a full scan on each wake.

Record the USB meter's cumulative Wh at the start and after 24, 48 and 72
hours. Do not rely solely on the bank's percentage LEDs.

## Test stages and acceptance criteria

### 1. Persistent-output test

- Use the 60-second interval for at least two hours.
- Continue for 24 hours even if the first two hours pass.
- Verify that the USB meter continues to show about 5 V.
- Verify that the sensor wakes without pressing the bank's button.

### 2. Accelerated alarm test

- Expect 120 messages in two hours, allowing one initial setup message.
- Confirm increasing sequence numbers and monotonic RTC timestamps.

### 3. Endurance test

- Restore the 600-second interval and run for 72 hours.
- Expect approximately 432 messages plus the initial setup message.
- Record Wh used, missed sequences, resets and any low-current-mode timeout.

The test passes when USB remains active for the full 72 hours, the sensor wakes
without manual intervention, no WAV traffic is present, and at least 99% of
scheduled feature messages reach the master.

## Runtime calculation

If the USB meter reports 3.0 Wh used after 72 hours, calculate the runtime from
an assumed 60 Wh usable bank energy:

```bash
python3 tools/runtime_calculator.py \
  --usable-wh 60 \
  --wh-used 3.0 \
  --test-hours 72
```

That example corresponds to 1 Wh/day and an estimated 60-day runtime.
