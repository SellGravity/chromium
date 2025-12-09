// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/navigation/profile_url_blocker_navigation_throttle.h"

#include "base/environment.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "chrome/browser/policy_manager/policy_ipc_client.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/url_matcher/url_matcher.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "net/base/net_errors.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

// static
std::unique_ptr<ProfileURLBlockerNavigationThrottle>
ProfileURLBlockerNavigationThrottle::MaybeCreateFor(
    content::NavigationThrottleRegistry& registry,
    PrefService* prefs) {
  if (!prefs) {
    return nullptr;
  }

  // Create throttle even if blocklist is empty (for IPC checks)
  return std::make_unique<ProfileURLBlockerNavigationThrottle>(registry, prefs);
}

ProfileURLBlockerNavigationThrottle::ProfileURLBlockerNavigationThrottle(
    content::NavigationThrottleRegistry& registry,
    PrefService* prefs)
    : content::NavigationThrottle(registry),
      prefs_(prefs),
      blocklist_matcher_(std::make_unique<url_matcher::URLMatcher>()),
      whitelist_matcher_(std::make_unique<url_matcher::URLMatcher>()) {
  // Build initial matchers
  RebuildURLMatchers();
}

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

  // ========== SKIP SUB-FRAME & RESOURCE REQUESTS FOR PERFORMANCE ==========
  // Only check main frame navigations to reduce latency
  // Sub-resources (images, scripts, etc.) inherit the main frame's policy
  if (!navigation_handle()->IsInMainFrame()) {
    return PROCEED;
  }

  // Get profile name from navigation context
  content::WebContents* web_contents = navigation_handle()->GetWebContents();
  Profile* profile =
      Profile::FromBrowserContext(web_contents->GetBrowserContext());
  std::string profile_name = profile->GetBaseName().AsUTF8Unsafe();

  // ========== BYPASS OTR PROFILES (DevTools, Incognito) ==========
  // OTR profiles are temporary profiles used for:
  // - Lighthouse/DevTools audits (created by Target.createBrowserContext)
  // - Incognito mode
  // - Guest mode
  // These should bypass policy checks to avoid conflicts
  if (profile->IsOffTheRecord()) {
    DVLOG(1) << "[Profile URL Blocker] BYPASS: OTR profile detected: "
              << profile_name << " (Lighthouse/Incognito/Guest mode)";
    return PROCEED;
  }

  DVLOG(2) << "[Profile URL Blocker] Checking URL for profile: "
            << profile_name;

  // PRIORITY 1: Check IPC server first (real-time, highest priority)
  // IPC can whitelist URLs that override local blocklist
  // IMPORTANT: Always check IPC even when disconnected for fail-closed security
  auto* ipc_client = policy_manager::PolicyIPCClient::GetInstance();
  if (ipc_client) {
    // Pass profile name explicitly for per-profile policy
    // CheckURLSync handles fail-closed logic internally (blocks when server offline)
    policy_manager::PolicyDecision decision =
        ipc_client->CheckURLSync(url.spec(), profile_name);

    if (decision.allow) {
      // Server explicitly allows this URL (either whitelisted or not in any list)
      DVLOG(1) << "[Profile URL Blocker] URL allowed by IPC: " << url.spec()
                << " | Profile: " << profile_name
                << " (reason: " << decision.reason << ")";
      return PROCEED;
    } else {
      // Server explicitly blocks this URL (or LOCKDOWN mode active)
      LOG(INFO) << "[Profile URL Blocker] URL blocked by IPC: " << url.spec()
                << " | Profile: " << profile_name
                << " (reason: " << decision.reason << ")";
      ShowBlockedNotification(url);

    // Create custom error page
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
        "<h1>Access Blocked</h1>"
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
  }

  // PRIORITY 2: Fallback to local prefs if IPC not available
  // Check local whitelist first (whitelist overrides everything)
  if (IsURLWhitelisted(url)) {
    DVLOG(1) << "[Profile URL Blocker] URL whitelisted (local prefs): " << url.spec();
    return PROCEED;
  }

  // Check local blocklist
  if (IsURLBlocked(url)) {
    LOG(INFO) << "[Profile URL Blocker] URL blocked (local prefs): " << url.spec();
    ShowBlockedNotification(url);

    // Create custom error page
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
        "<h1>Access Blocked</h1>"
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

    return ThrottleCheckResult(CANCEL, net::ERR_BLOCKED_BY_ADMINISTRATOR,
                               error_html);
  }

  // PRIORITY 3: Default allow (not in any list)
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

void ProfileURLBlockerNavigationThrottle::RebuildURLMatchers() {
  // Clear existing matchers
  blocklist_matcher_ = std::make_unique<url_matcher::URLMatcher>();
  whitelist_matcher_ = std::make_unique<url_matcher::URLMatcher>();

  if (!prefs_) {
    return;
  }

  // Build blocklist matcher
  if (prefs_->HasPrefPath(prefs::kProfileURLBlocklist)) {
    const base::Value::List& blocklist =
        prefs_->GetList(prefs::kProfileURLBlocklist);

    url_matcher::URLMatcherConditionSet::Vector condition_sets;
    base::MatcherStringPattern::ID id = 0;

    for (const auto& pattern_value : blocklist) {
      if (!pattern_value.is_string()) {
        continue;
      }

      const std::string& pattern = pattern_value.GetString();
      auto condition_set = CreateConditionSetFromPattern(pattern, id++);
      if (condition_set) {
        condition_sets.push_back(condition_set);
      }
    }

    blocklist_matcher_->AddConditionSets(condition_sets);
  }

  // Build whitelist matcher
  if (prefs_->HasPrefPath(prefs::kProfileURLAllowlist)) {
    const base::Value::List& whitelist =
        prefs_->GetList(prefs::kProfileURLAllowlist);

    url_matcher::URLMatcherConditionSet::Vector condition_sets;
    base::MatcherStringPattern::ID id = 10000;  // Offset IDs

    for (const auto& pattern_value : whitelist) {
      if (!pattern_value.is_string()) {
        continue;
      }

      const std::string& pattern = pattern_value.GetString();
      auto condition_set = CreateConditionSetFromPattern(pattern, id++);
      if (condition_set) {
        condition_sets.push_back(condition_set);
      }
    }

    whitelist_matcher_->AddConditionSets(condition_sets);
  }

  DVLOG(1) << "[Profile URL Blocker] Rebuilt URL matchers";
}

scoped_refptr<url_matcher::URLMatcherConditionSet>
ProfileURLBlockerNavigationThrottle::CreateConditionSetFromPattern(
    const std::string& pattern,
    base::MatcherStringPattern::ID id) {

  url_matcher::URLMatcherConditionFactory* factory =
      blocklist_matcher_->condition_factory();

  std::set<url_matcher::URLMatcherCondition> conditions;

  // Handle wildcard all pattern
  if (pattern == "*") {
    // Match any URL - use empty conditions with scheme filter
    std::vector<std::string> schemes = {"http", "https"};
    auto scheme_filter =
        std::make_unique<url_matcher::URLMatcherSchemeFilter>(schemes);

    return base::MakeRefCounted<url_matcher::URLMatcherConditionSet>(
        id, conditions, std::move(scheme_filter), nullptr, nullptr);
  }

  // Handle full URL with protocol (e.g., "https://example.com/path")
  if (pattern.find("://") != std::string::npos) {
    GURL pattern_url(pattern);
    if (!pattern_url.is_valid()) {
      LOG(WARNING) << "[Profile URL Blocker] Invalid URL pattern: " << pattern;
      return nullptr;
    }

    std::unique_ptr<url_matcher::URLMatcherSchemeFilter> scheme_filter;

    // Match scheme (convert string_view to string)
    if (pattern_url.has_scheme()) {
      scheme_filter =
          std::make_unique<url_matcher::URLMatcherSchemeFilter>(
              std::string(pattern_url.scheme()));
    }

    // Match host (convert string_view to string)
    if (pattern_url.has_host()) {
      conditions.insert(factory->CreateHostEqualsCondition(
          std::string(pattern_url.host())));
    }

    // Match path (convert string_view to string)
    if (pattern_url.has_path() && pattern_url.path() != "/") {
      conditions.insert(factory->CreatePathPrefixCondition(
          std::string(pattern_url.path())));
    }

    return base::MakeRefCounted<url_matcher::URLMatcherConditionSet>(
        id, conditions, std::move(scheme_filter), nullptr, nullptr);
  }

  // Handle subdomain wildcard (e.g., "*.example.com")
  if (base::StartsWith(pattern, "*.")) {
    std::string domain = pattern.substr(2);  // Remove "*."

    // Match URLs with subdomain but NOT the domain itself
    // E.g., "*.quangkai.com" matches "abc.quangkai.com" but NOT "quangkai.com"
    conditions.insert(factory->CreateHostSuffixCondition("." + domain));

    return base::MakeRefCounted<url_matcher::URLMatcherConditionSet>(
        id, conditions);
  }

  // Handle domain with path (e.g., "example.com/public")
  if (pattern.find("/") != std::string::npos) {
    size_t slash_pos = pattern.find("/");
    std::string domain = pattern.substr(0, slash_pos);
    std::string path = pattern.substr(slash_pos);

    // Match exact domain
    conditions.insert(factory->CreateHostEqualsCondition(domain));

    // Match path prefix
    conditions.insert(factory->CreatePathPrefixCondition(path));

    return base::MakeRefCounted<url_matcher::URLMatcherConditionSet>(
        id, conditions);
  }

  // Handle plain domain (e.g., "example.com")
  // Match exact domain OR with www. prefix
  conditions.insert(factory->CreateHostEqualsCondition(pattern));

  // Also match www. subdomain for convenience
  if (!base::StartsWith(pattern, "www.")) {
    conditions.insert(factory->CreateHostEqualsCondition("www." + pattern));
  }

  return base::MakeRefCounted<url_matcher::URLMatcherConditionSet>(
      id, conditions);
}

bool ProfileURLBlockerNavigationThrottle::IsURLBlocked(const GURL& url) {
  if (!blocklist_matcher_) {
    return false;
  }

  // Match URL against blocklist patterns
  std::set<base::MatcherStringPattern::ID> matches =
      blocklist_matcher_->MatchURL(url);

  if (!matches.empty()) {
    DVLOG(1) << "[Profile URL Blocker] URL matched blocklist: " << url.spec()
              << " (matched " << matches.size() << " patterns)";
    return true;
  }

  return false;
}

bool ProfileURLBlockerNavigationThrottle::IsURLWhitelisted(const GURL& url) {
  if (!whitelist_matcher_) {
    return false;
  }

  // Match URL against whitelist patterns
  std::set<base::MatcherStringPattern::ID> matches =
      whitelist_matcher_->MatchURL(url);

  if (!matches.empty()) {
    DVLOG(1) << "[Profile URL Blocker] URL matched whitelist: " << url.spec()
              << " (matched " << matches.size() << " patterns)";
    return true;
  }

  return false;
}

void ProfileURLBlockerNavigationThrottle::ShowBlockedNotification(
    const GURL& blocked_url) {
  DVLOG(1) << "[Profile URL Blocker] Blocked access to: " << blocked_url.spec();
}

bool ProfileURLBlockerNavigationThrottle::IsURLBlockedViaIPC(const GURL& url) {
  // Get singleton IPC client
  auto* ipc_client = policy_manager::PolicyIPCClient::GetInstance();

  if (!ipc_client || !ipc_client->IsConnected()) {
    // IPC not available, allow by default (soft fail)
    return false;
  }

  // Check URL against IPC server
  policy_manager::PolicyDecision decision =
      ipc_client->CheckURLSync(url.spec());

  if (!decision.allow) {
    DVLOG(1) << "[Profile URL Blocker] Blocked via IPC: " << url.spec()
              << " (reason: " << decision.reason << ")";
    return true;
  }

  return false;
}
