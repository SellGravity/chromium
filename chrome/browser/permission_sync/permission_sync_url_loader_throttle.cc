// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/permission_sync/permission_sync_url_loader_throttle.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/bind_post_task.h"
#include "chrome/browser/permission_sync/permission_cache_manager.h"
#include "chrome/browser/permission_sync/permission_sync_utils.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"
#include "url/gurl.h"

namespace permission_sync {

namespace {

// Fix #8: Map RequestDestination to ResourceType with explicit handling.
// kEmpty (XHR/Fetch) mapped explicitly. Unknown types → OTHER.
ResourceType MapDestination(network::mojom::RequestDestination destination) {
  switch (destination) {
    case network::mojom::RequestDestination::kDocument:
      return ResourceType::MAIN_FRAME;
    case network::mojom::RequestDestination::kIframe:
      return ResourceType::SUB_FRAME;
    case network::mojom::RequestDestination::kScript:
    case network::mojom::RequestDestination::kWorker:
    case network::mojom::RequestDestination::kSharedWorker:
    case network::mojom::RequestDestination::kServiceWorker:
      return ResourceType::SCRIPT;
    case network::mojom::RequestDestination::kImage:
      return ResourceType::IMAGE;
    case network::mojom::RequestDestination::kStyle:
      return ResourceType::STYLESHEET;
    case network::mojom::RequestDestination::kVideo:
    case network::mojom::RequestDestination::kAudio:
    case network::mojom::RequestDestination::kTrack:
      return ResourceType::MEDIA;
    case network::mojom::RequestDestination::kEmpty:
      // XHR and Fetch use kEmpty destination.
      return ResourceType::XHR;
    default:
      // kFont, kEmbed, kObject, kReport, kJson, kDictionary,
      // kWebBundle, and any future additions.
      return ResourceType::OTHER;
  }
}

}  // namespace

PermissionSyncURLLoaderThrottle::PermissionSyncURLLoaderThrottle(
    PermissionCacheManager* cache_manager)
    : cache_manager_(cache_manager) {
  DCHECK(cache_manager_);
}

PermissionSyncURLLoaderThrottle::~PermissionSyncURLLoaderThrottle() = default;

void PermissionSyncURLLoaderThrottle::WillStartRequest(
    network::ResourceRequest* request,
    bool* defer) {
  CheckURL(request->url, MapDestination(request->destination), defer);
}

// Fix #5: Also check redirect URLs. CDN redirects can change domain
// (e.g., domain A → domain B). If A is allowed but B is blocked,
// the request must be cancelled after redirect.
void PermissionSyncURLLoaderThrottle::WillRedirectRequest(
    net::RedirectInfo* redirect_info,
    const network::mojom::URLResponseHead& response_head,
    bool* defer,
    std::vector<std::string>* to_be_removed_request_headers,
    net::HttpRequestHeaders* modified_request_headers,
    net::HttpRequestHeaders* modified_cors_exempt_request_headers) {
  // Use kEmpty for destination since redirect_info doesn't carry it.
  // The original destination was already checked in WillStartRequest.
  CheckURL(redirect_info->new_url,
           ResourceType::XHR, defer);
}

const char* PermissionSyncURLLoaderThrottle::NameForLoggingWillStartRequest() {
  return "PermissionSyncURLLoaderThrottle";
}

void PermissionSyncURLLoaderThrottle::CheckURL(
    const GURL& url,
    ResourceType resource_type,
    bool* defer) {
  // Bypass internal schemes.
  if (ShouldBypassScheme(url)) {
    return;
  }

  std::string domain = ExtractDomainFromURL(url);

  ConnectionState state = cache_manager_->GetConnectionState();

  // ── FAST PATH: SYNCHRONIZED → evaluate from cache ──
  if (state == ConnectionState::SYNCHRONIZED) {
    PermissionDecision decision =
        cache_manager_->EvaluateFromCache(domain, resource_type);

    if (decision.action == PermissionAction::BLOCK) {
      DVLOG(1) << "[PermissionSync/Loader] BLOCKED: " << url.spec()
               << " | Reason: " << decision.reason;
      delegate_->CancelWithError(net::ERR_BLOCKED_BY_ADMINISTRATOR,
                                 "PermissionSync: " + decision.reason);
      return;
    }

    DVLOG(2) << "[PermissionSync/Loader] ALLOWED: " << url.spec();
    return;
  }

  // ── HAS RULES (startup or previously synced) → evaluate from cache ──
  if (cache_manager_->HasStartupRules() ||
      cache_manager_->HasEverSynchronized()) {
    PermissionDecision decision =
        cache_manager_->EvaluateFromCache(domain, resource_type);
    if (decision.action == PermissionAction::BLOCK) {
      DVLOG(1) << "[PermissionSync/Loader] BLOCKED (cached, state="
               << static_cast<int>(state) << "): " << url.spec();
      delegate_->CancelWithError(net::ERR_BLOCKED_BY_ADMINISTRATOR,
                                 "PermissionSync: " + decision.reason);
    }
    return;
  }

  // ── NO RULES AT ALL → fail-open (ALLOW everything) ──
  // No startup rules, never synced. Don't block subresource loads.
  // WS will sync rules in background; once synced, rules will apply.
  DVLOG(1) << "[PermissionSync/Loader] ALLOWED (no rules, fail-open): "
           << url.spec();
}

void PermissionSyncURLLoaderThrottle::OnPermissionDecision(
    PermissionDecision decision) {
  if (decision.action == PermissionAction::BLOCK) {
    DVLOG(1) << "[PermissionSync/Loader] BLOCKED (deferred)"
             << " | Reason: " << decision.reason;
    delegate_->CancelWithError(net::ERR_BLOCKED_BY_ADMINISTRATOR,
                               "PermissionSync: " + decision.reason);
  } else {
    DVLOG(1) << "[PermissionSync/Loader] ALLOWED (deferred)";
    delegate_->Resume();
  }
}

}  // namespace permission_sync
