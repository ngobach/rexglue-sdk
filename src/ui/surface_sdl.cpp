/**
 * @file        ui/surface_sdl.cpp
 * @brief       Surface wrapping an SDL window (used by the GLES presenter)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <rex/ui/surface_sdl.h>

#include <SDL3/SDL_video.h>

namespace rex::ui {

bool SDLWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  width_out = 0;
  height_out = 0;
  if (!window_) {
    return false;
  }
  int width = 0;
  int height = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &width, &height) || width <= 0 || height <= 0) {
    return false;
  }
  width_out = uint32_t(width);
  height_out = uint32_t(height);
  return true;
}

}  // namespace rex::ui
