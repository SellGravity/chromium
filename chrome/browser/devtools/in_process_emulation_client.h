#ifndef CHROME_BROWSER_DEVTOOLS_IN_PROCESS_EMULATION_CLIENT_H_
#define CHROME_BROWSER_DEVTOOLS_IN_PROCESS_EMULATION_CLIENT_H_

#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "base/memory/scoped_refptr.h"

namespace content {
class DevToolsAgentHost;
class WebContents;
}  // namespace content

class InProcessEmulationClient
    : public content::WebContentsObserver,
      public content::DevToolsAgentHostClient,
      public content::WebContentsUserData<InProcessEmulationClient> {
 public:
  ~InProcessEmulationClient() override;

  InProcessEmulationClient(const InProcessEmulationClient&) = delete;
  InProcessEmulationClient& operator=(const InProcessEmulationClient&) = delete;

  // content::WebContentsObserver:
  void DidStartNavigation(
      content::NavigationHandle* navigation_handle) override;
  void PrimaryMainFrameWasResized(bool width_changed) override;

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;

 private:
  friend class content::WebContentsUserData<InProcessEmulationClient>;

  explicit InProcessEmulationClient(content::WebContents* web_contents);

  void ApplyEmulation();

  scoped_refptr<content::DevToolsAgentHost> host_;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

#endif  // CHROME_BROWSER_DEVTOOLS_IN_PROCESS_EMULATION_CLIENT_H_
