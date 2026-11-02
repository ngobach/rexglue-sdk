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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL_video.h>

#if defined(__linux__)
#include <dlfcn.h>
#endif

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

using namespace ::rex::gles;

class GlesGuestOutputRefreshContext final : public Presenter::GuestOutputRefreshContext {
 public:
  explicit GlesGuestOutputRefreshContext(bool& is_8bpc_out_ref)
      : GuestOutputRefreshContext(is_8bpc_out_ref) {}
};

// GLES_LINK_SELFTEST diagnostic: links a trivial program with the current
// context so a failure can be attributed to the context (or the thread it is
// current on) rather than to a translated shader.
void LogEglState(const char* label) {
#if defined(__linux__)
  // Query the real EGL state, which SDL does not expose. EGL_DRAW/EGL_READ.
  static void* egl_lib = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
  if (!egl_lib) {
    return;
  }
  using GetCurrentSurfaceFn = void* (*)(int32_t);
  using GetCurrentContextFn = void* (*)(void);
  using GetErrorFn = int32_t (*)(void);
  auto get_current_surface = (GetCurrentSurfaceFn)dlsym(egl_lib, "eglGetCurrentSurface");
  auto get_current_context = (GetCurrentContextFn)dlsym(egl_lib, "eglGetCurrentContext");
  auto get_error = (GetErrorFn)dlsym(egl_lib, "eglGetError");
  if (!get_current_surface || !get_current_context || !get_error) {
    return;
  }
  REXLOG_INFO("GLES presenter: EGL state ({}): draw surface {}, read surface {}, context {}",
              label, get_current_surface(0x3050), get_current_surface(0x3051),
              get_current_context());
#endif
}

void LinkSelfTest(const char* label) {
  if (!std::getenv("GLES_LINK_SELFTEST")) {
    return;
  }
  static const char kVertexSource[] =
      "#version 300 es\nvoid main(){gl_Position=vec4(0.0,0.0,0.0,1.0);}\n";
  static const char kFragmentSource[] =
      "#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main(){o=vec4(1.0);}\n";
  LogEglState(label);
  auto& gl = ::rex::gles::GetFunctions();
  const char* vertex_source = kVertexSource;
  const char* fragment_source = kFragmentSource;
  GLuint vs = gl.CreateShader(GL_VERTEX_SHADER);
  gl.ShaderSource(vs, 1, &vertex_source, nullptr);
  gl.CompileShader(vs);
  GLuint fs = gl.CreateShader(GL_FRAGMENT_SHADER);
  gl.ShaderSource(fs, 1, &fragment_source, nullptr);
  gl.CompileShader(fs);
  GLint vs_ok = 0, fs_ok = 0;
  gl.GetShaderiv(vs, GL_COMPILE_STATUS, &vs_ok);
  gl.GetShaderiv(fs, GL_COMPILE_STATUS, &fs_ok);
  GLuint program = gl.CreateProgram();
  gl.AttachShader(program, vs);
  gl.AttachShader(program, fs);
  gl.LinkProgram(program);
  GLint link_ok = 0;
  gl.GetProgramiv(program, GL_LINK_STATUS, &link_ok);
  REXLOG_INFO("GLES presenter link self-test ({}): compiled {}/{}, link={}, GL error 0x{:X}", label,
              vs_ok, fs_ok, link_ok, uint32_t(gl.GetError()));
  gl.DeleteProgram(program);
  gl.DeleteShader(vs);
  gl.DeleteShader(fs);
}

