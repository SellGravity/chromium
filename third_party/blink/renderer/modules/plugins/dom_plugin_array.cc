/*
 *  Copyright (C) 2008 Nokia Corporation and/or its subsidiary(-ies)
 *  Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301 USA
 */

#include "third_party/blink/renderer/modules/plugins/dom_plugin_array.h"

#include "third_party/blink/public/common/features.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/navigator.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/page/plugin_data.h"
#include "third_party/blink/renderer/modules/plugins/dom_mime_type_array.h"
#include "third_party/blink/renderer/modules/plugins/navigator_plugins.h"
#include "third_party/blink/renderer/modules/webgl/webgl_debug_renderer_info.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

namespace {

// ========== ANTI-DETECTION: Standard Plugin Definitions ==========
// These are common plugins seen on real browsers
struct PluginDefinition {
  const char* name;
  const char* description;
  const char* filename;
};

// Standard plugins list - matches what real Chrome reports
static const PluginDefinition kStandardPlugins[] = {
    {"PDF Viewer", "Portable Document Format", "internal-pdf-viewer"},
    {"Chrome PDF Viewer", "Portable Document Format", "internal-pdf-viewer"},
    {"Chromium PDF Viewer", "Portable Document Format", "internal-pdf-viewer"},
    {"Microsoft Edge PDF Viewer", "Portable Document Format", "internal-pdf-viewer"},
    {"WebKit built-in PDF", "Portable Document Format", "internal-pdf-viewer"}
};

DOMPlugin* MakeFakePlugin(const PluginDefinition& def, LocalDOMWindow* window) {
  String plugin_name = String(def.name);
  String description = String(def.description);
  String filename = String(def.filename);
  
  auto* plugin_info =
      MakeGarbageCollected<PluginInfo>(plugin_name, filename, description,
                                       /*background_color=*/Color::kTransparent,
                                       /*may_use_external_handler=*/false);
  Vector<String> extensions{"pdf"};
  for (const char* mime_type : {"application/pdf", "text/pdf"}) {
    auto* mime_info = MakeGarbageCollected<MimeClassInfo>(
        mime_type, description, *plugin_info, extensions);
    plugin_info->AddMimeType(mime_info);
  }
  return MakeGarbageCollected<DOMPlugin>(window, *plugin_info);
}
}  // namespace

DOMPluginArray::DOMPluginArray(LocalDOMWindow* window) : window_(window) {
  if (IsPdfViewerAvailable()) {
    // ========== ANTI-DETECTION: Plugin Count Override ==========
    // Use --plugins-count=N to control how many plugins are reported (0-5)
    // This allows fingerprint variation between different browser profiles
    // 
    // Examples:
    //   --plugins-count=0  → navigator.plugins.length = 0
    //   --plugins-count=3  → navigator.plugins.length = 3 (first 3 plugins)
    //   --plugins-count=5  → navigator.plugins.length = 5 (all plugins, default)
    //
    // JavaScript test:
    //   console.log(navigator.plugins.length);     // → count
    //   console.log(navigator.plugins[0].name);    // → "PDF Viewer"
    //   console.log(navigator.plugins[0].filename); // → "internal-pdf-viewer"
    
    int plugins_count = WebGLDebugRendererInfo::GetPluginsCountOverride();
    
    // Create plugin objects up to the specified count
    for (int i = 0; i < plugins_count && i < 5; i++) {
      dom_plugins_.push_back(MakeFakePlugin(kStandardPlugins[i], window));
    }
  }
}

void DOMPluginArray::Trace(Visitor* visitor) const {
  visitor->Trace(window_);
  visitor->Trace(dom_plugins_);
  ScriptWrappable::Trace(visitor);
}

unsigned DOMPluginArray::length() const {
  return dom_plugins_.size();
}

DOMPlugin* DOMPluginArray::item(unsigned index) {
  if (index >= dom_plugins_.size()) {
    return nullptr;
  }
  return dom_plugins_[index].Get();
}

DOMPlugin* DOMPluginArray::namedItem(const AtomicString& property_name) {
  for (const auto& plugin : dom_plugins_) {
    if (plugin->name() == property_name) {
      return plugin.Get();
    }
  }
  return nullptr;
}

void DOMPluginArray::NamedPropertyEnumerator(Vector<String>& property_names,
                                             ExceptionState&) const {
  property_names.ReserveInitialCapacity(dom_plugins_.size());
  for (const auto& plugin : dom_plugins_) {
    property_names.UncheckedAppend(plugin->name());
  }
}

bool DOMPluginArray::NamedPropertyQuery(const AtomicString& property_name,
                                        ExceptionState& exception_state) const {
  Vector<String> properties;
  NamedPropertyEnumerator(properties, exception_state);
  return properties.Contains(property_name);
}

void DOMPluginArray::refresh(bool reload) {
  if (!window_) {
    return;
  }
  PluginData::RefreshBrowserSidePluginCache();
  if (PluginData* data = GetPluginData()) {
    data->ResetPluginData();
  }
  if (reload && window_->GetFrame()) {
    window_->GetFrame()->Reload(WebFrameLoadType::kReload);
  }
}

PluginData* DOMPluginArray::GetPluginData() const {
  return (window_ && window_->GetFrame()) ? window_->GetFrame()->GetPluginData()
                                          : nullptr;
}

HeapVector<Member<DOMMimeType>> DOMPluginArray::GetFixedMimeTypeArray() {
  HeapVector<Member<DOMMimeType>> mimetypes;
  if (dom_plugins_.empty())
    return mimetypes;
  DCHECK_EQ(dom_plugins_[0]->length(), 2u);
  mimetypes.push_back(dom_plugins_[0]->item(0));
  mimetypes.push_back(dom_plugins_[0]->item(1));
  return mimetypes;
}

bool DOMPluginArray::IsPdfViewerAvailable() {
  auto* data = GetPluginData();
  if (!data)
    return false;
  for (const Member<MimeClassInfo>& mime_info : data->Mimes()) {
    if (mime_info->Type() == "application/pdf")
      return true;
  }
  return false;
}

}  // namespace blink
