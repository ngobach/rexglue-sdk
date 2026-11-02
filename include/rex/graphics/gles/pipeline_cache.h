/**
 * @file        graphics/gles/pipeline_cache.h
 * @brief       GLES shader translation and program linking
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/string/buffer.h>

#include "constants.h"
#include "gl.h"
#include "shader.h"

namespace rex::graphics::gles {

using namespace ::rex::gles;

class GlesRenderTargetCache;

// A linked vertex + fragment program with the state needed to bind its combined
// sampler uniforms.
struct GlesProgram {
  struct SamplerBinding {
    uint32_t fetch_constant = 0;
    xenos::FetchOpDimension dimension = xenos::FetchOpDimension::k2D;
    bool is_signed = false;
    xenos::TextureFilter mag_filter = xenos::TextureFilter::kUseFetchConst;
    xenos::TextureFilter min_filter = xenos::TextureFilter::kUseFetchConst;
    xenos::TextureFilter mip_filter = xenos::TextureFilter::kUseFetchConst;
    xenos::AnisoFilter aniso_filter = xenos::AnisoFilter::kUseFetchConst;
    int32_t uniform_location = -1;
    bool is_vertex = false;
  };

  GLuint program = 0;
  std::vector<SamplerBinding> samplers;
  struct VertexMemorySamplerBinding {
    uint32_t fetch_constant = 0;  // UINT32_MAX for the index buffer.
    bool is_index = false;
    // The vertex stage reads it from a plain uniform array instead of a texture
    // (see SpirvShaderTranslator::Features::vertex_memory_uniforms).
    bool uniform_array = false;
    int32_t uniform_location = -1;
  };
  std::vector<VertexMemorySamplerBinding> vertex_memory_samplers;
  struct StageUniforms {
    // System constant member locations, indexed like kSystemConstantUniforms.
    int32_t system[kSystemConstantUniformCount];
    StageUniforms() {
      for (int32_t& location : system) {
        location = -1;
      }
    }
    int32_t float_constants = -1;
    uint32_t float_constant_count = 0;
    int32_t bool_constants = -1;
    int32_t loop_constants = -1;
    int32_t fetch_constants = -1;
  };
  // 0 = vertex, 1 = pixel.
  StageUniforms stage_uniforms[2];
  int32_t debug_sampler_location = -1;
  uint32_t vertex_texture_count = 0;
  uint32_t vertex_sampler_count = 0;
  uint32_t pixel_texture_count = 0;
  uint32_t pixel_sampler_count = 0;
};

class GlesPipelineCache {
 public:
  GlesPipelineCache(const RegisterFile& register_file, GlesRenderTargetCache& render_target_cache,
                    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y);
  ~GlesPipelineCache();

  bool Initialize();
  void Shutdown();

  GlesShader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                         uint32_t dword_count);
  void AnalyzeShaderUcode(Shader& shader) { shader.AnalyzeUcode(ucode_disasm_buffer_); }

  SpirvShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, bool ps_param_gen_used) const;
  SpirvShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control) const;

  bool EnsureShadersTranslated(GlesShader::GlesTranslation* vertex_shader,
                               GlesShader::GlesTranslation* pixel_shader);

  // Returns the program for the shader pair, linking it on first use. A null
  // pixel shader selects the depth-only fragment shader.
  const GlesProgram* GetProgram(GlesShader::GlesTranslation* vertex_shader,
                                GlesShader::GlesTranslation* pixel_shader);
  // Debug: links the guest vertex shader with a solid-color fragment shader.
  const GlesProgram* GetDebugColorProgram(GlesShader::GlesTranslation* vertex_shader,
                                          GlesShader::GlesTranslation* pixel_shader = nullptr);

  uint32_t shared_memory_storage_buffer_count_log2() const {
    return shader_translator_->GetSharedMemoryStorageBufferCountLog2();
  }
  uint32_t max_storage_buffer_range() const { return features_.max_storage_buffer_range; }
  // True when the host has no shader storage buffers (OpenGL ES 3.0) and vertex
  // memory is uploaded to integer textures per draw instead.
  bool uses_texture_memory() const { return uses_texture_memory_; }

 private:
  const GlesProgram* CreateProgram(GlesShader::GlesTranslation* vertex_shader,
                                   GlesShader::GlesTranslation* pixel_shader);
  bool TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                               GlesShader::GlesTranslation& translation);

  const RegisterFile& register_file_;
  GlesRenderTargetCache& render_target_cache_;
  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;

  SpirvShaderTranslator::Features features_;
  std::unique_ptr<SpirvShaderTranslator> shader_translator_;
  string::StringBuffer ucode_disasm_buffer_;

  std::unordered_map<uint64_t, GlesShader*> shaders_;
  std::map<std::pair<const void*, const void*>, std::unique_ptr<GlesProgram>> programs_;

  GLuint depth_only_fragment_shader_ = 0;
  GLuint debug_color_fragment_shader_ = 0;
  std::map<const void*, std::unique_ptr<GlesProgram>> debug_color_programs_;
  const GlesProgram* depth_only_program_ = nullptr;
  bool uses_texture_memory_ = false;
};

}  // namespace rex::graphics::gles
