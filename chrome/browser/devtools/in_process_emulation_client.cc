#include "chrome/browser/devtools/in_process_emulation_client.h"

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"

WEB_CONTENTS_USER_DATA_KEY_IMPL(InProcessEmulationClient);

InProcessEmulationClient::InProcessEmulationClient(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<InProcessEmulationClient>(*web_contents) {
  // We do not attach to DevToolsAgentHost here because WebContents is still
  // being constructed, and attaching here can cause crashes.
  // Instead, we attach right before the first navigation commits.
}

InProcessEmulationClient::~InProcessEmulationClient() {
  if (host_) {
    host_->DetachClient(this);
  }
}

void InProcessEmulationClient::DidStartNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInPrimaryMainFrame() ||
      navigation_handle->IsSameDocument()) {
    return;
  }

  // Do not emulate DevTools UI or Chrome internal pages.
  GURL url = navigation_handle->GetURL();
  if (url.SchemeIs("devtools") || url.SchemeIs("chrome")) {
    return;
  }

  // Attach the host if we haven't already.
  if (!host_) {
    host_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents());
    host_->AttachClient(this);
  }

  ApplyEmulation();
}

void InProcessEmulationClient::PrimaryMainFrameWasResized(bool width_changed) {
  ApplyEmulation();
}

void InProcessEmulationClient::DispatchProtocolMessage(
    content::DevToolsAgentHost* agent_host,
    base::span<const uint8_t> message) {
  // Ignore responses from the agent.
}

void InProcessEmulationClient::AgentHostClosed(
    content::DevToolsAgentHost* agent_host) {
  host_ = nullptr;
}

void InProcessEmulationClient::ApplyEmulation() {
  if (!host_) return;

  int width = 375;
  int height = 667;
  float dsr = 3.0f;
  bool mobile = true;

  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch("device-size")) {
    std::string size_str = command_line->GetSwitchValueASCII("device-size");
    std::vector<std::string> parts = base::SplitString(
        size_str, "x,", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (parts.size() == 2) {
      base::StringToInt(parts[0], &width);
      base::StringToInt(parts[1], &height);
    }
  }

  float scale = 1.0f;
  if (web_contents() && web_contents()->GetRenderWidgetHostView()) {
    gfx::Size view_size = web_contents()->GetRenderWidgetHostView()->GetVisibleViewportSize();
    if (view_size.width() > 0 && view_size.height() > 0 && width > 0 && height > 0) {
      float scale_w = static_cast<float>(view_size.width()) / width;
      float scale_h = static_cast<float>(view_size.height()) / height;
      scale = std::min(scale_w, scale_h);
    }
  }

  // 1. Emulation.setDeviceMetricsOverride
  std::string metrics_msg = base::StringPrintf(
      R"({"id":1001,"method":"Emulation.setDeviceMetricsOverride",)"
      R"("params":{"width":%d,"height":%d,"scale":%f,)"
      R"("screenWidth":%d,"screenHeight":%d,)"
      R"("dontSetVisibleSize":true,)"
      R"("deviceScaleFactor":%f,"mobile":%s}})",
      width, height, scale, width, height, dsr, mobile ? "true" : "false");
  host_->DispatchProtocolMessage(this, base::as_byte_span(metrics_msg));

  // 2. Emulation.setTouchEmulationEnabled
  std::string touch_msg = base::StringPrintf(
      R"({"id":1002,"method":"Emulation.setTouchEmulationEnabled",)"
      R"("params":{"enabled":true,"maxTouchPoints":5}})");
  host_->DispatchProtocolMessage(this, base::as_byte_span(touch_msg));

  // 3. Emulation.setUserAgentOverride
  std::string user_agent = "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1";
  if (command_line->HasSwitch("user-agent")) {
    user_agent = command_line->GetSwitchValueASCII("user-agent");
  }

  if (mobile && user_agent.find("Mobile Safari") == std::string::npos && user_agent.find("Safari/") != std::string::npos) {
    size_t safari_pos = user_agent.find("Safari/");
    user_agent.insert(safari_pos, "Mobile ");
  }

  std::string platform_name = "iPhone";
  std::string platform_os = "iOS";
  std::string platform_version = "16.6.0";
  std::string model = "iPhone";

  if (user_agent.find("Android") != std::string::npos) {
    platform_name = "Android";
    platform_os = "Android";
    size_t android_pos = user_agent.find("Android ");
    if (android_pos != std::string::npos) {
      size_t end_pos = user_agent.find(";", android_pos);
      if (end_pos != std::string::npos) {
        platform_version = user_agent.substr(android_pos + 8, end_pos - (android_pos + 8));
        size_t end_bracket = user_agent.find(")", end_pos);
        if (end_bracket != std::string::npos) {
          model = user_agent.substr(end_pos + 1, end_bracket - (end_pos + 1));
          // Trim leading space
          if (!model.empty() && model[0] == ' ') {
            model = model.substr(1);
          }
          // Strip " Build/..."
          size_t build_pos = model.find(" Build/");
          if (build_pos != std::string::npos) {
            model = model.substr(0, build_pos);
          }
        }
      }
    }
    if (platform_version.empty()) {
      platform_version = "13.0.0";
    } else if (platform_version.find('.') == std::string::npos) {
      platform_version += ".0.0";
    }
    if (model.empty()) model = "Android Device";
  }

  std::string ua_msg = base::StringPrintf(
      R"({"id":1003,"method":"Emulation.setUserAgentOverride",)"
      R"("params":{"userAgent":"%s",)"
      R"("acceptLanguage":"en-US,en;q=0.9",)"
      R"("platform":"%s",)"
      R"("userAgentMetadata":{)"
      R"("brands":[{"brand":"Chromium","version":"120"}],)"
      R"("fullVersionList":[{"brand":"Chromium","version":"120.0.0.0"}],)"
      R"("fullVersion":"120.0.0.0",)"
      R"("platform":"%s",)"
      R"("platformVersion":"%s",)"
      R"("architecture":"",)"
      R"("model":"%s",)"
      R"("mobile":true,)"
      R"("bitness":"",)"
      R"("wow64":false}}})",
      user_agent.c_str(), platform_name.c_str(), platform_os.c_str(),
      platform_version.c_str(), model.c_str());
  host_->DispatchProtocolMessage(this, base::as_byte_span(ua_msg));
}
