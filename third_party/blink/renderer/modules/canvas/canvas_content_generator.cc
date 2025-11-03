// Copyright 2025 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/canvas/canvas_content_generator.h"
#include "base/strings/stringprintf.h"
#include "base/logging.h"
#include <algorithm>

namespace blink {

uint32_t IntelligentCanvasSpoofing::LFSR32(uint32_t state) {
  // ✅ FIX: Kiểm tra state != 0 để tránh infinite loop
  if (state == 0) {
    state = 1;
  }
  
  uint32_t bit = state ^ (state >> 2) ^ (state >> 3) ^ (state >> 5);
  return (state >> 1) | (bit << 31);
}

std::vector<uint8_t> IntelligentCanvasSpoofing::GenerateCanvasContent(
    uint32_t width,
    uint32_t height,
    const CanvasDeviceProfile& profile) {
  
  // ✅ KIỂM TRA INPUT: Width và height phải > 0
  if (width == 0 || height == 0) {
    DVLOG(2) << "Invalid canvas dimensions: " << width << "x" << height;
    return std::vector<uint8_t>();
  }

  // ✅ KIỂM TRA OVERFLOW: width * height có thể quá lớn
  // Sử dụng size_t (unsigned long) để tính toán an toàn
  if (width > UINT32_MAX / height) {
    // Overflow would occur
    DVLOG(2) << "Canvas dimensions would cause overflow: " << width << "x" << height;
    return std::vector<uint8_t>();
  }
  
  size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
  
  // Giới hạn canvas: 10000 x 10000 pixels = 100 megapixels
  const size_t MAX_PIXELS = 100000000;
  if (pixel_count > MAX_PIXELS) {
    DVLOG(2) << "Canvas size too large: " << width << "x" << height;
    return std::vector<uint8_t>();
  }

  // ✅ KHÔNG DÙNG TRY-CATCH (Chromium disable exceptions)
  // Thay vào đó, khởi tạo vector và kiểm tra size
  std::vector<uint8_t> pixels;
  
  // Tính toán size cần allocate (pixel_count * 4 bytes cho RGBA)
  size_t total_bytes = pixel_count * 4;
  
  // Kiểm tra xem có đủ memory không (rough check)
  if (total_bytes > 1000000000) {  // > 1GB
    DVLOG(1) << "Requested memory too large: " << total_bytes << " bytes";
    return std::vector<uint8_t>();
  }
  
  // Reserve capacity trước
  pixels.reserve(total_bytes);
  
  // Resize với default value 0
  pixels.resize(total_bytes, 0);
  
  // Kiểm tra allocation thành công
  if (pixels.size() != total_bytes) {
    DVLOG(1) << "Failed to allocate canvas memory";
    return std::vector<uint8_t>();
  }

  uint32_t seed = profile.GenerateProfileSeed();
  uint32_t lfsr = seed;

  // ✅ LOOP ĐÚNG: Iterate qua từng pixel (mỗi 4 bytes)
  for (size_t i = 0; i < pixels.size(); i += 4) {
    // ✅ KIỂM TRA BOUNDS: Đảm bảo có đủ 4 bytes để viết
    if (i + 3 >= pixels.size()) {
      break; // Dừng lặp nếu không đủ bytes
    }

    lfsr = LFSR32(lfsr);
    
    // Ghi 4 bytes (RGBA)
    pixels[i]     = (lfsr >> 0) & 0xFF;    // Red channel
    pixels[i + 1] = (lfsr >> 8) & 0xFF;    // Green channel
    pixels[i + 2] = (lfsr >> 16) & 0xFF;   // Blue channel
    pixels[i + 3] = 0xFF;                   // Alpha = 255 (opaque)
  }
  
  return pixels;
}

String IntelligentCanvasSpoofing::GenerateCanvasHash(
    uint32_t width,
    uint32_t height,
    const CanvasDeviceProfile& profile) {
  
  auto pixels = GenerateCanvasContent(width, height, profile);
  
  // ✅ Kiểm tra pixels có dữ liệu không
  if (pixels.empty()) {
    return String("error");
  }

  // DJB2 hash function
  uint32_t hash = 5381;
  for (const auto& byte : pixels) {
    hash = ((hash << 5) + hash) + byte;
  }
  
  // Convert uint32_t thành hex string
  std::string hex_str = base::StringPrintf("%08x", hash);
  return String(hex_str.c_str());
}

}  // namespace blink