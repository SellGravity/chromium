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

  // ── GraBrowser: Block sensitive chrome:// internal pages ──
  // Default: block all pages in IsBlockedChromeURL list.
  // Override: if admin added an ALLOW rule on the server for this chrome:// URL,
  //           the page is whitelisted and accessible.
  if (IsBlockedChromeURL(url)) {
    // Check if server has explicitly whitelisted this page.
    // Use url.host() (e.g., "flags") — ExtractDomain() strips "chrome://"
    // from rule patterns when indexing, so the key is just the host.
    bool server_allowed = false;
    ConnectionState state = cache_manager_->GetConnectionState();
    if (state == ConnectionState::SYNCHRONIZED ||
        cache_manager_->HasStartupRules() ||
        cache_manager_->HasEverSynchronized()) {
      PermissionDecision decision = cache_manager_->EvaluateFromCache(
          std::string(url.host()), ResourceType::MAIN_FRAME);
      if (decision.action == PermissionAction::ALLOW &&
          !decision.matched_rule_id.empty()) {
        server_allowed = true;
        DVLOG(1) << "[GraBrowser] Server WHITELISTED internal page: "
                   << url.spec() << " (rule: " << decision.matched_rule_id << ")";
      }
    }

    if (!server_allowed) {
      DVLOG(1) << "[GraBrowser] BLOCKED internal page: " << url.spec();
      return ThrottleCheckResult(CANCEL, net::ERR_BLOCKED_BY_ADMINISTRATOR,
                                 CreateRestrictedPageError(std::string(url.host())));
    }
    // Server whitelisted → fall through to normal PROCEED
    return PROCEED;
  }

  // Bypass non-blocked internal Chrome schemes.
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
      DVLOG(1) << "[PermissionSync] BLOCKED: " << url.spec()
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
      DVLOG(1) << "[PermissionSync] BLOCKED (cached, state="
                  << static_cast<int>(state) << "): " << url.spec()
                  << " | Reason: " << decision.reason;
      return ThrottleCheckResult(CANCEL, net::ERR_BLOCKED_BY_ADMINISTRATOR,
                                 CreateBlockedErrorPage(url.spec()));
    }
    DVLOG(1) << "[PermissionSync] ALLOWED (cached, state="
             << static_cast<int>(state) << "): " << url.spec();
    return PROCEED;
  }

  // ── NO RULES AT ALL → DEFER (wait for initial sync) ──
  // Prevents startup URL from bypassing domain blocking.
  // DEFER until WS sync completes (typically ~1s), then re-evaluate.
  // Timeout after 3s → fail-open (avoid stuck browser if server offline).
  //
  // UX: Browser shows normal loading spinner during DEFER — user sees
  // the same thing as a slow page load. Only happens once per browser
  // session (first navigation before first sync).
  LOG(INFO) << "[PermissionSync] DEFERRED (awaiting initial sync): "
            << url.spec();

  cache_manager_->RegisterInitialSyncCallback(
      base::BindOnce(
          &PermissionSyncNavigationThrottle::OnInitialSyncComplete,
          weak_factory_.GetWeakPtr()));

  initial_sync_timer_.Start(
      FROM_HERE, kInitialSyncTimeout,
      base::BindOnce(
          &PermissionSyncNavigationThrottle::OnInitialSyncTimeout,
          base::Unretained(this)));

  return DEFER;
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
      "* { margin: 0; padding: 0; box-sizing: border-box; }"
      "@keyframes spin { to { transform: rotate(360deg); } }"
      "@keyframes fadeIn { from { opacity: 0; transform: translateY(20px); } "
      "to { opacity: 1; transform: translateY(0); } }"
      "@keyframes pulse { 0%%,100%% { opacity: 0.6; } 50%% { opacity: 1; } }"

      "#loader { position: fixed; inset: 0; background: #0a0e1a; "
      "display: flex; flex-direction: column; align-items: center; "
      "justify-content: center; z-index: 999; transition: opacity 0.5s; }"
      "#loader.hide { opacity: 0; pointer-events: none; }"
      ".spinner { width: 40px; height: 40px; border: 3px solid #1e2a4a; "
      "border-top-color: #ff6b6b; border-radius: 50%%; "
      "animation: spin 0.8s linear infinite; }"
      ".loader-text { color: #5a4a6a; font-size: 13px; margin-top: 16px; "
      "font-family: system-ui, sans-serif; animation: pulse 1.5s infinite; }"

      "body { font-family: 'Segoe UI', system-ui, -apple-system, sans-serif; "
      "background: linear-gradient(135deg, #0a0e1a 0%%, #111827 50%%, "
      "#0f172a 100%%); min-height: 100vh; color: #c8d6e5; "
      "padding: 40px 20px; }"

      ".card { max-width: 640px; margin: 0 auto; background: "
      "rgba(17,24,39,0.85); backdrop-filter: blur(20px); "
      "border-radius: 16px; border: 1px solid rgba(239,68,68,0.1); "
      "box-shadow: 0 8px 32px rgba(0,0,0,0.4), "
      "0 0 0 1px rgba(255,255,255,0.03); overflow: hidden; "
      "animation: fadeIn 0.6s ease-out; }"

      ".header { padding: 32px 32px 24px; text-align: center; "
      "border-bottom: 1px solid rgba(255,255,255,0.06); "
      "background: linear-gradient(180deg, rgba(239,68,68,0.08) 0%%, "
      "transparent 100%%); }"
      ".shield { font-size: 42px; margin-bottom: 12px; "
      "filter: drop-shadow(0 0 12px rgba(239,68,68,0.3)); }"
      ".header h1 { font-size: 20px; font-weight: 600; color: #f87171; "
      "letter-spacing: 0.5px; }"
      ".header p { font-size: 14px; color: #7a8ba5; margin-top: 8px; "
      "line-height: 1.6; }"

      ".url-bar { margin: 20px 32px 0; padding: 10px 16px; "
      "background: rgba(0,0,0,0.3); border-radius: 8px; "
      "border: 1px solid rgba(255,255,255,0.06); "
      "display: flex; align-items: center; gap: 10px; }"
      ".url-label { font-size: 11px; color: #5a6a7a; text-transform: "
      "uppercase; letter-spacing: 1px; white-space: nowrap; }"
      ".url-value { font-family: 'Cascadia Code', 'Fira Code', monospace; "
      "font-size: 13px; color: #f87171; word-break: break-all; }"

      ".disclaimer { margin: 20px 32px 24px; padding: 14px 16px; "
      "background: rgba(251,191,36,0.06); border-radius: 8px; "
      "border: 1px solid rgba(251,191,36,0.12); }"
      ".disclaimer h4 { font-size: 12px; color: #fbbf24; font-weight: 500; "
      "margin-bottom: 8px; display: flex; align-items: center; gap: 6px; }"
      ".disclaimer ol { padding-left: 18px; }"
      ".disclaimer li { font-size: 11px; color: #8a7a5a; line-height: 1.6; "
      "margin-bottom: 3px; }"

      ".footer { padding: 16px 32px 24px; text-align: center; }"
      ".back-btn { padding: 10px 36px; font-size: 13px; font-weight: 500; "
      "background: linear-gradient(135deg, #1e3a5f, #1e4080); "
      "color: #c8d6e5; border: 1px solid rgba(74,158,255,0.2); "
      "border-radius: 8px; cursor: pointer; transition: all 0.25s; }"
      ".back-btn:hover { background: linear-gradient(135deg, #264d73, "
      "#2a5996); box-shadow: 0 4px 16px rgba(74,158,255,0.15); "
      "transform: translateY(-1px); }"
      "</style>"
      "</head>"
      "<body>"

      "<div id='loader'>"
      "<div class='spinner'></div>"
      "<div class='loader-text'>Checking access policy...</div>"
      "</div>"

      "<div class='card' id='content' style='display:none'>"

      "<div class='header'>"
      "<div class='shield'>&#x26D4;</div>"
      "<h1>Access Blocked</h1>"
      "<p>The website you are trying to visit has been blocked by "
      "your organization's access policy. This restriction is enforced "
      "to ensure compliance with security guidelines.</p>"
      "</div>"

      "<div class='url-bar'>"
      "<span class='url-label'>Blocked URL:</span>"
      "<span class='url-value'>%s</span>"
      "</div>"

      "<div class='disclaimer'>"
      "<h4>&#x26A0; Notice &amp; Disclaimer</h4>"
      "<ol>"
      "<li>Access to this website has been restricted by your administrator "
      "based on the current security policy.</li>"
      "<li>If you believe this is an error, please contact your administrator "
      "to request access.</li>"
      "<li>All access attempts are logged and may be reviewed by your "
      "organization.</li>"
      "</ol>"
      "</div>"

      "<div class='footer'>"
      "<button class='back-btn' onclick='history.back()'>&#x2190; "
      "Go Back</button>"
      "</div>"

      "</div>"

      "<script>"
      "setTimeout(function(){"
      "document.getElementById('loader').classList.add('hide');"
      "document.getElementById('content').style.display='block';"
      "},1500);"
      "</script>"

      "</body>"
      "</html>",
      safe_url.c_str());
}

