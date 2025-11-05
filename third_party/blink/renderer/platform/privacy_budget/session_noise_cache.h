// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_SESSION_NOISE_CACHE_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_SESSION_NOISE_CACHE_H_

#include <chrono>
#include <random>

#include "base/no_destructor.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"

namespace blink {

// Session-based noise cache for fingerprinting protection.
// Generates noise once per session and caches it for reuse, significantly
// reducing CPU usage compared to per-call random generation.
//
// Thread-safe singleton that provides consistent noise values for the same
// input throughout the browser session.
class SessionNoiseCache {
 public:
  static SessionNoiseCache& GetInstance() {
    static base::NoDestructor<SessionNoiseCache> instance;
    return *instance;
  }

  // Get cached noise for a given double value
  double GetNoise(double value) {
    uint64_t key = HashValue(value);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      return it->value;  // Cache hit
    }

    // Cache miss - generate and store noise
    std::mt19937_64 gen(key);
    std::uniform_real_distribution<double> dis(-0.5, 0.5);
    double noise = dis(gen);
    cache_.Set(key, noise);
    return noise;
  }

  // Get cached noise with custom range
  double GetNoiseInRange(double value, double min_noise, double max_noise) {
    uint64_t key = HashValueWithRange(value, min_noise, max_noise);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      return it->value;  // Cache hit
    }

    // Cache miss - generate and store noise with custom range
    std::mt19937_64 gen(key);
    std::uniform_real_distribution<double> dis(min_noise, max_noise);
    double noise = dis(gen);
    cache_.Set(key, noise);
    return noise;
  }

  SessionNoiseCache(const SessionNoiseCache&) = delete;
  SessionNoiseCache& operator=(const SessionNoiseCache&) = delete;

 private:
  friend class base::NoDestructor<SessionNoiseCache>;

  SessionNoiseCache() {
    // Initialize session seed once at startup
    session_seed_ = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
  }

  // Hash function that combines session seed with value
  uint64_t HashValue(double value) const {
    uint64_t v = *reinterpret_cast<const uint64_t*>(&value);
    return session_seed_ ^ (v * 0x9e3779b97f4a7c15ULL);
  }

  // Hash function with range parameters for custom noise ranges
  uint64_t HashValueWithRange(double value,
                               double min_noise,
                               double max_noise) const {
    uint64_t v = *reinterpret_cast<const uint64_t*>(&value);
    uint64_t min_v = *reinterpret_cast<const uint64_t*>(&min_noise);
    uint64_t max_v = *reinterpret_cast<const uint64_t*>(&max_noise);
    return session_seed_ ^ (v * 0x9e3779b97f4a7c15ULL) ^
           (min_v * 0x7f4a7c15) ^ (max_v * 0x4a7c157f);
  }

  uint64_t session_seed_;
  HashMap<uint64_t, double> cache_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_SESSION_NOISE_CACHE_H_
