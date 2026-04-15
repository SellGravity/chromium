// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/app/chrome_exe_main_win.h"

#include <tchar.h>
#include <windows.h>

#include <malloc.h>
#include <stddef.h>
#include <shlobj.h>
#include <shellapi.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/debug/alias.h"
#include "base/debug/handle_hooks_win.h"
#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/process/memory.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/win/current_module.h"
#include "base/win/win_util.h"
#include "base/win/windows_version.h"
#include "build/build_config.h"
#include "chrome/app/delay_load_failure_hook_win.h"
#include "chrome/app/exit_code_watcher_win.h"
#include "chrome/app/main_dll_loader_win.h"
#include "chrome/app/packed_resources_integrity.h"
#include "chrome/browser/policy/policy_path_parser.h"
#include "chrome/browser/win/chrome_process_finder.h"
#include "chrome/chrome_elf/chrome_elf_main.h"
#include "chrome/common/chrome_paths_internal.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/install_static/initialize_from_primary_module.h"
#include "chrome/install_static/install_util.h"
#include "chrome/install_static/user_data_dir.h"
#include "components/crash/core/app/crash_switches.h"
#include "components/crash/core/app/crashpad.h"
#include "components/crash/core/app/fallback_crash_handling_win.h"
#include "components/crash/core/app/run_as_crashpad_handler_win.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"
#include "third_party/crashpad/crashpad/util/win/initial_client_data.h"

#if defined(WIN_CONSOLE_APP)
// Forward declaration of main.
int main();
#endif

namespace {

// =============================================================================
// PORTABLE SANDBOX PERMISSION FIX
// =============================================================================
// When Gra Browser is extracted to a new folder (USB, different drive, etc.),
// AppContainer sandbox fails because the folder lacks permissions for
// "ALL APPLICATION PACKAGES" (SID: S-1-15-2-1).
//
// This function automatically grants Read/Execute permissions to the browser
// directory, running only ONCE per new location (uses a marker file).
// =============================================================================

// Marker file to track if permissions have been granted for this location
constexpr wchar_t kPermissionMarkerFile[] = L".sandbox_permissions_granted";

// Get the directory containing the current executable
std::wstring GetExeDirectory() {
  wchar_t buffer[MAX_PATH + 1] = {0};
  DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return L"";
  }
  
  std::wstring path(buffer);
  size_t last_slash = path.find_last_of(L"\\/");
  if (last_slash != std::wstring::npos) {
    return path.substr(0, last_slash);
  }
  return L"";
}

