// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/policy_manager/policy_preference_syncer.h"

#include <memory>

#include "base/values.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace policy_manager {

class PolicyPreferenceSyncerTest : public testing::Test {
 protected:
  void SetUp() override {
    // Register preferences
    prefs_.registry()->RegisterListPref(prefs::kProfileURLBlocklist);

    // Create syncer
    syncer_ = std::make_unique<PolicyPreferenceSyncer>(&prefs_);
  }

  void TearDown() override {
    syncer_.reset();
  }

  TestingPrefServiceSimple prefs_;
  std::unique_ptr<PolicyPreferenceSyncer> syncer_;
};

TEST_F(PolicyPreferenceSyncerTest, InitializeWithEmptyPipeName) {
  // Initialize with empty pipe name should not crash
  syncer_->Initialize("");
  EXPECT_FALSE(syncer_->IsActive());
}

TEST_F(PolicyPreferenceSyncerTest, OnPolicyUpdatedWithURLBlocklist) {
  // Create mock policy with URL blocklist
  base::Value::Dict policy;
  base::Value::List blocklist;
  blocklist.Append("facebook.com");
  blocklist.Append("twitter.com");
  blocklist.Append("*.reddit.com");
  policy.Set("URLBlocklist", std::move(blocklist));

  // Apply policy update
  syncer_->OnPolicyUpdated(policy);

  // Verify blocklist was synced to preferences
  const auto& pref_blocklist = prefs_.GetList(prefs::kProfileURLBlocklist);
  ASSERT_EQ(3u, pref_blocklist.size());
  EXPECT_EQ("facebook.com", pref_blocklist[0].GetString());
  EXPECT_EQ("twitter.com", pref_blocklist[1].GetString());
  EXPECT_EQ("*.reddit.com", pref_blocklist[2].GetString());
}

TEST_F(PolicyPreferenceSyncerTest, OnPolicyUpdatedWithEmptyBlocklist) {
  // Set initial blocklist
  base::Value::List initial_blocklist;
  initial_blocklist.Append("test.com");
  prefs_.SetList(prefs::kProfileURLBlocklist, std::move(initial_blocklist));

  // Update with empty policy
  base::Value::Dict empty_policy;
  syncer_->OnPolicyUpdated(empty_policy);

  // Blocklist should remain unchanged
  const auto& pref_blocklist = prefs_.GetList(prefs::kProfileURLBlocklist);
  EXPECT_EQ(1u, pref_blocklist.size());
}

TEST_F(PolicyPreferenceSyncerTest, OnPolicyUpdatedMultipleTimes) {
  // First update
  base::Value::Dict policy1;
  base::Value::List blocklist1;
  blocklist1.Append("site1.com");
  policy1.Set("URLBlocklist", std::move(blocklist1));
  syncer_->OnPolicyUpdated(policy1);

  const auto& pref_blocklist1 = prefs_.GetList(prefs::kProfileURLBlocklist);
  EXPECT_EQ(1u, pref_blocklist1.size());

  // Second update - should replace first
  base::Value::Dict policy2;
  base::Value::List blocklist2;
  blocklist2.Append("site2.com");
  blocklist2.Append("site3.com");
  policy2.Set("URLBlocklist", std::move(blocklist2));
  syncer_->OnPolicyUpdated(policy2);

  const auto& pref_blocklist2 = prefs_.GetList(prefs::kProfileURLBlocklist);
  ASSERT_EQ(2u, pref_blocklist2.size());
  EXPECT_EQ("site2.com", pref_blocklist2[0].GetString());
  EXPECT_EQ("site3.com", pref_blocklist2[1].GetString());
}

TEST_F(PolicyPreferenceSyncerTest, OnPolicyUpdatedWithInvalidData) {
  // Create policy with invalid blocklist (not a list)
  base::Value::Dict policy;
  policy.Set("URLBlocklist", "not-a-list");

  // Should not crash or modify preferences
  syncer_->OnPolicyUpdated(policy);

  const auto& pref_blocklist = prefs_.GetList(prefs::kProfileURLBlocklist);
  EXPECT_EQ(0u, pref_blocklist.size());
}

TEST_F(PolicyPreferenceSyncerTest, OnPolicyUpdatedWithMixedTypes) {
  // Create blocklist with mixed types
  base::Value::Dict policy;
  base::Value::List blocklist;
  blocklist.Append("valid-string.com");
  blocklist.Append(123);  // Invalid: number
  blocklist.Append("another-valid.com");
  blocklist.Append(true);  // Invalid: boolean
  policy.Set("URLBlocklist", std::move(blocklist));

  syncer_->OnPolicyUpdated(policy);

  // Only valid strings should be added
  const auto& pref_blocklist = prefs_.GetList(prefs::kProfileURLBlocklist);
  ASSERT_EQ(2u, pref_blocklist.size());
  EXPECT_EQ("valid-string.com", pref_blocklist[0].GetString());
  EXPECT_EQ("another-valid.com", pref_blocklist[1].GetString());
}

TEST_F(PolicyPreferenceSyncerTest, OnPolicyUpdatedWithFeatures) {
  // Create policy with Features
  base::Value::Dict policy;
  base::Value::Dict features;
  features.Set("EnableHistory", false);
  features.Set("EnableDownloads", true);
  policy.Set("Features", std::move(features));

  // Should not crash (features are logged but not implemented yet)
  syncer_->OnPolicyUpdated(policy);
}

TEST_F(PolicyPreferenceSyncerTest, OnPolicyUpdatedWithHomepage) {
  // Create policy with homepage
  base::Value::Dict policy;
  policy.Set("HomepageLocation", "https://company.com");

  // Should not crash (homepage is logged but not implemented yet)
  syncer_->OnPolicyUpdated(policy);
}

TEST_F(PolicyPreferenceSyncerTest, OnPolicyUpdatedWithAllFields) {
  // Create comprehensive policy
  base::Value::Dict policy;

  // Blocklist
  base::Value::List blocklist;
  blocklist.Append("blocked1.com");
  blocklist.Append("blocked2.com");
  policy.Set("URLBlocklist", std::move(blocklist));

  // Allowlist
  base::Value::List allowlist;
  allowlist.Append("allowed.com");
  policy.Set("URLAllowlist", std::move(allowlist));

  // Features
  base::Value::Dict features;
  features.Set("EnableHistory", true);
  policy.Set("Features", std::move(features));

  // Homepage
  policy.Set("HomepageLocation", "https://test.com");

  // Apply policy
  syncer_->OnPolicyUpdated(policy);

  // Verify blocklist
  const auto& pref_blocklist = prefs_.GetList(prefs::kProfileURLBlocklist);
  EXPECT_EQ(2u, pref_blocklist.size());
}

}  // namespace policy_manager
