// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_UNICODE_GLYPHS_NOISE_GENERATOR_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_UNICODE_GLYPHS_NOISE_GENERATOR_H_

#include <chrono>
#include <random>

#include "base/bit_cast.h"
#include "base/command_line.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "third_party/blink/renderer/platform/fonts/glyph.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

// Unicode Glyphs noise generator for fingerprinting protection.
// Generates consistent noise for glyph metrics (width, bounds, etc.) when
// the --fonts-noise flag is enabled.
//
// Thread-safe singleton that provides deterministic noise values for the same
// glyph+font combination throughout the browser session.
//
// IMPORTANT: This operates independently from Font Metrics whitelist
// (--fonts-whitelist flag). The whitelist only affects Font Metrics queries,
// NOT Unicode Glyphs noise.
class UnicodeGlyphsNoiseGenerator {
 public:
  static UnicodeGlyphsNoiseGenerator& GetInstance() {
    static base::NoDestructor<UnicodeGlyphsNoiseGenerator> instance;
    return *instance;
  }

  // Thread-local flag to suppress font noise for specific rendering contexts (e.g., Canvas)
  static bool& SuppressNoiseFlag() {
    thread_local bool suppress_noise = false;
    return suppress_noise;
  }

  // Check if Unicode Glyphs noise is enabled via --fonts-noise flag
  bool IsEnabled() const { return noise_enabled_ && !SuppressNoiseFlag(); }

  // Get noise for glyph advance width (in pixels)
  // Returns value + noise, where noise is in range [-0.02, 0.02] pixels
  // This is invisible per-glyph but accumulates over ~50 chars to ±1px
  // which is enough to change integer-based fingerprints (offsetWidth)
  float GetNoisedWidth(float original_width, Glyph glyph) {
    if (!IsEnabled()) {
      return original_width;
    }
    return original_width + GetWidthNoise(original_width, glyph);
  }

  // Get noise for glyph bounds/extents (in pixels)
  // Returns value + noise, where noise is in range [-0.03, 0.03] pixels
  float GetNoisedBounds(float original_bounds, Glyph glyph, int coord_index) {
    if (!IsEnabled()) {
      return original_bounds;
    }
    return original_bounds + GetBoundsNoise(original_bounds, glyph, coord_index);
  }

  // DISABLED: ShouldHideCharacter was hiding 2% of characters randomly
  // causing missing glyphs on websites. Character visibility should NOT
  // be modified — use font list randomization instead (Option C).
  bool ShouldHideCharacter(uint32_t /*codepoint*/) {
    return false;
  }

  UnicodeGlyphsNoiseGenerator(const UnicodeGlyphsNoiseGenerator&) = delete;
  UnicodeGlyphsNoiseGenerator& operator=(const UnicodeGlyphsNoiseGenerator&) = delete;

 private:
  friend class base::NoDestructor<UnicodeGlyphsNoiseGenerator>;

  UnicodeGlyphsNoiseGenerator() {
    // Check if --fonts-noise flag is enabled (via its passed seed)
    base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
    if (!command_line || !command_line->HasSwitch("unicode-glyphs-seed")) {
      noise_enabled_ = false;
      session_seed_ = 0;
      return;
    }

    noise_enabled_ = true;

    // Try to load profile-persisted seed from --unicode-glyphs-seed flag
    if (command_line->HasSwitch("unicode-glyphs-seed")) {
      std::string seed_str = command_line->GetSwitchValueASCII("unicode-glyphs-seed");
      if (base::StringToUint64(seed_str, &session_seed_) && session_seed_ != 0) {
        // Successfully loaded profile seed - noise will be deterministic per profile!
        return;
      }
    }

    // Fallback: Initialize session seed from timestamp
    session_seed_ = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
  }

  // Get width noise for a glyph (cached)
  float GetWidthNoise(float width, Glyph glyph) {
    uint64_t key = HashGlyphMetric(width, glyph, 0);
    auto it = width_noise_cache_.find(key);
    if (it != width_noise_cache_.end()) {
      return it->value;  // Cache hit
    }

    // Cache miss - generate noise in range [-0.02, 0.02] pixels
    // Per-glyph: invisible (0.02px = 1/50th of a pixel)
    // Per-word (~10 chars): ±0.06px — still invisible
    // Per-line (~50 chars): ±0.14px — barely sub-pixel  
    // Per-long-text (~200 chars): ±0.28px — MAY shift integer offsetWidth by 1
    std::mt19937_64 gen(key);
    std::uniform_real_distribution<float> dis(-0.02f, 0.02f);
    float noise = dis(gen);
    width_noise_cache_.Set(key, noise);
    return noise;
  }

  // Get bounds noise for a glyph coordinate (cached)
  float GetBoundsNoise(float bounds, Glyph glyph, int coord_index) {
    uint64_t key = HashGlyphMetric(bounds, glyph, coord_index);
    auto it = bounds_noise_cache_.find(key);
    if (it != bounds_noise_cache_.end()) {
      return it->value;  // Cache hit
    }

    // Cache miss - generate noise in range [-0.03, 0.03] pixels  
    std::mt19937_64 gen(key);
    std::uniform_real_distribution<float> dis(-0.03f, 0.03f);
    float noise = dis(gen);
    bounds_noise_cache_.Set(key, noise);
    return noise;
  }

  // Hash function for glyph metrics (combines glyph, value, and coordinate)
  uint64_t HashGlyphMetric(float value, Glyph glyph, int coord_index) const {
    uint32_t v = base::bit_cast<uint32_t>(value);
    return session_seed_ ^ (static_cast<uint64_t>(glyph) * 0x9e3779b97f4a7c15ULL) ^
           (static_cast<uint64_t>(v) * 0x7f4a7c15) ^
           (static_cast<uint64_t>(coord_index) * 0x4a7c157f);
  }

  // Hash function for character coverage detection
  uint64_t HashCharacter(uint32_t codepoint) const {
    return session_seed_ ^ (static_cast<uint64_t>(codepoint) * 0x9e3779b97f4a7c15ULL);
  }

  bool noise_enabled_;
  uint64_t session_seed_;
  HashMap<uint64_t, float> width_noise_cache_;
  HashMap<uint64_t, float> bounds_noise_cache_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_UNICODE_GLYPHS_NOISE_GENERATOR_H_
