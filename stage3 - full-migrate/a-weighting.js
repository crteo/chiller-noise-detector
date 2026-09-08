/* Pure A-weighting and spectral-energy helpers. */

export function aWeightingDb(frequencyHz) {
  if (!Number.isFinite(frequencyHz) || frequencyHz <= 0) {
    return -Infinity;
  }

  const f2 = frequencyHz * frequencyHz;
  const c20 = 20.6 * 20.6;
  const c107 = 107.7 * 107.7;
  const c737 = 737.9 * 737.9;
  const c12194 = 12194 * 12194;

  const numerator =
    c12194 * f2 * f2;

  const denominator =
    (f2 + c20) *
    Math.sqrt((f2 + c107) * (f2 + c737)) *
    (f2 + c12194);

  const response = numerator / denominator;

  return response > 0
    ? 20 * Math.log10(response) + 2
    : -Infinity;
}

export function createAWeightingEnergyWeights(
  sampleRate,
  fftSize,
  binCount = fftSize / 2
) {
  const weights = new Float64Array(binCount);

  for (let bin = 1; bin < binCount; bin += 1) {
    const frequencyHz = bin * sampleRate / fftSize;
    const weightingDb = aWeightingDb(frequencyHz);

    weights[bin] = Number.isFinite(weightingDb)
      ? 10 ** (weightingDb / 10)
      : 0;
  }

  return weights;
}

export function calculateSpectralLevels(
  binLevelsDb,
  aWeightingEnergyWeights,
  minimumUsableDb = -120
) {
  const count = Math.min(
    binLevelsDb.length,
    aWeightingEnergyWeights.length
  );

  let unweightedEnergy = 0;
  let weightedEnergy = 0;
  let includedBins = 0;

  // Bin zero is DC and is intentionally excluded.
  for (let bin = 1; bin < count; bin += 1) {
    const levelDb = binLevelsDb[bin];

    // Values at the analyser floor cannot be distinguished from
    // silence. Excluding them prevents thousands of floor bins from
    // creating artificial summed energy.
    if (!Number.isFinite(levelDb) || levelDb <= minimumUsableDb) {
      continue;
    }

    const energy = 10 ** (levelDb / 10);

    unweightedEnergy += energy;
    weightedEnergy +=
      energy * aWeightingEnergyWeights[bin];
    includedBins += 1;
  }

  if (
    includedBins === 0 ||
    unweightedEnergy <= 0 ||
    weightedEnergy <= 0
  ) {
    return null;
  }

  const unweightedDb =
    10 * Math.log10(unweightedEnergy);

  const weightedDb =
    10 * Math.log10(weightedEnergy);

  return {
    includedBins,
    unweightedDb,
    weightedDb,
    weightingDifferenceDb:
      weightedDb - unweightedDb
  };
}

export function applyCalibrationConstant(
  digitalLevelDbfs,
  calibrationConstantDb
) {
  return (
    Number.isFinite(digitalLevelDbfs) &&
    Number.isFinite(calibrationConstantDb)
  )
    ? digitalLevelDbfs + calibrationConstantDb
    : null;
}
