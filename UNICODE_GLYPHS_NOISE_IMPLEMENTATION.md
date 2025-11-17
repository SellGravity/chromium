# Unicode Glyphs Fingerprint Noise Implementation

## Overview

This document describes the implementation of Unicode Glyphs fingerprint noise feature for Chromium, which adds randomized noise to Unicode Glyphs data when the `--fonts-noise` CLI flag is enabled.

## Architecture

### Key Components

1. **Preference System** - Per-profile noise seed persistence
2. **Noise Generator** - Thread-safe singleton for noise generation
3. **Glyph Metrics Hooks** - Integration points in font rendering pipeline
4. **Character Coverage Spoofing** - Optional character hiding for fingerprint confusion

### Independence from Font Whitelist

**CRITICAL**: Unicode Glyphs noise operates **independently** from Font Metrics whitelist:
- `--fonts-whitelist` flag → Controls which fonts can be loaded (Font Metrics level)
- `--fonts-noise` flag → Controls noise injection into glyph metrics (Unicode Glyphs level)

## Implementation Details

### 1. Preference System

#### Files Modified:
- `chrome/common/pref_names.h` - Added `kUnicodeGlyphsNoiseSeed` constant
- `chrome/browser/profiles/profile.cc` - Registered `kUnicodeGlyphsNoiseSeed` pref

#### Seed Generation:
- Location: `chrome/browser/chrome_content_browser_client.cc` (lines 2759-2773)
- When `--fonts-noise` flag is present:
  1. Load existing seed from profile prefs
  2. If seed is 0, generate new random seed (MT19937-64)
  3. Save seed to profile prefs for persistence
  4. Pass seed to renderer via `--unicode-glyphs-seed` flag

### 2. Noise Generator

#### File Created:
- `third_party/blink/renderer/platform/fonts/unicode_glyphs_noise_generator.h`

#### Class: `UnicodeGlyphsNoiseGenerator`
- **Type**: Thread-safe singleton (header-only)
- **Purpose**: Generate consistent noise for glyph metrics
- **Seed Source**: `--unicode-glyphs-seed` command-line flag

#### Key Methods:
```cpp
// Check if noise is enabled
bool IsEnabled() const;

// Apply noise to glyph width (±1.5 pixels)
float GetNoisedWidth(float original_width, Glyph glyph);

// Apply noise to glyph bounds (±2.0 pixels per coordinate)
float GetNoisedBounds(float original_bounds, Glyph glyph, int coord_index);

// Determine if character should be hidden (~2% rate)
bool ShouldHideCharacter(uint32_t codepoint);
```

#### Noise Characteristics:
- **Width noise**: Random offset in range [-1.5, 1.5] pixels
- **Bounds noise**: Random offset in range [-2.0, 2.0] pixels per coordinate
- **Coverage spoofing**: 2% false negative rate (character hidden)
- **Caching**: Results cached per glyph for consistency within session
- **Determinism**: Same seed → same noise (reproducible per profile)

### 3. Glyph Metrics Hooks

#### File Modified:
- `third_party/blink/renderer/platform/fonts/skia/skia_text_metrics.cc`

#### Integration Points:

##### A. `SkFontGetGlyphWidthForHarfBuzz` (Single Glyph)
- **Location**: Line 33-57
- **Hook Point**: After width calculation, before conversion to HarfBuzz format
- **Noise Application**: `GetNoisedWidth(width, glyph)`

##### B. `SkFontGetGlyphWidthForHarfBuzz` (Batch)
- **Location**: Line 59-94
- **Hook Point**: After batch width calculation, before conversion
- **Noise Application**: Loop over all glyphs applying `GetNoisedWidth`

##### C. `SkFontGetGlyphExtentsForHarfBuzz`
- **Location**: Line 102-152
- **Hook Point**: After bounds calculation, before conversion
- **Noise Application**: Apply `GetNoisedBounds` to each coordinate (left, top, width, height)
- **Coordinate Index**: Used to generate different noise for each dimension

##### D. `SkFontGetWidthForGlyph`
- **Location**: Line 196-208
- **Hook Point**: After width calculation, before return
- **Noise Application**: `GetNoisedWidth(width, glyph)`

### 4. Character Coverage Spoofing

#### File Modified:
- `third_party/blink/renderer/platform/fonts/shaping/harfbuzz_face.cc`

#### Integration Point:
- **Function**: `HarfBuzzFace::HbGlyphForCharacter` (lines 402-413)
- **Hook Point**: Before glyph lookup
- **Logic**: If `ShouldHideCharacter(character)` returns true, return 0 (glyph not found)
- **Effect**: Randomly hide ~2% of characters to confuse fingerprinting scripts

## Usage

