/**
 * @file        graphics/gles/texture_cache.h
 * @brief       Guest textures loaded into GLES textures (CPU untiling/decoding)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <cstdint>
#include <unordered_map>

#include <rex/graphics/pipeline/texture/cache.h>

#include "gl.h"
#include "shader.h"

namespace rex::graphics::gles {

using namespace ::rex::gles;

class GlesTextureCache final : public TextureCache {
 public:
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;
      xenos::ClampMode clamp_y : 3;
      xenos::ClampMode clamp_z : 3;
      uint32_t mag_linear : 1;
      uint32_t min_linear : 1;
      uint32_t mip_linear : 1;
      uint32_t mip_min_level : 4;
    };
    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    struct Hasher {
      size_t operator()(const SamplerParameters& parameters) const {
        return std::hash<uint32_t>{}(parameters.value);
      }
    };
    bool operator==(const SamplerParameters& other) const { return value == other.value; }
    bool operator!=(const SamplerParameters& other) const { return value != other.value; }
  };

  class GlesTexture final : public Texture {
   public:
    GlesTexture(TextureCache& texture_cache, const TextureKey& key, GLenum target);
    ~GlesTexture() override;

    GLenum target() const { return target_; }
    GLuint name() const { return name_; }
    // Ensures the GL texture object exists (called on the GL thread).
    bool EnsureCreated();

    uint32_t uploaded_level_mask() const { return uploaded_level_mask_; }
    void MarkLevelUploaded(uint32_t level) { uploaded_level_mask_ |= (1u << level); }
    void SetUploadedLevelCount(uint32_t level_count);
    // Texture::SetHostMemoryUsage is protected; expose it for the cache.
    void SetHostMemoryUsageFromCache(uint64_t usage) { SetHostMemoryUsage(usage); }

   private:
    GLenum target_ = 0;
    GLuint name_ = 0;
    uint32_t uploaded_level_mask_ = 0;
  };

  GlesTextureCache(const RegisterFile& register_file, SharedMemory& shared_memory,
                   memory::Memory& memory, uint32_t draw_resolution_scale_x,
                   uint32_t draw_resolution_scale_y);
  ~GlesTextureCache() override;

  bool Initialize();
  void Shutdown();

  SamplerParameters GetSamplerParameters(const GlesShader::SamplerBinding& binding) const;
  GLuint GetOrCreateSampler(SamplerParameters parameters);

  // Returns the GL texture for a guest texture binding (or 0 if there is none
  // or the format is unsupported), and its guest-to-host component swizzle.
  GLuint GetActiveBindingTexture(uint32_t fetch_constant, xenos::FetchOpDimension dimension,
                                 bool is_signed) const;
  uint32_t GetActiveBindingSwizzle(uint32_t fetch_constant) const;

  // Creates and loads the front buffer texture (texture fetch constant 0).
  GLuint RequestSwapTexture(uint32_t& width_out, uint32_t& height_out,
                            xenos::TextureFormat& format_out);

  // Applies a guest-to-host swizzle to the currently bound texture object.
  static void ApplySwizzle(GLenum target, uint32_t host_swizzle);

  bool IsSupportedTextureFormat(xenos::TextureFormat format) const;

  void ClearCache() override;
  void CompletedSubmissionUpdated() {}
  void BeginSubmission() {}
  void EndSubmission() {}
  void BeginFrame() {}

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override { return false; }
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  struct HostFormat {
    GLenum internal_format;
    GLenum format;
    GLenum type;
    uint32_t bytes_per_pixel;
    uint32_t host_swizzle;
  };
  bool GetHostFormat(xenos::TextureFormat format, HostFormat& host_format_out) const;

  // Decodes a single mip level of an uncompressed texture into an RGBA8 (or
  // R8/RG8, controlled by the host format) buffer.
  bool DecodeLevel(GlesTexture& texture, const HostFormat& host_format, uint32_t level,
                   uint32_t level_base, bool packed, std::vector<uint8_t>& data_out,
                   uint32_t& width_out, uint32_t& height_out, uint32_t& depth_out);

  memory::Memory& memory_;
  uint32_t max_texture_width_ = 0;
  uint32_t max_texture_height_ = 0;
  uint32_t max_texture_depth_ = 0;
  // Reused across uploads: allocating a multi-megabyte vector for every
  // texture load (the 1280x720 background is reloaded every frame on the
  // R36S) added several milliseconds per frame.
  std::vector<uint8_t> decode_staging_;
  std::unordered_map<SamplerParameters, GLuint, SamplerParameters::Hasher> samplers_;
};

}  // namespace rex::graphics::gles