// static
std::string PermissionSyncNavigationThrottle::CreateRestrictedPageError(
    const std::string& page_name) {
  std::string safe_name = base::EscapeForHTML(page_name);

  return base::StringPrintf(
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
      "<meta charset='utf-8'>"
      "<title>Access Restricted</title>"
      "<style>"
      "* { margin: 0; padding: 0; box-sizing: border-box; }"
      "@keyframes spin { to { transform: rotate(360deg); } }"
      "@keyframes fadeIn { from { opacity: 0; transform: translateY(20px); } "
      "to { opacity: 1; transform: translateY(0); } }"
      "@keyframes pulse { 0%%,100%% { opacity: 0.6; } 50%% { opacity: 1; } }"

      /* Loading overlay */
      "#loader { position: fixed; inset: 0; background: #0a0e1a; "
      "display: flex; flex-direction: column; align-items: center; "
      "justify-content: center; z-index: 999; transition: opacity 0.5s; }"
      "#loader.hide { opacity: 0; pointer-events: none; }"
      ".spinner { width: 40px; height: 40px; border: 3px solid #1e2a4a; "
      "border-top-color: #4a9eff; border-radius: 50%%; "
      "animation: spin 0.8s linear infinite; }"
      ".loader-text { color: #4a6a8a; font-size: 13px; margin-top: 16px; "
      "font-family: system-ui, sans-serif; animation: pulse 1.5s infinite; }"

      /* Main layout */
      "body { font-family: 'Segoe UI', system-ui, -apple-system, sans-serif; "
      "background: linear-gradient(135deg, #0a0e1a 0%%, #111827 50%%, "
      "#0f172a 100%%); min-height: 100vh; color: #c8d6e5; "
      "padding: 40px 20px; }"

      /* Card container */
      ".card { max-width: 640px; margin: 0 auto; background: "
      "rgba(17,24,39,0.85); backdrop-filter: blur(20px); "
      "border-radius: 16px; border: 1px solid rgba(74,158,255,0.1); "
      "box-shadow: 0 8px 32px rgba(0,0,0,0.4), "
      "0 0 0 1px rgba(255,255,255,0.03); overflow: hidden; "
      "animation: fadeIn 0.6s ease-out; }"

      /* Header */
      ".header { padding: 32px 32px 24px; text-align: center; "
      "border-bottom: 1px solid rgba(255,255,255,0.06); "
      "background: linear-gradient(180deg, rgba(239,68,68,0.08) 0%%, "
      "transparent 100%%); }"
      ".shield { font-size: 42px; margin-bottom: 12px; "
      "filter: drop-shadow(0 0 12px rgba(239,68,68,0.3)); }"
      ".header h1 { font-size: 20px; font-weight: 600; color: #f87171; "
      "letter-spacing: 0.5px; }"
      ".header p { font-size: 14px; color: #7a8ba5; margin-top: 8px; "
      "line-height: 1.6; }"

      /* URL display */
      ".url-bar { margin: 0 32px; margin-top: 20px; padding: 10px 16px; "
      "background: rgba(0,0,0,0.3); border-radius: 8px; "
      "border: 1px solid rgba(255,255,255,0.06); "
      "display: flex; align-items: center; gap: 10px; }"
      ".url-label { font-size: 11px; color: #5a6a7a; text-transform: "
      "uppercase; letter-spacing: 1px; white-space: nowrap; }"
      ".url-value { font-family: 'Cascadia Code', 'Fira Code', monospace; "
      "font-size: 13px; color: #f87171; word-break: break-all; }"

      /* Actions section */
      ".actions { padding: 24px 32px; }"
      ".actions h2 { font-size: 14px; color: #94a3b8; font-weight: 500; "
      "margin-bottom: 14px; text-transform: uppercase; "
      "letter-spacing: 0.8px; }"
      ".action-list { display: flex; flex-direction: column; gap: 10px; }"
      ".action-item { display: flex; align-items: flex-start; gap: 12px; "
      "padding: 14px 16px; background: rgba(255,255,255,0.02); "
      "border-radius: 10px; border: 1px solid rgba(255,255,255,0.05); "
      "transition: all 0.2s; cursor: default; }"
      ".action-item:hover { background: rgba(74,158,255,0.05); "
      "border-color: rgba(74,158,255,0.15); }"
      ".action-icon { font-size: 20px; flex-shrink: 0; margin-top: 1px; }"
      ".action-text h3 { font-size: 13px; color: #e2e8f0; "
      "font-weight: 500; margin-bottom: 3px; }"
      ".action-text p { font-size: 12px; color: #64748b; line-height: 1.5; }"
      ".action-btn { display: inline-block; margin-top: 6px; padding: "
      "5px 14px; font-size: 11px; background: rgba(74,158,255,0.12); "
      "color: #4a9eff; border: 1px solid rgba(74,158,255,0.25); "
      "border-radius: 5px; cursor: pointer; transition: all 0.2s; "
      "text-decoration: none; }"
      ".action-btn:hover { background: rgba(74,158,255,0.2); }"

      /* Disclaimer */
      ".disclaimer { margin: 0 32px 24px; padding: 14px 16px; "
      "background: rgba(251,191,36,0.06); border-radius: 8px; "
      "border: 1px solid rgba(251,191,36,0.12); }"
      ".disclaimer h4 { font-size: 12px; color: #fbbf24; font-weight: 500; "
      "margin-bottom: 8px; display: flex; align-items: center; gap: 6px; }"
      ".disclaimer ol { padding-left: 18px; }"
      ".disclaimer li { font-size: 11px; color: #8a7a5a; line-height: 1.6; "
      "margin-bottom: 3px; }"

      /* Footer */
      ".footer { padding: 16px 32px 24px; text-align: center; }"
      ".back-btn { padding: 10px 36px; font-size: 13px; font-weight: 500; "
      "background: linear-gradient(135deg, #1e3a5f, #1e4080); "
      "color: #c8d6e5; border: 1px solid rgba(74,158,255,0.2); "
      "border-radius: 8px; cursor: pointer; transition: all 0.25s; }"
      ".back-btn:hover { background: linear-gradient(135deg, #264d73, "
      "#2a5996); box-shadow: 0 4px 16px rgba(74,158,255,0.15); "
      "transform: translateY(-1px); }"
      "</style>"
      "</head>"
      "<body>"

      /* Loading overlay */
      "<div id='loader'>"
      "<div class='spinner'></div>"
      "<div class='loader-text'>Verifying access policy...</div>"
      "</div>"

      /* Main card */
      "<div class='card' id='content' style='display:none'>"

      "<div class='header'>"
      "<div class='shield'>&#x1F6E1;</div>"
      "<h1>Access Restricted</h1>"
      "<p>The page you are trying to access contains internal browser "
      "configuration and has been restricted by your administrator for "
      "security compliance.</p>"
      "</div>"

      "<div class='url-bar'>"
      "<span class='url-label'>Address:</span>"
      "<span class='url-value'>chrome://%s</span>"
      "</div>"

      "<div class='disclaimer'>"
      "<h4>&#x26A0; Notice &amp; Disclaimer</h4>"
      "<ol>"
      "<li>Internal browser pages are restricted to prevent unauthorized "
      "access to system configuration and diagnostic data.</li>"
      "<li>Any attempt to bypass these restrictions may be logged and "
      "reported to your administrator.</li>"
      "<li>The administrator reserves the right to modify access policies "
      "at any time without prior notice.</li>"
      "</ol>"
      "</div>"

      "<div class='footer'>"
      "<button class='back-btn' onclick='history.back()'>&#x2190; "
      "Go Back</button>"
      "</div>"

      "</div>"

      "<script>"
      "setTimeout(function(){"
      "document.getElementById('loader').classList.add('hide');"
      "document.getElementById('content').style.display='block';"
      "},1500);"
      "</script>"

      "</body>"
      "</html>",
      safe_name.c_str());
}

