# Quick Deploy

http://localhost:8080
cloudflared tunnel --url http://localhost:8080

# Stage 1A / 1B / 1C-1 — iPhone microphone → dBFS and provisional A-weighting

This package implements the unweighted PCM measurement path and a parallel,
provisional FFT-based A-weighting path.

It is intentionally limited to:

```text
Sound
 ↓
iPhone microphone
 ↓
getUserMedia()
 ↓
MediaStream
 ↓
AudioContext
 ↓
AudioWorklet
 ↓
Float32 PCM
 ↓
inspect samples
 ↓
DC removal
 ↓
RMS
 ↓
20 log10(RMS)
 ↓
dBFS
```

It does **not** yet include:

- ESP32
- MQTT
- tablet dashboard
- dB SPL
- dBA
- an assumed acoustic calibration constant

That separation is deliberate. The objective of Stage 1A/1B is to understand and validate the digital signal chain before adding transport or acoustic calibration.

---

## 1. Files

```text
stage1A_1B_phone_pcm_dbfs/
├── a-weighting.js
├── a-weighting.test.js
├── index.html
├── package.json
├── sensor.js
├── pcm-processor.js
└── README.md
```

### `index.html`

The user interface.

It shows:

- browser microphone status
- sample rate
- AudioWorklet block size
- latest normalized PCM sample
- DC offset
- RMS amplitude
- peak amplitude
- dBFS
- clipping percentage
- recent dBFS history
- a recent PCM waveform
- browser-reported microphone settings
- live frequency spectrum and dominant frequency
- provisional A-weighted digital level
- FFT-derived A-weighting effect

### `sensor.js`

This file:

1. asks Safari for microphone permission;
2. requests mono audio;
3. requests that echo cancellation, noise suppression and automatic gain control be disabled;
4. creates an `AudioContext`;
5. connects the microphone `MediaStream`;
6. starts the `AudioWorklet`;
7. receives calculated results from the worklet; and
8. displays them.

### `pcm-processor.js`

This is the main measurement code.

It receives normalized PCM samples and calculates:

```text
PCM
 ↓
mean
 ↓
DC removal
 ↓
mean square
 ↓
square root
 ↓
RMS
 ↓
20 log10(RMS)
 ↓
dBFS
```

---

# 2. Why the page must be hosted over HTTPS

On an iPhone, a normal webpage is generally allowed to access the microphone only from a **secure browser context**.

Therefore, simply copying the files to your laptop and opening:

```text
file:///.../index.html
```

is not the intended method.

Likewise, serving the files over an ordinary LAN URL such as:

```text
http://192.168.1.50
```

may not give Safari microphone access.

For this stage, host the three web files on an HTTPS static host.

Suitable examples include:

- GitHub Pages
- Cloudflare Pages
- Vercel
- Netlify

You do **not** need a permanently running Node.js server for this experiment.

The hosting service only serves three static files.

---

# 3. Simplest deployment method

## Option A — GitHub Pages

Create a new repository and place these files at its root:

```text
index.html
sensor.js
pcm-processor.js
README.md
```

Then enable GitHub Pages for the repository.

Open the resulting HTTPS page in Safari on your iPhone.

Example shape:

```text
https://YOUR-USERNAME.github.io/YOUR-REPOSITORY/
```

Do not hard-code the example above. Use the actual URL created for your repository.

---

## Option B — any HTTPS static host

Upload:

```text
index.html
sensor.js
pcm-processor.js
```

to a service that serves them from the **same directory and origin**.

Then open the HTTPS URL on your iPhone.

---

# 4. Running the experiment

Once the webpage is open on the iPhone:

1. Tap **Start microphone**.
2. Safari should ask for microphone permission.
3. Allow microphone access.
4. Watch the displayed values.
5. Speak normally near the phone.
6. Speak more loudly.
7. Move farther away.
8. Create a steady sound.
9. Observe how the PCM, RMS and dBFS quantities change.
10. Tap **Stop microphone** when finished.

---

# 5. What is the phone actually outputting?

The JavaScript does **not** receive Pascals.

The rough physical/electronic chain is:

