/**
 * @file        graphics/gles/graphics_system.cpp
 * @brief       Xenos GPU emulation on OpenGL ES
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <rex/graphics/gles/command_processor.h>
#include <rex/graphics/gles/graphics_system.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/gles/provider.h>

namespace rex::graphics::gles {

GlesGraphicsSystem::GlesGraphicsSystem() = default;

GlesGraphicsSystem::~GlesGraphicsSystem() = default;

std::string GlesGraphicsSystem::name() const { return "GLES"; }

void GlesGraphicsSystem::CreateProvider(bool with_presentation) {
  // The GL context lives on the window and is owned by the presenter, so the
  // provider needs no device of its own.
  (void)with_presentation;
  provider_ = rex::ui::gles::GlesProvider::Create();
}

std::unique_ptr<CommandProcessor> GlesGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<GlesCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::gles
