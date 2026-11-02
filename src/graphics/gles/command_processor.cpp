/**
 * @file        graphics/gles/command_processor.cpp
 * @brief       Xenos command processor for the OpenGL ES backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <rex/graphics/gles/command_processor.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <set>

#include <rex/graphics/flags.h>
#include <rex/graphics/gles/constants.h>
#include <rex/graphics/gles/pipeline_cache.h>
#include <rex/graphics/gles/primitive_processor.h>
#include <rex/graphics/gles/render_target_cache.h>
#include <rex/graphics/gles/shared_memory.h>
#include <rex/graphics/gles/texture_cache.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>

#include <xxhash.h>

#include <string>
#include <rex/ui/gles/presenter.h>
#include <rex/ui/presenter.h>

namespace rex::graphics::gles {

namespace {

GLenum CompareFuncToGL(xenos::CompareFunction compare_function) {
  switch (compare_function) {
    case xenos::CompareFunction::kNever:
      return GL_NEVER;
    case xenos::CompareFunction::kLess:
      return GL_LESS;
    case xenos::CompareFunction::kEqual:
      return GL_EQUAL;
    case xenos::CompareFunction::kLessEqual:
      return GL_LEQUAL;
    case xenos::CompareFunction::kGreater:
      return GL_GREATER;
    case xenos::CompareFunction::kNotEqual:
      return GL_NOTEQUAL;
    case xenos::CompareFunction::kGreaterEqual:
      return GL_GEQUAL;
    case xenos::CompareFunction::kAlways:
    default:
      return GL_ALWAYS;
  }
}

GLenum StencilOpToGL(xenos::StencilOp stencil_op) {
  switch (stencil_op) {
    case xenos::StencilOp::kKeep:
      return GL_KEEP;
    case xenos::StencilOp::kZero:
      return GL_ZERO;
    case xenos::StencilOp::kReplace:
      return GL_REPLACE;
    case xenos::StencilOp::kIncrementClamp:
      return GL_INCR;
    case xenos::StencilOp::kDecrementClamp:
      return GL_DECR;
    case xenos::StencilOp::kInvert:
      return GL_INVERT;
    case xenos::StencilOp::kIncrementWrap:
      return GL_INCR_WRAP;
    case xenos::StencilOp::kDecrementWrap:
      return GL_DECR_WRAP;
    default:
      return GL_KEEP;
  }
}

GLenum BlendFactorToGL(xenos::BlendFactor blend_factor) {
  switch (blend_factor) {
    case xenos::BlendFactor::kZero:
      return GL_ZERO;
    case xenos::BlendFactor::kOne:
      return GL_ONE;
    case xenos::BlendFactor::kSrcColor:
      return GL_SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return GL_ONE_MINUS_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha:
      return GL_SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return GL_ONE_MINUS_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor:
      return GL_DST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor:
      return GL_ONE_MINUS_DST_COLOR;
    case xenos::BlendFactor::kDstAlpha:
      return GL_DST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return GL_ONE_MINUS_DST_ALPHA;
    case xenos::BlendFactor::kConstantColor:
      return GL_CONSTANT_COLOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
      return GL_ONE_MINUS_CONSTANT_COLOR;
    case xenos::BlendFactor::kConstantAlpha:
      return GL_CONSTANT_ALPHA;
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return GL_ONE_MINUS_CONSTANT_ALPHA;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return GL_SRC_ALPHA_SATURATE;
    default:
      return GL_ONE;
  }
}

GLenum BlendOpToGL(xenos::BlendOp blend_op) {
  switch (blend_op) {
    case xenos::BlendOp::kAdd:
      return GL_FUNC_ADD;
    case xenos::BlendOp::kSubtract:
      return GL_FUNC_SUBTRACT;
    case xenos::BlendOp::kMin:
      return GL_MIN;
    case xenos::BlendOp::kMax:
      return GL_MAX;
    case xenos::BlendOp::kRevSubtract:
      return GL_FUNC_REVERSE_SUBTRACT;
    default:
      return GL_FUNC_ADD;
  }
}

GLenum PrimitiveTypeToGL(xenos::PrimitiveType primitive_type) {
  switch (primitive_type) {
    case xenos::PrimitiveType::kPointList:
      return GL_POINTS;
    case xenos::PrimitiveType::kLineList:
      return GL_LINES;
    case xenos::PrimitiveType::kLineStrip:
      return GL_LINE_STRIP;
    case xenos::PrimitiveType::kTriangleList:
      return GL_TRIANGLES;
    case xenos::PrimitiveType::kTriangleFan:
      return GL_TRIANGLE_FAN;
    case xenos::PrimitiveType::kTriangleStrip:
      return GL_TRIANGLE_STRIP;
    case xenos::PrimitiveType::kLineLoop:
      return GL_LINE_LOOP;
    default:
      return 0xFFFFFFFFu;
  }
}

uint32_t GetTextureTargetForDimension(xenos::FetchOpDimension dimension) {
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      return GL_TEXTURE_3D;
    case xenos::FetchOpDimension::kCube:
      return GL_TEXTURE_CUBE_MAP;
    default:
      return GL_TEXTURE_2D_ARRAY;
  }
}

// Unit assignment rules live in constants.h (kGuestTextureUnitCount,
// kMemoryUnitFloor, kScratchTextureUnit).

// Binds `texture` to `unit` on `target`. The caller must keep the unit ranges
// disjoint (see constants.h); do not unbind other targets here, ANGLE corrupts
// the unit's texture association when a target is cleared on a unit that already
// has a texture of another type.
void BindTextureUnit(uint32_t unit, GLenum target, GLuint texture) {
  GetFunctions().ActiveTexture(GLenum(GL_TEXTURE0 + unit));
  GetFunctions().BindTexture(target, texture);
}

}  // namespace

GlesCommandProcessor::GlesCommandProcessor(GraphicsSystem* graphics_system,
                                           system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

GlesCommandProcessor::~GlesCommandProcessor() { ShutdownContext(); }

bool GlesCommandProcessor::SetupContext() {
  auto* presenter =
      graphics_system_ ? static_cast<ui::gles::GlesPresenter*>(graphics_system_->presenter())
                       : nullptr;
  if (!presenter) {
    REXGPU_ERROR("GLES: no presenter");
    return false;
  }
  if (!presenter->MakeContextCurrentOnGlThread()) {
    REXGPU_ERROR("GLES: failed to make the GL context current on the command processor thread");
    return false;
  }

  const char* version =
      reinterpret_cast<const char*>(GetFunctions().GetString(GL_VERSION));
  const char* renderer =
      reinterpret_cast<const char*>(GetFunctions().GetString(GL_RENDERER));

  render_target_cache_ =
      std::make_unique<GlesRenderTargetCache>(*register_file_, *memory_, 1, 1);
  if (!render_target_cache_->Initialize()) {
    return false;
  }
  pipeline_cache_ = std::make_unique<GlesPipelineCache>(
      *register_file_, *render_target_cache_, 1, 1);
  if (!pipeline_cache_->Initialize()) {
    return false;
  }
  shared_memory_ = std::make_unique<GlesSharedMemory>(
      *memory_, pipeline_cache_->shared_memory_storage_buffer_count_log2(),
      !pipeline_cache_->uses_texture_memory());
  if (!shared_memory_->Initialize()) {
    return false;
  }
  texture_cache_ =
      std::make_unique<GlesTextureCache>(*register_file_, *shared_memory_, *memory_, 1, 1);
  if (!texture_cache_->Initialize()) {
    return false;
  }
  primitive_processor_ =
      std::make_unique<GlesPrimitiveProcessor>(*register_file_, *memory_, *shared_memory_);
  if (!primitive_processor_->Initialize()) {
    return false;
  }

  GetFunctions().GenVertexArrays(1, &vertex_array_);
  if (!vertex_array_) {
    REXGPU_ERROR("GLES: failed to create the vertex array object");
    return false;
  }
  GetFunctions().BindVertexArray(vertex_array_);
  GetFunctions().GenFramebuffers(1, &present_framebuffer_);
  GetFunctions().GenBuffers(1, &guest_index_buffer_);
  if (!present_framebuffer_ || !guest_index_buffer_) {
    REXGPU_ERROR("GLES: failed to create the presentation resources");
    return false;
  }

  if (getenv("GLES_DEBUG_QUAD")) {
    GLuint debug_vs = 0, debug_fs = 0;
    const char* debug_vs_source =
        "#version 300 es\n"
        "void main() {\n"
        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    const char* debug_fs_source =
        "#version 300 es\n"
        "precision mediump float;\n"
        "out vec4 o;\n"
        "void main() { o = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    if (CompileGlslEsShader(GL_VERTEX_SHADER, debug_vs_source, debug_vs) &&
        CompileGlslEsShader(GL_FRAGMENT_SHADER, debug_fs_source, debug_fs)) {
      debug_program_ = GetFunctions().CreateProgram();
      GetFunctions().AttachShader(debug_program_, debug_vs);
      GetFunctions().AttachShader(debug_program_, debug_fs);
      GetFunctions().LinkProgram(debug_program_);
      GetFunctions().DeleteShader(debug_vs);
      GetFunctions().DeleteShader(debug_fs);
    }
  }

  GetFunctions().Disable(GL_SCISSOR_TEST);
  GetFunctions().Disable(GL_DEPTH_TEST);
  GetFunctions().Disable(GL_STENCIL_TEST);
  GetFunctions().Disable(GL_CULL_FACE);
  GetFunctions().Disable(GL_BLEND);
  GetFunctions().Disable(GL_DITHER);

  REXGPU_INFO("GLES: command processor context ready");
  return true;
}

void GlesCommandProcessor::ShutdownContext() {
  primitive_processor_.reset();
  texture_cache_.reset();
  shared_memory_.reset();
  pipeline_cache_.reset();
  render_target_cache_.reset();
  if (vertex_array_) {
    GetFunctions().DeleteVertexArrays(1, &vertex_array_);
    vertex_array_ = 0;
  }
  if (present_framebuffer_) {
    GetFunctions().DeleteFramebuffers(1, &present_framebuffer_);
    present_framebuffer_ = 0;
  }
  if (guest_index_buffer_) {
    GetFunctions().DeleteBuffers(1, &guest_index_buffer_);
    guest_index_buffer_ = 0;
  }
  for (auto& texture_pair : vfetch_memory_textures_) {
    if (texture_pair.second.texture) {
      GetFunctions().DeleteTextures(1, &texture_pair.second.texture);
    }
  }
  vfetch_memory_textures_.clear();
  if (index_memory_texture_.texture) {
    GetFunctions().DeleteTextures(1, &index_memory_texture_.texture);
    index_memory_texture_ = {};
  }
  if (dummy_memory_texture_.texture) {
    GetFunctions().DeleteTextures(1, &dummy_memory_texture_.texture);
    dummy_memory_texture_ = {};
  }
}

Shader* GlesCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                         const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool GlesCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                     IndexBufferInfo* index_buffer_info,
                                     bool major_mode_explicit) {
  const bool log_draws = getenv("GLES_LOG_DRAWS") != nullptr;
  static uint32_t draws_logged = 0;
  const bool log_this_draw = log_draws && draws_logged < 60;
  auto draw_log = [&](const char* outcome, uint32_t extra) {
    if (!log_this_draw) {
      return;
    }
    const RegisterFile& log_regs = *register_file_;
    fprintf(stderr,
            "DRAWLOG prim=%u count=%u rasterize=%u pitch=%u edram=%u ps=%u vs_writes_interp=%u "
            "outcome=%s extra=%u\n",
            uint32_t(prim_type), index_count,
            uint32_t(draw_util::IsRasterizationPotentiallyDone(
                log_regs, draw_util::IsPrimitivePolygonal(log_regs))),
            uint32_t(log_regs.Get<reg::RB_SURFACE_INFO>().surface_pitch),
            uint32_t(log_regs.Get<reg::RB_MODECONTROL>().edram_mode),
            uint32_t(active_pixel_shader() != nullptr),
            uint32_t(active_vertex_shader() ? active_vertex_shader()->writes_interpolators() : 0),
            outcome, extra);
    fflush(stderr);
    ++draws_logged;
  };

  const RegisterFile& regs = *register_file_;
  const xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }

  const bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;
  const bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    draw_log("pitch0", 0);
    return true;
  }

  auto* vertex_shader = static_cast<GlesShader*>(active_vertex_shader());
  if (!vertex_shader) {
    REXGPU_ERROR("GLES IssueDraw: no vertex shader");
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  if (vertex_shader->memexport_eM_written()) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXGPU_ERROR("GLES IssueDraw: memexport is not supported by this backend yet");
    }
    return false;
  }

  GlesShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<GlesShader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Rasterization disabled and no memexport support - nothing to draw.
    draw_log("noraster", 0);
    return true;
  }
  if (pixel_shader && getenv("GLES_SKIP_2E37") &&
      pixel_shader->ucode_data_hash() == 0x2E372EA28CC404B7ull) {
    return true;
  }
  if (pixel_shader && pixel_shader->memexport_eM_written()) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXGPU_ERROR("GLES IssueDraw: pixel shader memexport is not supported by this backend yet");
    }
    return false;
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader
          ? (vertex_shader->writes_interpolators() &
             pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                    regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                    ps_param_gen_pos))
          : 0;

  PrimitiveProcessor::ProcessingResult processing_result;
  if (!primitive_processor_->Process(processing_result)) {
    REXGPU_ERROR("GLES IssueDraw: primitive processing failed");
    return false;
  }
  if (!processing_result.host_draw_vertex_count) {
    draw_log("noverts", 0);
    return true;
  }
  if (getenv("GLES_LOG_UNIFORMS") && render_target_cache_->draw_width()) {
    static uint32_t logged_uniforms = 0;
    if (logged_uniforms < 3) {
      ++logged_uniforms;
      GLint current = 0;
      GetFunctions().GetIntegerv(0x8B8D, &current);
      auto ui = [&](const char* name, uint32_t count) {
        GLint loc = GetFunctions().GetUniformLocation(GLuint(current), name);
        GLuint values[4] = {};
        if (loc >= 0) {
          GetFunctions().GetUniformuiv(GLuint(current), loc, values);
        }
        fprintf(stderr, "UNIFORM %s loc=%d u=[%08X %08X %08X %08X]\n", name, loc, values[0],
                values[1], values[2], values[3]);
      };
      GLint loc = GetFunctions().GetUniformLocation(GLuint(current),
                                                    "xe_uniform_system_constants_vs.ndc_scale");
      GLfloat scales[4] = {};
      if (loc >= 0) {
        GetFunctions().GetUniformfv(GLuint(current), loc, scales);
      }
      fprintf(stderr, "UNIFORM ndc_scale loc=%d f=[%f %f %f %f]\n", loc, scales[0], scales[1],
              scales[2], scales[3]);
      ui("xe_uniform_system_constants_vs.flags", 1);
      ui("xe_uniform_fetch_constants_vs.fetch_constants[0]", 4);
      ui("xe_uniform_system_constants_vs.texture_swizzled_signs[0]", 4);
    }
  }
  const GLenum host_primitive = PrimitiveTypeToGL(processing_result.host_primitive_type);
  if (host_primitive == 0xFFFFFFFFu) {
    REXGPU_ERROR("GLES IssueDraw: unsupported host primitive type {}",
                 uint32_t(processing_result.host_primitive_type));
    return false;
  }

  const SpirvShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, processing_result.host_vertex_shader_type, interpolator_mask,
          ps_param_gen_pos != UINT32_MAX);
  const SpirvShaderTranslator::Modification pixel_shader_modification =
      pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                         *pixel_shader, interpolator_mask, ps_param_gen_pos,
                         normalized_depth_control)
                   : SpirvShaderTranslator::Modification(0);

  auto* vertex_shader_translation = static_cast<GlesShader::GlesTranslation*>(
      vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  auto* pixel_shader_translation =
      pixel_shader ? static_cast<GlesShader::GlesTranslation*>(
                         pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                   : nullptr;
  if (!pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                pixel_shader_translation)) {
    REXGPU_ERROR("GLES IssueDraw: shader translation failed");
    return false;
  }

  uint32_t used_texture_mask = vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (pixel_shader) {
    used_texture_mask |= pixel_shader->GetUsedTextureMaskAfterTranslation();
  }
  texture_cache_->RequestTextures(used_texture_mask);

  auto check_gl = [&](const char* stage) {
    const GLenum error = GetFunctions().GetError();
    if (error != GL_NO_ERROR) {
      REXGPU_ERROR("GLES: GL error 0x{:X} after {}", uint32_t(error), stage);
    }
  };
    if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control, 
                                    pixel_shader ? draw_util::GetNormalizedColorMask(
                                                       regs, pixel_shader->writes_color_targets())
                                                 : 0,
                                    *vertex_shader)) {
    REXGPU_ERROR("GLES IssueDraw: render target update failed");
    return false;
  }

  const GlesProgram* guest_program =
      pipeline_cache_->GetProgram(vertex_shader_translation, pixel_shader_translation);
  const GlesProgram* program = guest_program;
  if (getenv("GLES_DEBUG_VS")) {
    const GlesProgram* debug_program = pipeline_cache_->GetDebugColorProgram(
        vertex_shader_translation,
        getenv("GLES_DEBUG_VS_PS") ? pixel_shader_translation : nullptr);
    if (debug_program) {
      program = debug_program;
    }
  }
  if (!program || !program->program) {
    REXGPU_ERROR("GLES IssueDraw: program creation failed");
    return false;
  }
  if (const char* only_draw = getenv("GLES_ONLY_DRAW")) {
    // Probe knob: render only one draw of each frame, so a full-screen debug
    // quad can be attributed to a specific draw.
    const uint32_t wanted = uint32_t(atoi(only_draw));
    const bool matches = frame_draw_index_ == wanted;
    ++frame_draw_index_;
    if (!matches) {
      return true;
    }
  } else {
    ++frame_draw_index_;
  }
  if (const char* only_program = getenv("GLES_ONLY_PROGRAM")) {
    // Probe knob: render only one GL program, so a full-screen debug quad can
    // be attributed to a specific draw.
    if (program->program != GLuint(atoi(only_program))) {
      return true;
    }
  }
  GetFunctions().UseProgram(program->program);
  check_gl("use_program");

  // Viewport and scissor in the guest (top-down) coordinate space, mirrored to
  // GL's bottom-up window coordinates using the render target texture height.
  const uint32_t render_target_width = render_target_cache_->draw_width();
  const uint32_t render_target_height = render_target_cache_->draw_height();
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(regs, 1, 1, /*origin_bottom_left=*/true,
                                 render_target_width ? render_target_width : 8192,
                                 render_target_height ? render_target_height : 8192,
                                 /*allow_reverse_z=*/false, normalized_depth_control,
                                 /*convert_z_to_float24=*/false, /*full_float24_in_0_to_1=*/false,
                                 pixel_shader ? pixel_shader->writes_depth() : false, viewport_info);
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    GetFunctions().Viewport(GLint(viewport_info.xy_offset[0]),
                   GLint(render_target_height) - GLint(viewport_info.xy_offset[1]) -
                       GLint(viewport_info.xy_extent[1]),
                   GLsizei(viewport_info.xy_extent[0]), GLsizei(viewport_info.xy_extent[1]));
  } else {
    GetFunctions().Viewport(-1, -1, 1, 1);
  }
  GetFunctions().DepthRangef(viewport_info.z_min, viewport_info.z_max);

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  const uint32_t scissor_x = std::min(scissor.offset[0], render_target_width);
  const uint32_t scissor_y = std::min(scissor.offset[1], render_target_height);
  const uint32_t scissor_w = std::min(scissor.extent[0], render_target_width - scissor_x);
  const uint32_t scissor_h = std::min(scissor.extent[1], render_target_height - scissor_y);
  if (scissor_w && scissor_h) {
    GetFunctions().Enable(GL_SCISSOR_TEST);
    GetFunctions().Scissor(GLint(scissor_x),
                  GLint(render_target_height - scissor_y - scissor_h), GLsizei(scissor_w),
                  GLsizei(scissor_h));
  } else {
    GetFunctions().Enable(GL_SCISSOR_TEST);
    GetFunctions().Scissor(0, 0, 0, 0);
  }

  const uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  ApplyRenderState(normalized_depth_control, normalized_color_mask, primitive_polygonal);
  check_gl("render_state");
  if (getenv("GLES_LOG_BINDINGS")) {
    static std::set<GLuint> logged_draw_state;
    if (logged_draw_state.insert(program->program).second) {
      fprintf(stderr,
              "DRAWSTATE program=%u guest_prim=%u host_prim=%u count=%u viewport=(%d,%d %dx%d) "
              "scissor=(%d,%d %dx%d) vm_xy=(%u,%u %ux%u) color_mask=0x%X z_test=%u zbuf=%u "
              "rt=%ux%u idx_type=%u idx_endian=%u\n",
              program->program, uint32_t(prim_type),
              uint32_t(processing_result.host_primitive_type),
              processing_result.host_draw_vertex_count, viewport_info.xy_offset[0],
              viewport_info.xy_offset[1], viewport_info.xy_extent[0], viewport_info.xy_extent[1],
              int32_t(scissor_x), int32_t(scissor_y), int32_t(scissor_w), int32_t(scissor_h),
              regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().tl_x,
              regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().tl_y,
              regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>().br_x,
              regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>().br_y, normalized_color_mask,
              uint32_t(normalized_depth_control.z_enable),
              uint32_t(normalized_depth_control.z_write_enable), render_target_cache_->draw_width(),
              render_target_cache_->draw_height(),
              uint32_t(processing_result.index_buffer_type),
              uint32_t(processing_result.host_shader_index_endian));
      fflush(stderr);
    }
  }
  UpdateSystemConstantValues(processing_result, viewport_info, used_texture_mask,
                             normalized_depth_control, normalized_color_mask);
  draw_log("draw", processing_result.host_draw_vertex_count);
  if (!getenv("GLES_SKIP_STAGE_CONSTANTS")) {
    SetStageConstants(*program, *vertex_shader, pixel_shader);
  }
  check_gl("stage_constants");
  if (!BindTextures(*program, *vertex_shader, pixel_shader)) {
    return false;
  }
  check_gl("textures");
  uint32_t index_bytes = 0;
  if (processing_result.index_buffer_type != PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    index_bytes = processing_result.host_draw_vertex_count *
                  (processing_result.host_index_format == xenos::IndexFormat::kInt16 ? 2 : 4);
  }
  if (!BindVertexMemoryTextures(*program, processing_result.guest_index_base, index_bytes)) {
    return false;
  }
  check_gl("vertex_memory_textures");

  if ((getenv("GLES_DEBUG_TEXTURE") || getenv("GLES_DEBUG_TEXPS")) && !debug_white_texture_) {
    const uint8_t white[4] = {255, 255, 255, 255};
    GetFunctions().GenTextures(1, &debug_white_texture_);
    GetFunctions().BindTexture(GL_TEXTURE_2D_ARRAY, debug_white_texture_);
    GetFunctions().TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA,
                              GL_UNSIGNED_BYTE, white);
    GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
  }
  if (debug_program_ && getenv("GLES_DEBUG_QUAD")) {
    static uint32_t logged_debug_quads = 0;
    GetFunctions().UseProgram(debug_program_);
    GetFunctions().Disable(GL_SCISSOR_TEST);
    GetFunctions().Disable(GL_DEPTH_TEST);
    GetFunctions().Disable(GL_STENCIL_TEST);
    GetFunctions().Disable(GL_BLEND);
    GetFunctions().Disable(GL_CULL_FACE);
    GetFunctions().ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GetFunctions().Viewport(0, 0, GLsizei(render_target_cache_->draw_width()),
                            GLsizei(render_target_cache_->draw_height()));
    GetFunctions().DrawArrays(GL_TRIANGLES, 0, 3);
    if (logged_debug_quads < 3) {
      ++logged_debug_quads;
      }
  }
  auto readback = [&](const char* label) {
    if (getenv("GLES_DEBUG_BBOX") && render_target_cache_->draw_width() &&
        render_target_cache_->draw_height()) {
      static uint32_t bbox_logged = 0;
      if (bbox_logged < 20) {
        ++bbox_logged;
        const uint32_t rt_w = render_target_cache_->draw_width();
        const uint32_t rt_h = render_target_cache_->draw_height();
        std::vector<uint8_t> pixels(size_t(rt_w) * rt_h * 4);
        GetFunctions().Finish();
        GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, render_target_cache_->draw_framebuffer());
        GetFunctions().PixelStorei(GL_PACK_ALIGNMENT, 1);
        GetFunctions().ReadPixels(0, 0, GLsizei(rt_w), GLsizei(rt_h), GL_RGBA, GL_UNSIGNED_BYTE,
                                  pixels.data());
        uint32_t nonzero = 0, min_x = rt_w, max_x = 0, min_y = rt_h, max_y = 0;
        uint32_t checksum = 0;
        for (uint32_t y = 0; y < rt_h; ++y) {
          const uint8_t* row = pixels.data() + size_t(y) * rt_w * 4;
          for (uint32_t x = 0; x < rt_w; ++x) {
            const uint8_t* pixel = row + size_t(x) * 4;
            if (pixel[0] | pixel[1] | pixel[2] | pixel[3]) {
              ++nonzero;
              checksum = checksum * 31 + (uint32_t(pixel[0]) << 16 | uint32_t(pixel[1]) << 8 |
                                          uint32_t(pixel[2]));
              min_x = std::min(min_x, x);
              max_x = std::max(max_x, x);
              min_y = std::min(min_y, y);
              max_y = std::max(max_y, y);
            }
          }
        }
        GLint scissor_box[4] = {};
        GetFunctions().GetIntegerv(GLenum(0x0C10), scissor_box);  // GL_SCISSOR_BOX
        GLint viewport_box[4] = {};
        GetFunctions().GetIntegerv(GLenum(0x0BA2), viewport_box);  // GL_VIEWPORT
        const size_t centre_offset = (size_t(rt_h) - 360) * size_t(rt_w) * 4 + size_t(rt_w / 2) * 4;
        uint32_t centre_pixel = 0;
        std::memcpy(&centre_pixel, pixels.data() + centre_offset, 4);
        fprintf(stderr,
                "BBOX %s program=%u rt=%ux%u nonzero=%u bbox=[%u,%u]-[%u,%u] checksum=0x%08X "
                "centre=0x%08X scissor=%d box=[%d,%d,%d,%d] viewport=[%d,%d,%d,%d]\n",
                label, program->program, rt_w, rt_h, nonzero, min_x, min_y, max_x, max_y,
                checksum, centre_pixel, GetFunctions().IsEnabled(GL_SCISSOR_TEST), scissor_box[0],
                scissor_box[1], scissor_box[2], scissor_box[3], viewport_box[0], viewport_box[1],
                viewport_box[2], viewport_box[3]);
        fflush(stderr);
      }
    }
    if (!getenv("GLES_DEBUG_READ") || !render_target_cache_->draw_width()) {
      return;
    }
    static uint32_t logged_readbacks = 0;
    if (logged_readbacks >= 4000) {
      return;
    }
    ++logged_readbacks;
    const uint32_t rt_w = render_target_cache_->draw_width();
    const uint32_t rt_h = render_target_cache_->draw_height();
    std::vector<uint8_t> pixels(size_t(rt_w) * 720 * 4);
    GetFunctions().Finish();
    GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, render_target_cache_->draw_framebuffer());
    GetFunctions().PixelStorei(GL_PACK_ALIGNMENT, 1);
    const GLint read_y = getenv("GLES_READ_BOTTOM") ? 0 : GLint(rt_h - 720);
    GetFunctions().ReadPixels(0, read_y, GLsizei(rt_w), 720, GL_RGBA,
                              GL_UNSIGNED_BYTE, pixels.data());
    {
      const GLenum read_error = GetFunctions().GetError();
      if (read_error != GL_NO_ERROR) {
        REXGPU_ERROR("GLES readback: ReadPixels failed with 0x{:X} (rt {}x{})",
                     uint32_t(read_error), rt_w, rt_h);
      }
    }
    uint32_t nonzero = 0;
    uint32_t checksum = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
      uint32_t value;
      std::memcpy(&value, pixels.data() + i, 4);
      if (value) {
        ++nonzero;
        checksum += value;
      }
    }
    auto sample_pixel = [&](uint32_t x, uint32_t y) {
      uint32_t value;
      std::memcpy(&value, pixels.data() + (size_t(y) * rt_w + x) * 4, 4);
      return value;
    };
    REXGPU_INFO("GLES {} readback: program={} vs={:016X} ps={:016X} prim={} count={} idx={} "
                "nonzero={} checksum=0x{:08X} px=[{:08X} {:08X} {:08X} {:08X}]",
                label, program->program, vertex_shader->ucode_data_hash(),
                pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                uint32_t(processing_result.host_primitive_type),
                processing_result.host_draw_vertex_count,
                uint32_t(processing_result.index_buffer_type), nonzero, checksum,
                sample_pixel(0, 0), sample_pixel(rt_w / 2, 360),
                sample_pixel(rt_w / 2, 719), sample_pixel(rt_w - 1, 719));
  };
  if (getenv("GLES_SAMPLE_QUAD")) {
    if (!debug_sample_program_ && getenv("GLES_SAMPLE_GUEST_FS") && pixel_shader_translation) {
      GLuint vs = 0;
      const char* vs_source =
          "#version 300 es\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),"
          "float(gl_VertexID&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
      if (CompileGlslEsShader(GL_VERTEX_SHADER, vs_source, vs)) {
        debug_sample_program_ = GetFunctions().CreateProgram();
        GetFunctions().AttachShader(debug_sample_program_, vs);
        GetFunctions().AttachShader(debug_sample_program_, pixel_shader_translation->shader_object());
        GetFunctions().LinkProgram(debug_sample_program_);
        GetFunctions().DeleteShader(vs);
        GLint link_status = 0;
        GetFunctions().GetProgramiv(debug_sample_program_, GL_LINK_STATUS, &link_status);
        fprintf(stderr, "SAMPLE guest FS program %u link=%d\n", debug_sample_program_,
                link_status);
        fflush(stderr);
      }
    }
    if (!debug_sample_program_) {
      GLuint vs = 0, fs = 0;
      std::string vs_source;
      if (const char* vs_mode = getenv("GLES_SAMPLE_VS_INT_SAMPLER")) {
        // '2' samples with an explicit LOD instead of texelFetch, to tell
        // whether only fetch-style vertex reads are broken on the driver.
        const char* fetch = vs_mode[0] == '2'
                                ? "textureLod(v,vec2(0.5/2048.0,0.5),0.0)"
                                : "texelFetch(v,ivec2(0,0),0)";
        vs_source =
            "#version 300 es\nprecision highp float;precision highp sampler2D;\n"
            "uniform highp sampler2D v;\nout highp float vt;\n"
            "void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));\n"
            "vt=float(floatBitsToUint(" +
            std::string(fetch) +
            ".x)&1u);\n"
            "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
      } else if (getenv("GLES_SAMPLE_VS_SAMPLER")) {
        vs_source =
            "#version 300 es\nprecision highp float;precision highp sampler2DArray;\n"
            "uniform highp sampler2DArray v;\n"
            "void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));\n"
            "float t=texelFetch(v,ivec3(0,0,0),0).x*1e-30;\n"
            "gl_Position=vec4(p*2.0-1.0+t,0.0,1.0);}\n";
      } else {
        vs_source =
            "#version 300 es\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),"
            "float(gl_VertexID&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
      }
      const char* vs_source_c = vs_source.c_str();
      const char* fs_source =
          getenv("GLES_SAMPLE_NO_FS_SAMPLER")
              ? (getenv("GLES_SAMPLE_VS_INT_SAMPLER")
                     ? "#version 300 es\nprecision highp float;in highp float vt;out vec4 o;\n"
                       "void main(){o=vec4(vt,1.0-vt,0.0,1.0);}\n"
                     : "#version 300 es\nprecision mediump float;out vec4 o;\n"
                       "void main(){o=vec4(1.0,0.0,1.0,1.0);}\n")
              : getenv("GLES_SAMPLE_GRAD")
              ? "#version 300 es\nprecision highp float;precision highp sampler2DArray;\n"
                "uniform sampler2DArray t;out vec4 o;void main(){"
                "o=textureGrad(t,vec3(0.5,0.5,0.0),vec2(0.0),vec2(0.0));}\n"
              : "#version 300 es\nprecision highp float;precision highp sampler2DArray;\n"
                "uniform sampler2DArray t;out vec4 o;void main(){o=texture(t,vec3(0.5,0.5,0.0));}\n";
      if (CompileGlslEsShader(GL_VERTEX_SHADER, vs_source_c, vs) &&
          CompileGlslEsShader(GL_FRAGMENT_SHADER, fs_source, fs)) {
        debug_sample_program_ = GetFunctions().CreateProgram();
        GetFunctions().AttachShader(debug_sample_program_, vs);
        GetFunctions().AttachShader(debug_sample_program_, fs);
        GetFunctions().LinkProgram(debug_sample_program_);
        GetFunctions().DeleteShader(vs);
        GetFunctions().DeleteShader(fs);
      }
      if (!debug_white_texture_) {
        const uint8_t white[4] = {255, 255, 255, 255};
        GetFunctions().GenTextures(1, &debug_white_texture_);
        GetFunctions().BindTexture(GL_TEXTURE_2D_ARRAY, debug_white_texture_);
        GetFunctions().TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA,
                                  GL_UNSIGNED_BYTE, white);
        GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
      }
    }
    if (debug_sample_program_) {
      GetFunctions().UseProgram(debug_sample_program_);
      if (getenv("GLES_SAMPLE_GUEST")) {
        BindTextureUnit(
            0, GL_TEXTURE_2D_ARRAY,
            texture_cache_->GetActiveBindingTexture(0, xenos::FetchOpDimension::k2D, false));
        GlesTextureCache::ApplySwizzle(
            GL_TEXTURE_2D_ARRAY, texture_cache_->GetActiveBindingSwizzle(0));
      } else {
        BindTextureUnit(0, GL_TEXTURE_2D_ARRAY, debug_white_texture_);
      }
      if (getenv("GLES_SAMPLE_SAMPLER")) {
        GlesShader::SamplerBinding binding;
        binding.fetch_constant = 0;
        GetFunctions().BindSampler(
            0, texture_cache_->GetOrCreateSampler(texture_cache_->GetSamplerParameters(binding)));
      } else {
        GetFunctions().BindSampler(0, 0);
      }
      GetFunctions().Uniform1i(
          GetFunctions().GetUniformLocation(debug_sample_program_, "t"), 0);
      if (getenv("GLES_SAMPLE_GUEST_FS")) {
        for (uint32_t unit = 0; unit < 8; ++unit) {
          std::string name = "xe_ps_sampler_" + std::to_string(unit);
          GLint location =
              GetFunctions().GetUniformLocation(debug_sample_program_, name.c_str());
          if (location >= 0) {
            GetFunctions().Uniform1i(location, GLint(unit));
          }
        }
      }
      if (getenv("GLES_SAMPLE_VS_INT_SAMPLER")) {
        // A texture whose first texel has bit 0 set, so the vertex shader can
        // report whether its own texture fetch actually returned data. Kept
        // above the guest sampler range (0..3) and the memory range (4..7).
        static const uint32_t kOneDword = 0x3F800001u;
        UploadUintTexture(dummy_memory_texture_, &kOneDword, 1);
        // Bind on every unit a shader could default to, to tell whether the
        // vertex stage ignores the program's sampler uniform assignment.
        for (uint32_t unit = 0; unit < 16; ++unit) {
          BindTextureUnit(unit, GL_TEXTURE_2D, dummy_memory_texture_.texture);
          GetFunctions().BindSampler(unit, 0);
        }
        const GLint v_location = GetFunctions().GetUniformLocation(debug_sample_program_, "v");
        GetFunctions().Uniform1i(v_location, 8);
        GLint v_value = -1;
        if (v_location >= 0) {
          GetFunctions().GetUniformiv(debug_sample_program_, v_location, &v_value);
        }
        GetFunctions().ActiveTexture(GLenum(GL_TEXTURE0 + 8));
        GLint bound_2d = 0, bound_array = 0;
        GetFunctions().GetIntegerv(GL_TEXTURE_BINDING_2D, &bound_2d);
        GetFunctions().GetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &bound_array);
        fprintf(stderr,
                "SAMPLE vs int: program=%u v_loc=%d v=%d unit8 2d=%d array=%d texture=%u bias=%f\n",
                debug_sample_program_, v_location, v_value, bound_2d, bound_array,
                dummy_memory_texture_.texture, -0.5f);
        fflush(stderr);
      } else if (getenv("GLES_SAMPLE_VS_SAMPLER")) {
        // Bind the guest texture at a unit above the guest sampler range to
        // test whether fragment sampling survives a program that also samples
        // in the vertex stage.
        BindTextureUnit(8, GL_TEXTURE_2D_ARRAY,
                        texture_cache_->GetActiveBindingTexture(
                            0, xenos::FetchOpDimension::k2D, false));
        GetFunctions().BindSampler(8, 0);
        GetFunctions().Uniform1i(
            GetFunctions().GetUniformLocation(debug_sample_program_, "v"), 8);
      }
      GetFunctions().Disable(GL_SCISSOR_TEST);
      GetFunctions().Disable(GL_DEPTH_TEST);
      GetFunctions().Disable(GL_STENCIL_TEST);
      GetFunctions().Disable(GL_BLEND);
      GetFunctions().Disable(GL_CULL_FACE);
      GetFunctions().ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      GetFunctions().Viewport(0, 0, GLsizei(render_target_cache_->draw_width()),
                              GLsizei(render_target_cache_->draw_height()));
      GetFunctions().DrawArrays(GL_TRIANGLES, 0, 3);
    }
  }
  if (getenv("GLES_GUEST_ALL")) {
    const GLuint guest_texture =
        texture_cache_->GetActiveBindingTexture(0, xenos::FetchOpDimension::k2D, false);
    for (uint32_t unit = 0; unit < kGuestTextureUnitCount; ++unit) {
      BindTextureUnit(unit, GL_TEXTURE_2D_ARRAY, guest_texture);
      GetFunctions().BindSampler(unit, 0);
    }
  }
  if (getenv("GLES_WHITE_ALL")) {
    if (!debug_white_texture_) {
      const uint8_t white[4] = {255, 255, 255, 255};
      GetFunctions().GenTextures(1, &debug_white_texture_);
      GetFunctions().BindTexture(GL_TEXTURE_2D_ARRAY, debug_white_texture_);
      GetFunctions().TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA,
                                GL_UNSIGNED_BYTE, white);
      GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    }
    for (uint32_t unit = 0; unit < kGuestTextureUnitCount; ++unit) {
      BindTextureUnit(unit, GL_TEXTURE_2D_ARRAY, debug_white_texture_);
      GetFunctions().BindSampler(unit, 0);
    }
  }
  if (getenv("GLES_FORCE_TEX_UNIT0") && pixel_shader) {
    for (uint32_t unit = 0; unit < 8; ++unit) {
      std::string name = "xe_ps_sampler_" + std::to_string(unit);
      GLint location = GetFunctions().GetUniformLocation(program->program, name.c_str());
      if (location >= 0) {
        GetFunctions().Uniform1i(location, GLint(unit));
      }
    }
  }
  // Bind the shared memory storage buffers and the index buffer, then draw.
  shared_memory_->Bind();
  if (getenv("GLES_LOG_BINDINGS")) {
    static std::set<GLuint> logged_draw_bindings;
    if (logged_draw_bindings.insert(program->program).second) {
      GLint active_unit = 0;
      GetFunctions().GetIntegerv(GL_ACTIVE_TEXTURE, &active_unit);
      GLint current_program = 0;
      GetFunctions().GetIntegerv(0x8B8D, &current_program);
      fprintf(stderr, "DRAWBIND program=%u active_program=%d active_unit=%d\n",
              program->program, current_program, active_unit);
      {
        GLint location = GetFunctions().GetUniformLocation(
            program->program, "xe_uniform_system_constants_vs.vertex_index_endian");
        GLuint value = 0;
        if (location >= 0) {
          GetFunctions().GetUniformuiv(program->program, location, &value);
        }
        fprintf(stderr, "  vertex_index_endian=%u (processing %u)\n", value,
                uint32_t(processing_result.host_shader_index_endian));
        GLint draw_fbo = -1, read_fbo = -1, viewport[4] = {};
        GetFunctions().GetIntegerv(0x8CA6, &draw_fbo);
        GetFunctions().GetIntegerv(0x8CAA, &read_fbo);
        GetFunctions().GetIntegerv(0x0BA2, viewport);
        fprintf(stderr, "  fbo draw=%d (rt %u) read=%d viewport=[%d %d %d %d]\n", draw_fbo,
                render_target_cache_->draw_framebuffer(), read_fbo, viewport[0], viewport[1],
                viewport[2], viewport[3]);
      }
      GetFunctions().ValidateProgram(program->program);
      {
        GLint active_count = 0, max_length = 0;
        GetFunctions().GetProgramiv(program->program, 0x8B86, &active_count);
        GetFunctions().GetProgramiv(program->program, 0x8B87, &max_length);
        std::string name(size_t(max_length > 1 ? max_length : 1), '\0');
        for (GLint index = 0; index < active_count; ++index) {
          GLsizei length = 0;
          GLint size = 0;
          GLenum type = 0;
          GetFunctions().GetActiveUniform(program->program, GLuint(index), GLsizei(name.size()),
                                          &length, &size, &type, name.data());
          if (name.find("sampler") != std::string::npos) {
            GLint location = GetFunctions().GetUniformLocation(program->program, name.c_str());
            fprintf(stderr, "  uniform %s type=0x%X size=%d loc=%d\n", name.c_str(),
                    uint32_t(type), size, location);
          }
        }
      }
      {
        GLint validate_status = 0;
        GetFunctions().GetProgramiv(program->program, GL_VALIDATE_STATUS, &validate_status);
        GLint log_length = 0;
        GetFunctions().GetProgramiv(program->program, GL_INFO_LOG_LENGTH, &log_length);
        std::string log(size_t(log_length ? log_length : 1), '\0');
        if (log_length > 1) {
          GetFunctions().GetProgramInfoLog(program->program, GLsizei(log.size()), nullptr,
                                           log.data());
        }
        fprintf(stderr, "  validate=%d log=%s\n", validate_status, log.c_str());
      }
      for (size_t i = 0; i < program->samplers.size(); ++i) {
        const GlesProgram::SamplerBinding& sampler = program->samplers[i];
        GLint value = -1;
        if (sampler.uniform_location >= 0) {
          GetFunctions().GetUniformiv(program->program, sampler.uniform_location, &value);
        }
        fprintf(stderr, "  sampler uniform[%zu] loc=%d value=%d\n", i,
                sampler.uniform_location, value);
      }
      for (const GlesProgram::VertexMemorySamplerBinding& sampler :
           program->vertex_memory_samplers) {
        GLint value = -1;
        if (sampler.uniform_location >= 0) {
          GetFunctions().GetUniformiv(program->program, sampler.uniform_location, &value);
        }
        fprintf(stderr, "  memory uniform fc=%u loc=%d value=%d\n", sampler.fetch_constant,
                sampler.uniform_location, value);
      }
      struct {
        const char* name;
        uint32_t count;
        GLfloat values[4];
      } float_uniforms[] = {
          {"xe_uniform_system_constants_vs.ndc_scale", 3, {}},
          {"xe_uniform_system_constants_vs.ndc_offset", 3, {}},
          {"xe_uniform_float_constants_vs.float_constants[0]", 4, {}},
          {"xe_uniform_float_constants_vs.float_constants[1]", 4, {}},
      };
      for (auto& uniform : float_uniforms) {
        GLint location = GetFunctions().GetUniformLocation(program->program, uniform.name);
        if (location >= 0) {
          GetFunctions().GetUniformfv(program->program, location, uniform.values);
        }
        fprintf(stderr, "  %s loc=%d f=[%f %f %f %f]\n", uniform.name, location,
                uniform.values[0], uniform.values[1], uniform.values[2], uniform.values[3]);
      }
      const char* uint_names[] = {"xe_uniform_system_constants_vs.flags",
                                  "xe_uniform_system_constants_vs.vertex_base_index",
                                  "xe_uniform_system_constants_vs.vertex_index_min",
                                  "xe_uniform_system_constants_vs.vertex_index_max"};
      for (const char* name : uint_names) {
        GLint location = GetFunctions().GetUniformLocation(program->program, name);
        GLuint value = 0;
        if (location >= 0) {
          GetFunctions().GetUniformuiv(program->program, location, &value);
        }
        fprintf(stderr, "  %s loc=%d u=%u\n", name, location, value);
      }
      {
        const char* ps_float_names[] = {"xe_uniform_system_constants_ps.color_exp_bias",
                                        "xe_uniform_system_constants_ps.alpha_test_reference"};
        for (const char* name : ps_float_names) {
          GLint location = GetFunctions().GetUniformLocation(program->program, name);
          GLfloat value[4] = {};
          if (location >= 0) {
            GetFunctions().GetUniformfv(program->program, location, value);
          }
          fprintf(stderr, "  %s loc=%d f=[%f %f %f %f] (constant %f)\n", name, location,
                  value[0], value[1], value[2], value[3], system_constants_.color_exp_bias[0]);
        }
        GLint ps_flags_location = GetFunctions().GetUniformLocation(
            program->program, "xe_uniform_system_constants_ps.flags");
        GLuint ps_flags = 0;
        if (ps_flags_location >= 0) {
          GetFunctions().GetUniformuiv(program->program, ps_flags_location, &ps_flags);
        }
        fprintf(stderr, "  ps flags loc=%d u=%u (constant %u)\n", ps_flags_location, ps_flags,
                system_constants_.flags);
      }
      for (uint32_t fetch_constant = 0; fetch_constant < 96; ++fetch_constant) {
        if (fetch_constant != 0 && fetch_constant != 95) {
          continue;
        }
        std::string name = fmt::format("xe_uniform_fetch_constants_vs.fetch_constants[{}]",
                                       fetch_constant >> 1);
        GLint location = GetFunctions().GetUniformLocation(program->program, name.c_str());
        GLuint value[4] = {};
        if (location >= 0) {
          GetFunctions().GetUniformuiv(program->program, location, value);
        }
        const uint32_t first = fetch_constant & 1 ? 2 : 0;
        const uint32_t word_0_index = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_constant * 2;
        fprintf(stderr,
                "  fetch[%u] loc=%d uniform=[%08X %08X] register=[%08X %08X]\n", fetch_constant,
                location, value[first], value[first + 1], register_file_->Get<uint32_t>(word_0_index),
                register_file_->Get<uint32_t>(word_0_index + 1));
      }
      for (uint32_t unit = 0; unit < 8; ++unit) {
        GetFunctions().ActiveTexture(GLenum(GL_TEXTURE0 + unit));
        GLint bound_2d = 0, bound_array = 0, sampler = 0;
        GetFunctions().GetIntegerv(GL_TEXTURE_BINDING_2D, &bound_2d);
        GetFunctions().GetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &bound_array);
        GetFunctions().GetIntegerv(GL_SAMPLER_BINDING, &sampler);
        fprintf(stderr, "  unit %u: 2d=%d array=%d sampler=%d\n", unit, bound_2d, bound_array,
                sampler);
      }
      GetFunctions().ActiveTexture(GLenum(active_unit));
      fflush(stderr);
    }
  }
  if (getenv("GLES_CLEAR_BEFORE_DRAW")) {
    GetFunctions().Disable(GL_SCISSOR_TEST);
    GetFunctions().ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    GetFunctions().Clear(GL_COLOR_BUFFER_BIT);
  }
  if (getenv("GLES_DOUBLE_DRAW") && getenv("GLES_DEBUG_VS_PS")) {
    GetFunctions().Disable(GL_SCISSOR_TEST);
    GetFunctions().ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    GetFunctions().Clear(GL_COLOR_BUFFER_BIT);
  }
  bool drawn = DrawPrimitives(processing_result);
  if (getenv("GLES_DOUBLE_DRAW") && getenv("GLES_DEBUG_VS_PS")) {
    readback("debug");
    GetFunctions().Disable(GL_SCISSOR_TEST);
    GetFunctions().ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    GetFunctions().Clear(GL_COLOR_BUFFER_BIT);
    readback("cleared");
    if (guest_program && guest_program != program) {
      GetFunctions().UseProgram(guest_program->program);
      SetStageConstants(*guest_program, *vertex_shader, pixel_shader);
      BindTextures(*guest_program, *vertex_shader, pixel_shader);
      BindVertexMemoryTextures(*guest_program, processing_result.guest_index_base, index_bytes);
      drawn = DrawPrimitives(processing_result);
      readback("guest");
    }
  } else {
    readback("guest");
  }
  GetFunctions().BindSampler(0, 0);
  return drawn;
}