void PermissionSyncNavigationThrottle::OnInitialSyncComplete() {
  initial_sync_timer_.Stop();

  const GURL& url = navigation_handle()->GetURL();
  std::string domain = ExtractDomainFromURL(url);
  ResourceType resource_type = GetResourceType(navigation_handle());

  PermissionDecision decision =
      cache_manager_->EvaluateFromCache(domain, resource_type);

  if (decision.action == PermissionAction::BLOCK) {
    LOG(INFO) << "[PermissionSync] BLOCKED (after initial sync): "
              << url.spec() << " | Rule: " << decision.matched_rule_id;
    CancelDeferredNavigation(
        ThrottleCheckResult(CANCEL, net::ERR_BLOCKED_BY_ADMINISTRATOR,
                            CreateBlockedErrorPage(url.spec())));
  } else {
    LOG(INFO) << "[PermissionSync] ALLOWED (after initial sync): "
              << url.spec();
    Resume();
  }
}

void PermissionSyncNavigationThrottle::OnInitialSyncTimeout() {
  LOG(WARNING) << "[PermissionSync] Initial sync timeout ("
               << kInitialSyncTimeout.InSeconds()
               << "s). Fail-open: "
               << navigation_handle()->GetURL().spec();
  Resume();
}

}  // namespace permission_sync
