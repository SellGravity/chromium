// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/permission_sync/permission_cache_manager_factory.h"

#include <memory>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/permission_sync/permission_cache_manager.h"
#include "chrome/browser/permission_sync/permission_startup_loader.h"
#include "chrome/browser/permission_sync/permission_sync_client.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_selections.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/storage_partition.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "services/cert_verifier/public/mojom/cert_verifier_service_factory.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace permission_sync {

namespace {

// Extract profile_id from the profile path.
// Profile::GetPath() returns the full profile directory path, e.g.:
//   C:\gravity-browser\profiles\<UUID>\chromium\Default
// We need to go up 2 levels (past "Default" and "chromium") to get <UUID>.
std::string ExtractProfileId(Profile* profile) {
  base::FilePath profile_path = profile->GetPath();

  // profile_path = .../UUID/chromium/Default
  // DirName()    = .../UUID/chromium
  // DirName()    = .../UUID
  // BaseName()   = UUID
  base::FilePath uuid_dir = profile_path.DirName().DirName();
  std::string profile_id = uuid_dir.BaseName().MaybeAsASCII();

  if (profile_id.empty()) {
    // Fallback: use the full path as identifier.
    profile_id = profile_path.MaybeAsASCII();
    LOG(WARNING) << "[PermissionSync] Could not extract profile_id from path: "
                 << profile_path.value()
                 << ". Using full path as fallback.";
  }

  LOG(INFO) << "[PermissionSync] Extracted profile_id: " << profile_id
            << " from path: " << profile_path.value();

  return profile_id;
}

// Called via PostDelayedTask AFTER profile init stack fully unwinds.
// Creates WebSocket connection in the background.
// If CacheManager was already destroyed (profile shutdown), weak ptr
// will be null and this is a no-op.
void InitializeSyncClient(
    base::WeakPtr<PermissionCacheManager> weak_cache_manager) {
  if (!weak_cache_manager) {
    LOG(WARNING) << "[PermissionSync] CacheManager destroyed before "
                 << "standalone NC could be created (profile shutdown).";
    return;
  }

  LOG(INFO) << "[PermissionSync] Creating standalone NC as reconnect fallback.";

  // Create a STANDALONE NetworkContext for reconnects.
  // This NC is NOT tied to profile's StoragePartition lifecycle,
  // so reconnects won't be affected by profile NC recycling.
  mojo::Remote<network::mojom::NetworkContext> standalone_nc;
  auto params = network::mojom::NetworkContextParams::New();
  params->cert_verifier_params = content::GetCertVerifierParams(
      cert_verifier::mojom::CertVerifierCreationParams::New());
  // DIRECT proxy: bypass all proxy for localhost WebSocket.
  params->initial_proxy_config =
      net::ProxyConfigWithAnnotation::CreateDirect();
  content::CreateNetworkContextInNetworkService(
      standalone_nc.BindNewPipeAndPassReceiver(), std::move(params));

  LOG(INFO) << "[PermissionSync] Standalone NC created. Bound="
            << standalone_nc.is_bound()
            << " Connected=" << standalone_nc.is_connected();

  // Transfer ownership to SyncClient — it keeps the NC alive.
  // This also updates the resolver to prefer standalone NC for reconnects.
  weak_cache_manager->GetSyncClient()->SetStandaloneNetworkContext(
      std::move(standalone_nc));

  // Do NOT call Connect() here — initial connection was already made
  // with profile's NC in BuildServiceInstanceForBrowserContext().
  // Standalone NC will be used automatically on reconnect via resolver.
}

}  // namespace

// static
PermissionCacheManager* PermissionCacheManagerFactory::GetForProfile(
    Profile* profile) {
  return static_cast<PermissionCacheManager*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
PermissionCacheManagerFactory* PermissionCacheManagerFactory::GetInstance() {
  static base::NoDestructor<PermissionCacheManagerFactory> instance;
  return instance.get();
}

PermissionCacheManagerFactory::PermissionCacheManagerFactory()
    : ProfileKeyedServiceFactory(
          "PermissionCacheManager",
          // Not available in incognito/OTR profiles.
          ProfileSelections::BuildForRegularProfile()) {}

PermissionCacheManagerFactory::~PermissionCacheManagerFactory() = default;

std::unique_ptr<KeyedService>
PermissionCacheManagerFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);

  // Extract profile ID from the profile path.
  std::string profile_id = ExtractProfileId(profile);

  LOG(INFO) << "[PermissionSync] Creating CacheManager for profile: "
            << profile_id;

  // 1. Create the CacheManager (owns the cache + state machine).
  auto cache_manager = std::make_unique<PermissionCacheManager>();

  // 2. Create the SyncClient (owns WebSocket connection).
  auto sync_client = std::make_unique<PermissionSyncClient>(
      cache_manager.get(), profile_id);

  // 3. Transfer SyncClient ownership to CacheManager.
  cache_manager->SetSyncClient(std::move(sync_client));

  // 3b. NC resolver (fallback) — only used if standalone NC fails.
  //     Standalone NC is created in InitializeSyncClient() callback.
  cache_manager->GetSyncClient()->SetNetworkContextResolver(
      base::BindRepeating(
          [](Profile* p) -> network::mojom::NetworkContext* {
            return p->GetDefaultStoragePartition()->GetNetworkContext();
          },
          base::Unretained(profile)));

  // ═══════════════════════════════════════════════════════════════
  // STEP 1: Load startup rules IMMEDIATELY (no network needed)
  // ═══════════════════════════════════════════════════════════════
  auto load_result = PermissionStartupLoader::Load(cache_manager.get());

  if (load_result.success) {
    LOG(INFO) << "[Factory] Startup rules loaded from "
              << load_result.source << ": "
              << load_result.rule_count << " rules, v"
              << load_result.version
              << ". Browser operational immediately.";
  } else {
    LOG(WARNING) << "[Factory] No startup rules: "
                 << load_result.error
                 << ". Browser will block until WebSocket sync.";
  }

  // ═══════════════════════════════════════════════════════════════
  // STEP 2: Connect WebSocket IMMEDIATELY using profile's NC
  // ═══════════════════════════════════════════════════════════════
  // Use profile's NetworkContext for the initial connection — it's
  // properly initialized and connects fast. Standalone NC is created
  // in the background (2s) as fallback for reconnects only.
  cache_manager->GetSyncClient()->Connect(
      profile->GetDefaultStoragePartition()->GetNetworkContext());

  // ═══════════════════════════════════════════════════════════════
  // STEP 3: Create standalone NC in background (fallback for reconnects)
  // ═══════════════════════════════════════════════════════════════
  // If profile NC gets recycled (1001), standalone NC takes over.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&InitializeSyncClient,
                     cache_manager->GetWeakPtr()),
      base::Seconds(2));  // 2s: Network Service ready, standalone NC stable

  return cache_manager;
}

bool PermissionCacheManagerFactory::ServiceIsCreatedWithBrowserContext()
    const {
  // Return false to defer factory creation from profile init to first use.
  // This unblocks the UI thread during profile initialization, allowing
  // the network stack (QUIC handshakes, DNS) to initialize faster when
  // multiple profiles launch simultaneously.
  //
  // The factory will be created on the first call to GetForProfile(),
  // which happens during the first navigation/subresource request.
  // Startup rules are still loaded immediately upon creation, so
  // permission enforcement is active from the first request onward.
  return false;
}

}  // namespace permission_sync
