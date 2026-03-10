1. Features description
1.1  Canvas (--canvas-noise): 
  -  Mô tả: Kỹ thuật chống Canvas Fingerprinting sử dụng cơ chế Deterministic Noise (Nhiễu Đơn Định). Thay vì cộng nhiễu ngẫu nhiên mỗi lần (khiến fingerprint bị nhảy lung tung - unstable), hệ thống sử dụng SessionNoiseCache để đảm bảo cùng một giá trị pixel ở cùng một vị trí luôn nhận được cùng một giá trị nhiễu. Điều này tạo ra một "Fingerprint giả" nhưng ổn định (Consistent) trong suốt phiên làm việc, đánh lừa các script phát hiện spoofing.
  -  Luồng:
Nội dung này chỉ được hỗ trợ trong Lark Docs
  - Files Cần Sửa:
1. third_party/blink/renderer/platform/graphics/static_bitmap_image.cc -> Core Logic: Hàm CreateNoisedImage sử dụng SessionNoiseCache.
2. third_party/blink/renderer/modules/canvas/canvas2d/canvas_rendering_context_2d.cc -> Hook vào quy trình vẽ.
3. third_party/blink/renderer/modules/canvas/canvas2d/base_rendering_context_2d.cc -> Hook getImageData.
4.third_party/blink/renderer/modules/canvas/htmlcanvaselement.cc -> Hook toDataURL, toBlob
  - CanvasNoise Component:
    - third_party/blink/renderer/platform/graphics/
      - static_bitmap_image.cc (CORE LOGIC)
        - CreateNoisedImage(source, seed)
        - Impl: SessionNoiseCache::GetNoiseInRange
        - Key: pixel_value * 10 + channel_index
        - Logic: Deterministic Noise (Pixel giống nhau -> Nhiễu giống nhau).
      - third_party/blink/renderer/modules/canvas/canvas2d/
        - base_rendering_context_2d.cc
          - ImageData* getImageData():
            - GetStaticBitmapImage()
            - Call CreateNoisedImage() (Inject Noise)
            - Return Modified ImageData.
      - third_party/blink/renderer/modules/canvas/
        - html_canvas_element.cc
          - String toDataURL():
            - GetStaticBitmapImage()-> CreateNoisedImage()
            - PNGEncoder -> Base64 String.
          - void toBlob():
            - Same flow -> Blob.
      - modules/canvas/BUILD.gn
        - (Logic tích hợp thẳng vào Core Graphics, không cần file nguồn mới).
1.2  Fonts (--fonts-noise):
  - Mô tả: Kỹ thuật chống Font Fingerprinting đa lớp kết hợp Metrics Noise (làm lệch ascent/descent) và Font Substitution (tráo đổi font ngẫu nhiên theo Profile, VD: Arial -> Helvetica). Bản đồ thay thế được lưu vĩnh viễn vào Profile Prefs -> đảm bảo tính nhất quán (Consistent) -> làm sai lệch hoàn toàn kết quả đo đạc Font Enumeration của Tracker.
  - Luồng:
Nội dung này chỉ được hỗ trợ trong Lark Docs
  - File cần sửa:
1. chrome/browser/chrome_content_browser_client.cc -> Logic Browser: Sinh Map ngẫu nhiên, lưu Prefs, Inject flag.
2. chrome/browser/profiles/profile.cc -> Đăng ký Pref kFontSubstitutionMapping.
3. chrome/common/pref_names.h -> Định nghĩa Key Pref.
4. third_party/blink/renderer/core/css/font_noise.cc -> Core: Parse Map & tính toán Noise.
    5. third_party/blink/renderer/core/css/css_font_selector.cc -> Hook: Thực hiện tráo đổi Font (Substitution).
    6. third_party/blink/renderer/platform/fonts/font_cache.cc -> Hook: Thực hiện làm lệch Metrics.
  - FontNoise Component:
    - chrome/browser/
      - chrome_content_browser_client.cc
        - AppendExtraCommandLineSwitches():
          - Kiểm tra Prefs: kFontSubstitutionMapping.
          - Auto-Gen: Nếu chưa có Map -> Shuffle danh sách kFontPool -> Tạo cặp ngẫu nhiên (VD: "Arial:Verdana") -> Lưu vào Prefs.
          - Inject: Truyền xuống Renderer qua cờ --font-substitution-map="Arial:Verdana,...".
    - third_party/blink/renderer/core/css/
      - font_noise.h
        - FontNoise (Singleton)
        - static FontNoise& Get()
        - void Initialize(): Parse chuỗi Map từ Command Line.
        - String GetSubstitute(const String& family): Trả về tên font thay thế.
        - void ApplyMetricsNoise(FontPlatformData*): Hàm tính toán nhiễu metrics.
      - font_noise.cc
        - Impl: substitution_map_ (HashMap<String, String>)
        - Initialize: Parse chuỗi "Source:Target" thành Map để tra cứu nhanh.
        - GetSubstitute: Kiểm tra Key trong Map -> Return Target Font.
        - ApplyMetricsNoise:
          - Dùng SessionNoiseCache (hoặc Hash nội bộ) để sinh nhiễu đơn định.
          - Chỉnh sửa ascent, descent, lineGap với biên độ ±0.1px.
      - css_font_selector.cc
        - GetFontData(family_name, ...):
          - Gọi FontNoise::Get().GetSubstitute(family_name).
          - Nếu có thay thế -> Load Font mới (Target) thay vì Font gốc.
      - third_party/blink/renderer/platform/fonts/
        - font_cache.cc
          - CreateFontPlatformData():
            - Sau khi load font hệ thống thành công.
            - Gọi FontNoise::Get().ApplyMetricsNoise(result).
            - Kết quả: Font vừa bị thay thế (sai tên), vừa bị lệch kích thước (sai metrics).
      - third_party/blink/renderer/core/css/
        - BUILD.gn
          - blink_core_sources += ["font_noise.cc", "font_noise.h"]
1.3 Rects (--rects-noise):
  - Mô tả: Kỹ thuật chống Fingerprinting dựa trên việc làm lệch nhẹ các tọa độ (x, y) và kích thước (width, height) của DOMRect. Hệ thống sử dụng Deterministic Noise: cùng một giá trị tọa độ đầu vào sẽ luôn sinh ra cùng một giá trị nhiễu đầu ra trong suốt phiên làm việc. Điều này giúp vượt qua các bài kiểm tra tính ổn định (Stability Checks) mà vẫn làm sai lệch Hash tổng thể.
  - Luồng:
Nội dung này chỉ được hỗ trợ trong Lark Docs
  - Files cần sửa:
1. third_party/blink/renderer/core/dom/rects_noise.h/cc -> Wrapper Logic: Gọi sang SessionNoiseCache.
2. third_party/blink/renderer/core/dom/element.cc -> Hook getBoundingClientRect, getClientRects.
3. third_party/blink/renderer/core/dom/range.cc -> Hook getBoundingClientRect (cho selection range).
4. third_party/blink/renderer/core/dom/BUILD.gn -> Thêm source file.
  - RectsNoise Component:
    - core/dom/rects_noise.h (Wrapper)
      - RectsNoise (Singleton)
      - static RectsNoise& Get()
      - double Perturb(double value): Hàm tính nhiễu đơn định cho 1 giá trị.
      - gfx::RectF PerturbRect(const gfx::RectF& rect): Hàm tiện ích xử lý cả hình chữ nhật.
    - core/dom/rects_noise.cc (Implementation)
      - Perturb(value):
        - Gọi SessionNoiseCache::GetInstance().GetNoiseInRange(value, -0.002, 0.002).
        - Lưu ý: Biên độ nhiễu rất nhỏ (0.002px) để không làm vỡ giao diện nhưng đủ để đổi Hash thập phân.
      - PerturbRect(rect):
        - Áp dụng Perturb cho cả 4 thuộc tính: x, y, width, height.
    - core/dom/element.cc
      - Function: getBoundingClientRect()
      - Hook:
        - Lấy result từ LayoutObject.
        - result = RectsNoise::Get().PerturbRect(result).
        - Trả về DOMRect::Create(result).
    - core/dom/range.cc
      - Function: getBoundingClientRect()
      - Hook: Tương tự như Element, áp dụng nhiễu trước khi trả về Rect của vùng bôi đen văn bản.
    - core/dom/BUILD.gn
      - blink_core_sources += ["rects_noise.cc", "rects_noise.h"]