```text
sound pressure in air
       ↓
microphone diaphragm
       ↓
analog electrical signal
       ↓
phone audio electronics / ADC
       ↓
iOS audio pipeline
       ↓
browser MediaStream
       ↓
Web Audio
       ↓
Float32 PCM samples
```

The browser normally exposes audio samples as floating point values approximately between:

```text
-1.0 and +1.0
```

Example:

```text
+0.00182
+0.00315
+0.00491
+0.00332
-0.00081
-0.00422
...
```

These are **normalized digital amplitudes**.

They are not:

```text
Pa
dB
dB SPL
dBA
volts
```

---

# 6. How to interpret `Latest PCM sample`

Suppose the page displays:

```text
Latest PCM sample
+0.0042310
```

This means the latest sample has a normalized digital amplitude of about:

```text
0.00423 full scale
```

It does **not** mean:

```text
0.00423 Pa
```

and it does **not** mean:

```text
0.00423 dB
```

A single PCM sample is also not a meaningful sound level by itself.

Sound level must be derived from many samples over a time window.

---

# 7. Sample rate

The page may show something such as:

```text
Sample rate
48000 Hz
```

This means the audio system is producing:

```text
48,000 PCM samples per second
```

Each sample is separated by approximately:

```text
1 / 48000 s
≈ 20.8 microseconds
```

Do not assume the sample rate is always 48 kHz.

The page displays the actual `AudioContext.sampleRate` reported by the browser.

---

# 8. AudioWorklet block size

You may see:

```text
AudioWorklet block size
128
```

This does **not** mean that the measurement window is 128 samples.

The browser passes audio to the `AudioWorklet` in small processing blocks.

The code then accumulates many such samples until approximately **100 ms** of audio has been collected.

For 48 kHz audio:

```text
0.100 s × 48,000 samples/s
= 4,800 samples
```

The RMS and dBFS calculation is therefore based on about 4,800 samples, not one 128-sample block.

---

# 9. DC offset

An ideal audio waveform is centered around zero.

For example:

```text
 +0.04
 +0.02
  0.00
 -0.02
 -0.04
```

has approximately zero average.

However, a captured signal could theoretically be shifted:

```text
 +0.14
 +0.12
 +0.10
 +0.08
 +0.06
```

The average is no longer zero.

The program calculates:

```text
mean = (x1 + x2 + ... + xN) / N
```

This mean is displayed as:

```text
DC offset
```

The code then calculates the RMS of the **AC component**, equivalent to subtracting the mean from every sample.

Mathematically:

```text
x_AC[n] = x[n] - mean
```

---

# 10. RMS amplitude

If we simply averaged a sound waveform, its positive and negative portions would mostly cancel.

Example:

```text
+0.1
+0.1
-0.1
-0.1
```

Average:

```text
0
```

Yet a real signal is clearly present.

Therefore we use RMS:

```text
RMS = sqrt(
  mean(
    x_AC[n]^2
  )
)
```

The operations are:

1. remove the DC component;
2. square each sample;
3. average the squared samples;
4. take the square root.

RMS is a useful measure of the effective amplitude/energy content of the waveform.

Example:

```text
RMS amplitude
0.00482
```

still does **not** mean 0.00482 Pa.

It means roughly:

```text
0.00482 full-scale RMS digital amplitude
```

---

# 11. dBFS

The code defines:

```text
dBFS = 20 × log10(RMS)
```

because full scale is represented by normalized amplitude `1.0`.

For example:

```text
RMS = 0.01
```

gives:

```text
20 × log10(0.01)
= -40 dBFS
```

Another example:

```text
RMS = 0.001
```

gives:

```text
-60 dBFS
```

Therefore:

```text
larger digital signal  → dBFS approaches 0
smaller digital signal → more negative dBFS
```

---

# 12. Why dBFS is usually negative

Digital full scale is the upper limit of the normalized signal representation.

Conceptually:

```text
 0 dBFS       digital full-scale RMS reference
-10 dBFS
-20 dBFS
-30 dBFS
-40 dBFS
-50 dBFS
-60 dBFS
 ...
```

A larger negative number means a smaller signal.

For example:

```text
-30 dBFS
```

is a larger digital level than:

```text
-60 dBFS
```

---

# 13. Important dBFS convention used by this code

This package uses:

