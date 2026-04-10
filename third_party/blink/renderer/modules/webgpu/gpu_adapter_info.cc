// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webgpu/gpu_adapter_info.h"

#include "base/command_line.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_memory_heap_info.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_subgroup_matrix_config.h"

namespace blink {

GPUAdapterInfo::GPUAdapterInfo(const String& vendor,
                               const String& architecture,
                               uint32_t subgroup_min_size,
                               uint32_t subgroup_max_size,
                               bool is_fallback_adapter,
                               const String& device,
                               const String& description,
                               const String& driver,
                               const String& backend,
                               const String& type,
                               const std::optional<uint32_t> d3d_shader_model,
                               const std::optional<uint32_t> vk_driver_version,
                               const String& power_preference)
    : vendor_(vendor),
      architecture_(architecture),
      subgroup_min_size_(subgroup_min_size),
      subgroup_max_size_(subgroup_max_size),
      is_fallback_adapter_(is_fallback_adapter),
      device_(device),
      description_(description),
      driver_(driver),
      backend_(backend),
      type_(type),
      d3d_shader_model_(d3d_shader_model),
      vk_driver_version_(vk_driver_version),
      power_preference_(power_preference) {}

void GPUAdapterInfo::AppendMemoryHeapInfo(GPUMemoryHeapInfo* info) {
  memory_heaps_.push_back(info);
}

void GPUAdapterInfo::AppendSubgroupMatrixConfig(
    GPUSubgroupMatrixConfig* config) {
  subgroup_matrix_configs_.push_back(config);
}

// Helper: get WebGPU-specific vendor override (--webgpu-vendor flag)
static std::string GetWebGPUVendorFlag() {
  auto* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd && cmd->HasSwitch("webgpu-vendor")) {
    return cmd->GetSwitchValueASCII("webgpu-vendor");
  }
  return "";
}

// Helper: get WebGPU-specific renderer override (--webgpu-renderer flag)
static std::string GetWebGPURendererFlag() {
  auto* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd && cmd->HasSwitch("webgpu-renderer")) {
    return cmd->GetSwitchValueASCII("webgpu-renderer");
  }
  return "";
}

static std::string ExtractWebGPUDeviceName(const std::string& angle_renderer) {
  size_t first_comma = angle_renderer.find(',');
  if (first_comma != std::string::npos) {
    size_t start = angle_renderer.find_first_not_of(" ", first_comma + 1);
    if (start != std::string::npos) {
      size_t target_end = angle_renderer.find(" Direct3D", start);
      if (target_end == std::string::npos) {
        target_end = angle_renderer.find(" Vulkan", start);
      }
      if (target_end == std::string::npos) {
        target_end = angle_renderer.find(" OpenGL", start);
      }
      if (target_end != std::string::npos) {
        return angle_renderer.substr(start, target_end - start);
      }
    }
  }
  return angle_renderer;
}

static std::string ExtractWebGPUVendorName(const std::string& vendor_override) {
  std::string v = vendor_override;
  for (char& c : v) c = std::tolower((unsigned char)c);
  if (v.find("nvidia") != std::string::npos) return "nvidia";
  if (v.find("amd") != std::string::npos) return "amd";
  if (v.find("intel") != std::string::npos) return "intel";
  if (v.find("apple") != std::string::npos) return "apple";
  return vendor_override;
}

const String& GPUAdapterInfo::vendor() const {
  std::string vendor_override = GetWebGPUVendorFlag();
  if (!vendor_override.empty()) {
    std::string clean_vendor = ExtractWebGPUVendorName(vendor_override);
    DEFINE_STATIC_LOCAL(String, spoofed_vendor, (String(clean_vendor.c_str())));
    return spoofed_vendor;
  }
  return vendor_;
}

const String& GPUAdapterInfo::architecture() const {
  std::string vendor_override = GetWebGPUVendorFlag();
  if (!vendor_override.empty()) {
    DEFINE_STATIC_LOCAL(String, spoofed_arch, (""));
    return spoofed_arch;
  }
  return architecture_;
}

