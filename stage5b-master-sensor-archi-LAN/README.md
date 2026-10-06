# Distributed Offline Noise Monitor

This repository contains an offline, two-point noise-monitoring system for the
1 SPD chiller plant room. Two ESP32-S3 sensor nodes periodically sample INMP441
digital microphones. A third ESP32-S3 acts as the Wi-Fi access point, validates
the sensor reports, calculates a combined result every ten seconds, and serves a
tablet dashboard from local flash storage.

This document is both an operating manual and an owner-level walkthrough of the
software. It explains what the current code does, why it does it, what the
display means, and what its scientific and engineering limits are.

> **The most important limitation:** this is a calibrated monitoring prototype,
> not a certified sound-level meter. Its `dB(A)` result is an estimate derived
> from an INMP441, FFT-based A-weighting, and a calibration offset. Use a
> traceably calibrated instrument for regulatory or occupational-safety
> decisions.

## Contents

- [The five-minute mental model](#the-five-minute-mental-model)
- [System architecture](#system-architecture)
- [Repository tour](#repository-tour)
- [Hardware and wiring](#hardware-and-wiring)
- [Configuration](#configuration)
- [Sensor firmware walkthrough](#sensor-firmware-walkthrough)
- [Master firmware walkthrough](#master-firmware-walkthrough)
- [Dashboard and noise map](#dashboard-and-noise-map)
- [Timing, concurrency, and duty cycling](#timing-concurrency-and-duty-cycling)
- [Network and API](#network-and-api)
- [Build, upload, and commissioning](#build-upload-and-commissioning)
- [Calibration and validation](#calibration-and-validation)
- [RF range and troubleshooting](#rf-range-and-troubleshooting)
- [Changing and extending the system](#changing-and-extending-the-system)
- [Security, storage, and failure behavior](#security-storage-and-failure-behavior)
- [Presentation guide and likely questions](#presentation-guide-and-likely-questions)
- [Owner's quick reference](#owners-quick-reference)

## The five-minute mental model

The system has three layers:

1. **Measure:** each sensor captures audio at 48 kHz during a five-second active
   window, divides it into 4,096-sample frames, applies a Hann window and FFT,
   applies A-weighting in the frequency domain, then enters five seconds of
   microphone standby. It produces one integrated report per ten-second cycle.
2. **Combine:** the master accepts authenticated HTTP reports from known nodes,
   tracks whether they are fresh, and publishes one equal-energy mean every ten
   seconds.
3. **Display:** a tablet joins the master's private Wi-Fi network and loads a
   dashboard from `http://192.168.4.1`. JavaScript polls the master and draws the
   current result, history, node states, and a preliminary spatial map.

The data path is:

```text
Sound
  -> INMP441 microphone
  -> I2S PCM samples
  -> sensor FFT + A-weighting + calibration
  -> HTTP POST over local 2.4 GHz Wi-Fi
  -> master validation + freshness tracking
  -> ten-second equal-energy aggregation
  -> JSON status API
  -> tablet dashboard
```

There is no cloud service, router, internet connection, database, or ESP-NOW in
the current design. The master itself creates the standard 802.11 B/G/N Wi-Fi
network. A tablet warning that the network has "no internet" is therefore
expected.

The main files are:

- [`sensor_node/sensor_node.ino`](sensor_node/sensor_node.ino): sampling, DSP,
  Wi-Fi client, and report transmission.
- [`master_node/master_node.ino`](master_node/master_node.ino): access point,
  validation, node state, aggregation, and web/API server.
- [`master_node/data/index.html`](master_node/data/index.html): complete offline
  dashboard, including CSS and JavaScript.
- [`master_node/data/map-config.json`](master_node/data/map-config.json): room,
  equipment, sensor coordinates, and map display settings.
- The two `config.h` files: device-specific identity, credentials, timing, and
  wiring. Their `.example` counterparts are safe templates.

## System architecture

```text
                    private Wi-Fi: SPD_Noise_Monitor

  Sensor 1 --------------------------------------------------+
  ESP32-S3 + INMP441                                        |
  48 kHz -> FFT -> dB(A) -> POST /api/v1/readings            |
                                                              v
                                                    +-------------------+
  Sensor 2 ---------------------------------------->| Master ESP32-S3   |
  ESP32-S3 + INMP441                                | 192.168.4.1       |
  48 kHz -> FFT -> dB(A) -> POST                    | AP + API + average|
                                                    | + LittleFS files  |
                                                    +---------+---------+
                                                              |
                                                              v
                                                    Tablet web browser
                                                    GET /api/v1/status
```

Each sensor is a Wi-Fi **station**. The master is the Wi-Fi **access point** and
HTTP server. Sensor reports travel directly to the master; the tablet is only a
viewer and does not participate in measurement or aggregation.

The current radio mode is ordinary B/G/N, not ESP-NOW and not Wi-Fi Long Range
(LR). B/G/N+LR was tested during development, but the master SSID stopped being
reliably discoverable by the tablet/clients on this board and Arduino stack, so
the production code was returned to interoperable B/G/N. RSSI reporting was
retained for range diagnosis.

## Repository tour

```text
.
├── README.md                         # this owner guide
├── IMPLEMENTATION_PLAN.md            # historical design plan
├── sensor_node/
│   ├── sensor_node.ino               # firmware used by both sensors
│   ├── config.h                      # real local sensor configuration
│   ├── config.h.example              # configuration template
│   └── platformio.ini
├── master_node/
│   ├── master_node.ino               # master firmware
│   ├── config.h                      # real local master configuration
│   ├── config.h.example              # configuration template
│   ├── platformio.ini
│   └── data/                         # uploaded separately to LittleFS
│       ├── index.html                # dashboard
│       ├── map-config.json           # editable room/map model
│       └── logo.png
```

`IMPLEMENTATION_PLAN.md` records earlier design intent, but the firmware and
data under `sensor_node/` and `master_node/` are authoritative for the current
runtime. When the plan and running code differ, document and operate the code as
it actually exists.

## Hardware and wiring

The intended installation uses:

- three ESP32-S3 boards: two sensor nodes and one master;
- two INMP441 I2S microphones;
- reliable USB power supplies and cables; and
- a tablet with 2.4 GHz Wi-Fi support.

The photographed boards are third-party ESP32-S3-N16R8 boards. `N16R8` means
16 MB flash and 8 MB PSRAM; it is not the module name `WROOM`. The current
PlatformIO target is the broadly compatible `esp32-s3-devkitc-1`. If upload,
flash size, USB, or PSRAM behavior differs, select the exact board definition
supported by the vendor.

### INMP441 wiring

The active sensor configuration uses:

| INMP441 pin | ESP32-S3 pin | Purpose |
|---|---:|---|
| SCK / BCLK | GPIO 18 | I2S bit clock |
| WS / LRCLK | GPIO 16 | I2S word/channel clock |
| SD / DOUT | GPIO 17 | microphone data into ESP32 |
| L/R | GND | selects the left I2S channel |
| VDD | 3.3 V | microphone power |
| GND | GND | common reference |

Do not power the INMP441 from 5 V. Keep the microphone wiring short, provide a
solid ground, and verify the physical wiring before changing pin definitions.

### Physical RF placement

The meandered copper section at the end of the ESP32 board is its PCB antenna.
For useful range:

- raise the node above the floor;
- keep the antenna end clear of metal enclosures, chillers, cable trays, the
  battery/power supply, and the human body;
- do not tape over or place a ground plane immediately behind the antenna;
- keep master and sensor antennas in similar orientation; and
- test at the actual installation height with doors and machinery in their
  normal states.

The observed improvement after lifting sensors from the floor is physically
reasonable. A conductive or reinforced floor detunes/absorbs the antenna, and
near-ground propagation suffers destructive reflections. Intermittent service
at 45–50 m inside a plant room is not surprising for small onboard antennas
among large metal machines.

## Configuration

Never put real passwords or API keys in documentation or screenshots. Use the
checked-in `.example` files to understand the required fields.

### Sensor configuration

[`sensor_node/config.h.example`](sensor_node/config.h.example) contains:

```cpp
#define NODE_ID "sensor-1"
#define NODE_NAME "Sensor 1"
#define AP_SSID "SPD_Noise_Monitor"
#define AP_PASSWORD "replace-with-at-least-8-characters"
#define API_KEY "replace-with-a-private-shared-key"
#define MASTER_HOST "192.168.4.1"
#define SENSOR_CALIBRATION_DB 123.01
#define SENSOR_I2S_BCLK 18
#define SENSOR_I2S_LRCLK 16
#define SENSOR_I2S_DIN 17
```

`NODE_ID` is the machine identity and must exactly match a node registered by
the master. Every physical sensor needs a unique ID. `NODE_NAME` is a human
label. `SENSOR_CALIBRATION_DB` must be determined independently for each full
board-and-microphone assembly; copying one sensor's value to another assumes
their sensitivities are identical.

### Master configuration

[`master_node/config.h.example`](master_node/config.h.example) defines the
network credentials, accepted node IDs/names, and timing:

```cpp
#define SENSOR_STALE_MS 15000UL
#define AGGREGATION_INTERVAL_MS 10000UL
#define AP_MAX_CONNECTIONS 8
#define AP_CHANNEL 1
```

- `SENSOR_STALE_MS`: a node becomes logically offline if the master has not
  accepted a report within this time. The current 15,000 ms value covers the
  deliberate ten-second sensor cycle with five seconds of timing margin.
- `AGGREGATION_INTERVAL_MS`: length of the master's output window. It is 10 s
  so each periodic sensor normally contributes one integrated sample.
- `AP_MAX_CONNECTIONS`: maximum associated Wi-Fi clients, including sensors and
  tablets. This is not the number of configured sensor nodes.
- `AP_CHANNEL`: fixed 2.4 GHz channel, restricted in code to 1–11. Test 1, 6,
  and 11 and choose the least congested one. Sensors automatically follow the
  access point's channel.

The checked-in example deliberately uses channel 1 as a neutral starting
point. The active `master_node/config.h` currently uses **channel 11**, which is
the channel that proved reliable in the present installation. Changing the
example file alone does not change a flashed master.

The SSID, password, and API key must agree between the master and every sensor.
The password must contain at least eight characters.

## Sensor firmware walkthrough

### 1. Sampling geometry

The key constants in [`sensor_node.ino`](sensor_node/sensor_node.ino) are:

```cpp
constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint16_t FFT_SIZE = 4096;
constexpr uint32_t CPU_FREQUENCY_MHZ = 160;
constexpr uint32_t SAMPLE_WINDOW_MS = 5000;
constexpr uint32_t REST_WINDOW_MS = 5000;
constexpr float PCM_FULL_SCALE = 8388608.0f;  // 2^23
```

At 48,000 samples/s, a 4,096-sample FFT frame lasts approximately 85.33 ms.
The bin spacing is:

```text
frequency resolution = 48,000 / 4,096 = 11.71875 Hz per bin
```

Only complete frames are added to a report. A five-second active window normally
contains about 58 complete frames, or approximately 4,949 ms of integrated
audio. The unfinished partial frame is discarded before standby, and the exact
completed-frame duration is sent as `integration_ms`.

The INMP441 supplies 24 meaningful signed bits in a 32-bit I2S container. The
code shifts by eight bits and normalizes against `2^23`:

```cpp
const int32_t pcm = i2sBuffer[i] >> 8;
vReal[fftIndex] = (float)pcm / PCM_FULL_SCALE;
```

This produces a full-scale-relative floating-point signal for DSP.

### 2. Periodic I2S capture

`setupI2S()` configures the ESP32 as I2S master and receiver, requests 32-bit
left-channel samples, assigns the configured pins, creates DMA buffers, and
stops the peripheral in standby. The audio task starts I2S for each measurement
window and blocks efficiently until DMA data is available:

```cpp
const esp_err_t result = i2s_read(
  I2S_PORT, i2sBuffer, sizeof(i2sBuffer), &bytesRead, portMAX_DELAY
);
```

`portMAX_DELAY` means the task yields the CPU while hardware is waiting for the
next DMA block. At the beginning of each active window, one 512-sample block is
discarded to provide about 10.7 ms for the INMP441 to leave standby. After five
seconds, I2S is stopped, the measurement is published, and the task delays for
five seconds. Stopping the serial clock places the INMP441 in standby. Audio
events during that rest window are not measured.

### 3. Windowing, FFT, and A-weighting

A finite FFT frame creates spectral leakage when a tone does not land exactly
on a frequency bin. A Hann window tapers both ends of the frame to reduce that
leakage:

```cpp
const float window =
  0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SIZE - 1)));
```

The window changes total signal energy, so `windowEnergyCorrection` compensates
for its mean-square gain. The window and all A-weighting gains are calculated
once during startup rather than recalculated for every frame.

The A-weighting function approximates the frequency sensitivity of human
hearing: low and very high frequencies contribute less than mid-band sound. For
each positive-frequency FFT bin, the code converts the A-weight value in dB to
a **power** multiplier:

```cpp
return powf(10.0f, calculateAWeightingDB(frequency) / 10.0f);
```

Using `/10` is correct for energy/power. `/20` would be used for an amplitude
ratio. `processFFTFrame()` removes the frame's DC mean, applies the window,
runs the FFT, builds a one-sided power spectrum, applies those gains, corrects
for window and FFT scaling, and accumulates weighted mean-square energy.

The reported `dominant_frequency` is the average location of the largest
**unweighted** spectral bin in each completed frame. It is a rough diagnostic,
not a precision tonal-analysis result: its resolution is about 11.72 Hz, noise
can make the maximum jump, and the average of bin peaks can hide changes.

### 4. From digital level to estimated dB(A)

At the end of each five-second active window, the audio task atomically takes
the completed-frame accumulator and resets it for the next report:

```cpp
const float rmsA = sqrtf(averageWeightedMS);
const float dbfsA = rmsA > 1e-12f ? 20.0f * log10f(rmsA) : -120.0f;
const float estimatedDBA = dbfsA + SENSOR_CALIBRATION_DB;
```

`dbfsA` is an A-weighted level relative to the microphone's digital full scale;
it is not yet sound pressure level. The calibration constant shifts this digital
reference to an estimated acoustic dB(A) reference. This simple offset assumes
the microphone chain remains acceptably linear across the frequencies and
levels of interest.

Values outside 0–160 dB(A), NaN, and infinity are rejected before networking.

### 5. Queue semantics favor current information

The queue has length one and is updated with `xQueueOverwrite()`:

```cpp
reportQueue = xQueueCreate(1, sizeof(MeasurementReport));
xQueueOverwrite(reportQueue, &report);
```

This is intentional. If networking is slow, the newest measurement replaces an
unsent old one. The monitor stays current and cannot build an ever-growing
backlog. The trade-off is that reports can be lost during an outage; there is no
historical replay and no application-level retry after a failed HTTP POST.

`boot_id` is randomly generated on every reboot, and `sequence` increases for
each report. The pair lets the master reject duplicates/out-of-order reports
while accepting sequence 1 after a legitimate sensor restart.

### 6. Network behavior and RSSI

The sensor joins as a standard station, reconnects with exponential delays from
1 to 10 seconds, and posts JSON to the master. Connect timeout is 750 ms and the
whole HTTP operation is limited to 1,200 ms. A successful report receives HTTP
`202 Accepted`.

Every report includes `wifi_rssi_dbm`, and the sensor also prints RSSI and
channel every five seconds. RSSI is a negative number: closer to zero is
stronger. Rough field guidance—not a guarantee—is:

| RSSI | Typical interpretation |
|---:|---|
| -30 to -50 dBm | excellent/strong |
| -50 to -67 dBm | generally reliable |
| -67 to -75 dBm | usable but reduced margin |
| -75 to -85 dBm | weak; loss and reconnects likely |
| below -85 dBm | often intermittent or unusable |

Packet loss, interference, antenna orientation, and noise floor matter in
addition to RSSI. Record RSSI together with success rate, not in isolation.

## Master firmware walkthrough

### 1. Access point and server

The master uses a fixed private network:

```cpp
IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);
WebServer server(80);
```

`setupAccessPoint()` starts a visible WPA-protected SoftAP on the configured
channel and explicitly enables B/G/N. Master Wi-Fi sleep is disabled so the
access point remains responsive. `setupWebServer()` mounts LittleFS, registers
the API/file routes, and starts port 80.

The master serves:

| Method and path | Purpose |
|---|---|
| `GET /` | dashboard HTML |
| `GET /map-config.json` | room/map configuration |
| `GET /logo.png` | dashboard logo |
| `POST /api/v1/readings` | authenticated sensor ingestion |
| `GET /api/v1/status` | current aggregate and node state |
| `GET /data` | compatibility alias for status |

### 2. What the master stores for each node

`NodeState` holds identity, latest measurement, boot/sequence numbers,
timestamps, RSSI, current-window energy, and whether the node contributed to
the last completed aggregate. State is in RAM only and resets when the master
reboots.

Although the array has capacity for eight nodes, `CONFIGURED_NODE_COUNT` is
currently two and only two entries are initialized. `AP_MAX_CONNECTIONS = 8`
does **not** automatically create six more sensor records.

### 3. Input validation

`handleReadingPost()` rejects reports in stages:

- missing/wrong API key: `401`;
- missing or oversized body: `400`/`413`;
- malformed JSON: `400`;
- unknown `node_id`: `403`;
- missing/wrong typed fields or invalid range: `422`;
- old sequence within the same boot: `409`;
- temporarily unavailable state mutex: `503`.

Only after validation does the master update the node and add its linear energy
to the current window. The payload's `node_name` does not control the displayed
identity; the master's configured name is authoritative.

### 4. “Online” means fresh application data

This is one of the most important semantics in the project:

```cpp
const bool isFresh = nodes[i].hasReading &&
  (uint32_t)(now - nodes[i].receivedAtMs) <= SENSOR_STALE_MS;
```

A node shown as disconnected has not delivered an **accepted report** recently.
It does not necessarily mean the ESP32 has disassociated from Wi-Fi. Possible
causes include weak RF, failed HTTP, wrong API key, unknown node ID, sensor DSP
failure, reboot, or a stale timeout that is too tight.

Conversely, `connected_stations` from `WiFi.softAPgetStationNum()` counts every
associated station—including the tablet—so it is not the sensor count. Use the
per-node `online` fields to assess reporting sensors.

### 5. Why dB values are converted to energy

Decibels are logarithmic and must not normally be averaged arithmetically. The
master first converts each report to a linear energy-like value:

```cpp
node.windowEnergySum += pow(10.0, dba / 10.0);
```

For each ten-second master window it:

1. averages all accepted reports from each node in linear space;
2. gives each contributing node one equal weight, even if one happened to send
   more reports during that window;
3. averages the contributing node energies; and
4. converts the result back with `10 * log10(...)`.

In equation form:

```text
node mean energy = mean(10^(each node report / 10))

overall dB(A) = 10 log10(
  sum(node mean energy for each contributing node) / contributing node count
)
```

For example, equal weighting of 70 and 80 dB(A) is approximately 77.4 dB(A),
not 75 dB(A), because the 80 dB location contains ten times the energy-like
quantity of the 70 dB location.

This combined number is best described as an **equal-energy spatial mean of the
active measurement locations**. It is not the acoustic sum of two microphones,
not room-wide exposure, and not proof that every point in the room has that
level. If only one node contributes, the result equals that node. Offline nodes
are excluded rather than treated as zero.

At the end of every window, samples are consumed and cleared; stale samples are
never reused in a later aggregate. If no node contributes, the master retains
the last result with `held: true`. It does not pretend that a held value is live.
`result_id` increments only for a genuinely new aggregate.

### 6. Shared-state protection

HTTP ingestion, status generation, freshness checks, and aggregation all touch
node state. A FreeRTOS mutex prevents one operation from reading half-written
state while another updates it. The master web task runs `server.handleClient()`
frequently, checks freshness every 100 ms, and publishes on a fixed ten-second
cadence. After a long delay it skips empty catch-up windows rather than emitting
a burst of artificial results.

## Dashboard and noise map

The dashboard is a single self-contained HTML file with inline CSS and
JavaScript. It requires no internet assets. It polls `/api/v1/status` every 350
ms with a 1.8 s request timeout. If no response arrives for 4 s, the browser
labels the master connection lost and preserves the last value as held.

### Current value and trend

The master determines the numerical aggregate. The browser only classifies it:

```javascript
if (v < 70) return "Low noise";
if (v < 80) return "Moderate noise";
return "High noise";
```

These thresholds and advice are project UI choices, not a substitute for an
applicable workplace-noise standard, which may depend on duration, dose,
impulsiveness, measurement position, and local law.

The history graph contains the latest 60 non-held `result_id` values in the
browser's memory. At the current ten-second aggregation interval, a full graph
therefore spans about ten minutes. It resets on page refresh and is not stored
on the ESP32.

Node “last connected” display times use browser `localStorage`; they are a UI
convenience, not an audit log.

### External map configuration and validation

Room changes no longer require editing dashboard JavaScript. Edit
[`map-config.json`](master_node/data/map-config.json), then upload LittleFS.
The browser validates schema version, units, positive dimensions, unique sensor
IDs, in-room coordinates, rectangles, display range, and interpolation values.
Invalid configuration disables the map and reports the problem in the browser
console instead of silently drawing nonsense.

Coordinates use metres from a **bottom-left origin**:

```json
{
  "room": { "widthM": 52.3, "heightM": 26.4 },
  "sensors": [
    { "id": "sensor-1", "name": "Sensor 1", "x": 9.3, "y": 18.64 },
    { "id": "sensor-2", "name": "Sensor 2", "x": 48.2, "y": 6.0 }
  ]
}
```

Equipment rectangles also use bottom-left `x`, `y`, then `widthM` and
`heightM`. Their IDs are for configuration integrity and their names are drawn
as labels.

### How the heat map is calculated

The map is available only when at least two nodes were online and contributed
to the same completed master window, and the aggregate is not held. For every
grid point, it uses inverse-distance weighting:

```javascript
weight = 1 / distance^sensorInterpolationPower;
estimatedEnergy = sum(weight * 10^(sensorDBA/10)) / sum(weight);
estimatedDBA = 10 * log10(estimatedEnergy);
```

Within `sensorAnchorRadiusM` of a sensor, it returns that sensor's value exactly.
`sensorInterpolationPower = 2` makes nearer measurements influence the estimate
more strongly. `gridPixelSize` controls drawing resolution/performance.
`displayMinDBA = 40` and `displayMaxDBA = 100` control the legend and color
scale; they do not clamp, filter, or change the sensor calculations.

The chiller rectangles are visual overlays only. The algorithm does not model
walls, obstruction, reflection, absorption, source directivity, machinery
sound power, airflow, or vertical variation. With only two sensors, many
different real sound fields could produce the same two measurements. Always
present it as a **preliminary interpolation between measured points**, not a
validated acoustic contour map.

### What `measurementHeightM` does

`measurementHeightM` currently affects **only the label shown above the map**.
It documents the intended/common measurement plane (currently 1.5 m above the
floor), but it is not used in distance, attenuation, FFT, aggregation, or map
interpolation calculations. All map mathematics is two-dimensional using `x`
and `y`.

This field is still valuable: measurements taken at inconsistent heights can
differ due to shielding, reflections, and source geometry. Treat it as an
installation rule and metadata. A genuine 3D model would require a `z` value
for sensors/sources and a revised interpolation or acoustic model.

## Timing, concurrency, and duty cycling

ESP32 Arduino runs on FreeRTOS. The project deliberately separates time-critical
audio from networking:

| Device | Task | Core | Priority | Responsibility |
|---|---|---:|---:|---|
| Sensor | `audioTask` | 1 | 2 | I2S capture and FFT |
| Sensor | `networkTask` | 0 | 1 | reconnect, RSSI, HTTP POST |
| Master | `webServerTask` | 0 | 1 | HTTP, freshness, aggregation |

The Arduino `loop()` functions merely delay because the explicit tasks do the
work.

The sensor now uses a 50% **measurement** duty cycle:

- sensor I2S/FFT runs for five seconds, then stops for five seconds;
- one result integrates the complete FFT frames from each active window;
- the INMP441 enters standby while its I2S clock is stopped;
- the sensor remains associated with Wi-Fi and publishes once per cycle;
- the master SoftAP and web server remain active;
- the master disables Wi-Fi sleep;
- sensors call `WiFi.setSleep(true)`, allowing modem power saving between radio
  activity; and
- the sensor CPU is explicitly set to 160 MHz.

This is not whole-board sleep or ESP32 deep sleep. During the five-second rest,
the I2S clock is stopped and the microphone enters standby, but the ESP32 CPU,
FreeRTOS scheduler, network task, and Wi-Fi association remain available. This
explains why the current does not fall to the microamp range and may look fairly
steady on a slowly updating USB power meter.

The current mode intentionally leaves five-second gaps in acoustic coverage. It
can miss short events that occur wholly during microphone standby. The two
sensors also start their cycles from their own boot times, so power them at
approximately the same time if synchronized map updates are important.

Sensor modem sleep can add latency or reduce marginal-link robustness, so
`WiFi.setSleep(false)` is a useful controlled A/B test during RF diagnosis. It
will increase power consumption and heat. It should not be confused with the
earlier hypothesis that the entire sensor was duty-cycling.

### Current power observation

The latest reported bench result after re-uploading the periodic-sampling
firmware was **17 mWh over 5 minutes**. That corresponds to approximately:

```text
average input power = 17 mWh / (5/60 h) ≈ 204 mW
equivalent current at 5 V = 204 mW / 5 V ≈ 41 mA
daily energy = 204 mW × 24 h ≈ 4.9 Wh/day
```

Treat this as a measured test result, not a firmware guarantee. Board regulator
losses, LEDs, USB circuitry, radio conditions, retransmission activity, supply
voltage, and the power meter all affect it. Earlier 70–80 mA observations show
why comparisons should use the same hardware, supply, placement, radio channel,
warm-up time, and a sufficiently long measurement interval. For each energy
experiment, record both the firmware version and total mWh over at least an
hour; an instantaneous current display can hide short Wi-Fi and sampling peaks.

## Network and API

### Example sensor report

The sensor sends a body shaped like:

```json
{
  "node_id": "sensor-1",
  "node_name": "Sensor 1",
  "boot_id": 123456789,
  "sequence": 42,
  "uptime_ms": 43000,
  "measured_at_ms": 42800,
  "estimated_dba": 76.321,
  "dbfs_a": -46.689,
  "dominant_frequency": 246.09,
  "integration_ms": 4949,
  "sample_rate": 48000,
  "wifi_rssi_dbm": -68
}
```

It also sends `Content-Type: application/json` and the shared `X-API-Key` HTTP
header. The maximum accepted body is 1,024 bytes.

### Example status interpretation

Open `http://192.168.4.1/api/v1/status` while connected to the master's Wi-Fi.
Important fields are:

- `has_value`: at least one aggregate has existed since master boot;
- `held`: the displayed aggregate has no new contributing data;
- `result_id`: monotonic ID for new completed aggregates;
- `overall_dba`: latest equal-energy spatial mean;
- `active_node_count`: nodes with fresh accepted reports now;
- `aggregate_node_count`: nodes that contributed to the last output window;
- `connected_stations`: all Wi-Fi clients, including the tablet;
- `nodes[].age_ms`: time since that node's last accepted report;
- `nodes[].wifi_rssi_dbm`: RSSI measured by the sensor when it posted;
- `nodes[].window_dba`: its energy mean in the last aggregate window;
- `nodes[].contributed`: whether it participated in that window.

`active_node_count` and `aggregate_node_count` can briefly differ at window
boundaries. That is expected: one describes freshness “now,” the other describes
the just-completed calculation.

## Build, upload, and commissioning

### Required software

- Arduino IDE 2.x with Espressif's ESP32 board package, or PlatformIO;
- `arduinoFFT` for sensor firmware;
- ArduinoJson 7.x for master firmware; and
- an ESP32 LittleFS upload tool if using Arduino IDE.

`WiFi`, `HTTPClient`, `WebServer`, `LittleFS`, and I2S are supplied by the ESP32
Arduino platform.

### Configure the two sensors

Both physical sensors run the same sketch, but each upload needs its own
`NODE_ID`, `NODE_NAME`, and calibration constant. A reliable sequence is:

1. Put Sensor 1's identity and calibration in `sensor_node/config.h` and upload.
2. Confirm its serial banner at 115200 baud.
3. Change those fields for Sensor 2 and upload to the second board.
4. Confirm the second banner says `sensor-2`.

If both boards use the same ID, they compete for one master record and their
sequence/boot behavior will appear erratic.

### Upload the master—and its filesystem

Firmware and dashboard data are separate uploads. Uploading only
`master_node.ino` does not update `index.html`, `map-config.json`, or the logo.
After firmware upload, upload the complete `master_node/data/` directory to
LittleFS, reset, and watch Serial Monitor at 115200 baud.

Expected startup includes:

```text
SPD NOISE MONITOR MASTER READY
Network   : SPD_Noise_Monitor
Radio     : B/G/N on channel 11
Dashboard : http://192.168.4.1
Nodes     : sensor-1, sensor-2
[WEB] Server started.
```

`LittleFS.begin(true)` currently requests automatic formatting if the
filesystem cannot mount. This helps recover corrupt/uninitialized flash, but it
can erase dashboard files; a subsequent filesystem upload may be required.

### PlatformIO commands

From each project folder:

```sh
cd sensor_node
pio run -e esp32-s3-sensor -t upload
pio device monitor -b 115200
```

```sh
cd master_node
pio run -e esp32-s3-master -t upload
pio run -e esp32-s3-master -t uploadfs
pio device monitor -b 115200
```

Use the board's **COM** USB-C port for the most straightforward serial monitor
and upload workflow. If necessary, hold BOOT while tapping RESET to enter the
bootloader. Exact behavior depends on the clone board's USB wiring.

### Connect the tablet

1. Power the master and wait for `[WEB] Server started.`.
2. In tablet Wi-Fi settings, join the configured SSID and enter its password.
3. Choose “stay connected” if the tablet warns there is no internet.
4. Disable automatic switching to mobile data/another Wi-Fi network if it keeps
   leaving the master.
5. Open exactly `http://192.168.4.1`—not HTTPS.

If the master prints that it is ready but the SSID is absent, reset it, scan
from another 2.4 GHz-capable device, verify the code says B/G/N rather than LR,
try channel 1/6/11, and check power quality. A successful `softAP()` print shows
software initialization, not proof that another device can receive the beacon.

### Commissioning checklist

1. Test all three boards together on a bench at 1–2 m.
2. Confirm both sensors receive `202` responses—absence of `[HTTP] Report
   failed`—and the master prints `[READING]` once per node per ten-second cycle.
3. Inspect `/api/v1/status`: both nodes online, increasing sequences, sensible
   ages, and RSSI present.
4. Compare both sensors side by side in a steady sound field; investigate a
   persistent difference before deployment.
5. Move one sensor to each final position at the planned 1.5 m height.
6. Log RSSI and accepted-report continuity at distance, with machinery running.
7. Confirm room and sensor coordinates against physical measurements.
8. Verify the dashboard after a master reboot, sensor reboot, one-node outage,
   and both-node outage.

## Calibration and validation

A calibration constant is not a universal property of the INMP441 model. It
belongs to the individual microphone, board, enclosure/opening, gain/scaling
pipeline, and reference method.

A practical field calibration procedure is:

1. Place the sensor microphone and a trusted calibrated sound-level meter close
   together, without one shielding the other.
2. Use a stable broadband sound field, ideally at several representative
   levels. A proper acoustic calibrator is better if mechanically compatible.
3. Allow readings to stabilize and compare like-for-like A-weighted,
   time-averaged values.
4. Adjust `SENSOR_CALIBRATION_DB` by the reference-minus-sensor difference.
5. Repeat at low, medium, and high levels and for both nodes independently.
6. Document date, reference instrument, geometry, environment, firmware commit,
   and resulting constants.

If the error changes substantially with sound level or frequency, a single
offset is insufficient. Potential causes include microphone response,
clipping, enclosure effects, wiring noise, algorithm normalization, or an
inappropriate reference procedure.

Check for clipping near high levels: normalized PCM repeatedly near ±1 means
the digital microphone is saturating, and no calibration offset can recover the
lost waveform. Also validate the noise floor in a quiet environment.

## RF range and troubleshooting

There is no single guaranteed “ESP32 Wi-Fi range.” Link budget depends on both
ends' transmit power, antenna efficiency/orientation, receiver sensitivity,
data rate, interference, obstructions, reflections, and mounting. Metal-filled
plant rooms are a difficult multipath environment.

### A useful range test

At 1 m, 10 m, 25 m, and the intended 45–50 m position:

1. keep both boards at their intended height and orientation;
2. record sensor serial RSSI every five seconds for at least two minutes;
3. record master sequence numbers and node `age_ms`;
4. count missed seconds/reconnects, not just whether it eventually works;
5. repeat on channels 1, 6, and 11 under comparable conditions;
6. repeat with sensor `WiFi.setSleep(false)` as a controlled experiment; and
7. note doors, people, machinery, and line-of-sight conditions.

Choose the channel/placement with the best sustained delivery, not merely the
highest single RSSI sample.

### Symptom-based diagnosis

**Works beside master but fails at distance:** identity/API/DSP are probably
functional; prioritize antenna clearance, height, power quality, channel
congestion, RSSI, and physical obstructions.

**Strong RSSI but HTTP failures:** inspect the HTTP response code. `401` means
API-key mismatch, `403` unknown node, `409` duplicate/old sequence, `422` bad
payload/value, and negative/timeout errors suggest transport loss.

**Dashboard says disconnected but Wi-Fi client count remains high:** the station
may still be associated but not delivering accepted reports, or the counted
station may be the tablet. Inspect `age_ms`, sequence, sensor serial, and master
`[READING]` logs.

**Node becomes offline during its planned rest:** confirm the master was flashed
with `SENSOR_STALE_MS = 15000` and that the complete sensor cycle remains close
to ten seconds. A missed cycle will still make the node stale shortly afterward.

**No sensor level output at all:** verify microphone power/wiring and I2S startup
messages. Network problems do not stop `[LEVEL]` serial output because DSP is a
separate task.

**Map unavailable while both rows look connected:** map generation requires two
nodes that both contributed to the same completed aggregation window. Inspect
`contributed` and `window_dba` in the status API.

### When to consider a radio redesign

Try placement, antennas, channel selection, power, and measured diagnostics
first because they preserve tablet compatibility and the simple architecture.
If the required installation still cannot achieve acceptable packet delivery,
options include an ESP32 board with a certified external antenna, relocating
the master, adding infrastructure, or using ESP-NOW LR for the sensor-to-master
leg while keeping a standard B/G/N SoftAP for the tablet.

ESP-NOW LR is not a one-line mode switch for the whole system. It would replace
HTTP sensor reports with ESP-NOW packets, require peer/channel management,
acknowledgement/retry design and compatible LR settings on sensor and master,
while the master would still need standard Wi-Fi for the tablet. It may improve
link margin at lower data rates, but it cannot fix a blocked/detuned antenna.

## Changing and extending the system

### Change room dimensions or object positions

Edit only `master_node/data/map-config.json`, validate the JSON, upload the
LittleFS filesystem, and reload the tablet page. Coordinates must remain inside
the room. Firmware upload alone will not update the map.

### Change map colors or range

Set `displayMinDBA` and `displayMaxDBA` in the JSON. Current values are 40 and
100. They affect visualization only. The classification breakpoints (70 and 80)
and color stops are presently in `index.html`, so changing the safety/status
logic still requires editing the dashboard.

### Add another sensor

This currently requires code, configuration, and map changes:

1. add ID/name macros to `master_node/config.h` and its example;
2. add another initialized `NodeState` entry;
3. increase `CONFIGURED_NODE_COUNT` without exceeding `MAX_NODES`;
4. flash a sensor with the matching unique `NODE_ID` and its calibration;
5. add its coordinates to `map-config.json`; and
6. ensure `AP_MAX_CONNECTIONS` covers every sensor plus tablet(s).

The aggregation loop and API JSON already iterate over the configured count.
The dashboard node list is dynamic. The noise map will use every synchronized
configured anchor it receives.

### Change the periodic sampling cycle

`SAMPLE_WINDOW_MS` and `REST_WINDOW_MS` are sensor source constants; aggregation
interval and stale timeout live in master configuration. Change them as a
coordinated set. Longer sampling improves acoustic coverage and integration;
longer rest saves more energy but creates larger blind periods. The master
aggregation interval should normally match the complete cycle, and the stale
timeout should be longer than that cycle with allowance for Wi-Fi/HTTP jitter.

### Improve data retention

The cleanest next feature would be append-only logging on the master (with
storage limits and corruption/wear strategy) or export to an external system.
Define timestamps first: the devices currently have only milliseconds since
boot and no real-time clock, NTP, or internet time source.

## Security, storage, and failure behavior

- WPA protects the local radio network if the password is strong.
- The API key restricts casual/accidental writes but is a shared secret.
- HTTP is unencrypted. Anyone who joins the WLAN can potentially observe local
  traffic and access the status/dashboard; this is not enterprise security.
- Credentials compiled into firmware can be recovered by a determined person
  with physical access.
- There is no user authentication on the dashboard or status endpoint.
- No measurement history is persisted by the master or sensors.
- Sensor outage data is dropped rather than replayed.
- Master reboot clears node state, aggregate state, and sequence knowledge.
- Sensor reboot changes `boot_id`, allowing its sequence counter to restart.
- Browser refresh clears the 60-point chart but may preserve “last connected”
  timestamps in that browser's local storage.
- `millis()` timestamps wrap after about 49.7 days; the subtraction patterns in
  freshness/timers are deliberately unsigned/wrap-safe for intervals far below
  half that range.

## Presentation guide and likely questions

### A clear two-minute explanation

> “The system uses two ESP32-S3 nodes with digital I2S microphones to sample
> at 48 kHz during five-second measurement windows. Each sensor performs an FFT,
> applies frequency-domain A-weighting, integrates complete frames, then applies its
> calibration offset. It sends the result over a private Wi-Fi network to a
> third ESP32-S3. The master validates identity and sequence, rejects stale or
> malformed data, and computes an equal-energy mean once per ten-second cycle. It also
> hosts the entire offline dashboard for a tablet. The trend is live browser
> memory, while the map is a preliminary inverse-distance interpolation of
> synchronized sensor measurements—not an acoustic simulation or certified
> compliance measurement.”

### Design choices worth defending

**Why three ESP32 boards?** Two sensing locations give spatial information, and
the dedicated master supplies a stable offline network, centralized timing,
validation, aggregation, and UI without requiring internet infrastructure.

**Why I2S microphones?** Digital samples avoid the ESP32 ADC and analog signal
path. They still require per-assembly calibration and careful acoustic mounting.

**Why 48 kHz and a 4,096-point FFT?** 48 kHz covers the normal audio band; 4,096
samples give about 11.72 Hz bin spacing and an 85 ms frame, a workable balance
of frequency resolution, latency, memory, and computation.

**Why a Hann window?** It reduces spectral leakage at frame boundaries. The code
corrects the corresponding energy loss.

**Why A-weighting?** It approximates human hearing sensitivity and is common in
environmental/occupational sound reporting. It is applied as frequency-dependent
power weighting before RMS/decibel conversion.

**Why not average dB numbers directly?** Decibels are logarithmic. Conversion to
linear energy prevents mathematically invalid arithmetic averaging.

**Why average nodes rather than add them?** The nodes measure the same environment
at different positions; adding their readings would falsely treat microphones
as independent sources. Equal-energy averaging creates one representative
location mean, with explicit limitations.

**Why does one sensor produce the overall result when the other is offline?** The
design favors availability and excludes missing locations. The dashboard shows
the contributor count so the reduced spatial coverage is visible.

**Why HTTP instead of ESP-NOW?** HTTP over a standard SoftAP is transparent,
easy to inspect, and compatible with the tablet on the same network. ESP-NOW LR
is a possible separate sensor transport if field tests justify the additional
protocol and reliability engineering.

**Is it duty-cycled?** The master remains continuous. Each sensor measures for
five seconds and places I2S/the microphone in standby for five seconds while
Wi-Fi modem sleep remains enabled. Those standby periods are not measured.

**What does disconnected mean?** No valid report was accepted within the stale
timeout. It is application freshness, not direct proof of Wi-Fi disassociation.

**Is the heat map measured everywhere?** No. Only the sensor points are measured.
The rest is inverse-distance interpolation in linear energy space, using no
obstacle or propagation model.

**Does measurement height change the calculation?** Not currently. It documents
and labels the 2D measurement plane; consistent physical placement must enforce
it.

**Can this prove legal compliance?** No. That requires the applicable standard,
approved instrumentation/calibration, prescribed placement and time weighting,
uncertainty analysis, and documented procedures.

**What happens without internet?** Nothing adverse—the system is intentionally
offline. The master provides DHCP/Wi-Fi, API, and static files locally.

**What is the main technical risk?** In the present installation, RF reliability
and antenna placement are more immediate risks than compute capacity. Measurement
traceability/calibration is the main risk if results are used beyond indicative
monitoring.

### Honest limitations to volunteer

- two points cannot characterize a complex room sound field;
- low-cost microphones and a single offset are not certified metrology;
- the map ignores 3D geometry and physical propagation;
- the status threshold can mark brief packet loss as disconnection;
- history is not persisted or time-stamped with wall-clock time;
- standard onboard 2.4 GHz antennas are challenged by metal plant rooms; and
- the UI's risk labels need alignment with the governing safety framework.

Stating these limits strengthens the presentation because it separates what the
prototype demonstrates from what a production/compliance system would require.

## Owner's quick reference

| Item | Current behavior |
|---|---|
| Sensor sample rate | 48,000 samples/s |
| FFT size / bin spacing | 4,096 / 11.71875 Hz |
| Sensor sampling | 5 s active / 5 s standby at 160 MHz |
| Sensor reporting | once per ten-second cycle |
| Master aggregation | fixed ten-second equal-energy mean |
| Stale threshold | 15,000 ms |
| Radio | standard 2.4 GHz 802.11 B/G/N SoftAP, currently channel 11 |
| Latest observed sensor input | 17 mWh/5 min (~204 mW; measured test result) |
| Master address | `192.168.4.1` |
| Dashboard | `http://192.168.4.1` |
| Status API | `/api/v1/status` |
| Sensor ingest API | `POST /api/v1/readings` |
| Map coordinates | metres, origin at bottom-left |
| Map display range | 40–100 dB(A) |
| Measurement height | 1.5 m metadata/label only |
| Persistent measurement log | none |
| Current configured nodes | `sensor-1`, `sensor-2` |

When diagnosing any problem, separate the pipeline into four questions:

1. **Measurement:** does the sensor print plausible `[LEVEL]` output?
2. **Radio/transport:** is it connected, what is RSSI, and does POST return 202?
3. **Master logic:** do `[READING]`, freshness, and `[AVERAGE]` advance?
4. **Presentation:** does `/api/v1/status` look correct before blaming the UI?

That sequence localizes most faults quickly and prevents a dashboard symptom
from being mistaken for a microphone, Wi-Fi, or aggregation failure.
