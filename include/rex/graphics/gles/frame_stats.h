/**
 * @file        graphics/gles/frame_stats.h
 * @brief       Timing counters shared by the GLES backend (GLES_LOG_FPS)
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

// Wall-clock seconds spent loading/decoding guest textures into GL textures,
// and the number of loads and bytes decoded. Reset by the command processor
// when it reports the frame breakdown.
extern double g_texture_load_seconds;
extern double g_texture_decode_seconds;
extern double g_texture_upload_seconds;
extern uint64_t g_texture_load_count;
extern uint64_t g_texture_load_bytes;

}  // namespace rex::graphics::gles
