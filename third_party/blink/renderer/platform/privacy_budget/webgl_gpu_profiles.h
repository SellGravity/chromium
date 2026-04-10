// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_WEBGL_GPU_PROFILES_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_WEBGL_GPU_PROFILES_H_

// Built-in GPU fingerprint profiles for WebGL parameter spoofing.
// When the user selects a GPU via --webgl-renderer or fingerprint_config.json,
// the browser auto-applies the correct WebGL parameter values for that GPU,
// so the manager app doesn't need to specify hundreds of parameters manually.
//
// Values are derived from the ANGLE D3D11 backend source code
// (renderer11_utils.cpp) for D3D_FEATURE_LEVEL_11_0 and 11_1.
// On Windows with Direct3D11, all modern NVIDIA/AMD/Intel GPUs report
// essentially the same capability values because D3D11 Feature Level 11.0
// is the limiting factor, not the actual GPU silicon.
//
// GL enum constants reference (decimal values):
//   GL_MAX_TEXTURE_SIZE            = 3379
//   GL_MAX_CUBE_MAP_TEXTURE_SIZE   = 34076
//   GL_MAX_RENDERBUFFER_SIZE       = 34024
//   GL_MAX_VIEWPORT_DIMS           = 3386  (int array[2])
//   GL_MAX_TEXTURE_IMAGE_UNITS     = 34930
//   GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 35661
//   GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS   = 35660
//   GL_MAX_VERTEX_ATTRIBS          = 34921
//   GL_MAX_VERTEX_UNIFORM_VECTORS  = 36347  (0x8DFB)
//   GL_MAX_VARYING_VECTORS          = 36348  (0x8DFC)
//   GL_MAX_FRAGMENT_UNIFORM_VECTORS = 36349  (0x8DFD)
//   GL_MAX_VERTEX_UNIFORM_COMPONENTS   = 35658  (= maxVertexUniformVectors * 4)
//   GL_MAX_FRAGMENT_UNIFORM_COMPONENTS = 35657  (= maxFragmentUniformVectors * 4)
//   GL_MAX_FRAGMENT_INPUT_COMPONENTS   = 37157
//   GL_MAX_VERTEX_OUTPUT_COMPONENTS    = 37154
//   GL_ALIASED_POINT_SIZE_RANGE    = 33901 (0x846D, float array[2])
//   GL_ALIASED_LINE_WIDTH_RANGE    = 33902 (0x846E, float array[2])
//   --- WebGL2 only ---
//   GL_MAX_3D_TEXTURE_SIZE         = 32883
//   GL_MAX_ARRAY_TEXTURE_LAYERS    = 35071
//   GL_MAX_DRAW_BUFFERS            = 34852
//   GL_MAX_COLOR_ATTACHMENTS       = 36063
//   GL_MAX_ELEMENT_INDEX           = 36203 (int64)
//   GL_MAX_ELEMENTS_INDICES        = 33001
//   GL_MAX_ELEMENTS_VERTICES       = 33000
//   GL_MAX_FRAGMENT_UNIFORM_BLOCKS = 35373
//   GL_MAX_VERTEX_UNIFORM_BLOCKS   = 35371
//   GL_MAX_COMBINED_UNIFORM_BLOCKS = 35374
//   GL_MAX_UNIFORM_BLOCK_SIZE      = 35376 (int64)
//   GL_MAX_UNIFORM_BUFFER_BINDINGS = 35375
//   GL_MAX_VARYING_COMPONENTS      = 35659
//   GL_MAX_SAMPLES                 = 36183
//   GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS = 35978
//   GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS       = 35979
//   GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS    = 35981
//   GL_MIN_PROGRAM_TEXEL_OFFSET    = 35076  (0x8904)
//   GL_MAX_PROGRAM_TEXEL_OFFSET    = 35077  (0x8905)
//   GL_MAX_SERVER_WAIT_TIMEOUT     = 36184 (int64)

#include <map>
#include <string>
#include <vector>

#include "base/no_destructor.h"
#include "third_party/blink/renderer/platform/allow_discouraged_type.h"

namespace blink {

struct WebGLGpuProfile {
  std::map<unsigned int, int> int_params
      ALLOW_DISCOURAGED_TYPE("GPU profile data loaded once at init");
  std::map<unsigned int, std::vector<float>> float_array_params
      ALLOW_DISCOURAGED_TYPE("GPU profile data loaded once at init");
  std::map<unsigned int, std::vector<int>> int_array_params
      ALLOW_DISCOURAGED_TYPE("GPU profile data loaded once at init");
      
