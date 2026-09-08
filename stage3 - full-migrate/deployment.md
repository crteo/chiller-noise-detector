# Deployment and end-to-end testing

This guide deploys the full `INMP441 → ESP32 → Firebase → tablet` system.

## 1. Required items

- An original ESP32 development board compatible with the `esp32dev` target
- An INMP441 I2S microphone module
- Jumper wires and a stable USB power supply
- A 2.4 GHz Wi-Fi network with internet access
- A Firebase project with Realtime Database and Authentication enabled
- Node.js 20 or newer
- PlatformIO CLI, or VS Code with the PlatformIO extension

## 2. Wire the microphone

Disconnect USB power before changing wires.

| INMP441 | ESP32 | Purpose |
|---|---:|---|
| VDD | 3V3 | Power |
| GND | GND | Ground |
| SCK/BCLK | GPIO 18 | I2S bit clock |
| WS/LRCL | GPIO 16 | I2S left/right clock |
| SD | GPIO 36 | I2S data input |
| L/R | GND | Left channel, the firmware default |

If L/R is connected to 3V3, set `MICROPHONE_RIGHT_CHANNEL true` in `config.h`.
GPIO 36 is input-only, which is appropriate for the microphone data line.

## 3. Configure Firebase

1. Open Firebase Console and select the `science-park-noise-monitoring` project.
2. In **Build → Realtime Database**, create or open the database in the
   `asia-southeast1` region.
3. In **Build → Authentication → Sign-in method**, enable **Email/Password** and
   **Anonymous** sign-in.
4. In **Authentication → Users**, add a dedicated device user. Use its email and
   password only in the ESP32's ignored `config.h` file. Copy this user's UID.
5. In **Project settings → General**, copy the Web API key.
6. Put these rules in **Realtime Database → Rules**, then publish them:

```json
{
  "rules": {
    ".read": false,
    ".write": false,
    "current": {
      ".read": "auth != null",
      ".write": "auth != null && auth.uid == 'PASTE_DEVICE_USER_UID_HERE'",
      ".validate": "newData.hasChildren(['version', 'deviceId', 'bootId', 'sequence', 'measuredAt', 'uploadedAt', 'values', 'status', 'spectrum', 'diagnostics', 'waveform']) && newData.child('version').val() == 2"
    }
  }
}
```

Replace `PASTE_DEVICE_USER_UID_HERE` with the UID copied in step 4. Authenticated
tablet sessions can read, while only the dedicated device account can write.
For a multi-device deployment, use custom claims or a device-ID-to-UID mapping.

## 4. Configure the tablet

Edit `firebase-config.js` and set `apiKey` to the Web API key from step 3. Leave
the supplied database URL unchanged:

```js
export const firebaseConfig = {
  databaseURL: "https://science-park-noise-monitoring-default-rtdb.asia-southeast1.firebasedatabase.app/current.json",
  apiKey: "YOUR_FIREBASE_WEB_API_KEY",
};
```

The API key identifies the Firebase project and is safe in the web bundle. Do
not put a device password, database secret, or service-account key here.

## 5. Configure and flash the ESP32

From the repository root:

```sh
cp firmware/noise_monitor/config.example.h firmware/noise_monitor/config.h
```

Edit `config.h` and fill in Wi-Fi, Firebase API key, device email, and device
password. Confirm the database URL and `DEVICE_ID`.

With PlatformIO CLI installed:

```sh
cd firmware
pio run
pio run --target upload
pio device monitor --baud 115200
```

With the VS Code extension, open the `firmware` directory as the PlatformIO
project, build the `esp32dev` environment, upload it, then open the serial
monitor at 115200 baud. If your board differs, change `board` in
`firmware/platformio.ini` to its exact board ID and confirm GPIO 36 exists.

The serial monitor should show Wi-Fi progress followed by `Noise monitor
running`. Authentication or upload HTTP failures appear there.

## 6. Test the software without hardware

From the repository root:

```sh
npm install
npm test
npm run test:dsp
npm start
```

Open:

- `http://localhost:8080/?demo=1` — tablet with synthetic live data
- `http://localhost:8080/?mode=diagnostics&demo=1` — synthetic diagnostics

Confirm the value and charts update, Pause/Connect works in diagnostics, and no
browser microphone permission is requested. Generate deployable static files
with `npm run build`; output is written to `dist/`.

## 7. Run the live end-to-end system

1. Power the ESP32 and leave the serial monitor open.
2. In Firebase Console, inspect `/current`. It should be replaced about five
   times per second with a version 2 object.
3. Start the dashboard with `npm start`.
4. Open `http://localhost:8080/`. Status should become **INMP441 connected**.
5. Open `http://localhost:8080/?mode=diagnostics`. Confirm a 48,000 Hz sample
   rate, 100 ms windows, changing PCM values, spectrum, and dominant frequency.
6. Clap or play a steady tone near the microphone. Confirm level, waveform, and
   spectrum respond without clipping during ordinary use.
7. Unplug the ESP32. The tablet should show stale data after 2 seconds and
   disconnected after 10 seconds.
8. Reconnect it. A new boot ID should be accepted when its sequence restarts.
9. Disable Wi-Fi temporarily. Capture should continue; after reconnection, only
   the newest reading should upload instead of a backlog.

For permanent deployment, run `npm run build` and host `dist/` on an HTTPS
static host such as Firebase Hosting, Cloudflare Pages, Netlify, or Vercel.

## 8. Check I2S alignment and channel selection

If all readings remain at the floor:

1. Verify VDD uses 3.3 V and all grounds are shared.
2. Verify SD goes to GPIO 36, SCK to GPIO 18, and WS to GPIO 16.
3. Match `MICROPHONE_RIGHT_CHANNEL` to the INMP441 L/R connection.
4. Inspect `latestSample`, RMS, and peak in diagnostics while speaking close to
   the microphone.

The firmware assumes the INMP441's 24-bit sample is left-aligned in the ESP32's
32-bit receive word. Before calibration, verify this on the actual board: quiet
audio should produce small changing values, louder audio should increase RMS,
and ordinary sound should remain below a normalized peak of 1.0.

## 9. Calibrate acoustic readings

Leave `CALIBRATION_ENABLED false` until calibration is complete. The dashboard
will display digital diagnostics and leave dB SPL/dB(A) blank.

1. Place the assembled microphone and a trusted reference sound-level meter
   close together with matching orientation.
2. Use a stable broadband source in the intended operating range.
3. Record the ESP32 A-weighted digital value and reference dB(A).
4. Calculate `constant = reference dB(A) - ESP32 dBFS(A)`.
5. Repeat at several levels. A changing offset indicates clipping, noise-floor
   limits, or a nonlinear setup that a single constant cannot correct.
6. Put the validated offset in `CALIBRATION_CONSTANT_DB`, set
   `CALIBRATION_ENABLED true`, rebuild, and flash.
7. Repeat the comparison and document the equipment, source, distance,
   environment, date, and uncertainty.

The result is a calibrated project instrument. It should not be described as a
standards-compliant sound-level meter without formal conformance testing.

## 10. Production hardening

Before unattended deployment, install the relevant Google root CA in the
firmware and replace both `client.setInsecure()` calls with `setCACert()`. Also
restrict Firebase write rules by device identity, provide stable power, fix the
enclosure and microphone orientation, and run an extended recovery test.
