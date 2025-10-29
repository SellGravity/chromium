// Copyright 2025 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/canvas/canvas_content_generator.h"
#include "base/strings/stringprintf.h"

namespace blink {

uint32_t IntelligentCanvasSpoofing::LFSR32(uint32_t state) {
  uint32_t bit = state ^ (state >> 2) ^ (state >> 3) ^ (state >> 5);
  return (state >> 1) | (bit << 31);
}

std::vector<uint8_t> IntelligentCanvasSpoofing::GenerateCanvasContent(
    uint32_t width,
    uint32_t height,
    const CanvasDeviceProfile& profile) {
  
  uint32_t seed = profile.GenerateProfileSeed();
  std::vector<uint8_t> pixels(width * height * 4, 0);  // RGBA
  
  uint32_t lfsr = seed;
  for (size_t i = 0; i < pixels.size(); i += 4) {
    lfsr = LFSR32(lfsr);
    
    pixels[i]     = (lfsr >> 0) & 0xFF;    // Red
    pixels[i + 1] = (lfsr >> 8) & 0xFF;    // Green
    pixels[i + 2] = (lfsr >> 16) & 0xFF;   // Blue
    pixels[i + 3] = 0xFF;                   // Alpha
  }
  
  return pixels;
}

String IntelligentCanvasSpoofing::GenerateCanvasHash(
    uint32_t width,
    uint32_t height,
    const CanvasDeviceProfile& profile) {
  
  auto pixels = GenerateCanvasContent(width, height, profile);
  
  // Simple hash function
  uint32_t hash = 5381;
  for (const auto& byte : pixels) {
    hash = ((hash << 5) + hash) + byte;  // DJB2 hash
  }
  
  // Convert to hex string using Chromium WTF String formatting
  std::string hex_str = base::StringPrintf("%08x", hash);
  return String(hex_str.c_str());
}


}  // namespace blink
