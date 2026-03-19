// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_SESSION_NOISE_CACHE_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_SESSION_NOISE_CACHE_H_

#include <chrono>
#include <fstream>
#include <random>

#include "base/bit_cast.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/hash/hash.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"

namespace blink {

// Session-based noise cache for fingerprinting protection.
// Automatically persists seeds to JSON file in user-data-dir for consistent
// fingerprints across browser restarts.
//
// Seed derivation priority:
// 1. Load from fingerprint_config.json in user-data-dir (if exists)
// 2. Generate random seed and SAVE to fingerprint_config.json
// 3. Fallback to random seed (if no user-data-dir)
//
// Usage: Just pass --canvas-noise --audio-noise flags, seeds auto-managed!
class SessionNoiseCache {
 public:
  static SessionNoiseCache& GetInstance() {
    static ::base::NoDestructor<SessionNoiseCache> instance;
    return *instance;
  }

  // Get the session seed (for debugging/logging)
  uint64_t GetSessionSeed() const { return session_seed_; }
  
  // Get fonts noise seed
  uint64_t GetFontsNoiseSeed() const { return fonts_noise_seed_; }
  
  // Get audio noise seed  
  uint64_t GetAudioNoiseSeed() const { return audio_noise_seed_; }
  
  // Get rects noise seed
  uint64_t GetRectsNoiseSeed() const { return rects_noise_seed_; }

  // Get WebGL vendor (from JSON config or CLI)
  const std::string& GetWebGLVendor() const { return webgl_vendor_; }
  
  // Get WebGL renderer (from JSON config or CLI)
  const std::string& GetWebGLRenderer() const { return webgl_renderer_; }
  
  // Get User-Agent (from JSON config or CLI)
  const std::string& GetUserAgent() const { return user_agent_; }
  
  // Get hardware concurrency (from JSON config, 0 = use real value)
  int GetHardwareConcurrency() const { return hardware_concurrency_; }
  
  // Get device memory (from JSON config, 0 = use real value)
  int GetDeviceMemory() const { return device_memory_; }

  // Get cached noise for a given double value
  double GetNoise(double value) {
    uint64_t key = HashValue(value);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      return it->value;  // Cache hit
    }

