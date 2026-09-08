#include "../noise_monitor/dsp.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <vector>

int main() {
  assert(fabs(aWeightingDb(1000.0f)) < 0.1f);
  assert(fabs(aWeightingDb(100.0f) + 19.1f) < 0.2f);
  std::vector<int32_t> samples(4800);
  const float amplitude = 0.25f;
  DspResult result{};
  const float frequencies[] = {100.0f, 500.0f, 1000.0f, 2000.0f};
  const float expected[] = {-19.1f, -3.25f, 0.0f, 1.2f};
  for (size_t tone = 0; tone < 4; ++tone) {
    for (size_t i = 0; i < samples.size(); ++i)
      samples[i] = static_cast<int32_t>(amplitude * sin(2.0 * M_PI * frequencies[tone] * i / 48000.0) * 2147483647.0);
    resetDspState();
    for (int warmup = 0; warmup < 3; ++warmup)
      assert(processPcm(samples.data(), samples.size(), 48000, result));
    assert(fabs((result.aWeightedDbfs - result.dbfs) - expected[tone]) < 0.3f);
  }
  assert(fabs(result.dbfs - (-15.0515f)) < 0.1f);
  assert(fabs(result.dominantHz - 2003.90625f) < 0.1f);
  puts("firmware DSP tests passed");
}
