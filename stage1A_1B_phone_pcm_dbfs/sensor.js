
import {
  calculateSpectralLevels,
  createAWeightingEnergyWeights
} from "./a-weighting.js?v=20260905-3";

const $ = (id) => document.getElementById(id);

let stream = null;
let audioContext = null;
let sourceNode = null;
let workletNode = null;
let muteGain = null;

let analyser = null;
let fftDisplayData = null;
let fftMeasurementData = null;
let aWeightingEnergyWeights = null;
let latestDbfs = null;

const history = [];
const HISTORY_POINTS = 150;

function finiteText(value, digits = 6) {
  return Number.isFinite(value)
    ? value.toFixed(digits)
    : "—";
}

function setStatus(text, cls = "") {
  $("micStatus").textContent = text;
  $("micStatus").className =
    `value ${cls}`.trim();
}

/*
==========================================================
dBFS HISTORY
==========================================================
*/

function drawHistory() {
  const canvas = $("historyChart");
  const ctx = canvas.getContext("2d");

  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);

  if (history.length < 2) return;

  const vals =
    history.filter(Number.isFinite);

  if (vals.length < 2) return;

  let min = Math.min(...vals);
  let max = Math.max(...vals);

  /*
    Maintain at least a 10 dB display range.
  */
  if (max - min < 10) {
    const mid = (min + max) / 2;

    min = mid - 5;
    max = mid + 5;
  }

  ctx.beginPath();

  history.forEach((v, i) => {
    if (!Number.isFinite(v)) return;

    const x =
      i *
      (w - 1) /
      Math.max(
        1,
        history.length - 1
      );

    const y =
      h -
      ((v - min) /
        (max - min)) *
        h;

    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  });

  ctx.stroke();
}

/*
==========================================================
PCM WAVEFORM
==========================================================
*/

function drawWaveform(samples) {
  const canvas = $("waveform");
  const ctx = canvas.getContext("2d");

  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);

  /*
    Draw zero line.
  */
  ctx.beginPath();

  ctx.moveTo(
    0,
    h / 2
  );

  ctx.lineTo(
    w,
    h / 2
  );

  ctx.stroke();

  if (
    !samples ||
    samples.length < 2
  ) {
    return;
  }

  /*
    Find largest absolute amplitude
    so the waveform can be scaled
    to the canvas.
  */
  let maxAbs = 0;

  for (const sample of samples) {
    maxAbs =
      Math.max(
        maxAbs,
        Math.abs(sample)
      );
  }

  /*
    Prevent excessive zoom or
    divide-by-zero.
  */
  maxAbs =
    Math.max(
      maxAbs,
      0.001
    );

  ctx.beginPath();

  samples.forEach(
    (sample, i) => {

      const x =
        i *
        (w - 1) /
        Math.max(
          1,
          samples.length - 1
        );

      const y =
        h / 2 -
        (
          sample /
          maxAbs
        ) *
        (h * 0.44);

      if (i === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    }
  );

  ctx.stroke();
}

/*
==========================================================
RMS / dBFS DISPLAY
==========================================================
*/

function renderMeasurement(m) {
  latestDbfs = m.dbfs;
  $("sampleRate").textContent =
    `${m.sampleRate} Hz`;

  $("blockSize").textContent =
    `${m.blockSize}`;

  $("latestSample").textContent =
    finiteText(
      m.latestSample,
      7
    );

  $("dcOffset").textContent =
    finiteText(
      m.mean,
      8
    );

  $("rms").textContent =
    finiteText(
      m.rms,
      8
    );

  $("peak").textContent =
    finiteText(
      m.peak,
      7
    );

  $("dbfs").textContent =
    Number.isFinite(m.dbfs)
      ? `${m.dbfs.toFixed(2)} dBFS`
      : "−∞ dBFS";

  $("clipPercent").textContent =
    `${(
      100 *
      m.clippingFraction
    ).toFixed(3)} %`;

  /*
    Store dBFS value for history.
  */
  history.push(
    m.dbfs
  );

  while (
    history.length >
    HISTORY_POINTS
  ) {
    history.shift();
  }

  drawHistory();

  drawWaveform(
    m.waveform
  );
}

/*
==========================================================
FFT SPECTRUM
==========================================================
*/

