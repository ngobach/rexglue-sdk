/**
 * @file        ui/gles/provider.cpp
 * @brief       OpenGL ES graphics provider
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <rex/ui/gles/provider.h>

#include <rex/ui/gles/presenter.h>

namespace rex::ui::gles {

std::unique_ptr<GlesProvider> GlesProvider::Create() {
  return std::unique_ptr<GlesProvider>(new GlesProvider());
}

std::unique_ptr<Presenter> GlesProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return GlesPresenter::Create(std::move(host_gpu_loss_callback));
}

std::unique_ptr<ImmediateDrawer> GlesProvider::CreateImmediateDrawer() {
  // ImGui overlays are not available on this backend yet. The app treats a
  // null drawer as "no overlays" and keeps rendering the guest output.
  return nullptr;
}

}  // namespace rex::ui::gles
