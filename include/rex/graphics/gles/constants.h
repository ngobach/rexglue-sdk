/**
 * @file        graphics/gles/constants.h
 * @brief       Flattened GLES shader constant uniform descriptions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <cstdint>

namespace rex::graphics::gles {

// Texture units used by the GLES backend.
//
// Guest texture samplers are assigned units 0..kGuestTextureUnitCount-1 (this
// game uses up to 4 samplers in one pixel shader); vertex memory (index and
// vertex fetch) textures start at kMemoryUnitFloor. The ranges must not overlap:
// ANGLE's Metal backend resolves a single texture per unit, and a texture of
// another target left on a unit makes the unit "unloadable" - it is replaced
// with the zero texture.
//
// Uploads and any other internal binds must use kScratchTextureUnit, which no
// shader ever samples, so they can't leave a stray binding on a sampled unit.
// Everything also has to stay below GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS (at least
// 16) because the memory textures are sampled from the vertex shader.
inline constexpr uint32_t kGuestTextureUnitCount = 4;
inline constexpr uint32_t kMemoryUnitFloor = kGuestTextureUnitCount;
inline constexpr uint32_t kVertexMemoryUnitCount = 4;
inline constexpr uint32_t kScratchTextureUnit = 12;
// Fragment-only debug texture, above the guest sampler and memory ranges.
inline constexpr uint32_t kDebugSamplerUnit = 15;

enum class SystemConstantType : uint32_t {
  kUInt,
  kInt,
  kFloat,
  kVec2,
  kVec3,
  kVec4,
  kUVec2,
  kUVec4Array,
  kVec4Array,
};

struct SystemConstantUniform {
  const char* name;
  uint32_t offset;
  SystemConstantType type;
  uint32_t count;
};

extern const SystemConstantUniform kSystemConstantUniforms[];
inline constexpr uint32_t kSystemConstantUniformCount = 36;

}  // namespace rex::graphics::gles
