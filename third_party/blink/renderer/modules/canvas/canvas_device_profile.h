// Copyright 2025 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_CANVAS_CANVAS_DEVICE_PROFILE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_CANVAS_CANVAS_DEVICE_PROFILE_H_

#include <cstdint>

namespace blink {

// Device profile for intelligent canvas spoofing
struct CanvasDeviceProfile {
  enum class OS {
    kWindows,
    kMacOS,
    kLinux,
    kAndroid,
    kIOS,
  };
  
  enum class Browser {
    kChrome,
    kFirefox,
    kSafari,
    kEdge,
  };
  
  enum class GPUVendor {
    kNVIDIA,
    kAMD,
    kIntel,
    kApple,
  };
  
  OS operating_system = OS::kWindows;
  Browser browser = Browser::kChrome;
  GPUVendor gpu_vendor = GPUVendor::kNVIDIA;
  
  // Generate stable seed based on profile
  uint32_t GenerateProfileSeed() const {
    uint32_t seed = 0;
    seed |= (static_cast<uint32_t>(operating_system) << 16);
    seed |= (static_cast<uint32_t>(browser) << 8);
    seed |= (static_cast<uint32_t>(gpu_vendor));
    return seed;
  }
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_CANVAS_CANVAS_DEVICE_PROFILE_H_
