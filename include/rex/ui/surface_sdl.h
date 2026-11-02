/**
 * @file        ui/surface_sdl.h
 * @brief       Surface wrapping an SDL window (used by the GLES presenter)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <rex/ui/surface.h>

struct SDL_Window;

namespace rex::ui {

// A presentation surface identified only by the SDL window. Backends that
// create their own host context (OpenGL ES) don't need a platform-native
// surface; they need the window so SDL can create the GL context and swap.
class SDLWindowSurface final : public Surface {
 public:
  explicit SDLWindowSurface(SDL_Window* window) : window_(window) {}

  TypeIndex GetType() const override { return kTypeIndex_SDLWindow; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  SDL_Window* window_;
};

}  // namespace rex::ui
