// Explicit synthetic data only: ?demo=1. Never writes to Firebase.
export function demoMeasurement(sequence = 0, now = Date.now()) {
  const dbfs = -55 + 8 * Math.sin(sequence / 20);
  return {
    version:2, deviceId:"DEMO", bootId:"demo-boot", firmwareVersion:"demo",
    sequence, measuredAt:now, uploadedAt:now, uptimeMs:sequence * 200, windowMs:100,
    values:{dbfs, aWeightedDbfs:dbfs, calibrationConstant:120, dbSpl:dbfs+120, dbSplA:dbfs+120},
    status:{capture:"running", clockSynced:true, sampleRate:48000, clippingFraction:0},
    diagnostics:{rms:10 ** (dbfs/20), peak:Math.SQRT2*10 ** (dbfs/20), mean:0, latestSample:0,
      blockSize:256, droppedSamples:0, captureErrors:0},
    spectrum:{fftSize:4096, maxHz:24000, measuredAt:now, dominantHz:1000,
      bands:Array.from({length:64}, (_,i) => Math.round(180*Math.exp(-(((i-2.2)/2)**2))+15))},
    waveform:Array.from({length:128}, (_,i) => 0.003*Math.sin(2*Math.PI*1000*i/48000))
  };
}
