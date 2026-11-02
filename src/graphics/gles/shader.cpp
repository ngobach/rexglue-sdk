/**
 * @file        graphics/gles/shader.cpp
 * @brief       SPIR-V shader compiled to GLSL ES for the OpenGL ES backend
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

#include <rex/graphics/gles/gl.h>
#include <rex/graphics/gles/shader.h>
#include <rex/logging.h>

#include <spirv_glsl.hpp>

namespace rex::graphics::gles {

namespace {

namespace spvc = SPIRV_CROSS_NAMESPACE;

// GLSL ES 3.10 built-ins that SPIRV-Cross emits without a fallback, provided
// here for the GLSL ES 3.00 (no storage buffers) path.
constexpr char kGles300HelperFunctions[] = R"(
int bitfieldExtract(int value, int offset, int bits) {
  return (value << (32 - offset - bits)) >> (32 - bits);
}
uint bitfieldExtract(uint value, int offset, int bits) {
  return (value >> uint(offset)) & ((1u << uint(bits)) - 1u);
}
ivec2 bitfieldExtract(ivec2 value, int offset, int bits) {
  return ivec2(bitfieldExtract(value.x, offset, bits), bitfieldExtract(value.y, offset, bits));
}
uvec2 bitfieldExtract(uvec2 value, int offset, int bits) {
  return uvec2(bitfieldExtract(value.x, offset, bits), bitfieldExtract(value.y, offset, bits));
}
ivec3 bitfieldExtract(ivec3 value, int offset, int bits) {
  return ivec3(bitfieldExtract(value.x, offset, bits), bitfieldExtract(value.y, offset, bits),
               bitfieldExtract(value.z, offset, bits));
}
uvec3 bitfieldExtract(uvec3 value, int offset, int bits) {
  return uvec3(bitfieldExtract(value.x, offset, bits), bitfieldExtract(value.y, offset, bits),
               bitfieldExtract(value.z, offset, bits));
}
ivec4 bitfieldExtract(ivec4 value, int offset, int bits) {
  return ivec4(bitfieldExtract(value.x, offset, bits), bitfieldExtract(value.y, offset, bits),
               bitfieldExtract(value.z, offset, bits), bitfieldExtract(value.w, offset, bits));
}
uvec4 bitfieldExtract(uvec4 value, int offset, int bits) {
  return uvec4(bitfieldExtract(value.x, offset, bits), bitfieldExtract(value.y, offset, bits),
               bitfieldExtract(value.z, offset, bits), bitfieldExtract(value.w, offset, bits));
}
float ldexp(float x, int exp) { return x * exp2(float(exp)); }
vec2 ldexp(vec2 x, ivec2 exp) { return x * exp2(vec2(exp)); }
vec3 ldexp(vec3 x, ivec3 exp) { return x * exp2(vec3(exp)); }
vec4 ldexp(vec4 x, ivec4 exp) { return x * exp2(vec4(exp)); }
)";

}  // namespace

bool CompileGlslEsShader(GLenum stage, const std::string& source, GLuint& shader_out) {
  shader_out = GetFunctions().CreateShader(stage);
  if (!shader_out) {
    REXGPU_ERROR("GLES shader: glCreateShader failed for stage 0x{:X}", uint32_t(stage));
    return false;
  }
  const char* source_ptr = source.c_str();
  GetFunctions().ShaderSource(shader_out, 1, &source_ptr, nullptr);
  GetFunctions().CompileShader(shader_out);
  GLint compile_status = 0;
  GetFunctions().GetShaderiv(shader_out, GL_COMPILE_STATUS, &compile_status);
  if (!compile_status) {
    GLint log_length = 0;
    GetFunctions().GetShaderiv(shader_out, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(size_t(log_length ? log_length : 1), '\0');
    GetFunctions().GetShaderInfoLog(shader_out, GLsizei(log.size()), nullptr, log.data());
    const char* stage_name = stage == GL_VERTEX_SHADER ? "vertex" : "fragment";
    REXGPU_ERROR("GLES shader: {} stage failed to compile:\n{}", stage_name, log);
    REXGPU_ERROR("GLES shader: generated {} source:\n{}", stage_name, source);
    GetFunctions().DeleteShader(shader_out);
    shader_out = 0;
    return false;
  }
  return true;
}

GlesShader::GlesShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                       const uint32_t* ucode_dwords, size_t ucode_dword_count,
                       bool texture_memory, std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian),
      texture_memory_(texture_memory) {}

GlesShader::GlesTranslation::~GlesTranslation() {
  if (shader_object_) {
    GetFunctions().DeleteShader(shader_object_);
  }
}

Shader::Translation* GlesShader::CreateTranslationInstance(uint64_t modification) {
  return new GlesTranslation(*this, modification);
}

bool GlesShader::GlesTranslation::EnsureShaderObject() {
  if (shader_object_attempted_) {
    return shader_object_ != 0;
  }
  shader_object_attempted_ = true;

  if (!is_valid() || translated_binary().empty() ||
      (translated_binary().size() % sizeof(uint32_t)) != 0) {
    REXGPU_ERROR("GLES shader: cannot compile an empty or invalid translation");
    return false;
  }

  // GLSL ES 3.00 has no derivative control; lower coarse/fine derivatives to
  // the plain instructions (SPIR-V is a linear stream of word0=(count<<16)|op).
  std::vector<uint32_t> spirv_words(translated_binary().size() / sizeof(uint32_t));
  std::memcpy(spirv_words.data(), translated_binary().data(), translated_binary().size());
  // The first five words are the module header.
  for (size_t i = 5; i < spirv_words.size();) {
    const uint32_t word_count = spirv_words[i] >> 16;
    const uint32_t opcode = spirv_words[i] & 0xFFFF;
    switch (opcode) {
      case spv::OpDPdxFine:
      case spv::OpDPdxCoarse:
        spirv_words[i] = (word_count << 16) | spv::OpDPdx;
        break;
      case spv::OpDPdyFine:
      case spv::OpDPdyCoarse:
        spirv_words[i] = (word_count << 16) | spv::OpDPdy;
        break;
      case spv::OpFwidthFine:
      case spv::OpFwidthCoarse:
        spirv_words[i] = (word_count << 16) | spv::OpFwidth;
        break;
      default:
        break;
    }
    if (!word_count) {
      break;
    }
    i += word_count;
  }

  spvc::CompilerGLSL compiler(spirv_words.data(), spirv_words.size());
  spvc::CompilerGLSL::Options options;
  // GLSL ES 3.10 is used when storage buffers are available; without them
  // (ES 3.0, like ANGLE on macOS) vertex memory is fetched from textures and
  // GLSL ES 3.00 is enough.
  const bool texture_memory =
      static_cast<const GlesShader&>(shader()).texture_memory();
  options.version = texture_memory ? 300 : 310;
  options.es = true;
  options.emit_push_constant_as_uniform_buffer = false;
  options.enable_420pack_extension = false;
  // UBO layouts in the guest constant blocks are not std140-compatible; use
  // flattened plain uniforms and set them by name.
  options.emit_uniform_buffer_as_plain_uniforms = true;
  compiler.set_common_options(options);

  compiler.build_combined_image_samplers();
  const auto& combined_samplers = compiler.get_combined_image_samplers();
  const char* stage_prefix =
      shader().type() == xenos::ShaderType::kVertex ? "xe_vs_sampler_" : "xe_ps_sampler_";
  combined_samplers_.clear();
  combined_samplers_.reserve(combined_samplers.size());
  for (size_t i = 0; i < combined_samplers.size(); ++i) {
    const auto& remap = combined_samplers[i];
    CombinedSampler& sampler = combined_samplers_.emplace_back();
    sampler.name = stage_prefix + std::to_string(i);
    compiler.set_name(remap.combined_id, sampler.name);
    sampler.texture_binding = compiler.get_decoration(remap.image_id, spv::DecorationBinding);
    sampler.sampler_binding = compiler.get_decoration(remap.sampler_id, spv::DecorationBinding);
  }

  glsl_source_ = std::string();
  try {
    glsl_source_ = compiler.compile();
  } catch (const std::exception& exception) {
    REXGPU_ERROR("GLES shader: SPIRV-Cross failed: {}", exception.what());
    return false;
  }
  if (glsl_source_.empty()) {
    REXGPU_ERROR("GLES shader: SPIRV-Cross produced no source");
    return false;
  }
  if (texture_memory &&
      (glsl_source_.find("bitfieldExtract") != std::string::npos ||
       glsl_source_.find("ldexp") != std::string::npos)) {
    // Insert the helper functions after the precision declarations, only when
    // SPIRV-Cross actually emitted the ES 3.10 built-ins.
    size_t insert_position = glsl_source_.find("\nvoid main");
    if (insert_position == std::string::npos) {
      insert_position = glsl_source_.size();
    }
    glsl_source_.insert(insert_position, std::string("\n") + kGles300HelperFunctions);
  }
  if (texture_memory) {
    // Vertex-stage guest memory is a plain uniform array (see
    // SpirvShaderTranslator::Features::vertex_memory_uniforms). SPIRV-Cross
    // doesn't emit bare uniform arrays, so declare the ones the translated code
    // references here; the command processor fills them per draw.
    static const char* const kMemoryUniformNames[] = {"xe_vfetch_data_0",
                                                      "xe_vfetch_data_1", "xe_index_data"};
    std::string declarations;
    for (const char* name : kMemoryUniformNames) {
      if (glsl_source_.find(name) != std::string::npos &&
          glsl_source_.find(std::string("uniform highp uint ") + name) == std::string::npos) {
        declarations += fmt::format("uniform highp uint {}[{}];\n", name,
                                    SpirvShaderTranslator::Features::kVertexMemoryUniformCount);
      }
    }
    if (!declarations.empty()) {
      size_t insert_position = glsl_source_.find("\nvoid main");
      if (insert_position == std::string::npos) {
        insert_position = glsl_source_.size();
      }
      glsl_source_.insert(insert_position, "\n" + declarations);
    }
  }
  const bool is_vertex = shader().type() == xenos::ShaderType::kVertex;
  if (!is_vertex && getenv("GLES_MINIMAL_PS")) {
    const size_t main_position = glsl_source_.find("void main()");
    if (main_position != std::string::npos) {
      glsl_source_.resize(main_position);
      const char* minimal_interp = getenv("GLES_DEBUG_INTERP");
      if (minimal_interp && minimal_interp[0] == 'a') {
        glsl_source_ +=
            "void main() { xe_out_fragment_data_0 = "
            "vec4(texture(xe_ps_sampler_0, vec3(0.5, 0.5, 0.0)).rgb, 1.0); }\n";
      } else if (minimal_interp && minimal_interp[0] == 'S') {
        glsl_source_ +=
            "void main() { xe_out_fragment_data_0 = "
            "vec4(texelFetch(xe_ps_sampler_0, ivec3(0, 0, 0), 0).rgb, 1.0); }\n";
      } else if (minimal_interp && minimal_interp[0] == '0') {
        glsl_source_ +=
            "void main() { xe_out_fragment_data_0 = vec4(xe_interpolator_0.xyz, 1.0); }\n";
      } else {
        glsl_source_ +=
            "void main() { xe_out_fragment_data_0 = vec4(1.0, 0.0, 1.0, 1.0); }\n";
      }
    }
  }
  if (!is_vertex && getenv("GLES_DEBUG_VFETCH_FS")) {
    // Sample the vertex memory texture from the fragment stage: separates "the
    // texture is empty" from "vertex-stage integer sampling is broken".
    const size_t main_position = glsl_source_.find("void main");
    if (main_position != std::string::npos) {
      glsl_source_.insert(main_position,
                          "uniform highp sampler2D xe_vfetch_mem_0;\n");
    }
    const size_t closing = glsl_source_.rfind('}');
    if (closing != std::string::npos) {
      const char* mode = getenv("GLES_DEBUG_VFETCH_FS");
      std::string forced =
          "    xe_out_fragment_data_0 = vec4(0.0, 0.0, 1.0, 1.0);\n";
      if (mode[0] == 'r') {
        forced =
            "    uint xe_debug_vfetch = "
            "floatBitsToUint(texelFetch(xe_vfetch_mem_0, ivec2(1, 0), 0).x);\n"
            "    xe_out_fragment_data_0 = vec4(xe_debug_vfetch != 0u ? 1.0 : "
            "0.0, xe_interpolator_0.x, 0.0, 1.0);\n";
      } else if (mode[0] == 'z') {
        forced =
            "    xe_out_fragment_data_0 = vec4(vec2(textureSize(xe_vfetch_mem_0, "
            "0)) / vec2(4096.0), 0.0, 1.0);\n";
      }
      glsl_source_.insert(closing, forced);
    }
  }
  if (is_vertex && getenv("GLES_VS_VALUE")) {
    // Always-compiling per-vertex value probe: forces a full-screen quad, but
    // reports values through a dedicated varying that the fragment probe reads.
    const size_t main_position = glsl_source_.find("void main");
    if (main_position != std::string::npos) {
      glsl_source_.insert(main_position, "out highp vec4 xe_debug_value;\n");
    }
    const size_t closing = glsl_source_.rfind('}');
    if (closing != std::string::npos) {
      std::string value;
      const char* mode = getenv("GLES_VS_VALUE");
      if (mode[0] == 'i') {
        value = "vec4(float(gl_VertexID) / 8.0, 0.0, 0.0, 1.0)";
      } else if (mode[0] == 'p') {
        value = "vec4(xe_var_registers[3].xy / 2048.0, 0.0, 1.0)";
      } else if (mode[0] == 'r') {
        value =
            "vec4(clamp(float(xe_var_vfetch_address - "
            "int(xe_uniform_fetch_constants_vs.fetch_constants[47].z >> 2u)) / 36.0, 0.0, "
            "1.0), 0.0, 0.0, 1.0)";
      } else if (mode[0] == 't') {
        // Fixed texel (1, 0): does a vertex-stage texelFetch see the data at all?
        value =
            "vec4(floatBitsToUint(texelFetch(xe_vfetch_mem_0, ivec2(1, 0), 0).x) != 0u ? "
            "1.0 : 0.0, 0.0, 0.0, 1.0)";
      } else if (mode[0] == 'u') {
        // Fixed texel (0, 0), the first dword of the uploaded range.
        value =
            "vec4(floatBitsToUint(texelFetch(xe_vfetch_mem_0, ivec2(0, 0), 0).x) != 0u ? "
            "1.0 : 0.0, 0.0, 0.0, 1.0)";
      } else if (mode[0] == 'v') {
        // Same texel as 't', but read with an explicit-LOD sample instead of a
        // fetch: Metal may support one and not the other in vertex functions.
        value =
            "vec4(floatBitsToUint(textureLod(xe_vfetch_mem_0, vec2(1.5 / 2048.0, 0.5), 0.0).x) "
            "!= 0u ? 1.0 : 0.0, 0.0, 0.0, 1.0)";
      } else {
        value = "vec4(1.0, 0.0, 1.0, 1.0)";
      }
      glsl_source_.insert(closing,
                          "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
                          "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
                          "    xe_debug_value = " +
                              value + ";\n");
    }
  }
  if (!is_vertex && getenv("GLES_FS_VALUE")) {
    const size_t main_position = glsl_source_.find("void main");
    if (main_position != std::string::npos) {
      glsl_source_.insert(main_position, "in highp vec4 xe_debug_value;\n");
    }
    const size_t closing = glsl_source_.rfind('}');
    if (closing != std::string::npos) {
      glsl_source_.insert(closing,
                          "    xe_out_fragment_data_0 = xe_debug_value;\n");
    }
  }
  if (is_vertex && getenv("GLES_FORCE_VS_INTERP")) {
    const size_t closing = glsl_source_.rfind('}');
    if (closing != std::string::npos) {
      std::string injection;
      const char* mode = getenv("GLES_FORCE_VS_INTERP");
      if (mode[0] == 'd') {
        // Keep the guest-computed clip position observable while forcing a
        // full-screen quad so the fragments are actually visible.
        injection += "    highp vec2 xe_debug_clip = gl_Position.xy;\n"
                     "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
                     "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
                     "    xe_interpolator_0 = vec4(xe_debug_clip * 0.5 + 0.5, 0.0, 1.0);\n";
      } else if (mode[0] == 's') {
        // As 'd', but shows textureSize of the vertex fetch sampler.
        injection += "    highp vec2 xe_debug_size = "
                     "vec2(textureSize(xe_vfetch_mem_0, 0)) / vec2(4096.0);\n"
                     "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
                     "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
                     "    xe_interpolator_0 = vec4(xe_debug_size, 0.0, 1.0);\n";
      } else if (mode[0] == 'h') {
        // As 'd', but shows raw fetched texels 1 and 3 (low 16 bits).
        injection += "    float xe_debug_t1 = "
                     "float(floatBitsToUint(texelFetch(xe_vfetch_mem_0, ivec2(1, 0), 0).x) & "
                     "0xFFFFu) / 65536.0;\n"
                     "    float xe_debug_t3 = "
                     "float(floatBitsToUint(texelFetch(xe_vfetch_mem_0, ivec2(3, 0), 0).x) & "
                     "0xFFFFu) / 65536.0;\n"
                     "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
                     "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
                     "    xe_interpolator_0 = vec4(xe_debug_t1, xe_debug_t3, 0.0, 1.0);\n";
      } else if (mode[0] == 'f') {
        // As 'd', but shows the fetched position register instead.
        injection += "    float xe_debug_x = xe_var_registers[3].x / 2048.0;\n"
                     "    float xe_debug_y = xe_var_registers[3].y / 2048.0;\n"
                     "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
                     "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
                     "    xe_interpolator_0 = vec4(xe_debug_x, xe_debug_y, 0.0, 1.0);\n";
      } else if (mode[0] == 'e') {
        // As 'd', but red shows the fetch constant's endian mode field and green
        // the base address (scaled), for fetch constant 95 / word pair 47.
        injection += "    highp vec2 xe_debug_clip = gl_Position.xy;\n"
                     "    float xe_debug_endian = "
                     "float(xe_uniform_fetch_constants_vs.fetch_constants[47].w & 3u);\n"
                     "    float xe_debug_base = "
                     "float(xe_uniform_fetch_constants_vs.fetch_constants[47].z >> 2u) / "
                     "268435456.0;\n"
                     "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
                     "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
                     "    xe_interpolator_0 = vec4(xe_debug_endian == 1.0 ? 1.0 : 0.0, "
                     "xe_debug_endian == 2.0 ? 1.0 : 0.0, "
                     "xe_debug_endian == 3.0 ? 1.0 : 0.0, 1.0);\n";
      } else if (mode[0] == 'i') {
        // As 'd', but red shows gl_VertexID and green the fetched index.
        injection += "    highp vec2 xe_debug_clip = gl_Position.xy;\n"
                     "    float xe_debug_index = xe_var_registers[0].x;\n"
                     "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
                     "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
                     "    xe_interpolator_0 = vec4(float(gl_VertexID) / 8.0, "
                     "xe_debug_index / 8.0, 0.0, 1.0);\n";
      } else if (mode[0] == 'y') {
        // Per-vertex view: red = vertex fetch offset / 36 (index * 9 / 36),
        // green/blue = the fetched position / 2048. All channels must vary
        // across the forced quad if the fetch really returns per-vertex data.
        injection +=
            "    int xe_debug_rel = xe_var_vfetch_address - "
            "int(xe_uniform_fetch_constants_vs.fetch_constants[47].z >> 2u);\n"
            "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
            "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
            "    xe_interpolator_0 = vec4(clamp(float(xe_debug_rel) / 36.0, 0.0, 1.0), "
            "clamp(xe_var_registers[3].x / 2048.0, 0.0, 1.0), "
            "clamp(xe_var_registers[3].y / 2048.0, 0.0, 1.0), 1.0);\n";
      } else if (mode[0] == 'z') {
        // Raw texel read at the fetch base: red = base dword != 0,
        // green = (base + 1) dword != 0. Independent of the guest address math.
        injection +=
            "    uint xe_debug_base = "
            "xe_uniform_fetch_constants_vs.fetch_constants[47].z >> 2u;\n"
            "    uint xe_debug_t0 = floatBitsToUint(texelFetch(xe_vfetch_mem_0, "
            "ivec2(int(xe_debug_base) & 2047, int(xe_debug_base >> 11u)), 0).x);\n"
            "    uint xe_debug_t1 = floatBitsToUint(texelFetch(xe_vfetch_mem_0, "
            "ivec2(int(xe_debug_base + 1u) & 2047, int((xe_debug_base + 1u) >> 11u)), 0).x);\n"
            "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
            "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
            "    xe_interpolator_0 = vec4(xe_debug_t0 != 0u ? 1.0 : 0.0, "
            "xe_debug_t1 != 0u ? 1.0 : 0.0, 1.0, 1.0);\n";
      } else if (mode[0] == 'w') {
        // Report whether the vertex stage really reads the memory texture: the
        // value goes to a varying so a fragment-stage probe can compare.
        injection +=
            "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
            "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n"
            "    xe_interpolator_0 = vec4("
            "floatBitsToUint(texelFetch(xe_vfetch_mem_0, ivec2(1, 0), 0).x) != 0u ? 1.0 : "
            "0.0, 0.0, 0.0, 1.0);\n";
      } else if (mode[0] == 'q') {
        // Override the clip position with a full-screen quad, keeping the
        // vertex fetch and the rest of the shader intact.
        injection += "    gl_Position = vec4(float(int(gl_VertexID) & 1) * 2.0 - 1.0, "
                     "float((int(gl_VertexID) >> 1) & 1) * 2.0 - 1.0, 0.0, 1.0);\n";
      } else if (mode[0] == 'p') {
        // Visualise the clip-space position as an interpolator.
        injection += "    xe_interpolator_0 = vec4(gl_Position.xy * 0.5 + 0.5, 0.0, 1.0);\n";
      }
      for (uint32_t index = 0;
           index < 8 && mode[0] != 'p' && mode[0] != 'q' && mode[0] != 'd' && mode[0] != 'i' &&
           mode[0] != 'e' && mode[0] != 'f' && mode[0] != 'h' && mode[0] != 's' &&
           mode[0] != 'y' && mode[0] != 'z' && mode[0] != 'w';
           ++index) {
        std::string name = "xe_interpolator_" + std::to_string(index);
        // SPIRV-Cross emits `out vec4` (precision defaults to highp in vertex
        // shaders), so the qualifiers must match without an explicit precision.
        if (glsl_source_.find(" out vec4 " + name) != std::string::npos ||
            glsl_source_.find(" out highp vec4 " + name) != std::string::npos) {
          injection += "    " + name + " = vec4(1.0, 0.0, 0.0, 1.0);\n";
        }
      }
      glsl_source_.insert(closing, injection);
    }
  }
  if (!is_vertex && getenv("GLES_NO_DISCARD")) {
    // Keeps the guest fragment shader's output but neutralizes alpha-test
    // rejection, to tell "the alpha test rejects everything" from "the shader
    // computes nothing".
    size_t discard_position = 0;
    while ((discard_position = glsl_source_.find("discard;", discard_position)) !=
           std::string::npos) {
      glsl_source_.replace(discard_position, 8, ";        ");
      discard_position += 8;
    }
  }
  if (!is_vertex && getenv("GLES_FORCE_PS_OUTPUT")) {
    size_t discard_position = 0;
    while ((discard_position = glsl_source_.find("discard;", discard_position)) !=
           std::string::npos) {
      glsl_source_.replace(discard_position, 8, ";        ");
      discard_position += 8;
    }
    const size_t main_position = glsl_source_.rfind("void main()");
    const size_t closing = glsl_source_.rfind('}');
    if (main_position != std::string::npos && closing != std::string::npos) {
      const char* interp = getenv("GLES_DEBUG_INTERP");
      std::string forced_output = "    xe_out_fragment_data_0 = vec4(1.0, 0.0, 1.0, 1.0);\n";
      if (interp && interp[0] == '0') {
        forced_output = "    xe_out_fragment_data_0 = vec4(xe_interpolator_0.xy, 0.0, 1.0);\n";
      } else if (interp && interp[0] == '1') {
        forced_output = "    xe_out_fragment_data_0 = vec4(xe_interpolator_1.xyz, 1.0);\n";
      } else if (interp && interp[0] == 's') {
        forced_output =
            "    xe_out_fragment_data_0 = texture(xe_ps_sampler_0, vec3(0.5, 0.5, 0.0));\n";
      } else if (interp && interp[0] == 't') {
        forced_output =
            "    xe_out_fragment_data_0 = vec4(vec2(textureSize(xe_ps_sampler_0, 0).xy) / "
            "vec2(4096.0), 0.0, 1.0);\n";
      } else if (interp && interp[0] == 'a') {
        forced_output =
            "    xe_out_fragment_data_0 = vec4(texture(xe_ps_sampler_0, vec3(0.5, 0.5, 0.0)).rgb, "
            "1.0);\n";
      } else if (interp && interp[0] == '1') {
        forced_output =
            "    xe_out_fragment_data_0 = vec4(texture(xe_ps_sampler_1, vec3(0.5, 0.5, 0.0)).rgb, "
            "1.0);\n";
      } else if (interp && interp[0] == 'L') {
        forced_output =
            "    xe_out_fragment_data_0 = textureLod(xe_ps_sampler_0, vec3(0.5, 0.5, 0.0), 0.0);\n";
      } else if (interp && interp[0] == 'S') {
        forced_output =
            "    xe_out_fragment_data_0 = texelFetch(xe_ps_sampler_0, ivec3(0, 0, 0), 0);\n";
      } else if (interp && interp[0] == 'g') {
        forced_output = "    xe_out_fragment_data_0 = vec4(0.0, 1.0, 0.0, 1.0);\n";
      } else if (interp && interp[0] == 'p') {
        forced_output =
            "    xe_out_fragment_data_0 = vec4(fract(gl_FragCoord.xy / vec2(64.0, 64.0)), 0.0, "
            "1.0);\n";
      } else if (interp && interp[0] == 'W') {
        forced_output =
            "    xe_out_fragment_data_0 = vec4(1.0, 1.0, 1.0, 1.0) + "
            "texture(xe_ps_sampler_0, vec3(0.5, 0.5, 0.0));\n";
      } else if (interp && interp[0] == 'v') {
        // Green plus an infinitesimal texture read: green proves the draw ran even
        // if the sampler resolves to the zero texture.
        forced_output =
            "    xe_out_fragment_data_0 = vec4(0.0, 1.0, 0.0, 1.0) + "
            "texture(xe_ps_sampler_0, vec3(0.5, 0.5, 0.0)) * 1e-20;\n";
      } else if (interp && interp[0] == 'w') {
        forced_output =
            "    xe_out_fragment_data_0 = vec4(texture(xe_ps_sampler_0, vec3(0.5, 0.5, 0.0)).rgb, "
            "1.0) + vec4(0.0, 0.0, 0.0, 0.0);\n";
      }
      glsl_source_.insert(closing, forced_output);
    }
  }

  if (const char* dump_root = getenv("GLES_DUMP_SHADERS")) {
    char path[512];
    snprintf(path, sizeof(path), "%s/glsl_%016llX_%016llX_%s.glsl", dump_root,
             static_cast<unsigned long long>(shader().ucode_data_hash()),
             static_cast<unsigned long long>(modification()), is_vertex ? "vs" : "fs");
    FILE* dump_file = fopen(path, "wb");
    if (dump_file) {
      fwrite(glsl_source_.data(), 1, glsl_source_.size(), dump_file);
      fclose(dump_file);
    }
  }
  if (!CompileGlslEsShader(is_vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, glsl_source_,
                          shader_object_)) {
    return false;
  }

  static uint32_t logged_shaders = 0;
  if (logged_shaders < 8) {
    ++logged_shaders;
    REXGPU_INFO("GLES shader: compiled {} shader {:016X} ({} combined samplers)",
                is_vertex ? "vertex" : "fragment", shader().ucode_data_hash(),
                uint32_t(combined_samplers_.size()));
    REXGPU_INFO("GLES shader: generated source:\n{}", glsl_source_);
  }
  return true;
}

}  // namespace rex::graphics::gles
