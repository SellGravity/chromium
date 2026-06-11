// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/capture/video/create_video_capture_device_factory.h"

#include "base/command_line.h"
#include "base/notimplemented.h"
#include "base/system/sys_info.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "media/base/media_switches.h"
#include "media/capture/video/fake_video_capture_device_factory.h"
#include "media/capture/video/file_video_capture_device_factory.h"

#if BUILDFLAG(IS_LINUX)
#include "media/capture/video/linux/video_capture_device_factory_linux.h"
#elif BUILDFLAG(IS_CHROMEOS)
#include "media/capture/video/chromeos/public/cros_features.h"
#include "media/capture/video/chromeos/video_capture_device_factory_chromeos.h"
#include "media/capture/video/linux/video_capture_device_factory_linux.h"
#elif BUILDFLAG(IS_WIN)
#include "media/capture/video/win/video_capture_device_factory_win.h"
#elif BUILDFLAG(IS_APPLE)
#include "media/capture/video/apple/video_capture_device_factory_apple.h"
#elif BUILDFLAG(IS_ANDROID)
#include "media/capture/video/android/video_capture_device_factory_android.h"
#elif BUILDFLAG(IS_FUCHSIA)
#include "media/capture/video/fuchsia/video_capture_device_factory_fuchsia.h"
#endif

namespace media {

namespace {

std::unique_ptr<VideoCaptureDeviceFactory>
CreateFakeVideoCaptureDeviceFactory() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kUseFileForFakeVideoCapture)) {
    return std::make_unique<FileVideoCaptureDeviceFactory>();
  } else {
    std::string fake_options = command_line->GetSwitchValueASCII(
        switches::kUseFakeDeviceForMediaStream);
    if (command_line->HasSwitch("media-device-count")) {
      std::string count_str = command_line->GetSwitchValueASCII("media-device-count");
      // Format is A,B,C where C is the number of webcams.
      std::string video_count = "1";
      size_t pos1 = count_str.find(',');
      if (pos1 != std::string::npos) {
        size_t pos2 = count_str.find(',', pos1 + 1);
        if (pos2 != std::string::npos) {
          video_count = count_str.substr(pos2 + 1);
        }
      } else if (!count_str.empty()) {
        video_count = count_str; // Fallback if only 1 number is passed
      }

      if (!fake_options.empty()) fake_options += ",";
      fake_options += "device-count=" + video_count;
    }

    std::vector<FakeVideoCaptureDeviceSettings> config;
    FakeVideoCaptureDeviceFactory::ParseFakeDevicesConfigFromOptionsString(
        fake_options, &config);
    auto result = std::make_unique<FakeVideoCaptureDeviceFactory>();
    result->SetToCustomDevicesConfig(config);
    return std::move(result);
  }
}

std::unique_ptr<VideoCaptureDeviceFactory>
CreatePlatformSpecificVideoCaptureDeviceFactory(
    scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner) {
#if BUILDFLAG(IS_LINUX)
  return std::make_unique<VideoCaptureDeviceFactoryLinux>(ui_task_runner);
#elif BUILDFLAG(IS_CHROMEOS)
  if (base::SysInfo::IsRunningOnChromeOS())
    return std::make_unique<VideoCaptureDeviceFactoryChromeOS>(ui_task_runner);
  return std::make_unique<VideoCaptureDeviceFactoryLinux>(ui_task_runner);
#elif BUILDFLAG(IS_WIN)
  return std::make_unique<VideoCaptureDeviceFactoryWin>();
#elif BUILDFLAG(IS_APPLE)
#if BUILDFLAG(IS_IOS_TVOS)
  return CreateFakeVideoCaptureDeviceFactory();
#else
  return std::make_unique<VideoCaptureDeviceFactoryApple>();
#endif  // BUILDFLAG(IS_IOS_TVOS)
#elif BUILDFLAG(IS_ANDROID)
  return std::make_unique<VideoCaptureDeviceFactoryAndroid>();
#elif BUILDFLAG(IS_FUCHSIA)
  return std::make_unique<VideoCaptureDeviceFactoryFuchsia>();
#elif BUILDFLAG(IS_IOS)
  return CreateFakeVideoCaptureDeviceFactory();
#else
  NOTIMPLEMENTED();
  return nullptr;
#endif
}

}  // anonymous namespace

bool ShouldUseFakeVideoCaptureDeviceFactory() {
  // FAKE DEVICE INJECTION (REPLACE METHOD)
  // Always use FakeVideoCaptureDeviceFactory to spoof video stream.
  return true;
}

std::unique_ptr<VideoCaptureDeviceFactory> CreateVideoCaptureDeviceFactory(
    scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner) {
  if (ShouldUseFakeVideoCaptureDeviceFactory()) {
    return CreateFakeVideoCaptureDeviceFactory();
  } else {
    // |ui_task_runner| is needed for the Linux ChromeOS factory to retrieve
    // screen rotations.
    return CreatePlatformSpecificVideoCaptureDeviceFactory(ui_task_runner);
  }
}

}  // namespace media
