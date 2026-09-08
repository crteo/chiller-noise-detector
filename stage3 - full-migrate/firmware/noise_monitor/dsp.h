#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t DSP_FFT_SIZE = 4096;
constexpr size_t DSP_BAND_COUNT = 64;
constexpr size_t DSP_WAVEFORM_SIZE = 128;

struct DspResult {
  float mean;
  float rms;
  float peak;
  float latestSample;
  float dbfs;
  float aWeightedDbfs;
  float dominantHz;
  float clippingFraction;
  uint8_t bands[DSP_BAND_COUNT];
  int16_t waveform[DSP_WAVEFORM_SIZE];
};

float aWeightingDb(float frequencyHz);
void resetDspState();
bool processPcm(const int32_t *raw, size_t count, uint32_t sampleRate,
                DspResult &result);
