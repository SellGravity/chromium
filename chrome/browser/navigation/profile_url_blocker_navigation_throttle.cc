// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/navigation/profile_url_blocker_navigation_throttle.h"

#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "net/base/net_errors.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace {

// Check if URL matches pattern
// Supports: exact match, domain match, wildcard subdomain match
// Examples:
//   "facebook.com" matches "facebook.com" and "www.facebook.com"
//   "*.reddit.com" matches "old.reddit.com" but not "reddit.com"
//   "https://example.com/path" matches exact URL
bool URLMatchesPattern(const GURL& url, const std::string& pattern) {
  std::string url_host(url.host());
  std::string url_spec = url.spec();

  // Exact URL match
  if (url_spec.find(pattern) == 0) {
    return true;
  }

  // Domain match (e.g., "facebook.com" matches "www.facebook.com")
  if (url_host == pattern || url_host.ends_with("." + pattern)) {
    return true;
  }

  // Wildcard subdomain match (e.g., "*.example.com")
  if (pattern.starts_with("*.")) {
    std::string domain = pattern.substr(2);  // Remove "*."
    if (url_host.ends_with("." + domain)) {
      return true;
    }
  }

  return false;
}

}  // namespace

// static
std::unique_ptr<ProfileURLBlockerNavigationThrottle>
ProfileURLBlockerNavigationThrottle::MaybeCreateFor(
    content::NavigationThrottleRegistry& registry,
    PrefService* prefs) {
  if (!prefs) {
    return nullptr;
  }

  // Only create throttle if blocklist is not empty
  if (!prefs->HasPrefPath(prefs::kProfileURLBlocklist)) {
    return nullptr;
  }

  const base::Value::List& blocklist =
      prefs->GetList(prefs::kProfileURLBlocklist);
  if (blocklist.empty()) {
    return nullptr;
  }

  return std::make_unique<ProfileURLBlockerNavigationThrottle>(registry, prefs);
}

ProfileURLBlockerNavigationThrottle::ProfileURLBlockerNavigationThrottle(
    content::NavigationThrottleRegistry& registry,
    PrefService* prefs)
    : content::NavigationThrottle(registry), prefs_(prefs) {}

ProfileURLBlockerNavigationThrottle::~ProfileURLBlockerNavigationThrottle() =
    default;

content::NavigationThrottle::ThrottleCheckResult
ProfileURLBlockerNavigationThrottle::WillStartRequest() {
  const GURL& url = navigation_handle()->GetURL();

  // Don't block internal Chrome pages
  if (url.SchemeIs("chrome") || url.SchemeIs("chrome-extension") ||
      url.SchemeIs("devtools") || url.SchemeIs("about")) {
    return PROCEED;
  }

  if (IsURLBlocked(url)) {
    ShowBlockedNotification(url);

    // Create custom error page with confirmation dialog
    std::string error_html = base::StringPrintf(
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset='utf-8'>"
        "<title>Access Blocked</title>"
        "<style>"
        "body { font-family: Arial, sans-serif; text-align: center; "
        "padding: 50px; background: #f5f5f5; }"
        ".container { background: white; padding: 40px; border-radius: 8px; "
        "box-shadow: 0 2px 10px rgba(0,0,0,0.1); max-width: 500px; margin: 0 auto; }"
        "h1 { color: #d93025; }"
        "p { color: #5f6368; line-height: 1.6; }"
        ".blocked-url { color: #1a73e8; font-weight: bold; word-break: break-all; }"
        "button { background: #1a73e8; color: white; border: none; "
        "padding: 12px 24px; font-size: 16px; border-radius: 4px; "
        "cursor: pointer; margin-top: 20px; }"
        "button:hover { background: #1557b0; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class='container'>"
        "<h1>🚫 Access Blocked</h1>"
        "<p>You do not have access to this site:</p>"
        "<p class='blocked-url'>%s</p>"
        "<p>This URL has been blocked by your profile settings.</p>"
        "<button onclick='goBack()'>OK</button>"
        "</div>"
        "<script>"
        "function goBack() {"
        "  window.history.back();"
        "}"
        "</script>"
        "</body>"
        "</html>",
        url.spec().c_str());

    // Return error with custom HTML content
    return ThrottleCheckResult(CANCEL, net::ERR_BLOCKED_BY_ADMINISTRATOR,
                               error_html);
  }

  return PROCEED;
}

content::NavigationThrottle::ThrottleCheckResult
ProfileURLBlockerNavigationThrottle::WillRedirectRequest() {
  // Also check redirects
  return WillStartRequest();
}

const char* ProfileURLBlockerNavigationThrottle::GetNameForLogging() {
  return "ProfileURLBlockerNavigationThrottle";
}

bool ProfileURLBlockerNavigationThrottle::IsURLBlocked(const GURL& url) {
  if (!prefs_ || !prefs_->HasPrefPath(prefs::kProfileURLBlocklist)) {
    return false;
  }

  const base::Value::List& blocklist =
      prefs_->GetList(prefs::kProfileURLBlocklist);

  for (const auto& pattern_value : blocklist) {
    if (!pattern_value.is_string()) {
      continue;
    }

    const std::string& pattern = pattern_value.GetString();
    if (URLMatchesPattern(url, pattern)) {
      return true;
    }
  }

  return false;
}

void ProfileURLBlockerNavigationThrottle::ShowBlockedNotification(
    const GURL& blocked_url) {
  // TODO: Implement notification UI
  // For now, we'll just log. The notification can be added using:
  // - Chrome notifications API
  // - InfoBar
  // - Or a simple console message

  LOG(INFO) << "[Profile URL Blocker] Blocked access to: " << blocked_url.spec()
            << " - Redirecting to google.com";

  // You could also use content::WebContents to show a message:
  // content::WebContents* web_contents = navigation_handle()->GetWebContents();
  // if (web_contents) {
  //   // Show InfoBar or notification here
  // }
}
