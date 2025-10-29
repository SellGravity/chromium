// Copyright 2025 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_CANVAS_CANVAS_CONTENT_GENERATOR_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_CANVAS_CANVAS_CONTENT_GENERATOR_H_

#include <vector>
#include <cstdint>
#include "third_party/blink/renderer/modules/canvas/canvas_device_profile.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class IntelligentCanvasSpoofing {
 public:
  // Generate deterministic canvas content based on device profile
  static std::vector<uint8_t> GenerateCanvasContent(
      uint32_t width,
      uint32_t height,
      const CanvasDeviceProfile& profile);
  
  // Generate canvas hash for fingerprinting
  static String GenerateCanvasHash(
      uint32_t width,
      uint32_t height,
      const CanvasDeviceProfile& profile);
  
 private:
  // LFSR pseudo-random generator
  static uint32_t LFSR32(uint32_t state);
};

}  // namespace blink

#endif
