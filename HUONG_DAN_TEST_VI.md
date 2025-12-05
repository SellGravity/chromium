# Hướng Dẫn Kiểm Tra Fingerprint Noise - Testing Guide

## Tổng Quan (Overview)

Tài liệu này hướng dẫn cách kiểm tra (test) tính năng bảo vệ fingerprinting đã được triển khai trong Chromium. Tính năng này bao gồm việc thêm noise (nhiễu) vào audio, canvas, rects và font fingerprinting.

## Các Bước Kiểm Tra Nhanh (Quick Testing Steps)

### 1. Chạy Unit Tests (Kiểm Tra Đơn Vị)

Đã tạo file test cho SessionNoiseCache tại:
- `third_party/blink/renderer/platform/privacy_budget/session_noise_cache_test.cc`

Để chạy tests:

```bash
# Bước 1: Build tests
autoninja -C out/Default blink_platform_unittests

# Bước 2: Chạy SessionNoiseCache tests
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.*"
```

Hoặc sử dụng script tự động:

```bash
# Chạy script kiểm tra
./test_fingerprint_noise.sh
```

### 2. Kiểm Tra Trong Browser (Browser Testing)

#### A. Test Audio Fingerprinting

```bash
# Build Chrome
autoninja -C out/Default chrome

# Chạy với audio noise
out/Default/chrome --audio-noise --user-data-dir=/tmp/test_audio
```

Mở trang test: https://webbrowsertools.com/audiocontext-fingerprint/

**Kết quả mong đợi:**
- Mỗi lần refresh trang sẽ có fingerprint khác nhau
- Các giá trị audio sẽ bị randomize

#### B. Test Canvas Fingerprinting

```bash
# Chạy với canvas noise và seed cố định
out/Default/chrome --canvas-noise --canvas-seed=12345678 --user-data-dir=/tmp/test_canvas
```

Mở trang test: https://browserleaks.com/canvas

**Kết quả mong đợi:**
- Canvas fingerprint sẽ nhất quán trong cùng một profile (cùng seed)
- Seed khác nhau sẽ cho fingerprint khác nhau

#### C. Test Rect Measurements

```bash
# Chạy với rects noise
out/Default/chrome --rects-noise --user-data-dir=/tmp/test_rects
```

**Kết quả mong đợi:**
- Các giá trị rect sẽ thay đổi nhẹ (±0.0001px)
- Không ảnh hưởng đến hiển thị

#### D. Test Font Measurements

```bash
# Chạy với fonts noise
out/Default/chrome --fonts-noise --user-data-dir=/tmp/test_fonts
```

Mở trang test: https://browserleaks.com/fonts

**Kết quả mong đợi:**
- Font measurements sẽ có noise nhỏ
- Không có lag khi test nhiều fonts
- Detection vẫn hoạt động bình thường

#### E. Test Tất Cả Cùng Lúc

```bash
# Chạy với tất cả protections
out/Default/chrome \
  --audio-noise \
  --canvas-noise \
  --rects-noise \
  --fonts-noise \
  --user-data-dir=/tmp/test_all
```

## Các File Đã Tạo (Created Files)

### 1. Unit Test File
- **File:** `third_party/blink/renderer/platform/privacy_budget/session_noise_cache_test.cc`
- **Mục đích:** Test SessionNoiseCache class
- **Số tests:** 12 test cases
- **Nội dung test:**
  - Tạo noise cơ bản
  - Caching mechanism
  - Custom range noise
  - Edge cases (zero, negative, very large/small values)

### 2. Browser Test File
- **File:** `chrome/browser/privacy_budget/session_noise_cache_browsertest.cc`
- **Mục đích:** Test end-to-end trong browser
- **Nội dung test:**
  - Canvas seed được set đúng
  - Không crash khi có noise
  - Canvas operations hoạt động bình thường
  - Rect measurements hoạt động bình thường

### 3. Testing Documentation
- **File:** `TESTING_FINGERPRINT_NOISE.md`
- **Mục đích:** Hướng dẫn chi tiết về testing
- **Nội dung:**
  - Build instructions
  - Test commands
  - Manual testing guide
  - Troubleshooting
  - Performance testing

### 4. Test Script
- **File:** `test_fingerprint_noise.sh`
- **Mục đích:** Script tự động build và test
- **Chức năng:**
  - Check build directory
  - Build unit tests
  - Run SessionNoiseCache tests
  - Hiển thị kết quả và hướng dẫn tiếp theo

## Kiểm Tra Kết Quả (Verify Results)

### Unit Tests
Chạy và xem kết quả:

```bash
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.*"
```

**Output mong đợi:**
```
[==========] Running 12 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 12 tests from SessionNoiseCacheTest
[ RUN      ] SessionNoiseCacheTest.BasicNoiseGeneration
[       OK ] SessionNoiseCacheTest.BasicNoiseGeneration (0 ms)
[ RUN      ] SessionNoiseCacheTest.NoiseIsCached
[       OK ] SessionNoiseCacheTest.NoiseIsCached (0 ms)
...
[==========] 12 tests from 1 test suite ran. (XX ms total)
[  PASSED  ] 12 tests.
```

