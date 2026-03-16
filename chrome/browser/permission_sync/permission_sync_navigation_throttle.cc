// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/permission_sync/permission_sync_navigation_throttle.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/permission_sync/permission_cache_manager.h"
#include "chrome/browser/permission_sync/permission_cache_manager_factory.h"
#include "chrome/browser/permission_sync/permission_sync_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"
#include "url/gurl.h"

namespace permission_sync {

namespace {

// Map NavigationHandle to ResourceType.
ResourceType GetResourceType(content::NavigationHandle* handle) {
  if (handle->IsInMainFrame()) {
    return ResourceType::MAIN_FRAME;
  }
  return ResourceType::SUB_FRAME;
}

}  // namespace

// static
void PermissionSyncNavigationThrottle::MaybeCreateAndAdd(
    content::NavigationThrottleRegistry& registry) {
  content::NavigationHandle& handle = registry.GetNavigationHandle();
  content::WebContents* web_contents = handle.GetWebContents();
  if (!web_contents) {
    return;
  }

  Profile* profile =
      Profile::FromBrowserContext(web_contents->GetBrowserContext());
  if (!profile || profile->IsOffTheRecord()) {
    return;
  }

  auto* cache_manager =
      PermissionCacheManagerFactory::GetForProfile(profile);
  if (!cache_manager) {
    return;
  }

  registry.AddThrottle(
      std::make_unique<PermissionSyncNavigationThrottle>(
          registry, cache_manager));
}

PermissionSyncNavigationThrottle::PermissionSyncNavigationThrottle(
    content::NavigationThrottleRegistry& registry,
    PermissionCacheManager* cache_manager)
    : content::NavigationThrottle(registry),
      cache_manager_(cache_manager) {
  DCHECK(cache_manager_);
}

PermissionSyncNavigationThrottle::~PermissionSyncNavigationThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
PermissionSyncNavigationThrottle::WillStartRequest() {
  return CheckPermission();
}

content::NavigationThrottle::ThrottleCheckResult
PermissionSyncNavigationThrottle::WillRedirectRequest() {
  return CheckPermission();
}

const char* PermissionSyncNavigationThrottle::GetNameForLogging() {
  return "PermissionSyncNavigationThrottle";
}

// Fix #1: Fail-OPEN design. No rules loaded → allow everything.
// Only BLOCK when a rule explicitly matches.
// Never DEFER indefinitely — avoids infinite loading.
content::NavigationThrottle::ThrottleCheckResult
PermissionSyncNavigationThrottle::CheckPermission() {
  const GURL& url = navigation_handle()->GetURL();

  // Bypass internal Chrome schemes.
  if (ShouldBypassScheme(url)) {
    return PROCEED;
  }

  std::string domain = ExtractDomainFromURL(url);
  ResourceType resource_type = GetResourceType(navigation_handle());
  ConnectionState state = cache_manager_->GetConnectionState();

  // ── FAST PATH: SYNCHRONIZED → evaluate from cache ──
  if (state == ConnectionState::SYNCHRONIZED) {
    PermissionDecision decision =
        cache_manager_->EvaluateFromCache(domain, resource_type);

    if (decision.action == PermissionAction::BLOCK) {
      LOG(INFO) << "[PermissionSync] BLOCKED: " << url.spec()
                << " | Reason: " << decision.reason;
      return ThrottleCheckResult(CANCEL, net::ERR_BLOCKED_BY_ADMINISTRATOR,
                                 CreateBlockedErrorPage(url.spec()));
    }

    DVLOG(1) << "[PermissionSync] ALLOWED: " << url.spec();
    return PROCEED;
  }

  // ── HAS RULES (startup or previously synced) → evaluate from cache ──
  if (cache_manager_->HasStartupRules() ||
      cache_manager_->HasEverSynchronized()) {
    PermissionDecision decision =
        cache_manager_->EvaluateFromCache(domain, resource_type);
    if (decision.action == PermissionAction::BLOCK) {
      LOG(INFO) << "[PermissionSync] BLOCKED (cached, state="
                << static_cast<int>(state) << "): " << url.spec()
                << " | Reason: " << decision.reason;
      return ThrottleCheckResult(CANCEL, net::ERR_BLOCKED_BY_ADMINISTRATOR,
                                 CreateBlockedErrorPage(url.spec()));
    }
    DVLOG(1) << "[PermissionSync] ALLOWED (cached, state="
             << static_cast<int>(state) << "): " << url.spec();
    return PROCEED;
  }

  // ── NO RULES AT ALL → fail-open (ALLOW everything) ──
  // No startup rules, never synced. Don't block the user.
  // WS will sync rules in background; once synced, rules will apply.
  LOG(INFO) << "[PermissionSync] ALLOWED (no rules loaded yet, fail-open): "
            << url.spec();
  return PROCEED;
}

void PermissionSyncNavigationThrottle::OnPermissionDecision(
    PermissionDecision decision) {
  if (decision.action == PermissionAction::BLOCK) {
    const GURL& url = navigation_handle()->GetURL();
    LOG(INFO) << "[PermissionSync] BLOCKED (deferred): " << url.spec()
              << " | Reason: " << decision.reason;
    CancelDeferredNavigation(
        ThrottleCheckResult(CANCEL, net::ERR_BLOCKED_BY_ADMINISTRATOR,
                            CreateBlockedErrorPage(url.spec())));
  } else {
    DVLOG(1) << "[PermissionSync] ALLOWED (deferred): "
             << navigation_handle()->GetURL().spec();
    Resume();
  }
}

// static
// Fix #3: HTML-escape URL to prevent XSS in error page.
std::string PermissionSyncNavigationThrottle::CreateBlockedErrorPage(
    const std::string& url) {
  // Escape HTML special characters to prevent injection.
  std::string safe_url = base::EscapeForHTML(url);

  return base::StringPrintf(
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
      "<meta charset='utf-8'>"
      "<title>Access Blocked</title>"
      "<style>"
      "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', "
      "Roboto, sans-serif; text-align: center; padding: 50px; "
      "background: #f5f5f5; }"
      ".container { background: white; padding: 40px; border-radius: 12px; "
      "box-shadow: 0 4px 20px rgba(0,0,0,0.08); max-width: 500px; "
      "margin: 80px auto; }"
      "h1 { color: #d93025; font-size: 24px; }"
      "p { color: #5f6368; line-height: 1.8; }"
      ".blocked-url { color: #1a73e8; font-weight: 600; "
      "word-break: break-all; font-size: 14px; "
      "background: #e8f0fe; padding: 8px 16px; border-radius: 6px; "
      "display: inline-block; margin: 12px 0; }"
      "button { background: #1a73e8; color: white; border: none; "
      "padding: 12px 32px; font-size: 15px; border-radius: 6px; "
      "cursor: pointer; margin-top: 20px; transition: background 0.2s; }"
      "button:hover { background: #1557b0; }"
      "</style>"
      "</head>"
      "<body>"
      "<div class='container'>"
      "<h1>&#x26D4; Access Blocked</h1>"
      "<p>You do not have permission to access:</p>"
      "<p class='blocked-url'>%s</p>"
      "<p>This URL has been blocked by your organization's policy.</p>"
      "<button onclick='history.back()'>Go Back</button>"
      "</div>"
      "</body>"
      "</html>",
      safe_url.c_str());
}

}  // namespace permission_sync
