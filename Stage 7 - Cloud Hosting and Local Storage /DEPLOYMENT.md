# Stage 7 deployment runbook

This runbook starts from an unconfigured Firebase account and finishes with two sensors sending audio to a laptop, optional Firebase uploads, and a web dashboard on Netlify.

## 0. Decide the pilot mode

Choose one before starting:

- **Local-only:** no billing account, no internet required during collection. Set `firebase.enabled` to `false`. All WAVs, previews, spectra and the queue remain on the laptop.
- **Cloud metadata only:** Firebase enabled but both audio upload flags false. The dashboard shows spectra/features but has no playback.
- **Cloud preview audio:** recommended Stage 7 demonstration. Raw WAV stays local; 16-bit preview uploads. Firebase Storage requires the Blaze plan, so configure a strict budget alert. Blaze is pay-as-you-go, not a promise that every use remains free.

Do not enable raw cloud audio initially.

## 1. Prepare local tools

Install:

1. Visual Studio Code with PlatformIO, or the PlatformIO CLI.
2. Python 3.11 or newer.
3. Node.js only if you want to deploy Firebase rules from the CLI. Rules can instead be pasted into the Firebase console.
4. Git for safe version control, without committing credentials.

In a terminal, move into the repository root that contains the Stage 7 folder. Because the name contains spaces, keep paths quoted.

Create the laptop Python environment:

```bash
cd "Stage 7 - Cloud Hosting and Local Storage/collector"
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

On Windows PowerShell, activation is:

```powershell
.venv\Scripts\Activate.ps1
```

## 2. Create a strong shared local key

Generate one value:

```bash
openssl rand -hex 32
```

Keep this value temporarily in a password manager. It must be identical in:

- `master_node/data/config.json` → `local_api.shared_key`;
- each sensor's `sensor_node/data/config.json` → `local_api.shared_key`;
- `collector/config.json` → `server.shared_key`.

This key stays on the private pilot network. Do not reuse a personal password.

## 3. Configure and flash the master

1. Copy `master_node/data/config.example.json` to `master_node/data/config.json`.
2. Set a strong `local_ap.password` of at least eight characters.
3. Paste the shared local key.
4. Keep the two `nodes` IDs aligned with the two sensor configurations.
5. Keep `capture.interval_ms` at `300000` (five minutes) and `duration_ms` at `10000` for the first battery/storage trial.
6. If the master can join a site or phone-hotspot Wi-Fi network, set `upstream_wifi.enabled` true and provide its credentials. Otherwise set it false; the laptop collector will provide time.

In PlatformIO, open `master_node` as the project and connect the master ESP32-S3. Upload the LittleFS configuration first, then firmware:

```bash
cd "master_node"
pio run --target uploadfs
pio run --target upload
pio device monitor --baud 115200
```

Expected serial output includes `stage7-master-v1.0.0 ready` and `http://192.168.4.1`.

If you change `config.json` later, run `uploadfs` again. A normal firmware upload does not automatically replace LittleFS data.

## 4. Configure and flash each sensor

Copy `sensor_node/data/config.example.json` to `sensor_node/data/config.json`.

For the near-field board use:

```json
"device": {
  "id": "cwp1-2-near",
  "name": "CWP1-2 Drive-End Near Field",
  "role": "near"
}
```

For the reference board use:

```json
"device": {
  "id": "cwp1-2-reference",
  "name": "CWP1-2 Background Reference",
  "role": "reference"
}
```

For both boards:

1. Copy the master's AP SSID and password into `local_network`.
2. Keep `master_host` as `192.168.4.1`.
3. Keep collector host/port as `192.168.4.200:8080`.
4. Paste the same shared key.
5. Verify I2S pins against the physical wiring.
6. Leave `calibration_offset_db` at `0.0` until a real acoustic calibration is performed.

Flash the near board:

```bash
cd "sensor_node"
pio run --target uploadfs
pio run --target upload
pio device monitor --baud 115200
```

Then edit `data/config.json` to the reference identity and repeat `uploadfs` and `upload` on the second board. Save a private copy of each final board configuration outside the shared `data/config.json` filename so you can reproduce both boards.

Expected serial output identifies the node, role and PSRAM allocation. A fatal LittleFS message means `uploadfs` was omitted or the JSON is invalid.

## 5. Configure the laptop's pilot network

1. Power the master.
2. Join its AP from the laptop using the configured SSID/password.
3. Give that Wi-Fi interface the manual IPv4 address `192.168.4.200` and subnet mask `255.255.255.0`. The high address avoids the ESP32 AP's low DHCP lease range used by sensors.
4. Keep the internet default route on Ethernet, USB tethering or a second adapter. On macOS, use Network settings → service order and place the internet interface above pilot Wi-Fi. Avoid setting the pilot interface as the preferred default route.
5. Open `http://192.168.4.1` in a browser. The local diagnostic page should load.

