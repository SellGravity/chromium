// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_SESSION_NOISE_CACHE_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_SESSION_NOISE_CACHE_H_

#include <chrono>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <vector>

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
#include "third_party/blink/renderer/platform/allow_discouraged_type.h"
#include "third_party/blink/renderer/platform/privacy_budget/webgl_gpu_profiles.h"


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

  // Get the session seed — independent of --webgl-renderer
  uint64_t GetSessionSeed() const { return session_seed_; }
  
  // Get fonts noise seed — independent of --webgl-renderer
  uint64_t GetFontsNoiseSeed() const { return fonts_noise_seed_; }
  
  // Get audio noise seed — independent of --webgl-renderer
  uint64_t GetAudioNoiseSeed() const { return audio_noise_seed_; }
  
  // Get rects noise seed — independent of --webgl-renderer
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

  // Ensures GPU profile is dynamically applied if it hasn't been yet.
  // This bypasses early-initialization timing bugs where CommandLine might not have switches during constructor.
  void EnsureGpuProfileApplied() {
    if (webgl_renderer_.empty()) {
      auto* cmd = ::base::CommandLine::ForCurrentProcess();
      if (cmd && cmd->HasSwitch("webgl-renderer")) {
        webgl_renderer_ = cmd->GetSwitchValueASCII("webgl-renderer");
        ApplyGpuProfileIfNeeded();
      }
    }
  }

  // --- WebGL Parameter Override Accessors ---
  // Returns spoofed integer value for a GL parameter, if configured.
  std::optional<int> GetWebGLIntOverride(unsigned int pname) {
    EnsureGpuProfileApplied();
    auto it = webgl_int_overrides_.find(pname);
    if (it != webgl_int_overrides_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  // Returns spoofed float value for a GL parameter, if configured.
  std::optional<float> GetWebGLFloatOverride(unsigned int pname) {
    EnsureGpuProfileApplied();
    auto it = webgl_float_overrides_.find(pname);
    if (it != webgl_float_overrides_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  // Returns spoofed float array for a GL parameter (e.g. ALIASED_POINT_SIZE_RANGE).
  std::optional<std::vector<float>> GetWebGLFloatArrayOverride(unsigned int pname) {
    EnsureGpuProfileApplied();
    auto it = webgl_float_array_overrides_.find(pname);
    if (it != webgl_float_array_overrides_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  // Returns spoofed int array for a GL parameter (e.g. MAX_VIEWPORT_DIMS).
  std::optional<std::vector<int>> GetWebGLIntArrayOverride(unsigned int pname) {
    EnsureGpuProfileApplied();
    auto it = webgl_int_array_overrides_.find(pname);
    if (it != webgl_int_array_overrides_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  // Returns spoofed extensions array if a profile is applied.
  std::optional<std::vector<std::string>> GetWebGL1ExtensionsOverride() {
    EnsureGpuProfileApplied();
    if (!webgl1_extensions_override_.empty()) {
      return webgl1_extensions_override_;
    }
    return std::nullopt;
  }

  std::optional<std::vector<std::string>> GetWebGL2ExtensionsOverride() {
    EnsureGpuProfileApplied();
    if (!webgl2_extensions_override_.empty()) {
      return webgl2_extensions_override_;
    }
    return std::nullopt;
  }

  // Compute deterministic noise for a given double value.
  // Thread-safe: no shared mutable state. Same seed + same value = same noise.
  double GetNoise(double value) {
    uint64_t key = HashValue(value);
    std::mt19937_64 gen(key);
    std::uniform_real_distribution<double> dis(-0.5, 0.5);
    return dis(gen);
  }

  // Compute deterministic noise with custom range.
  // Thread-safe: no shared mutable state.
  double GetNoiseInRange(double value, double min_noise, double max_noise) {
    uint64_t key = HashValueWithRange(value, min_noise, max_noise);
    std::mt19937_64 gen(key);
    std::uniform_real_distribution<double> dis(min_noise, max_noise);
    return dis(gen);
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

    // Always try to load fingerprint_config.json from user-data-dir
    // This loads webgl_parameters overrides, seeds, and other metadata
    std::string user_data_dir;
    if (command_line->HasSwitch("user-data-dir")) {
      user_data_dir = command_line->GetSwitchValueASCII("user-data-dir");
    }

    if (!user_data_dir.empty()) {
      config_file_path_ = base::FilePath::FromUTF8Unsafe(user_data_dir)
                              .Append(FILE_PATH_LITERAL("fingerprint_config.json"));
      // Load config — this may override seeds, webgl params, vendor/renderer, etc.
      bool loaded_from_file = LoadConfigFromFile();

      if (has_seeds_from_flags) {
        // CLI seeds take priority over file seeds
      } else if (loaded_from_file) {
        // File provided seeds
      } else {
        // Fallback: derive seeds from path hash
        GenerateSeedsFromPath(user_data_dir);
      }
    } else {
      // In renderer, we must only use explicitly passed seeds from the browser process via command line.
      // If none are passed, they remain 0 (disabled). We do NOT randomly invent seeds here!
    }
    // Auto-apply GPU profile based on webgl_renderer string (applies in both browser and renderer!)
    ApplyGpuProfileIfNeeded();
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



    return has_any_seed;
  }

  void LoadMetadataFromCommandLine(::base::CommandLine* command_line) {
    // WebGL vendor override: --webgl-vendor="Google Inc. (NVIDIA)"
    if (command_line->HasSwitch("webgl-vendor")) {
      webgl_vendor_ = command_line->GetSwitchValueASCII("webgl-vendor");
    }

    // WebGL renderer override: --webgl-renderer="ANGLE (NVIDIA, ...)"
    if (command_line->HasSwitch("webgl-renderer")) {
      webgl_renderer_ = command_line->GetSwitchValueASCII("webgl-renderer");
    }
    
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

    // Load WebGL parameter overrides (for GPU fingerprint spoofing)
    // Format: "webgl_parameters": { "3379": 32768, "34930": 16, ... }
    // Keys are GL enum integer values as strings,
    // values can be int, float, or arrays of int/float.
    if (const base::Value::Dict* params_dict = dict.FindDict("webgl_parameters")) {
      LoadWebGLParameterOverrides(*params_dict);
    }

    if (has_any_seed) {
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

  // Auto-apply built-in GPU profile based on webgl_renderer_ string.
  // Profile values are used as a baseline; explicit JSON webgl_parameters
  // or values already loaded take priority (won't be overwritten).
  // This means: Manager app just sets webgl_renderer, browser fills the rest.
  void ApplyGpuProfileIfNeeded() {
    if (webgl_renderer_.empty()) {
      return;  // No renderer set, nothing to match
    }

    const WebGLGpuProfile* profile = FindGpuProfile(webgl_renderer_);
    if (!profile) {
      return;  // No matching profile found
    }

    // D3D11 same-backend check: skip parameter/extension overrides.
    // WARP (Microsoft Basic Render Driver) with --ignore-gpu-blocklist
    // returns IDENTICAL D3D11 Feature Level 11.0 capabilities as real GPUs.
    // Overriding them causes CreepJS capability hash mismatches because
    // our profile values may differ subtly from the actual D3D11 implementation.
    // WebGL noise is handled separately (currently disabled to prevent
    // BrowserScan cross-path detection).
    bool same_backend = webgl_renderer_.find("D3D11") != std::string::npos;

    if (!same_backend) {
      // Cross-backend spoofing: apply all parameter overrides
      for (const auto& [pname, value] : profile->int_params) {
        if (webgl_int_overrides_.find(pname) == webgl_int_overrides_.end()) {
          webgl_int_overrides_[pname] = value;
        }
      }
      for (const auto& [pname, value] : profile->float_array_params) {
        if (webgl_float_array_overrides_.find(pname) == webgl_float_array_overrides_.end()) {
          webgl_float_array_overrides_[pname] = value;
        }
      }
      for (const auto& [pname, value] : profile->int_array_params) {
        if (webgl_int_array_overrides_.find(pname) == webgl_int_array_overrides_.end()) {
          webgl_int_array_overrides_[pname] = value;
        }
      }
      if (webgl1_extensions_override_.empty()) {
        webgl1_extensions_override_ = profile->webgl1_extensions;
      }
      if (webgl2_extensions_override_.empty()) {
        webgl2_extensions_override_ = profile->webgl2_extensions;
      }
    }
    // D3D11 same-backend: real GPU values pass through (identical to profile)
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

  // Parse a JSON dict of WebGL parameter overrides into typed maps.
  void LoadWebGLParameterOverrides(const base::Value::Dict& params_dict) {
    for (auto [key_str, val] : params_dict) {
      unsigned int pname = 0;
      if (!base::StringToUint(key_str, &pname)) {
        continue;  // Skip keys that aren't valid uint GL enum values
      }

      if (val.is_int()) {
        webgl_int_overrides_[pname] = val.GetInt();
      } else if (val.is_double()) {
        webgl_float_overrides_[pname] = static_cast<float>(val.GetDouble());
      } else if (val.is_list()) {
        const base::Value::List& arr = val.GetList();
        bool all_int = true;
        bool all_numeric = true;
        for (const auto& elem : arr) {
          if (!elem.is_int() && !elem.is_double()) {
            all_numeric = false;
            break;
          }
          if (!elem.is_int()) {
            all_int = false;
          }
        }
        if (!all_numeric || arr.empty()) continue;

        if (all_int) {
          std::vector<int> int_arr;
          int_arr.reserve(arr.size());
          for (const auto& elem : arr) {
            int_arr.push_back(elem.GetInt());
          }
          webgl_int_array_overrides_[pname] = std::move(int_arr);
        } else {
          std::vector<float> float_arr;
          float_arr.reserve(arr.size());
          for (const auto& elem : arr) {
            float_arr.push_back(
                static_cast<float>(elem.is_int() ? elem.GetInt()
                                                 : elem.GetDouble()));
          }
          webgl_float_array_overrides_[pname] = std::move(float_arr);
        }
      }
    }
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

  // WebGL parameter override maps (keyed by GLenum integer value)
  std::map<unsigned int, int> webgl_int_overrides_
      ALLOW_DISCOURAGED_TYPE("Loaded once at init from JSON; no blink HashMap needed");
  std::map<unsigned int, float> webgl_float_overrides_
      ALLOW_DISCOURAGED_TYPE("Loaded once at init from JSON; no blink HashMap needed");
  std::map<unsigned int, std::vector<float>> webgl_float_array_overrides_
      ALLOW_DISCOURAGED_TYPE("Loaded once at init from JSON; no blink HashMap needed");
  std::map<unsigned int, std::vector<int>> webgl_int_array_overrides_
      ALLOW_DISCOURAGED_TYPE("Loaded once at init from JSON; no blink HashMap needed");
  std::vector<std::string> webgl1_extensions_override_
      ALLOW_DISCOURAGED_TYPE("Loaded once at init from JSON; no blink HashMap needed");
  std::vector<std::string> webgl2_extensions_override_
      ALLOW_DISCOURAGED_TYPE("Loaded once at init from JSON; no blink HashMap needed");
  
  base::FilePath config_file_path_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_SESSION_NOISE_CACHE_H_
