// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_UTILS_H_
#define CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_UTILS_H_

#include <string>

#include "url/gurl.h"

namespace permission_sync {

// Returns true if the URL scheme should bypass permission checks.
// Shared by NavigationThrottle and URLLoaderThrottle.
inline bool ShouldBypassScheme(const GURL& url) {
  return url.SchemeIs("chrome") || url.SchemeIs("chrome-extension") ||
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
