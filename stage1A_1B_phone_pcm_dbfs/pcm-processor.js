/*
Stage 1A / 1B PCM meter

Pipeline implemented here:

Float32 PCM
    ↓
accumulate ~100 ms
    ↓
mean = DC offset
    ↓
remove DC mathematically
    ↓
RMS
    ↓
20 * log10(RMS)
    ↓
dBFS

IMPORTANT:
- PCM samples are normalized digital amplitudes, approximately in [-1, +1].
- They are NOT Pascals.
- dBFS is referenced to digital full scale.
- No acoustic calibration is performed.
*/

class PCMMeterProcessor extends AudioWorkletProcessor {
  constructor() {
    super();

    this.windowTarget = Math.max(1, Math.round(sampleRate * 0.100));
    this.waveformTarget = 512;
    this.resetWindow();
  }

  resetWindow() {
    this.n = 0;
    this.sum = 0;
    this.sumSquares = 0;
    this.peak = 0;
    this.clipped = 0;
    this.latestSample = 0;
    this.lastBlockSize = 0;
    this.waveform = [];
  }

  process(inputs) {
    const input = inputs[0];

    if (!input || input.length === 0 || !input[0]) {
      return true;
    }

    const channel = input[0];
    this.lastBlockSize = channel.length;

    for (let i = 0; i < channel.length; i++) {
      const x = channel[i];

      this.latestSample = x;
      this.n += 1;
      this.sum += x;
      this.sumSquares += x * x;

      const absX = Math.abs(x);
      if (absX > this.peak) this.peak = absX;

      // "Clipping" here means normalized samples have reached
      // very close to full digital scale.
      if (absX >= 0.999) this.clipped += 1;

      if (this.waveform.length < this.waveformTarget) {
        this.waveform.push(x);
      }

      if (this.n >= this.windowTarget) {
        const mean = this.sum / this.n;

        // RMS after DC removal.
        //
        // mean[(x - mean)^2]
        // = mean[x^2] - mean^2
        const meanSquareAC = Math.max(
          0,
          (this.sumSquares / this.n) - (mean * mean)
        );

        const rms = Math.sqrt(meanSquareAC);

        // Our documented convention:
        // dBFS = 20 log10(RMS / 1.0)
        //
        // Therefore a sine wave with peak amplitude 1.0
        // has RMS = 1/sqrt(2) and reads about -3.01 dBFS.
        const dbfs = rms > 0
          ? 20 * Math.log10(rms)
          : -Infinity;

        this.port.postMessage({
          sampleRate,
          blockSize: this.lastBlockSize,
          windowSamples: this.n,
          latestSample: this.latestSample,
          mean,
          rms,
          peak: this.peak,
          dbfs,
          clippingFraction: this.clipped / this.n,
          waveform: this.waveform
        });

        this.resetWindow();
      }
    }

    return true;
  }
}

registerProcessor("pcm-meter", PCMMeterProcessor);
