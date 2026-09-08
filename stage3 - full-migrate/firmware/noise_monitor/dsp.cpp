#include "dsp.h"

#include <math.h>
#include <string.h>

namespace {
float realPart[DSP_FFT_SIZE];
float imagPart[DSP_FFT_SIZE];

// Sixth-order 48 kHz A-weighting filter, implemented as transposed direct form
// II. State persists across measurement windows.
constexpr double aWeightB[] = {
  0.234301792299513, -0.468603584599026, -0.234301792299513,
  0.937207169198052, -0.234301792299513, -0.468603584599026,
  0.234301792299513
};
constexpr double aWeightA[] = {
  1.0, -4.113043408775872, 6.553121752655046, -4.990849294163384,
  1.785737302937575, -0.246190595319487, 0.011224250033231
};
double aWeightState[6] = {};

double filterAWeight(double input) {
  const double output = aWeightB[0] * input + aWeightState[0];
  for (size_t i = 0; i < 5; ++i)
    aWeightState[i] = aWeightB[i + 1] * input - aWeightA[i + 1] * output + aWeightState[i + 1];
  aWeightState[5] = aWeightB[6] * input - aWeightA[6] * output;
  return output;
}

void fft(float *real, float *imag, size_t n) {
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float tr = real[i]; real[i] = real[j]; real[j] = tr;
      float ti = imag[i]; imag[i] = imag[j]; imag[j] = ti;
    }
  }
  for (size_t length = 2; length <= n; length <<= 1) {
    const float angle = -2.0f * static_cast<float>(M_PI) / length;
    const float wrStep = cosf(angle), wiStep = sinf(angle);
    for (size_t start = 0; start < n; start += length) {
      float wr = 1.0f, wi = 0.0f;
      for (size_t j = 0; j < length / 2; ++j) {
        const size_t even = start + j, odd = even + length / 2;
        const float tr = wr * real[odd] - wi * imag[odd];
        const float ti = wr * imag[odd] + wi * real[odd];
        real[odd] = real[even] - tr; imag[odd] = imag[even] - ti;
        real[even] += tr; imag[even] += ti;
        const float nextWr = wr * wrStep - wi * wiStep;
        wi = wr * wiStep + wi * wrStep; wr = nextWr;
      }
    }
  }
}

float normalized(int32_t sample) {
  // The INMP441's signed 24-bit value is left-aligned in a 32-bit I2S slot.
  return static_cast<float>(sample) / 2147483648.0f;
}
}

float aWeightingDb(float f) {
  if (!(f > 0.0f)) return -INFINITY;
  const double f2 = static_cast<double>(f) * f;
  const double c20 = 20.6 * 20.6, c107 = 107.7 * 107.7;
  const double c737 = 737.9 * 737.9, c12194 = 12194.0 * 12194.0;
  const double numerator = c12194 * f2 * f2;
  const double denominator = (f2 + c20) * sqrt((f2 + c107) * (f2 + c737)) * (f2 + c12194);
  return static_cast<float>(20.0 * log10(numerator / denominator) + 2.0);
}

void resetDspState() {
  memset(aWeightState, 0, sizeof(aWeightState));
}

bool processPcm(const int32_t *raw, size_t count, uint32_t sampleRate,
                DspResult &out) {
  if (!raw || count < DSP_FFT_SIZE || sampleRate < 8000) return false;
  memset(&out, 0, sizeof(out));
  double sum = 0.0, sumSquares = 0.0;
  float peak = 0.0f;
  size_t clipped = 0;
  for (size_t i = 0; i < count; ++i) {
    const float x = normalized(raw[i]);
    sum += x; sumSquares += static_cast<double>(x) * x;
    peak = fmaxf(peak, fabsf(x));
    if (fabsf(x) >= 0.999f) ++clipped;
  }
  out.mean = static_cast<float>(sum / count);
  out.rms = sqrtf(fmaxf(0.0f, static_cast<float>(sumSquares / count) - out.mean * out.mean));
  out.peak = peak;
  out.latestSample = normalized(raw[count - 1]);
  out.clippingFraction = static_cast<float>(clipped) / count;
  out.dbfs = out.rms > 0.0f ? 20.0f * log10f(out.rms) : -120.0f;

  double weightedSquares = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double weighted = filterAWeight(normalized(raw[i]) - out.mean);
    weightedSquares += weighted * weighted;
  }
  const float weightedRms = sqrtf(static_cast<float>(weightedSquares / count));
  out.aWeightedDbfs = weightedRms > 0.0f ? 20.0f * log10f(weightedRms) : -120.0f;

  for (size_t i = 0; i < DSP_FFT_SIZE; ++i) {
    const float window = 0.5f - 0.5f * cosf(2.0f * static_cast<float>(M_PI) * i / (DSP_FFT_SIZE - 1));
    realPart[i] = (normalized(raw[i]) - out.mean) * window;
    imagPart[i] = 0.0f;
  }
  fft(realPart, imagPart, DSP_FFT_SIZE);

  float maxPower = 0.0f;
  size_t dominantBin = 1;
  float bandDb[DSP_BAND_COUNT];
  for (size_t i = 0; i < DSP_BAND_COUNT; ++i) bandDb[i] = -120.0f;
  for (size_t bin = 1; bin < DSP_FFT_SIZE / 2; ++bin) {
    const float power = realPart[bin] * realPart[bin] + imagPart[bin] * imagPart[bin];
    if (power > maxPower) { maxPower = power; dominantBin = bin; }
    const size_t band = (bin * DSP_BAND_COUNT) / (DSP_FFT_SIZE / 2);
    const float amplitude = 2.0f * sqrtf(power) / (DSP_FFT_SIZE * 0.5f);
    const float binDb = amplitude > 0.0f ? 20.0f * log10f(amplitude) : -120.0f;
    if (band < DSP_BAND_COUNT) bandDb[band] = fmaxf(bandDb[band], binDb);
  }
  out.dominantHz = static_cast<float>(dominantBin) * sampleRate / DSP_FFT_SIZE;
  for (size_t i = 0; i < DSP_BAND_COUNT; ++i) {
    const float bounded = fmaxf(-120.0f, fminf(0.0f, bandDb[i]));
    out.bands[i] = static_cast<uint8_t>(lroundf((bounded + 120.0f) * 255.0f / 120.0f));
  }
  for (size_t i = 0; i < DSP_WAVEFORM_SIZE; ++i) {
    const size_t source = i * count / DSP_WAVEFORM_SIZE;
    const float x = fmaxf(-1.0f, fminf(1.0f, normalized(raw[source])));
    out.waveform[i] = static_cast<int16_t>(lroundf(x * 32767.0f));
  }
  return true;
}
