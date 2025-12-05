# Testing Guide: Fingerprint Noise Implementation

## Overview

This guide explains how to test the fingerprinting noise protection features implemented in Chromium. The implementation includes noise generation for audio, canvas, rects, and font fingerprinting.

## Prerequisites

### Build Environment Setup

1. **Ensure you have the Chromium build tools installed:**
   ```bash
   # Follow the official Chromium build instructions:
   # https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md
   ```

2. **Verify you're in the Chromium source directory:**
   ```bash
   cd /home/runner/work/chromium/chromium
   ```

## Building the Code

### 1. Configure the Build (if not already done)

```bash
# For a debug build with testing enabled
gn gen out/Default --args='is_debug=true is_component_build=true'

# For a release build
gn gen out/Release --args='is_debug=false'
```

### 2. Build the Tests

```bash
# Build the platform unit tests
autoninja -C out/Default blink_platform_unittests

# Or build everything (this takes much longer)
autoninja -C out/Default chrome
```

### 3. Build Specific Components

If you're only testing specific changes:

```bash
# Build just the platform library
autoninja -C out/Default third_party/blink/renderer/platform

# Build the browser tests
autoninja -C out/Default browser_tests
```

## Running Unit Tests

### Run All Platform Unit Tests

```bash
# Run all blink platform unit tests
out/Default/blink_platform_unittests
```

### Run Only Session Noise Cache Tests

```bash
# Run tests with a filter
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.*"
```

### Run Specific Test

```bash
# Run a single test case
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.BasicNoiseGeneration"
```

### Verbose Output

```bash
# Run with verbose output
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.*" --gtest_print_time=1 -v
```

## Testing the Implementation End-to-End

### 1. Build Chrome Browser

```bash
autoninja -C out/Default chrome
```

### 2. Test Audio Fingerprinting Protection

```bash
# Run Chrome with audio noise enabled
out/Default/chrome --audio-noise --user-data-dir=/tmp/test_audio "https://webbrowsertools.com/audiocontext-fingerprint/"
```

**Expected Results:**
- Each page refresh should show different audio fingerprints
- All four audio tests should show randomized values
- Multiple calls to the same AudioContext should return different values

### 3. Test Rect Measurements Protection

```bash
# Run Chrome with rects noise enabled
out/Default/chrome --rects-noise --user-data-dir=/tmp/test_rects "https://browserleaks.com/rects"
```

**Expected Results:**
- Rect measurements should vary slightly (within ±0.0001px)
- Values should be different on each access

### 4. Test Font Measurements Protection

```bash
# Run Chrome with fonts noise enabled
out/Default/chrome --fonts-noise --user-data-dir=/tmp/test_fonts "https://browserleaks.com/fonts"
```

**Expected Results:**
- Font measurements should vary slightly
- No UI lag during font detection
- Font detection still works but with added noise

### 5. Test Canvas Noise with Profile Seed

```bash
# Run Chrome with a specific canvas seed
out/Default/chrome --canvas-noise --canvas-seed=12345678 --user-data-dir=/tmp/test_canvas
```

**Expected Results:**
- Canvas fingerprint should be consistent within the same profile
- Different seed values should produce different fingerprints

### 6. Test Combined Protection

```bash
# Run Chrome with all protections enabled
out/Default/chrome \
  --audio-noise \
  --rects-noise \
  --fonts-noise \
  --canvas-noise \
  --user-data-dir=/tmp/test_all \
  "https://browserleaks.com/canvas"
```

## Manual Testing with Test Page

Create a test HTML file to verify noise generation:

```html
<!DOCTYPE html>
<html>
<head>
    <title>Fingerprint Noise Test</title>
</head>
<body>
    <h1>Fingerprint Noise Verification</h1>
    
    <h2>Audio Test</h2>
    <button onclick="testAudio()">Test Audio Fingerprint</button>
    <pre id="audio-result"></pre>
    
    <h2>Rect Test</h2>
    <button onclick="testRect()">Test Rect Fingerprint</button>
    <pre id="rect-result"></pre>
    
    <h2>Canvas Test</h2>
    <button onclick="testCanvas()">Test Canvas Fingerprint</button>
    <pre id="canvas-result"></pre>
    
    <script>
    async function testAudio() {
        const results = [];
        
        // Test AudioBuffer (should change each time with noise)
        for (let i = 0; i < 3; i++) {
            const ctx = new AudioContext();
            const buffer = ctx.createBuffer(1, 128, 44100);
            const data = buffer.getChannelData(0);
            const sum = data.slice(0, 10).reduce((a,b) => a+b, 0);
            results.push(`Call ${i+1}: ${sum}`);
            await ctx.close();
        }
        
        document.getElementById('audio-result').textContent = results.join('\n');
        
        // Check if values are different
        const unique = new Set(results).size;
        if (unique === 3) {
            document.getElementById('audio-result').textContent += '\n\n✅ PASS: Values are randomized';
        } else {
            document.getElementById('audio-result').textContent += '\n\n❌ FAIL: Values are identical';
        }
    }
    
    function testRect() {
        const results = [];
        const div = document.createElement('div');
        div.style.cssText = 'position:absolute;left:100px;top:50px;width:200px;height:100px';
        document.body.appendChild(div);
        
        // Test multiple calls
        for (let i = 0; i < 5; i++) {
            const rect = div.getBoundingClientRect();
            results.push(`Call ${i+1}: x=${rect.x.toFixed(6)}, width=${rect.width.toFixed(6)}`);
            
            // Force recreation
            div.style.left = '100.001px';
            div.offsetHeight;
            div.style.left = '100px';
            div.offsetHeight;
        }
        
        document.body.removeChild(div);
        document.getElementById('rect-result').textContent = results.join('\n');
    }
    
    function testCanvas() {
        const results = [];
        
        for (let i = 0; i < 3; i++) {
            const canvas = document.createElement('canvas');
            canvas.width = 200;
            canvas.height = 50;
            const ctx = canvas.getContext('2d');
            
            ctx.fillStyle = '#FF0000';
            ctx.fillRect(0, 0, 200, 50);
            ctx.fillStyle = '#00FF00';
            ctx.fillRect(10, 10, 50, 30);
            
            const dataURL = canvas.toDataURL();
            const hash = dataURL.substring(0, 100);
            results.push(`Call ${i+1}: ${hash}`);
        }
        
        document.getElementById('canvas-result').textContent = results.join('\n');
    }
    </script>
</body>
</html>
```