If the laptop has only one Wi-Fi interface and no second connection, continue in local-only mode. Cloud upload will catch up after you reconnect the laptop to the internet, but it cannot reach the master AP at the same time.

Check the addresses:

```bash
ping 192.168.4.1
```

The sensors must be able to reach `192.168.4.200`; permit inbound TCP port 8080 in the laptop firewall for the private interface.

## 6. Configure and test the collector locally

Copy `collector/config.example.json` to `collector/config.json` and:

1. paste the shared key;
2. confirm `asset_id` is `cwp1-2`;
3. leave `firebase.enabled` false for the first test;
4. keep `local_storage.root` on a disk with sufficient space.

Validate without opening a server:

```bash
cd "collector"
source .venv/bin/activate
python audio_collector.py --config config.json --check
```

Start the collector:

```bash
python audio_collector.py --config config.json
```

In a second terminal:

```bash
curl http://127.0.0.1:8080/health
```

Expected JSON includes `"ok": true` and `"firebase": false`.

Power both sensors. At the next scheduled time, the collector should log two stored capture keys. Inspect:

```text
collector/local-data/captures/<date>/<session>/capture-<number>/
```

Play both `preview-s16.wav` files, and open each `analysis.json`. Do not proceed to cloud setup until both nodes repeatedly produce matched session/capture numbers.

## 7. Create the Firebase project

Firebase console labels may evolve, but the required resources are stable:

1. Open the [Firebase console](https://console.firebase.google.com/) and create a project, for example `cwp1-2-acoustic-pilot`.
2. Analytics is optional and not used by this implementation.
3. Open **Build → Firestore Database**, create a database and choose a region near the deployment. Start in locked/production mode.
4. Open **Build → Authentication → Sign-in method**, enable Google.
5. Open **Project settings → Your apps**, add a Web app. Do not enable Firebase Hosting. Copy the displayed Firebase configuration values for the dashboard later.
6. Open **Build → Storage** and create the default bucket if cloud audio is required.

As of February 3, 2026, Firebase requires a Blaze billing plan to maintain Cloud Storage access. Add billing only if you accept that requirement. Set Google Cloud Billing budgets/alerts immediately. A budget alert warns—it does not automatically cap all charges.

Official references:

- [Firebase web setup](https://firebase.google.com/docs/web/setup)
- [Google sign-in for web](https://firebase.google.com/docs/auth/web/google-signin)
- [Cloud Storage plan changes](https://firebase.google.com/docs/storage/faqs-storage-changes-announced-sept-2024)
- [Firestore pricing and free quota](https://firebase.google.com/docs/firestore/pricing)

## 8. Deploy Firebase security rules

Recommended CLI method:

```bash
npm install --global firebase-tools
firebase login
cd "Stage 7 - Cloud Hosting and Local Storage/firebase"
firebase use --add
firebase deploy --only firestore:rules,storage
```

Select the new project when prompted. The included rules deny every browser write and deny reads unless the user's UID is authorized.

Console alternative:

1. Paste `firebase/firestore.rules` into Firestore → Rules and publish.
2. Paste `firebase/storage.rules` into Storage → Rules and publish.

Do not temporarily publish open test rules such as `allow read, write: if true`.

## 9. Create an authorized dashboard user

1. Attempt Google sign-in once from the local or deployed dashboard. The screen may report permission denied; Authentication still creates the user.
2. In Firebase Authentication → Users, copy that user's UID.
3. In Firestore, create the top-level collection `authorized_users`.
4. Create a document whose document ID is exactly the UID. Add an optional string field such as `email` for administrator reference.

The document ID, not the email field, grants access. Repeat for each approved viewer. Delete the document to revoke dashboard data/audio access; disabling the Authentication user is an additional revocation measure.

## 10. Create the Firebase service account

In Firebase console:

1. Open Project settings → Service accounts.
2. Choose Firebase Admin SDK → Generate new private key.
3. Save the downloaded JSON as `collector/service-account.json`.
4. Confirm the file is ignored by Git. Never email it or place it in `dashboard`.

The generated default service account is powerful. For a longer-lived production deployment, replace it with a purpose-built Google Cloud service account restricted to the required Firestore and Storage roles.

Update `collector/config.json`:

```json
"firebase": {
  "enabled": true,
  "service_account_file": "./service-account.json",
  "storage_bucket": "YOUR-PROJECT-ID.firebasestorage.app",
  "upload_preview_audio": true,
  "upload_raw_audio": false,
  "retry_seconds": 60
}
```

Confirm the exact bucket name in Firebase Project settings or Storage; older projects may use an `appspot.com` bucket name. Use the exact displayed value.

Restart the collector. Existing `pending`/`retry` rows will upload. New rows will first commit locally and then upload. In Firestore, confirm:

```text
assets / cwp1-2 / captures / <session>-<capture>
```

In Storage, confirm preview objects under `assets/cwp1-2/`.

## 11. Configure the dashboard

Copy `dashboard/config.example.json` to `dashboard/config.json`. Paste the public web-app configuration from Firebase Project settings. Keep:

```json
"assetId": "cwp1-2",
"captureLimit": 50
```

The Firebase web API key is expected to be visible in browser code; it is not the Admin private key. Authentication and rules enforce access.

For a local dashboard test, do not double-click `index.html`, because module imports and JSON fetches require HTTP. Run:

```bash
cd "dashboard"
python3 -m http.server 4173
```

Open `http://localhost:4173`. In Firebase Authentication → Settings → Authorized domains, ensure `localhost` is listed. Sign in with the user authorized in step 9.

## 12. Deploy with Netlify Drop

Netlify is used only for the static dashboard.

1. Ensure `dashboard/config.json` exists and contains the correct Firebase web configuration.
2. Open [Netlify Drop](https://app.netlify.com/drop).
3. Drag the **dashboard folder itself** into the drop area. It must contain `index.html`, `app.js`, `styles.css`, `_headers` and `config.json` at its top level.
4. Wait for the generated HTTPS URL.
5. In Firebase Authentication → Settings → Authorized domains, add only the hostname, for example `your-site.netlify.app` without `https://` or a path.
6. Open the Netlify URL, sign in and verify status, spectra and preview playback.
7. Optionally set a stable custom Netlify site name. If the hostname changes, update Firebase authorized domains.

The `_headers` file tells Netlify not to cache `config.json` and adds restrictive browser security headers.

Official Netlify instructions: [Netlify Drop quickstart](https://docs.netlify.com/start/quickstarts/netlify-drop-quickstart/).

## 13. Run a normal collection session

Use this order:

1. Connect laptop Wi-Fi to the master AP and establish the separate internet path if needed.
2. Connect laptop power and disable sleep for the planned period.
3. Activate the Python environment and start the collector.
4. Open `http://192.168.4.1`; verify clock and next capture.
5. Power the near and reference nodes.
6. Watch for two stored messages with the same session/capture ID.
7. Check the Netlify dashboard after the uploader logs completion.
8. Record pump state, VSD percentage, other operating equipment, microphone placement and interventions in the experiment log.
9. Stop the collector with Ctrl-C at the end.
10. Back up `collector/local-data`.

## 14. Verify the queue and disk

SQLite inspection:

```bash
cd "collector"
sqlite3 local-data/collector.db \
  "select cloud_state,count(*) from captures group by cloud_state;"
```

Recent failures:

```bash
sqlite3 local-data/collector.db \
  "select capture_key,cloud_attempts,cloud_error from captures where cloud_state='retry' order by received_utc desc limit 10;"
```

Disk usage:

```bash
du -sh local-data
df -h .
```

Do not manually change queue state unless you understand the SQLite manifest. Restarting the collector is normally sufficient to retry.

## 15. Common problems

### Sensor reports collector connection failed

- collector is not running;
- laptop is not `192.168.4.200` on the master AP;
- firewall blocks inbound 8080;
- sensor `collector.host` or shared key differs;
- laptop slept during the capture.

### Features arrive but no WAV appears

The master and collector are separate destinations. A working master feature POST proves only the sensor-to-master path. Test `http://192.168.4.200:8080/health` from the pilot network and inspect collector logs.

### Master clock is not ready

Start the collector with the correct shared key; it calls `/api/v1/time` every poll. Alternatively allow the master's upstream Wi-Fi to reach its configured NTP servers.

### Firebase permission denied in dashboard

- confirm Google sign-in is enabled;
- add the Netlify hostname to Authorized domains;
- verify `authorized_users/<exact UID>` exists;
- deploy both rule files to the same Firebase project named in dashboard config.

### Collector Firebase errors

- verify `firebase.enabled`, service-account path and exact bucket name;
- verify billing/Storage status if audio is enabled;
- check the service account belongs to the same project;
- retain the local queue and fix the configuration—do not recollect data.

### Dashboard loads but spectrum is empty

The capture may predate the Stage 7 analysis schema, or the collector upload may still be pending. Inspect the Firestore node map for `spectrum_hz` and `spectrum_dbfs`.

### Audio will not play

Confirm `upload_preview_audio` is true, the capture node has `preview_storage_path`, Storage rules are deployed, and the user UID is authorized. The dashboard intentionally plays the 16-bit preview rather than the 32-bit raw WAV.

## 16. Before leaving the pilot unattended

- Measure actual sensor current over a full capture interval.
- Calculate laptop disk growth from the chosen interval.
- Create a tested backup process.
- Add billing alerts if Storage is enabled.
- Confirm that site policy permits acoustic recording and cloud storage.
- Document microphone distances/orientation permanently.
- Test loss of internet, one sensor and master reboot.
- Decide a retention policy only after data has been backed up. Stage 7 never automatically deletes local WAVs.