### Browser Tests
Chạy browser tests:

```bash
# Build browser tests
autoninja -C out/Default browser_tests

# Run session noise cache browser tests
out/Default/browser_tests --gtest_filter="SessionNoiseCacheBrowserTest.*"
```

## Troubleshooting (Xử Lý Lỗi)

### Lỗi Build

**Lỗi:** `gn not found`
- **Giải pháp:** Cài đặt depot_tools

**Lỗi:** `ninja not found`
- **Giải pháp:** Đảm bảo depot_tools trong PATH

**Lỗi:** Build fails với errors
- **Giải pháp:** Xem build log để tìm lỗi cụ thể
- Kiểm tra BUILD.gn files đã được update đúng chưa

### Lỗi Test

**Lỗi:** Tests fail
- **Giải pháp:** Chạy từng test riêng lẻ để debug:
  ```bash
  out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.BasicNoiseGeneration"
  ```

**Lỗi:** Browser không hiển thị noise effect
- **Giải pháp:** 
  - Verify command-line flags
  - Check build includes your changes
  - Use `chrome://version` để verify build

## Tài Liệu Liên Quan (Related Documentation)

1. **FINGERPRINTING_PROTECTION_SOLUTION.md**
   - Giải thích chi tiết về implementation
   - Audio, rect, font fingerprinting protection

2. **UNICODE_GLYPHS_NOISE_IMPLEMENTATION.md**
   - Unicode glyphs fingerprint noise
   - Font whitelist system

3. **TESTING_FINGERPRINT_NOISE.md**
   - Hướng dẫn testing chi tiết bằng tiếng Anh
   - Performance testing
   - Debugging guide

## Kiểm Tra Thủ Công (Manual Testing)

### Tạo Test HTML Page

Tạo file `test_noise.html`:

```html
<!DOCTYPE html>
<html>
<head><title>Noise Test</title></head>
<body>
    <h1>Fingerprint Noise Test</h1>
    
    <h2>Canvas Test</h2>
    <button onclick="testCanvas()">Test</button>
    <pre id="canvas-result"></pre>
    
    <h2>Rect Test</h2>
    <button onclick="testRect()">Test</button>
    <pre id="rect-result"></pre>
    
    <script>
    function testCanvas() {
        const results = [];
        for (let i = 0; i < 3; i++) {
            const canvas = document.createElement('canvas');
            canvas.width = 100;
            canvas.height = 100;
            const ctx = canvas.getContext('2d');
            ctx.fillStyle = '#FF0000';
            ctx.fillRect(0, 0, 100, 100);
            const url = canvas.toDataURL();
            results.push(`Test ${i+1}: ${url.substring(0, 50)}...`);
        }
        document.getElementById('canvas-result').textContent = results.join('\n');
    }
    
    function testRect() {
        const results = [];
        const div = document.createElement('div');
        div.style.cssText = 'position:absolute;left:100px;top:50px;width:200px;height:100px';
        document.body.appendChild(div);
        
        for (let i = 0; i < 5; i++) {
            const rect = div.getBoundingClientRect();
            results.push(`Test ${i+1}: x=${rect.x.toFixed(6)}, width=${rect.width.toFixed(6)}`);
            div.style.left = '100.001px';
            div.offsetHeight;
            div.style.left = '100px';
            div.offsetHeight;
        }
        
        document.body.removeChild(div);
        document.getElementById('rect-result').textContent = results.join('\n');
    }
    </script>
</body>
</html>
```

Mở với Chrome:
```bash
out/Default/chrome --canvas-noise --rects-noise --user-data-dir=/tmp/test file:///path/to/test_noise.html
```

## Checklist Kiểm Tra

- [ ] Unit tests pass cho SessionNoiseCache
- [ ] Browser tests pass
- [ ] Audio fingerprinting test hiển thị randomized values
- [ ] Canvas fingerprinting hoạt động với seed
- [ ] Rect measurements có noise
- [ ] Font measurements có noise
- [ ] Không có performance regression
- [ ] Không có visible artifacts
- [ ] Profile seed persistence hoạt động

## Liên Hệ và Hỗ Trợ

Nếu gặp vấn đề:
1. Đọc kỹ error messages
2. Check TESTING_FINGERPRINT_NOISE.md
3. Review implementation documentation
4. Kiểm tra git log để xem changes gần đây

---

**Tóm tắt:**
- Đã tạo 12 unit tests cho SessionNoiseCache
- Đã tạo browser tests để verify end-to-end
- Đã tạo script tự động để build và test
- Đã tạo tài liệu hướng dẫn chi tiết

**Để bắt đầu test ngay:**
```bash
./test_fingerprint_noise.sh
```
