// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NAVIGATION_PROFILE_URL_BLOCKER_NAVIGATION_THROTTLE_H_
#define CHROME_BROWSER_NAVIGATION_PROFILE_URL_BLOCKER_NAVIGATION_THROTTLE_H_

#include <memory>
#include <string>

#include "components/url_matcher/url_matcher.h"
#include "content/public/browser/navigation_throttle.h"

class GURL;
class PrefService;

namespace policy_manager {
class PolicyIPCClient;
}  // namespace policy_manager

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
  // Check if URL is in blocklist using Chrome's URLMatcher
  bool IsURLBlocked(const GURL& url);

  // Check if URL is in whitelist (whitelist takes priority)
  bool IsURLWhitelisted(const GURL& url);

  // Check URL via IPC policy server (real-time check)
  bool IsURLBlockedViaIPC(const GURL& url);

  // Show notification that URL was blocked
  void ShowBlockedNotification(const GURL& blocked_url);

  // Build URL matchers from preference patterns
  void RebuildURLMatchers();

  // Create URLMatcher condition set from a pattern string
  scoped_refptr<url_matcher::URLMatcherConditionSet>
  CreateConditionSetFromPattern(const std::string& pattern,
                                 base::MatcherStringPattern::ID id);

  const raw_ptr<PrefService> prefs_;

  // URLMatcher for efficient pattern matching
  std::unique_ptr<url_matcher::URLMatcher> blocklist_matcher_;
  std::unique_ptr<url_matcher::URLMatcher> whitelist_matcher_;
};

#endif  // CHROME_BROWSER_NAVIGATION_PROFILE_URL_BLOCKER_NAVIGATION_THROTTLE_H_