### Command-Line Flags

```bash
# Enable Unicode Glyphs noise only
chrome.exe --fonts-noise

# Enable with Font Metrics whitelist (independent features)
chrome.exe --fonts-noise --fonts-whitelist="Arial,Verdana"

# Empty whitelist still allows Unicode Glyphs noise
chrome.exe --fonts-noise --fonts-whitelist=""
```

### Flag Interactions

| Flags | Font Metrics Behavior | Unicode Glyphs Noise |
|-------|----------------------|---------------------|
| `--fonts-noise` | Default 33 fonts | Enabled |
| `--fonts-noise --fonts-whitelist=""` | Only default 33 fonts | Enabled |
| `--fonts-noise --fonts-whitelist="A,B"` | Default 33 + A,B | Enabled |
| No flags | All fonts allowed | Disabled |

### Profile Persistence

- **Location**: `<profile_directory>/Preferences`
- **Key**: `"fingerprinting.unicode_glyphs_noise_seed"`
- **Type**: uint64
- **Lifetime**: Persistent across browser sessions per profile

Example Preferences JSON:
```json
{
  "fingerprinting": {
    "canvas_noise_seed": 1234567890,
    "audio_noise_seed": 9876543210,
    "rects_noise_seed": 5555555555,
    "unicode_glyphs_noise_seed": 7777777777
  }
}
```

## Testing Recommendations

### Unit Tests

1. **Noise Generation**:
   - Test deterministic noise with same seed
   - Verify noise range constraints (width ±1.5px, bounds ±2.0px)
   - Check cache consistency within session

2. **Character Coverage**:
   - Verify ~2% character hiding rate
   - Test deterministic hiding with same seed
   - Ensure hiding is consistent for same character

3. **Integration**:
   - Test glyph width queries return noised values
   - Test glyph bounds queries return noised values
   - Verify no noise when `--fonts-noise` is disabled

### Fingerprinting Tests

1. **Effectiveness**:
   - Run fingerprinting scripts (e.g., FingerprintJS)
   - Verify different profiles produce different results
   - Confirm same profile produces consistent results within session

2. **Realism**:
   - Visual inspection: noise should be imperceptible
   - Layout verification: text rendering remains correct
   - Performance: no significant slowdown

### Performance Benchmarks

- **Cache Hit Rate**: Should be >95% for typical web pages
- **Memory Usage**: ~20-30MB worst case (large cache)
- **Latency**: <0.1ms per glyph query (with caching)

## File Changes Summary

### Files Modified:
1. `chrome/common/pref_names.h` - Added pref constant
2. `chrome/browser/profiles/profile.cc` - Registered pref
3. `chrome/browser/chrome_content_browser_client.cc` - Seed generation/passing
4. `third_party/blink/renderer/platform/fonts/skia/skia_text_metrics.cc` - Glyph metrics hooks
5. `third_party/blink/renderer/platform/fonts/shaping/harfbuzz_face.cc` - Character coverage hook

### Files Created:
1. `third_party/blink/renderer/platform/fonts/unicode_glyphs_noise_generator.h` - Noise generator class

### Total Lines Changed:
- Added: ~250 lines
- Modified: ~40 lines

## Known Limitations

1. **Noise Range**: Fixed ranges (±1.5px width, ±2.0px bounds) may need tuning based on testing
2. **Coverage Rate**: 2% character hiding rate is conservative; may need adjustment
3. **Cache Size**: Unbounded cache could grow large on long sessions (future: add eviction policy)
4. **Platform Differences**: Noise is consistent across platforms, but baseline metrics may differ

## Future Enhancements

1. **Adaptive Noise**: Vary noise intensity based on glyph size
2. **Cache Eviction**: Implement LRU cache with size limits
3. **Configuration**: Allow noise range customization via flags
4. **Telemetry**: Add metrics for cache performance and effectiveness

## Security Considerations

- **Seed Security**: Seed is stored in plaintext in Preferences file (low risk - not cryptographic)
- **Determinism**: Same seed = same noise (good for consistency, potential fingerprint vector if seed leaked)
- **Side Channels**: Noise timing is deterministic (no timing side channels)

## Maintenance Notes

- When updating Skia text metrics code, ensure noise hooks remain intact
- If HarfBuzz integration changes, verify character coverage spoofing still works
- Profile migration: seed will be preserved automatically (no special handling needed)

## References

- Related Implementation: `session_noise_cache.h` (canvas/audio/rects noise)
- Font Whitelist: `font_cache.cc` (`IsWhitelistedFont` function)
- CLI Flags: `chrome/browser/about_flags.cc` (for user-facing flag registration)

---

**Implementation Date**: 2024
**Author**: Claude Code
**Status**: Complete
