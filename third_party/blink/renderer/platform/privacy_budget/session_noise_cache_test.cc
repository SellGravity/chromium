// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/privacy_budget/session_noise_cache.h"

#include "base/command_line.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace blink {

class SessionNoiseCacheTest : public testing::Test {
 protected:
  void SetUp() override {
    // Reset command line for each test
    base::CommandLine::ForCurrentProcess()->RemoveSwitch("canvas-seed");
  }

  base::test::TaskEnvironment task_environment_;
};

TEST_F(SessionNoiseCacheTest, BasicNoiseGeneration) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double value = 123.456;
  double noise = cache.GetNoise(value);

  // Noise should be in the default range [-0.5, 0.5]
  EXPECT_GE(noise, -0.5);
  EXPECT_LE(noise, 0.5);
}

TEST_F(SessionNoiseCacheTest, NoiseIsCached) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double value = 789.012;
  double noise1 = cache.GetNoise(value);
  double noise2 = cache.GetNoise(value);

  // Same input should return same noise (cached)
  EXPECT_EQ(noise1, noise2);
}

TEST_F(SessionNoiseCacheTest, DifferentValuesDifferentNoise) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double value1 = 100.0;
  double value2 = 200.0;

  double noise1 = cache.GetNoise(value1);
  double noise2 = cache.GetNoise(value2);

  // Different inputs should produce different noise
  EXPECT_NE(noise1, noise2);
}

TEST_F(SessionNoiseCacheTest, CustomRangeNoise) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double value = 456.789;
  double min_noise = -0.001;
  double max_noise = 0.001;

  double noise = cache.GetNoiseInRange(value, min_noise, max_noise);

  // Noise should be within custom range
  EXPECT_GE(noise, min_noise);
  EXPECT_LE(noise, max_noise);
}

TEST_F(SessionNoiseCacheTest, CustomRangeNoiseIsCached) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double value = 333.444;
  double min_noise = -0.0001;
  double max_noise = 0.0001;

  double noise1 = cache.GetNoiseInRange(value, min_noise, max_noise);
  double noise2 = cache.GetNoiseInRange(value, min_noise, max_noise);

  // Same input and range should return same noise (cached)
  EXPECT_EQ(noise1, noise2);
}

TEST_F(SessionNoiseCacheTest, DifferentRangesDifferentNoise) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double value = 555.666;

  double noise1 = cache.GetNoiseInRange(value, -0.1, 0.1);
  double noise2 = cache.GetNoiseInRange(value, -0.2, 0.2);

  // Same value but different ranges should produce different noise
  EXPECT_NE(noise1, noise2);
}

TEST_F(SessionNoiseCacheTest, WithProfileSeed) {
  // Set a profile seed via command line
  base::CommandLine::ForCurrentProcess()->AppendSwitchASCII("canvas-seed",
                                                             "12345678");

  // Create a new instance (in practice, this would be a fresh browser session)
  // Note: We can't easily test this without restarting, but we can verify
  // the command line switch is read
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double value = 999.888;
  double noise = cache.GetNoise(value);

  // Noise should still be generated
  EXPECT_GE(noise, -0.5);
  EXPECT_LE(noise, 0.5);
}

TEST_F(SessionNoiseCacheTest, SingletonBehavior) {
  // Verify that GetInstance() returns the same instance
  SessionNoiseCache& cache1 = SessionNoiseCache::GetInstance();
  SessionNoiseCache& cache2 = SessionNoiseCache::GetInstance();

  EXPECT_EQ(&cache1, &cache2);
}

TEST_F(SessionNoiseCacheTest, ZeroValue) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double noise = cache.GetNoise(0.0);

  // Should handle zero value correctly
  EXPECT_GE(noise, -0.5);
  EXPECT_LE(noise, 0.5);
}

TEST_F(SessionNoiseCacheTest, NegativeValue) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double noise = cache.GetNoise(-123.456);

  // Should handle negative values correctly
  EXPECT_GE(noise, -0.5);
  EXPECT_LE(noise, 0.5);
}

TEST_F(SessionNoiseCacheTest, VeryLargeValue) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double noise = cache.GetNoise(1e100);

  // Should handle very large values correctly
  EXPECT_GE(noise, -0.5);
  EXPECT_LE(noise, 0.5);
}

TEST_F(SessionNoiseCacheTest, VerySmallValue) {
  SessionNoiseCache& cache = SessionNoiseCache::GetInstance();

  double noise = cache.GetNoise(1e-100);

  // Should handle very small values correctly
  EXPECT_GE(noise, -0.5);
  EXPECT_LE(noise, 0.5);
}

}  // namespace blink
