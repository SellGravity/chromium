#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_WEBAUDIO_AUDIO_NOISE_GENERATOR_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_WEBAUDIO_AUDIO_NOISE_GENERATOR_H_

#include <chrono>
#include <random>
#include "base/synchronization/lock.h"

namespace blink {

// Global singleton PRNG for audio fingerprinting noise
class AudioNoiseGenerator {
 public:
  static AudioNoiseGenerator& GetInstance();
  
  // Get random float in range [min_val, max_val]
  float GetNoise(float min_val, float max_val) {
    base::AutoLock lock(lock_);
    std::uniform_real_distribution<float> dis(min_val, max_val);
    return dis(gen_);
  }
  
  // Get random int in range [min_val, max_val]
  int GetNoiseInt(int min_val, int max_val) {
    base::AutoLock lock(lock_);
    std::uniform_int_distribution<int> dis(min_val, max_val);
    return dis(gen_);
  }

 private:
  AudioNoiseGenerator()
      : gen_(static_cast<unsigned int>(
            std::chrono::steady_clock::now().time_since_epoch().count())) {}
  
  std::mt19937 gen_;
  base::Lock lock_;  // Protect gen_ từ multiple threads
};

// Function-local static to avoid exit-time destructor issues
inline AudioNoiseGenerator& AudioNoiseGenerator::GetInstance() {
  static AudioNoiseGenerator* instance = new AudioNoiseGenerator();
  return *instance;
}

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_WEBAUDIO_AUDIO_NOISE_GENERATOR_H_