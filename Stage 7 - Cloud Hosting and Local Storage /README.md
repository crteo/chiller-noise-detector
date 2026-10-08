# Stage 7 — Cloud Hosting and Local Storage

## Owner's manual

Stage 7 is a local-first acoustic condition-monitoring pilot for CWP1-2. Two battery-powered ESP32-S3 sensor nodes record synchronized sound: one microphone is mounted near the pump and one is placed as a background reference. An ESP32-S3 master creates the private Wi-Fi network and the capture schedule. A laptop collector preserves WAV files, performs the fuller spectral analysis, and—when enabled—copies selected results to Firebase. A static, authenticated dashboard can be deployed by dragging its folder into Netlify.

The system is deliberately a data-collection platform, not a finished fault detector. Its first job is to create trustworthy, labelled, repeatable data with enough context to determine whether pump sound separates normal operation from abnormal operation.

## The architecture at a glance

```text
 near microphone ESP32 ----\
                            \  private AP + HTTP     laptop collector
 reference ESP32 ----------- > master ESP32 <------> local WAV + SQLite
          | features          /  192.168.4.1               |
          + WAV directly to laptop 192.168.4.200            | Admin SDK
                                                            v
                                               Firestore + Cloud Storage
                                                            |
                                               authenticated browser
                                                            v
                                                Netlify static dashboard
```

There are two separate planes:

1. **Acquisition plane:** sensor nodes, master AP and laptop collector. This continues to operate without cloud access.
2. **Presentation plane:** Firebase and the Netlify-hosted dashboard. This is optional and catches up from the laptop's persistent queue after an outage.

Netlify is used because drag-and-drop deployment is convenient for a static site. It is not used for server functions, audio ingestion or secret storage. Firebase provides authentication, document data and optional audio-object storage. The service-account credential exists only on the laptop.

## Important network requirement

The sensor nodes join the master's private Wi-Fi AP. Their WAV uploads go directly to the collector at `192.168.4.200`. An ordinary laptop Wi-Fi radio cannot simultaneously join that AP and another Wi-Fi network for internet access.

For live cloud uploads use two laptop network paths:

