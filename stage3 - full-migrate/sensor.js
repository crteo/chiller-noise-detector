import {firebaseConfig} from "./firebase-config.js";
import {createFirebaseConnection} from "./firebase-client.js";
import {validateMeasurement, freshness, createHistory, HISTORY_MS} from "./measurement.js";
import {demoMeasurement} from "./demo.js";
const $ = id => document.getElementById(id);
const query = new URLSearchParams(location.search);
const demo = query.get("demo") === "1";
const diagnostics = query.get("mode") === "diagnostics";
const historyStore = createHistory();
const remoteHistory = historyStore.points;
const REMOTE_HISTORY_MS = HISTORY_MS;
let latestSpectrumBands = [], maxHz = 24000, latest = null, connection = null;
let transport = "connecting", transportDetail = "", demoTimer, demoSequence = 0;
const text = (id, value) => { $(id).textContent = value; };
const level = (v, unit = "") => Number.isFinite(v) ? `${v.toFixed(2)} ${unit}` : "—";
const finiteOrNull = value => Number.isFinite(value) ? value : null;
if (!diagnostics) document.body.classList.add("remote-display");
document.title = "Chiller Noise Monitor";
$("calibrationConstant").readOnly = true;
$("remoteDisplayLink").href = demo ? "./?demo=1" : "./";
$("remoteDisplayLink").textContent = "Open tablet display";
function selectTab(button) {
  for (const tab of document.querySelectorAll('[role="tab"]')) {
    const selected = tab === button;
    tab.setAttribute("aria-selected", String(selected)); tab.tabIndex = selected ? 0 : -1;
    $(tab.getAttribute("aria-controls")).hidden = !selected;
  }
}
const tabs = [...document.querySelectorAll('[role="tab"]')];
for (const tab of tabs) {
  tab.addEventListener("click", () => selectTab(tab));
  tab.addEventListener("keydown", event => {
    const index = tabs.indexOf(tab);
    const next = event.key === "ArrowRight" ? tabs[(index+1)%tabs.length]
      : event.key === "ArrowLeft" ? tabs[(index+tabs.length-1)%tabs.length]
      : event.key === "Home" ? tabs[0] : event.key === "End" ? tabs.at(-1) : null;
    if (next) {event.preventDefault(); next.focus(); selectTab(next);}
  });
}
if (diagnostics) selectTab($("diagnosticsTab"));
function receive(raw) {
  if (raw === null) {
    latest = null; historyStore.clear(); latestSpectrumBands = [];
    transportDetail = "No measurement at /current"; render(); return;
  }
  try {
    const m = validateMeasurement(raw);
    if (!historyStore.add(m)) return;
    latest = m; transportDetail = "";
    latestSpectrumBands = m.spectrum.bands; maxHz = m.spectrum.maxHz;
    renderDiagnostics(m);
    render();
  } catch (error) {
    latest = null; latestSpectrumBands = []; transportDetail = error.message; render();
  }
}
function renderDiagnostics(m) {
  const v = m.values, d = m.diagnostics;
  for (const [id, value, unit] of [["displayDbfs",v.dbfs,"dBFS"],["displayAWeightedDbfs",v.aWeightedDbfs,"dBFS(A)"],
    ["displayDbSpl",v.dbSpl,"dB SPL"],["displayDbSplA",v.dbSplA,"dB SPL(A)"],
    ["dbfs",v.dbfs,"dBFS"],["aWeightedDbfs",v.aWeightedDbfs,"dBFS(A)"],
    ["aWeightingEffect",v.dbfs === null || v.aWeightedDbfs === null ? null : v.aWeightedDbfs-v.dbfs,"dB"]]) text(id,level(value,unit));
  $("calibrationConstant").value = v.calibrationConstant ?? "";
  text("calibrationStatus", v.calibrationConstant === null ? "Uncalibrated: configure the ESP32 calibration constant."
    : `Device calibration: ${v.calibrationConstant.toFixed(2)} dB. Change in firmware and reflash.`);
  text("sampleRate",`${m.status.sampleRate} Hz`); text("blockSize",d.blockSize);
  text("windowMs",`${m.windowMs} ms`);
  for (const [id,key] of [["latestSample","latestSample"],["dcOffset","mean"],["rms","rms"],["peak","peak"]]) text(id,d[key].toFixed(7));
  text("clipPercent",`${(m.status.clippingFraction*100).toFixed(3)} %`);
  text("dominantFrequency",`Dominant frequency: ${level(m.spectrum.dominantHz,"Hz")}`);
  text("audioSettings",JSON.stringify({deviceId:m.deviceId, firmwareVersion:m.firmwareVersion,
    bootId:m.bootId, ...m.status, droppedSamples:d.droppedSamples, captureErrors:d.captureErrors}, null, 2));
  text("spectralDiagnostics",`${m.spectrum.fftSize}-point Hann FFT; ${m.status.sampleRate/m.spectrum.fftSize} Hz/bin; 64 linear bands; display range −120 to 0 dBFS/bin. A-weighting uses an independent stateful 48 kHz filter.`);
  drawWaveform(m.waveform);
  const c = $("fftCanvas"), ctx = c.getContext("2d"); ctx.clearRect(0,0,c.width,c.height);
  ctx.strokeStyle = "#3b82f6"; ctx.beginPath();
  m.spectrum.bands.forEach((v,i) => {const x=i*c.width/63,y=c.height*(1-v/255); i ? ctx.lineTo(x,y) : ctx.moveTo(x,y);}); ctx.stroke();
}
function render() {
  historyStore.prune();
  const state = freshness(latest);
  const live = state === "live" && transport === "connected";
  const labels = {live:"INMP441 connected", stale:"Data stale", offline:"Sensor disconnected",
    waiting:"Waiting for ESP32", "clock-error":"Sensor clock error", "capture-error":"I2S capture error"};
  const label = transport === "connected" ? labels[state]
    : {connecting:"Connecting…", reconnecting:"Reconnecting…", paused:"Display paused", error:"Configuration error"}[transport];
  text("tabletConnectionStatus",`${demo ? "DEMO · " : ""}${label}`);
  $("tabletStatusBar").className = `mic-status-bar ${live ? "" : state === "stale" ? "stale" : "offline"}`;
  const detail = transportDetail || (latest ? `${latest.deviceId} · Last sample ${Math.max(0,(Date.now()-latest.measuredAt)/1000).toFixed(1)} seconds ago` : "Waiting for the first measurement");
  text("tabletSessionDetails",detail); text("remoteSessionDetails",detail);
  text("remoteConnectionStatus",label); text("micStatus",label);
  renderTabletDashboard(live ? latest.values : {});
  if (!live) {
    text("tabletNoiseStatus",`Status: ${label}`);
    for (const id of ["displayDbfs","displayAWeightedDbfs","displayDbSpl","displayDbSplA","dbfs","aWeightedDbfs","aWeightingEffect"]) text(id,"—");
  }
  const canvas = $("historyChart"), ctx = canvas.getContext("2d"); ctx.clearRect(0,0,canvas.width,canvas.height);
  ctx.strokeStyle="#3b82f6"; ctx.beginPath();
  remoteHistory.forEach((p,i) => {
    if (!Number.isFinite(p.dbfs)) return;
    const x=canvas.width*(1-(Date.now()-p.time)/HISTORY_MS), y=canvas.height*(1-(p.dbfs+120)/120);
    if (!i || p.time-remoteHistory[i-1].time>2000 || p.identity!==remoteHistory[i-1].identity) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  }); ctx.stroke();
}
function start() {
  if (connection || demoTimer) return;
  $("startBtn").disabled=true; $("stopBtn").disabled=false;
  if (demo) {
    transport="connected"; receive(demoMeasurement(demoSequence++));
    demoTimer=setInterval(() => receive(demoMeasurement(demoSequence++)),200);
  } else connection=createFirebaseConnection({config:firebaseConfig, onMeasurement:receive,
    onStatus:status => {transport=status.state; transportDetail=status.detail; render();}});
}
function stop() {
  connection?.close(); connection=null; clearInterval(demoTimer); demoTimer=null;
  transport="paused"; $("startBtn").disabled=false; $("stopBtn").disabled=true; render();
}
$("startBtn").addEventListener("click",start); $("stopBtn").addEventListener("click",stop);
window.addEventListener("pagehide",stop);
window.addEventListener("pageshow", event => {if(event.persisted) start();});
setInterval(render,500);
start();
function renderTabletDashboard(values) {
  const calibratedAWeighted = finiteOrNull(values.dbSplA);
  const heroValue = $("tabletDbA");
  heroValue.innerHTML = Number.isFinite(calibratedAWeighted)
    ? `${calibratedAWeighted.toFixed(1)}<span class="hero-unit"> dB(A)</span>`
    : `—<span class="hero-unit"> dB(A)</span>`;

  const noiseStatus = $("tabletNoiseStatus");
  if (!Number.isFinite(calibratedAWeighted)) {
    heroValue.className = "hero-value";
    noiseStatus.className = "noise-status";
    noiseStatus.textContent = "Status: Waiting for calibrated measurement";
  } else if (calibratedAWeighted < 50) {
    heroValue.className = "hero-value level-safe";
    noiseStatus.className = "noise-status safe";
    noiseStatus.textContent = "Status: Safe";
  } else if (calibratedAWeighted <= 80) {
    heroValue.className = "hero-value level-loud";
    noiseStatus.className = "noise-status loud";
    noiseStatus.textContent = "Status: Loud noise";
  } else {
    heroValue.className = "hero-value level-harmful";
    noiseStatus.className = "noise-status harmful";
    noiseStatus.textContent = "Status: Harmful noise levels";
  }

  const historyValues = remoteHistory.map(point => point.value);
  const minimum = historyValues.length ? Math.min(...historyValues) : null;
  const maximum = historyValues.length ? Math.max(...historyValues) : null;
  const average = historyValues.length
    ? historyValues.reduce((sum, value) => sum + value, 0) / historyValues.length
    : null;

  $("tabletMin").textContent = Number.isFinite(minimum) ? minimum.toFixed(1) : "—";
  $("tabletAvg").textContent = Number.isFinite(average) ? average.toFixed(1) : "—";
  $("tabletMax").textContent = Number.isFinite(maximum) ? maximum.toFixed(1) : "—";

  drawTabletFrequencyChart();
  drawTabletHistoryChart();
}

