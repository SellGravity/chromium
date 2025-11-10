#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_WEBAUDIO_AUDIO_NOISE_GENERATOR_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_WEBAUDIO_AUDIO_NOISE_GENERATOR_H_

#include "third_party/blink/renderer/platform/privacy_budget/session_noise_cache.h"

namespace blink {

// Audio noise generator using SessionNoiseCache for deterministic, cached noise
// This ensures same audio sample values get same noise across calls in session
class AudioNoiseGenerator {
 public:
  static AudioNoiseGenerator& GetInstance();

  // Get cached noise based on sample value in range [min_val, max_val]
  // Uses SessionNoiseCache internally for deterministic, cached results
  float GetNoise(float sample_value, float min_val, float max_val) {
    double noise = SessionNoiseCache::GetInstance().GetNoiseInRange(
        static_cast<double>(sample_value),
        static_cast<double>(min_val),
        static_cast<double>(max_val));
    return static_cast<float>(noise);
  }

  // Get cached noise int based on value in range [min_val, max_val]
  int GetNoiseInt(int value, int min_val, int max_val) {
    double noise = SessionNoiseCache::GetInstance().GetNoiseInRange(
        static_cast<double>(value),
        static_cast<double>(min_val),
        static_cast<double>(max_val));
    return static_cast<int>(noise);
  }

 private:
  AudioNoiseGenerator() = default;
};

// Function-local static to avoid exit-time destructor issues
inline AudioNoiseGenerator& AudioNoiseGenerator::GetInstance() {
  static AudioNoiseGenerator* instance = new AudioNoiseGenerator();
  return *instance;
}

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_WEBAUDIO_AUDIO_NOISE_GENERATOR_H_