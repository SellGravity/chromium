// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NAVIGATION_PROFILE_URL_BLOCKER_NAVIGATION_THROTTLE_H_
#define CHROME_BROWSER_NAVIGATION_PROFILE_URL_BLOCKER_NAVIGATION_THROTTLE_H_

#include "content/public/browser/navigation_throttle.h"

class GURL;
class PrefService;

namespace content {
class NavigationHandle;
class NavigationThrottleRegistry;
}  // namespace content

// ProfileURLBlockerNavigationThrottle blocks URLs based on per-profile
// blocklist preference. When a URL is blocked, the user is redirected to
// google.com and shown a notification.
//
// This is different from policy-based URL blocking in that:
// - It's per-profile, not enterprise policy
// - It redirects instead of showing error page
// - It shows a custom notification message
class ProfileURLBlockerNavigationThrottle
    : public content::NavigationThrottle {
 public:
  static std::unique_ptr<ProfileURLBlockerNavigationThrottle> MaybeCreateFor(
      content::NavigationThrottleRegistry& registry,
      PrefService* prefs);

  ProfileURLBlockerNavigationThrottle(
      content::NavigationThrottleRegistry& registry,
      PrefService* prefs);
  ProfileURLBlockerNavigationThrottle(
      const ProfileURLBlockerNavigationThrottle&) = delete;
  ProfileURLBlockerNavigationThrottle& operator=(
      const ProfileURLBlockerNavigationThrottle&) = delete;
  ~ProfileURLBlockerNavigationThrottle() override;

  // NavigationThrottle overrides.
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  const char* GetNameForLogging() override;

 private:
  // Check if URL is in blocklist and should be blocked
  bool IsURLBlocked(const GURL& url);

  // Show notification that URL was blocked
  void ShowBlockedNotification(const GURL& blocked_url);

  const raw_ptr<PrefService> prefs_;
};

#endif  // CHROME_BROWSER_NAVIGATION_PROFILE_URL_BLOCKER_NAVIGATION_THROTTLE_H_
