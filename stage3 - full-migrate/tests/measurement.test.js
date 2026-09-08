import test from "node:test";
import assert from "node:assert/strict";
import {demoMeasurement} from "../demo.js";
import {createHistory, freshness, validateMeasurement} from "../measurement.js";

test("accepts the version 2 ESP32 payload", () => {
  const now = Date.now();
  assert.equal(validateMeasurement(demoMeasurement(1, now)).sequence, 1);
  assert.equal(freshness(demoMeasurement(1, now), now), "live");
});

test("rejects inconsistent calibration and malformed spectrum", () => {
  const badCalibration = demoMeasurement(1);
  badCalibration.values.dbSplA += 1;
  assert.throws(() => validateMeasurement(badCalibration));
  const badBands = demoMeasurement(1);
  badBands.spectrum.bands.pop();
  assert.throws(() => validateMeasurement(badBands));
});

test("freshness detects stale, offline, clock and capture states", () => {
  const now = Date.now();
  assert.equal(freshness(demoMeasurement(1, now - 3000), now), "stale");
  assert.equal(freshness(demoMeasurement(1, now - 11000), now), "offline");
  const clock = demoMeasurement(1, now); clock.status.clockSynced = false;
  assert.equal(freshness(clock, now), "clock-error");
  const capture = demoMeasurement(1, now); capture.status.capture = "error";
  assert.equal(freshness(capture, now), "capture-error");
});

test("history rejects duplicates and accepts a new boot identity", () => {
  const store = createHistory();
  const now = Date.now();
  assert.equal(store.add(demoMeasurement(1, now), now), true);
  assert.equal(store.add(demoMeasurement(1, now), now), false);
  const restarted = demoMeasurement(0, now + 1); restarted.bootId = "new-boot";
  assert.equal(store.add(restarted, now + 1), true);
  assert.equal(store.points.length, 2);
});