// Headless/remote testing: GLES_SCREENSHOT_DIR writes the presented frame as a
// BMP every GLES_SCREENSHOT_INTERVAL frames (default 60), starting with the
// first few. BMP is used because it is trivial to write and universally
// viewable. Returns without doing anything when the variable is unset.
void MaybeSaveScreenshot(uint32_t drawable_width, uint32_t drawable_height) {
  static const char* const directory = std::getenv("GLES_SCREENSHOT_DIR");
  if (!directory || !*directory) {
    return;
  }
  static uint32_t frame = 0;
  static uint32_t interval = [] {
    const char* value = std::getenv("GLES_SCREENSHOT_INTERVAL");
    int parsed = value ? std::atoi(value) : 0;
    return parsed > 0 ? uint32_t(parsed) : 60u;
  }();
  const uint32_t current = frame++;
  if (current >= 6 && current % interval != 0) {
    return;
  }

  const size_t stride = size_t(drawable_width) * 4;
  std::vector<uint8_t> rgba(stride * drawable_height);
  ::rex::gles::Functions& gl = ::rex::gles::GetFunctions();
  gl.PixelStorei(::rex::gles::GL_PACK_ALIGNMENT, 1);
  gl.ReadPixels(0, 0, ::rex::gles::GLsizei(drawable_width),
                ::rex::gles::GLsizei(drawable_height), ::rex::gles::GL_RGBA,
                ::rex::gles::GL_UNSIGNED_BYTE, rgba.data());

  const std::string path =
      std::string(directory) + "/frame_" + std::to_string(current) + ".bmp";
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    return;
  }
  const uint32_t row_bytes = (drawable_width * 3 + 3) & ~uint32_t(3);
  const uint32_t image_bytes = row_bytes * drawable_height;
  uint8_t header[54] = {'B', 'M'};
  const uint32_t file_size = 54 + image_bytes;
  const uint32_t pixel_offset = 54;
  const uint32_t dib_size = 40;
  const int32_t width = int32_t(drawable_width);
  const int32_t height = int32_t(drawable_height);
  const uint16_t planes = 1;
  const uint16_t bits_per_pixel = 24;
  std::memcpy(header + 2, &file_size, 4);
  std::memcpy(header + 10, &pixel_offset, 4);
  std::memcpy(header + 14, &dib_size, 4);
  std::memcpy(header + 18, &width, 4);
  std::memcpy(header + 22, &height, 4);
  std::memcpy(header + 26, &planes, 2);
  std::memcpy(header + 28, &bits_per_pixel, 2);
  std::memcpy(header + 34, &image_bytes, 4);
  std::fwrite(header, 1, sizeof(header), file);
  std::vector<uint8_t> row(row_bytes, 0);
  // BMP rows are stored bottom-up, and GL's ReadPixels row 0 is the bottom
  // row, so write them in GL order.
  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* source = rgba.data() + size_t(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      row[size_t(x) * 3 + 0] = source[size_t(x) * 4 + 2];
      row[size_t(x) * 3 + 1] = source[size_t(x) * 4 + 1];
      row[size_t(x) * 3 + 2] = source[size_t(x) * 4 + 0];
    }
    std::fwrite(row.data(), 1, row_bytes, file);
  }
  std::fclose(file);
}

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
  if (std::getenv("GLES_LINK_SELFTEST")) {
    LinkSelfTest("UI thread, after SDL context creation");
  }

  // All GL calls (including the guest output blit) are made from the command
  // processor thread; release the context from the UI thread.
  REXLOG_INFO("GLES presenter: releasing the context from the UI thread: {}",
              SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_window_), nullptr));
  return SurfacePaintConnectResult::kSuccess;
}

bool GlesPresenter::MakeContextCurrentOnGlThread() {
  std::lock_guard lock(context_mutex_);
  if (!sdl_window_ || !gl_context_) {
    REXLOG_ERROR("GLES presenter: no window or context when making the GL thread current");
    return false;
  }
  gl_thread_id_ = std::this_thread::get_id();
  if (!EnsureContextCurrent()) {
    REXLOG_ERROR("GLES presenter: SDL_GL_MakeCurrent failed on the GL thread: {}",
                 SDL_GetError());
    return false;
  }
  if (std::getenv("GLES_LINK_SELFTEST")) {
    LinkSelfTest("GL thread, after handoff");
    REXLOG_INFO("GLES presenter: context is the one created by the UI thread: {}",
                SDL_GL_GetCurrentContext() == gl_context_);
    SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_window_), nullptr);
    SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_window_),
                       static_cast<SDL_GLContext>(gl_context_));
    LinkSelfTest("GL thread, after release+remake");
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
  MaybeSaveScreenshot(window_drawable_width(), window_drawable_height());
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
