# Stage 6 — CWP1-2 acoustic pilot

Stage 6 is an experimental acoustic condition-monitoring data collector for
CWP1-2. It deliberately does **not** diagnose faults or average the two sensor
locations into one room-noise value.

The two ESP32-S3 sensor nodes have different jobs:

- `cwp1-2-near`: fixed 10–20 cm from the motor drive-end bearing.
- `cwp1-2-reference`: a background/reference microphone that hears the plant
  room and nearby equipment but receives less direct sound from CWP1-2.

The master ESP32-S3 provides a private Wi-Fi network, gives both nodes the same
future capture window, receives per-capture acoustic features, and reports
near-minus-reference contrasts. A laptop joined to the same access point stores
the raw WAV files. Raw audio remains the evidence from which features and
models can be regenerated later.

## Architecture

```text
near-field INMP441 + ESP32-S3 ----\
                                   >-- master ESP32-S3 AP -- laptop collector
reference INMP441 + ESP32-S3 -----/       192.168.4.1       192.168.4.50
            synchronized 10 s capture        |                   |
            features -> master               |                   +-- WAV
            WAV ------------------------------+------------------>+-- metadata
```

The master is a coordinator and live status display, not the long-term data
store. It never averages the two locations. When both nodes report the same
`capture_id`, it calculates dB differences and band-power ratios that can help
identify common background changes.

## Per-capture measurements

Each node records 10 seconds at 32 kHz into PSRAM and reports:

- unweighted broadband dBFS;
- experimental estimated dB(Z) and estimated dB(A);
- peak dBFS, crest factor, kurtosis and zero-crossing rate;
- absolute dBFS power in 60–200 Hz, 200–1,000 Hz, 1–5 kHz and 5–15 kHz;
- the three largest separated spectral peaks;
- capture/session identifiers, sample count, clipping fraction and RSSI.

`estimated_dbz` is not standards-compliant metrology. The INMP441 response is
not flat below about 60 Hz or above about 15 kHz. For anomaly modelling on one
fixed node, `broadband_dbfs_unweighted` is normally the safer feature.

## Files

```text
stage-6/
├── README.md
├── master_node/
│   ├── master_node.ino
│   ├── config.h.example
│   └── platformio.ini
├── sensor_node/
│   ├── sensor_node.ino
│   ├── config.h.example
│   └── platformio.ini
└── collector/
    └── audio_receiver.py
```

## Configuration

Copy both examples before compiling:

```bash
cp stage-6/master_node/config.h.example stage-6/master_node/config.h
cp stage-6/sensor_node/config.h.example stage-6/sensor_node/config.h
```

Flash the sensor firmware twice. Change these values for the second board:

```cpp
#define NODE_ID "cwp1-2-reference"
#define NODE_NAME "CWP1-2 Background Reference"
#define MEASUREMENT_ROLE "reference"
```

Use independently determined calibration constants for the two complete
microphone/enclosure assemblies. The API key and Wi-Fi credentials must match
on the master, sensors and collector.

The sensor sketch requires an ESP32-S3 board with PSRAM. A 10-second,
32 kHz, 32-bit mono capture occupies 1,280,000 bytes.

## Laptop collector

Assign the laptop Wi-Fi interface the static address `192.168.4.50/24`, join the
master's access point, and run:

```bash
python3 stage-6/collector/audio_receiver.py \
  --output stage-6/captures \
  --api-key 'the-same-private-key'
```

The collector synchronizes the master's UTC clock every minute. Until that
happens, the master advertises `clock_set: false` and the sensors will not begin
scheduled captures. This prevents incorrectly timestamped WAV files.

The collector stores WAV files and compact capture-level metadata. It does not
create a multi-million-row PCM CSV. Analyse WAV files directly and store one
feature row per recording.

Open `http://192.168.4.1` to inspect schedule, node health and same-capture
near/reference contrasts.

## Initial pilot settings

The example configuration schedules a simultaneous 10-second recording every
60 seconds. Use this during commissioning. After placement and data delivery
are verified, change `CAPTURE_INTERVAL_MS` on the master to `300000UL` for a
five-minute baseline interval. The sensor obtains interval and duration from
the master; do not hard-code a different schedule on each node.

## Commissioning sequence

1. Place both nodes side by side and collect at least 20 captures. Their band
   levels should be repeatable; investigate persistent differences.
2. Fix the near node 10–20 cm from the CWP1-2 drive-end bearing without
   touching the machine or obstructing cooling.
3. Survey candidate reference positions while CWP1-2 is off and nearby
   equipment changes state.
4. Choose a reference position that tracks common plant-room changes but hears
   substantially less direct CWP1-2 sound than the near node.
5. Record target pump state, nearby equipment states, VSD percentage, motor
   current, flow, differential pressure and valve position for later joining.
6. Treat raw audio as potentially privacy-sensitive and follow the site's
   retention and access requirements.

## Important limits

- These scripts are experimental and must not initiate trips or maintenance.
- A shared acoustic rise is evidence of common sound, not proof of its source.
- Near-minus-reference values are log power ratios, not perfect source
  separation.
- Airborne sound is affected by reflections, airflow, people and microphone
  mounting.
- The master holds only the latest feature report. The collector is required
  for durable data.
- Lost uploads are logged but are not persisted on the sensor for retry.
