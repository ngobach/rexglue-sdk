/**
 * @file        graphics/gles/render_target_cache.cpp
 * @brief       EDRAM emulation with GLES framebuffer objects
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/gles/constants.h>
#include <rex/graphics/gles/render_target_cache.h>
#include <rex/graphics/gles/shared_memory.h>
#include <rex/graphics/gles/texture_cache.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>

namespace rex::graphics::gles {

namespace {

// Converts the guest color render target format to a GLES internal format that
// can be rendered to, or 0 if unsupported.
GLenum ColorFormatToGL(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return GL_RGBA8;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      return GL_RGB10_A2;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return GL_RG16F;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return GL_RGBA16F;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return GL_RG32F;
    default:
      return 0;
  }
}

GLenum DepthFormatToGL(xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
    case xenos::DepthRenderTargetFormat::kD24FS8:
      // Float24 depth is rendered through the unorm24 path for now; the exact
      // encoding is only relevant for floats outside 0...1.
      return GL_DEPTH24_STENCIL8;
    default:
      return 0;
  }
}

}  // namespace

GlesRenderTargetCache::GlesRenderTargetCache(const RegisterFile& register_file,
                                             const memory::Memory& memory,
                                             uint32_t draw_resolution_scale_x,
                                             uint32_t draw_resolution_scale_y)
    : RenderTargetCache(register_file, memory, draw_resolution_scale_x, draw_resolution_scale_y),
      memory_(memory) {}

GlesRenderTargetCache::~GlesRenderTargetCache() { Shutdown(); }

bool GlesRenderTargetCache::Initialize() {
  GLint max_texture_size = 0;
  GetFunctions().GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  max_width_ = max_height_ = uint32_t(std::max(max_texture_size, 0));
  if (!max_width_) {
    REXGPU_ERROR("GLES render target cache: GL_MAX_TEXTURE_SIZE is zero");
    return false;
  }
  GetFunctions().GenFramebuffers(1, &draw_framebuffer_);
  if (!draw_framebuffer_) {
    REXGPU_ERROR("GLES render target cache: glGenFramebuffers failed");
    return false;
  }
  InitializeCommon();
  REXGPU_INFO("GLES render target cache: max render target {}x{}", max_width_, max_height_);
  return true;
}

void GlesRenderTargetCache::Shutdown() {
  DestroyAllRenderTargets(true);
  if (draw_framebuffer_) {
    GetFunctions().DeleteFramebuffers(1, &draw_framebuffer_);
    draw_framebuffer_ = 0;
  }
  ShutdownCommon();
}

void GlesRenderTargetCache::ClearCache() {
  RenderTargetCache::ClearCache();
  last_update_color_texture_ = 0;
  last_update_depth_texture_ = 0;
  draw_width_ = draw_height_ = 0;
}

void GlesRenderTargetCache::DestroyRenderTarget(GlesRenderTarget* render_target) {
  if (!render_target) {
    return;
  }
  if (render_target->framebuffer) {
    GetFunctions().DeleteFramebuffers(1, &render_target->framebuffer);
  }
  if (render_target->texture) {
    GetFunctions().DeleteTextures(1, &render_target->texture);
  }
  delete render_target;
}

RenderTargetCache::RenderTarget* GlesRenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  const uint32_t width = key.GetWidth();
  const uint32_t height = GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples);
  if (!width || !height || width > max_width_ || height > max_height_) {
    return nullptr;
  }

  GLenum internal_format;
  GLenum attachment;
  if (key.is_depth) {
    internal_format = DepthFormatToGL(key.GetDepthFormat());
    attachment = GL_DEPTH_STENCIL_ATTACHMENT;
  } else {
    internal_format = ColorFormatToGL(key.GetColorFormat());
    attachment = GL_COLOR_ATTACHMENT0;
  }
  if (!internal_format) {
    static uint32_t logged_format_mask = 0;
    const uint32_t format_bit = 1u << (key.resource_format & 31);
    if (!(logged_format_mask & format_bit)) {
      logged_format_mask |= format_bit;
      REXGPU_WARN("GLES render target cache: unsupported {} format {}",
                  key.is_depth ? "depth" : "color", key.GetFormatName());
    }
    return nullptr;
  }

  GlesRenderTarget* render_target =
      new GlesRenderTarget(key, width, height, key.is_depth, internal_format, attachment);
  GetFunctions().GenTextures(1, &render_target->texture);
  if (!render_target->texture) {
    delete render_target;
    return nullptr;
  }
  GetFunctions().ActiveTexture(GLenum(GL_TEXTURE0 + kScratchTextureUnit));
  GetFunctions().BindTexture(GL_TEXTURE_2D, render_target->texture);
  GLenum format = GL_RGBA;
  GLenum type = GL_UNSIGNED_BYTE;
  if (key.is_depth) {
    format = GL_DEPTH_STENCIL;
    type = GL_UNSIGNED_INT_24_8;
  } else if (internal_format == GL_RG16F) {
    format = GL_RG;
    type = GL_FLOAT;
  } else if (internal_format == GL_RG32F) {
    format = GL_RG;
    type = GL_FLOAT;
  } else if (internal_format == GL_RGBA16F) {
    format = GL_RGBA;
    type = GL_FLOAT;
  }
  GetFunctions().TexImage2D(GL_TEXTURE_2D, 0, GLint(internal_format), GLsizei(width), GLsizei(height), 0,
                   format, type, nullptr);
  GetFunctions().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  GetFunctions().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  GetFunctions().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  GetFunctions().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GetFunctions().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

  GetFunctions().GenFramebuffers(1, &render_target->framebuffer);
  GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, render_target->framebuffer);
  GetFunctions().FramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, render_target->texture, 0);
  const GLenum status = GetFunctions().CheckFramebufferStatus(GL_FRAMEBUFFER);
  GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    REXGPU_ERROR("GLES render target cache: framebuffer incomplete (0x{:X}) for {}", uint32_t(status),
                 key.GetDebugName());
    DestroyRenderTarget(render_target);
    return nullptr;
  }

  REXGPU_DEBUG("GLES render target cache: created {}x{} {} render target ({})", width, height,
               key.is_depth ? "depth" : "color", key.GetDebugName());
  return render_target;
}

bool GlesRenderTargetCache::PerformTransfers() {
  RenderTarget* const* accumulated = last_update_accumulated_render_targets();
  const std::vector<Transfer>* transfers = last_update_transfers();
  if (!accumulated || !transfers) {
    return true;
  }
  bool all_performed = true;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (accumulated[i] == nullptr || transfers[i].empty()) {
      continue;
    }
    auto* dest = static_cast<GlesRenderTarget*>(accumulated[i]);
    for (const Transfer& transfer : transfers[i]) {
      auto* source = static_cast<GlesRenderTarget*>(transfer.source);
      if (!source) {
        continue;
      }
      if (source->internal_format != dest->internal_format ||
          source->width != dest->width || source->height != dest->height) {
        static bool logged = false;
        if (!logged) {
          logged = true;
          REXGPU_WARN(
              "GLES render target cache: unsupported ownership transfer between "
              "{} and {}",
              source->key().GetDebugName(), dest->key().GetDebugName());
        }
        all_performed = false;
        continue;
      }
      Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
      const uint32_t rectangle_count =
          transfer.GetRectangles(dest->key().base_tiles, dest->key().GetPitchTiles(),
                                 dest->key().msaa_samples, dest->key().Is64bpp(), rectangles);
      GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, source->framebuffer);
      GetFunctions().BindFramebuffer(GL_DRAW_FRAMEBUFFER, dest->framebuffer);
      const GLbitfield blit_mask =
          dest->is_depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT;
      for (uint32_t r = 0; r < rectangle_count; ++r) {
        const Transfer::Rectangle& rectangle = rectangles[r];
        if (!rectangle.width_pixels || !rectangle.height_pixels) {
          continue;
        }
        // Transfers copy EDRAM tiles between host render targets. Both are
        // stored in the same Y-mirrored convention, so the same rectangle works
        // for the source and the destination.
        const GLint x0 = GLint(rectangle.x_pixels);
        const GLint y0 = GLint(rectangle.y_pixels);
        const GLint x1 = GLint(rectangle.x_pixels + rectangle.width_pixels);
        const GLint y1 = GLint(rectangle.y_pixels + rectangle.height_pixels);
        GetFunctions().BlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, blit_mask, GL_NEAREST);
      }
      GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      GetFunctions().BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }
  return all_performed;
}

void GlesRenderTargetCache::BindDrawFramebuffer(RenderTarget* const* render_targets) {
  GLuint color_texture = 0;
  GLuint depth_texture = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  if (render_targets) {
    auto* depth = static_cast<GlesRenderTarget*>(render_targets[0]);
    auto* color = static_cast<GlesRenderTarget*>(render_targets[1]);
    if (color) {
      color_texture = color->texture;
      width = color->width;
      height = color->height;
    }
    if (depth) {
      depth_texture = depth->texture;
      if (!width) {
        width = depth->width;
        height = depth->height;
      }
    }
  }

  draw_width_ = width;
  draw_height_ = height;
  last_update_color_texture_ = color_texture;
  last_update_color_width_ = width;
  last_update_color_height_ = height;
  last_update_depth_texture_ = depth_texture;

  if (!draw_framebuffer_ || (!color_texture && !depth_texture)) {
    GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }
  GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, draw_framebuffer_);
  GetFunctions().FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);
  GetFunctions().FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                             depth_texture, 0);
  if (color_texture) {
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    GetFunctions().DrawBuffers(1, &draw_buffer);
  } else {
    GetFunctions().DrawBuffers(1, &GL_NONE);
  }
  if (color_texture && getenv("GLES_GREEN_RT")) {
    GetFunctions().Disable(GL_SCISSOR_TEST);
    GetFunctions().ClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    GetFunctions().Clear(GL_COLOR_BUFFER_BIT);
  }
  const GLenum status = GetFunctions().CheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXGPU_ERROR("GLES render target cache: draw framebuffer incomplete (0x{:X})",
                   uint32_t(status));
    }
  }
}

bool GlesRenderTargetCache::Update(bool is_rasterization_done,
                                   reg::RB_DEPTHCONTROL normalized_depth_control,
                                   uint32_t normalized_color_mask, const Shader& vertex_shader) {
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }
  if (!PerformTransfers()) {
    return false;
  }
  BindDrawFramebuffer(last_update_accumulated_render_targets());
  return true;
}

void GlesRenderTargetCache::ReadRenderTargetPixels(GlesRenderTarget& render_target, uint32_t guest_x,
                                                   uint32_t guest_y, uint32_t width,
                                                   uint32_t height, uint8_t* dest,
                                                   uint32_t dest_stride) {
  // Guest row 0 is stored at GL row (height - 1), so read the mirrored block and
  // flip it while copying into the destination.
  const uint32_t gl_y_base = render_target.height - (guest_y + height);
  resolve_row_.resize(size_t(width) * 4);
  GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, render_target.framebuffer);
  GetFunctions().PixelStorei(GL_PACK_ALIGNMENT, 1);
  for (uint32_t row = 0; row < height; ++row) {
    GetFunctions().ReadPixels(GLint(guest_x), GLint(gl_y_base + (height - 1 - row)), GLsizei(width), 1,
                     GL_RGBA, GL_UNSIGNED_BYTE, resolve_row_.data());
    std::memcpy(dest + size_t(row) * dest_stride, resolve_row_.data(), size_t(width) * 4);
  }
  GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

bool GlesRenderTargetCache::ResolveCopy(const memory::Memory& memory,
                                        const draw_util::ResolveInfo& resolve_info,
                                        uint32_t& written_address_out,
                                        uint32_t& written_length_out) {
  const uint32_t dest_bpe_log2 =
      rex::log2_floor(FormatInfo::Get(xenos::TextureFormat(resolve_info.copy_dest_info.copy_dest_format))
                          ->bits_per_pixel >> 3);
  if (dest_bpe_log2 != 2) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXGPU_WARN("GLES render target cache: resolve to a {}-byte-per-pixel format is unsupported",
                  uint32_t(1) << dest_bpe_log2);
    }
    return false;
  }

  uint32_t dump_base, dump_row_length, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length, dump_rows, dump_pitch);
  if (!dump_row_length || !dump_rows) {
    return true;
  }

  // 1x 32bpp EDRAM tiles are 80x16 pixels.
  constexpr uint32_t kTileWidth = xenos::kEdramTileWidthSamples;
  constexpr uint32_t kTileHeight = xenos::kEdramTileHeightSamples;
  const uint32_t dump_width = dump_row_length * kTileWidth;
  const uint32_t dump_height = dump_rows * kTileHeight;
  resolve_dump_.assign(size_t(dump_width) * dump_height * 4, 0);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length, dump_rows, dump_pitch,
                                 resolve_rectangles_);
  for (const ResolveCopyDumpRectangle& rectangle : resolve_rectangles_) {
    auto* render_target = static_cast<GlesRenderTarget*>(rectangle.render_target);
    if (!render_target) {
      continue;
    }
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    const uint32_t dispatch_count =
        rectangle.GetDispatches(dump_pitch, dump_row_length, dispatches);
    for (uint32_t d = 0; d < dispatch_count; ++d) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[d];
      for (uint32_t row = 0; row < dispatch.height_tiles; ++row) {
        const uint32_t dump_row = dispatch.offset / dump_pitch + row;
        const uint32_t first_column = dispatch.offset % dump_pitch;
        // Fast path: the whole run of tiles is contiguous in the source render
        // target with the same pitch, so it can be read in a single call.
        const uint32_t first_tile =
            dump_base + dispatch.offset + row * dump_pitch;
        const uint32_t source_local_first =
            (first_tile - render_target->key().base_tiles) & (xenos::kEdramTileCount - 1);
        const uint32_t source_local_last =
            (first_tile + dispatch.width_tiles - 1 - render_target->key().base_tiles) &
            (xenos::kEdramTileCount - 1);
        const bool contiguous =
            render_target->key().GetPitchTiles() == dump_pitch &&
            source_local_last >= source_local_first &&
            source_local_last - source_local_first + 1 == dispatch.width_tiles;
        if (contiguous) {
          const uint32_t source_pitch_tiles = render_target->key().GetPitchTiles();
          const uint32_t source_x = (source_local_first % source_pitch_tiles) * kTileWidth;
          const uint32_t source_y = (source_local_first / source_pitch_tiles) * kTileHeight;
          const uint32_t copy_width = dispatch.width_tiles * kTileWidth;
          ReadRenderTargetPixels(*render_target, source_x, source_y, copy_width, kTileHeight,
                                 resolve_dump_.data() +
                                     (size_t(dump_row) * kTileHeight * dump_width +
                                      size_t(first_column) * kTileWidth) * 4,
                                 dump_width * 4);
        } else {
          // General case: read each tile separately.
          for (uint32_t column = 0; column < dispatch.width_tiles; ++column) {
            const uint32_t global_tile = first_tile + column;
            const uint32_t local_tile =
                (global_tile - render_target->key().base_tiles) & (xenos::kEdramTileCount - 1);
            const uint32_t source_pitch_tiles = render_target->key().GetPitchTiles();
            const uint32_t source_x = (local_tile % source_pitch_tiles) * kTileWidth;
            const uint32_t source_y = (local_tile / source_pitch_tiles) * kTileHeight;
            ReadRenderTargetPixels(*render_target, source_x, source_y, kTileWidth, kTileHeight,
                                   resolve_dump_.data() +
                                       (size_t(dump_row) * kTileHeight * dump_width +
                                        size_t(first_column + column) * kTileWidth) * 4,
                                   dump_width * 4);
          }
        }
      }
    }
  }


  // Copy the dumped EDRAM pixels to the guest destination texture.
  const auto& regs = register_file();
  const uint32_t copy_dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
  const auto rb_copy_dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  const auto rb_copy_dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
  const uint32_t dest_pitch = rb_copy_dest_pitch.copy_dest_pitch;
  const uint32_t copy_width = resolve_info.coordinate_info.width_div_8 * 8;
  const uint32_t copy_height = resolve_info.height_div_8 * 8;
  const uint32_t dest_origin_x = resolve_info.copy_dest_coordinate_info.offset_x_div_8 * 8;
  const uint32_t dest_origin_y = resolve_info.copy_dest_coordinate_info.offset_y_div_8 * 8;
  const uint32_t source_origin_x = resolve_info.coordinate_info.edram_offset_x_div_8 * 8;
  const uint32_t source_origin_y = resolve_info.coordinate_info.edram_offset_y_div_8 * 8;
  if (source_origin_x + copy_width > dump_width || source_origin_y + copy_height > dump_height) {
    REXGPU_ERROR("GLES render target cache: resolve copy outside the EDRAM dump");
    return false;
  }

  const uint32_t copy_end = resolve_info.copy_dest_extent_start +
                            resolve_info.copy_dest_extent_length;
  if (copy_end > SharedMemory::kBufferSize) {
    return false;
  }
  uint8_t* destination = memory.TranslatePhysical(copy_dest_base);
  for (uint32_t y = 0; y < copy_height; ++y) {
    const uint8_t* source_row =
        resolve_dump_.data() +
        (size_t(source_origin_y + y) * dump_width + source_origin_x) * 4;
    for (uint32_t x = 0; x < copy_width; ++x) {
      uint32_t value;
      std::memcpy(&value, source_row + size_t(x) * 4, 4);
      xenos::Endian endian = xenos::Endian::kNone;
      switch (rb_copy_dest_info.copy_dest_endian) {
        case xenos::Endian128::k8in16:
          endian = xenos::Endian::k8in16;
          break;
        case xenos::Endian128::k8in32:
          endian = xenos::Endian::k8in32;
          break;
        case xenos::Endian128::k16in32:
          endian = xenos::Endian::k16in32;
          break;
        default:
          break;
      }
      value = xenos::GpuSwap(value, endian);
      if (rb_copy_dest_info.copy_dest_swap) {
        value = (value & 0xFF00FF00) | ((value & 0x00FF0000) >> 16) | ((value & 0x000000FF) << 16);
      }
      const uint32_t offset = uint32_t(texture_util::GetTiledOffset2D(
          int32_t(dest_origin_x + x), int32_t(dest_origin_y + y), dest_pitch, dest_bpe_log2));
      std::memcpy(destination + offset, &value, 4);
    }
  }

  written_address_out = resolve_info.copy_dest_extent_start;
  written_length_out = resolve_info.copy_dest_extent_length;
  return true;
}

bool GlesRenderTargetCache::ResolveClear(const draw_util::ResolveInfo& resolve_info) {
  const bool clear_depth = resolve_info.IsClearingDepth();
  const bool clear_color = resolve_info.IsClearingColor();
  if (!clear_depth && !clear_color) {
    return true;
  }

  Transfer::Rectangle clear_rectangle;
  RenderTarget* clear_render_targets[2] = {nullptr, nullptr};
  if (!PrepareHostRenderTargetsResolveClear(resolve_info, clear_rectangle,
                                            clear_render_targets[0], clear_transfers_[0],
                                            clear_render_targets[1], clear_transfers_[1])) {
    return true;
  }
  // Perform the ownership transfers needed before clearing.
  for (uint32_t i = 0; i < 2; ++i) {
    auto* dest = static_cast<GlesRenderTarget*>(clear_render_targets[i]);
    if (!dest) {
      continue;
    }
    for (const Transfer& transfer : clear_transfers_[i]) {
      auto* source = static_cast<GlesRenderTarget*>(transfer.source);
      if (!source) {
        continue;
      }
      if (source->internal_format != dest->internal_format || source->width != dest->width ||
          source->height != dest->height) {
        continue;
      }
      Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
      const uint32_t rectangle_count =
          transfer.GetRectangles(dest->key().base_tiles, dest->key().GetPitchTiles(),
                                 dest->key().msaa_samples, dest->key().Is64bpp(), rectangles);
      GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, source->framebuffer);
      GetFunctions().BindFramebuffer(GL_DRAW_FRAMEBUFFER, dest->framebuffer);
      for (uint32_t r = 0; r < rectangle_count; ++r) {
        const Transfer::Rectangle& rectangle = rectangles[r];
        GetFunctions().BlitFramebuffer(GLint(rectangle.x_pixels), GLint(rectangle.y_pixels),
                              GLint(rectangle.x_pixels + rectangle.width_pixels),
                              GLint(rectangle.y_pixels + rectangle.height_pixels),
                              GLint(rectangle.x_pixels), GLint(rectangle.y_pixels),
                              GLint(rectangle.x_pixels + rectangle.width_pixels),
                              GLint(rectangle.y_pixels + rectangle.height_pixels),
                              dest->is_depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT,
                              GL_NEAREST);
      }
      GetFunctions().BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      GetFunctions().BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }
  }

  auto* color_target = static_cast<GlesRenderTarget*>(clear_render_targets[1]);
  auto* depth_target = static_cast<GlesRenderTarget*>(clear_render_targets[0]);
  if (clear_color && color_target) {
    const uint32_t clear_value = uint32_t(resolve_info.rb_color_clear);
    GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, color_target->framebuffer);
    GetFunctions().Enable(GL_SCISSOR_TEST);
    GetFunctions().Scissor(GLint(clear_rectangle.x_pixels),
                  GLint(color_target->height - (clear_rectangle.y_pixels +
                                                clear_rectangle.height_pixels)),
                  GLsizei(clear_rectangle.width_pixels), GLsizei(clear_rectangle.height_pixels));
    GetFunctions().ClearColor(float(clear_value & 0xFF) / 255.0f, float((clear_value >> 8) & 0xFF) / 255.0f,
                     float((clear_value >> 16) & 0xFF) / 255.0f,
                     float((clear_value >> 24) & 0xFF) / 255.0f);
    GetFunctions().Clear(GL_COLOR_BUFFER_BIT);
    GetFunctions().Disable(GL_SCISSOR_TEST);
    GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  if (clear_depth && depth_target) {
    // Depth clears in resolve are rare for the titles this backend targets; the
    // guest normally clears depth with a draw. Do the color-equivalent clear for
    // the unorm24 representation anyway.
    const uint32_t depth_clear = resolve_info.rb_depth_clear;
    GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, depth_target->framebuffer);
    GetFunctions().Enable(GL_SCISSOR_TEST);
    GetFunctions().Scissor(GLint(clear_rectangle.x_pixels),
                  GLint(depth_target->height - (clear_rectangle.y_pixels +
                                                clear_rectangle.height_pixels)),
                  GLsizei(clear_rectangle.width_pixels), GLsizei(clear_rectangle.height_pixels));
    GetFunctions().ClearDepthf(float(depth_clear & 0xFFFFFF) / 16777215.0f);
    GetFunctions().ClearStencil(GLint((depth_clear >> 24) & 0xFF));
    GetFunctions().Clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    GetFunctions().Disable(GL_SCISSOR_TEST);
    GetFunctions().BindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  return true;
}

bool GlesRenderTargetCache::Resolve(const memory::Memory& memory, GlesSharedMemory& shared_memory,
                                    GlesTextureCache& texture_cache,
                                    uint32_t& written_address_out, uint32_t& written_length_out) {
  written_address_out = 0;
  written_length_out = 0;

  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(register_file(), memory, draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), false, false, resolve_info)) {
    return false;
  }
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  bool copied = true;
  if (resolve_info.copy_dest_extent_length) {
    copied = ResolveCopy(memory, resolve_info, written_address_out, written_length_out);
    if (copied && written_length_out) {
      // Keep the shared-memory (SSBO) mirror in sync and let the texture cache
      // know the data came from the GPU.
      shared_memory.WriteRangeFromGuestMemory(written_address_out, written_length_out);
      texture_cache.MarkRangeAsResolved(written_address_out, written_length_out);
      shared_memory.RangeWrittenByGpu(written_address_out, written_length_out);
    }
  }
  const bool cleared = ResolveClear(resolve_info);
  return copied && cleared;
}

}  // namespace rex::graphics::gles
