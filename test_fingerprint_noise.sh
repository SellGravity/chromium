#!/bin/bash
# Copyright 2024 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Script to build and test the fingerprint noise implementation

set -e

echo "========================================"
echo "Fingerprint Noise Testing Script"
echo "========================================"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if we're in the chromium directory
if [ ! -f "BUILD.gn" ]; then
    echo -e "${RED}Error: Not in chromium root directory${NC}"
    echo "Please run this script from /home/runner/work/chromium/chromium"
    exit 1
fi

echo -e "${GREEN}✓ Verified we're in chromium root directory${NC}"
echo ""

# Configuration
BUILD_DIR="out/Default"
TEST_NAME="blink_platform_unittests"

echo "Build directory: $BUILD_DIR"
echo "Test target: $TEST_NAME"
echo ""

# Step 1: Check if build directory exists
echo "Step 1: Checking build configuration..."
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Build directory doesn't exist. Creating...${NC}"
    gn gen "$BUILD_DIR" --args='is_debug=true is_component_build=true'
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ Build directory created${NC}"
    else
        echo -e "${RED}✗ Failed to create build directory${NC}"
        exit 1
    fi
else
    echo -e "${GREEN}✓ Build directory exists${NC}"
fi
echo ""

# Step 2: Build the unit tests
echo "Step 2: Building unit tests..."
echo "This may take several minutes..."
autoninja -C "$BUILD_DIR" "$TEST_NAME"
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Build completed successfully${NC}"
else
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi
echo ""

# Step 3: Run the SessionNoiseCache tests
echo "Step 3: Running SessionNoiseCache unit tests..."
"$BUILD_DIR/$TEST_NAME" --gtest_filter="SessionNoiseCacheTest.*" --gtest_print_time=1
TEST_RESULT=$?
echo ""

if [ $TEST_RESULT -eq 0 ]; then
    echo -e "${GREEN}✓ All SessionNoiseCache tests passed!${NC}"
else
    echo -e "${RED}✗ Some tests failed${NC}"
    exit 1
fi
echo ""

# Step 4: Summary
echo "========================================"
echo "Test Summary"
echo "========================================"
echo ""
echo "Unit Tests:"
echo "  - SessionNoiseCacheTest: PASSED"
echo ""
echo -e "${GREEN}All tests completed successfully!${NC}"
echo ""

# Step 5: Instructions for browser testing
echo "========================================"
echo "Next Steps - Browser Testing"
echo "========================================"
echo ""
echo "To test the fingerprinting noise in the browser:"
echo ""
echo "1. Build Chrome:"
echo "   autoninja -C $BUILD_DIR chrome"
echo ""
echo "2. Test audio fingerprinting:"
echo "   $BUILD_DIR/chrome --audio-noise --user-data-dir=/tmp/test_audio"
echo ""
echo "3. Test canvas fingerprinting:"
echo "   $BUILD_DIR/chrome --canvas-noise --canvas-seed=12345678 --user-data-dir=/tmp/test_canvas"
echo ""
echo "4. Test rect measurements:"
echo "   $BUILD_DIR/chrome --rects-noise --user-data-dir=/tmp/test_rects"
echo ""
echo "5. Test font measurements:"
echo "   $BUILD_DIR/chrome --fonts-noise --user-data-dir=/tmp/test_fonts"
echo ""
echo "6. Test all protections together:"
echo "   $BUILD_DIR/chrome --audio-noise --canvas-noise --rects-noise --fonts-noise --user-data-dir=/tmp/test_all"
echo ""
echo "For detailed testing instructions, see: TESTING_FINGERPRINT_NOISE.md"
echo ""
