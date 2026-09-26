# Two-Sensor Noise Monitor Implementation

The architecture in this document is implemented in `sensor_node/` and
`master_node/`. The phase list remains as a commissioning and verification
checklist.

## 1. Target system

Refactor the current single ESP32-S3 application into three devices:

```text
INMP441 + ESP32-S3 (sensor-1) --\
                                    >-- Master ESP32-S3 -- Wi-Fi --> Tablet
INMP441 + ESP32-S3 (sensor-2) --/        192.168.4.1
```

- The master creates the existing `SPD_Noise_Monitor` 2.4 GHz Wi-Fi network.
- Both sensor nodes join that network as Wi-Fi stations.
- The tablet also joins that network and opens `http://192.168.4.1`.
- Sensor nodes calculate calibrated A-weighted sound level locally and POST
  readings to the master.
- The master accepts readings only from known nodes, accumulates their linear
  A-weighted energy, and publishes one equal-energy average on a fixed
  one-second master-controlled interval.
- The master does not require an INMP441.

This retains the fully offline operation of the current project and avoids
sending high-rate PCM audio over Wi-Fi.

## 2. Implemented source layout

```text
noise-monitor/
├── README.md
├── sensor_node/
│   ├── sensor_node.ino
│   ├── config.h
│   └── config.h.example
└── master_node/
    ├── master_node.ino
    ├── config.h
    ├── config.h.example
    └── data/
        ├── index.html
        └── logo.png
```

Each sketch includes a ready-to-edit `config.h` and a clean
`config.h.example`. The two sensor firmwares remain identical except for
`NODE_ID`, `NODE_NAME`, and the per-microphone calibration constant.

Suggested sensor configuration:

```cpp
#define NODE_ID "sensor-1"       // change to sensor-2 on the second board
#define NODE_NAME "Sensor 1"
#define AP_SSID "SPD_Noise_Monitor"
#define AP_PASSWORD "replace-me"
#define API_KEY "replace-with-the-same-key-on-all-three-boards"
#define CALIB_CONST 123.01        // calibrate each microphone independently
```

## 3. Firmware responsibilities

### Sensor node firmware

Move these parts of the current sketch into `sensor_node.ino`:

- INMP441/I2S setup and capture task.
- FFT, Parseval normalization, A-weighting, spectrum calculation if it remains
  useful for diagnostics, and level integration.
- Per-node calibration.

Replace the access point, LittleFS, web server, and dashboard code with:

- Wi-Fi station connection to the master's access point.
- Automatic reconnect with bounded exponential backoff.
- An HTTP POST after every completed integration interval.
- A monotonically increasing `sequence` value and device `uptime_ms`.
- A small queue or single latest-value slot so audio capture never waits for
  an HTTP request.

The current integration period is:

```text
4096 samples/frame × 3 frames ÷ 48000 samples/second = 0.256 seconds
```

Keep that period initially so the refactor can be compared directly with the
existing monitor. Network transmission should run in a separate FreeRTOS task
from I2S capture and DSP.

Recommended POST body:

```json
{
  "node_id": "sensor-1",
  "boot_id": 3456789012,
  "sequence": 1234,
  "uptime_ms": 456789,
  "measured_at_ms": 456700,
  "estimated_dba": 73.42,
  "dbfs_a": -49.59,
  "integration_ms": 256,
  "sample_rate": 48000
}
```

Send `Content-Type: application/json` and the shared key in `X-API-Key`.
Reject non-finite levels before transmission.

### Master firmware

Retain and adapt these parts of the current sketch:

- SoftAP at `192.168.4.1`.
- LittleFS-hosted dashboard.
- `WebServer` task.

Remove I2S, FFT, and microphone processing from the master. Add:

- `POST /api/v1/readings` for sensor reports.
- A registry initially configured for `sensor-1` and `sensor-2`, implemented as
  a capacity-bounded array so more sensor nodes can be added later without
  rewriting the aggregation or API code.
- Validation of API key, node ID, numeric ranges, sequence order, and payload
  size.
- Receive timestamps based on the master's `millis()`; do not compare uptime
  clocks from different ESP32 boards.
- Per-node energy sums and sample counts for the current one-second window.
- A mutex protecting node state, window accumulators, and the latest aggregate.
- `GET /api/v1/status` (or retain `/data` as a compatibility alias) for the
  dashboard.
- Serial logging for joins, accepted/rejected messages, stale nodes, and new
  aggregate results.

