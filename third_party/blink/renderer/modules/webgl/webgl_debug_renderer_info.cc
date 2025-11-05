/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/modules/webgl/webgl_debug_renderer_info.h"

#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "third_party/blink/renderer/modules/webgl/webgl_rendering_context_base.h"


namespace blink {

WebGLDebugRendererInfo::WebGLDebugRendererInfo(
    WebGLRenderingContextBase* context,
    ExecutionContext*)
    : WebGLExtension(context) {}

bool WebGLDebugRendererInfo::Supported(
    WebGLRenderingContextBase* context) {
  return true;
}

const char* WebGLDebugRendererInfo::ExtensionName() {
  return "WEBGL_debug_renderer_info";
}

WebGLExtensionName WebGLDebugRendererInfo::GetName() const {
  return kWebGLDebugRendererInfoName;
}

// ==================== WebGL Override Methods ====================

// Static method to get WebGL vendor override from command line
std::string WebGLDebugRendererInfo::GetWebGLVendorOverride() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line && command_line->HasSwitch("webgl-vendor")) {
    return command_line->GetSwitchValueASCII("webgl-vendor");
  }
  return std::string();
}

// Static method to get WebGL renderer override from command line
std::string WebGLDebugRendererInfo::GetWebGLRendererOverride() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line && command_line->HasSwitch("webgl-renderer")) {
    return command_line->GetSwitchValueASCII("webgl-renderer");
  }
  return std::string();
}

// ==================== Canvas Override Methods ====================

// Static method to get Canvas vendor override from command line
std::string WebGLDebugRendererInfo::GetCanvasVendorOverride() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line && command_line->HasSwitch("canvas-vendor")) {
    return command_line->GetSwitchValueASCII("canvas-vendor");
  }
  return std::string();
}

// Static method to get Canvas renderer override from command line
std::string WebGLDebugRendererInfo::GetCanvasRendererOverride() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line && command_line->HasSwitch("canvas-renderer")) {
    return command_line->GetSwitchValueASCII("canvas-renderer");
  }
  return std::string();
}

// Static method to get Canvas noise flag from command line
std::string WebGLDebugRendererInfo::GetCanvasNoiseOverride() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line && command_line->HasSwitch("canvas-noise")) {
    return command_line->GetSwitchValueASCII("canvas-noise");
  }
  return std::string();
}

// Static method to get Canvas seed from command line
std::string WebGLDebugRendererInfo::GetCanvasSeedOverride() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line && command_line->HasSwitch("canvas-seed")) {
    return command_line->GetSwitchValueASCII("canvas-seed");
  }
  return std::string();
}

// ==================== Fingerprinting Protection Flags ====================

// Static method to check if Rects noise is enabled
bool WebGLDebugRendererInfo::GetRectsNoiseFlag() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  return command_line && command_line->HasSwitch("rects-noise");
}

// Static method to check if Audio noise is enabled
bool WebGLDebugRendererInfo::GetAudioNoiseFlag() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  return command_line && command_line->HasSwitch("audio-noise");
}

// Static method to check if Fonts noise is enabled
bool WebGLDebugRendererInfo::GetFontsNoiseFlag() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  return command_line && command_line->HasSwitch("fonts-noise");
}

}  // namespace blink
