# Chiller Noise Monitor

The application implements this measurement path:

```text
INMP441 → I2S → ESP32 DSP → Firebase Realtime Database → tablet
```

The ESP32 captures 48 kHz audio using 32-bit I2S slots on GPIO 18 (BCLK),
GPIO 16 (LRCLK), and GPIO 36 (DIN). It calculates unweighted RMS/dBFS,
stateful A-weighted RMS, clipping, dominant frequency, a 64-band FFT
spectrum, and a small waveform preview. Raw audio is not stored or transmitted.

The tablet subscribes to `/current` over Firebase's REST event stream. It
validates schema version 2, detects duplicate and stale readings, and keeps a
rolling 30-second history while open. Open `/` for the tablet and
`/?mode=diagnostics` for diagnostics.

## Repository layout

- `firmware/noise_monitor/` — ESP32 capture, DSP, authentication, and upload
- `firmware/platformio.ini` — pinned PlatformIO ESP32 environment
- `firebase-client.js` — authenticated Firebase stream and reconnection
- `measurement.js` — payload validation, freshness, and rolling history
- `sensor.js` — dashboard rendering
- `firebase-config.js` — public tablet Firebase configuration
- `deployment.md` — complete setup and end-to-end test procedure

## Local dashboard

```sh
npm install
npm test
npm run test:dsp
npm start
```

Open `http://localhost:8080/?demo=1` to test the UI without hardware. Read
[`deployment.md`](./deployment.md) before connecting Firebase or flashing the
device.

Acoustic dB SPL and dB(A) remain unavailable until the assembled device is
calibrated against a reference sound-level meter. A calibration offset cannot
be inherited from the previous phone microphone pipeline.
