/**
 * @file        graphics/gles/constants.cpp
 * @brief       Flattened GLES shader constant uniform descriptions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <cstddef>

#include <rex/graphics/gles/constants.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>

namespace rex::graphics::gles {

using SystemConstants = SpirvShaderTranslator::SystemConstants;

const SystemConstantUniform kSystemConstantUniforms[] = {
    {"flags", offsetof(SystemConstants, flags), SystemConstantType::kUInt, 1},
    {"vertex_index_load_address", offsetof(SystemConstants, vertex_index_load_address),
     SystemConstantType::kUInt, 1},
    {"vertex_index_endian", offsetof(SystemConstants, vertex_index_endian),
     SystemConstantType::kUInt, 1},
    {"line_loop_closing_index", offsetof(SystemConstants, line_loop_closing_index),
     SystemConstantType::kUInt, 1},
    {"vertex_base_index", offsetof(SystemConstants, vertex_base_index), SystemConstantType::kInt,
     1},
    {"vertex_index_reset", offsetof(SystemConstants, vertex_index_reset),
     SystemConstantType::kUInt, 1},
    {"compute_memexport_vertex_count", offsetof(SystemConstants, compute_memexport_vertex_count),
     SystemConstantType::kUInt, 1},
    {"user_clip_planes", offsetof(SystemConstants, user_clip_planes),
     SystemConstantType::kVec4Array, 6},
    {"vertex_index_min", offsetof(SystemConstants, vertex_index_min), SystemConstantType::kUInt, 1},
    {"vertex_index_max", offsetof(SystemConstants, vertex_index_max), SystemConstantType::kUInt, 1},
    {"ndc_scale", offsetof(SystemConstants, ndc_scale), SystemConstantType::kVec3, 1},
    {"point_vertex_diameter_min", offsetof(SystemConstants, point_vertex_diameter_min),
     SystemConstantType::kFloat, 1},
    {"ndc_offset", offsetof(SystemConstants, ndc_offset), SystemConstantType::kVec3, 1},
    {"point_vertex_diameter_max", offsetof(SystemConstants, point_vertex_diameter_max),
     SystemConstantType::kFloat, 1},
    {"point_constant_diameter", offsetof(SystemConstants, point_constant_diameter),
     SystemConstantType::kVec2, 1},
    {"point_screen_diameter_to_ndc_radius",
     offsetof(SystemConstants, point_screen_diameter_to_ndc_radius), SystemConstantType::kVec2, 1},
    {"textures_resolution_scaled", offsetof(SystemConstants, textures_resolution_scaled),
     SystemConstantType::kUInt, 1},
    {"texture_swizzled_signs", offsetof(SystemConstants, texture_swizzled_signs),
     SystemConstantType::kUVec4Array, 2},
    {"texture_swizzles", offsetof(SystemConstants, texture_swizzles),
     SystemConstantType::kUVec4Array, 4},
    {"alpha_test_reference", offsetof(SystemConstants, alpha_test_reference),
     SystemConstantType::kFloat, 1},
    {"alpha_to_mask", offsetof(SystemConstants, alpha_to_mask), SystemConstantType::kUInt, 1},
    {"edram_32bpp_tile_pitch_dwords_scaled",
     offsetof(SystemConstants, edram_32bpp_tile_pitch_dwords_scaled), SystemConstantType::kUInt, 1},
    {"edram_depth_base_dwords_scaled",
     offsetof(SystemConstants, edram_depth_base_dwords_scaled), SystemConstantType::kUInt, 1},
    {"color_exp_bias", offsetof(SystemConstants, color_exp_bias), SystemConstantType::kVec4, 1},
    {"edram_poly_offset_front_scale", offsetof(SystemConstants, edram_poly_offset_front_scale),
     SystemConstantType::kFloat, 1},
    {"edram_poly_offset_back_scale", offsetof(SystemConstants, edram_poly_offset_back_scale),
     SystemConstantType::kFloat, 1},
    {"edram_poly_offset_front_offset", offsetof(SystemConstants, edram_poly_offset_front_offset),
     SystemConstantType::kFloat, 1},
    {"edram_poly_offset_back_offset", offsetof(SystemConstants, edram_poly_offset_back_offset),
     SystemConstantType::kFloat, 1},
    {"edram_stencil_front", offsetof(SystemConstants, edram_stencil_front),
     SystemConstantType::kUVec2, 1},
    {"edram_stencil_back", offsetof(SystemConstants, edram_stencil_back), SystemConstantType::kUVec2,
     1},
    {"edram_rt_base_dwords_scaled", offsetof(SystemConstants, edram_rt_base_dwords_scaled),
     SystemConstantType::kUVec4Array, 1},
    {"edram_rt_format_flags", offsetof(SystemConstants, edram_rt_format_flags),
     SystemConstantType::kUVec4Array, 1},
    {"edram_rt_blend_factors_ops", offsetof(SystemConstants, edram_rt_blend_factors_ops),
     SystemConstantType::kUVec4Array, 1},
    {"edram_rt_keep_mask", offsetof(SystemConstants, edram_rt_keep_mask),
     SystemConstantType::kUVec4Array, 2},
    {"edram_rt_clamp", offsetof(SystemConstants, edram_rt_clamp), SystemConstantType::kVec4Array, 4},
    {"edram_blend_constant", offsetof(SystemConstants, edram_blend_constant),
     SystemConstantType::kVec4, 1},
};

static_assert(sizeof(kSystemConstantUniforms) / sizeof(kSystemConstantUniforms[0]) ==
                  kSystemConstantUniformCount,
              "update kSystemConstantUniformCount");

}  // namespace rex::graphics::gles