function drawTabletFrequencyChart() {
  const canvas = $("tabletFrequencyChart");
  const context = canvas.getContext("2d");
  const width = canvas.width;
  const height = canvas.height;

  context.clearRect(0, 0, width, height);
  context.strokeStyle = "#d7dde6";
  context.setLineDash([5, 6]);
  for (let x = 0; x <= width; x += width / 4) {
    context.beginPath();
    context.moveTo(x, 12);
    context.lineTo(x, height - 28);
    context.stroke();
  }
  context.setLineDash([]);

  if (latestSpectrumBands.length) {
    const barWidth = width / latestSpectrumBands.length;
    for (let index = 0; index < latestSpectrumBands.length; index += 1) {
      const normalized = latestSpectrumBands[index] / 255;
      const barHeight = normalized * (height - 42);
      context.fillStyle = index < latestSpectrumBands.length / 4
        ? "#22c55e"
        : "#3b82f6";
      context.fillRect(
        index * barWidth,
        height - 27 - barHeight,
        Math.max(1, barWidth - 1),
        barHeight
      );
    }
  }

  context.fillStyle = "#7b8494";
  context.font = "12px system-ui";
  context.fillText("0 Hz", 2, height - 7);
  context.fillText(`${maxHz / 4000} kHz`, width * .25 - 16, height - 7);
  context.fillText(`${maxHz / 2000} kHz`, width * .5 - 19, height - 7);
  context.fillText(`${maxHz / 1000} kHz`, width - 48, height - 7);
}