The access point currently permits four stations, which is sufficient for two
sensors plus one tablet and one service slot. Increase that limit deliberately
when adding future sensors, while staying within the board and network's tested
capacity.

## 4. Correct decibel average

Decibels are logarithmic, so do not use an arithmetic mean. For equal spatial
weighting of `N` independently calibrated, currently connected A-weighted
levels:

```text
Lavg = 10 × log10((Σ 10^(Li/10)) / N)
```

Equivalent C++ for an array of current node levels:

```cpp
double averageDBA(const double levels[], size_t count)
{
  if (count == 0)
  {
    return NAN;
  }

  double energySum = 0.0;

  for (size_t i = 0; i < count; i++)
  {
    energySum += pow(10.0, levels[i] / 10.0);
  }

  return 10.0 * log10(energySum / count);
}
```

Examples:

| Sensor 1 | Sensor 2 | Correct overall average |
|---:|---:|---:|
| 70.0 dBA | 70.0 dBA | 70.0 dBA |
| 70.0 dBA | 80.0 dBA | 77.4 dBA |
| 60.0 dBA | 80.0 dBA | 77.0 dBA |
| Offline | 80.0 dBA | 80.0 dBA |

This is an equal-energy spatial average. It is not the combined sound level at
one point; that calculation would omit division by two and would be 3.01 dB
higher when both inputs are equal.

### Fixed-window aggregation, availability, and freshness rules

Use the following initial policy:

1. Store the newest valid reading from each node and convert every accepted
   dBA sample to linear energy using `10^(dBA/10)`.
2. Mark a node connected while its latest valid message is no more than 2,000
   ms old; otherwise mark it disconnected and exclude it from the calculation.
3. Accumulate energy and sample count independently for each node until the
   master's fixed one-second boundary. Sensor start times do not control the
   publication cadence.
4. At the boundary, calculate each contributing node's mean energy, then take
   an equal spatial mean across those node means and convert it back to dBA.
5. Consume and clear all window accumulators after every boundary; never reuse
   an old reading in a later window.
6. Increment `result_id` no more than once per completed one-second window.
7. With one contributing node, the overall value is that node's time-averaged
   value for the window.
8. With no new contributing samples, retain the last computed overall value
   and return it with `held: true`. With no connected nodes this is the normal
   offline hold behavior. If no valid value has ever been computed, return
   `has_value: false` and let the dashboard show `--.-`.
9. Do not increment `result_id` while holding a value.

This provides a useful degraded mode without presenting an offline node's old
reading as current. The dashboard must visibly distinguish a held value from a
live one even though its number remains on screen.

Recommended master response:

```json
{
  "has_value": true,
  "held": false,
  "result_id": 901,
  "overall_dba": 77.42,
  "aggregation": "equal_energy_mean",
  "aggregation_interval_ms": 1000,
  "aggregate_node_count": 2,
  "active_node_count": 2,
  "configured_node_count": 2,
  "nodes": [
    {"id":"sensor-1","online":true,"dba":70.00,"age_ms":82,"sequence":1234},
    {"id":"sensor-2","online":true,"dba":80.00,"age_ms":41,"sequence":1229}
  ],
  "connected_stations": 3,
  "uptime_ms": 987654
}
```

`softAPgetStationNum()` counts sensors and tablets together, so it should not
be presented as a tablet count unless the master tracks sensor MAC addresses
and subtracts them. Prefer the neutral label `connected_stations`.

When both sensors are disconnected after at least one valid aggregate, the
response keeps `overall_dba` and `result_id`, sets `held: true`, and sets
`active_node_count` to zero.

## 5. Dashboard changes

Retain the present dark glass theme, typography, color thresholds, local logo,
native canvas chart, and offline-only assets. Use exactly two top-level cards
in a responsive desktop layout:

- **Left card (approximately 68% width):** the main overall dBA value, live or
  held state, safety classification, and the aggregate history chart below it.
- **Right card (approximately 32% width):** an expandable sensor-node list.
  Initially it contains Sensor 1 and Sensor 2 followed by intentional empty
  space for additional nodes.

On narrow screens, stack the left card above the sensor card. Do not compress
the sensor rows until their labels or timestamps become difficult to read.

Change only the data semantics and connection detail:

- Rename the main label to **Overall sound level**.
- Read `overall_dba` rather than `estimated_dba`.
- Plot only newly incremented aggregate `result_id` values, producing at most
  one history point per second.
