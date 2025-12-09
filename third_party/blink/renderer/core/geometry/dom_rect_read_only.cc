// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/geometry/dom_rect_read_only.h"

#include "base/command_line.h"
#include "third_party/blink/renderer/bindings/core/v8/script_value.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_dom_rect_init.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_object_builder.h"
#include "third_party/blink/renderer/platform/privacy_budget/session_noise_cache.h"
#include "ui/gfx/geometry/point_f.h"

namespace blink {

namespace {

// Micro-noise for ClientRects fingerprinting protection.
//
// DESIGN: Same as Font/Canvas micro-noise
// - Visual: Human eye sees NO difference (elements don't shift)
// - Detection: JS getBoundingClientRect() returns slightly different values
//
// The noise amplitude is 0.0001px, which is:
// - Completely invisible (10,000 elements needed to shift 1px total)
// - But changes JS comparison: (rect1.x === rect2.x) → false
//
// NOTE: We apply noise in constructor, so values are cached per DOMRect instance.
// This prevents jitter when reading same rect multiple times.
double ApplyRectsMicroNoise(double value) {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (!command_line || !command_line->HasSwitch("rects-noise")) {
    return value;
  }

  // Get seed from session cache (persisted per-profile)
  uint64_t seed = SessionNoiseCache::GetInstance().GetRectsNoiseSeed();
  if (seed == 0) {
    seed = SessionNoiseCache::GetInstance().GetSessionSeed();
  }

  // Deterministic hash from seed + value
  uint64_t value_bits = *reinterpret_cast<const uint64_t*>(&value);
  uint64_t hash = seed ^ (value_bits * 0x9e3779b97f4a7c15ULL);

  // Micro-noise: ±0.0001px (completely invisible)
  constexpr double kMicroNoiseAmplitude = 0.0001;
  double normalized = (static_cast<double>(hash & 0xFFFFFFFF) / 0x7FFFFFFF) - 1.0;
  double noise = normalized * kMicroNoiseAmplitude;

  return value + noise;
}

// Legacy function name for compatibility
double ApplyRectsNoise(double value) {
  return ApplyRectsMicroNoise(value);
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
    : x_(ApplyRectsNoise(x)),
      y_(ApplyRectsNoise(y)),
      width_(ApplyRectsNoise(width)),
      height_(ApplyRectsNoise(height)) {}

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