function drawSpectrum() {

  /*
    If microphone/audio has stopped,
    exit the animation loop.
  */
  if (
    !analyser ||
    !audioContext ||
    !fftDisplayData ||
    !fftMeasurementData ||
    !aWeightingEnergyWeights
  ) {
    return;
  }

  /*
    Schedule next screen refresh.
  */
  requestAnimationFrame(
    drawSpectrum
  );

  const fftCanvas =
    $("fftCanvas");

  const dominantFrequencyEl =
    $("dominantFrequency");

  if (
    !fftCanvas ||
    !dominantFrequencyEl
  ) {
    return;
  }

  const fftCtx =
    fftCanvas.getContext("2d");

  /*
    Fill fftData with the latest frequency-domain
    magnitudes. Byte data is consistently supported
    across desktop and mobile Web Audio implementations;
    0 maps to minDecibels and 255 maps to maxDecibels.
  */
  analyser.getByteFrequencyData(
    fftDisplayData
  );

  analyser.getFloatFrequencyData(
    fftMeasurementData
  );

  const width =
    fftCanvas.width;

  const height =
    fftCanvas.height;

  /* Paint an explicit background; do not depend on page/theme defaults. */
  fftCtx.fillStyle =
    "#ffffff";

  fftCtx.fillRect(
    0,
    0,
    width,
    height
  );

  const sampleRate =
    audioContext.sampleRate;

  const nyquist =
    sampleRate / 2;

  /*
    For learning/debugging,
    display only 0–5000 Hz.
  */
  const maxDisplayFrequency =
    5000;

  /*
    Convert display frequency
    limit into FFT-bin count.
  */
  const maxBin =
    Math.min(
      fftDisplayData.length,
      Math.floor(
        maxDisplayFrequency /
        nyquist *
        fftDisplayData.length
      )
    );

  /*
    Vertical display limits.

    0 dBFS-like:
      top of graph

    -120 dB:
      bottom of graph
  */
  const minDisplayDb =
    -120;

  const maxDisplayDb =
    0;

  let strongestBin =
    0;

  let strongestValue =
    -Infinity;

  /*
    Draw spectrum as a continuous line.
    This is much easier to inspect
    than hundreds of thin bars.
  */
  fftCtx.beginPath();

  for (
    let i = 0;
    i < maxBin;
    i++
  ) {

    const magnitude =
      fftDisplayData[i];

    const db =
      minDisplayDb +
      (magnitude / 255) *
      (maxDisplayDb - minDisplayDb);

    /*
      Ignore the first few bins
      when looking for dominant
      frequency.

      Bin 0 = DC.
    */
    if (
      i > 1 &&
      magnitude > strongestValue
    ) {
      strongestValue =
        magnitude;

      strongestBin =
        i;
    }

    const x =
      i /
      Math.max(
        1,
        maxBin - 1
      ) *
      width;

    let normalized =
      (
        db -
        minDisplayDb
      ) /
      (
        maxDisplayDb -
        minDisplayDb
      );

    normalized =
      Math.max(
        0,
        Math.min(
          1,
          normalized
        )
      );

    /*
      Keep the trace within the drawable pixel range.
      Canvas coordinates end at height - 1, so using
      height would hide values at the noise floor under
      the lower edge of the canvas.
    */
    const drawableHeight =
      height - 1;

    const y =
      drawableHeight -
      normalized *
      drawableHeight;

    if (i === 0) {
      fftCtx.moveTo(
        x,
        y
      );
    } else {
      fftCtx.lineTo(
        x,
        y
      );
    }
  }

  /*
    Set the spectrum styling explicitly so it remains
    visible regardless of inherited page styling.
  */
  fftCtx.strokeStyle =
    "#2563eb";

  fftCtx.lineWidth =
    2;

  fftCtx.stroke();

  /*
    Calculate frequency spacing
    between FFT bins.

    Δf = sampleRate / fftSize

    Example:

    48000 / 4096
    ≈ 11.72 Hz per bin
  */
  const binFrequency =
    sampleRate /
    analyser.fftSize;

  if (strongestValue <= 0) {
    dominantFrequencyEl.textContent =
      `Waiting for audio… (${audioContext.state})`;
  } else {
    const dominantFrequency =
      strongestBin *
      binFrequency;

    dominantFrequencyEl.textContent =
      `Dominant frequency: ${
        dominantFrequency.toFixed(1)
      } Hz`;
  }

  const spectralLevels =
    calculateSpectralLevels(
      fftMeasurementData,
      aWeightingEnergyWeights,
      analyser.minDecibels
    );

  if (spectralLevels) {
    $("aWeightingEffect").textContent =
      `${spectralLevels.weightingDifferenceDb.toFixed(2)} dB`;

    const provisionalAWeightedDbfs =
      Number.isFinite(latestDbfs)
        ? latestDbfs + spectralLevels.weightingDifferenceDb
        : null;

    $("aWeightedDbfs").textContent =
      Number.isFinite(provisionalAWeightedDbfs)
        ? `${provisionalAWeightedDbfs.toFixed(2)} dBFS(A)*`
        : "—";

    $("spectralDiagnostics").textContent =
      `Spectral bins used: ${spectralLevels.includedBins}; ` +
      `raw unweighted sum: ${spectralLevels.unweightedDb.toFixed(2)} dB; ` +
      `raw A-weighted sum: ${spectralLevels.weightedDb.toFixed(2)} dB`;
  } else {
    $("aWeightingEffect").textContent = "—";
    $("aWeightedDbfs").textContent = "Waiting for spectral data…";
    $("spectralDiagnostics").textContent =
      "No FFT bins are above the analyser floor.";
  }

  /*
    OPTIONAL DEBUGGING:

    At 48 kHz and FFT size 4096:

    bin 10  ≈ 117 Hz
    bin 50  ≈ 586 Hz
    bin 85  ≈ 996 Hz
    bin 170 ≈ 1992 Hz

    Uncomment this if you want
    to inspect FFT values in the
    browser console.

    console.log(
      "117 Hz:",
      fftDisplayData[10],
      "586 Hz:",
      fftDisplayData[50],
      "996 Hz:",
      fftDisplayData[85],
      "1992 Hz:",
      fftDisplayData[170]
    );
  */
}