bool GlesCommandProcessor::DrawPrimitives(
    const PrimitiveProcessor::ProcessingResult& processing_result) {
  const GLenum host_primitive = PrimitiveTypeToGL(processing_result.host_primitive_type);
  const GLenum index_type = processing_result.host_index_format == xenos::IndexFormat::kInt16
                                ? GL_UNSIGNED_SHORT
                                : GL_UNSIGNED_INT;
  // Drain errors from the binding work so the check below attributes only the draw.
  while (GetFunctions().GetError() != GL_NO_ERROR) {
  }
  switch (processing_result.index_buffer_type) {
    case PrimitiveProcessor::ProcessedIndexBufferType::kNone: {
      GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      if (getenv("GLES_FORCE_INDEXED")) {
        // The guest auto-indexed path relies on gl_VertexID; check whether the
        // backend handles it by feeding the same sequence as indices instead.
        std::vector<uint32_t> indices(processing_result.host_draw_vertex_count);
        for (uint32_t i = 0; i < indices.size(); ++i) {
          indices[i] = i;
        }
        GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, guest_index_buffer_);
        GetFunctions().BufferData(GL_ELEMENT_ARRAY_BUFFER,
                                  GLsizeiptr(indices.size() * sizeof(uint32_t)), indices.data(),
                                  GL_STREAM_DRAW);
        GetFunctions().DrawElements(host_primitive, GLsizei(indices.size()), GL_UNSIGNED_INT,
                                    nullptr);
        break;
      }
      GetFunctions().DrawArrays(host_primitive, 0, GLsizei(processing_result.host_draw_vertex_count));
    } break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
      const uint32_t guest_index_base = processing_result.guest_index_base;
      const uint32_t index_size = index_type == GL_UNSIGNED_SHORT ? 2 : 4;
      const uint32_t index_bytes = processing_result.host_draw_vertex_count * index_size;
      if (pipeline_cache_->uses_texture_memory()) {
        // ES 3.0: guest memory is not bound as a buffer, so upload the raw
        // guest indices. The vertex shader endian-swaps gl_VertexIndex exactly
        // like with the guest buffer bound directly on the storage-buffer path.
        GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, guest_index_buffer_);
        GetFunctions().BufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(index_bytes),
                         memory_->TranslatePhysical(guest_index_base), GL_STREAM_DRAW);
        GetFunctions().DrawElements(host_primitive, GLsizei(processing_result.host_draw_vertex_count),
                           index_type, nullptr);
        break;
      }
      const uint32_t slice_size_log2 = shared_memory_->slice_size_log2();
      if ((guest_index_base & ((uint32_t(1) << slice_size_log2) - 1)) + index_bytes >
          (uint32_t(1) << slice_size_log2)) {
        static bool logged = false;
        if (!logged) {
          logged = true;
          REXGPU_ERROR("GLES draw: index buffer crosses a shared memory binding boundary");
        }
        return false;
      }
      const GLuint buffer = shared_memory_->buffer_for_address(guest_index_base);
      const GLintptr offset = shared_memory_->offset_for_address(guest_index_base);
      GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
      GetFunctions().DrawElements(host_primitive, GLsizei(processing_result.host_draw_vertex_count),
                         index_type, reinterpret_cast<const void*>(offset));
    } break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted: {
      if (!primitive_processor_->UploadConvertedIndexBuffers()) {
        return false;
      }
      const auto buffer = primitive_processor_->GetConvertedIndexBuffer(
          processing_result.host_index_buffer_handle);
      GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer.first);
      GetFunctions().DrawElements(host_primitive, GLsizei(processing_result.host_draw_vertex_count),
                         index_type, reinterpret_cast<const void*>(buffer.second));
    } break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA: {
      const auto buffer = primitive_processor_->GetBuiltinIndexBuffer(
          processing_result.host_index_buffer_handle);
      GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer.first);
      GetFunctions().DrawElements(host_primitive, GLsizei(processing_result.host_draw_vertex_count),
                         index_type, reinterpret_cast<const void*>(buffer.second));
    } break;
    default:
      REXGPU_ERROR("GLES draw: unsupported index buffer type");
      return false;
  }
  const GLenum draw_error = GetFunctions().GetError();
  if (draw_error != GL_NO_ERROR) {
    GLint current_program = 0;
    GetFunctions().GetIntegerv(0x8B8D, &current_program);
    REXGPU_ERROR(
        "GLES draw: GL error 0x{:X} (program={}, prim={}, count={}, index_type={}, type={})",
        uint32_t(draw_error), current_program, uint32_t(host_primitive),
        processing_result.host_draw_vertex_count, uint32_t(index_type),
        uint32_t(processing_result.index_buffer_type));
    return false;
  }
  return true;
}

