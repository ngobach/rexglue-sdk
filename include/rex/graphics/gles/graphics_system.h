/**
 * @file        graphics/gles/graphics_system.h
 * @brief       Xenos GPU emulation on OpenGL ES
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <memory>

#include <rex/graphics/graphics_system.h>

namespace rex::graphics::gles {

class GlesGraphicsSystem final : public GraphicsSystem {
 public:
  GlesGraphicsSystem();
  ~GlesGraphicsSystem() override;

  std::string name() const override;

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::gles
