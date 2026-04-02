// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/taskbar/taskbar_decorator_win.h"

#include <objbase.h>

#include <shobjidl.h>

#include <wrl/client.h>

#include <memory>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/metrics/histogram_macros.h"
#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/win/scoped_gdi_object.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/avatar_menu.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_avatar_icon_util.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "skia/ext/font_utils.h"
#include "skia/ext/image_operations.h"
#include "skia/ext/legacy_display_globals.h"
#include "skia/ext/platform_canvas.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "third_party/skia/include/core/SkStream.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/win/icon_util.h"
#include "ui/views/win/hwnd_util.h"

namespace taskbar {

namespace {

constexpr int kOverlayIconSize = 16;

// Responsible for invoking TaskbarList::SetOverlayIcon(). The call to
// TaskbarList::SetOverlayIcon() runs a nested run loop that proves
// problematic when called on the UI thread. Additionally it seems the call may
// take a while to complete. For this reason we call it on a worker thread.
//
// Docs for TaskbarList::SetOverlayIcon() say it does nothing if the HWND is not
// valid.
void SetOverlayIcon(HWND hwnd,
                    std::unique_ptr<SkBitmap> bitmap,
                    const std::string& alt_text) {
  Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
  HRESULT result = ::CoCreateInstance(
      CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar));
  if (FAILED(result) || FAILED(taskbar->HrInit()))
    return;

  base::win::ScopedGDIObject<HICON> icon;
  if (bitmap) {
    DCHECK_GE(bitmap.get()->width(), bitmap.get()->height());

    // Maintain aspect ratio on resize, but prefer more square.
    // (We used to round down here, but rounding up produces nicer results.)
    const int resized_height = base::ClampCeil(
        kOverlayIconSize *
        (static_cast<float>(bitmap.get()->height()) / bitmap.get()->width()));

    DCHECK_GE(kOverlayIconSize, resized_height);
    // Since the target size is so small, we use our best resizer.
    SkBitmap sk_icon = skia::ImageOperations::Resize(
        *bitmap.get(), skia::ImageOperations::RESIZE_LANCZOS3, kOverlayIconSize,
        resized_height);

    // Paint the resized icon onto a 16x16 canvas otherwise Windows will badly
    // hammer it to 16x16. We'll use a circular clip to be consistent with the
    // way profile icons are rendered in the profile switcher.
    SkBitmap offscreen_bitmap;
    offscreen_bitmap.allocN32Pixels(kOverlayIconSize, kOverlayIconSize);
    SkCanvas offscreen_canvas(offscreen_bitmap, SkSurfaceProps{});
    offscreen_canvas.clear(SK_ColorTRANSPARENT);

    static const SkRRect overlay_icon_clip =
        SkRRect::MakeOval(SkRect::MakeWH(kOverlayIconSize, kOverlayIconSize));
    offscreen_canvas.clipRRect(overlay_icon_clip, true);

    // Note: the original code used kOverlayIconSize - resized_height, but in
    // order to center the icon in the circle clip area, we're going to center
    // it in the paintable region instead, rounding up to the closest pixel to
    // avoid smearing.
    const int y_offset = std::ceilf((kOverlayIconSize - resized_height) / 2.0f);
    offscreen_canvas.drawImage(sk_icon.asImage(), 0, y_offset);

    icon = IconUtil::CreateHICONFromSkBitmap(offscreen_bitmap);
    if (!icon.is_valid())
      return;
  }
  taskbar->SetOverlayIcon(hwnd, icon.get(), base::UTF8ToWide(alt_text).c_str());
}

void PostSetOverlayIcon(HWND hwnd,
                        std::unique_ptr<SkBitmap> bitmap,
                        const std::string& alt_text) {
  base::ThreadPool::CreateCOMSTATaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE})
      ->PostTask(FROM_HERE, base::BindOnce(&SetOverlayIcon, hwnd,
                                           std::move(bitmap), alt_text));
}

}  // namespace

void DrawTaskbarDecorationString(gfx::NativeWindow window,
                                 const std::string& content,
                                 const std::string& alt_text) {
  HWND hwnd = views::HWNDForNativeWindow(window);

  // GraBrowser: Premium blue badge design
  constexpr SkColor kBadgeColor = SkColorSetRGB(0x1A, 0x73, 0xE8);  // Google Blue
  constexpr SkColor kBorderColor = SkColorSetRGB(0x42, 0xA5, 0xF5); // Light blue ring
  constexpr SkColor kForegroundColor = SK_ColorWHITE;
  constexpr int kRadius = kOverlayIconSize / 2;
  constexpr int kMinMargin = 3;
  constexpr int kMaxBounds = kOverlayIconSize - 2 * kMinMargin;
  constexpr int kMaxTextSize = 24;
  constexpr int kMinTextSize = 7;

  auto badge = std::make_unique<SkBitmap>();
  badge->allocN32Pixels(kOverlayIconSize, kOverlayIconSize);

  SkCanvas canvas(*badge.get(),
                  skia::LegacyDisplayGlobals::GetSkSurfaceProps());

  canvas.clear(SK_ColorTRANSPARENT);

  // Draw outer ring (light blue border)
  SkPaint ring_paint;
  ring_paint.setAntiAlias(true);
  ring_paint.setColor(kBorderColor);
  canvas.drawCircle(kRadius, kRadius, kRadius, ring_paint);

  // Draw inner filled circle (main blue)
  SkPaint fill_paint;
  fill_paint.setAntiAlias(true);
  fill_paint.setColor(kBadgeColor);
  canvas.drawCircle(kRadius, kRadius, kRadius - 1, fill_paint);

  // Draw text (white, bold)
  SkPaint text_paint;
  text_paint.setColor(kForegroundColor);

  SkFont font = skia::DefaultFont();
  font.setEdging(SkFont::Edging::kSubpixelAntiAlias);

  SkRect bounds;
  int text_size = kMaxTextSize;
  do {
    font.setSize(text_size--);
    font.measureText(content.c_str(), content.size(), SkTextEncoding::kUTF8,
                     &bounds);
  } while (text_size >= kMinTextSize &&
           (bounds.width() > kMaxBounds || bounds.height() > kMaxBounds));

  canvas.drawSimpleText(content.c_str(), content.size(), SkTextEncoding::kUTF8,
                        kRadius - bounds.width() / 2 - bounds.x(),
                        kRadius - bounds.height() / 2 - bounds.y(), font,
                        text_paint);

  PostSetOverlayIcon(hwnd, std::move(badge), alt_text);
}