void GlesCommandProcessor::ApplyRenderState(reg::RB_DEPTHCONTROL normalized_depth_control,
                                            uint32_t normalized_color_mask,
                                            bool primitive_polygonal) {
  const RegisterFile& regs = *register_file_;
  if (getenv("GLES_FORCE_STATE")) {
    GetFunctions().Disable(GL_DEPTH_TEST);
    GetFunctions().Disable(GL_STENCIL_TEST);
    GetFunctions().Disable(GL_BLEND);
    GetFunctions().Disable(GL_CULL_FACE);
    GetFunctions().Disable(GL_SCISSOR_TEST);
    GetFunctions().ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    return;
  }

  if (normalized_depth_control.z_enable) {
    GetFunctions().Enable(GL_DEPTH_TEST);
    GetFunctions().DepthFunc(CompareFuncToGL(normalized_depth_control.zfunc));
    GetFunctions().DepthMask(normalized_depth_control.z_write_enable ? GL_TRUE : GL_FALSE);
  } else {
    GetFunctions().Disable(GL_DEPTH_TEST);
    GetFunctions().DepthMask(GLenum(GL_FALSE));
  }

  if (normalized_depth_control.stencil_enable) {
    GetFunctions().Enable(GL_STENCIL_TEST);
    auto stencil_front = regs.Get<reg::RB_STENCILREFMASK>();
    auto stencil_back = regs.Get<reg::RB_STENCILREFMASK>(
        primitive_polygonal && normalized_depth_control.backface_enable
            ? XE_GPU_REG_RB_STENCILREFMASK_BF
            : XE_GPU_REG_RB_STENCILREFMASK);
    GetFunctions().StencilFuncSeparate(GL_FRONT, CompareFuncToGL(normalized_depth_control.stencilfunc),
                              GLint(stencil_front.stencilref), stencil_front.stencilmask);
    GetFunctions().StencilOpSeparate(GL_FRONT, StencilOpToGL(normalized_depth_control.stencilfail),
                            StencilOpToGL(normalized_depth_control.stencilzfail),
                            StencilOpToGL(normalized_depth_control.stencilzpass));
    GetFunctions().StencilMaskSeparate(GL_FRONT, stencil_front.stencilwritemask);
    if (primitive_polygonal && normalized_depth_control.backface_enable) {
      GetFunctions().StencilFuncSeparate(GL_BACK, CompareFuncToGL(normalized_depth_control.stencilfunc_bf),
                                GLint(stencil_back.stencilref), stencil_back.stencilmask);
      GetFunctions().StencilOpSeparate(GL_BACK, StencilOpToGL(normalized_depth_control.stencilfail_bf),
                              StencilOpToGL(normalized_depth_control.stencilzfail_bf),
                              StencilOpToGL(normalized_depth_control.stencilzpass_bf));
      GetFunctions().StencilMaskSeparate(GL_BACK, stencil_back.stencilwritemask);
    } else {
      GetFunctions().StencilFuncSeparate(GL_BACK, CompareFuncToGL(normalized_depth_control.stencilfunc),
                                GLint(stencil_front.stencilref), stencil_front.stencilmask);
      GetFunctions().StencilOpSeparate(GL_BACK, StencilOpToGL(normalized_depth_control.stencilfail),
                              StencilOpToGL(normalized_depth_control.stencilzfail),
                              StencilOpToGL(normalized_depth_control.stencilzpass));
      GetFunctions().StencilMaskSeparate(GL_BACK, stencil_front.stencilwritemask);
    }
  } else {
    GetFunctions().Disable(GL_STENCIL_TEST);
  }

  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal && (pa_su_sc_mode_cntl.cull_front || pa_su_sc_mode_cntl.cull_back)) {
    GetFunctions().Enable(GL_CULL_FACE);
    if (pa_su_sc_mode_cntl.cull_front && pa_su_sc_mode_cntl.cull_back) {
      GetFunctions().CullFace(GL_FRONT_AND_BACK);
    } else if (pa_su_sc_mode_cntl.cull_front) {
      GetFunctions().CullFace(GL_FRONT);
    } else {
      GetFunctions().CullFace(GL_BACK);
    }
    GetFunctions().FrontFace(pa_su_sc_mode_cntl.face ? GL_CW : GL_CCW);
  } else {
    GetFunctions().Disable(GL_CULL_FACE);
  }

  // Only the first color render target is currently attached.
  const uint32_t color_mask = normalized_color_mask & 0b1111;
  if (color_mask) {
    GetFunctions().ColorMask((color_mask & 1) ? GL_TRUE : GL_FALSE, (color_mask & 2) ? GL_TRUE : GL_FALSE,
                    (color_mask & 4) ? GL_TRUE : GL_FALSE, (color_mask & 8) ? GL_TRUE : GL_FALSE);
    auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
    GetFunctions().BlendFuncSeparate(BlendFactorToGL(blend_control.color_srcblend),
                            BlendFactorToGL(blend_control.color_destblend),
                            BlendFactorToGL(blend_control.alpha_srcblend),
                            BlendFactorToGL(blend_control.alpha_destblend));
    GetFunctions().BlendEquationSeparate(BlendOpToGL(blend_control.color_comb_fcn),
                                BlendOpToGL(blend_control.alpha_comb_fcn));
    GetFunctions().BlendColor(regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
                     regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                     regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
                     regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA));
    GetFunctions().Enable(GL_BLEND);
  } else {
    GetFunctions().ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    GetFunctions().Disable(GL_BLEND);
  }

  if (primitive_polygonal) {
    float slope_factor, constant_factor;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, slope_factor,
                                             constant_factor);
    if (slope_factor != 0.0f || constant_factor != 0.0f) {
      GetFunctions().Enable(GL_POLYGON_OFFSET_FILL);
      GetFunctions().PolygonOffset(slope_factor, constant_factor);
    } else {
      GetFunctions().Disable(GL_POLYGON_OFFSET_FILL);
    }
  } else {
    GetFunctions().Disable(GL_POLYGON_OFFSET_FILL);
  }
}