```text
dBFS = 20 log10(RMS / 1.0)
```

Therefore a full-scale sine wave:

```text
x(t) = sin(t)
```

has peak amplitude:

```text
1.0
```

but RMS:

```text
1 / sqrt(2)
≈ 0.7071
```

so the page would calculate:

```text
20 log10(0.7071)
≈ -3.01 dBFS
```

This convention is documented deliberately because different digital metering systems can use different RMS normalizations.

---

# 14. Peak amplitude

`Peak amplitude` is the largest absolute PCM sample observed during the current 100 ms measurement window.

For example:

```text
Peak amplitude
0.0374
```

means no sample in that window exceeded approximately 3.74% of normalized full scale.

RMS and peak answer different questions.

### Peak

```text
What was the largest individual excursion?
```

### RMS

```text
How large was the waveform on average in an energy-related sense?
```

---

# 15. Clipped samples

The code currently counts a sample as close to clipping when:

```text
|sample| >= 0.999
```

If you see:

```text
Clipped samples
0.000 %
```

no samples in the measurement window reached that threshold.

If clipping becomes significant, the digital waveform can no longer faithfully represent further increases in acoustic input.

For example:

```text
real sound level increases
        ↓
microphone / audio chain reaches maximum
        ↓
PCM cannot increase proportionally
        ↓
dBFS stops tracking the real increase correctly
```

That would make acoustic calibration unreliable above the clipping/compression region.

---

# 16. What the waveform graph means

The waveform display shows a short portion of recent normalized PCM.

A quiet signal may appear relatively small.

A louder signal should generally produce larger excursions.

However, the plot automatically scales itself so that you can see the waveform shape.

Therefore do **not** use its visual height to compare absolute levels between different time periods.

Use RMS and dBFS for that comparison.

---

# 17. What the dBFS history graph means

The dBFS history graph shows recent 100 ms measurements.

You can use it to investigate:

- whether louder sounds increase dBFS;
- whether quiet sounds reduce dBFS;
- whether the signal stabilizes;
- whether sudden changes gradually settle;
- whether phone processing may be altering the gain.

This will become particularly useful for investigating possible automatic gain control.

---

# 18. Browser microphone settings

The page asks for:

```javascript
{
  channelCount: 1,
  echoCancellation: false,
  noiseSuppression: false,
  autoGainControl: false
}
```

However:

> a request is not proof that the complete iPhone audio chain is truly unprocessed.

The page therefore also displays:

```text
browserSaysConstraintIsSupported
```

and:

```text
reportedTrackSettings
```

Study these values.

If the browser reports that a setting is unsupported or does not report it, do not assume it has been disabled.

---

# 19. Critical limitation: dBFS is not dB SPL

This is the most important interpretation rule.

The page may display:

```text
-43.2 dBFS
```

You cannot conclude:

```text
43.2 dB SPL
```

or:

```text
43.2 dBA
```

The references are different.

## dBFS

Referenced to:

```text
digital full scale
```

## dB SPL

Referenced to:

```text
20 µPa acoustic pressure
```

The missing relationship is:

```text
physical pressure in Pa
           ↕
phone digital amplitude
```

That relationship has not been acoustically calibrated.

---

# 20. Do not assume calibration is a constant yet

A common simplified model is:

```text
dB SPL = dBFS + C
```

where `C` is a calibration offset.

That is only valid if the overall system behaves approximately as a fixed-gain linear system over the relevant level and frequency range.

For the phone, that is currently an **unverified hypothesis**.

The real mapping could be:

```text
dB SPL = F(dBFS)
```

or more generally:

```text
true level = F(digital level, frequency, signal history, ...)
```

because the phone pipeline may contain:

- automatic gain control;
- compression;
- limiting;
- frequency-dependent response;
- microphone nonlinearity;
- operating-system processing;
- browser processing.

Therefore this Stage 1A/1B package intentionally applies **no acoustic calibration**.

---

# 21. First tests to perform

## Test 1 — quiet versus loud

Use the phone in a stable position.

Observe dBFS while:

1. the room is quiet;
2. you speak softly;
3. you speak normally;
4. you speak loudly.

Expected qualitative result:

```text
louder sound
    ↓
greater PCM amplitude
    ↓
greater RMS
    ↓
dBFS becomes less negative
```