Save this as `test_fingerprint.html` and open it with:

```bash
out/Default/chrome --audio-noise --rects-noise --canvas-noise --user-data-dir=/tmp/test file:///path/to/test_fingerprint.html
```

## Debugging Test Failures

### Check Build Logs

```bash
# View build output for errors
autoninja -C out/Default blink_platform_unittests 2>&1 | tee build.log
```

### Run Tests with GDB

```bash
# Debug a failing test
gdb --args out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.BasicNoiseGeneration"
```

### Check for Memory Leaks

```bash
# Run tests with AddressSanitizer
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.*"
```

## Continuous Integration Testing

### Run Pre-Submit Checks

```bash
# Run git cl presubmit checks
git cl presubmit
```

### Run All Tests

```bash
# Run the comprehensive test suite
python3 testing/xvfb.py out/Default/blink_platform_unittests
```

## Troubleshooting

### Common Issues

1. **Build fails with "gn not found"**
   - Install depot_tools: https://chromium.googlesource.com/chromium/tools/depot_tools.git

2. **Build fails with "ninja not found"**
   - depot_tools includes ninja, ensure depot_tools is in your PATH

3. **Tests fail with "command line not initialized"**
   - This is expected in some test environments
   - The tests use base::CommandLine::ForCurrentProcess() which should be initialized by the test framework

4. **Browser doesn't show noise effect**
   - Verify you're using the correct command-line flags
   - Check that the build includes your changes
   - Use `chrome://version` to verify the build

5. **Noise is too noticeable**
   - The noise ranges are carefully calibrated
   - Audio: ±0.000001 (imperceptible)
   - Rects: ±0.0001px (sub-pixel)
   - Fonts: ±0.0001px (sub-pixel)

## Performance Testing

### Measure Test Performance

```bash
# Run tests with timing information
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.*" --gtest_print_time=1
```

### Profile the Code

```bash
# Build with profiling enabled
gn gen out/Profile --args='is_debug=false enable_profiling=true'
autoninja -C out/Profile chrome

# Run with profiler
out/Profile/chrome --audio-noise --no-sandbox --single-process --profile-directory=/tmp/profile
```

## Verification Checklist

- [ ] Unit tests pass: `SessionNoiseCacheTest.*`
- [ ] Audio fingerprinting test shows randomized values
- [ ] Rect measurements show slight variations
- [ ] Font measurements show slight variations
- [ ] Canvas fingerprinting works with seed
- [ ] No performance regression on fingerprinting sites
- [ ] No visible artifacts or distortions
- [ ] Profile seed persistence works correctly
- [ ] All command-line flags work as expected

## Next Steps

1. **Add more comprehensive tests** for other fingerprinting vectors
2. **Create browser tests** to verify end-to-end functionality
3. **Add performance benchmarks** to ensure no regression
4. **Document any new command-line flags** in chrome://flags

## Additional Resources

- [Chromium Testing Documentation](https://chromium.googlesource.com/chromium/src/+/main/docs/testing/)
- [GTest Documentation](https://github.com/google/googletest/blob/main/docs/primer.md)
- [Chromium Build Instructions](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md)
- [Fingerprinting Protection Solution](FINGERPRINTING_PROTECTION_SOLUTION.md)
- [Unicode Glyphs Noise Implementation](UNICODE_GLYPHS_NOISE_IMPLEMENTATION.md)

## Contact

If you encounter issues or have questions:
1. Check the existing documentation
2. Review the test output carefully
3. Look for similar issues in the Chromium bug tracker
4. Ask in the appropriate Chromium development channels
