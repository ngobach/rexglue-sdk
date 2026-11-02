/**
 * @file        ui/gles/provider.h
 * @brief       OpenGL ES graphics provider
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <memory>

#include <rex/ui/graphics_provider.h>

namespace rex::ui::gles {

class GlesProvider final : public GraphicsProvider {
 public:
  static std::unique_ptr<GlesProvider> Create();

  std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback host_gpu_loss_callback) override;
  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;
};

}  // namespace rex::ui::gles
