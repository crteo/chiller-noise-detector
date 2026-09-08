export const HISTORY_MS = 30000;
const numberOrNull = v => v === null || Number.isFinite(v);
export function validateMeasurement(m) {
  const fail = () => { throw new Error("Invalid or unsupported ESP32 measurement"); };
  if (!m || m.version !== 2 || typeof m.deviceId !== "string" || !m.deviceId ||
      typeof m.bootId !== "string" || !m.bootId || !Number.isSafeInteger(m.sequence) || m.sequence < 0 ||
      !Number.isFinite(m.measuredAt) || m.measuredAt <= 0 ||
      !Number.isFinite(m.uploadedAt) || m.uploadedAt <= 0 ||
      !Number.isFinite(m.windowMs) || m.windowMs <= 0 || m.windowMs > 10000 ||
      !m.values || !m.status || !m.spectrum || !m.diagnostics) fail();
  for (const key of ["dbfs", "aWeightedDbfs", "dbSpl", "dbSplA", "calibrationConstant"]) {
    if (!numberOrNull(m.values[key])) fail();
  }
  for (const [digital, acoustic] of [["dbfs", "dbSpl"], ["aWeightedDbfs", "dbSplA"]]) {
    const expected = m.values[digital] === null || m.values.calibrationConstant === null
      ? null : m.values[digital] + m.values.calibrationConstant;
    if (expected === null ? m.values[acoustic] !== null
      : !Number.isFinite(m.values[acoustic]) || Math.abs(expected - m.values[acoustic]) > 0.05) fail();
  }
  if (typeof m.status.capture !== "string" || typeof m.status.clockSynced !== "boolean" ||
      !Number.isFinite(m.status.sampleRate) || m.status.sampleRate < 8000 || m.status.sampleRate > 96000 ||
      !Number.isFinite(m.status.clippingFraction) || m.status.clippingFraction < 0 || m.status.clippingFraction > 1) fail();
  const s = m.spectrum;
  if (!Array.isArray(s.bands) || s.bands.length !== 64 || !s.bands.every(v => Number.isFinite(v) && v >= 0 && v <= 255) ||
      s.maxHz !== m.status.sampleRate / 2 || s.fftSize !== 4096 ||
      !Number.isFinite(s.measuredAt) || Math.abs(s.measuredAt - m.measuredAt) > m.windowMs ||
      !numberOrNull(s.dominantHz)) fail();
  for (const key of ["rms", "peak", "mean", "latestSample", "blockSize", "droppedSamples", "captureErrors"]) {
    if (!Number.isFinite(m.diagnostics[key])) fail();
  }
  if (!Array.isArray(m.waveform) || m.waveform.length > 256 ||
      !m.waveform.every(v => Number.isFinite(v) && Math.abs(v) <= 1)) fail();
  return m;
}
export function freshness(m, now = Date.now()) {
  if (!m) return "waiting";
  if (!m.status.clockSynced || m.measuredAt > now + 2000 || m.uploadedAt > now + 2000) return "clock-error";
  if (m.status.capture !== "running") return "capture-error";
  const age = now - Math.min(m.measuredAt, m.uploadedAt);
  return age > 10000 ? "offline" : age > 2000 ? "stale" : "live";
}
export function createHistory() {
  let last = null;
  const points = [];
  return {
    points,
    clear() { points.length = 0; last = null; },
    prune(now = Date.now()) { while (points.length && points[0].time < now - HISTORY_MS) points.shift(); },
    add(m, now = Date.now()) {
      this.prune(now);
      const identity = `${m.deviceId}/${m.bootId}`;
      if (last && identity === last.identity && m.sequence <= last.sequence) return false;
      if (last && m.measuredAt < last.time) return false;
      last = { identity, sequence: m.sequence, time: m.measuredAt };
      if (freshness(m, now) !== "live") return true;
      if (Number.isFinite(m.values.dbSplA)) points.push({time:m.measuredAt, value:m.values.dbSplA, dbfs:m.values.dbfs, identity});
      return true;
    }
  };
}
