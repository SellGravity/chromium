// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_NAVIGATION_THROTTLE_H_
#define CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_NAVIGATION_THROTTLE_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/navigation_throttle.h"

namespace content {
class NavigationHandle;
class NavigationThrottleRegistry;
}  // namespace content

namespace permission_sync {

class PermissionCacheManager;
struct PermissionDecision;

// ============================================================================
// PermissionSyncNavigationThrottle (FRS Section 3.4.2)
// ============================================================================
// Intercepts MAIN_FRAME and SUB_FRAME navigations and evaluates them against
// the PermissionCacheManager's in-memory cache.
//
// Flow per connection state:
//   SYNCHRONIZED → EvaluateFromCache() synchronously → PROCEED or CANCEL
//   CONNECTING/SYNCING → DEFER, callback fires when cache ready
//   DISCONNECTED → CANCEL (fail-secure, FRS §7)
//
// Note on WillProcessResponse: Not overridden because we only check domain
// against permission rules. Meta refresh and JS redirects create new
// navigations with their own WillStartRequest() calls. If future requirements
// need final-URL or response-header checks, add WillProcessResponse here.
//
class PermissionSyncNavigationThrottle : public content::NavigationThrottle {
 public:
  // Factory: creates and adds throttle to registry if PermissionCacheManager
  // is available for the profile.
  static void MaybeCreateAndAdd(
      content::NavigationThrottleRegistry& registry);

  PermissionSyncNavigationThrottle(
      content::NavigationThrottleRegistry& registry,
      PermissionCacheManager* cache_manager);

  PermissionSyncNavigationThrottle(
      const PermissionSyncNavigationThrottle&) = delete;
  PermissionSyncNavigationThrottle& operator=(
      const PermissionSyncNavigationThrottle&) = delete;

  ~PermissionSyncNavigationThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  const char* GetNameForLogging() override;

 private:
  // Single evaluation path per request.
  ThrottleCheckResult CheckPermission();

  // Callback from EvaluateRequestAsync (deferred path only).
  void OnPermissionDecision(PermissionDecision decision);

  // Build a blocked error page HTML with XSS-safe URL escaping.
  static std::string CreateBlockedErrorPage(const std::string& url);

  const raw_ptr<PermissionCacheManager> cache_manager_;

  base::WeakPtrFactory<PermissionSyncNavigationThrottle> weak_factory_{this};
};

}  // namespace permission_sync

#endif  // CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_NAVIGATION_THROTTLE_H_
