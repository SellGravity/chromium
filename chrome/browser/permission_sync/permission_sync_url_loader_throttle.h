// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_URL_LOADER_THROTTLE_H_
#define CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_URL_LOADER_THROTTLE_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/permission_sync/permission_cache_manager.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"

namespace permission_sync {

class PermissionCacheManager;
struct PermissionDecision;

// ============================================================================
// PermissionSyncURLLoaderThrottle (FRS Section 3.4.2)
// ============================================================================
// Intercepts subresource requests (SCRIPT, IMAGE, XHR, FETCH, STYLESHEET,
// MEDIA, WEBSOCKET) and evaluates them against the PermissionCacheManager.
//
// IMPORTANT — Thread safety:
//   URLLoaderThrottle methods (WillStartRequest, WillRedirectRequest) run on
//   the IO/network thread, NOT the UI thread. PermissionCacheManager lives on
//   the UI thread.
//
//   - Atomic reads (GetConnectionState) are safe cross-thread.
//   - EvaluateFromCache() grabs snapshot under lock — safe cross-thread.
//   - For the deferred path, callbacks must be posted back to the IO thread
//     using BindPostTaskToCurrentDefault to avoid cross-thread delegate
//     access violations.
//
class PermissionSyncURLLoaderThrottle : public blink::URLLoaderThrottle {
 public:
  explicit PermissionSyncURLLoaderThrottle(
      PermissionCacheManager* cache_manager);

  PermissionSyncURLLoaderThrottle(
      const PermissionSyncURLLoaderThrottle&) = delete;
  PermissionSyncURLLoaderThrottle& operator=(
      const PermissionSyncURLLoaderThrottle&) = delete;

  ~PermissionSyncURLLoaderThrottle() override;

  // blink::URLLoaderThrottle:
  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override;
  void WillRedirectRequest(
      net::RedirectInfo* redirect_info,
      const network::mojom::URLResponseHead& response_head,
      bool* defer,
      std::vector<std::string>* to_be_removed_request_headers,
      net::HttpRequestHeaders* modified_request_headers,
      net::HttpRequestHeaders* modified_cors_exempt_request_headers) override;
  const char* NameForLoggingWillStartRequest() override;

 private:
  // Shared evaluation logic for both initial request and redirects.
  void CheckURL(const GURL& url,
                ResourceType resource_type,
                bool* defer);

  // Callback from EvaluateRequestAsync when deferred.
  // MUST run on the thread that called WillStartRequest (IO thread).
  void OnPermissionDecision(PermissionDecision decision);

  const raw_ptr<PermissionCacheManager> cache_manager_;

  base::WeakPtrFactory<PermissionSyncURLLoaderThrottle> weak_factory_{this};
};

}  // namespace permission_sync

#endif  // CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_SYNC_URL_LOADER_THROTTLE_H_