void DrawTaskbarDecoration(gfx::NativeWindow window, const gfx::Image* image) {
  HWND hwnd = views::HWNDForNativeWindow(window);

  // SetOverlayIcon() does nothing if the window is not visible so testing here
  // avoids all the wasted effort of the image resizing.
  if (!::IsWindowVisible(hwnd))
    return;

  // Copy the image since we're going to use it on a separate thread and
  // gfx::Image isn't thread safe.
  std::unique_ptr<SkBitmap> bitmap;
  if (image) {
    // If `image` is an old avatar, then it's guaranteed to by 2x by code in
    // ProfileAttributesEntry::GetAvatarIcon().
    bitmap = std::make_unique<SkBitmap>(
        profiles::GetWin2xAvatarIconAsSquare(*image->ToSkBitmap()));
  }

  PostSetOverlayIcon(hwnd, std::move(bitmap), "");
}

void UpdateTaskbarDecoration(Profile* profile, gfx::NativeWindow window) {
  // Custom taskbar icon text: --taskbar-title=ABC draws "ABC" on the icon
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line && command_line->HasSwitch("taskbar-title")) {
    std::string title = command_line->GetSwitchValueASCII("taskbar-title");
    if (!title.empty()) {
      taskbar::DrawTaskbarDecorationString(window, title, title);
      return;
    }
  }

  // Auto-increment: if no --taskbar-title but --user-data-dir is set,
  // read and increment open_count from file, display as badge
  if (command_line && command_line->HasSwitch("user-data-dir")) {
    static bool s_open_count_initialized = false;
    static base::NoDestructor<std::string> s_open_count_str;

    if (!s_open_count_initialized) {
      s_open_count_initialized = true;
      std::string user_data_dir =
          command_line->GetSwitchValueASCII("user-data-dir");
      if (!user_data_dir.empty()) {
        base::FilePath count_file =
            base::FilePath::FromUTF8Unsafe(user_data_dir)
                .Append(FILE_PATH_LITERAL("open_count"));
        int count = 0;
        std::string count_content;
        if (base::ReadFileToString(count_file, &count_content)) {
          base::StringToInt(count_content, &count);
        }
        count++;
        // Save real count to file
        base::WriteFile(count_file, base::NumberToString(count));
        // Cap display at "99+" for badge readability (16x16 icon)
        *s_open_count_str = (count > 99) ? "99+" : base::NumberToString(count);
      }
    }

    if (!s_open_count_str->empty()) {
      taskbar::DrawTaskbarDecorationString(window, *s_open_count_str,
                                           *s_open_count_str);
      return;
    }
  }

  if (profile->IsGuestSession() ||
      // Browser process and profile manager may be null in tests.
      (g_browser_process && g_browser_process->profile_manager() &&
       g_browser_process->profile_manager()
               ->GetProfileAttributesStorage()
               .GetNumberOfProfiles() <= 1)) {
    taskbar::DrawTaskbarDecoration(window, nullptr);
    return;
  }

  // We need to draw the taskbar decoration. Even though we have an icon on the
  // window's relaunch details, we draw over it because the user may have
  // pinned the badge-less Chrome shortcut which will cause Windows to ignore
  // the relaunch details.
  // TODO(calamity): ideally this should not be necessary but due to issues
  // with the default shortcut being pinned, we add the runtime badge for
  // safety. See crbug.com/313800.
  gfx::Image decoration;
  AvatarMenu::ImageLoadStatus status = AvatarMenu::GetImageForMenuButton(
      profile->GetPath(), &decoration, kOverlayIconSize);

  // If the user is using a Gaia picture and the picture is still being loaded,
  // wait until the load finishes. This taskbar decoration will be triggered
  // again upon the finish of the picture load.
  if (status == AvatarMenu::ImageLoadStatus::LOADING ||
      status == AvatarMenu::ImageLoadStatus::PROFILE_DELETED ||
      status == AvatarMenu::ImageLoadStatus::BROWSER_SHUTTING_DOWN) {
    return;
  }

  taskbar::DrawTaskbarDecoration(window, &decoration);
}

}  // namespace taskbar
