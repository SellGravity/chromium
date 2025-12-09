// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/frame/navigator_concurrent_hardware.h"

#include "base/system/sys_info.h"
#include "third_party/blink/renderer/platform/privacy_budget/session_noise_cache.h"

namespace blink {

unsigned NavigatorConcurrentHardware::hardwareConcurrency() const {
  int override_concurrency = SessionNoiseCache::GetInstance().GetHardwareConcurrency();
  if (override_concurrency > 0) {
    return static_cast<unsigned>(override_concurrency);
  }
  return static_cast<unsigned>(base::SysInfo::NumberOfProcessors());
}

}  // namespace blink
