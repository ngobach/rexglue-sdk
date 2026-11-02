/**
 * @file        graphics/gles/pipeline_cache.cpp
 * @brief       GLES shader translation and program linking
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/gles/gl.h>
#include <rex/graphics/gles/pipeline_cache.h>
#include <rex/graphics/gles/render_target_cache.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>

#include <spirv.hpp>
#include <spirv_glsl.hpp>
#include <xxhash.h>

namespace rex::graphics::gles {

namespace spvc = SPIRV_CROSS_NAMESPACE;

GlesPipelineCache::GlesPipelineCache(const RegisterFile& register_file,
                                     GlesRenderTargetCache& render_target_cache,
                                     uint32_t draw_resolution_scale_x,
                                     uint32_t draw_resolution_scale_y)
    : register_file_(register_file),
      render_target_cache_(render_target_cache),
      draw_resolution_scale_x_(draw_resolution_scale_x),
      draw_resolution_scale_y_(draw_resolution_scale_y),
      features_(false),
      shader_translator_(nullptr) {}

GlesPipelineCache::~GlesPipelineCache() { Shutdown(); }

bool GlesPipelineCache::Initialize() {
  GLint max_storage_block_size = 0;
  GetFunctions().GetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_storage_block_size);
  // On OpenGL ES 3.0 the storage buffer enums are invalid, leaving the value
  // untouched (0) and setting GL_INVALID_ENUM.
  while (GetFunctions().GetError() != GL_NO_ERROR) {
  }
  uses_texture_memory_ = max_storage_block_size <= 0;
  GLint max_combined_texture_units = 0;
  GetFunctions().GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_combined_texture_units);
  GLint max_vertex_texture_units = 0;
  GetFunctions().GetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &max_vertex_texture_units);
  REXGPU_INFO("GLES: {} combined texture image units, {} vertex texture image units",
              max_combined_texture_units, max_vertex_texture_units);

  if (uses_texture_memory_) {
    REXGPU_INFO(
        "GLES pipeline cache: no shader storage buffers; using vertex memory textures "
        "(GLSL ES 3.00)");
    features_.spirv_version = spv::Spv_1_0;
    features_.max_storage_buffer_range = 128 * 1024 * 1024;
    features_.shared_memory_via_textures = true;
    // ANGLE's Metal backend returns 0 for every vertex-stage texture read, so
    // the vertex stage takes its memory from a plain uniform array instead.
    features_.vertex_memory_uniforms = true;
    features_.derivative_control = false;
  } else {
    features_.spirv_version = spv::Spv_1_3;
    features_.max_storage_buffer_range =
        std::min(uint32_t(max_storage_block_size), uint32_t(SharedMemory::kBufferSize));
  }
  features_.full_draw_index_uint32 = true;
  features_.vertex_pipeline_stores_and_atomics = false;
  features_.fragment_stores_and_atomics = false;
  features_.clip_distance = false;
  features_.cull_distance = false;
  // The backend applies the guest swizzle with GL_TEXTURE_SWIZZLE_*.
  features_.image_view_format_swizzle = true;
  features_.signed_zero_inf_nan_preserve_float32 = false;
  features_.denorm_flush_to_zero_float32 = false;
  features_.rounding_mode_rte_float32 = false;
  features_.fragment_shader_sample_interlock = false;
  features_.demote_to_helper_invocation = false;
  features_.sample_rate_shading = false;

  shader_translator_ = std::make_unique<SpirvShaderTranslator>(
      features_, false, false, false, draw_resolution_scale_x_, draw_resolution_scale_y_);

  // A fragment shader without color outputs for depth-only draws.
  std::vector<uint8_t> depth_only_spirv;
  try {
    depth_only_spirv = shader_translator_->CreateDepthOnlyFragmentShader();
  } catch (const std::exception& exception) {
    REXGPU_ERROR("GLES pipeline cache: depth-only translation failed: {}", exception.what());
    return false;
  }
  if (depth_only_spirv.empty()) {
    REXGPU_ERROR("GLES pipeline cache: failed to create the depth-only fragment shader");
    return false;
  }
  std::string depth_only_glsl;
  try {
    spvc::CompilerGLSL compiler(reinterpret_cast<const uint32_t*>(depth_only_spirv.data()),
                                depth_only_spirv.size() / sizeof(uint32_t));
    spvc::CompilerGLSL::Options options;
    options.version = 300;
    options.es = true;
    options.emit_push_constant_as_uniform_buffer = false;
    options.enable_420pack_extension = false;
    options.emit_uniform_buffer_as_plain_uniforms = true;
    compiler.set_common_options(options);
    depth_only_glsl = compiler.compile();
  } catch (const std::exception& exception) {
    REXGPU_ERROR("GLES pipeline cache: SPIRV-Cross failed for the depth-only shader: {}",
                 exception.what());
    return false;
  }
  if (getenv("GLES_DEBUG_VS")) {
    const char* debug_fs_source =
        getenv("GLES_DEBUG_INTERPS")
            ? "#version 300 es\nprecision highp float;\nin highp vec4 xe_interpolator_0;\n"
              "in highp vec4 xe_interpolator_1;\nout vec4 o;\n"
              "void main() { o = vec4(xe_interpolator_0.xy, xe_interpolator_1.x, 1.0); }\n"
            : getenv("GLES_DEBUG_TEXPS")
            ? "#version 300 es\nprecision highp float;\nprecision highp sampler2DArray;\n"
              "uniform sampler2DArray xe_debug_tex;\nout vec4 o;\n"
              "void main() { o = texture(xe_debug_tex, vec3(0.5, 0.5, 0.0)); }\n"
            : "#version 300 es\nprecision mediump float;\nout vec4 o;\n"
              "void main() { o = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    CompileGlslEsShader(GL_FRAGMENT_SHADER, debug_fs_source, debug_color_fragment_shader_);
  }
  if (!CompileGlslEsShader(GL_FRAGMENT_SHADER, depth_only_glsl, depth_only_fragment_shader_)) {
    REXGPU_ERROR("GLES pipeline cache: failed to compile the depth-only fragment shader");
    return false;
  }

  REXGPU_INFO("GLES pipeline cache: shared memory max storage block {} MB ({} bindings)",
              features_.max_storage_buffer_range >> 20,
              shared_memory_storage_buffer_count_log2() >= 32
                  ? 0
                  : (1u << shared_memory_storage_buffer_count_log2()));
  return true;
}

void GlesPipelineCache::Shutdown() {
  for (const auto& program_pair : programs_) {
    if (program_pair.second->program) {
      GetFunctions().DeleteProgram(program_pair.second->program);
    }
  }
  programs_.clear();
  depth_only_program_ = nullptr;
  if (depth_only_fragment_shader_) {
    GetFunctions().DeleteShader(depth_only_fragment_shader_);
    depth_only_fragment_shader_ = 0;
  }
  for (const auto& shader_pair : shaders_) {
    delete shader_pair.second;
  }
  shaders_.clear();
  shader_translator_.reset();
}

GlesShader* GlesPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                          const uint32_t* host_address, uint32_t dword_count) {
  const uint64_t data_hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    return it->second;
  }
  GlesShader* shader = new GlesShader(shader_type, data_hash, host_address, dword_count,
                                     uses_texture_memory_);
  shaders_.emplace(data_hash, shader);
  return shader;
}

SpirvShaderTranslator::Modification GlesPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  SpirvShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));
  modification.vertex.interpolator_mask = interpolator_mask;
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    modification.vertex.tessellation_mode = uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode);
  } else {
    modification.vertex.tessellation_mode = 0;
  }
  // No clip/cull distance support, so user clip planes cannot be emulated.
  modification.vertex.user_clip_plane_count = 0;
  modification.vertex.user_clip_plane_cull = 0;
  modification.vertex.point_ps_ucp_mode = 0;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  }
  modification.vertex.vertex_kill_and = 0;
  (void)pa_cl_clip_cntl;
  return modification;
}

SpirvShaderTranslator::Modification GlesPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  SpirvShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                              regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }
  using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
  if (!uses_texture_memory_ && shader.implicit_early_z_write_allowed() &&
      (!shader.writes_color_target(0) ||
       !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    // early_fragment_tests requires GLSL ES 3.10.
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }
  (void)normalized_depth_control;
  return modification;
}

bool GlesPipelineCache::TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                                                GlesShader::GlesTranslation& translation) {
  if (!translator.TranslateAnalyzedShader(translation)) {
    REXGPU_ERROR("GLES pipeline cache: shader {:016X} translation failed",
                 translation.shader().ucode_data_hash());
    return false;
  }
  if (!translation.EnsureShaderObject()) {
    REXGPU_ERROR("GLES pipeline cache: shader {:016X} failed to compile to GLSL ES",
                 translation.shader().ucode_data_hash());
    return false;
  }
  return true;
}

bool GlesPipelineCache::EnsureShadersTranslated(GlesShader::GlesTranslation* vertex_shader,
                                                GlesShader::GlesTranslation* pixel_shader) {
  if (!vertex_shader->shader().is_ucode_analyzed()) {
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
  }
  if (!vertex_shader->is_translated()) {
    if (!TranslateAnalyzedShader(*shader_translator_, *vertex_shader)) {
      return false;
    }
  }
  if (!vertex_shader->is_valid()) {
    return false;
  }
  if (pixel_shader) {
    if (!pixel_shader->shader().is_ucode_analyzed()) {
      pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
    }
    if (!pixel_shader->is_translated()) {
      if (!TranslateAnalyzedShader(*shader_translator_, *pixel_shader)) {
        return false;
      }
    }
    if (!pixel_shader->is_valid()) {
      return false;
    }
  }
  return true;
}

const GlesProgram* GlesPipelineCache::CreateProgram(GlesShader::GlesTranslation* vertex_shader,
                                                    GlesShader::GlesTranslation* pixel_shader) {
  const bool depth_only = pixel_shader == nullptr;
  GLuint fragment_shader =
      depth_only ? depth_only_fragment_shader_ : pixel_shader->shader_object();
  if (!fragment_shader || !vertex_shader->shader_object()) {
    REXGPU_ERROR("GLES pipeline cache: missing stage objects (vs={}, fs={}, depth_only={})",
                 vertex_shader->shader_object(), fragment_shader, depth_only);
    return nullptr;
  }

  GLuint program_name = GetFunctions().CreateProgram();
  if (!program_name) {
    REXGPU_ERROR("GLES pipeline cache: glCreateProgram failed");
    return nullptr;
  }
  GetFunctions().AttachShader(program_name, vertex_shader->shader_object());
  GetFunctions().AttachShader(program_name, fragment_shader);
  GetFunctions().LinkProgram(program_name);
  GetFunctions().DetachShader(program_name, vertex_shader->shader_object());
  GetFunctions().DetachShader(program_name, fragment_shader);

  GLint link_status = 0;
  GetFunctions().GetProgramiv(program_name, GL_LINK_STATUS, &link_status);
  if (!link_status) {
    GLint log_length = 0;
    GetFunctions().GetProgramiv(program_name, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(size_t(log_length ? log_length : 1), '\0');
    GetFunctions().GetProgramInfoLog(program_name, GLsizei(log.size()), nullptr, log.data());
    REXGPU_ERROR("GLES pipeline cache: program link failed:\n{}", log);
    GetFunctions().DeleteProgram(program_name);
    return nullptr;
  }

  auto program = std::make_unique<GlesProgram>();
  program->program = program_name;

  // Constant uniforms are flattened into structs named per stage; resolve every
  // member location once per program.
  auto fill_stage_uniforms = [&](const char* suffix, bool is_vertex) {
    GlesProgram::StageUniforms& uniforms =
        program->stage_uniforms[is_vertex ? 0 : 1];
    for (uint32_t i = 0; i < kSystemConstantUniformCount; ++i) {
      std::string name = fmt::format("xe_uniform_system_constants_{}.{}", suffix,
                                     kSystemConstantUniforms[i].name);
      uniforms.system[i] = GetFunctions().GetUniformLocation(program_name, name.c_str());
    }
    uniforms.float_constants = GetFunctions().GetUniformLocation(
        program_name,
        fmt::format("xe_uniform_float_constants_{}.float_constants", suffix).c_str());
    uniforms.bool_constants = GetFunctions().GetUniformLocation(
        program_name,
        fmt::format("xe_uniform_bool_loop_constants_{}.bool_constants", suffix).c_str());
    uniforms.loop_constants = GetFunctions().GetUniformLocation(
        program_name,
        fmt::format("xe_uniform_bool_loop_constants_{}.loop_constants", suffix).c_str());
    uniforms.fetch_constants = GetFunctions().GetUniformLocation(
        program_name,
        fmt::format("xe_uniform_fetch_constants_{}.fetch_constants", suffix).c_str());
  };
  fill_stage_uniforms("vs", true);
  if (!depth_only) {
    fill_stage_uniforms("ps", false);
  }

  // Vertex memory textures (ES 3.0 fallback).
  for (const SpirvShader::SpirvTranslation::VertexMemorySampler& vertex_memory_sampler :
       vertex_shader->vertex_memory_samplers()) {
    GlesProgram::VertexMemorySamplerBinding& binding =
        program->vertex_memory_samplers.emplace_back();
    binding.fetch_constant = vertex_memory_sampler.fetch_constant;
    binding.is_index = vertex_memory_sampler.is_index;
    binding.uniform_array = vertex_memory_sampler.uniform_count != 0;
    binding.uniform_location =
        GetFunctions().GetUniformLocation(program_name, vertex_memory_sampler.name.c_str());
    REXGPU_INFO("GLES program {} vertex memory sampler {} (index={}) location={}", program_name,
                vertex_memory_sampler.name, vertex_memory_sampler.is_index,
                binding.uniform_location);
  }
  auto add_samplers = [&](GlesShader::GlesTranslation* translation, bool is_vertex) {
    const auto& texture_bindings = translation->translation_texture_bindings();
    const auto& sampler_bindings = translation->translation_sampler_bindings();
    for (const GlesShader::CombinedSampler& combined : translation->combined_samplers()) {
      GlesProgram::SamplerBinding& sampler = program->samplers.emplace_back();
      sampler.is_vertex = is_vertex;
      sampler.uniform_location =
          GetFunctions().GetUniformLocation(program_name, combined.name.c_str());
      if (combined.texture_binding < texture_bindings.size()) {
        const auto& texture = texture_bindings[combined.texture_binding];
        sampler.fetch_constant = texture.fetch_constant;
        sampler.dimension = texture.dimension;
        sampler.is_signed = texture.is_signed;
      }
      if (combined.sampler_binding >= texture_bindings.size() &&
          combined.sampler_binding - texture_bindings.size() < sampler_bindings.size()) {
        const auto& sampler_binding =
            sampler_bindings[combined.sampler_binding - texture_bindings.size()];
        sampler.mag_filter = sampler_binding.mag_filter;
        sampler.min_filter = sampler_binding.min_filter;
        sampler.mip_filter = sampler_binding.mip_filter;
        sampler.aniso_filter = sampler_binding.aniso_filter;
      }
    }
  };
  add_samplers(vertex_shader, true);
  if (!depth_only) {
    add_samplers(pixel_shader, false);
  }
  program->vertex_texture_count =
      uint32_t(static_cast<GlesShader&>(vertex_shader->shader())
                   .GetTextureBindingsAfterTranslation()
                   .size());
  program->vertex_sampler_count =
      uint32_t(static_cast<GlesShader&>(vertex_shader->shader())
                   .GetSamplerBindingsAfterTranslation()
                   .size());
  if (!depth_only) {
    program->pixel_texture_count =
        uint32_t(static_cast<GlesShader&>(pixel_shader->shader())
                     .GetTextureBindingsAfterTranslation()
                     .size());
    program->pixel_sampler_count =
        uint32_t(static_cast<GlesShader&>(pixel_shader->shader())
                     .GetSamplerBindingsAfterTranslation()
                     .size());
  }

  std::string pixel_shader_name =
      depth_only ? std::string("depth-only FS")
                 : fmt::format("PS {:016X}", pixel_shader->shader().ucode_data_hash());
  REXGPU_DEBUG(
      "GLES pipeline cache: linked program {} for VS {:016X} mod {:016X} + {} ({} samplers)",
      program_name, vertex_shader->shader().ucode_data_hash(), vertex_shader->modification(),
      pixel_shader_name, uint32_t(program->samplers.size()));

  const GlesProgram* result = program.get();
  programs_.emplace(std::make_pair(static_cast<const void*>(vertex_shader),
                                   static_cast<const void*>(pixel_shader)),
                    std::move(program));
  return result;
}

const GlesProgram* GlesPipelineCache::GetDebugColorProgram(
    GlesShader::GlesTranslation* vertex_shader, GlesShader::GlesTranslation* pixel_shader) {
  const void* cache_key = pixel_shader ? static_cast<const void*>(pixel_shader)
                                       : static_cast<const void*>(vertex_shader);
  auto it = debug_color_programs_.find(cache_key);
  if (it != debug_color_programs_.end()) {
    return it->second.get();
  }
  const GLuint fragment_shader =
      pixel_shader ? pixel_shader->shader_object() : debug_color_fragment_shader_;
  if (!fragment_shader || !vertex_shader->shader_object()) {
    return nullptr;
  }
  GLuint program_name = GetFunctions().CreateProgram();
  GetFunctions().AttachShader(program_name, vertex_shader->shader_object());
  GetFunctions().AttachShader(program_name, fragment_shader);
  GetFunctions().LinkProgram(program_name);
  GetFunctions().DetachShader(program_name, vertex_shader->shader_object());
  GetFunctions().DetachShader(program_name, fragment_shader);
  GLint link_status = 0;
  GetFunctions().GetProgramiv(program_name, GL_LINK_STATUS, &link_status);
  if (!link_status) {
    REXGPU_ERROR("GLES pipeline cache: debug color program link failed");
    GetFunctions().DeleteProgram(program_name);
    return nullptr;
  }
  auto program = std::make_unique<GlesProgram>();
  program->program = program_name;
  auto fill_vs = [&]() {
    GlesProgram::StageUniforms& uniforms = program->stage_uniforms[0];
    for (uint32_t i = 0; i < kSystemConstantUniformCount; ++i) {
      uniforms.system[i] = GetFunctions().GetUniformLocation(
          program_name,
          fmt::format("xe_uniform_system_constants_vs.{}", kSystemConstantUniforms[i].name)
              .c_str());
    }
    uniforms.fetch_constants = GetFunctions().GetUniformLocation(
        program_name, "xe_uniform_fetch_constants_vs.fetch_constants");
    uniforms.float_constants = GetFunctions().GetUniformLocation(
        program_name, "xe_uniform_float_constants_vs.float_constants");
  };
  fill_vs();
  program->debug_sampler_location =
      GetFunctions().GetUniformLocation(program_name, "xe_debug_tex");
  for (const SpirvShader::SpirvTranslation::VertexMemorySampler& vertex_memory_sampler :
       vertex_shader->vertex_memory_samplers()) {
    GlesProgram::VertexMemorySamplerBinding& binding = program->vertex_memory_samplers.emplace_back();
    binding.fetch_constant = vertex_memory_sampler.fetch_constant;
    binding.is_index = vertex_memory_sampler.is_index;
    binding.uniform_location =
        GetFunctions().GetUniformLocation(program_name, vertex_memory_sampler.name.c_str());
  }
  const GlesProgram* result = program.get();
  debug_color_programs_.emplace(cache_key, std::move(program));
  return result;
}

const GlesProgram* GlesPipelineCache::GetProgram(GlesShader::GlesTranslation* vertex_shader,
                                                 GlesShader::GlesTranslation* pixel_shader) {
  auto it = programs_.find(
      std::make_pair(static_cast<const void*>(vertex_shader), static_cast<const void*>(pixel_shader)));
  if (it != programs_.end()) {
    return it->second.get();
  }
  const GlesProgram* program = CreateProgram(vertex_shader, pixel_shader);
  if (!program) {
    return nullptr;
  }
  if (!pixel_shader) {
    depth_only_program_ = program;
  }
  return program;
}

}  // namespace rex::graphics::gles