void GlesCommandProcessor::UpdateSystemConstantValues(
    const PrimitiveProcessor::ProcessingResult& processing_result,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask) {
  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  auto vgt_max_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  auto vgt_min_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);

  uint32_t flags = 0;
  if (processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA ||
      processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
      processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    if (processing_result.host_primitive_reset_enabled &&
        processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  if (draw_util::IsPrimitivePolygonal(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  flags |= uint32_t(rb_surface_info.msaa_samples) << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_format ==
        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
  }
  system_constants_.flags = getenv("GLES_ZERO_FLAGS") ? 0u : flags;

  if (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad) {
    system_constants_.vertex_index_load_address = processing_result.guest_index_base;
  } else {
    system_constants_.vertex_index_load_address = 0;
  }
  system_constants_.vertex_index_reset =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset)
          ? regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
          : 0;
  system_constants_.compute_memexport_vertex_count = 0;
  xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 &&
      guest_index_endian != xenos::Endian::kNone && guest_index_endian != xenos::Endian::k8in16) {
    guest_index_endian =
        guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  system_constants_.vertex_index_endian =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)
          ? guest_index_endian
          : processing_result.host_shader_index_endian;
  system_constants_.line_loop_closing_index = processing_result.line_loop_closing_index;
  system_constants_.vertex_base_index = vgt_indx_offset;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      std::memcpy(user_clip_plane_write_ptr,
                  &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4], 4 * sizeof(float));
      user_clip_plane_write_ptr += 4;
    }
  }

  system_constants_.tessellation_factor_range_min =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  system_constants_.tessellation_factor_range_max =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;

  for (uint32_t i = 0; i < 3; ++i) {
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    system_constants_.point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    system_constants_.point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    system_constants_.point_constant_diameter[0] = float(pa_su_point_size.width) * (2.0f / 16.0f);
    system_constants_.point_constant_diameter[1] = float(pa_su_point_size.height) * (2.0f / 16.0f);
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        1.0f / std::max(viewport_info.xy_extent[0], uint32_t(1));
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        1.0f / std::max(viewport_info.xy_extent[1], uint32_t(1));
  }

  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
      const uint32_t texture_signs_shift = 8 * (texture_index & 3);
      const uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      const uint32_t texture_signs_mask = ((UINT32_C(1) << 8) - 1) << texture_signs_shift;
      texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) |
                           (uint32_t(texture_signs) << texture_signs_shift);
    }
  }
  system_constants_.textures_resolution_scaled = 0;
  std::memset(system_constants_.texture_swizzles, 0, sizeof(system_constants_.texture_swizzles));

  system_constants_.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  system_constants_.alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8) : 0;

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    int32_t color_exp_bias = color_info.color_exp_bias;
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) = UINT32_C(0x3F800000) + (color_exp_bias << 23);
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }
  (void)normalized_color_mask;
}