const String& GPUAdapterInfo::device() const {
  std::string renderer_override = GetWebGPURendererFlag();
  if (!renderer_override.empty()) {
    std::string raw_device = ExtractWebGPUDeviceName(renderer_override);
    // Strip (0xXXXX) from device if it exists
    size_t hex_pos = raw_device.find("(0x");
    while (hex_pos != std::string::npos) {
      size_t hex_end = raw_device.find(")", hex_pos);
      if (hex_end == std::string::npos) break;
      
      size_t start_erase = hex_pos;
      while (start_erase > 0 && raw_device[start_erase - 1] == ' ') {
        start_erase--;
      }
      
      size_t end_erase = hex_end + 1;
      while (end_erase < raw_device.length() && raw_device[end_erase] == ' ') {
        end_erase++;
      }
      
      raw_device.erase(start_erase, end_erase - start_erase);
      if (start_erase > 0 && start_erase < raw_device.length()) {
         raw_device.insert(start_erase, " ");
      }
      
      hex_pos = raw_device.find("(0x");
    }
    DEFINE_STATIC_LOCAL(String, spoofed_device, (String(raw_device.c_str())));
    return spoofed_device;
  }
  return device_;
}

const String& GPUAdapterInfo::description() const {
  std::string vendor_override = GetWebGPUVendorFlag();
  if (!vendor_override.empty()) {
    DEFINE_STATIC_LOCAL(String, spoofed_desc, (""));
    return spoofed_desc;
  }
  return description_;
}

uint32_t GPUAdapterInfo::subgroupMinSize() const {
  return subgroup_min_size_;
}

uint32_t GPUAdapterInfo::subgroupMaxSize() const {
  return subgroup_max_size_;
}

bool GPUAdapterInfo::isFallbackAdapter() const {
  return is_fallback_adapter_;
}

const String& GPUAdapterInfo::driver() const {
  std::string vendor_override = GetWebGPUVendorFlag();
  if (!vendor_override.empty()) {
    std::string v = vendor_override;
    for (char& c : v) c = std::tolower((unsigned char)c);
    if (v.find("nvidia") != std::string::npos) {
      DEFINE_STATIC_LOCAL(String, nv_driver, ("536.23"));
      return nv_driver;
    } else if (v.find("amd") != std::string::npos) {
      DEFINE_STATIC_LOCAL(String, amd_driver, ("31.0.24002.92"));
      return amd_driver;
    } else if (v.find("intel") != std::string::npos) {
      DEFINE_STATIC_LOCAL(String, intel_driver, ("31.0.101.4502"));
      return intel_driver;
    }
    DEFINE_STATIC_LOCAL(String, generic_driver, ("1.0.0"));
    return generic_driver;
  }
  return driver_;
}

const String& GPUAdapterInfo::backend() const {
  std::string renderer_override = GetWebGPURendererFlag();
  if (!renderer_override.empty()) {
    // WebGPU on Windows ALWAYS uses D3D12, even when WebGL uses D3D11.
    // This is normal Chrome behavior — WebGL and WebGPU have different backends.
    if (renderer_override.find("D3D11") != std::string::npos ||
        renderer_override.find("D3D12") != std::string::npos) {
      DEFINE_STATIC_LOCAL(String, d3d12_backend, ("d3d12"));
      return d3d12_backend;
    } else if (renderer_override.find("Vulkan") != std::string::npos) {
      DEFINE_STATIC_LOCAL(String, vulkan_backend, ("vulkan"));
      return vulkan_backend;
    } else if (renderer_override.find("OpenGL") != std::string::npos) {
      DEFINE_STATIC_LOCAL(String, opengl_backend, ("opengl"));
      return opengl_backend;
    } else if (renderer_override.find("Metal") != std::string::npos) {
      DEFINE_STATIC_LOCAL(String, metal_backend, ("metal"));
      return metal_backend;
    }
    DEFINE_STATIC_LOCAL(String, default_backend, ("d3d12"));
    return default_backend;
  }
  return backend_;
}

const String& GPUAdapterInfo::type() const {
  std::string vendor_override = GetWebGPUVendorFlag();
  if (!vendor_override.empty()) {
    DEFINE_STATIC_LOCAL(String, spoofed_type, ("discrete GPU"));
    return spoofed_type;
  }
  return type_;
}

const HeapVector<Member<GPUMemoryHeapInfo>>& GPUAdapterInfo::memoryHeaps()
    const {
  return memory_heaps_;
}

const HeapVector<Member<GPUSubgroupMatrixConfig>>&
GPUAdapterInfo::subgroupMatrixConfigs() const {
  return subgroup_matrix_configs_;
}

const std::optional<uint32_t>& GPUAdapterInfo::d3dShaderModel() const {
  return d3d_shader_model_;
}

const std::optional<uint32_t>& GPUAdapterInfo::vkDriverVersion() const {
  return vk_driver_version_;
}

const String& GPUAdapterInfo::powerPreference() const {
  return power_preference_;
}

void GPUAdapterInfo::Trace(Visitor* visitor) const {
  visitor->Trace(memory_heaps_);
  visitor->Trace(subgroup_matrix_configs_);
  ScriptWrappable::Trace(visitor);
}

}  // namespace blink
