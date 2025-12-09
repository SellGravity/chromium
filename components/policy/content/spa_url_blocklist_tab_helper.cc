// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/policy/content/spa_url_blocklist_tab_helper.h"

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace policy {

namespace {
// Policy server endpoint
constexpr char kPolicyServerUrl[] = "http://127.0.0.1:8765/api/check_url";
}  // namespace

SpaUrlBlocklistTabHelper::SpaUrlBlocklistTabHelper(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<SpaUrlBlocklistTabHelper>(*web_contents) {
  // Initialization - no logging to avoid I/O overhead
}

SpaUrlBlocklistTabHelper::~SpaUrlBlocklistTabHelper() = default;

void SpaUrlBlocklistTabHelper::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  // 1. Only care about the main frame (address bar), ignore iframes
  if (!navigation_handle->IsInPrimaryMainFrame()) {
    return;
  }

  // 2. Only care about committed navigations (URL actually changed)
  if (!navigation_handle->HasCommitted()) {
    return;
  }

  // 3. CRITICAL: Do NOT skip IsSameDocument navigations!
  // YouTube uses same-document navigation (pushState) when switching videos.
  // If we add "if (navigation_handle->IsSameDocument()) return;" here,
  // the code will FAIL to catch YouTube video switches!

  const GURL& url = navigation_handle->GetURL();

  // 4. Skip non-HTTP URLs
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return;
  }

  // 5. Only check URLs that might be blocked (skip common safe domains)
  // This reduces unnecessary HTTP calls and improves performance
  const std::string host(url.host());
  
  // Skip policy check for known-safe Google/browser internal domains
  if (host == "www.google.com" || host == "google.com" ||
      host == "accounts.google.com" || host == "apis.google.com" ||
      host == "clients1.google.com" || host == "clients2.google.com" ||
      host.find(".gstatic.com") != std::string::npos ||
      host.find(".googleapis.com") != std::string::npos ||
      host == "chrome.google.com" || host == "update.googleapis.com") {
    return;
  }

  // 6. Rate limit: Skip if we checked this exact URL recently
  // (SPA often triggers multiple navigations for same URL)
  if (last_checked_url_ == url.spec()) {
    return;
  }
  last_checked_url_ = url.spec();

  // 7. Send to Python policy server (only for potentially blocked URLs)
  CheckUrlWithServer(url, GetProfileName());
}

std::string SpaUrlBlocklistTabHelper::GetProfileName() {
  // TODO: Get actual profile name from Profile object
  // For now, return "Default"
  return "Default";
}

void SpaUrlBlocklistTabHelper::CheckUrlWithServer(
    const GURL& url,
    const std::string& profile_name) {
  // Build JSON request body
  base::Value::Dict request_dict;
  request_dict.Set("url", url.spec());
  request_dict.Set("profile", profile_name);

  std::string request_body;
  base::JSONWriter::Write(request_dict, &request_body);

  // Create network request
  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(kPolicyServerUrl);
  resource_request->method = "POST";
  resource_request->headers.SetHeader("Content-Type", "application/json");

  // Traffic annotation required by Chromium network stack
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("gra_browser_policy_check", R"(
        semantics {
          sender: "GraBrowser Policy Check"
          description:
            "Checks if the current URL is blocked by the local policy server."
          trigger:
            "Navigation to a new URL or same-document navigation (SPA)."
          data:
            "The URL being navigated to and the profile name."
          destination: LOCAL
        }
        policy {
          cookies_allowed: NO
          setting:
            "This feature is controlled by the local policy server."
        })");

  // Create URL loader
  auto url_loader = network::SimpleURLLoader::Create(
      std::move(resource_request), traffic_annotation);

  url_loader->AttachStringForUpload(request_body, "application/json");

  // Set timeout (2 seconds)
  url_loader->SetTimeoutDuration(base::Seconds(2));

  // Get URL loader factory from browser context
  content::WebContents* contents = web_contents();
  if (!contents) {
    return;
  }

  content::BrowserContext* browser_context = contents->GetBrowserContext();
  if (!browser_context) {
    return;
  }

  auto* storage_partition = browser_context->GetDefaultStoragePartition();
  if (!storage_partition) {
    return;
  }

  // Keep raw pointer for the call, move ownership to callback
  network::SimpleURLLoader* loader_ptr = url_loader.get();

  // Send request
  loader_ptr->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
      storage_partition->GetURLLoaderFactoryForBrowserProcess().get(),
      base::BindOnce(&SpaUrlBlocklistTabHelper::OnServerResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(url_loader),
                     url));
}

