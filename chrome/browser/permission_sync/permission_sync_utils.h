// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_UTILS_H_
#define CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_UTILS_H_

#include <string>
#include <string_view>

#include "url/gurl.h"

namespace permission_sync {

// ============================================================================
// GraBrowser: Blocked chrome:// internal pages
// ============================================================================
// Default: ALL pages below are blocked on startup.
// Admin can whitelist specific pages via Permission Sync server.
// Server sends ALLOW rule for e.g. "chrome://settings" → browser allows it.
// No CLI flag needed — server controls everything.

inline bool IsBlockedChromeURL(const GURL& url) {
  if (!url.SchemeIs("chrome")) {
    return false;
  }

  std::string_view host = url.host();
  return host == "version" ||        // Leaks CLI flags, paths, versions
         host == "flags" ||          // Leaks enabled/disabled feature flags
         host == "chrome-urls" ||    // Lists ALL chrome:// pages
         host == "about" ||          // Alias that lists all pages
         host == "tracing" ||        // Debug/profiling tool
         host == "net-internals" ||  // Network debugging info
         host == "gpu" ||            // GPU hardware info
         host == "system" ||         // System information
         host == "sandbox" ||        // Sandbox configuration
         host == "policy" ||         // Applied policies
         host == "settings" ||       // Browser settings, proxy config
         host == "extensions" ||     // Installed extensions info
         host == "history" ||        // Browsing history
         host == "downloads" ||      // Download history
         host == "components";       // Internal components & versions
}

// Returns true if the URL scheme should bypass permission checks.
// Shared by NavigationThrottle and URLLoaderThrottle.
// NOTE: Blocked chrome:// URLs are NOT bypassed (they must be evaluated).
inline bool ShouldBypassScheme(const GURL& url) {
  // We no longer bypass the "chrome" scheme by default, because administrators
  // must be able to dynamically block internal pages (e.g., password manager)
  // via Permission Cache Manager.
  return url.SchemeIs("chrome-extension") ||
         url.SchemeIs("devtools") || url.SchemeIs("about") ||
         url.SchemeIs("data") || url.SchemeIs("blob") ||
         url.SchemeIs("chrome-untrusted") || url.SchemeIs("file") ||
         url.SchemeIs("chrome-search");
}

// Extract the host from a URL as std::string.
inline std::string ExtractDomainFromURL(const GURL& url) {
  return std::string(url.host());
}

}  // namespace permission_sync

#endif  // CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_UTILS_H_