- Give each sensor its own row containing its name, latest dBA value, a green
  **Connected** dot/label or red **Disconnected** dot/label, and timing text.
- While connected, timing text reads `Last reading: just now` or an age such as
  `Last reading: 1.2 s ago`.
- On disconnect, freeze that sensor's last value, change its dot to red, and
  show `Last connected: <tablet-local date and time>`. The browser derives this
  from `age_ms` and stores the transition time locally because the offline
  master has no trustworthy wall clock.
- When one node disconnects, immediately recalculate from the remaining node
  and show `Averaging 1 of 2 sensors` beneath the main reading.
- When all nodes disconnect, keep the last displayed main value, stop adding
  history points, and show `Held value — all sensors disconnected` prominently.
- Before the first valid sensor report, show `--.-` and `Waiting for a sensor`.
- Keep the existing low/moderate/high thresholds unless the site's safety
  policy specifies different exposure limits.
- Persist each sensor's most recent disconnect/last-connected display time in
  browser local storage.

The dashboard should poll `/api/v1/status` at the existing 350 ms interval.
The master must send `Cache-Control: no-store`.

## 6. Refactor sequence

### Phase 1: Baseline and configuration

1. Record a short test set from the current single-node build.
2. Resolve the current pin discrepancy: the sketch uses GPIO 17 for I2S data,
   while its README says GPIO 36. Confirm the real wiring and use one value in
   code and documentation.
3. Extract all credentials, node identity, pins, and calibration constants into
   configuration headers.
4. Give each INMP441 its own calibration constant.

### Phase 2: Sensor firmware

1. Copy the verified I2S/FFT/A-weighting path into the sensor sketch.
2. Remove master-only LittleFS and access-point code.
3. Add station-mode reconnect logic and JSON reporting.
4. Verify that DSP continues during Wi-Fi loss and resumes reporting after a
   reconnect.

### Phase 3: Master firmware

1. Start from the current access-point/web-server code.
2. Remove all audio buffers and DSP dependencies.
3. Add the authenticated reading endpoint and capacity-bounded node registry.
4. Add freshness transitions, per-node window accumulators, a fixed one-second
   publication clock, and the energy-average function.
5. Expose aggregate and per-node health in the status endpoint.

### Phase 4: Dashboard

1. Preserve the current theme, typography, glass treatment, and color tokens.
2. Change the JSON field mapping and implement live, degraded, held, and
   never-received states.
3. Replace the old right-side history card with a sensor-status card; move the
   history chart into the bottom of the larger left card.
4. Build sensor rows from the `nodes` array so future nodes require
   configuration rather than new HTML.
5. Test landscape, portrait, refresh, and master reboot behavior.

### Phase 5: Validation

1. Unit-test the averaging function with the examples above.
2. Feed known synthetic pairs through the HTTP endpoint and confirm JSON and
   chart output.
3. Disconnect one sensor and confirm that its indicator changes within two
   seconds and the overall result equals the remaining sensor.
4. Reconnect it and confirm automatic recovery without rebooting the master.
5. Disconnect both sensors and confirm the last main value remains visible,
   carries a held/offline label, and is not appended to history again.
6. Reboot either sensor and confirm a reset sequence counter is accepted after
   the master detects the new boot/session.
7. Run both microphones side by side against the same reference source; verify
   their calibrated readings agree within the chosen tolerance.
8. Run the system for at least eight hours and inspect heap, rejected payloads,
   Wi-Fi reconnects, and result cadence.

## 7. Acceptance criteria

- The tablet connects only to the master's SSID and loads all assets offline.
- Both sensor nodes reconnect automatically after either side reboots.
- The displayed value matches the equal-energy formula within 0.1 dB.
- With one sensor connected, the overall value equals that sensor's value.
- With all sensors disconnected, the last overall value remains visible and is
  clearly marked as held rather than live.
- Sensor health is visible without changing the existing visual theme.
- The chart contains aggregated readings only and never duplicates a
  `result_id`.
- Audio capture is not blocked by HTTP or dashboard activity.
- Each sensor has an independently documented calibration constant.
- The firmware and LittleFS image can be uploaded using the README procedure.

## 8. Important measurement limitation

The existing `CALIB_CONST = 123.01` is an estimate, not proof of sound-level
meter accuracy. A credible dBA result requires each complete sensor assembly
to be calibrated against an appropriate acoustic calibrator or traceable
reference. Enclosure, microphone variation, mounting, reflections, and
placement can all change the reading. Label results as estimated dBA until
that calibration and validation are complete.