For example:

```text
quiet:  -62 dBFS
speech: -41 dBFS
loud:   -27 dBFS
```

These numbers are examples only.

---

## Test 2 — distance

Produce a reasonably steady sound from another device.

Keep its volume fixed.

Measure approximately:

```text
0.25 m
0.5 m
1 m
2 m
```

Do not interpret the results as a precision free-field inverse-square-law experiment unless environmental conditions are tightly controlled.

The aim at this stage is simply to verify that the digital chain responds systematically to changing input level.

---

## Test 3 — look for AGC / compression

Use a steady source.

Procedure:

1. leave the source at a lower level for about 20–30 s;
2. suddenly increase it;
3. keep the higher level constant;
4. suddenly decrease it again;
5. watch the dBFS history.

A fixed-gain system would ideally show a fairly immediate step followed by a stable level.

If you instead see something like:

```text
sound becomes louder
        ↓
dBFS jumps
        ↓
dBFS slowly falls despite constant source
```

this could indicate gain control or compression.

It does not prove AGC by itself, but it is evidence worth investigating.

---

## Test 4 — clipping

Create a relatively loud sound near the phone.

Watch:

```text
Peak amplitude
```

and:

```text
Clipped samples
```

If peak remains very close to:

```text
1.0
```

or the clipping percentage rises, the signal chain may be saturating.

Do not use that region for calibration.

---

# 22. Useful sanity check with synthetic PCM

The formula can be checked mathematically without any microphone.

For a sine wave:

```text
x(t) = A sin(2πft)
```

the RMS is:

```text
A / sqrt(2)
```

If:

```text
A = 0.1
```

then:

```text
RMS ≈ 0.0707107
```

and:

```text
dBFS
= 20 log10(0.0707107)
≈ -23.01 dBFS
```

This validates the **mathematics of PCM → RMS → dBFS**.

It does not validate the acoustic behaviour of the iPhone microphone.

That distinction is important:

```text
digital algorithm validation
≠
acoustic calibration
```

---

# 23. Assumptions / limitations register

Keep these attached to any results from this stage.

| ID | Assumption / limitation | Consequence |
|---|---|---|
| A1 | Browser PCM is sufficiently stable for relative experiments | Hidden processing could distort level relationships |
| A2 | Requested AGC/noise processing settings are actually respected sufficiently | The browser may not fully control the iPhone audio path |
| A3 | No acoustic calibration reference is available | dB SPL cannot be claimed |
| A4 | Phone frequency response is unknown | Response may differ greatly by frequency |
| A5 | Phone gain may vary with level | A single calibration offset may not be valid |
| A6 | Loud inputs may compress or clip | High-level readings may cease to be proportional |
| A7 | 100 ms is being used as an educational RMS window | It is not yet an IEC sound-level-meter time weighting |

---

# 24. What counts as success for Stage 1A / 1B

Do not judge this stage by whether the page displays a believable dB number.

Stage 1A/1B is successful when you can explain:

1. what a PCM sample is;
2. why the samples are approximately between -1 and +1;
3. why they are not Pascals;
4. what the sampling rate means;
5. what DC offset is;
6. why ordinary averaging does not measure sound amplitude;
7. why RMS is calculated;
8. what `20 log10(RMS)` means;
9. why the result is negative;
10. why the result is dBFS rather than dB SPL;
11. how clipping affects the result;
12. why a calibration offset has not yet been assumed.

Once those points are clear, proceed to the next stage.

---

# 25. Next stage

The next logical measurement stage is:

```text
PCM
 ↓
frequency analysis / digital filtering
 ↓
A-weighting
 ↓
A-weighted digital RMS
 ↓
dBFS(A)
```

Only after understanding that should the project introduce a physical acoustic calibration model.

The ESP32 transport can be added independently after this measurement chain is understood.

---

# 26. Stage 1C-1: provisional FFT A-weighting

The original AudioWorklet RMS/dBFS path is unchanged. In parallel, the browser
analyser now supplies two views of the same spectrum:

```text
byte FFT magnitudes  → spectrum drawing and dominant frequency
float FFT bin levels → A-weighted and unweighted energy sums
```