// Check if we've already granted permissions for THIS SPECIFIC directory.
// The marker file stores the path where permissions were granted.
// If the browser folder was moved/copied, the stored path won't match
// the current path, so we'll re-grant permissions.
bool HasPermissionMarker(const std::wstring& dir) {
  std::wstring marker_path = dir + L"\\" + kPermissionMarkerFile;
  
  // Step 1: Check if marker file exists
  HANDLE hFile = ::CreateFileW(
      marker_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  
  if (hFile == INVALID_HANDLE_VALUE) {
    return false;  // File doesn't exist -> need to grant permissions
  }
  
  // Step 2: Read the stored path from marker file
  wchar_t stored_path[MAX_PATH + 1] = {0};
  DWORD bytes_read = 0;
  BOOL read_success = ::ReadFile(
      hFile,
      stored_path,
      MAX_PATH * sizeof(wchar_t),
      &bytes_read,
      nullptr);
  ::CloseHandle(hFile);
  
  if (!read_success || bytes_read == 0) {
    return false;  // Can't read file -> re-grant permissions
  }
  
  // Ensure null-termination
  size_t char_count = bytes_read / sizeof(wchar_t);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
  if (char_count <= MAX_PATH) {
    stored_path[char_count] = L'\0';
  } else {
    stored_path[MAX_PATH] = L'\0';
  }
#pragma clang diagnostic pop
  
  // Step 3 & 4: Compare stored path with current directory
  // Case-insensitive comparison for Windows paths
  if (_wcsicmp(stored_path, dir.c_str()) == 0) {
    return true;   // Paths match -> permissions already granted for this location
  }
  
  // Paths don't match -> browser was moved/copied, need to re-grant permissions
  return false;
}

// Create the permission marker file with the current directory path.
// This allows us to detect if the browser folder is moved/copied later.
void CreatePermissionMarker(const std::wstring& dir) {
  std::wstring marker_path = dir + L"\\" + kPermissionMarkerFile;
  HANDLE hFile = ::CreateFileW(
      marker_path.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM,
      nullptr);
  if (hFile != INVALID_HANDLE_VALUE) {
    // Write the current directory path (as wide string)
    // This will be compared on next startup to detect folder moves
    DWORD written;
    ::WriteFile(hFile, dir.c_str(), 
                static_cast<DWORD>(dir.length() * sizeof(wchar_t)), 
                &written, nullptr);
    ::CloseHandle(hFile);
  }
}

// Grant Read/Execute permissions to ALL APPLICATION PACKAGES using Windows API
// This is more reliable than calling icacls.exe
bool GrantAppContainerPermissions(const std::wstring& dir) {
  // ALL APPLICATION PACKAGES SID: S-1-15-2-1
  PSID pSid = nullptr;
  if (!::ConvertStringSidToSidW(L"S-1-15-2-1", &pSid)) {
    return false;
  }

  // Get current DACL
  PACL pOldDacl = nullptr;
  PSECURITY_DESCRIPTOR pSD = nullptr;
  DWORD result = ::GetNamedSecurityInfoW(
      dir.c_str(),
      SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION,
      nullptr, nullptr,
      &pOldDacl,
      nullptr,
      &pSD);
  
  if (result != ERROR_SUCCESS) {
    ::LocalFree(pSid);
    return false;
  }

  // Create new ACE for ALL APPLICATION PACKAGES
  // Grant: Read, Execute, List folder contents (for directories)
  EXPLICIT_ACCESSW ea = {0};
  ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
  ea.grfAccessMode = GRANT_ACCESS;
  ea.grfInheritance = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
  ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
  ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(pSid);

  // Merge with existing DACL
  PACL pNewDacl = nullptr;
  result = ::SetEntriesInAclW(1, &ea, pOldDacl, &pNewDacl);
  
  if (result != ERROR_SUCCESS) {
    ::LocalFree(pSid);
    ::LocalFree(pSD);
    return false;
  }

  // Apply new DACL to directory (with inheritance to children)
  result = ::SetNamedSecurityInfoW(
      const_cast<LPWSTR>(dir.c_str()),
      SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION,
      nullptr, nullptr,
      pNewDacl,
      nullptr);

  // If direct API fails, fallback to icacls (handles inheritance better)
  if (result != ERROR_SUCCESS) {
    // Build icacls command: icacls "dir" /grant *S-1-15-2-1:(OI)(CI)(RX) /T /Q
    std::wstring cmd = L"icacls \"" + dir + L"\" /grant *S-1-15-2-1:(OI)(CI)(RX) /T /Q";
    
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;  // Hidden window - no black CMD popup
    
    PROCESS_INFORMATION pi = {0};
    
    // Need to use cmd.exe to run icacls
    std::wstring full_cmd = L"cmd.exe /c " + cmd;
    std::vector<wchar_t> cmd_buffer(full_cmd.begin(), full_cmd.end());
    cmd_buffer.push_back(L'\0');
    
    if (::CreateProcessW(
            nullptr,
            cmd_buffer.data(),
            nullptr, nullptr,
            FALSE,
            CREATE_NO_WINDOW,  // No window at all
            nullptr, nullptr,
            &si, &pi)) {
      // Wait for completion (max 30 seconds)
      ::WaitForSingleObject(pi.hProcess, 30000);
      ::CloseHandle(pi.hProcess);
      ::CloseHandle(pi.hThread);
      result = ERROR_SUCCESS;
    }
  }

  // Cleanup
  ::LocalFree(pNewDacl);
  ::LocalFree(pSid);
  ::LocalFree(pSD);
  
  return result == ERROR_SUCCESS;
}

// Main entry point for sandbox permission fix
// Called once at browser startup, before sandbox initialization
void EnsureSandboxPermissions() {
  // Only run for browser process (not renderer, GPU, etc.)
  // Note: At this early stage, we check command line for --type= switch
  // since InitializeProcessType() hasn't been called yet
  std::wstring cmd_line_str(::GetCommandLineW() ? ::GetCommandLineW() : L"");
  if (cmd_line_str.find(L"--type=") != std::wstring::npos) {
    return;  // This is a child process, skip
  }

  std::wstring exe_dir = GetExeDirectory();
  if (exe_dir.empty()) {
    return;
  }

  // Check if we've already done this for this directory
  if (HasPermissionMarker(exe_dir)) {
    return;  // Already granted, skip
  }

  // Grant permissions to ALL APPLICATION PACKAGES
  if (GrantAppContainerPermissions(exe_dir)) {
    // Success! Create marker file to avoid running again
    CreatePermissionMarker(exe_dir);
  }
  // If failed, we'll try again next time (no marker created)
}

// =============================================================================
// END PORTABLE SANDBOX PERMISSION FIX
// =============================================================================

// Sets the current working directory for the process to the directory holding
// the executable if this is the browser process. This avoids leaking a handle
// to an arbitrary directory to child processes (e.g., the crashpad handler
// process) created before MainDllLoader changes the current working directory
// to the browser's version directory.
void SetCwdForBrowserProcess() {
  if (!::IsBrowserProcess())
    return;

  std::array<wchar_t, MAX_PATH + 1> buffer;
  buffer[0] = L'\0';
  DWORD length = ::GetModuleFileName(nullptr, &buffer[0], buffer.size());
  if (!length || length >= buffer.size())
    return;

  base::SetCurrentDirectory(
      base::FilePath(base::FilePath::StringViewType(&buffer[0], length))
          .DirName());
}

bool IsFastStartSwitch(const std::string& command_line_switch) {
  return command_line_switch == switches::kProfileDirectory;
}

bool ContainsNonFastStartFlag(const base::CommandLine& command_line) {
  const base::CommandLine::SwitchMap& switches = command_line.GetSwitches();
  if (switches.size() > 1)
    return true;
  for (base::CommandLine::SwitchMap::const_iterator it = switches.begin();
       it != switches.end(); ++it) {
    if (!IsFastStartSwitch(it->first))
      return true;
  }
  return false;
}

bool AttemptFastNotify(const base::CommandLine& command_line) {
  if (ContainsNonFastStartFlag(command_line))
    return false;

  base::FilePath user_data_dir;
  if (!chrome::GetDefaultUserDataDirectory(&user_data_dir))
    return false;
  policy::path_parser::CheckUserDataDirPolicy(&user_data_dir);

  HWND chrome = FindRunningChromeWindow(user_data_dir);
  if (!chrome)
    return false;
  return AttemptToNotifyRunningChrome(chrome) ==
         NotifyChromeResult::NOTIFY_SUCCESS;
}

// Returns true if the child process |command_line| contains a /prefetch:#
// argument where # is in [1, 8] prior to Win11 and [1,16] for it and later.
// The intent of the function is to ensure that all child processes have a
// /prefetch:N cmd line arg in the required range.
// No child process shall have /prefetch:0 or it will interefere with the main
// browser process prefetch. This includes things like /prefetch:simians where
// simians will evalate to 0. Absence of a /prefetch:N argument is the same as
// /prefetch:0 and is also excluded.
// The function assumes only one /prefetch:N argument for child processes.
bool HasValidWindowsPrefetchArgument(const base::CommandLine& command_line) {
  static constexpr std::wstring_view kPrefetchArgumentPrefix(L"/prefetch:");

  for (const auto& arg : command_line.argv()) {
    if (!base::StartsWith(arg, kPrefetchArgumentPrefix)) {
      continue;  // Ignore arguments that don't start with "/prefetch:".
    }
    auto value = std::wstring_view(arg).substr(kPrefetchArgumentPrefix.size());
    int profile = 0;
    return base::StringToInt(value, &profile) && profile >= 1 &&
           profile <=
               (base::win::GetVersion() < base::win::Version::WIN11 ? 8 : 16);
  }
  return false;
}

int RunFallbackCrashHandler(const base::CommandLine& cmd_line) {
  // Retrieve the product & version details we need to report the crash
  // correctly.
  wchar_t exe_file[MAX_PATH] = {};
  CHECK(::GetModuleFileName(nullptr, exe_file, std::size(exe_file)));

  std::wstring product_name, version, channel_name, special_build;
  install_static::GetExecutableVersionDetails(exe_file, &product_name, &version,
                                              &special_build, &channel_name);

  return crash_reporter::RunAsFallbackCrashHandler(
      cmd_line, base::WideToUTF8(product_name), base::WideToUTF8(version),
      base::WideToUTF8(channel_name));
}

// In 32-bit builds, the main thread starts with the default (small) stack size.
// The ARCH_CPU_32_BITS blocks here and below are in support of moving the main
// thread to a fiber with a larger stack size.
#if defined(ARCH_CPU_32_BITS)
// The information needed to transfer control to the large-stack fiber and later
// pass the main routine's exit code back to the small-stack fiber prior to
// termination.
struct FiberState {
  HINSTANCE instance;
  LPVOID original_fiber;
  int fiber_result;
};

// A PFIBER_START_ROUTINE function run on a large-stack fiber that calls the
// main routine, stores its return value, and returns control to the small-stack
// fiber. |params| must be a pointer to a FiberState struct.
void WINAPI FiberBinder(void* params) {
  auto* fiber_state = static_cast<FiberState*>(params);
  // Call the main routine from the fiber. Reusing the entry point minimizes
  // confusion when examining call stacks in crash reports - seeing wWinMain on
  // the stack is a handy hint that this is the main thread of the process.
#if !defined(WIN_CONSOLE_APP)
  fiber_state->fiber_result =
      wWinMain(fiber_state->instance, nullptr, nullptr, 0);
#else   // !defined(WIN_CONSOLE_APP)
  fiber_state->fiber_result = main();
#endif  // !defined(WIN_CONSOLE_APP)
  // Switch back to the main thread to exit.
  ::SwitchToFiber(fiber_state->original_fiber);
}
#endif  // defined(ARCH_CPU_32_BITS)

}  // namespace

