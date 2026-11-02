/**
 * @file        graphics/gles/command_processor.h
 * @brief       Xenos command processor for the OpenGL ES backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/util/draw.h>

#include "gl.h"

namespace rex::graphics::gles {

using namespace ::rex::gles;

class GlesPipelineCache;
class GlesPrimitiveProcessor;
class GlesRenderTargetCache;
class GlesSharedMemory;
class GlesTextureCache;
class GlesProgram;
class GlesShader;

class GlesCommandProcessor final : public CommandProcessor {
 public:
  GlesCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~GlesCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  void UpdateSystemConstantValues(const PrimitiveProcessor::ProcessingResult& processing_result,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);
  bool UpdateBindings(const GlesShader& vertex_shader, const GlesShader* pixel_shader);
  bool BindTextures(const GlesProgram& program, const GlesShader& vertex_shader,
                    const GlesShader* pixel_shader);
  void SetStageConstants(const GlesProgram& program, const GlesShader& vertex_shader,
                         const GlesShader* pixel_shader);
  void ApplyRenderState(reg::RB_DEPTHCONTROL normalized_depth_control,
                        uint32_t normalized_color_mask, bool primitive_polygonal);
  bool DrawPrimitives(const PrimitiveProcessor::ProcessingResult& processing_result);
  void PresentSwapTexture(GLuint texture, GLenum target, uint32_t width, uint32_t height);
  void PresentRenderTargetTexture(GLuint texture, uint32_t width, uint32_t height);

  // ES 3.0 vertex memory textures.
  struct VertexMemoryTexture {
    GLuint texture = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t data_hash = 0;
    uint32_t data_size = 0;
  };
  bool BindVertexMemoryTextures(const GlesProgram& program, uint32_t index_guest_base,
                                uint32_t index_bytes);
  void UploadMemoryUniform(GLint location, const void* data, uint32_t dword_count);
  GLuint UploadUintTexture(VertexMemoryTexture& destination, const void* data,
                           uint32_t dword_count, bool always_upload = false);

  GLuint GetWindowDrawableWidth() const;
  GLuint GetWindowDrawableHeight() const;

  std::unique_ptr<GlesSharedMemory> shared_memory_;
  std::unique_ptr<GlesTextureCache> texture_cache_;
  std::unique_ptr<GlesRenderTargetCache> render_target_cache_;
  std::unique_ptr<GlesPipelineCache> pipeline_cache_;
  std::unique_ptr<GlesPrimitiveProcessor> primitive_processor_;

  GLuint vertex_array_ = 0;
  GLuint present_framebuffer_ = 0;
  GLuint guest_index_buffer_ = 0;
  GLuint debug_program_ = 0;
  GLuint debug_white_texture_ = 0;
  GLuint debug_sample_program_ = 0;
  VertexMemoryTexture dummy_memory_texture_;
  VertexMemoryTexture index_memory_texture_;
  std::unordered_map<uint32_t, VertexMemoryTexture> vfetch_memory_textures_;
  std::vector<uint32_t> memory_texture_staging_;
  std::vector<uint32_t> memory_uniform_staging_;
  // Draw index within the current frame, for the GLES_ONLY_DRAW probe knob.
  uint32_t frame_draw_index_ = 0;
  // Binding 0 - system constants, 1 - vertex float constants, 2 - pixel float
  // constants, 3 - bool and loop constants, 4 - fetch constants.
  SpirvShaderTranslator::SystemConstants system_constants_ = {};
};

}  // namespace rex::graphics::gles
