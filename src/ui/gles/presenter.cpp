/**
 * @file        ui/gles/presenter.cpp
 * @brief       OpenGL ES presenter (SDL/EGL context, guest output blit)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <rex/ui/gles/presenter.h>

#include <algorithm>
#include <cstdlib>

#include <SDL3/SDL_video.h>

#include <rex/graphics/gles/gl.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

namespace rex::gles {

Functions& GetFunctions() {
  static Functions functions;
  return functions;
}

bool LoadFunctions(GetProcAddressFn get_proc_address) {
  Functions& functions = GetFunctions();
  bool ok = true;
#define REX_GL_LOAD(ret, name, args)                                             \
  functions.name = reinterpret_cast<PFN_##name>(get_proc_address("gl" #name)); \
  ok = ok && functions.name != nullptr;
  REX_GL_FUNCTION_LIST(REX_GL_LOAD)
#undef REX_GL_LOAD
  return ok;
}

}  // namespace rex::gles

namespace rex::ui::gles {

namespace {

class GlesGuestOutputRefreshContext final : public Presenter::GuestOutputRefreshContext {
 public:
  explicit GlesGuestOutputRefreshContext(bool& is_8bpc_out_ref)
      : GuestOutputRefreshContext(is_8bpc_out_ref) {}
};

}  // namespace


GlesPresenter::GlesPresenter(HostGpuLossCallback host_gpu_loss_callback)
    : Presenter(std::move(host_gpu_loss_callback)) {}

GlesPresenter::~GlesPresenter() {
  std::lock_guard lock(context_mutex_);
  if (gl_context_) {
    SDL_GL_DestroyContext(static_cast<SDL_GLContext>(gl_context_));
    gl_context_ = nullptr;
  }
}

std::unique_ptr<GlesPresenter> GlesPresenter::Create(
    HostGpuLossCallback host_gpu_loss_callback) {
  auto presenter = std::unique_ptr<GlesPresenter>(new GlesPresenter(host_gpu_loss_callback));
  if (!presenter->InitializeCommonSurfaceIndependent()) {
    REXLOG_ERROR("GLES presenter: surface-independent initialization failed");
    return nullptr;
  }
  return presenter;
}

Surface::TypeFlags GlesPresenter::GetSupportedSurfaceTypes() const {
  // The context is created by SDL on the window itself, so the SDL window
  // surface is all that is needed (also on KMSDRM, which has no native handle).
  return Surface::kTypeFlag_SDLWindow;
}

GlesPresenter::SurfacePaintConnectResult
GlesPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
    bool was_paintable, bool& is_vsync_implicit_out) {
  std::lock_guard lock(context_mutex_);
  width_ = new_surface_width;
  height_ = new_surface_height;
  surface_width_ = new_surface_width;
  surface_height_ = new_surface_height;
  // The GL context lives on the guest output thread, so presentation must
  // happen there (SDL_GL_SwapWindow is called from PaintAndPresentImpl).
  is_vsync_implicit_out = false;

  if (gl_context_) {
    return SurfacePaintConnectResult::kSuccessUnchanged;
  }
  if (!window_from_ui_thread()) {
    return SurfacePaintConnectResult::kFailure;
  }
  sdl_window_ = window_from_ui_thread()->GetSDLWindowHandle();
  if (!sdl_window_) {
    REXLOG_ERROR("GLES presenter: the window has no SDL_Window handle");
    return SurfacePaintConnectResult::kFailure;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  // Prefer ES 3.1 (storage buffers, used for the guest shared memory mirror),
  // but fall back to ES 3.0 where only that is available (ANGLE on macOS).
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

  SDL_GLContext context = SDL_GL_CreateContext(static_cast<SDL_Window*>(sdl_window_));
  if (!context) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    context = SDL_GL_CreateContext(static_cast<SDL_Window*>(sdl_window_));
  }
  if (!context) {
    REXLOG_ERROR("GLES presenter: SDL_GL_CreateContext failed: {}", SDL_GetError());
    return SurfacePaintConnectResult::kFailure;
  }
  gl_context_ = context;
  if (!SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_window_), context)) {
    REXLOG_ERROR("GLES presenter: SDL_GL_MakeCurrent failed: {}", SDL_GetError());
    SDL_GL_DestroyContext(context);
    gl_context_ = nullptr;
    return SurfacePaintConnectResult::kFailure;
  }

  if (!::rex::gles::LoadFunctions([](const char* name) {
        return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
      })) {
    REXLOG_ERROR("GLES presenter: unable to resolve the GLES entry points");
    SDL_GL_DestroyContext(context);
    gl_context_ = nullptr;
    return SurfacePaintConnectResult::kFailure;
  }

  const char* version =
      reinterpret_cast<const char*>(::rex::gles::GetFunctions().GetString(::rex::gles::GL_VERSION));
  const char* renderer =
      reinterpret_cast<const char*>(::rex::gles::GetFunctions().GetString(::rex::gles::GL_RENDERER));
  REXLOG_INFO("GLES presenter: {} ({})", version ? version : "?", renderer ? renderer : "?");

  // All GL calls (including the guest output blit) are made from the command
  // processor thread; release the context from the UI thread.
  SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_window_), nullptr);
  return SurfacePaintConnectResult::kSuccess;
}

bool GlesPresenter::MakeContextCurrentOnGlThread() {
  std::lock_guard lock(context_mutex_);
  if (!sdl_window_ || !gl_context_) {
    return false;
  }
  gl_thread_id_ = std::this_thread::get_id();
  if (!EnsureContextCurrent()) {
    return false;
  }
  SDL_GL_SetSwapInterval(1);
  return true;
}

void GlesPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  std::lock_guard lock(context_mutex_);
  if (!gl_context_) {
    return;
  }
  if (sdl_window_ && IsGlThread()) {
    SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_window_), nullptr);
  }
  SDL_GL_DestroyContext(static_cast<SDL_GLContext>(gl_context_));
  gl_context_ = nullptr;
  gl_thread_id_ = std::thread::id();
}

bool GlesPresenter::EnsureContextCurrent() {
  if (!gl_context_ || !sdl_window_) {
    return false;
  }
  return SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_window_),
                            static_cast<SDL_GLContext>(gl_context_));
}

uint32_t GlesPresenter::window_drawable_width() const {
  int width = 0, height = 0;
  if (sdl_window_) {
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(sdl_window_), &width, &height);
  }
  if (width <= 0) {
    width = int(surface_width_);
  }
  return uint32_t(std::max(width, 0));
}

uint32_t GlesPresenter::window_drawable_height() const {
  int width = 0, height = 0;
  if (sdl_window_) {
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(sdl_window_), &width, &height);
  }
  if (height <= 0) {
    height = int(surface_height_);
  }
  return uint32_t(std::max(height, 0));
}

uint32_t GlesPresenter::surface_width() const {
  std::lock_guard lock(context_mutex_);
  return surface_width_;
}

uint32_t GlesPresenter::surface_height() const {
  std::lock_guard lock(context_mutex_);
  return surface_height_;
}

bool GlesPresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  if (!IsGlThread()) {
    // Refreshes from other threads only update metadata; the guest output blit
    // happens on the command processor thread.
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("GLES presenter: guest output refresh from a non-GL thread; dropping");
    }
    return false;
  }
  std::lock_guard lock(context_mutex_);
  if (!EnsureContextCurrent()) {
    return false;
  }
  is_8bpc_out_ref = true;
  GlesGuestOutputRefreshContext context(is_8bpc_out_ref);
  if (!refresher(context)) {
    REXLOG_ERROR("GLES presenter: refresher failed");
    return false;
  }
  if (frontbuffer_width) {
    width_ = frontbuffer_width;
  }
  if (frontbuffer_height) {
    height_ = frontbuffer_height;
  }
  return true;
}

Presenter::PaintResult GlesPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  (void)execute_ui_drawers;
  if (!IsGlThread()) {
    // The UI thread may request a tick on resize; the guest output thread will
    // present on its next refresh.
    return PaintResult::kNotPresented;
  }
  std::lock_guard lock(context_mutex_);
  if (!EnsureContextCurrent()) {
    return PaintResult::kNotPresented;
  }
  SDL_GL_SwapWindow(static_cast<SDL_Window*>(sdl_window_));
  const char* error = SDL_GetError();
  if (error && *error) {
    fprintf(stderr, "GLES paint: SDL error '%s'\n", error);
    SDL_ClearError();
  }
  return PaintResult::kPresented;
}

bool GlesPresenter::CaptureGuestOutput(RawImage& image_out) {
  std::lock_guard lock(context_mutex_);
  if (!IsGlThread() || !EnsureContextCurrent() || !width_ || !height_) {
    return false;
  }
  const uint32_t drawable_width = window_drawable_width();
  const uint32_t drawable_height = window_drawable_height();
  if (!drawable_width || !drawable_height) {
    return false;
  }
  image_out.width = drawable_width;
  image_out.height = drawable_height;
  image_out.stride = size_t(drawable_width) * 4;
  image_out.data.resize(image_out.stride * drawable_height);
  ::rex::gles::GetFunctions().PixelStorei(::rex::gles::GL_PACK_ALIGNMENT, 1);
  ::rex::gles::GetFunctions().ReadPixels(0, 0, ::rex::gles::GLsizei(drawable_width),
                                ::rex::gles::GLsizei(drawable_height), ::rex::gles::GL_RGBA,
                                ::rex::gles::GL_UNSIGNED_BYTE, image_out.data.data());
  return true;
}

}  // namespace rex::ui::gles