void GlesCommandProcessor::SetStageConstants(const GlesProgram& program,
                                             const GlesShader& vertex_shader,
                                             const GlesShader* pixel_shader) {
  const RegisterFile& regs = *register_file_;

  uint32_t bool_loop_constants[8 + 32];
  std::memcpy(bool_loop_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
              sizeof(bool_loop_constants));
  uint32_t fetch_constants[32 * 6];
  std::memcpy(fetch_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
              sizeof(fetch_constants));

  const uint8_t* system_constants = reinterpret_cast<const uint8_t*>(&system_constants_);
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const GlesProgram::StageUniforms& uniforms = program.stage_uniforms[stage];
    if (stage == 1 && (!pixel_shader || getenv("GLES_SKIP_PS_CONSTANTS"))) {
      continue;
    }
    for (uint32_t i = 0; i < kSystemConstantUniformCount; ++i) {
      const int32_t location = uniforms.system[i];
      if (location < 0) {
        continue;
      }
      const SystemConstantUniform& uniform = kSystemConstantUniforms[i];
      const uint8_t* value = system_constants + uniform.offset;
      switch (uniform.type) {
        case SystemConstantType::kUInt:
          GetFunctions().Uniform1ui(location, *reinterpret_cast<const uint32_t*>(value));
          break;
        case SystemConstantType::kInt:
          GetFunctions().Uniform1i(location, *reinterpret_cast<const int32_t*>(value));
          break;
        case SystemConstantType::kFloat:
          GetFunctions().Uniform1f(location, *reinterpret_cast<const float*>(value));
          break;
        case SystemConstantType::kVec2:
          GetFunctions().Uniform2fv(location, 1, reinterpret_cast<const float*>(value));
          break;
        case SystemConstantType::kVec3:
          GetFunctions().Uniform3fv(location, 1, reinterpret_cast<const float*>(value));
          break;
        case SystemConstantType::kVec4:
          GetFunctions().Uniform4fv(location, 1, reinterpret_cast<const float*>(value));
          break;
        case SystemConstantType::kUVec2:
          GetFunctions().Uniform2uiv(location, 1, reinterpret_cast<const uint32_t*>(value));
          break;
        case SystemConstantType::kUVec4Array:
          GetFunctions().Uniform4uiv(location, GLsizei(uniform.count),
                                     reinterpret_cast<const uint32_t*>(value));
          break;
        case SystemConstantType::kVec4Array:
          GetFunctions().Uniform4fv(location, GLsizei(uniform.count),
                                    reinterpret_cast<const float*>(value));
          break;
      }
    }
    if (uniforms.bool_constants >= 0) {
      GetFunctions().Uniform4uiv(uniforms.bool_constants, 2, bool_loop_constants);
    }
    if (uniforms.loop_constants >= 0) {
      GetFunctions().Uniform4uiv(uniforms.loop_constants, 8, bool_loop_constants + 8);
    }
    if (uniforms.fetch_constants >= 0) {
      GetFunctions().Uniform4uiv(uniforms.fetch_constants, 48, fetch_constants);
    }
  }

  // Float constants, tightly packed in the order the shader expects.
  auto upload_float_constants = [&](const Shader::ConstantRegisterMap& constant_map,
                                    uint32_t register_base, uint32_t stage) {
    const int32_t location = program.stage_uniforms[stage].float_constants;
    if (location < 0 || !constant_map.float_count) {
      return;
    }
    float constants[256 * 4];
    size_t offset = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bitmap = constant_map.float_bitmap[i];
      uint32_t index;
      while (rex::bit_scan_forward(bitmap, &index)) {
        bitmap &= ~(uint64_t(1) << index);
        std::memcpy(constants + offset, &regs[register_base + (i << 8) + (index << 2)],
                    4 * sizeof(float));
        offset += 4;
      }
    }
    GetFunctions().Uniform4fv(location, GLsizei(constant_map.float_count), constants);
  };
  upload_float_constants(vertex_shader.constant_register_map(), XE_GPU_REG_SHADER_CONSTANT_000_X,
                         0);
  if (pixel_shader) {
    upload_float_constants(pixel_shader->constant_register_map(),
                           XE_GPU_REG_SHADER_CONSTANT_256_X, 1);
  }
}