    // Simple eviction policy: clear cache if it grows too large
    if (cache_.size() > 10000) {
      cache_.clear();
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

    // Simple eviction policy: clear cache if it grows too large
    if (cache_.size() > 10000) {
      cache_.clear();
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
  friend class ::base::NoDestructor<SessionNoiseCache>;

  SessionNoiseCache() 
      : session_seed_(0), 
        fonts_noise_seed_(0), 
        audio_noise_seed_(0), 
        rects_noise_seed_(0),
        hardware_concurrency_(0),
        device_memory_(0) {
    InitializeSeeds();
  }

  void InitializeSeeds() {
    ::base::CommandLine* command_line = ::base::CommandLine::ForCurrentProcess();
    if (!command_line) {
      GenerateAllRandomSeeds();
      return;
    }

    // Try to get seeds from command-line flags first (sandbox-safe)
    bool has_seeds_from_flags = LoadSeedsFromCommandLine(command_line);
    
    // Always load metadata flags (hardware-concurrency, device-memory, etc.)
    LoadMetadataFromCommandLine(command_line);

    if (has_seeds_from_flags) {
      return;
    }

    // Derive deterministic seeds from user-data-dir path hash
    // Same profile path = same seeds across restarts
    // Different profile = different seeds (different fingerprint)
    std::string user_data_dir;
    if (command_line->HasSwitch("user-data-dir")) {
      user_data_dir = command_line->GetSwitchValueASCII("user-data-dir");
    }

    if (!user_data_dir.empty()) {
      GenerateSeedsFromPath(user_data_dir);
    } else {
      // No user-data-dir — truly random seeds
      GenerateAllRandomSeeds();
    }
  }

  bool LoadSeedsFromCommandLine(::base::CommandLine* command_line) {
    bool has_any_seed = false;

    // Canvas/main noise seed: --canvas-seed=12345678901234567890 (from browser process)
    // Also support --noise-seed for backward compatibility
    if (command_line->HasSwitch("canvas-seed")) {
      std::string seed_str = command_line->GetSwitchValueASCII("canvas-seed");
      uint64_t seed = 0;
      if (base::StringToUint64(seed_str, &seed) && seed != 0) {
        session_seed_ = seed;
        has_any_seed = true;
      }
    } else if (command_line->HasSwitch("noise-seed")) {
      std::string seed_str = command_line->GetSwitchValueASCII("noise-seed");
      uint64_t seed = 0;
      if (base::StringToUint64(seed_str, &seed) && seed != 0) {
        session_seed_ = seed;
        has_any_seed = true;
      }
    }

    // Fonts/Unicode glyphs noise seed: --unicode-glyphs-seed (from browser process)
    // Also support --fonts-noise-seed for backward compatibility
    if (command_line->HasSwitch("unicode-glyphs-seed")) {
      std::string seed_str = command_line->GetSwitchValueASCII("unicode-glyphs-seed");
      uint64_t seed = 0;
      if (base::StringToUint64(seed_str, &seed) && seed != 0) {
        fonts_noise_seed_ = seed;
        has_any_seed = true;
      }
    } else if (command_line->HasSwitch("fonts-noise-seed")) {
      std::string seed_str = command_line->GetSwitchValueASCII("fonts-noise-seed");
      uint64_t seed = 0;
      if (base::StringToUint64(seed_str, &seed) && seed != 0) {
        fonts_noise_seed_ = seed;
        has_any_seed = true;
      }
    }

    // Audio noise seed: --audio-noise-seed=12345678901234567890
    if (command_line->HasSwitch("audio-noise-seed")) {
      std::string seed_str = command_line->GetSwitchValueASCII("audio-noise-seed");
      uint64_t seed = 0;
      if (base::StringToUint64(seed_str, &seed) && seed != 0) {
        audio_noise_seed_ = seed;
        has_any_seed = true;
      }
    }

    // Rects noise seed: --rects-noise-seed=12345678901234567890
    if (command_line->HasSwitch("rects-noise-seed")) {
      std::string seed_str = command_line->GetSwitchValueASCII("rects-noise-seed");
      uint64_t seed = 0;
      if (base::StringToUint64(seed_str, &seed) && seed != 0) {
        rects_noise_seed_ = seed;
        has_any_seed = true;
      }
    }

    // Fill missing seeds with random values if we have at least one
    if (has_any_seed) {
      if (session_seed_ == 0) session_seed_ = GenerateRandomSeed();
      if (fonts_noise_seed_ == 0) fonts_noise_seed_ = GenerateRandomSeed();
      if (audio_noise_seed_ == 0) audio_noise_seed_ = GenerateRandomSeed();
      if (rects_noise_seed_ == 0) rects_noise_seed_ = GenerateRandomSeed();
    }

    return has_any_seed;
  }

  void LoadMetadataFromCommandLine(::base::CommandLine* command_line) {
    // WebGL overrides are already handled via existing flags:
    // --webgl-vendor="..." --webgl-renderer="..."
    // These are read directly where needed, not cached here
    
    // Hardware concurrency: --hardware-concurrency=8
    if (command_line->HasSwitch("hardware-concurrency")) {
      std::string value = command_line->GetSwitchValueASCII("hardware-concurrency");
      int hc = 0;
      if (base::StringToInt(value, &hc) && hc > 0) {
        hardware_concurrency_ = hc;
      }
    }

    // Device memory: --device-memory=8
    if (command_line->HasSwitch("device-memory")) {
      std::string value = command_line->GetSwitchValueASCII("device-memory");
      int dm = 0;
      if (base::StringToInt(value, &dm) && dm > 0) {
        device_memory_ = dm;
      }
    }
  }

  bool LoadConfigFromFile() {
    if (config_file_path_.empty()) {
      return false;
    }

    std::string json_content;
    if (!base::ReadFileToString(config_file_path_, &json_content)) {
      return false;  // File doesn't exist or can't read
    }

    auto parsed = base::JSONReader::Read(json_content, base::JSON_PARSE_RFC);
    if (!parsed || !parsed->is_dict()) {
      return false;  // Invalid JSON
    }

    const base::Value::Dict& dict = parsed->GetDict();
    bool has_any_seed = false;

    // Load canvas/main noise seed
    if (const std::string* seed_str = dict.FindString("noise_seed")) {
      uint64_t seed = 0;
      if (base::StringToUint64(*seed_str, &seed) && seed != 0) {
        session_seed_ = seed;
        has_any_seed = true;
      }
    }

    // Load fonts noise seed
    if (const std::string* seed_str = dict.FindString("fonts_noise_seed")) {
      uint64_t seed = 0;
      if (base::StringToUint64(*seed_str, &seed) && seed != 0) {
        fonts_noise_seed_ = seed;
        has_any_seed = true;
      }
    }

    // Load audio noise seed
    if (const std::string* seed_str = dict.FindString("audio_noise_seed")) {
      uint64_t seed = 0;
      if (base::StringToUint64(*seed_str, &seed) && seed != 0) {
        audio_noise_seed_ = seed;
        has_any_seed = true;
      }
    }

    // Load rects noise seed
    if (const std::string* seed_str = dict.FindString("rects_noise_seed")) {
      uint64_t seed = 0;
      if (base::StringToUint64(*seed_str, &seed) && seed != 0) {
        rects_noise_seed_ = seed;
        has_any_seed = true;
      }
    }

    // Load WebGL vendor (for portable fingerprint)
    if (const std::string* vendor = dict.FindString("webgl_vendor")) {
      webgl_vendor_ = *vendor;
    }

    // Load WebGL renderer (for portable fingerprint)
    if (const std::string* renderer = dict.FindString("webgl_renderer")) {
      webgl_renderer_ = *renderer;
    }

    // Load User-Agent (for portable fingerprint)
    if (const std::string* ua = dict.FindString("user_agent")) {
      user_agent_ = *ua;
    }

    // Load hardware concurrency (for portable fingerprint)
    if (std::optional<int> hc = dict.FindInt("hardware_concurrency")) {
      hardware_concurrency_ = *hc;
    }

    // Load device memory (for portable fingerprint)
    if (std::optional<int> dm = dict.FindInt("device_memory")) {
      device_memory_ = *dm;
    }

    // Fill missing seeds with random values and update file
    if (has_any_seed) {
      bool needs_save = false;
      if (session_seed_ == 0) {
        session_seed_ = GenerateRandomSeed();
        needs_save = true;
      }
      if (fonts_noise_seed_ == 0) {
        fonts_noise_seed_ = GenerateRandomSeed();
        needs_save = true;
      }
      if (audio_noise_seed_ == 0) {
        audio_noise_seed_ = GenerateRandomSeed();
        needs_save = true;
      }
      if (rects_noise_seed_ == 0) {
        rects_noise_seed_ = GenerateRandomSeed();
        needs_save = true;
      }
      if (needs_save) {
        SaveConfigToFile();
      }
      return true;
    }

    return false;
  }

  void SaveConfigToFile() {
    if (config_file_path_.empty()) {
      return;
    }

    // First try to load existing config to preserve other fields
    base::Value::Dict config;
    
    std::string existing_json;
    if (base::ReadFileToString(config_file_path_, &existing_json)) {
      auto parsed = base::JSONReader::Read(existing_json, base::JSON_PARSE_RFC);
      if (parsed && parsed->is_dict()) {
        config = std::move(parsed->GetDict());
      }
    }

    // Update seed values
    config.Set("noise_seed", base::NumberToString(session_seed_));
    config.Set("fonts_noise_seed", base::NumberToString(fonts_noise_seed_));
    config.Set("audio_noise_seed", base::NumberToString(audio_noise_seed_));
    config.Set("rects_noise_seed", base::NumberToString(rects_noise_seed_));

    // Write to file
    std::string json_output;
    if (base::JSONWriter::WriteWithOptions(
            config, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json_output)) {
      base::WriteFile(config_file_path_, json_output);
    }
  }

  // Derive deterministic seeds from user-data-dir path hash
  // Same path = same seeds across restarts (stored in RAM cache only)
  void GenerateSeedsFromPath(const std::string& path) {
    uint64_t base_hash = base::PersistentHash(path);
    
    // Derive 4 unique seeds using different salt multipliers
    session_seed_ = base_hash * 0x9e3779b97f4a7c15ULL + 1;
    fonts_noise_seed_ = base_hash * 0x517cc1b727220a95ULL + 2;
    audio_noise_seed_ = base_hash * 0x6c62272e07bb0142ULL + 3;
    rects_noise_seed_ = base_hash * 0xe7037ed1a0b428dbULL + 4;
  }

  void GenerateAllRandomSeeds() {
    session_seed_ = GenerateRandomSeed();
    fonts_noise_seed_ = GenerateRandomSeed();
    audio_noise_seed_ = GenerateRandomSeed();
    rects_noise_seed_ = GenerateRandomSeed();
  }

  uint64_t GenerateRandomSeed() {
    // Use Chromium's base::RandUint64() instead of std::random_device
    // std::random_device uses rand_s on Windows which can crash in renderer
    return base::RandUint64();
  }

  // Hash function that combines session seed with value
  uint64_t HashValue(double value) const {
    uint64_t v = base::bit_cast<uint64_t>(value);
    return session_seed_ ^ (v * 0x9e3779b97f4a7c15ULL);
  }

  // Hash function with range parameters for custom noise ranges
  uint64_t HashValueWithRange(double value,
                               double min_noise,
                               double max_noise) const {
    uint64_t v = base::bit_cast<uint64_t>(value);
    uint64_t min_v = base::bit_cast<uint64_t>(min_noise);
    uint64_t max_v = base::bit_cast<uint64_t>(max_noise);
    return session_seed_ ^ (v * 0x9e3779b97f4a7c15ULL) ^
           (min_v * 0x7f4a7c15) ^ (max_v * 0x4a7c157f);
  }

  uint64_t session_seed_;       // Canvas noise seed
  uint64_t fonts_noise_seed_;   // Fonts noise seed
  uint64_t audio_noise_seed_;   // Audio noise seed
  uint64_t rects_noise_seed_;   // ClientRects noise seed
  
  // Portable fingerprint metadata (loaded from JSON)
  std::string webgl_vendor_;    // WebGL vendor string override
  std::string webgl_renderer_;  // WebGL renderer string override
  std::string user_agent_;      // User-Agent override
  int hardware_concurrency_;    // navigator.hardwareConcurrency override
  int device_memory_;           // navigator.deviceMemory override
  
  base::FilePath config_file_path_;
  HashMap<uint64_t, double> cache_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_SESSION_NOISE_CACHE_H_