/*
==========================================================
START MICROPHONE
==========================================================
*/

async function startMicrophone() {

  /*
    Microphone access on iPhone
    requires a secure HTTPS context.
  */
  if (!window.isSecureContext) {

    alert(
      "This page is not running in a secure context. " +
      "On iPhone, open it over HTTPS."
    );

    return;
  }

  if (
    !navigator.mediaDevices
      ?.getUserMedia
  ) {

    alert(
      "getUserMedia() is unavailable in this browser."
    );

    return;
  }

  $("startBtn").disabled =
    true;

  try {

    /*
    ======================================================
    REQUEST MICROPHONE
    ======================================================
    */

    const supported =
      navigator.mediaDevices
        .getSupportedConstraints();

    const requestedAudio = {
      channelCount: 1,
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false
    };

    stream =
      await navigator.mediaDevices
        .getUserMedia({
          audio:
            requestedAudio
        });

    const track =
      stream
        .getAudioTracks()[0];

    const settings =
      track.getSettings();

    /*
      Display browser-reported
      audio settings.
    */
    $("audioSettings")
      .textContent =
      JSON.stringify(
        {
          requested:
            requestedAudio,

          browserSaysConstraintIsSupported:
            {
              channelCount:
                Boolean(
                  supported
                    .channelCount
                ),

              echoCancellation:
                Boolean(
                  supported
                    .echoCancellation
                ),

              noiseSuppression:
                Boolean(
                  supported
                    .noiseSuppression
                ),

              autoGainControl:
                Boolean(
                  supported
                    .autoGainControl
                )
            },

          reportedTrackSettings:
            settings
        },
        null,
        2
      );

    /*
    ======================================================
    AUDIO CONTEXT
    ======================================================
    */

    audioContext =
      new AudioContext();

    await audioContext.resume();

    /*
    ======================================================
    FFT ANALYSER
    ======================================================
    */

    analyser =
      audioContext
        .createAnalyser();

    /*
      Number of time-domain samples
      used by each FFT.
    */
    analyser.fftSize =
      4096;

    /*
      No averaging between
      successive spectra.

      This makes the spectrum
      respond immediately.
    */
    analyser.smoothingTimeConstant =
      0;

    /*
      Explicit analyser display
      / output limits.
    */
    analyser.minDecibels =
      -120;

    analyser.maxDecibels =
      0;

    /*
    fftSize = 4096

      frequencyBinCount =
      4096 / 2 =
      2048 frequency bins. Byte magnitudes avoid
      non-finite -Infinity values before audio arrives.
    */
    fftDisplayData =
      new Uint8Array(
        analyser
          .frequencyBinCount
      );

    fftMeasurementData =
      new Float32Array(
        analyser.frequencyBinCount
      );

    aWeightingEnergyWeights =
      createAWeightingEnergyWeights(
        audioContext.sampleRate,
        analyser.fftSize,
        analyser.frequencyBinCount
      );

    /*
    ======================================================
    LOAD AUDIOWORKLET
    ======================================================
    */

    await audioContext
      .audioWorklet
      .addModule(
        "./pcm-processor.js?v=20260905-2"
      );

    /*
    ======================================================
    CREATE AUDIO NODES
    ======================================================
    */

    sourceNode =
      audioContext
        .createMediaStreamSource(
          stream
        );

    workletNode =
      new AudioWorkletNode(
        audioContext,
        "pcm-meter"
      );

    /*
      Zero-gain output prevents
      microphone feedback while
      keeping the processing graph
      active.
    */
    muteGain =
      audioContext
        .createGain();

    muteGain.gain.value =
      0;

    /*
    ======================================================
    AUDIO GRAPH
    ======================================================

                       ┌→ AnalyserNode ──┐
                       │                 │
      Microphone ──────┤                 ├→ muteGain → output
                       │                 │
                       └→ AudioWorklet ──┘

    Both paths are therefore connected
    to an active destination.

    muteGain = 0,
    so no microphone audio is heard.
    */

    sourceNode.connect(
      analyser
    );

    sourceNode.connect(
      workletNode
    );

    /*
      IMPORTANT CHANGE:

      The analyser is also connected
      onward to the active graph.

      This helps ensure Safari keeps
      the analyser branch processing.
    */
    analyser.connect(
      muteGain
    );

    workletNode.connect(
      muteGain
    );

    muteGain.connect(
      audioContext.destination
    );

    /*
      Some mobile browsers can leave a newly created context suspended
      until after its graph is connected, even when resume() was called
      earlier in the same button interaction.
    */
    if (
      audioContext.state !==
      "running"
    ) {
      await audioContext.resume();
    }

    /*
    ======================================================
    RECEIVE RMS / dBFS DATA
    ======================================================
    */

    workletNode
      .port
      .onmessage =
      (event) => {

        renderMeasurement(
          event.data
        );
      };

    /*
    ======================================================
    START FFT VISUALISATION
    ======================================================
    */

    $("dominantFrequency").textContent =
      `Starting spectrum… (${audioContext.state})`;

    drawSpectrum();

    /*
    ======================================================
    UPDATE UI
    ======================================================
    */

    $("sampleRate")
      .textContent =
      `${audioContext.sampleRate} Hz`;

    setStatus(
      "Running",
      "ok"
    );

    $("stopBtn").disabled =
      false;

  } catch (err) {

    console.error(err);

    setStatus(
      "Error",
      "bad"
    );

    $("startBtn").disabled =
      false;

    alert(
      `Microphone could not start: ${err.message}`
    );
  }
}