- Wi-Fi connected to `CWP1-2_Acoustic_Pilot`, with laptop address `192.168.4.200` (kept away from the AP's low DHCP leases).
- Ethernet, USB phone tethering, or a second Wi-Fi adapter for internet access.

Put the internet interface above the pilot Wi-Fi service in the operating-system service order. The system also works with no second interface: collection and analysis remain local, and the Firebase queue uploads later.

The master's optional upstream Wi-Fi is useful for its own NTP clock, but it is **not a router** for the laptop or sensors. Stage 7 intentionally does not depend on experimental ESP32 NAT behaviour.

## Repository map

```text
Stage 7 - Cloud Hosting and Local Storage/
├── master_node/
│   ├── master_node.ino             master AP, scheduler and local API
│   ├── platformio.ini              ESP32-S3/LittleFS build definition
│   └── data/config.example.json    master settings template
├── sensor_node/
│   ├── sensor_node.ino             shared near/reference firmware
│   ├── platformio.ini              ESP32-S3/PSRAM/LittleFS build
│   └── data/config.example.json    per-node settings template
├── collector/
│   ├── audio_collector.py          receiver, analyser, queue and uploader
│   ├── config.example.json         laptop/cloud settings template
│   └── requirements.txt            pinned Python dependencies
├── dashboard/
│   ├── index.html                  authenticated single-page dashboard
│   ├── app.js                      Firebase reads, plot and audio playback
│   ├── styles.css                  responsive presentation
│   ├── _headers                    Netlify security/cache headers
│   └── config.example.json         public Firebase web configuration
├── firebase/
│   ├── firestore.rules             authorized, read-only browser access
│   ├── storage.rules               authorized audio reads
│   └── firebase.json               rules deployment configuration
├── DEPLOYMENT.md                   start-to-finish installation runbook
└── README.md                       this manual
```

Real configuration files are named `config.json`. They and the Firebase service account are excluded by the repository `.gitignore`. The `*.example.json` files are safe templates and document every adjustment point.

## One capture, from beginning to end

1. The master calculates the next capture ID, monotonic start time and UTC time. Its `/api/v1/schedule` response is the single schedule authority.
2. Each sensor polls the schedule every two seconds. Request midpoint timing compensates for most request latency, allowing the two nodes to start approximately together.
3. Each sensor captures mono 32-bit PCM at 32 kHz into PSRAM. A ten-second capture contains 320,000 samples and approximately 1.28 MB of audio before the WAV header.
4. The sensor calculates lightweight features locally and posts them to the master. The master accepts only configured node IDs, the current boot session and increasing capture IDs.
5. The sensor sends its WAV to the laptop's authenticated `/upload` endpoint. The local shared key protects both master writes and collector uploads; it is not an internet-grade user identity.
6. The collector validates metadata and the WAV format, writes the raw file, creates a 16-bit preview and performs a larger FFT. It then creates a SQLite row. Only after this commit is the capture eligible for cloud transfer.
7. The uploader copies allowed audio objects to Cloud Storage and merges the node data into a Firestore capture document. If the network is unavailable, the row remains `retry` and is attempted later.
8. An authenticated dashboard user reads recent capture documents, plots both spectra on the same axes and requests short-lived audio download URLs from Storage.

## What each component does

### Master firmware

`master_node/master_node.ino` loads `/config.json` from LittleFS at boot. It fails closed if mandatory fields are absent, the AP password is too short, or the capture duration is not shorter than the interval.

Its endpoints are:

| Endpoint | Caller | Purpose | Authentication |
|---|---|---|---|
| `GET /` | technician | small local diagnostic page | private AP only |
| `GET /api/v1/schedule` | sensors | next synchronized capture | private AP only |
| `POST /api/v1/features` | sensors | lightweight node features | `X-API-Key` |
| `GET /api/v1/status` | collector | master/node health and latest contrast | private AP only |
| `POST /api/v1/time` | collector | UTC fallback when master NTP is absent | `X-API-Key` |

The boot-generated `session_id` prevents a delayed report from an older master boot being mistaken for a current capture. Near-minus-reference contrast is calculated only when both reports have the same capture ID.

The master can run `WIFI_AP_STA`: its AP remains the sensor network while its station interface connects to an upstream network for NTP. No Firebase credential or long-term audio is held on the ESP32.

### Sensor firmware

The same `sensor_node.ino` is flashed to both sensors. Identity and behaviour come from each board's `data/config.json`; change `device.id`, `device.name` and `device.role` between flashes. Valid roles are `near` and `reference`.

The node uses a 1.92 MB maximum PSRAM buffer (15 seconds at 32 kHz × 32 bits). Its local DSP reports:

- unweighted broadband dBFS and peak dBFS;
- estimated dB(Z) and dB(A);
- crest factor, kurtosis, zero-crossing rate and clipped fraction;
- energy in 60–200, 200–1,000, 1,000–5,000 and 5,000–15,000 Hz bands;
- three separated spectral peaks.

`calibration_offset_db` defaults to zero. Until it has been measured against a traceable sound calibrator, `estimated_dbz` is only an offset estimate and must not be presented as calibrated sound-pressure level. dBFS and relative near-minus-reference values remain useful for consistent experiments.

The node keeps Wi-Fi power saving enabled and runs the CPU at 160 MHz, as in the low-power direction of Stage 5b. However, Stage 7 is **not guaranteed to equal Stage 5b battery life**: ten seconds of continuous I2S capture plus roughly 1.28 MB of Wi-Fi transmission is material energy use. The main battery control is `capture.interval_ms` on the master. Start at five minutes, measure real current and only shorten it if the experiment needs greater time resolution.

### Laptop collector

`collector/audio_collector.py` has four responsibilities:

- a threaded HTTP receiver on port 8080;
- loss-resistant local organization plus SHA-256 integrity values;
- NumPy spectral analysis and 16-bit browser preview generation;
- a persistent SQLite-to-Firebase retry queue.

Local files follow:

```text
local-data/captures/YYYY-MM-DD/<session-id>/capture-<id>/
├── <node>-raw-s32.wav
├── <node>-preview-s16.wav
└── <node>-analysis.json
```

`collector.db` is the manifest. The unique `(node_id, session_id, capture_id)` constraint makes sensor retries idempotent. Cloud state is `pending`, `uploaded`, or `retry`; error text and attempt count aid troubleshooting.

The collector's larger analysis produces RMS and peak dBFS, crest factor, spectral centroid, normalized entropy, flatness, and a downsampled spectrum. These are candidate model inputs, not proof of a fault. Keeping WAV locally means features can be recomputed later without recollecting the experiment.

The raw-to-preview conversion halves the sample width from 32 to 16 bits for predictable browser playback. The raw signal remains authoritative for analysis.

### Firebase data model

The collector writes with the Firebase Admin SDK:

```text
assets/<asset-id>                         latest master status in `current`
assets/<asset-id>/captures/<session-id>-<capture-id>
  capture_start_utc
  nodes.<node-id>                         metadata, features, spectrum, paths
authorized_users/<firebase-auth-uid>      presence grants dashboard read access
```

Storage objects are under:

```text
assets/<asset-id>/<session-id>/<capture-id>/<node-id>/preview.wav
assets/<asset-id>/<session-id>/<capture-id>/<node-id>/raw-s32.wav  # only if enabled
```

Browser rules grant reads only when a signed-in user's UID has a corresponding `authorized_users` document. Browser writes are always denied. Admin SDK requests from the collector bypass client rules, which is why the service account must be protected.

### Web dashboard and Netlify decision

The dashboard contains only static HTML, CSS, JavaScript and public Firebase web-app identifiers. Firebase web API keys identify the project; they are not service-account secrets. Security comes from Authentication plus Firestore and Storage rules.

Netlify is a good fit here because no trusted server code is required and the `dashboard` folder can be deployed with Netlify Drop. Netlify is **not necessary** to the acquisition stack: local collection and Firebase continue if it is down. Firebase Hosting would also work, but Netlify is the final choice because drag-and-drop was requested.

The dashboard shows:

- master clock/upstream state and last cloud update;
- the latest matched near-minus-reference result;
- overlaid near/reference spectra;
- recent capture features;
- browser-playable preview WAVs when cloud audio is enabled.

## Configuration ownership

Keep the same values aligned across files:

| Value | Master | Both sensors | Collector |
|---|---|---|---|
| AP SSID/password | `local_ap` | `local_network` | laptop network settings |
| shared local key | `local_api.shared_key` | `local_api.shared_key` | `server.shared_key` |
| master address | fixed `192.168.4.1` | `master_host` | `master.base_url` |
| collector address | — | `collector.host` | laptop fixed `192.168.4.200` |
| node IDs/roles | `nodes[]` allow-list | `device` | learned from upload |
| asset ID | — | — | collector and dashboard |

The Firebase Admin service account belongs only in `collector/service-account.json`. Never put it in either firmware, the dashboard, Netlify, a screenshot or source control.

## Storage and cost reality

At the default 10-second duration, one raw recording is about 1.28 MB per node. Two nodes produce about 2.56 MB per capture. That is approximately:

- every minute: 110 GB per 30-day month;
- every five minutes: 22 GB per month;
- every fifteen minutes: 7.4 GB per month.

This is why defaults keep raw WAV on the laptop and upload only 16-bit previews when Firebase is enabled. Even previews at five-minute intervals are roughly 11 GB/month before overhead, so a continuously free all-audio cloud archive is not realistic.

Firebase Cloud Storage now requires a Blaze billing account for maintained access, although no-cost quotas may still apply in eligible regions. Treat billing alerts and lifecycle decisions as mandatory. To avoid billing entirely, set `firebase.enabled` to `false`, or set both audio upload flags false and use only the services available to your Firebase plan. The local archive is always the source of truth.

## Data-quality and future anomaly modelling

For the stated pump, which normally runs near one stable VSD percentage or is off, a model scoped to that running state is reasonable. First prove the dataset contains stable and separable information:

1. Label every capture with `off`, `running-normal`, maintenance/activity notes and known abnormal events. Stage 7 does not invent those labels.
2. Collect repeated sessions across days, loads, weather and surrounding-equipment combinations.
3. Compare within matched capture IDs: near microphone, reference microphone and their differences.
4. Check repeatability within the normal-running class before fitting a detector.
5. Use time-grouped validation—train on earlier days and test on later days—rather than randomly splitting adjacent captures.
6. Begin with robust scaling plus a simple distance/Isolation Forest/one-class model. Deep learning is unnecessary until the data volume and labelled outcomes justify it.

Background equipment is handled experimentally, not magically. Keep the reference microphone fixed, record which other machines are operating, and inspect whether near-minus-reference spectra reduce common noise. A reference microphone can also remove true pump energy if placed poorly; placement trials are part of the pilot.

dB(Z) can be a useful broadband level feature after calibration, particularly for trends at a fixed operating point. Without calibration it is effectively dBFS plus an assumed constant. Frequency-band and spectral-shape features add information that one broadband number cannot: two recordings may have the same total level but different tones, cavitation-like broadband energy or mechanical harmonics.

## Operational checklist

Before a collection run:

- verify both microphones and physical locations are unchanged;
- start the collector before powering sensors;
- open `http://192.168.4.1` and confirm UTC is ready;
- confirm the collector `/health` endpoint;
- observe one matched capture and play both local previews;
- record operating state and surrounding equipment in the experiment log;
- check laptop free space and power settings.

After a run:

- stop the collector cleanly with Ctrl-C;
- back up `local-data` and the external experiment log;
- check SQLite for `retry` rows if cloud upload was expected;
- do not delete raw WAV merely because a preview appears online;
- note any sensor movement, outages or configuration changes.

## Failure behaviour

| Failure | Result | Recovery |
|---|---|---|
| Internet/Firebase unavailable | local capture continues; queue becomes `retry` | restore internet; leave collector running |
| Netlify unavailable | acquisition and Firebase writes continue | use local files; dashboard returns later |
| Collector stopped | features still reach master; WAV upload fails | start collector before next captures; sensor has no durable resend queue |
| Master reboots | new session ID; sensors adopt next schedule | expected; session separates records |
| One sensor absent | other node is stored; no matched contrast | repair node; do not compare mismatched IDs |
| Laptop disk full | new uploads fail | free/extend storage; monitor capacity before runs |
| Firebase credential leaked | cloud project at risk | revoke key immediately and issue a new service account |

The largest remaining resilience limitation is deliberate: sensor nodes buffer one capture in RAM and do not retain failed WAV uploads across reboot or the next capture. For a pilot, the laptop should be supervised. A later stage could add SD storage or a retransmission ledger on each sensor.

## Start here

Follow [DEPLOYMENT.md](DEPLOYMENT.md) exactly for first installation. It includes Firebase, authentication, security rules, firmware configuration, laptop networking, collector testing and Netlify Drop.
