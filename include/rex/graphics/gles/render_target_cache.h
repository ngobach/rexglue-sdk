/**
 * @file        graphics/gles/render_target_cache.h
 * @brief       EDRAM emulation with GLES framebuffer objects
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/pipeline/render_target/cache.h>

#include "gl.h"

namespace rex::graphics::gles {

using namespace ::rex::gles;

class GlesSharedMemory;
class GlesTextureCache;

class GlesRenderTargetCache final : public RenderTargetCache {
 public:
  GlesRenderTargetCache(const RegisterFile& register_file, const memory::Memory& memory,
                        uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y);
  ~GlesRenderTargetCache() override;

  bool Initialize();
  void Shutdown();

  Path GetPath() const override { return Path::kHostRenderTargets; }

  void ClearCache() override;

  // Performs the resolve/copy according to the current registers into guest
  // memory, and clears the render targets if requested.
  bool Resolve(const memory::Memory& memory, GlesSharedMemory& shared_memory,
               GlesTextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out);

  bool Update(bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask, const Shader& vertex_shader) override;

  // Framebuffer with the last update's render targets attached, ready for
  // drawing (0 if nothing is bound).
  GLuint draw_framebuffer() const { return draw_framebuffer_; }
  uint32_t draw_width() const { return draw_width_; }
  uint32_t draw_height() const { return draw_height_; }
  // Color texture of the last update's color render target (for presentation),
  // or 0.
  GLuint last_update_color_texture() const { return last_update_color_texture_; }
  uint32_t last_update_color_width() const { return last_update_color_width_; }
  uint32_t last_update_color_height() const { return last_update_color_height_; }

 protected:
  uint32_t GetMaxRenderTargetWidth() const override { return max_width_; }
  uint32_t GetMaxRenderTargetHeight() const override { return max_height_; }
  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;
  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override {
    (void)format;
    return false;
  }
  bool IsGammaFormatHostStorageSeparate() const override { return false; }

 private:
  struct GlesRenderTarget final : RenderTarget {
    GlesRenderTarget(RenderTargetKey key, uint32_t width, uint32_t height, bool is_depth,
                     GLenum internal_format, GLenum attachment)
        : RenderTarget(key),
          width(width),
          height(height),
          is_depth(is_depth),
          internal_format(internal_format),
          attachment(attachment) {}

    uint32_t width;
    uint32_t height;
    bool is_depth;
    GLenum internal_format;
    GLenum attachment;
    GLuint texture = 0;
    GLuint framebuffer = 0;
  };

  bool PerformTransfers();
  void BindDrawFramebuffer(RenderTarget* const* render_targets);
  void DestroyRenderTarget(GlesRenderTarget* render_target);

  // Resolve helpers.
  bool ResolveCopy(const memory::Memory& memory, const draw_util::ResolveInfo& resolve_info,
                   uint32_t& written_address_out, uint32_t& written_length_out);
  bool ResolveClear(const draw_util::ResolveInfo& resolve_info);
  void ReadRenderTargetPixels(GlesRenderTarget& render_target, uint32_t guest_x, uint32_t guest_y,
                              uint32_t width, uint32_t height, uint8_t* dest, uint32_t dest_stride);

  const memory::Memory& memory_;
  uint32_t max_width_ = 0;
  uint32_t max_height_ = 0;

  GLuint draw_framebuffer_ = 0;
  uint32_t draw_width_ = 0;
  uint32_t draw_height_ = 0;
  GLuint last_update_color_texture_ = 0;
  uint32_t last_update_color_width_ = 0;
  uint32_t last_update_color_height_ = 0;
  GLuint last_update_depth_texture_ = 0;

  std::vector<uint8_t> resolve_dump_;
  std::vector<uint8_t> resolve_row_;
  std::vector<ResolveCopyDumpRectangle> resolve_rectangles_;
  std::vector<Transfer> clear_transfers_[2];
};

}  // namespace rex::graphics::gles
