// Copyright 2026 Gravity Browser

#include "extensions/browser/gravity_extension_state_tracker.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/browser_context.h"

namespace extensions {

namespace {

const char kInstalledOnceKey[] = "installed_once";
const char kTimestampKey[] = "ts";
const char kVersionKey[] = "ver";
const char kStateFileName[] = "gravity_extension_state.json";

base::Lock& GetGlobalLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

std::map<base::FilePath, std::unique_ptr<GravityExtensionStateTracker>>&
GetTrackerMap() {
  static base::NoDestructor<
      std::map<base::FilePath, std::unique_ptr<GravityExtensionStateTracker>>>
      map;
  return *map;
}

}  // namespace

// static
GravityExtensionStateTracker* GravityExtensionStateTracker::GetForBrowserContext(
    content::BrowserContext* context) {
  base::FilePath profile_path = context->GetPath();

  base::AutoLock lock(GetGlobalLock());
  auto& map = GetTrackerMap();
  auto it = map.find(profile_path);
  if (it != map.end()) {
    return it->second.get();
  }

  auto tracker = std::make_unique<GravityExtensionStateTracker>(profile_path);
  auto* raw = tracker.get();
  map[profile_path] = std::move(tracker);
  return raw;
}

GravityExtensionStateTracker::GravityExtensionStateTracker(
    const base::FilePath& profile_path)
    : state_file_path_(profile_path.AppendASCII(kStateFileName)) {}

GravityExtensionStateTracker::~GravityExtensionStateTracker() = default;

bool GravityExtensionStateTracker::HasBeenInstalledAndMark(
    const ExtensionId& extension_id,
    const std::string& version) {
  if (!loaded_) {
    LoadState();
  }

  base::Value::Dict* installed = state_.FindDict(kInstalledOnceKey);
  if (!installed) {
    installed = &state_.Set(kInstalledOnceKey, base::Value::Dict())->GetDict();
  }

  // Check if already tracked with the same version.
  const base::Value::Dict* entry = installed->FindDict(extension_id);
  if (entry) {
    const std::string* tracked_version = entry->FindString(kVersionKey);
    if (tracked_version && *tracked_version == version) {
      LOG(INFO) << "[Gravity] Extension " << extension_id
                << " v" << version << " already installed — suppressing";
      return true;  // Same version seen before — skip onInstalled
    }
  }

  // First time or version changed — mark and let onInstalled fire.
  base::Value::Dict new_entry;
  new_entry.Set(kTimestampKey,
                static_cast<double>(base::Time::Now().InSecondsFSinceUnixEpoch()));
  new_entry.Set(kVersionKey, version);
  installed->Set(extension_id, std::move(new_entry));

  SaveState();  // Synchronous write — guaranteed to persist before browser exit

  LOG(INFO) << "[Gravity] Marked extension " << extension_id
            << " v" << version << " as installed";
  return false;  // Not seen before — fire onInstalled
}

void GravityExtensionStateTracker::LoadState() {
  loaded_ = true;
  std::string contents;
  if (!base::ReadFileToString(state_file_path_, &contents)) {
    return;  // File doesn't exist yet — first launch
  }

  auto parsed = base::JSONReader::ReadDict(contents,
                                            base::JSON_ALLOW_TRAILING_COMMAS);
  if (parsed.has_value()) {
    state_ = std::move(*parsed);
  } else {
    LOG(WARNING) << "[Gravity] Failed to parse extension state file, "
                 << "starting empty";
  }
}

void GravityExtensionStateTracker::SaveState() {
  std::string json;
  if (!base::JSONWriter::WriteWithOptions(
          state_, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json)) {
    LOG(ERROR) << "[Gravity] Failed to serialize extension state";
    return;
  }
  if (!base::WriteFile(state_file_path_, json)) {
    LOG(ERROR) << "[Gravity] Failed to write extension state file: "
               << state_file_path_;
  }
}

}  // namespace extensions
