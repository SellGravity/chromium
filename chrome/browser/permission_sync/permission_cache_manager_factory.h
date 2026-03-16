// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_CACHE_MANAGER_FACTORY_H_
#define CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_CACHE_MANAGER_FACTORY_H_

#include "chrome/browser/profiles/profile_keyed_service_factory.h"

namespace base {
template <typename T>
class NoDestructor;
}

namespace content {
class BrowserContext;
}

namespace permission_sync {

class PermissionCacheManager;

// ============================================================================
// PermissionCacheManagerFactory
// ============================================================================
// Singleton factory that creates and manages PermissionCacheManager instances
// per browser profile. Each profile gets exactly one PermissionCacheManager
// which owns a PermissionSyncClient for WebSocket communication.
//
// Lifecycle:
//   - Created lazily on first access for each profile.
//   - Destroyed when the profile is shut down.
//   - Incognito profiles do NOT get a CacheManager (returns nullptr).
//
// Usage:
//   auto* manager = PermissionCacheManagerFactory::GetForProfile(profile);
//   if (manager) { ... }
//
class PermissionCacheManagerFactory : public ProfileKeyedServiceFactory {
 public:
  // Returns the PermissionCacheManager for the given profile, creating it
  // on first access. Returns nullptr for incognito/OTR profiles.
  static PermissionCacheManager* GetForProfile(Profile* profile);

  // Returns the factory singleton.
  static PermissionCacheManagerFactory* GetInstance();

  PermissionCacheManagerFactory(const PermissionCacheManagerFactory&) = delete;
  PermissionCacheManagerFactory& operator=(
      const PermissionCacheManagerFactory&) = delete;

 private:
  friend base::NoDestructor<PermissionCacheManagerFactory>;

  PermissionCacheManagerFactory();
  ~PermissionCacheManagerFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;

  // Create service eagerly during profile init (not lazily).
  // This ensures our ProfileObserver is registered BEFORE
  // NotifyProfileInitializationComplete fires.
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace permission_sync

#endif  // CHROME_BROWSER_PERMISSION_SYNC_PERMISSION_CACHE_MANAGER_FACTORY_H_
