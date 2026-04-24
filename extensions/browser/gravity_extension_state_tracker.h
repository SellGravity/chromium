// Copyright 2026 Gravity Browser
// Tracks which command-line extensions have already had their first
// chrome.runtime.onInstalled event dispatched, preventing repeated
// "install" events on every browser launch.

#ifndef EXTENSIONS_BROWSER_GRAVITY_EXTENSION_STATE_TRACKER_H_
#define EXTENSIONS_BROWSER_GRAVITY_EXTENSION_STATE_TRACKER_H_

#include <string>

#include "base/files/file_path.h"
#include "base/values.h"
#include "extensions/common/extension_id.h"

namespace content {
class BrowserContext;
}

namespace extensions {

class GravityExtensionStateTracker {
 public:
  // Returns the tracker for the given context.
  static GravityExtensionStateTracker* GetForBrowserContext(
      content::BrowserContext* context);

  explicit GravityExtensionStateTracker(const base::FilePath& profile_path);
  ~GravityExtensionStateTracker();

  GravityExtensionStateTracker(const GravityExtensionStateTracker&) = delete;
  GravityExtensionStateTracker& operator=(const GravityExtensionStateTracker&) = delete;

  // Returns true if this extension has been installed before with the same
  // version. If not seen (or different version), marks it and returns false.
  bool HasBeenInstalledAndMark(const ExtensionId& extension_id,
                               const std::string& version);

 private:
  void LoadState();
  void SaveState();

  base::FilePath state_file_path_;
  base::Value::Dict state_;
  bool loaded_ = false;
};

}  // namespace extensions

#endif  // EXTENSIONS_BROWSER_GRAVITY_EXTENSION_STATE_TRACKER_H_
