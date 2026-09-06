import test from "node:test";
import assert from "node:assert/strict";

import {
  aWeightingDb,
  calculateSpectralLevels,
  createAWeightingEnergyWeights
} from "./a-weighting.js";

function closeTo(actual, expected, tolerance = 0.15) {
  assert.ok(
    Math.abs(actual - expected) <= tolerance,
    `expected ${actual} to be within ${tolerance} dB of ${expected}`
  );
}

test("A-weighting matches standard reference points", () => {
  closeTo(aWeightingDb(100), -19.1);
  closeTo(aWeightingDb(500), -3.25);
  closeTo(aWeightingDb(1000), 0, 0.05);
  closeTo(aWeightingDb(2000), 1.2);
});

test("DC and invalid frequencies are excluded", () => {
  assert.equal(aWeightingDb(0), -Infinity);
  assert.equal(aWeightingDb(-1), -Infinity);
  assert.equal(aWeightingDb(Number.NaN), -Infinity);
});

test("two equal bins combine by 3.01 dB", () => {
  const levels = new Float32Array([-Infinity, -40, -40]);
  const unityWeights = new Float64Array([0, 1, 1]);
  const result = calculateSpectralLevels(levels, unityWeights);

  closeTo(result.unweightedDb, -36.9897, 0.001);
  closeTo(result.weightingDifferenceDb, 0, 0.001);
});

test("single-bin tones receive their expected correction", () => {
  const sampleRate = 48000;
  const fftSize = 48000;
  const weights = createAWeightingEnergyWeights(sampleRate, fftSize);

  for (const [frequency, expected] of [
    [100, -19.1],
    [500, -3.25],
    [1000, 0],
    [2000, 1.2]
  ]) {
    const levels = new Float32Array(weights.length);
    levels.fill(-Infinity);
    levels[frequency] = -40;

    const result = calculateSpectralLevels(levels, weights);
    closeTo(result.weightingDifferenceDb, expected);
  }
});

test("empty and analyser-floor spectra return no result", () => {
  const weights = new Float64Array([0, 1, 1]);

  assert.equal(
    calculateSpectralLevels(
      new Float32Array([-Infinity, -Infinity, -Infinity]),
      weights
    ),
    null
  );

  assert.equal(
    calculateSpectralLevels(
      new Float32Array([-120, -120, -120]),
      weights
    ),
    null
  );
});