  std::vector<std::string> webgl1_extensions
      ALLOW_DISCOURAGED_TYPE("GPU profile data loaded once at init");
  std::vector<std::string> webgl2_extensions
      ALLOW_DISCOURAGED_TYPE("GPU profile data loaded once at init");
};

// Returns a GPU profile matching the given renderer string.
// Matches by checking if the renderer string contains known GPU identifiers.
// Returns nullptr if no matching profile is found.
inline const WebGLGpuProfile* FindGpuProfile(const std::string& renderer) {
  // ===================================================================
  // D3D_FEATURE_LEVEL_11_0 / 11_1 profile (all modern GPUs on Windows)
  // This covers: ALL NVIDIA GeForce GTX 9xx+, RTX 20xx/30xx/40xx
  //              ALL AMD Radeon RX 4xx+, RX 5xxx/6xxx/7xxx
  //              ALL Intel HD/UHD 5xx+, Iris, Arc
  // On D3D11, the limits are determined by Feature Level, NOT the GPU.
  // ===================================================================
  //
  // Values from ANGLE source: renderer11_utils.cpp GenerateCaps()
  // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION = 16384
  // D3D11_REQ_TEXTURECUBE_DIMENSION = 16384
  // D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION = 2048
  // D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION = 2048
  // D3D11_VIEWPORT_BOUNDS_MAX = 32767
  // D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT = 8
  // D3D11_STANDARD_VERTEX_ELEMENT_COUNT = 32
  // D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT = 4096
  // D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT = 16
  // D3D11_VS_OUTPUT_REGISTER_COUNT = 32 (minus 2 reserved = 30)
  // D3D11_PS_INPUT_REGISTER_COUNT = 32 (minus 2 reserved = 30)
  // D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT = 14 (minus 3 reserved = 11)
  // PixelUniformVectors = 1024

  static const base::NoDestructor<WebGLGpuProfile> kD3D11Profile([] {
    WebGLGpuProfile p;

    // --- WebGL 1 parameters (NVIDIA Specific Dump) ---
    p.int_params[3379]  = 16384;   // GL_MAX_TEXTURE_SIZE
    p.int_params[34076] = 16384;   // GL_MAX_CUBE_MAP_TEXTURE_SIZE
    p.int_params[34024] = 16384;   // GL_MAX_RENDERBUFFER_SIZE
    p.int_params[34930] = 16;      // GL_MAX_TEXTURE_IMAGE_UNITS
    p.int_params[35660] = 16;      // GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
    p.int_params[35661] = 32;      // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
    p.int_params[34921] = 16;      // GL_MAX_VERTEX_ATTRIBS
    p.int_params[36347] = 4096;    // GL_MAX_VERTEX_UNIFORM_VECTORS
    p.int_params[36349] = 1024;    // GL_MAX_FRAGMENT_UNIFORM_VECTORS
    p.int_params[36348] = 30;      // GL_MAX_VARYING_VECTORS

    // --- WebGL 2 additional parameters (NVIDIA Specific Dump) ---
    p.int_params[32883] = 2048;    // GL_MAX_3D_TEXTURE_SIZE
    p.int_params[35071] = 2048;    // GL_MAX_ARRAY_TEXTURE_LAYERS
    p.int_params[34852] = 8;       // GL_MAX_DRAW_BUFFERS
    p.int_params[36063] = 8;       // GL_MAX_COLOR_ATTACHMENTS
    p.int_params[33001] = 1048576; // GL_MAX_ELEMENTS_INDICES
    p.int_params[33000] = 1048576; // GL_MAX_ELEMENTS_VERTICES
    p.int_params[36203] = -2;      // GL_MAX_ELEMENT_INDEX → uint32(-2) = 4294967294 (D3D11 standard)
    p.int_params[35657] = 4096;    // GL_MAX_FRAGMENT_UNIFORM_COMPONENTS
    p.int_params[35658] = 16384;   // GL_MAX_VERTEX_UNIFORM_COMPONENTS (4096*4)
    p.int_params[37157] = 120;     // GL_MAX_FRAGMENT_INPUT_COMPONENTS
    p.int_params[37154] = 120;     // GL_MAX_VERTEX_OUTPUT_COMPONENTS
    p.int_params[35373] = 12;      // GL_MAX_FRAGMENT_UNIFORM_BLOCKS
    p.int_params[35371] = 12;      // GL_MAX_VERTEX_UNIFORM_BLOCKS
    p.int_params[35374] = 24;      // GL_MAX_COMBINED_UNIFORM_BLOCKS
    p.int_params[35375] = 24;      // GL_MAX_UNIFORM_BUFFER_BINDINGS
    p.int_params[35376] = 65536;   // GL_MAX_UNIFORM_BLOCK_SIZE
    p.int_params[35659] = 120;     // GL_MAX_VARYING_COMPONENTS
    p.int_params[36183] = 8;       // GL_MAX_SAMPLES
    p.int_params[35978] = 120;     // GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS
    p.int_params[35979] = 4;       // GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS
    p.int_params[35968] = 4;       // GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS
    p.int_params[35076] = -8;      // GL_MIN_PROGRAM_TEXEL_OFFSET
    p.int_params[35077] = 7;       // GL_MAX_PROGRAM_TEXEL_OFFSET
    
    // Additional parameters required for strict hash matching
    p.int_params[34045] = 2;       // GL_MAX_TEXTURE_LOD_BIAS (0x84FD)
    p.int_params[35380] = 256;     // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT (0x8A3A)
    p.int_params[35377] = 212992;  // GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS (0x8A31) = 12*16384+16384
    p.int_params[35379] = 200704;  // GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS (0x8A33)
    p.int_params[34047] = 16;      // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT (0x84FF)

    // CreepJS-specific parameters (queried for capabilities hash)
    p.int_params[3408]  = 4;       // GL_SUBPIXEL_BITS (0x0D50) - always 4 on D3D11 
    p.int_params[36184] = 0;       // GL_MAX_SERVER_WAIT_TIMEOUT (0x8D6C) - 0 on D3D11
    p.int_params[37447] = 0;       // GL_MAX_CLIENT_WAIT_TIMEOUT_WEBGL (0x9247) - 0 on D3D11

    // --- Float array parameters ---
    p.float_array_params[33901] = {1.0f, 1024.0f};   // GL_ALIASED_POINT_SIZE_RANGE (0x846D)
    p.float_array_params[33902] = {1.0f, 1.0f};      // GL_ALIASED_LINE_WIDTH_RANGE (0x846E)

    // --- Int array parameters ---
    p.int_array_params[3386] = {32767, 32767};  // GL_MAX_VIEWPORT_DIMS (D3D11_VIEWPORT_BOUNDS_MAX)

    // --- Strict Extensions Array (Windows D3D11 Profile) ---
    p.webgl1_extensions = {
      "ANGLE_instanced_arrays", "EXT_blend_minmax", "EXT_clip_control",
      "EXT_color_buffer_half_float", "EXT_depth_clamp", "EXT_disjoint_timer_query",
      "EXT_float_blend", "EXT_frag_depth", "EXT_polygon_offset_clamp",
      "EXT_shader_texture_lod", "EXT_texture_compression_bptc",
      "EXT_texture_compression_rgtc", "EXT_texture_filter_anisotropic",
      "EXT_texture_mirror_clamp_to_edge", "EXT_sRGB", "KHR_parallel_shader_compile",
      "OES_element_index_uint", "OES_fbo_render_mipmap", "OES_standard_derivatives",
      "OES_texture_float", "OES_texture_float_linear", "OES_texture_half_float",
      "OES_texture_half_float_linear", "OES_vertex_array_object",
      "WEBGL_blend_func_extended", "WEBGL_color_buffer_float",
      "WEBGL_compressed_texture_s3tc", "WEBGL_compressed_texture_s3tc_srgb",
      "WEBGL_debug_renderer_info", "WEBGL_debug_shaders", "WEBGL_depth_texture",
      "WEBGL_draw_buffers", "WEBGL_lose_context", "WEBGL_multi_draw",
      "WEBGL_polygon_mode"
    };

    p.webgl2_extensions = {
      "EXT_clip_control", "EXT_color_buffer_float", "EXT_color_buffer_half_float",
      "EXT_conservative_depth", "EXT_depth_clamp", "EXT_disjoint_timer_query_webgl2",
      "EXT_float_blend", "EXT_polygon_offset_clamp", "EXT_render_snorm",
      "EXT_texture_compression_bptc", "EXT_texture_compression_rgtc",
      "EXT_texture_filter_anisotropic", "EXT_texture_mirror_clamp_to_edge",
      "EXT_texture_norm16", "KHR_parallel_shader_compile",
      "NV_shader_noperspective_interpolation", "OES_draw_buffers_indexed",
      "OES_sample_variables", "OES_shader_multisample_interpolation",
      "OES_texture_float_linear", "OVR_multiview2", "WEBGL_blend_func_extended",
      "WEBGL_clip_cull_distance", "WEBGL_compressed_texture_s3tc",
      "WEBGL_compressed_texture_s3tc_srgb", "WEBGL_debug_renderer_info",
      "WEBGL_debug_shaders", "WEBGL_lose_context", "WEBGL_multi_draw",
      "WEBGL_polygon_mode", "WEBGL_provoking_vertex", "WEBGL_stencil_texturing"
    };

    return p;
  }());

  // All modern GPUs on D3D11 use the same Feature Level 11.0 profile.
  // We match by checking if the renderer contains known GPU brand identifiers.
  // The renderer string format is:
  //   "ANGLE (NVIDIA, NVIDIA GeForce RTX 4070 Ti Direct3D11 vs_5_0 ps_5_0, D3D11)"
  //   "ANGLE (Intel, Intel(R) UHD Graphics 630 Direct3D11, D3D11)"
  //   "ANGLE (AMD, AMD Radeon RX 6800 XT Direct3D11, D3D11)"

  if (renderer.find("Direct3D11") != std::string::npos ||
      renderer.find("D3D11") != std::string::npos) {
    return kD3D11Profile.get();
  }

  // Match by GPU vendor even without D3D11 suffix
  if (renderer.find("NVIDIA") != std::string::npos ||
      renderer.find("Intel") != std::string::npos ||
      renderer.find("AMD") != std::string::npos ||
      renderer.find("Radeon") != std::string::npos) {
    return kD3D11Profile.get();
  }

  return nullptr;
}

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_PRIVACY_BUDGET_WEBGL_GPU_PROFILES_H_