1.4 Audio (--audio-noise):
  - Mô tả: Kỹ thuật chống Fingerprinting sử dụng Deterministic Noise (Nhiễu Đơn Định). Thay vì tạo nhiễu ngẫu nhiên (gây không ổn định), hệ thống sử dụng SessionNoiseCache với giá trị mẫu âm thanh làm khóa (Key). Điều này đảm bảo cùng một dữ liệu âm thanh đầu vào luôn sinh ra cùng một kết quả nhiễu đầu ra trong suốt phiên làm việc, đánh lừa các thuật toán kiểm tra tính nhất quán (Consistency Checks) của các trang web fingerprinting.
  - Luồng: 
Nội dung này chỉ được hỗ trợ trong Lark Docs
  - File cần sửa:
1. third_party/blink/renderer/modules/webaudio/audio_noise_generator.h -> NEW Wrapper Class.
2. third_party/blink/renderer/modules/webaudio/analyser_handler.cc -> Hook getByteFrequencyData, getByteTimeDomainData.
3. third_party/blink/renderer/modules/webaudio/audio_buffer.cc -> Hook getChannelData (PCM data).
4. third_party/blink/renderer/modules/webaudio/oscillator_handler.cc -> Hook frequency.
5. third_party/blink/renderer/modules/webaudio/dynamics_compressor_handler.cc -> Hook parameters.
  - AudioNoise Component
    - modules/webaudio/
      - audio_noise_generator.h
        - AudioNoiseGenerator (Singleton Wrapper)
        - static AudioNoiseGenerator& GetInstance()
        - GetNoise(float val, min, max): Gọi SessionNoiseCache::GetNoiseInRange.
        - GetNoiseInt(int val, min, max): Tính toán nhiễu dựa trên giá trị mẫu (Deterministic).
      - analyser_handler.cc
        - ApplyFrequencyDataNoise(data):
          - Duyệt mảng tần số.
          - noise = generator.GetNoiseInt(data[i], -3, 3).
          - data[i] += noise.
      - audio_buffer.cc
        - getChannelData():
          - Thêm nhiễu vào buffer PCM thô (float* data).
          - Tập trung xử lý 8 mẫu đầu/cuối để phá vỡ hash nhưng không làm hỏng âm thanh.
      - oscillator_handler.cc
        - Process():
          - Làm lệch tần số frequency bằng chính giá trị tần số làm seed $\rightarrow$ Pitch shift nhất quán.
      - BUILD.gn
        - (Không cần thêm file .cc riêng, logic nằm trong header wrapper và các handler có sẵn).
1.5 GPU/WebGL Spoofing
  - Mô tả: Kỹ thuật chống Fingerprinting bằng cách can thiệp vào WebGL Context thông qua các cờ khởi động (Command Line Flags). Hệ thống sẽ nhận thông tin Vendor/Renderer giả lập từ dòng lệnh và tiêm (inject) vào các API của WebGL và ANGLE,
  - Luồng:
Nội dung này chỉ được hỗ trợ trong Lark Docs
  - Files Cần Sửa:
    - gpu/config/gpu_spoofer.h/cc -> NEW files (Core Logic nhận giá trị từ CLI).
    - gpu/config/BUILD.gn -> + "gpu_spoofer.cc/h".
    - third_party/blink/renderer/modules/webgl/webgl_rendering_context_base.cc -> Hook getParameter() (Standard) và readPixels().
    - third_party/blink/renderer/modules/webgl/webgl_debug_renderer_info.cc -> Hook getParameter() (Unmasked Info - Critical).
    - content/public/common/content_switches.h -> Định nghĩa switches mới (kWebGLVendor, kWebGLRenderer).
  - GPUSpoofer Component Chi Tiết:
    - gpu/config/gpu_spoofer.h
      - GPUSpoofer (Singleton)
      - static GPUSpoofer& Get()
      - void Initialize(): Đọc từ CommandLine.
      - String GetVendorString(bool unmasked)
      - String GetRendererString(bool unmasked)
      - void ApplyNoise(void* pixels, size_t size)
    - gpu/config/gpu_spoofer.cc
      - Impl: fake_vendor_, fake_renderer_ (std::string).
      - Logic Initialize():
        - auto* command_line = base::CommandLine::ForCurrentProcess();
        - if (HasSwitch(kWebGLVendor)) fake_vendor_ = GetSwitchValueASCII(kWebGLVendor);
        - if (HasSwitch(kWebGLRenderer)) fake_renderer_ = GetSwitchValueASCII(kWebGLRenderer);
      - Logic GetRendererString(unmasked):
        - Nếu unmasked == true và đang chạy trên Windows -> Format chuỗi ANGLE: ANGLE (fake_vendor_, fake_renderer_, ...) để trông giống thật.
        - Nếu unmasked == false -> Trả về fake_renderer_ trực tiếp.
    - webgl_rendering_context_base.cc
      - void getParameter(GLenum pname): Hook GL_VENDOR, GL_RENDERER.
      - void readPixels(...): Gọi GPUSpoofer::Get().ApplyNoise().
    - webgl_debug_renderer_info.cc
      - void getParameter(GLenum pname): Hook UNMASKED_VENDOR_WEBGL, UNMASKED_RENDERER_WEBGL.
    - BUILD.gn
      - gpu_sources += ["gpu_spoofer.*"]
1.6 Blacklist-Whitelist
  - Mô tả: Hệ thống kiểm soát truy cập Web tập trung thông qua giao thức HTTP. Thay vì lưu danh sách chặn cục bộ, trình duyệt gửi URL điều hướng tới Policy Server để xác thực. Server quyết định Blacklist (Chặn) hoặc Whitelist (Cho phép). Hệ thống áp dụng cơ chế Fail-Closed Security: Nếu không kết nối được Server hoặc Server ngoại tuyến, trình duyệt tự động chuyển sang chế độ Lockdown (Chặn tất cả) để đảm bảo an toàn tuyệt đối.
  - Luồng:
Nội dung này chỉ được hỗ trợ trong Lark Docs
  - File cần sửa:
    - chrome/browser/main/chrome_browser_main_win.cc -> Initialization: Khởi tạo kết nối tới Policy Server qua biến môi trường.
    - chrome/browser/navigation/profile_url_blocker_navigation_throttle.cc -> Hook: Chặn luồng điều hướng và gọi Client kiểm tra.
    - components/policy_manager/policy_ipc_client.cc/h -> Core Logic: Xử lý HTTP Request và Fail-Closed logic.
    - components/policy_manager/BUILD.gn -> Cấu hình build cho module Policy.
  - Policyclient component:
    - chrome/browser/main/chrome_browser_main_win.ccInitialization (PostCreateThreads)
      - Đọc biến môi trường env->GetVar("CHROMIUM_POLICY_URL").
      - Mặc định: http://localhost:8765 nếu không cấu hình.
      - Gọi ipc_client->Initialize(server_url).
      - Log trạng thái: Xác nhận "Fail-closed security: ENABLED".
    - components/policy_manager/policy_ipc_client.ccCheckUrl(const GURL& url)
      - Check Connection: Nếu !is_connected_ hoặc Server timeout -> Return FALSE (Block - Fail Closed).
      - Send Request: Gửi HTTP Request tới Server API.
      - Parse Response:
        - 200 OK + Body "ALLOW" -> Return TRUE.
        - 200 OK + Body "BLOCK" -> Return FALSE.
        - Lỗi 404/500 -> Return FALSE.
    - chrome/browser/navigation/profile_url_blocker_navigation_throttle.ccWillStartRequest()
      - Bỏ qua các URL nội bộ (chrome://, devtools://).
      - Gọi PolicyIPCClient::GetInstance()->CheckUrl(url).
      - Nếu kết quả là FALSE:
        - Log: [Policy] Blocked access to: url.
        - Return NavigationThrottle::CANCEL (hoặc Redirect về trang an toàn).
    - Nếu kết quả là TRUE:
      - Return NavigationThrottle::PROCEED.
1.7 Fingerprint Seed Presistence
  - Mô tả: Cơ chế quản lý trạng thái giả lập tự động dựa trên Profile. Khi người dùng khởi chạy với cờ kích hoạt (ví dụ: --canvas-noise), Browser Process sẽ kiểm tra Profile Preferences. Nếu chưa có Seed, hệ thống tự động sinh ngẫu nhiên và lưu vĩnh viễn vào Profile. Nếu đã có, hệ thống tải lại Seed cũ. Cuối cùng, Seed được tiêm (inject) xuống Renderer Process qua cờ nội bộ (Internal Switch) để khởi tạo bộ đệm nhiễu.
  - Luồng:
Nội dung này chỉ được hỗ trợ trong Lark Docs
  - File cần sửa:
    - chrome/browser/chrome_content_browser_client.cc -> Logic trung tâm: Kiểm tra flag User, thao tác Prefs, Inject flag Internal.
    - chrome/browser/profiles/profile.cc -> Đăng ký biến lưu trữ kCanvasNoiseSeed vào Profile.
    - chrome/common/pref_names.h -> Định nghĩa tên biến Preferences.
    - third_party/blink/renderer/platform/privacy_budget/session_noise_cache.cc -> Renderer nhận Seed từ cờ Internal để khởi tạo Cache.
  - Fingerprint Seed Component:
    - chrome/browser/chrome_content_browser_client.cc
      - Function: AppendExtraCommandLineSwitches
      - Logic:
        - Check HasSwitch("canvas-noise").
        - Get Profile* và PrefService*.
        - uint64_t seed = prefs->GetUint64(kCanvasNoiseSeed).
        - Auto-Gen: Nếu seed == 0 -> base::RandUint64() -> prefs->SetUint64(...).
        - Inject: command_line->AppendSwitchASCII("canvas-seed", NumberToString(seed)).
    - chrome/browser/profiles/profile.cc
      - Function: RegisterProfilePrefs
      - Logic:
        - registry->RegisterUint64Pref(prefs::kCanvasNoiseSeed, 0);
        - (Tương tự cho Audio, Rects, Fonts).
    - chrome/common/pref_names.h
      - Constants:
        - inline constexpr char kCanvasNoiseSeed[] = "fingerprinting.canvas_noise_seed";
    - third_party/blink/renderer/platform/privacy_budget/session_noise_cache.cc
      - Class: SessionNoiseCache
      - Logic Initialize():
        - Đọc base::CommandLine::ForCurrentProcess().
        - Check HasSwitch("canvas-seed") (Đây là cờ do Browser Process truyền xuống, không phải User nhập).
        - session_seed_ = GetSwitchValueASCII("canvas-seed").
        - Khởi tạo PRNG Engine từ Seed này để tạo nhiễu Canvas.
 