bool GlesCommandProcessor::BindTextures(const GlesProgram& program,
                                        const GlesShader& vertex_shader,
                                        const GlesShader* pixel_shader) {
  (void)vertex_shader;
  (void)pixel_shader;
  if (getenv("GLES_ONLY_WHITE_UNIT0")) {
    if (!debug_white_texture_) {
      const uint8_t white[4] = {255, 255, 255, 255};
      GetFunctions().GenTextures(1, &debug_white_texture_);
      GetFunctions().BindTexture(GL_TEXTURE_2D_ARRAY, debug_white_texture_);
      GetFunctions().TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA,
                                GL_UNSIGNED_BYTE, white);
      GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      GetFunctions().TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    }
    BindTextureUnit(0, GL_TEXTURE_2D_ARRAY, debug_white_texture_);
    GetFunctions().BindSampler(0, 0);
    return true;
  }
  if (getenv("GLES_LOG_DRAW_TEXTURES")) {
    static uint32_t draws_logged = 0;
    if (draws_logged < 40) {
      ++draws_logged;
      fprintf(stderr, "DRAWTEX program=%u", program.program);
      for (const GlesProgram::SamplerBinding& sampler : program.samplers) {
        const xenos::xe_gpu_texture_fetch_t fetch =
            register_file_->GetTextureFetch(sampler.fetch_constant);
        fprintf(stderr, " [fc%u page=0x%X %ux%u fmt%u]", sampler.fetch_constant,
                uint32_t(fetch.base_address), uint32_t(fetch.size_2d.width) + 1,
                uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.format));
      }
      fprintf(stderr, "\n");
      fflush(stderr);
    }
  }
  uint32_t texture_unit = 0;
  static std::set<GLuint> logged_texture_programs;
  const bool log_textures =
      getenv("GLES_LOG_BINDINGS") && logged_texture_programs.insert(program.program).second;
  if (program.samplers.size() > kGuestTextureUnitCount) {
    static bool logged_overflow = false;
    if (!logged_overflow) {
      logged_overflow = true;
      REXGPU_ERROR("GLES program {} uses {} samplers, only {} unit(s) are reserved for them",
                   program.program, uint32_t(program.samplers.size()),
                   uint32_t(kGuestTextureUnitCount));
    }
  }
  for (const GlesProgram::SamplerBinding& sampler : program.samplers) {
    if (texture_unit >= kGuestTextureUnitCount) {
      break;
    }
    const GLenum target = GetTextureTargetForDimension(sampler.dimension);
    GLuint texture = texture_cache_->GetActiveBindingTexture(
        sampler.fetch_constant, sampler.dimension, sampler.is_signed);
    if (getenv("GLES_DEBUG_TEXTURE") && debug_white_texture_) {
      texture = debug_white_texture_;
    }
    BindTextureUnit(texture_unit, target, texture);
    if (!getenv("GLES_NO_SWIZZLE")) {
      GlesTextureCache::ApplySwizzle(
          target, texture_cache_->GetActiveBindingSwizzle(sampler.fetch_constant));
    }
    GlesShader::SamplerBinding shader_sampler_binding;
    shader_sampler_binding.fetch_constant = sampler.fetch_constant;
    shader_sampler_binding.mag_filter = sampler.mag_filter;
    shader_sampler_binding.min_filter = sampler.min_filter;
    shader_sampler_binding.mip_filter = sampler.mip_filter;
    shader_sampler_binding.aniso_filter = sampler.aniso_filter;
    const GLuint sampler_object =
        getenv("GLES_NO_SAMPLER")
            ? 0
            : texture_cache_->GetOrCreateSampler(
                  texture_cache_->GetSamplerParameters(shader_sampler_binding));
    GetFunctions().BindSampler(texture_unit, sampler_object);
    if (sampler.uniform_location >= 0) {
      GetFunctions().Uniform1i(sampler.uniform_location, GLint(texture_unit));
    }
    if (log_textures) {
      GLint value = -1;
      if (sampler.uniform_location >= 0) {
        GetFunctions().GetUniformiv(program.program, sampler.uniform_location, &value);
      }
      const xenos::xe_gpu_texture_fetch_t fetch =
          register_file_->GetTextureFetch(sampler.fetch_constant);
      fprintf(stderr,
              "  guest sampler[%u] loc=%d fc=%u tex=%u target=0x%X uniform=%d "
              "fetch=%ux%u fmt=%u dim=%u tiled=%u stacked=%u page=0x%X\n",
              texture_unit, sampler.uniform_location, sampler.fetch_constant, texture,
              uint32_t(target), value, uint32_t(fetch.size_2d.width) + 1,
              uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.format),
              uint32_t(fetch.dimension), uint32_t(fetch.tiled), uint32_t(fetch.stacked),
              uint32_t(fetch.base_address));
      fflush(stderr);
    }
    ++texture_unit;
  }
  if (program.debug_sampler_location >= 0 && debug_white_texture_) {
    GLuint debug_texture = debug_white_texture_;
    if (getenv("GLES_SAMPLE_GUEST")) {
      debug_texture =
          texture_cache_->GetActiveBindingTexture(0, xenos::FetchOpDimension::k2D, false);
    }
    BindTextureUnit(kDebugSamplerUnit, GL_TEXTURE_2D_ARRAY, debug_texture);
    GetFunctions().BindSampler(kDebugSamplerUnit, 0);
    GetFunctions().Uniform1i(program.debug_sampler_location, GLint(kDebugSamplerUnit));
  }
  return true;
}

