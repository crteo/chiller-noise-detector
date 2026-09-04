const $ = (id) => document.getElementById(id);

let stream = null;
let audioContext = null;
let sourceNode = null;
let workletNode = null;
let muteGain = null;

const history = [];
const HISTORY_POINTS = 150;

function finiteText(value, digits = 6) {
  return Number.isFinite(value) ? value.toFixed(digits) : "—";
}

function setStatus(text, cls = "") {
  $("micStatus").textContent = text;
  $("micStatus").className = `value ${cls}`.trim();
}

function drawHistory() {
  const canvas = $("historyChart");
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);

  if (history.length < 2) return;

  const vals = history.filter(Number.isFinite);
  if (vals.length < 2) return;

  let min = Math.min(...vals);
  let max = Math.max(...vals);

  if (max - min < 10) {
    const mid = (min + max) / 2;
    min = mid - 5;
    max = mid + 5;
  }

  ctx.beginPath();

  history.forEach((v, i) => {
    if (!Number.isFinite(v)) return;

    const x = i * (w - 1) / Math.max(1, history.length - 1);
    const y = h - ((v - min) / (max - min)) * h;

    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });

  ctx.stroke();
}

function drawWaveform(samples) {
  const canvas = $("waveform");
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);

  ctx.beginPath();
  ctx.moveTo(0, h / 2);
  ctx.lineTo(w, h / 2);
  ctx.stroke();

  if (!samples || samples.length < 2) return;

  let maxAbs = 0;
  for (const x of samples) maxAbs = Math.max(maxAbs, Math.abs(x));
  maxAbs = Math.max(maxAbs, 0.001);

  ctx.beginPath();

  samples.forEach((sample, i) => {
    const x = i * (w - 1) / Math.max(1, samples.length - 1);
    const y = h / 2 - (sample / maxAbs) * (h * 0.44);

    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });

  ctx.stroke();
}

function renderMeasurement(m) {
  $("sampleRate").textContent = `${m.sampleRate} Hz`;
  $("blockSize").textContent = `${m.blockSize}`;
  $("latestSample").textContent = finiteText(m.latestSample, 7);
  $("dcOffset").textContent = finiteText(m.mean, 8);
  $("rms").textContent = finiteText(m.rms, 8);
  $("peak").textContent = finiteText(m.peak, 7);
  $("dbfs").textContent = Number.isFinite(m.dbfs)
    ? `${m.dbfs.toFixed(2)} dBFS`
    : "−∞ dBFS";
  $("clipPercent").textContent = `${(100 * m.clippingFraction).toFixed(3)} %`;

  history.push(m.dbfs);
  while (history.length > HISTORY_POINTS) history.shift();

  drawHistory();
  drawWaveform(m.waveform);
}

async function startMicrophone() {
  if (!window.isSecureContext) {
    alert(
      "This page is not running in a secure context. On iPhone, open it over HTTPS. " +
      "See README.md for deployment instructions."
    );
    return;
  }

  if (!navigator.mediaDevices?.getUserMedia) {
    alert("getUserMedia() is unavailable in this browser.");
    return;
  }

  $("startBtn").disabled = true;

  try {
    const supported = navigator.mediaDevices.getSupportedConstraints();

    const requestedAudio = {
      channelCount: 1,
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false
    };

    stream = await navigator.mediaDevices.getUserMedia({
      audio: requestedAudio
    });

    const track = stream.getAudioTracks()[0];
    const settings = track.getSettings();

    $("audioSettings").textContent = JSON.stringify({
      requested: requestedAudio,
      browserSaysConstraintIsSupported: {
        channelCount: Boolean(supported.channelCount),
        echoCancellation: Boolean(supported.echoCancellation),
        noiseSuppression: Boolean(supported.noiseSuppression),
        autoGainControl: Boolean(supported.autoGainControl)
      },
      reportedTrackSettings: settings
    }, null, 2);

    audioContext = new AudioContext();
    await audioContext.resume();

    await audioContext.audioWorklet.addModule("./pcm-processor.js");

    sourceNode = audioContext.createMediaStreamSource(stream);
    workletNode = new AudioWorkletNode(audioContext, "pcm-meter");

    // AudioWorklet must remain connected to an active graph.
    // Gain = 0 prevents microphone sound being played through the phone speaker.
    muteGain = audioContext.createGain();
    muteGain.gain.value = 0;

    sourceNode.connect(workletNode);
    workletNode.connect(muteGain);
    muteGain.connect(audioContext.destination);

    workletNode.port.onmessage = (event) => {
      renderMeasurement(event.data);
    };

    $("sampleRate").textContent = `${audioContext.sampleRate} Hz`;
    setStatus("Running", "ok");
    $("stopBtn").disabled = false;

  } catch (err) {
    console.error(err);
    setStatus("Error", "bad");
    $("startBtn").disabled = false;
    alert(`Microphone could not start: ${err.message}`);
  }
}

async function stopMicrophone() {
  if (sourceNode) {
    sourceNode.disconnect();
    sourceNode = null;
  }

  if (workletNode) {
    workletNode.disconnect();
    workletNode = null;
  }

  if (muteGain) {
    muteGain.disconnect();
    muteGain = null;
  }

  if (stream) {
    stream.getTracks().forEach(track => track.stop());
    stream = null;
  }

  if (audioContext) {
    await audioContext.close();
    audioContext = null;
  }

  setStatus("Stopped");
  $("startBtn").disabled = false;
  $("stopBtn").disabled = true;
}

$("startBtn").addEventListener("click", startMicrophone);
$("stopBtn").addEventListener("click", stopMicrophone);
