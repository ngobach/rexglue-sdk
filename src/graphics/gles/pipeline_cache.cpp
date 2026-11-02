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
#include <cctype>
#include <set>

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

namespace {

// Env knobs are documented as 0/1; treat any value other than empty and "0"
// as enabled. Checking only for presence made GLES_FORCE_TEXTURE_MEMORY=0
// still select the texture path.
bool EnvEnabled(const char* name) {
  const char* value = getenv(name);
  return value && *value && strcmp(value, "0") != 0;
}

// The guest permits a vertex shader to export only a subset of the
// interpolators its fragment shader reads (the values are undefined on the
// real GPU). Mali's linker rejects such a program outright ("Fragment shader
// uses a varying xe_interpolator_N that has not been declared in the vertex
// shader"), while ANGLE and Mesa tolerate it. Collect the interpolator indices
// referenced by the GLSL and synthesize declarations for the missing ones.
void CollectInterpolatorIndices(const std::string& source, std::set<uint32_t>& indices) {
  static constexpr char kToken[] = "xe_interpolator_";
  const size_t token_length = sizeof(kToken) - 1;
  for (size_t position = source.find(kToken); position != std::string::npos;
       position = source.find(kToken, position + token_length)) {
    size_t digit_end = position + token_length;
    while (digit_end < source.size() &&
           std::isdigit(static_cast<unsigned char>(source[digit_end]))) {
      ++digit_end;
    }
    if (digit_end > position + token_length) {
      const std::string digits = source.substr(position + token_length,
                                                digit_end - (position + token_length));
      indices.insert(uint32_t(std::strtoul(digits.c_str(), nullptr, 10)));
    }
  }
}

std::string AddMissingInterpolators(const std::string& source,
                                    const std::vector<uint32_t>& missing) {
  std::string declarations;
  std::string initializers;
  for (uint32_t index : missing) {
    declarations += fmt::format("out highp vec4 xe_interpolator_{};\n", index);
    initializers += fmt::format("    xe_interpolator_{} = vec4(0.0);\n", index);
  }
  std::string patched = source;
  size_t main_position = patched.find("\nvoid main");
  if (main_position == std::string::npos) {
    main_position = patched.size();
  }
  patched.insert(main_position, "\n" + declarations);
  main_position = patched.find("void main");
  if (main_position != std::string::npos) {
    const size_t brace = patched.find('{', main_position);
    if (brace != std::string::npos) {
      patched.insert(brace + 1, "\n" + initializers);
    }
  }
  return patched;
}

// Links a trivial vertex shader with the given fragment shader (or a trivial
// one when 0) so a failure can be attributed to the context rather than to the
// translated shaders.
void LinkSelfTest(GLuint fragment_shader, const char* label) {
  static const char kVertexSource[] =
      "#version 300 es\nvoid main(){gl_Position=vec4(0.0,0.0,0.0,1.0);}\n";
  static const char kFragmentSource[] =
      "#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main(){o=vec4(1.0);}\n";
  auto& gl = GetFunctions();
  const char* source = kVertexSource;
  GLuint vs = gl.CreateShader(GL_VERTEX_SHADER);
  gl.ShaderSource(vs, 1, &source, nullptr);
  gl.CompileShader(vs);
  GLuint fs = fragment_shader;
  if (!fs) {
    const char* fragment_source = kFragmentSource;
    fs = gl.CreateShader(GL_FRAGMENT_SHADER);
    gl.ShaderSource(fs, 1, &fragment_source, nullptr);
    gl.CompileShader(fs);
  }
  GLuint program = gl.CreateProgram();
  gl.AttachShader(program, vs);
  gl.AttachShader(program, fs);
  gl.LinkProgram(program);
  GLint ok = 0;
  gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
  const GLenum error = gl.GetError();
  gl.DeleteProgram(program);
  gl.DeleteShader(vs);
  if (!fragment_shader) {
    gl.DeleteShader(fs);
  }
  REXGPU_INFO("GLES link self-test ({}): link={} GL error 0x{:X}, vs={}, fs={}", label, ok,
              uint32_t(error), vs, fs);
}

}  // namespace

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
  // The R36S intentionally uses the ES 3.0 vertex-memory path even though its
  // Mali driver advertises ES 3.2: mirroring the 512 MB guest address space as
  // storage buffers exhausts the 1 GB device (and the Mali blob refuses the
  // large allocations), while per-draw vertex/index uploads do not.
  if (EnvEnabled("GLES_FORCE_TEXTURE_MEMORY")) {
    uses_texture_memory_ = true;
  }
  GLint max_storage_buffer_bindings = 0;
  GetFunctions().GetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS,
                             &max_storage_buffer_bindings);
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
    // by default the vertex stage takes its memory from plain uniform arrays
    // instead. Mali (R36S) does support vertex texture fetches and has tight
    // uniform limits (512-uint arrays exhaust them and programs fail to link),
    // so allow selecting the texture path there with GLES_VERTEX_TEXTURE_FETCH.
    features_.vertex_memory_uniforms = !EnvEnabled("GLES_VERTEX_TEXTURE_FETCH");
    features_.derivative_control = false;
  } else {
    features_.spirv_version = spv::Spv_1_3;
    // Split the 512 MB guest mirror into as many slices as the driver allows
    // (bounded to 16). The Mali-G31 blob refuses large single SSBO
    // allocations, and finer slices keep the lazy mirror allocation in
    // GlesSharedMemory small.
    uint32_t bindings_log2 = 0;
    const uint32_t max_bindings =
        uint32_t(std::max<GLint>(max_storage_buffer_bindings, 1));
    while (bindings_log2 < 4 && (uint32_t(1) << (bindings_log2 + 1)) <= max_bindings) {
      ++bindings_log2;
    }
    uint32_t slice_size = uint32_t(1) << (SharedMemory::kBufferSizeLog2 - bindings_log2);
    while (bindings_log2 > 0 && slice_size > uint32_t(max_storage_block_size)) {
      --bindings_log2;
      slice_size <<= 1;
    }
    features_.max_storage_buffer_range = slice_size;
    REXGPU_INFO("GLES: {} storage buffer bindings, {} MB shared memory slices",
                max_storage_buffer_bindings, slice_size >> 20);
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
    // The depth-only fragment shader is linked with translated vertex shaders,
    // so it must use the same GLSL ES version they do. Mixing 3.00 and 3.10 in
    // one program fails to link ("all shaders must use same shading language
    // version").
    options.version = uses_texture_memory_ ? 300 : 310;
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
    const std::string version_directive =
        uses_texture_memory_ ? "#version 300 es\n" : "#version 310 es\n";
    std::string debug_fs_source = version_directive;
    if (getenv("GLES_DEBUG_INTERPS")) {
      debug_fs_source +=
          "precision highp float;\nin highp vec4 xe_interpolator_0;\n"
          "in highp vec4 xe_interpolator_1;\nout vec4 o;\n"
          "void main() { o = vec4(xe_interpolator_0.xy, xe_interpolator_1.x, 1.0); }\n";
    } else if (getenv("GLES_DEBUG_TEXPS")) {
      debug_fs_source +=
          "precision highp float;\nprecision highp sampler2DArray;\n"
          "uniform sampler2DArray xe_debug_tex;\nout vec4 o;\n"
          "void main() { o = texture(xe_debug_tex, vec3(0.5, 0.5, 0.0)); }\n";
    } else {
      debug_fs_source +=
          "precision mediump float;\nout vec4 o;\n"
          "void main() { o = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    }
    CompileGlslEsShader(GL_FRAGMENT_SHADER, debug_fs_source, debug_color_fragment_shader_);
  }
  if (!CompileGlslEsShader(GL_FRAGMENT_SHADER, depth_only_glsl, depth_only_fragment_shader_)) {
    REXGPU_ERROR("GLES pipeline cache: failed to compile the depth-only fragment shader");
    return false;
  }
  if (const char* dump_root = getenv("GLES_DUMP_SHADERS")) {
    char path[512];
    snprintf(path, sizeof(path), "%s/depth_only_fs.glsl", dump_root);
    if (FILE* dump_file = fopen(path, "wb")) {
      fwrite(depth_only_glsl.data(), 1, depth_only_glsl.size(), dump_file);
      fclose(dump_file);
    }
  }

  REXGPU_INFO("GLES pipeline cache: shared memory max storage block {} MB ({} bindings)",
              features_.max_storage_buffer_range >> 20,
              shared_memory_storage_buffer_count_log2() >= 32
                  ? 0
                  : (1u << shared_memory_storage_buffer_count_log2()));
  if (EnvEnabled("GLES_LINK_SELFTEST")) {
    LinkSelfTest(0, "init, trivial pair");
    LinkSelfTest(depth_only_fragment_shader_, "init, depth-only FS");
  }
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
  if (EnvEnabled("GLES_LINK_SELFTEST")) {
    LinkSelfTest(0, "draw, trivial pair");
    LinkSelfTest(fragment_shader, "draw, real FS");
  }

  // Mali refuses to link a fragment shader that reads an interpolator the
  // vertex shader does not declare. Patch the vertex shader copy used for this
  // program so the pair links (see AddMissingInterpolators).
  GLuint vertex_shader_object = vertex_shader->shader_object();
  GLuint patched_vertex_shader = 0;
  if (!depth_only) {
    std::set<uint32_t> vertex_interpolators;
    CollectInterpolatorIndices(vertex_shader->glsl_source(), vertex_interpolators);
    std::set<uint32_t> pixel_interpolators;
    CollectInterpolatorIndices(pixel_shader->glsl_source(), pixel_interpolators);
    std::vector<uint32_t> missing_interpolators;
    for (uint32_t index : pixel_interpolators) {
      if (!vertex_interpolators.count(index)) {
        missing_interpolators.push_back(index);
      }
    }
    if (!missing_interpolators.empty()) {
      std::string patched_source =
          AddMissingInterpolators(vertex_shader->glsl_source(), missing_interpolators);
      if (CompileGlslEsShader(GL_VERTEX_SHADER, patched_source, patched_vertex_shader)) {
        vertex_shader_object = patched_vertex_shader;
        REXGPU_INFO(
            "GLES pipeline cache: VS {:016X} does not declare {} interpolator(s) read by PS "
            "{:016X}; declared and zeroed them for linking",
            vertex_shader->shader().ucode_data_hash(), uint32_t(missing_interpolators.size()),
            pixel_shader->shader().ucode_data_hash());
      }
    }
  }

  GLuint program_name = GetFunctions().CreateProgram();
  if (!program_name) {
    REXGPU_ERROR("GLES pipeline cache: glCreateProgram failed");
    return nullptr;
  }
  GetFunctions().AttachShader(program_name, vertex_shader_object);
  GetFunctions().AttachShader(program_name, fragment_shader);
  GetFunctions().LinkProgram(program_name);
  GetFunctions().DetachShader(program_name, vertex_shader_object);
  GetFunctions().DetachShader(program_name, fragment_shader);
  if (patched_vertex_shader) {
    GetFunctions().DeleteShader(patched_vertex_shader);
  }

  GLint link_status = 0;
  GetFunctions().GetProgramiv(program_name, GL_LINK_STATUS, &link_status);
  if (!link_status) {
    const GLenum link_error = GetFunctions().GetError();
    GLint vs_compile_status = 0, fs_compile_status = 0;
    GetFunctions().GetShaderiv(vertex_shader->shader_object(), GL_COMPILE_STATUS,
                               &vs_compile_status);
    GetFunctions().GetShaderiv(fragment_shader, GL_COMPILE_STATUS, &fs_compile_status);
    GLint active_uniforms = 0, active_attributes = 0;
    GetFunctions().GetProgramiv(program_name, GL_ACTIVE_UNIFORMS, &active_uniforms);
    GetFunctions().GetProgramiv(program_name, GL_ACTIVE_ATTRIBUTES, &active_attributes);
    GLint max_vs_uniform_vectors = 0, max_fs_uniform_vectors = 0;
    GetFunctions().GetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &max_vs_uniform_vectors);
    GetFunctions().GetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &max_fs_uniform_vectors);
    GLint log_length = 0;
    GetFunctions().GetProgramiv(program_name, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(size_t(log_length ? log_length : 1), '\0');
    GetFunctions().GetProgramInfoLog(program_name, GLsizei(log.size()), nullptr, log.data());
    REXGPU_ERROR(
        "GLES pipeline cache: program link failed (GL error 0x{:X}, uniforms {}, "
        "attributes {}, max uniform vectors VS/FS {}/{}):\n{}",
        uint32_t(link_error), active_uniforms, active_attributes, max_vs_uniform_vectors,
        max_fs_uniform_vectors, log);
    REXGPU_ERROR(
        "GLES pipeline cache: link state: program={}, vs={} (compiled {}, {:016X}), fs={} "
        "(compiled {}, {:016X})",
        program_name, vertex_shader->shader_object(), vs_compile_status,
        vertex_shader->shader().ucode_data_hash(), fragment_shader, fs_compile_status,
        depth_only ? 0 : pixel_shader->shader().ucode_data_hash());
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
