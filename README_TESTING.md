# Testing Guide - Fingerprint Noise Implementation
# Hướng Dẫn Kiểm Tra - Fingerprinting Noise Implementation

[English](#english) | [Tiếng Việt](#tieng-viet)

---

## English

### 🎯 What Was Done

This PR implements comprehensive testing infrastructure for the fingerprint noise protection features in Chromium. The implementation answers your question: **"Bây giờ tôi sẽ test như thế nào?"** (How do I test now?)

### 📦 Files Created

1. **Unit Tests** (`third_party/blink/renderer/platform/privacy_budget/session_noise_cache_test.cc`)
   - 12 test cases for SessionNoiseCache class
   - Tests noise generation, caching, edge cases

2. **Browser Tests** (`chrome/browser/privacy_budget/session_noise_cache_browsertest.cc`)
   - 4 end-to-end tests
   - Verifies canvas, rect functionality

3. **Documentation**
   - `TESTING_FINGERPRINT_NOISE.md` - Comprehensive English guide
   - `HUONG_DAN_TEST_VI.md` - Complete Vietnamese guide
   - `TESTING_SUMMARY.md` - Quick reference

4. **Automation**
   - `test_fingerprint_noise.sh` - Automated test script

### 🚀 Quick Start

#### Option 1: Run Automated Script (Recommended)

```bash
./test_fingerprint_noise.sh
```

This script will:
- ✅ Check build environment
- ✅ Build unit tests
- ✅ Run all SessionNoiseCache tests
- ✅ Show results and next steps

#### Option 2: Manual Testing

**Step 1: Build Unit Tests**
```bash
autoninja -C out/Default blink_platform_unittests
```

**Step 2: Run Tests**
```bash
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.*"
```

**Expected Output:**
```
[==========] Running 12 tests from 1 test suite.
[----------] 12 tests from SessionNoiseCacheTest
[ RUN      ] SessionNoiseCacheTest.BasicNoiseGeneration
[       OK ] SessionNoiseCacheTest.BasicNoiseGeneration
...
[  PASSED  ] 12 tests.
```

### 🌐 Browser Testing

**Build Chrome:**
```bash
autoninja -C out/Default chrome
```

**Test Audio Fingerprinting:**
```bash
out/Default/chrome --audio-noise --user-data-dir=/tmp/test_audio \
  "https://webbrowsertools.com/audiocontext-fingerprint/"
```

**Test Canvas Fingerprinting:**
```bash
out/Default/chrome --canvas-noise --canvas-seed=12345678 \
  --user-data-dir=/tmp/test_canvas "https://browserleaks.com/canvas"
```

**Test All Protections:**
```bash
out/Default/chrome \
  --audio-noise \
  --canvas-noise \
  --rects-noise \
  --fonts-noise \
  --user-data-dir=/tmp/test_all
```

### 📊 Test Coverage

- ✅ **12 Unit Tests** - SessionNoiseCache functionality
- ✅ **4 Browser Tests** - End-to-end integration
- ✅ **Manual Tests** - Real-world fingerprinting websites
- ✅ **Documentation** - Comprehensive guides in English and Vietnamese

### 📚 Documentation Files

- **`TESTING_SUMMARY.md`** - Start here for quick overview
- **`TESTING_FINGERPRINT_NOISE.md`** - Detailed English guide
- **`HUONG_DAN_TEST_VI.md`** - Detailed Vietnamese guide
- **`test_fingerprint_noise.sh`** - Automated test script

### 🔍 What to Verify

1. **All unit tests pass** ✅
2. **Browser tests pass** ✅
3. **No build errors** ✅
4. **Fingerprinting sites show randomized values** ✅
5. **No performance regression** ✅

### 📝 Next Steps

1. Run `./test_fingerprint_noise.sh` to verify tests pass
2. Test in browser with fingerprinting websites
3. Review documentation for detailed instructions
4. Report any issues found

---

## Tiếng Việt

### 🎯 Những Gì Đã Làm

PR này triển khai cơ sở hạ tầng kiểm tra toàn diện cho tính năng bảo vệ fingerprinting trong Chromium. Đây là câu trả lời cho câu hỏi của bạn: **"Bây giờ tôi sẽ test như thế nào?"**

### 📦 Các File Đã Tạo

1. **Unit Tests** (`third_party/blink/renderer/platform/privacy_budget/session_noise_cache_test.cc`)
   - 12 test cases cho class SessionNoiseCache
   - Test tạo noise, caching, edge cases

2. **Browser Tests** (`chrome/browser/privacy_budget/session_noise_cache_browsertest.cc`)
   - 4 tests end-to-end
   - Verify canvas, rect functionality

3. **Tài Liệu**
   - `TESTING_FINGERPRINT_NOISE.md` - Hướng dẫn tiếng Anh đầy đủ
   - `HUONG_DAN_TEST_VI.md` - Hướng dẫn tiếng Việt đầy đủ
   - `TESTING_SUMMARY.md` - Tham khảo nhanh

4. **Tự Động Hóa**
   - `test_fingerprint_noise.sh` - Script test tự động

### 🚀 Bắt Đầu Nhanh

#### Cách 1: Chạy Script Tự Động (Khuyến Nghị)

```bash
./test_fingerprint_noise.sh
```

Script này sẽ:
- ✅ Kiểm tra môi trường build
- ✅ Build unit tests
- ✅ Chạy tất cả SessionNoiseCache tests
- ✅ Hiển thị kết quả và hướng dẫn tiếp theo

#### Cách 2: Test Thủ Công

**Bước 1: Build Unit Tests**
```bash
autoninja -C out/Default blink_platform_unittests
```

**Bước 2: Chạy Tests**
```bash
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.*"
```

**Kết Quả Mong Đợi:**
```
[==========] Running 12 tests from 1 test suite.
[----------] 12 tests from SessionNoiseCacheTest
[ RUN      ] SessionNoiseCacheTest.BasicNoiseGeneration
[       OK ] SessionNoiseCacheTest.BasicNoiseGeneration
...
[  PASSED  ] 12 tests.
```

### 🌐 Test Trên Browser

**Build Chrome:**
```bash
autoninja -C out/Default chrome
```

**Test Audio Fingerprinting:**
```bash
out/Default/chrome --audio-noise --user-data-dir=/tmp/test_audio \
  "https://webbrowsertools.com/audiocontext-fingerprint/"
```

**Test Canvas Fingerprinting:**
```bash
out/Default/chrome --canvas-noise --canvas-seed=12345678 \
  --user-data-dir=/tmp/test_canvas "https://browserleaks.com/canvas"
```

**Test Tất Cả Protections:**
```bash
out/Default/chrome \
  --audio-noise \
  --canvas-noise \
  --rects-noise \
  --fonts-noise \
  --user-data-dir=/tmp/test_all
```

### 📊 Phạm Vi Test

- ✅ **12 Unit Tests** - Chức năng SessionNoiseCache
- ✅ **4 Browser Tests** - Tích hợp end-to-end
- ✅ **Manual Tests** - Website fingerprinting thực tế
- ✅ **Tài Liệu** - Hướng dẫn đầy đủ bằng tiếng Anh và tiếng Việt

### 📚 Các File Tài Liệu

- **`TESTING_SUMMARY.md`** - Bắt đầu tại đây để có cái nhìn tổng quan
- **`TESTING_FINGERPRINT_NOISE.md`** - Hướng dẫn chi tiết tiếng Anh
- **`HUONG_DAN_TEST_VI.md`** - Hướng dẫn chi tiết tiếng Việt
- **`test_fingerprint_noise.sh`** - Script test tự động

### 🔍 Những Gì Cần Verify

1. **Tất cả unit tests pass** ✅
2. **Browser tests pass** ✅
3. **Không có lỗi build** ✅
4. **Website fingerprinting hiển thị giá trị random** ✅
5. **Không có performance regression** ✅

### 📝 Bước Tiếp Theo

1. Chạy `./test_fingerprint_noise.sh` để verify tests pass
2. Test trên browser với các website fingerprinting
3. Xem tài liệu để có hướng dẫn chi tiết
4. Báo cáo bất kỳ vấn đề nào tìm thấy

---

## 🆘 Troubleshooting / Xử Lý Sự Cố

### Build Fails / Build Thất Bại

**Problem:** `gn not found`
**Solution:** Install depot_tools and add to PATH

**Problem:** `ninja not found`
**Solution:** depot_tools includes ninja, ensure it's in PATH

### Tests Fail / Tests Thất Bại

**Problem:** Tests don't pass
**Solution:** Run individual tests to debug:
```bash
out/Default/blink_platform_unittests --gtest_filter="SessionNoiseCacheTest.BasicNoiseGeneration"
```

### Browser Issues / Vấn Đề Browser

**Problem:** Noise not visible
**Solution:** 
- Verify command-line flags
- Check chrome://version
- Test on fingerprinting websites

---

## 📞 Support / Hỗ Trợ

For detailed instructions, see:
Để có hướng dẫn chi tiết, xem:

- **English:** `TESTING_FINGERPRINT_NOISE.md`
- **Vietnamese:** `HUONG_DAN_TEST_VI.md`
- **Quick Reference:** `TESTING_SUMMARY.md`

---

## ✅ Status / Trạng Thái

**All testing infrastructure is ready to use!**
**Tất cả cơ sở hạ tầng testing đã sẵn sàng sử dụng!**

**Start testing now with:**
**Bắt đầu test ngay với:**
```bash
./test_fingerprint_noise.sh
```