For every non-DC FFT bin, `a-weighting.js` calculates the continuous
A-weighting response and converts it to an energy multiplier. Linear bin
energies are summed before converting back to decibels. Bins at the analyser's
minimum level are excluded so a large number of artificial floor values cannot
create false energy.

The displayed **A-weighting effect** is:

```text
weighted spectral sum − unweighted spectral sum
```

The provisional digital estimate applies that relative correction to the
independently measured time-domain dBFS value:

```text
provisional dBFS(A) = time-domain dBFS + FFT A-weighting effect
```

This cancels the analyser's unknown absolute FFT offset, but it does not prove
that the analyser's internal window and normalization are suitable for an
absolute measurement. The asterisk in `dBFS(A)*` marks that limitation. It is
also not dBA because no acoustic reference calibration has been applied.

Run the deterministic mathematics checks with:

```sh
npm test
```

# 27. Browser tone validation plan

Use a second device and speaker as the tone source. Do not generate the tone
through the measuring phone unless speaker-to-microphone coupling is the
specific system being tested. Keep the speaker, measuring phone, volume,
orientation and distance fixed for the entire sequence.

Before recording results:

1. Use a quiet room and keep each tone safely below clipping.
2. Start the microphone and confirm the AudioContext reports `running`.
3. Confirm the displayed dominant frequency follows the source.
4. Let each tone settle for at least two seconds.
5. Record the median or typical reading over about five seconds; do not select
   a single favorable frame.
6. Repeat the sequence once to check repeatability.

At a 48 kHz sample rate and FFT size 4096, bin spacing is 11.71875 Hz. The
dominant-frequency display therefore reports the nearest strong bin rather
than necessarily the exact generator frequency.

| Source tone | Likely displayed dominant bin at 48 kHz | Expected A-weighting effect | Expected provisional relationship |
|---:|---:|---:|---|
| 100 Hz | 93.8 or 105.5 Hz | about −19.1 dB | dBFS(A)* about 19 dB below dBFS |
| 500 Hz | 503.9 Hz | about −3.25 dB | dBFS(A)* about 3 dB below dBFS |
| 1 kHz | 996.1 Hz | about 0 dB | dBFS(A)* close to dBFS |
| 2 kHz | 2003.9 Hz | about +1.20 dB | dBFS(A)* about 1 dB above dBFS |

For a strong, clean single tone, accept approximately ±1 dB around the expected
weighting effect initially; allow ±2 dB at 100 Hz. Spectral leakage, room noise,
harmonics, speaker distortion and microphone processing all add energy outside
the intended tone bin. The theoretical correction applies to a pure tone, so
the measured correction will move toward the weighting of any significant
harmonics or background noise.

The absolute unweighted dBFS readings do not need to match between frequencies:
the source speaker, room and phone microphone all have frequency-dependent
responses. The primary validation quantity is **A-weighting effect**, not the
absolute dBFS level.

Investigate a run when:

- dominant frequency does not remain near the source tone;
- the spectrum shows strong harmonics or unrelated peaks;
- clipping is nonzero;
- the 1 kHz correction is not close to 0 dB;
- results change by more than roughly 1 dB on an immediate repeat; or
- the browser reports that echo cancellation, noise suppression or automatic
  gain control remained enabled despite the request.

Passing these tone checks validates the frequency-dependent correction. It does
not validate absolute FFT normalization or turn the result into calibrated dBA.

---

# 28. Display and Diagnostics tabs

The dashboard has two views. **Display** contains the four operator-facing
levels: dBFS, provisional dBFS(A), manually calibrated dB SPL, and manually
calibrated dB SPL(A). **Diagnostics** retains the raw PCM fields, browser audio
settings, charts, spectrum, weighting effect, and calculation diagnostics.

The manual calibration uses one additive constant:

```text
dB SPL    = dBFS    + calibration constant
dB SPL(A) = dBFS(A) + calibration constant
```

For a reference source with a known sound pressure level, calculate the initial
constant as `reference dB SPL − measured dBFS`. A single constant assumes the
phone behaves as a fixed-gain linear system; it does not correct the microphone's
frequency response or automatic processing. The A-weighted values retain an
asterisk while their FFT normalization remains provisional.