function drawTabletHistoryChart() {
  const canvas = $("tabletHistoryChart");
  const context = canvas.getContext("2d");
  const width = canvas.width;
  const height = canvas.height;
  const top = 14;
  const bottom = height - 30;
  const minDb = 30;
  const maxDb = 120;
  const now = Date.now();

  context.clearRect(0, 0, width, height);
  context.strokeStyle = "#e1e5eb";
  context.fillStyle = "#7b8494";
  context.font = "12px system-ui";

  for (const level of [30, 60, 90, 120]) {
    const y = bottom - (level - minDb) / (maxDb - minDb) * (bottom - top);
    context.beginPath();
    context.moveTo(38, y);
    context.lineTo(width, y);
    context.stroke();
    context.fillText(String(level), 5, y + 4);
  }

  if (remoteHistory.length > 1) {
    context.beginPath();
    remoteHistory.forEach((point, index) => {
      const x = 38 + Math.max(0, 1 - (now - point.time) / REMOTE_HISTORY_MS) * (width - 38);
      const bounded = Math.max(minDb, Math.min(maxDb, point.value));
      const y = bottom - (bounded - minDb) / (maxDb - minDb) * (bottom - top);
      if (index === 0 || point.identity !== remoteHistory[index - 1].identity || point.time - remoteHistory[index - 1].time > 2000) context.moveTo(x, y);
      else context.lineTo(x, y);
    });
    context.strokeStyle = "#3b82f6";
    context.lineWidth = 3;
    context.stroke();
  }

  context.fillStyle = "#7b8494";
  context.fillText("−30s", 38, height - 7);
  context.fillText("Now", width - 25, height - 7);
}

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
