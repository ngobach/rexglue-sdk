/**
 * @file        ui/gles/presenter.h
 * @brief       OpenGL ES presenter (SDL/EGL context, guest output blit)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <memory>
#include <mutex>
#include <thread>

#include <rex/ui/presenter.h>

namespace rex::ui::gles {

class GlesPresenter final : public Presenter {
 public:
  static std::unique_ptr<GlesPresenter> Create(HostGpuLossCallback host_gpu_loss_callback);

  ~GlesPresenter() override;

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(RawImage& image_out) override;

  // Makes the EGL/CGL context current on the calling (command processor)
  // thread. The context is created on the UI thread when the surface is
  // connected, but all GL work - including the guest output blit - happens on
  // this thread to avoid sharing a context between threads.
  bool MakeContextCurrentOnGlThread();
  bool IsGlThread() const { return gl_thread_id_ == std::this_thread::get_id(); }

  uint32_t window_drawable_width() const;
  uint32_t window_drawable_height() const;
  uint32_t surface_width() const;
  uint32_t surface_height() const;

 private:
  explicit GlesPresenter(HostGpuLossCallback host_gpu_loss_callback);

  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(
      uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
      std::function<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out_ref) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

  bool EnsureContextCurrent();

  mutable std::mutex context_mutex_;
  void* sdl_window_ = nullptr;
  void* gl_context_ = nullptr;
  std::thread::id gl_thread_id_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t surface_width_ = 0;
  uint32_t surface_height_ = 0;
};

}  // namespace rex::ui::gles
