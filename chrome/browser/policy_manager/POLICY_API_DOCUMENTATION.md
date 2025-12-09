# Policy Manager API Documentation

## Tài liệu Nguyên lý Hoạt động - Hệ thống Chặn URL qua Policy Server

### Mục lục
1. [Tổng quan Kiến trúc](#1-tổng-quan-kiến-trúc)
2. [Luồng Hoạt động](#2-luồng-hoạt-động)
3. [API Endpoints](#3-api-endpoints)
4. [Cấu trúc Dữ liệu](#4-cấu-trúc-dữ-liệu)
5. [Cơ chế Bảo mật Fail-Closed](#5-cơ-chế-bảo-mật-fail-closed)
6. [Hướng dẫn Xây dựng Management App](#6-hướng-dẫn-xây-dựng-management-app)
7. [Ví dụ Code](#7-ví-dụ-code)

---

## 1. Tổng quan Kiến trúc

### 1.1 Sơ đồ Kiến trúc Hệ thống

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          MANAGEMENT APP (Admin)                              │
│                                                                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
│  │ Add/Remove  │  │  Blocklist  │  │  Whitelist  │  │   Monitor   │        │
│  │    URLs     │  │   Viewer    │  │   Viewer    │  │ Connections │        │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘        │
│         │                │                │                │                │
│         └────────────────┴────────────────┴────────────────┘                │
│                                    │                                        │
│                                    ▼                                        │
│                          ┌─────────────────┐                               │
│                          │   REST API      │                               │
│                          │  HTTP Client    │                               │
│                          └────────┬────────┘                               │
└───────────────────────────────────│────────────────────────────────────────┘
                                    │
                           HTTP (Port 8765)
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        POLICY SERVER (Python Flask)                          │
│                                                                             │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐             │
│  │   REST API      │  │   Profile       │  │   Persistent    │             │
│  │   Handlers      │  │   Manager       │  │   Storage       │             │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘             │
│           │                    │                    │                       │
│           │        ┌───────────┴───────────┐       │                       │
│           │        │   ProfilePolicyServer │       │                       │
│           │        │                       │       │                       │
│           │        │  - profile_policies   │◄──────┘                       │
│           │        │  - active_profiles    │      (policies.json)          │
│           │        │  - storage_file       │                               │
│           │        └───────────────────────┘                               │
│           │                    │                                            │
│           └────────────────────┘                                            │
│                        │                                                    │
│                  endpoints:                                                 │
│                  POST /api/check_url                                        │
│                  GET  /api/policy                                           │
│                  GET  /api/version                                          │
│                  GET  /api/health                                           │
└─────────────────────│───────────────────────────────────────────────────────┘
                      │
                HTTP (Port 8765)
                      │
                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                          CHROMIUM BROWSER (Client)                           │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                     Navigation Flow                                  │   │
│  │                                                                      │   │
│  │   User navigates    ProfileURLBlockerNavigation   PolicyIPCClient    │   │
│  │   to URL  ───────►  Throttle::WillStartRequest ──►  CheckURLSync    │   │
│  │                              │                           │           │   │
│  │                              │                           ▼           │   │
│  │                              │                    HTTP POST to       │   │
│  │                              │                    /api/check_url     │   │
│  │                              │                           │           │   │
│  │                              ◄───────────────────────────┘           │   │
│  │                              │                                       │   │
│  │                              ▼                                       │   │
│  │                     ┌────────────────┐                              │   │
│  │                     │ allow: true?   │                              │   │
│  │                     └───────┬────────┘                              │   │
│  │                             │                                       │   │
│  │              ┌──────────────┴──────────────┐                        │   │
│  │              │                             │                        │   │
│  │              ▼                             ▼                        │   │
│  │        ┌──────────┐                 ┌──────────────┐                │   │
│  │        │ PROCEED  │                 │    BLOCK     │                │   │
│  │        │ (allow)  │                 │ (show error) │                │   │
│  │        └──────────┘                 └──────────────┘                │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  Components:                                                                │
│  - PolicyIPCClient (chrome/browser/policy_manager/)                        │
│  - ProfileURLBlockerNavigationThrottle (chrome/browser/navigation/)        │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 Các Component Chính

| Component | File | Mô tả |
|-----------|------|-------|
| **Policy Server** | `test_policy_server_http.py` | Flask HTTP server quản lý policy |
| **PolicyIPCClient** | `policy_ipc_client.cc/h` | Client C++ giao tiếp với server |
| **NavigationThrottle** | `profile_url_blocker_navigation_throttle.cc/h` | Chặn navigation dựa trên policy |
| **Policy Storage** | `policies.json` | File lưu trữ persistent |

---

## 2. Luồng Hoạt động

### 2.1 Luồng Kiểm tra URL (URL Check Flow)

```
┌──────────────┐     ┌─────────────────────────────────┐     ┌──────────────────┐
│   Browser    │     │  ProfileURLBlockerNavigation    │     │  PolicyIPCClient │
│  (User)      │     │        Throttle                 │     │                  │
└──────┬───────┘     └───────────────┬─────────────────┘     └────────┬─────────┘
       │                             │                                 │
       │  1. Navigate to URL         │                                 │
       │────────────────────────────►│                                 │
       │                             │                                 │
       │                             │  2. WillStartRequest()          │
       │                             │─────────────────────────────────│
       │                             │                                 │
       │                             │  3. CheckURLSync(url, profile)  │
       │                             │────────────────────────────────►│
       │                             │                                 │
       │                             │                    ┌────────────┴────────────┐
       │                             │                    │ 4. HTTP POST            │
       │                             │                    │    /api/check_url       │
       │                             │                    │    {"url": "...",       │
       │                             │                    │     "profile": "..."}   │
       │                             │                    └────────────┬────────────┘
       │                             │                                 │
       │                             │                    ┌────────────▼────────────┐
       │                             │                    │      Policy Server      │
       │                             │                    │                         │
       │                             │                    │  5. Check whitelist     │
       │                             │                    │  6. Check blocklist     │
       │                             │                    │  7. Return decision     │
       │                             │                    └────────────┬────────────┘
       │                             │                                 │
       │                             │  8. PolicyDecision              │
       │                             │    {allow: bool, reason: str}   │
       │                             │◄────────────────────────────────│
       │                             │                                 │
       │                             │  9. PROCEED or BLOCK            │
       │◄────────────────────────────│                                 │
       │                             │                                 │
```

### 2.2 Ưu tiên Kiểm tra (Priority Order)

```
┌─────────────────────────────────────────────────────────────────┐
│                     URL CHECK PRIORITY                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  PRIORITY 1: WHITELIST (Highest)                                │
│  ─────────────────────────────────                              │
│  • Nếu URL khớp whitelist → ALLOW ngay lập tức                  │
│  • Không cần kiểm tra blocklist                                 │
│                                                                  │
│              │                                                   │
│              ▼                                                   │
│                                                                  │
│  PRIORITY 2: BLOCKLIST                                          │
│  ─────────────────────────────────                              │
│  • Nếu URL khớp blocklist → BLOCK                               │
│  • Hiển thị trang lỗi custom                                    │
│                                                                  │
│              │                                                   │
│              ▼                                                   │
│                                                                  │
│  PRIORITY 3: DEFAULT ALLOW                                      │
│  ─────────────────────────────────                              │
│  • URL không trong bất kỳ list nào → ALLOW                      │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 2.3 Pattern Matching

Server hỗ trợ các loại pattern sau:

| Pattern | Ví dụ | Mô tả |
|---------|-------|-------|
| Exact domain | `facebook.com` | Khớp chính xác domain |
| Subdomain wildcard | `*.facebook.com` | Khớp tất cả subdomain (m.facebook.com, www.facebook.com) |
| Full URL | `https://example.com/path` | Khớp URL cụ thể |
| All | `*` | Chặn tất cả (hiếm khi dùng) |

**Code xử lý pattern (Python - Server side):**
```python
@staticmethod
def match_pattern(url, pattern):
    """Simple pattern matching."""
    if pattern == "*":
        return True

    if pattern.startswith("*."):
        domain = pattern[2:]
        return domain in url

    return pattern in url
```

---

## 3. API Endpoints

### 3.1 POST /api/check_url

**Mục đích:** Kiểm tra xem URL có bị chặn không

**Request:**
```json
POST /api/check_url
Content-Type: application/json

{
    "url": "https://facebook.com/feed",
    "profile": "Default"
}
```

**Response (ALLOWED):**
```json
{
    "allow": true,
    "reason": "Not in any list"
}
```

**Response (BLOCKED):**
```json
{
    "allow": false,
    "reason": "Blacklisted: facebook.com"
}
```

**Response (WHITELISTED):**
```json
{
    "allow": true,
    "reason": "Whitelisted: https://company.com"
}
```

---

### 3.2 GET /api/policy

**Mục đích:** Lấy toàn bộ policy của một profile

**Request:**
```
GET /api/policy?profile=Default&role=viewer
```

**Response:**
```json
{
    "Features": {
        "EnableHistory": true,
        "EnableDownloads": true
    },
    "HomepageLocation": "https://google.com",
    "URLBlocklist": [
        "facebook.com",
        "twitter.com",
        "*.reddit.com",
        "tiktok.com"
    ],
    "URLAllowlist": [
        "https://company.com"
    ],
    "version": 1
}
```

---

### 3.3 GET /api/version

**Mục đích:** Lấy version hiện tại của policy (dùng để detect changes)

**Request:**
```
GET /api/version?profile=Default
```

**Response:**
```json
{
    "version": 3
}
```

---

### 3.4 GET /api/health

**Mục đích:** Health check - kiểm tra server có hoạt động

**Request:**
```
GET /api/health
```
hoặc
```
POST /api/health
```

**Response:**
```json
{
    "status": "healthy",
    "server": "Policy Server HTTP API",
    "profiles": 5
}
```

---

## 4. Cấu trúc Dữ liệu

### 4.1 File policies.json

```json
{
    "Default": {
        "blocklist": {
            "1": "facebook.com",
            "2": "twitter.com",
            "3": "*.reddit.com",
            "4": "tiktok.com",
            "5": "youtube.com"
        },
        "whitelist": {
            "1": "https://company.com",
            "2": "https://old.reddit.com"
        },
        "next_blocklist_id": 6,
        "next_whitelist_id": 3,
        "version": 3
    },
    "ProfileA": {
        "blocklist": {
            "1": "facebook.com",
            "2": "*.facebook.com"
        },
        "whitelist": {
            "1": "twitter.com"
        },
        "next_blocklist_id": 3,
        "next_whitelist_id": 2,
        "version": 1
    }
}
```

### 4.2 Giải thích Cấu trúc

| Field | Type | Mô tả |
|-------|------|-------|
| `blocklist` | `Dict[int, str]` | Map từ ID → URL pattern bị chặn |
| `whitelist` | `Dict[int, str]` | Map từ ID → URL pattern được phép |
| `next_blocklist_id` | `int` | ID tiếp theo cho blocklist entry mới |
| `next_whitelist_id` | `int` | ID tiếp theo cho whitelist entry mới |
| `version` | `int` | Version number, tăng mỗi khi policy thay đổi |

---

## 5. Cơ chế Bảo mật Fail-Closed

### 5.1 Nguyên lý Fail-Closed

**Fail-Closed** có nghĩa: Khi server không khả dụng → **CHẶN TẤT CẢ** thay vì cho phép.

Điều này ngăn chặn bypass bằng cách tắt server.

```
┌─────────────────────────────────────────────────────────────────┐
│                    FAIL-CLOSED SECURITY                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Server ONLINE:                                                  │
│  ────────────────                                               │
│  ✓ Kiểm tra URL bình thường                                     │
│  ✓ Whitelist/Blocklist hoạt động                                │
│                                                                  │
│  Server OFFLINE (Grace Period < 30s):                           │
│  ──────────────────────────────────────                         │
│  ⚠ Retry kết nối                                                │
│  ⚠ Cho phép URL trong thời gian grace period                    │
│                                                                  │
│  Server OFFLINE (Grace Period > 30s):                           │
│  ──────────────────────────────────────                         │
│  🔒 LOCKDOWN MODE                                               │
│  🔒 CHẶN TẤT CẢ navigation                                      │
│  🔒 Hiển thị: "Policy server unavailable"                       │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 Các Tham số Bảo mật (Client-side)

```cpp
// Default values trong PolicyIPCClient
bool fail_closed_mode_ = true;           // BẬT fail-closed mặc định
base::TimeDelta grace_period_ = base::Seconds(30);  // 30 giây grace period
int kMaxFailuresBeforeLockdown = 3;      // 3 lần thất bại → lockdown
bool auto_reconnect_enabled_ = true;     // Tự động reconnect
base::TimeDelta reconnect_delay_ = base::Seconds(5); // 5 giây giữa các lần retry
```

### 5.3 Logic Kiểm tra Lockdown

```cpp
bool PolicyIPCClient::IsInLockdownMode() const {
  // Nếu không bật fail-closed → không bao giờ lockdown
  if (!fail_closed_mode_) {
    return false;
  }

  // Lockdown nếu quá nhiều lần thất bại liên tiếp
  if (consecutive_failures_ >= kMaxFailuresBeforeLockdown) {
    return true;
  }

  // Lockdown nếu quá lâu không liên lạc được với server
  if (GetTimeSinceLastContact() > grace_period_) {
    return true;
  }

  return false;
}
```

---

## 6. Hướng dẫn Xây dựng Management App

### 6.1 Kiến trúc Đề xuất cho Management App

```
┌─────────────────────────────────────────────────────────────────┐
│                    MANAGEMENT APP (C#/Python/Electron)           │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                        UI Layer                              ││
│  │                                                              ││
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       ││
│  │  │  Profile     │  │  Blocklist   │  │  Whitelist   │       ││
│  │  │  Selector    │  │  Editor      │  │  Editor      │       ││
│  │  └──────────────┘  └──────────────┘  └──────────────┘       ││
│  │                                                              ││
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       ││
│  │  │  Connection  │  │  Activity    │  │  Settings    │       ││
│  │  │  Status      │  │  Monitor     │  │  Panel       │       ││
│  │  └──────────────┘  └──────────────┘  └──────────────┘       ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                     API Client Layer                         ││
│  │                                                              ││
│  │  class PolicyAPIClient {                                     ││
│  │      // Core methods                                         ││
│  │      CheckHealth()                                           ││
│  │      GetPolicy(profile)                                      ││
│  │      CheckURL(url, profile)                                  ││
│  │                                                              ││
│  │      // Management methods (cần thêm vào server)            ││
│  │      AddToBlocklist(profile, url)                           ││
│  │      RemoveFromBlocklist(profile, id)                       ││
│  │      AddToWhitelist(profile, url)                           ││
│  │      RemoveFromWhitelist(profile, id)                       ││
│  │      ListProfiles()                                          ││
│  │      CreateProfile(name)                                     ││
│  │      DeleteProfile(name)                                     ││
│  │  }                                                           ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│                        HTTP REST API                            │
│                              │                                   │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                    Policy Server (Flask)                     ││
│  │              http://localhost:8765                           ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

### 6.2 API Endpoints Cần Thêm cho Management

Để quản lý đầy đủ từ app, cần thêm các endpoints sau vào server:

```python
# === PROFILE MANAGEMENT ===

@app.route('/api/profiles', methods=['GET'])
def list_profiles():
    """List all profiles."""
    return jsonify({
        "profiles": list(server.profile_policies.keys()),
        "active_profiles": list(server.active_profiles)
    })

@app.route('/api/profiles/<profile_name>', methods=['POST'])
def create_profile(profile_name):
    """Create a new profile."""
    server.get_profile_policy(profile_name)  # Auto-creates if not exists
    return jsonify({"status": "created", "profile": profile_name})

@app.route('/api/profiles/<profile_name>', methods=['DELETE'])
def delete_profile(profile_name):
    """Delete a profile."""
    if profile_name in server.profile_policies:
        del server.profile_policies[profile_name]
        server.save_policies()
        return jsonify({"status": "deleted"})
    return jsonify({"error": "Profile not found"}), 404


# === BLOCKLIST MANAGEMENT ===

@app.route('/api/profiles/<profile_name>/blocklist', methods=['GET'])
def get_blocklist(profile_name):
    """Get blocklist for a profile."""
    policy = server.get_profile_policy(profile_name)
    return jsonify({
        "profile": profile_name,
        "blocklist": policy["blocklist"],
        "version": policy["version"]
    })

@app.route('/api/profiles/<profile_name>/blocklist', methods=['POST'])
def add_to_blocklist(profile_name):
    """Add URL to blocklist."""
    data = request.get_json()
    url = data.get('url')
    
    if not url:
        return jsonify({"error": "Missing URL"}), 400
    
    policy = server.get_profile_policy(profile_name)
    
    with server.lock:
        # Check for duplicates
        for existing in policy["blocklist"].values():
            if existing == url:
                return jsonify({"error": "URL already exists"}), 409
        
        url_id = policy["next_blocklist_id"]
        policy["blocklist"][str(url_id)] = url
        policy["next_blocklist_id"] += 1
        policy["version"] += 1
        server.save_policies()
    
    return jsonify({
        "status": "added",
        "id": url_id,
        "url": url,
        "version": policy["version"]
    })

@app.route('/api/profiles/<profile_name>/blocklist/<int:url_id>', methods=['DELETE'])
def remove_from_blocklist(profile_name, url_id):
    """Remove URL from blocklist by ID."""
    policy = server.get_profile_policy(profile_name)
    
    with server.lock:
        str_id = str(url_id)
        if str_id in policy["blocklist"]:
            removed = policy["blocklist"].pop(str_id)
            policy["version"] += 1
            server.save_policies()
            return jsonify({"status": "removed", "url": removed})
        
    return jsonify({"error": "ID not found"}), 404


# === WHITELIST MANAGEMENT ===

@app.route('/api/profiles/<profile_name>/whitelist', methods=['GET'])
def get_whitelist(profile_name):
    """Get whitelist for a profile."""
    policy = server.get_profile_policy(profile_name)
    return jsonify({
        "profile": profile_name,
        "whitelist": policy["whitelist"],
        "version": policy["version"]
    })

@app.route('/api/profiles/<profile_name>/whitelist', methods=['POST'])
def add_to_whitelist(profile_name):
    """Add URL to whitelist."""
    data = request.get_json()
    url = data.get('url')
    
    if not url:
        return jsonify({"error": "Missing URL"}), 400
    
    policy = server.get_profile_policy(profile_name)
    
    with server.lock:
        url_id = policy["next_whitelist_id"]
        policy["whitelist"][str(url_id)] = url
        policy["next_whitelist_id"] += 1
        policy["version"] += 1
        server.save_policies()
    
    return jsonify({
        "status": "added",
        "id": url_id,
        "url": url,
        "version": policy["version"]
    })

@app.route('/api/profiles/<profile_name>/whitelist/<int:url_id>', methods=['DELETE'])
def remove_from_whitelist(profile_name, url_id):
    """Remove URL from whitelist by ID."""
    policy = server.get_profile_policy(profile_name)
    
    with server.lock:
        str_id = str(url_id)
        if str_id in policy["whitelist"]:
            removed = policy["whitelist"].pop(str_id)
            policy["version"] += 1
            server.save_policies()
            return jsonify({"status": "removed", "url": removed})
        
    return jsonify({"error": "ID not found"}), 404
```

### 6.3 Example: C# Management Client

```csharp
using System.Net.Http;
using System.Text.Json;

public class PolicyManagerClient
{
    private readonly HttpClient _client;
    private readonly string _baseUrl;
    
    public PolicyManagerClient(string baseUrl = "http://localhost:8765")
    {
        _baseUrl = baseUrl;
        _client = new HttpClient();
    }
    
    // Health Check
    public async Task<bool> IsServerHealthyAsync()
    {
        try
        {
            var response = await _client.GetAsync($"{_baseUrl}/api/health");
            if (response.IsSuccessStatusCode)
            {
                var json = await response.Content.ReadAsStringAsync();
                var data = JsonSerializer.Deserialize<HealthResponse>(json);
                return data?.Status == "healthy";
            }
            return false;
        }
        catch { return false; }
    }
    
    // Get all profiles
    public async Task<List<string>> GetProfilesAsync()
    {
        var response = await _client.GetAsync($"{_baseUrl}/api/profiles");
        var json = await response.Content.ReadAsStringAsync();
        var data = JsonSerializer.Deserialize<ProfilesResponse>(json);
        return data?.Profiles ?? new List<string>();
    }
    
    // Get blocklist for profile
    public async Task<Dictionary<string, string>> GetBlocklistAsync(string profile)
    {
        var response = await _client.GetAsync($"{_baseUrl}/api/profiles/{profile}/blocklist");
        var json = await response.Content.ReadAsStringAsync();
        var data = JsonSerializer.Deserialize<BlocklistResponse>(json);
        return data?.Blocklist ?? new Dictionary<string, string>();
    }
    
    // Add URL to blocklist
    public async Task<bool> AddToBlocklistAsync(string profile, string url)
    {
        var content = new StringContent(
            JsonSerializer.Serialize(new { url }),
            Encoding.UTF8,
            "application/json"
        );
        var response = await _client.PostAsync(
            $"{_baseUrl}/api/profiles/{profile}/blocklist", 
            content
        );
        return response.IsSuccessStatusCode;
    }
    
    // Remove URL from blocklist
    public async Task<bool> RemoveFromBlocklistAsync(string profile, int id)
    {
        var response = await _client.DeleteAsync(
            $"{_baseUrl}/api/profiles/{profile}/blocklist/{id}"
        );
        return response.IsSuccessStatusCode;
    }
    
    // Check if URL is blocked
    public async Task<PolicyDecision> CheckUrlAsync(string url, string profile = "Default")
    {
        var content = new StringContent(
            JsonSerializer.Serialize(new { url, profile }),
            Encoding.UTF8,
            "application/json"
        );
        var response = await _client.PostAsync($"{_baseUrl}/api/check_url", content);
        var json = await response.Content.ReadAsStringAsync();
        return JsonSerializer.Deserialize<PolicyDecision>(json);
    }
}

// Data classes
public class HealthResponse
{
    public string Status { get; set; }
    public int Profiles { get; set; }
}

public class ProfilesResponse
{
    public List<string> Profiles { get; set; }
    public List<string> ActiveProfiles { get; set; }
}

public class BlocklistResponse
{
    public string Profile { get; set; }
    public Dictionary<string, string> Blocklist { get; set; }
    public int Version { get; set; }
}

public class PolicyDecision
{
    public bool Allow { get; set; }
    public string Reason { get; set; }
}
```

### 6.4 Example: Python Management Client

```python
import requests
from typing import Dict, List, Optional

class PolicyManagerClient:
    def __init__(self, base_url: str = "http://localhost:8765"):
        self.base_url = base_url
    
    def is_healthy(self) -> bool:
        """Check if server is healthy."""
        try:
            r = requests.get(f"{self.base_url}/api/health", timeout=5)
            return r.json().get("status") == "healthy"
        except:
            return False
    
    def get_profiles(self) -> List[str]:
        """Get all profiles."""
        r = requests.get(f"{self.base_url}/api/profiles")
        return r.json().get("profiles", [])
    
    def get_blocklist(self, profile: str) -> Dict[str, str]:
        """Get blocklist for a profile."""
        r = requests.get(f"{self.base_url}/api/profiles/{profile}/blocklist")
        return r.json().get("blocklist", {})
    
    def add_to_blocklist(self, profile: str, url: str) -> bool:
        """Add URL to blocklist."""
        r = requests.post(
            f"{self.base_url}/api/profiles/{profile}/blocklist",
            json={"url": url}
        )
        return r.status_code == 200
    
    def remove_from_blocklist(self, profile: str, url_id: int) -> bool:
        """Remove URL from blocklist."""
        r = requests.delete(
            f"{self.base_url}/api/profiles/{profile}/blocklist/{url_id}"
        )
        return r.status_code == 200
    
    def get_whitelist(self, profile: str) -> Dict[str, str]:
        """Get whitelist for a profile."""
        r = requests.get(f"{self.base_url}/api/profiles/{profile}/whitelist")
        return r.json().get("whitelist", {})
    
    def add_to_whitelist(self, profile: str, url: str) -> bool:
        """Add URL to whitelist."""
        r = requests.post(
            f"{self.base_url}/api/profiles/{profile}/whitelist",
            json={"url": url}
        )
        return r.status_code == 200
    
    def check_url(self, url: str, profile: str = "Default") -> dict:
        """Check if URL is blocked."""
        r = requests.post(
            f"{self.base_url}/api/check_url",
            json={"url": url, "profile": profile}
        )
        return r.json()


# Usage example
if __name__ == "__main__":
    client = PolicyManagerClient()
    
    if client.is_healthy():
        print("✓ Server is healthy")
        
        # List profiles
        profiles = client.get_profiles()
        print(f"Profiles: {profiles}")
        
        # Add URL to blocklist
        client.add_to_blocklist("Default", "instagram.com")
        
        # Check URL
        result = client.check_url("https://instagram.com/")
        print(f"Check instagram.com: {result}")
    else:
        print("✗ Server is offline")
```

---

## 7. Ví dụ Code

### 7.1 Test API với cURL

```bash
# Health check
curl http://localhost:8765/api/health

# Check URL
curl -X POST http://localhost:8765/api/check_url \
  -H "Content-Type: application/json" \
  -d '{"url": "https://facebook.com", "profile": "Default"}'

# Get policy
curl "http://localhost:8765/api/policy?profile=Default"

# Get version
curl "http://localhost:8765/api/version?profile=Default"
```

### 7.2 Test với PowerShell

```powershell
# Health check
Invoke-RestMethod -Uri "http://localhost:8765/api/health"

# Check URL
$body = @{
    url = "https://facebook.com"
    profile = "Default"
} | ConvertTo-Json

Invoke-RestMethod -Uri "http://localhost:8765/api/check_url" `
    -Method POST `
    -ContentType "application/json" `
    -Body $body

# Get policy
Invoke-RestMethod -Uri "http://localhost:8765/api/policy?profile=Default"
```

### 7.3 Kết nối Từ JavaScript/Browser

```javascript
class PolicyClient {
    constructor(baseUrl = 'http://localhost:8765') {
        this.baseUrl = baseUrl;
    }
    
    async checkUrl(url, profile = 'Default') {
        const response = await fetch(`${this.baseUrl}/api/check_url`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ url, profile })
        });
        return response.json();
    }
    
    async getPolicy(profile = 'Default') {
        const response = await fetch(
            `${this.baseUrl}/api/policy?profile=${profile}`
        );
        return response.json();
    }
    
    async isHealthy() {
        try {
            const response = await fetch(`${this.baseUrl}/api/health`);
            const data = await response.json();
            return data.status === 'healthy';
        } catch {
            return false;
        }
    }
}

// Usage
const client = new PolicyClient();
client.checkUrl('https://facebook.com').then(result => {
    console.log('Allowed:', result.allow);
    console.log('Reason:', result.reason);
});
```

---

## Tổng kết

### Các điểm quan trọng:

1. **Server chạy tại `http://localhost:8765`**
2. **Mọi request đều dùng JSON**
3. **Whitelist có priority cao hơn Blocklist**
4. **Fail-closed mode chặn tất cả khi server offline**
5. **Mỗi profile có blocklist/whitelist riêng**
6. **Version tăng mỗi khi policy thay đổi**

### Files liên quan:
- `test_policy_server_http.py` - Policy Server
- `policy_ipc_client.cc/h` - Client C++ trong Chromium
- `profile_url_blocker_navigation_throttle.cc/h` - Navigation throttle
- `policies.json` - Persistent storage

---

*Tài liệu được tạo bởi GitHub Copilot - Phiên bản 1.0*
