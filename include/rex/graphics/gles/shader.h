/**
 * @file        graphics/gles/shader.h
 * @brief       SPIR-V shader compiled to GLSL ES for the OpenGL ES backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>

#include "gl.h"

namespace rex::graphics::gles {

using namespace ::rex::gles;

// Compiles a GLSL ES source into a stage object (GL_VERTEX_SHADER or
// GL_FRAGMENT_SHADER). Logs the source on failure.
bool CompileGlslEsShader(GLenum stage, const std::string& source, GLuint& shader_out);

class GlesShader final : public SpirvShader {
 public:
  // A sampler produced by SPIRV-Cross combining a guest texture image variable
  // and a sampler variable (Vulkan separates them, GLSL ES does not).
  struct CombinedSampler {
    // Index into the shader's texture binding vector (descriptor set binding).
    uint32_t texture_binding;
    // Index into the shader's sampler binding vector (after the textures).
    uint32_t sampler_binding;
    // Explicit name assigned to the combined sampler uniform.
    std::string name;
  };

  class GlesTranslation final : public SpirvTranslation {
   public:
    GlesTranslation(GlesShader& shader, uint64_t modification)
        : SpirvTranslation(shader, modification) {}
    ~GlesTranslation() override;

    // Compiles the translated SPIR-V into a GLSL ES shader object on the GL
    // thread. The object is kept alive so it can be linked into multiple
    // programs with different pixel shaders.
    bool EnsureShaderObject();

    bool shader_object_valid() const { return shader_object_ != 0; }
    uint32_t shader_object() const { return shader_object_; }
    const std::vector<CombinedSampler>& combined_samplers() const { return combined_samplers_; }
    // GLSL ES source generated from the SPIR-V, for diagnostics.
    const std::string& glsl_source() const { return glsl_source_; }

   private:
    uint32_t shader_object_ = 0;
    std::string glsl_source_;
    std::vector<CombinedSampler> combined_samplers_;
    bool shader_object_attempted_ = false;
  };

  GlesShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
             size_t ucode_dword_count, bool texture_memory,
             std::endian ucode_source_endian = std::endian::big);

  bool texture_memory() const { return texture_memory_; }

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;

 private:
  bool texture_memory_;
};

}  // namespace rex::graphics::gles
