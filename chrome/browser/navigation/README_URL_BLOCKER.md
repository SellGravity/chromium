# Per-Profile URL Blocker

This feature allows blocking specific URLs per Chrome profile with automatic redirection to Google.com.

## Features

- ✅ **Per-Profile**: Each Chrome profile has its own URL blocklist
- ✅ **Persistent**: Blocklist is saved in profile preferences
- ✅ **Auto-Redirect**: Blocked URLs redirect to https://www.google.com instead of showing error page
- ✅ **Flexible Matching**: Supports exact URLs, domains, and wildcard patterns

## How to Use

### 1. Set Blocklist via Preferences File

Edit your Chrome profile's Preferences file:
- Location: `%USERPROFILE%\AppData\Local\Google\Chrome\User Data\Default\Preferences`
- Or use: `chrome://version` → Copy "Profile Path" → Open `Preferences` file

Add the blocklist:

```json
{
  "profile": {
    "url_blocklist": [
      "facebook.com",
      "x.com",
      "*.reddit.com",
      "https://www.youtube.com/watch"
    ]
  }
}
```

### 2. Pattern Matching

| Pattern | Matches | Doesn't Match |
|---------|---------|---------------|
| `facebook.com` | `facebook.com`, `www.facebook.com`, `m.facebook.com` | `fbcdn.com` |
| `*.reddit.com` | `old.reddit.com`, `www.reddit.com` | `reddit.com` (no subdomain) |
| `https://example.com/path` | Exact URL only | Other paths |

### 3. Programmatic Access

You can also set the blocklist programmatically:

```cpp
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"

PrefService* prefs = profile->GetPrefs();
base::Value::List blocklist;
blocklist.Append("facebook.com");
blocklist.Append("twitter.com");
prefs->SetList(prefs::kProfileURLBlocklist, std::move(blocklist));
```

## Testing

```bash
# 1. Run Chrome with a test profile
out/Default/chrome.exe --user-data-dir=c:\temp\test_profile

# 2. Set blocklist via DevTools Console:
# chrome://settings → Developer Tools → Console:
chrome.settingsPrivate.setPref('profile.url_blocklist', ['facebook.com', 'twitter.com']);

# 3. Try navigating to blocked sites
# → Should redirect to google.com

# 4. Check logs for blocked URLs:
# Look for: "[Profile URL Blocker] Blocked access to: ..."
```

## Implementation Details

### Files Created:
- `chrome/browser/navigation/profile_url_blocker_navigation_throttle.h`
- `chrome/browser/navigation/profile_url_blocker_navigation_throttle.cc`

### Prefs Added:
- `prefs::kProfileURLBlocklist` in `chrome/common/pref_names.h`

### Navigation Flow:
```
User navigates to URL
  ↓
ProfileURLBlockerNavigationThrottle::WillStartRequest()
  ↓
IsURLBlocked() checks pref "profile.url_blocklist"
  ↓
If matched:
  - ShowBlockedNotification() logs message
  - Returns CANCEL with redirect to https://www.google.com
Else:
  - Returns PROCEED (allow navigation)
```

## Future Enhancements

- [ ] Add UI notification (InfoBar or Chrome notification)
- [ ] Add settings page UI for managing blocklist
- [ ] Support custom redirect URL per profile
- [ ] Add whitelist exceptions
- [ ] Add time-based blocking (e.g., block during work hours)

## Notes

- Internal Chrome pages (`chrome://`, `chrome-extension://`, `devtools://`) are never blocked
- The blocklist is per-profile, not global
- Changes to preferences require Chrome restart to take effect
- This is different from enterprise policy URL blocking (which shows error page)

## Related Code

- Policy-based blocking: `components/policy/content/policy_blocklist_navigation_throttle.cc`
- URL matching: `components/policy/core/browser/url_blocklist_manager.cc`