void SpaUrlBlocklistTabHelper::OnServerResponse(
    std::unique_ptr<network::SimpleURLLoader> url_loader,
    const GURL& checked_url,
    std::unique_ptr<std::string> response_body) {
  if (!response_body) {
    // Server unreachable - silently allow (fail-open for performance)
    return;
  }

  // Parse JSON response
  auto json_result = base::JSONReader::Read(
      *response_body, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!json_result || !json_result->is_dict()) {
    // Invalid response - silently allow
    return;
  }

  const base::Value::Dict& response_dict = json_result->GetDict();

  // Check if URL is allowed
  std::optional<bool> allow = response_dict.FindBool("allow");
  if (!allow.has_value()) {
    // Missing field - silently allow
    return;
  }

  if (!allow.value()) {
    // URL is blocked!
    const std::string* reason = response_dict.FindString("reason");
    std::string block_reason = reason ? *reason : "Policy blocked";

    // Only log when actually blocking (rare event)
    DLOG(WARNING) << "[GraBrowser] BLOCKING: " << checked_url.spec();

    BlockPageWithJS(block_reason);
  }
  // No logging for allowed URLs - this is the common case
}

void SpaUrlBlocklistTabHelper::BlockPageWithJS(
    const std::string& blocked_reason) {
  content::WebContents* contents = web_contents();
  if (!contents) {
    return;
  }

  content::RenderFrameHost* main_frame = contents->GetPrimaryMainFrame();
  if (!main_frame) {
    return;
  }

  // Check if frame is live and ready for JS execution
  if (!main_frame->IsRenderFrameLive()) {
    LOG(WARNING) << "[GraBrowser] Frame not live, cannot inject blocked page";
    return;
  }

  // Escape the reason for safe JS injection
  std::string safe_reason = blocked_reason;
  base::ReplaceChars(safe_reason, "'", "\\'", &safe_reason);
  base::ReplaceChars(safe_reason, "\"", "\\\"", &safe_reason);

  // JavaScript to stop SPA and show blocked page
  std::string js_script =
      "(function() {"
      "  window.stop();"
      "  document.querySelectorAll('video, audio').forEach(function(el) {"
      "    el.pause();"
      "    el.src = '';"
      "    el.load();"
      "  });"
      "  document.documentElement.innerHTML = '"
      "    <html>"
      "    <head><title>Blocked - GraBrowser</title>"
      "    <style>"
      "      body {"
      "        margin: 0; padding: 0;"
      "        display: flex; justify-content: center; align-items: center;"
      "        min-height: 100vh;"
      "        background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);"
      "        font-family: -apple-system, BlinkMacSystemFont, Segoe UI, Roboto, sans-serif;"
      "      }"
      "      .container {"
      "        text-align: center; padding: 40px;"
      "        background: rgba(255,255,255,0.05);"
      "        border-radius: 20px;"
      "        border: 1px solid rgba(255,255,255,0.1);"
      "        max-width: 500px;"
      "      }"
      "      .icon { font-size: 80px; margin-bottom: 20px; }"
      "      h1 { color: #e94560; font-size: 28px; margin: 0 0 15px 0; }"
      "      p { color: #a0a0a0; font-size: 16px; line-height: 1.6; margin: 0 0 10px 0; }"
      "      .reason {"
      "        color: #ffd700; font-family: monospace;"
      "        background: rgba(255,215,0,0.1);"
      "        padding: 8px 16px; border-radius: 8px;"
      "        display: inline-block; margin-top: 15px;"
      "      }"
      "      .btn {"
      "        margin-top: 25px; padding: 12px 30px;"
      "        background: #e94560; color: white;"
      "        border: none; border-radius: 8px;"
      "        font-size: 16px; cursor: pointer;"
      "      }"
      "      .btn:hover { background: #ff6b6b; }"
      "    </style>"
      "    </head>"
      "    <body>"
      "      <div class=\"container\">"
      "        <div class=\"icon\">🛡️</div>"
      "        <h1>ACCESS BLOCKED</h1>"
      "        <p>This content has been blocked by your organization\\'s policy.</p>"
      "        <p>If you believe this is an error, please contact your administrator.</p>"
      "        <div class=\"reason\">" + safe_reason + "</div>"
      "        <br>"
      "        <button class=\"btn\" onclick=\"history.back()\">Go Back</button>"
      "      </div>"
      "    </body>"
      "    </html>"
      "  ';"
      "})();";

  // Use isolated world ID for extension-like JS injection (avoids CanExecuteJavaScript check)
  // World ID 1 is reserved for Chrome extensions, we use a custom ID
  constexpr int32_t kGraBrowserIsolatedWorldId = 999;
  main_frame->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(js_script),
      base::NullCallback(),
      kGraBrowserIsolatedWorldId);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(SpaUrlBlocklistTabHelper);

}  // namespace policy
