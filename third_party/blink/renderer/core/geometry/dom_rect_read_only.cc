// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/geometry/dom_rect_read_only.h"

#include "base/bit_cast.h"
#include "base/command_line.h"
#include "third_party/blink/renderer/bindings/core/v8/script_value.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_dom_rect_init.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_object_builder.h"
#include "third_party/blink/renderer/platform/privacy_budget/session_noise_cache.h"
#include "ui/gfx/geometry/point_f.h"

namespace blink {

namespace {

// ==========================================================================
// RECTS NOISE - DISABLED BY DEFAULT
// ==========================================================================
// 
// WARNING: Applying noise to DOMRect values causes CreepJS to detect
// "failed math calculation" errors because:
// 1. Layout geometry calculations become inconsistent
// 2. CSS rules like width + padding don't add up correctly
// 3. Element dimensions don't match their parent containers
//
// This feature is DISABLED even when --rects-noise flag is set.
// Fingerprint via rects is less important than Canvas/WebGL, and
// the detection risk is too high.
//
// If you need this feature, set both --rects-noise AND --rects-noise-force
// ==========================================================================

[[maybe_unused]] double ApplyRectsMicroNoise(double value) {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  
  // DISABLED: Only enable if BOTH flags are set
  // This prevents accidental enabling via just --rects-noise
  if (!command_line || 
      !command_line->HasSwitch("rects-noise") ||
      !command_line->HasSwitch("rects-noise-force")) {
    return value;  // Return original value (no noise)
  }

  // Get seed from session cache (persisted per-profile)
  uint64_t seed = SessionNoiseCache::GetInstance().GetRectsNoiseSeed();
  if (seed == 0) {
    seed = SessionNoiseCache::GetInstance().GetSessionSeed();
  }

  // Deterministic hash from seed + value
  uint64_t value_bits = base::bit_cast<uint64_t>(value);
  uint64_t hash = seed ^ (value_bits * 0x9e3779b97f4a7c15ULL);

  // Micro-noise: ±0.0001px (completely invisible)
  constexpr double kMicroNoiseAmplitude = 0.0001;
  double normalized = (static_cast<double>(hash & 0xFFFFFFFF) / 0x7FFFFFFF) - 1.0;
  double noise = normalized * kMicroNoiseAmplitude;

  return value + noise;
}

}  // namespace

DOMRectReadOnly* DOMRectReadOnly::Create(double x,
                                         double y,
                                         double width,
                                         double height) {
  return MakeGarbageCollected<DOMRectReadOnly>(x, y, width, height);
}

ScriptObject DOMRectReadOnly::toJSONForBinding(
    ScriptState* script_state) const {
  V8ObjectBuilder result(script_state);
  result.AddNumber("x", x());
  result.AddNumber("y", y());
  result.AddNumber("width", width());
  result.AddNumber("height", height());
  result.AddNumber("top", top());
  result.AddNumber("right", right());
  result.AddNumber("bottom", bottom());
  result.AddNumber("left", left());
  return result.ToScriptObject();
}

DOMRectReadOnly* DOMRectReadOnly::FromRect(const gfx::Rect& rect) {
  return MakeGarbageCollected<DOMRectReadOnly>(rect.x(), rect.y(), rect.width(),
                                               rect.height());
}

DOMRectReadOnly* DOMRectReadOnly::FromRectF(const gfx::RectF& rect) {
  return MakeGarbageCollected<DOMRectReadOnly>(rect.x(), rect.y(), rect.width(),
                                               rect.height());
}

DOMRectReadOnly* DOMRectReadOnly::fromRect(const DOMRectInit* other) {
  return MakeGarbageCollected<DOMRectReadOnly>(other->x(), other->y(),
                                               other->width(), other->height());
}

DOMRectReadOnly::DOMRectReadOnly(double x,
                                 double y,
                                 double width,
                                 double height)
    : x_(x),
      y_(y),
      width_(width),
      height_(height) {}

gfx::PointF DOMRectReadOnly::Center() const {
  return gfx::PointF(left() + std::fabs(width_) / 2.0,
                     top() + std::fabs(height_) / 2.0);
}

// Getter implementations - noise already applied in constructor
double DOMRectReadOnly::x() const {
  return x_;
}

double DOMRectReadOnly::y() const {
  return y_;
}

double DOMRectReadOnly::width() const {
  return width_;
}

double DOMRectReadOnly::height() const {
  return height_;
}

}  // namespace blink