void GlesCommandProcessor::UploadMemoryUniform(GLint location, const void* data,
                                               uint32_t dword_count) {
  if (location < 0) {
    return;
  }
  const uint32_t count = SpirvShaderTranslator::Features::kVertexMemoryUniformCount;
  memory_uniform_staging_.assign(count, 0);
  const uint32_t copy_count = std::min(dword_count, count);
  if (data && copy_count) {
    std::memcpy(memory_uniform_staging_.data(), data, size_t(copy_count) * 4);
  }
  GetFunctions().Uniform1uiv(location, GLsizei(count), memory_uniform_staging_.data());
}

GLuint GlesCommandProcessor::UploadUintTexture(VertexMemoryTexture& destination, const void* data,
                                             uint32_t dword_count, bool always_upload) {
  constexpr uint32_t kWidth = 2048;
  if (!destination.texture) {
    GetFunctions().GenTextures(1, &destination.texture);
    if (!destination.texture) {
      REXGPU_ERROR("GLES: failed to create a vertex memory texture");
      return 0;
    }
    GetFunctions().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GetFunctions().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GetFunctions().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GetFunctions().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  const uint32_t height = std::max(1u, (dword_count + kWidth - 1) / kWidth);
  // Debug knob: only the first dwords are uploaded, to check whether the size of
  // the allocation is what makes the vertex shader's fetches fail.
  uint32_t upload_dwords = dword_count;
  if (const char* max_dwords = getenv("GLES_VFETCH_MAX_DWORDS")) {
    upload_dwords = std::min(upload_dwords, uint32_t(atoi(max_dwords)));
  }
  const uint32_t upload_height = std::max(1u, (upload_dwords + kWidth - 1) / kWidth);
  const uint64_t data_hash =
      data && dword_count ? XXH3_64bits(data, size_t(dword_count) * 4) : 0;
  if (!always_upload && destination.texture && destination.width == kWidth &&
      destination.height == height && destination.data_size == dword_count &&
      destination.data_hash == data_hash) {
    return destination.texture;
  }
  destination.data_hash = data_hash;
  destination.data_size = dword_count;
  // Uploads never run on a unit that a shader samples: leaving a 2D integer
  // texture on a guest sampler unit (or vice versa) makes ANGLE resolve the
  // unit to the zero texture. The caller binds the finished texture on the unit
  // the shader reads it from.
  GetFunctions().ActiveTexture(GLenum(GL_TEXTURE0 + kScratchTextureUnit));
  GetFunctions().BindTexture(GL_TEXTURE_2D, destination.texture);
  if (destination.width != kWidth || destination.height != upload_height) {
    GetFunctions().TexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, GLsizei(kWidth), GLsizei(upload_height),
                              0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    destination.width = kWidth;
    destination.height = upload_height;
  }
  memory_texture_staging_.assign(size_t(kWidth) * upload_height, 0);
  if (data && upload_dwords) {
    std::memcpy(memory_texture_staging_.data(), data, size_t(upload_dwords) * 4);
  }
  GetFunctions().TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(kWidth), GLsizei(upload_height),
                               GL_RED_INTEGER, GL_UNSIGNED_INT, memory_texture_staging_.data());
  {
    const GLenum upload_error = GetFunctions().GetError();
    if (upload_error != GL_NO_ERROR) {
      static uint32_t upload_errors = 0;
      if (upload_errors < 8) {
        ++upload_errors;
        GLint max_texture_size = 0;
        GetFunctions().GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
        REXGPU_ERROR("GLES vertex memory: upload of {}x{} dwords failed with 0x{:X} "
                     "(max texture size {})",
                     kWidth, height, uint32_t(upload_error), max_texture_size);
      }
    }
  }
  return destination.texture;
}

bool GlesCommandProcessor::BindVertexMemoryTextures(const GlesProgram& program,
                                                    uint32_t index_guest_base,
                                                    uint32_t index_bytes) {
  if (!pipeline_cache_->uses_texture_memory()) {
    return true;
  }
  const bool log_bindings = getenv("GLES_LOG_BINDINGS") != nullptr;
  static std::set<GLuint> logged_programs;
  const bool log_this = log_bindings && logged_programs.insert(program.program).second;
  if (log_this) {
    fprintf(stderr, "BINDINGS program=%u samplers=%zu vertex_memory=%zu\n", program.program,
            program.samplers.size(), program.vertex_memory_samplers.size());
    for (size_t i = 0; i < program.samplers.size(); ++i) {
      const GlesProgram::SamplerBinding& sampler = program.samplers[i];
      GLint value = -1;
      if (sampler.uniform_location >= 0) {
        GetFunctions().GetUniformiv(program.program, sampler.uniform_location, &value);
      }
      fprintf(stderr, "  sampler[%zu] loc=%d unit=%d fc=%u dim=%u vecs=%llu\n", i,
              sampler.uniform_location, value, sampler.fetch_constant,
              uint32_t(sampler.dimension), (unsigned long long)sampler.is_signed);
    }
    fflush(stderr);
  }
  // Place the vertex memory textures after the guest samplers of this program,
  // and never below kMemoryUnitFloor: the index/vertex fetch textures are 2D
  // integer textures, and leaving one on a unit that a later program samples
  // with a sampler2DArray makes ANGLE treat the unit as unloadable (it resolves
  // one texture per unit), so the draw samples the zero texture.
  const uint32_t memory_unit_base =
      std::max(uint32_t(program.samplers.size()), kMemoryUnitFloor);
  uint32_t texture_unit = memory_unit_base;
  if (const char* override_unit = getenv("GLES_MEMORY_UNIT_OVERRIDE")) {
    texture_unit = uint32_t(atoi(override_unit));
  }
  for (const GlesProgram::VertexMemorySamplerBinding& sampler :
       program.vertex_memory_samplers) {
    if (sampler.uniform_array) {
      // Plain uniform array: upload the dword window starting at the fetch
      // constant's base address. The shader indexes it with the address minus
      // that base, so element 0 is the first dword of the buffer.
      const void* data = nullptr;
      uint32_t dwords = 0;
      if (sampler.is_index) {
        if (index_bytes) {
          const uint32_t aligned_base = index_guest_base & ~uint32_t(3);
          dwords = (index_bytes + (index_guest_base - aligned_base) + 3) >> 2;
          data = memory_->TranslatePhysical(aligned_base);
        }
      } else {
        xenos::xe_gpu_vertex_fetch_t vfetch =
            register_file_->GetVertexFetch(sampler.fetch_constant);
        if (vfetch.type == xenos::FetchConstantType::kVertex && vfetch.size &&
            vfetch.address < (SharedMemory::kBufferSize >> 2)) {
          data = memory_->TranslatePhysical(vfetch.address << 2);
          dwords = vfetch.size;
        }
      }
      UploadMemoryUniform(sampler.uniform_location, data, dwords);
      if (log_this) {
        fprintf(stderr, "  memory uniform[%s] fc=%u loc=%d dwords=%u\n",
                sampler.is_index ? "index" : "vertex", sampler.fetch_constant,
                sampler.uniform_location, dwords);
        fflush(stderr);
      }
      continue;
    }
    if (texture_unit >= memory_unit_base + kVertexMemoryUnitCount &&
        !getenv("GLES_MEMORY_UNIT_OVERRIDE")) {
      break;
    }
    GLuint texture = 0;
    if (sampler.is_index) {
      if (index_bytes) {
        const uint32_t aligned_base = index_guest_base & ~uint32_t(3);
        const uint32_t dwords =
            (index_bytes + (index_guest_base - aligned_base) + 3) >> 2;
        texture = UploadUintTexture(index_memory_texture_,
                                    memory_->TranslatePhysical(aligned_base), dwords);
      }
    } else {
      xenos::xe_gpu_vertex_fetch_t vfetch =
          register_file_->GetVertexFetch(sampler.fetch_constant);
      if (vfetch.type == xenos::FetchConstantType::kVertex && vfetch.size &&
          vfetch.address < (SharedMemory::kBufferSize >> 2)) {
        if (getenv("GLES_LOG_VFETCH_VERTS")) {
          static uint32_t vfetch_dumps = 0;
          if (vfetch_dumps < 40) {
            ++vfetch_dumps;
            const uint32_t* source = static_cast<const uint32_t*>(static_cast<const void*>(
                memory_->TranslatePhysical(uint32_t(vfetch.address) << 2)));
            fprintf(stderr, "VFETCHDUMP fetch=%u base=0x%X size=%u program=%u\n",
                    sampler.fetch_constant, uint32_t(vfetch.address), uint32_t(vfetch.size),
                    program.program);
            for (uint32_t vertex = 0; vertex < 4; ++vertex) {
              fprintf(stderr, "  v%u:", vertex);
              for (uint32_t dword = 0; dword < 9; ++dword) {
                const uint32_t value = source[vertex * 9 + dword];
                float as_float;
                std::memcpy(&as_float, &value, 4);
                fprintf(stderr, " %08X(%.3f)", value, as_float);
              }
              fprintf(stderr, "\n");
            }
            fflush(stderr);
          }
        }
        texture = UploadUintTexture(
            vfetch_memory_textures_[sampler.fetch_constant],
            memory_->TranslatePhysical(vfetch.address << 2), vfetch.size);
      } else {
        static bool logged = false;
        if (!logged) {
          logged = true;
          REXGPU_WARN("GLES: vertex fetch constant {} is invalid", sampler.fetch_constant);
        }
      }
    }
    if (!texture) {
      if (!dummy_memory_texture_.texture) {
        static const uint32_t kZero = 0;
        UploadUintTexture(dummy_memory_texture_, &kZero, 1);
      }
      texture = dummy_memory_texture_.texture;
    }
    BindTextureUnit(texture_unit, GL_TEXTURE_2D, texture);
    GetFunctions().BindSampler(texture_unit, 0);
    if (sampler.uniform_location >= 0) {
      GetFunctions().Uniform1i(sampler.uniform_location, GLint(texture_unit));
    }
    if (log_this) {
      GLint value = -1;
      if (sampler.uniform_location >= 0) {
        GetFunctions().GetUniformiv(program.program, sampler.uniform_location, &value);
      }
      xenos::xe_gpu_vertex_fetch_t vfetch{};
      if (!sampler.is_index) {
        vfetch = register_file_->GetVertexFetch(sampler.fetch_constant);
      }
      fprintf(stderr,
              "  vfetch[%s] fc=%u loc=%d unit=%u uniform=%d tex=%u "
              "address=0x%X size=%u type=%u index_bytes=%u raw=[%08X %08X]\n",
              sampler.is_index ? "index" : "vertex", sampler.fetch_constant,
              sampler.uniform_location, texture_unit, value, texture, uint32_t(vfetch.address),
              uint32_t(vfetch.size), uint32_t(vfetch.type), index_bytes,
              sampler.is_index ? 0 : register_file_->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                                            sampler.fetch_constant * 2],
              sampler.is_index ? 0 : register_file_->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                                            sampler.fetch_constant * 2 + 1]);
      if (!sampler.is_index) {
        const uint32_t* source = static_cast<const uint32_t*>(
            static_cast<const void*>(memory_->TranslatePhysical(uint32_t(vfetch.address) << 2)));
        fprintf(stderr, "    data:");
        for (uint32_t dword = 0; dword < 40; ++dword) {
          fprintf(stderr, " %08X", source[dword]);
        }
        fprintf(stderr, "\n");
      }
      fflush(stderr);
    }
    ++texture_unit;
  }
  return true;
}

void GlesCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);
  // The texture cache keeps its bindings until it is told that the guest changed
  // a texture fetch constant; without this every draw samples whichever texture
  // was bound to the fetch constant first.
  if (texture_cache_ && index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    texture_cache_->TextureFetchConstantWritten(
        (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
  }
}

bool GlesCommandProcessor::IssueCopy() {
  static uint32_t logged_copies = 0;
  if (logged_copies < 5) {
    ++logged_copies;
    REXGPU_DEBUG("GLES IssueCopy: begin");
  }
  uint32_t written_address = 0;
  uint32_t written_length = 0;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                     written_length)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXGPU_WARN("GLES: resolve failed");
    }
    return false;
  }
  static uint32_t logged_copies_done = 0;
  if (logged_copies_done < 5) {
    ++logged_copies_done;
    REXGPU_DEBUG("GLES IssueCopy: done (written 0x{:08X}+0x{:X})", written_address,
                written_length);
  }
  return true;
}

GLuint GlesCommandProcessor::GetWindowDrawableWidth() const {
  if (!graphics_system_ || !graphics_system_->presenter()) {
    return 0;
  }
  return static_cast<ui::gles::GlesPresenter*>(graphics_system_->presenter())
      ->window_drawable_width();
}

GLuint GlesCommandProcessor::GetWindowDrawableHeight() const {
  if (!graphics_system_ || !graphics_system_->presenter()) {
    return 0;
  }
  return static_cast<ui::gles::GlesPresenter*>(graphics_system_->presenter())
      ->window_drawable_height();
}

void GlesCommandProcessor::PresentSwapTexture(GLuint texture, GLenum target, uint32_t width,
                                              uint32_t height) {
  if (!texture || !width || !height) {
    return;
  }
  const GLuint drawable_width = GetWindowDrawableWidth();
  const GLuint drawable_height = GetWindowDrawableHeight();
  if (!drawable_width || !drawable_height) {
    return;
  }
  GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, present_framebuffer_);
  if (target == GL_TEXTURE_2D_ARRAY) {
    GetFunctions().FramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, 0);
  } else {
    GetFunctions().FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, texture, 0);
  }
  if (GetFunctions().CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXGPU_WARN("GLES: presentation framebuffer is incomplete");
    }
    GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }
  GetFunctions().BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, present_framebuffer_);
  GetFunctions().Viewport(0, 0, GLsizei(drawable_width), GLsizei(drawable_height));
  if (getenv("GLES_DEBUG_QUAD")) {
    GLuint debug_vs = 0, debug_fs = 0;
    const char* debug_vs_source =
        "#version 300 es\n"
        "void main() {\n"
        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    const char* debug_fs_source =
        "#version 300 es\n"
        "precision mediump float;\n"
        "out vec4 o;\n"
        "void main() { o = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    if (CompileGlslEsShader(GL_VERTEX_SHADER, debug_vs_source, debug_vs) &&
        CompileGlslEsShader(GL_FRAGMENT_SHADER, debug_fs_source, debug_fs)) {
      debug_program_ = GetFunctions().CreateProgram();
      GetFunctions().AttachShader(debug_program_, debug_vs);
      GetFunctions().AttachShader(debug_program_, debug_fs);
      GetFunctions().LinkProgram(debug_program_);
      GetFunctions().DeleteShader(debug_vs);
      GetFunctions().DeleteShader(debug_fs);
    }
  }

  GetFunctions().Disable(GL_SCISSOR_TEST);
  GetFunctions().Disable(GL_DEPTH_TEST);
  GetFunctions().Disable(GL_STENCIL_TEST);
  GetFunctions().Disable(GL_BLEND);
  GetFunctions().Disable(GL_CULL_FACE);
  GetFunctions().ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // The guest image is stored top-down; GL's window origin is bottom-left, so
  // present with an inverted destination rectangle.
  GetFunctions().BlitFramebuffer(0, 0, GLint(width), GLint(height), 0, GLint(drawable_height),
                        GLint(drawable_width), 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void GlesCommandProcessor::PresentRenderTargetTexture(GLuint texture, uint32_t width,
                                                      uint32_t height) {
  // The render target texture is Y-mirrored and may be taller than the guest
  // output; the active rows are at the top (guest row 0 at the last row).
  const uint32_t target_height = render_target_cache_ ? render_target_cache_->draw_height() : 0;
  if (!texture || !width || !height || !target_height) {
    return;
  }
  const GLuint drawable_width = GetWindowDrawableWidth();
  const GLuint drawable_height = GetWindowDrawableHeight();
  if (!drawable_width || !drawable_height) {
    return;
  }
  GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, present_framebuffer_);
  GetFunctions().FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  if (GetFunctions().CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }
  const GLint source_y0 = GLint(target_height) - GLint(height);
  const GLint source_y1 = GLint(target_height);
  GetFunctions().BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, present_framebuffer_);
  GetFunctions().Viewport(0, 0, GLsizei(drawable_width), GLsizei(drawable_height));
  if (getenv("GLES_DEBUG_QUAD")) {
    GLuint debug_vs = 0, debug_fs = 0;
    const char* debug_vs_source =
        "#version 300 es\n"
        "void main() {\n"
        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    const char* debug_fs_source =
        "#version 300 es\n"
        "precision mediump float;\n"
        "out vec4 o;\n"
        "void main() { o = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    if (CompileGlslEsShader(GL_VERTEX_SHADER, debug_vs_source, debug_vs) &&
        CompileGlslEsShader(GL_FRAGMENT_SHADER, debug_fs_source, debug_fs)) {
      debug_program_ = GetFunctions().CreateProgram();
      GetFunctions().AttachShader(debug_program_, debug_vs);
      GetFunctions().AttachShader(debug_program_, debug_fs);
      GetFunctions().LinkProgram(debug_program_);
      GetFunctions().DeleteShader(debug_vs);
      GetFunctions().DeleteShader(debug_fs);
    }
  }

  GetFunctions().Disable(GL_SCISSOR_TEST);
  GetFunctions().Disable(GL_DEPTH_TEST);
  GetFunctions().Disable(GL_STENCIL_TEST);
  GetFunctions().Disable(GL_BLEND);
  GetFunctions().Disable(GL_CULL_FACE);
  GetFunctions().ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  GetFunctions().BlitFramebuffer(0, source_y0, GLint(width), source_y1, 0, GLint(drawable_height),
                        GLint(drawable_width), 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void GlesCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                     uint32_t frontbuffer_height) {
  frame_draw_index_ = 0;
  if (getenv("GLES_LOG_FPS")) {
    static uint32_t swaps = 0;
    static auto swap_start = std::chrono::steady_clock::now();
    ++swaps;
    if (swaps % 30 == 1) {
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - swap_start).count();
      REXGPU_INFO("GLES swap {} ({:.1f} fps) frontbuffer={}x{}", swaps,
                  seconds > 0.0 ? double(swaps) / seconds : 0.0, frontbuffer_width,
                  frontbuffer_height);
    }
  }
  if (getenv("GLES_DEBUG_SWAP_READ") && render_target_cache_) {
    static uint32_t logged_swap_reads = 0;
    if (logged_swap_reads < 20) {
      ++logged_swap_reads;
      const uint32_t rt_w = render_target_cache_->draw_width();
      const uint32_t rt_h = render_target_cache_->draw_height();
      if (rt_w && render_target_cache_->draw_framebuffer()) {
        std::vector<uint8_t> pixels(size_t(rt_w) * 720 * 4);
        GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER,
                                       render_target_cache_->draw_framebuffer());
        GetFunctions().PixelStorei(GL_PACK_ALIGNMENT, 1);
        GetFunctions().ReadPixels(0, GLint(rt_h - 720), GLsizei(rt_w), 720, GL_RGBA,
                                  GL_UNSIGNED_BYTE, pixels.data());
        uint32_t nonzero = 0, checksum = 0, first = 0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
          uint32_t value;
          std::memcpy(&value, pixels.data() + i, 4);
          if (i == 0) first = value;
          if (value) {
            ++nonzero;
            checksum += value;
          }
        }
        REXGPU_INFO("GLES swap readback: {}x{} first={:08X} nonzero={} checksum=0x{:08X}", rt_w,
                    720, first, nonzero, checksum);
      }
    }
  }
  (void)frontbuffer_ptr;
  if (!graphics_system_) {
    return;
  }
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("GLES: no presenter for the swap");
    return;
  }
  auto* gles_presenter = static_cast<ui::gles::GlesPresenter*>(presenter);
  (void)gles_presenter;

  uint32_t swap_width = 0, swap_height = 0;
  xenos::TextureFormat swap_format = xenos::TextureFormat::k_8_8_8_8;
  GLuint swap_texture = 0;
  GLenum swap_target = GL_TEXTURE_2D_ARRAY;
  if (texture_cache_) {
    swap_texture = texture_cache_->RequestSwapTexture(swap_width, swap_height, swap_format);
  }
  const GLuint guest_output_width = swap_texture ? swap_width : frontbuffer_width;
  const GLuint guest_output_height = swap_texture ? swap_height : frontbuffer_height;

  const bool presented = presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, guest_output_width, guest_output_height,
      [this, swap_texture, swap_target, swap_width, swap_height, guest_output_width,
       guest_output_height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        (void)context;
        if (swap_texture) {
          PresentSwapTexture(swap_texture, swap_target, swap_width, swap_height);
        } else if (render_target_cache_) {
          PresentRenderTargetTexture(render_target_cache_->last_update_color_texture(),
                                     guest_output_width, guest_output_height);
        }
        return true;
      });
}

}  // namespace rex::graphics::gles
