// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_POLICY_CONTENT_SPA_URL_BLOCKLIST_TAB_HELPER_H_
#define COMPONENTS_POLICY_CONTENT_SPA_URL_BLOCKLIST_TAB_HELPER_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "services/network/public/cpp/simple_url_loader.h"

class PolicyBlocklistService;

namespace policy {

// GraBrowser: TabHelper that observes URL changes for SPA (Single Page Apps)
// like YouTube, Facebook, Twitter that use History API (pushState/replaceState)
// to change URLs without full page reload.
//
// CRITICAL: This helper intentionally does NOT skip IsSameDocument navigations
// because YouTube uses same-document navigation when switching videos.
//
// Key features:
// - Catches same-document navigations (YouTube video switches)
// - Calls external Python policy server via HTTP
// - JavaScript injection to forcefully stop SPA rendering
class SpaUrlBlocklistTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<SpaUrlBlocklistTabHelper> {
 public:
  ~SpaUrlBlocklistTabHelper() override;

  SpaUrlBlocklistTabHelper(const SpaUrlBlocklistTabHelper&) = delete;
  SpaUrlBlocklistTabHelper& operator=(const SpaUrlBlocklistTabHelper&) = delete;

  // content::WebContentsObserver:
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;

 private:
  friend class content::WebContentsUserData<SpaUrlBlocklistTabHelper>;

  explicit SpaUrlBlocklistTabHelper(content::WebContents* web_contents);

  // Send URL to Python policy server for checking
  void CheckUrlWithServer(const GURL& url, const std::string& profile_name);

  // Callback when server responds
  void OnServerResponse(std::unique_ptr<network::SimpleURLLoader> url_loader,
                        const GURL& checked_url,
                        std::unique_ptr<std::string> response_body);

  // Inject JavaScript to block page and stop all media
  void BlockPageWithJS(const std::string& blocked_reason);

  // Get current profile name
  std::string GetProfileName();

  // Rate limiting: track last checked URL to avoid duplicate checks
  std::string last_checked_url_;

  // Factory for weak pointers (must be last member)
  base::WeakPtrFactory<SpaUrlBlocklistTabHelper> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace policy

#endif  // COMPONENTS_POLICY_CONTENT_SPA_URL_BLOCKLIST_TAB_HELPER_H_