/*
==========================================================
STOP MICROPHONE
==========================================================
*/

async function stopMicrophone() {

  /*
    Disconnect microphone source.
  */
  if (sourceNode) {

    sourceNode.disconnect();

    sourceNode =
      null;
  }

  /*
    Disconnect RMS/dBFS worklet.
  */
  if (workletNode) {

    workletNode.disconnect();

    workletNode =
      null;
  }

  /*
    Disconnect FFT analyser.
  */
  if (analyser) {

    analyser.disconnect();

    analyser =
      null;
  }

  /*
    Clearing the FFT buffers
    causes drawSpectrum()
    to stop on the next
    animation frame.
  */
  fftDisplayData = null;
  fftMeasurementData = null;
  aWeightingEnergyWeights = null;
  latestDbfs = null;

  /*
    Disconnect mute output.
  */
  if (muteGain) {

    muteGain.disconnect();

    muteGain =
      null;
  }

  /*
    Stop physical microphone.
  */
  if (stream) {

    stream
      .getTracks()
      .forEach(
        track =>
          track.stop()
      );

    stream =
      null;
  }

  /*
    Close Web Audio context.
  */
  if (audioContext) {

    await audioContext.close();

    audioContext =
      null;
  }

  /*
    Reset UI.
  */
  setStatus(
    "Stopped"
  );

  $("startBtn").disabled =
    false;

  $("stopBtn").disabled =
    true;

  /*
    Reset dominant-frequency
    display.
  */
  const dominantFrequencyEl =
    $("dominantFrequency");

  if (
    dominantFrequencyEl
  ) {
    dominantFrequencyEl
      .textContent =
      "Dominant frequency: —";
  }

  $("aWeightingEffect").textContent = "—";
  $("aWeightedDbfs").textContent = "—";
  $("spectralDiagnostics").textContent =
    "Start the microphone to calculate the spectral weighting correction.";

  /*
    Clear FFT graph.
  */
  const fftCanvas =
    $("fftCanvas");

  if (fftCanvas) {

    const fftCtx =
      fftCanvas
        .getContext("2d");

    fftCtx.clearRect(
      0,
      0,
      fftCanvas.width,
      fftCanvas.height
    );
  }
}

/*
==========================================================
BUTTON EVENTS
==========================================================
*/

$("startBtn")
  .addEventListener(
    "click",
    startMicrophone
  );

$("stopBtn")
  .addEventListener(
    "click",
    stopMicrophone
  );