__declspec(dllexport) __cdecl void GetPakFileHashes(
    const uint8_t** resources_pak,
    const uint8_t** chrome_100_pak,
    const uint8_t** chrome_200_pak) {
  *resources_pak = kSha256_resources_pak.data();
  *chrome_100_pak = kSha256_chrome_100_percent_pak.data();
  *chrome_200_pak = kSha256_chrome_200_percent_pak.data();
}

#if !defined(WIN_CONSOLE_APP)
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE prev, wchar_t*, int) {
#else   // !defined(WIN_CONSOLE_APP)
int main() {
  HINSTANCE instance = GetModuleHandle(nullptr);
#endif  // !defined(WIN_CONSOLE_APP)

#if defined(ARCH_CPU_32_BITS)
  enum class FiberStatus { kConvertFailed, kCreateFiberFailed, kSuccess };
  FiberStatus fiber_status = FiberStatus::kSuccess;
  // GetLastError result if fiber conversion failed.
  DWORD fiber_error = ERROR_SUCCESS;
  if (!::IsThreadAFiber()) {
    // Make the main thread's stack size 4 MiB so that it has roughly the same
    // effective size as the 64-bit build's 8 MiB stack.
    constexpr size_t kStackSize = 4 * 1024 * 1024;  // 4 MiB
    // Leak the fiber on exit.
    LPVOID original_fiber =
        ::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (original_fiber) {
      FiberState fiber_state = {instance, original_fiber};
      // Create a fiber with a bigger stack and switch to it. Leak the fiber on
      // exit.
      LPVOID big_stack_fiber = ::CreateFiberEx(
          0, kStackSize, FIBER_FLAG_FLOAT_SWITCH, FiberBinder, &fiber_state);
      if (big_stack_fiber) {
        ::SwitchToFiber(big_stack_fiber);
        // The fibers must be cleaned up to avoid obscure TLS-related shutdown
        // crashes.
        ::DeleteFiber(big_stack_fiber);
        ::ConvertFiberToThread();
        // Control returns here after Chrome has finished running on FiberMain.
        return fiber_state.fiber_result;
      }
      fiber_status = FiberStatus::kCreateFiberFailed;
    } else {
      fiber_status = FiberStatus::kConvertFailed;
    }
    // If we reach here then creating and switching to a fiber has failed. This
    // probably means we are low on memory and will soon crash. Try to report
    // this error once crash reporting is initialized.
    fiber_error = ::GetLastError();
    base::debug::Alias(&fiber_error);
  }
  // If we are already a fiber then continue normal execution.
#endif  // defined(ARCH_CPU_32_BITS)

  SetCwdForBrowserProcess();
  
  // PORTABLE SANDBOX FIX: Ensure sandbox permissions for portable deployments.
  // This grants Read/Execute permissions to "ALL APPLICATION PACKAGES" group
  // so that AppContainer sandbox can access the browser directory.
  EnsureSandboxPermissions();
  
  install_static::InitializeFromPrimaryModule();
  SignalInitializeCrashReporting();
  if (IsBrowserProcess())
    chrome::DisableDelayLoadFailureHooksForMainExecutable();
#if defined(ARCH_CPU_32_BITS)
  // Intentionally crash if converting to a fiber failed.
  CHECK_EQ(fiber_status, FiberStatus::kSuccess);
#endif  // defined(ARCH_CPU_32_BITS)

  // Done here to ensure that OOMs that happen early in process initialization
  // are correctly signaled to the OS.
  base::EnableTerminationOnOutOfMemory();
  logging::RegisterAbslAbortHook();

  // Initialize the CommandLine singleton from the environment.
  base::CommandLine::Init(0, nullptr);
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();

  const std::string process_type =
      command_line->GetSwitchValueASCII(switches::kProcessType);

#if !defined(COMPONENT_BUILD) && DCHECK_IS_ON()
  // In non-component mode, chrome.exe contains its own base::FeatureList
  // instance pointer, which remains nullptr. Attempts to access feature state
  // from chrome.exe should fail, instead of silently returning a default state.
  base::FeatureList::FailOnFeatureAccessWithoutFeatureList();

  // Patch the main EXE on non-component builds when DCHECKs are enabled.
  // This allows detection of third party code that might attempt to meddle with
  // Chrome's handles. This must be done when single-threaded to avoid other
  // threads attempting to make calls through the hooks while they are being
  // emplaced.
  // Note: The DLL is patched separately, in chrome/app/chrome_main.cc.
  base::debug::HandleHooks::AddIATPatch(CURRENT_MODULE());
#endif  // !defined(COMPONENT_BUILD) && DCHECK_IS_ON()

  // Confirm that an explicit prefetch profile is used for all process types
  // except for the browser process. Any new process type will have to assign
  // itself a prefetch id. See kPrefetchArgument* constants in
  // content_switches.cc for details.
  DCHECK(process_type.empty() ||
         HasValidWindowsPrefetchArgument(*command_line));

  if (process_type == crash_reporter::switches::kCrashpadHandler) {
    // Check if we should monitor the exit code of this process
    std::unique_ptr<ExitCodeWatcher> exit_code_watcher;

    crash_reporter::SetupFallbackCrashHandling(*command_line);
    // no-periodic-tasks is specified for self monitoring crashpad instances.
    // This is to ensure we are a crashpad process monitoring the browser
    // process and not another crashpad process.
    if (!command_line->HasSwitch("no-periodic-tasks")) {
      // Retrieve the client process from the command line
      crashpad::InitialClientData initial_client_data;
      if (initial_client_data.InitializeFromString(
              command_line->GetSwitchValueASCII("initial-client-data"))) {
        // Setup exit code watcher to monitor the parent process
        HANDLE duplicate_handle = INVALID_HANDLE_VALUE;
        if (DuplicateHandle(
                ::GetCurrentProcess(), initial_client_data.client_process(),
                ::GetCurrentProcess(), &duplicate_handle,
                PROCESS_QUERY_INFORMATION, FALSE, DUPLICATE_SAME_ACCESS)) {
          base::Process parent_process(duplicate_handle);
          exit_code_watcher = std::make_unique<ExitCodeWatcher>();
          if (exit_code_watcher->Initialize(std::move(parent_process))) {
            exit_code_watcher->StartWatching();
          }
        }
      }
    }

    // The handler process must always be passed the user data dir on the
    // command line.
    DCHECK(command_line->HasSwitch(switches::kUserDataDir));

    base::FilePath user_data_dir =
        command_line->GetSwitchValuePath(switches::kUserDataDir);
    int crashpad_status = crash_reporter::RunAsCrashpadHandler(
        *base::CommandLine::ForCurrentProcess(), user_data_dir,
        switches::kProcessType, switches::kUserDataDir);
    if (crashpad_status != 0 && exit_code_watcher) {
      // Crashpad failed to initialize, explicitly stop the exit code watcher
      // so the crashpad-handler process can exit with an error
      exit_code_watcher->StopWatching();
    }
    return crashpad_status;
  } else if (process_type == crash_reporter::switches::kFallbackCrashHandler) {
    return RunFallbackCrashHandler(*command_line);
  }

  const base::TimeTicks exe_entry_point_ticks = base::TimeTicks::Now();

  // Signal Chrome Elf that Chrome has begun to start.
  SignalChromeElf();

  // The exit manager is in charge of calling the dtors of singletons.
  base::AtExitManager exit_manager;

  if (AttemptFastNotify(*command_line))
    return 0;

  // Load and launch the chrome dll. *Everything* happens inside.
  VLOG(1) << "About to load main DLL.";
  MainDllLoader* loader = MakeMainDllLoader();
  int rc = loader->Launch(instance, exe_entry_point_ticks);
  loader->RelaunchChromeBrowserWithNewCommandLineIfNeeded();
  delete loader;

  // Process shutdown is hard and some process types have been crashing during
  // shutdown. TerminateCurrentProcessImmediately is safer and faster.
  if (process_type == switches::kUtilityProcess) {
    base::Process::TerminateCurrentProcessImmediately(rc);
  }
  return rc;
}
