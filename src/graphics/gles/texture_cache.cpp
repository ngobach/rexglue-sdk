/**
 * @file        graphics/gles/texture_cache.cpp
 * @brief       Guest textures loaded into GLES textures (CPU untiling/decoding)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <algorithm>
#include <chrono>
#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/gles/constants.h>
#include <rex/graphics/gles/frame_stats.h>
#include <rex/graphics/gles/texture_cache.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/math.h>

namespace rex::graphics::gles {

double g_texture_load_seconds = 0.0;
double g_texture_decode_seconds = 0.0;
double g_texture_upload_seconds = 0.0;
uint64_t g_texture_load_count = 0;
uint64_t g_texture_load_bytes = 0;

namespace {

GLenum SwizzleComponentToGL(uint32_t component) {
  switch (component & 0b111) {
    case xenos::XE_GPU_TEXTURE_SWIZZLE_R:
      return GL_RED;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_G:
      return GL_GREEN;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_B:
      return GL_BLUE;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_A:
      return GL_ALPHA;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
      return GL_ZERO;
    default:
      return GL_ONE;
  }
}

GLenum ClampModeToWrap(xenos::ClampMode clamp_mode) {
  switch (clamp_mode) {
    case xenos::ClampMode::kRepeat:
      return GL_REPEAT;
    case xenos::ClampMode::kMirroredRepeat:
      return GL_MIRRORED_REPEAT;
    case xenos::ClampMode::kMirrorClampToEdge:
      // ES 3.1 has no core mirror-clamp-to-edge; mirrored repeat is the closest
      // available behavior.
      return GL_MIRRORED_REPEAT;
    default:
      return GL_CLAMP_TO_EDGE;
  }
}

xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToEdge ||
      clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    return xenos::ClampMode::kMirrorClampToEdge;
  }
  return clamp_mode;
}

uint8_t FloatToUNorm8(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return uint8_t(value * 255.0f + 0.5f);
}

// Guest k_8_8_8_8 is D3D A8R8G8B8: in little-endian memory the bytes are
// B, G, R, A, so the 32-bit value has blue in the lowest byte.
void UnpackRGBA8(uint32_t value, uint8_t* out) {
  out[0] = uint8_t((value >> 16) & 0xFF);
  out[1] = uint8_t((value >> 8) & 0xFF);
  out[2] = uint8_t(value & 0xFF);
  out[3] = uint8_t((value >> 24) & 0xFF);
}

}  // namespace

GlesTextureCache::GlesTexture::GlesTexture(TextureCache& texture_cache, const TextureKey& key,
                                           GLenum target)
    : Texture(texture_cache, key), target_(target) {}

GlesTextureCache::GlesTexture::~GlesTexture() {
  if (name_) {
    GetFunctions().DeleteTextures(1, &name_);
  }
}

bool GlesTextureCache::GlesTexture::EnsureCreated() {
  if (name_) {
    return true;
  }
  GetFunctions().GenTextures(1, &name_);
  if (!name_) {
    REXGPU_ERROR("GLES texture cache: glGenTextures failed");
    return false;
  }
  // Bind on the scratch unit, never on one a shader samples: a texture of
  // another target left on a sampled unit makes ANGLE resolve that unit to the
  // zero texture.
  GetFunctions().ActiveTexture(GLenum(GL_TEXTURE0 + kScratchTextureUnit));
  GetFunctions().BindTexture(target_, name_);
  const bool sampler_tex = target_ == GL_TEXTURE_2D_ARRAY || target_ == GL_TEXTURE_3D ||
                           target_ == GL_TEXTURE_CUBE_MAP;
  (void)sampler_tex;
  GetFunctions().TexParameteri(target_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  GetFunctions().TexParameteri(target_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  GetFunctions().TexParameteri(target_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  GetFunctions().TexParameteri(target_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GetFunctions().TexParameteri(target_, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  GetFunctions().TexParameteri(target_, GL_TEXTURE_BASE_LEVEL, 0);
  GetFunctions().TexParameteri(target_, GL_TEXTURE_MAX_LEVEL, GLint(key().mip_max_level));
  return true;
}

void GlesTextureCache::GlesTexture::SetUploadedLevelCount(uint32_t level_count) {
  if (!level_count) {
    return;
  }
  GetFunctions().TexParameteri(target_, GL_TEXTURE_MAX_LEVEL, GLint(level_count - 1));
}

GlesTextureCache::GlesTextureCache(const RegisterFile& register_file, SharedMemory& shared_memory,
                                   memory::Memory& memory, uint32_t draw_resolution_scale_x,
                                   uint32_t draw_resolution_scale_y)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y),
      memory_(memory) {}

GlesTextureCache::~GlesTextureCache() { Shutdown(); }

bool GlesTextureCache::Initialize() {
  GLint max_texture_size = 0;
  GetFunctions().GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  max_texture_width_ = max_texture_height_ = uint32_t(std::max(max_texture_size, 0));
  max_texture_depth_ = 1;
  if (max_texture_width_ == 0) {
    REXGPU_ERROR("GLES texture cache: GL_MAX_TEXTURE_SIZE is zero");
    return false;
  }
  GLint max_3d_size = 0;
  GetFunctions().GetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max_3d_size);
  GLint max_array_layers = 0;
  GetFunctions().GetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_array_layers);
  max_texture_depth_ = uint32_t(std::max({max_3d_size, max_array_layers, 1}));
  REXGPU_INFO("GLES texture cache: max 2D {}x{}, max layers {}", max_texture_width_,
              max_texture_height_, max_texture_depth_);
  return true;
}

void GlesTextureCache::Shutdown() {
  for (const auto& sampler_pair : samplers_) {
    GetFunctions().DeleteSamplers(1, &sampler_pair.second);
  }
  samplers_.clear();
  DestroyAllTextures(true);
}

void GlesTextureCache::ClearCache() {
  TextureCache::ClearCache();
  // Render targets may have been resolved into texture memory, so all host
  // copies must be reloaded from guest memory.
}

GLuint GlesTextureCache::GetOrCreateSampler(SamplerParameters parameters) {
  auto it = samplers_.find(parameters);
  if (it != samplers_.end()) {
    return it->second;
  }
  GLuint sampler = 0;
  GetFunctions().GenSamplers(1, &sampler);
  if (!sampler) {
    return 0;
  }
  GetFunctions().SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER,
                          parameters.mag_linear ? GL_LINEAR : GL_NEAREST);
  GLenum min_filter;
  if (parameters.mip_linear) {
    min_filter = parameters.min_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
  } else {
    min_filter = parameters.min_linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
  }
  GetFunctions().SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GLint(min_filter));
  GetFunctions().SamplerParameteri(sampler, GL_TEXTURE_WRAP_S,
                          GLint(ClampModeToWrap(parameters.clamp_x)));
  GetFunctions().SamplerParameteri(sampler, GL_TEXTURE_WRAP_T,
                          GLint(ClampModeToWrap(parameters.clamp_y)));
  GetFunctions().SamplerParameteri(sampler, GL_TEXTURE_WRAP_R,
                          GLint(ClampModeToWrap(parameters.clamp_z)));
  samplers_.emplace(parameters, sampler);
  return sampler;
}

GlesTextureCache::SamplerParameters GlesTextureCache::GetSamplerParameters(
    const GlesShader::SamplerBinding& binding) const {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);

  SamplerParameters parameters;
  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y, fetch_clamp_z);
  parameters.clamp_x = NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampMode(fetch_clamp_z);

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  parameters.mip_min_level = fetch.mip_min_level;
  return parameters;
}

uint32_t GlesTextureCache::GetActiveBindingSwizzle(uint32_t fetch_constant) const {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant);
  return binding ? binding->host_swizzle : uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_0000);
}

GLuint GlesTextureCache::GetActiveBindingTexture(uint32_t fetch_constant,
                                                 xenos::FetchOpDimension dimension,
                                                 bool is_signed) const {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant);
  if (!binding || !binding->texture) {
    return 0;
  }
  const GlesTexture* texture = static_cast<const GlesTexture*>(
      is_signed && binding->texture_signed ? binding->texture_signed : binding->texture);
  return texture->name();
}

GLuint GlesTextureCache::RequestSwapTexture(uint32_t& width_out, uint32_t& height_out,
                                            xenos::TextureFormat& format_out) {
  width_out = 0;
  height_out = 0;
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 ||
      key.dimension != xenos::DataDimension::k2DOrStacked) {
    REXGPU_ERROR("GLES texture cache: invalid swap texture fetch constant");
    return 0;
  }
  GlesTexture* texture = static_cast<GlesTexture*>(FindOrCreateTexture(key));
  if (!texture || !texture->EnsureCreated()) {
    return 0;
  }
  if (!LoadTextureData(*texture)) {
    return 0;
  }
  texture->MarkAsUsed();
  width_out = texture->key().GetWidth();
  height_out = texture->key().GetHeight();
  format_out = texture->key().format;
  return texture->name();
}

bool GlesTextureCache::IsSupportedTextureFormat(xenos::TextureFormat format) const {
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
    case xenos::TextureFormat::k_8_8_8_8_GAMMA_EDRAM:
    case xenos::TextureFormat::k_4_4_4_4:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_1_5_5_5:
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_16_16:
      return true;
    default:
      return false;
  }
}

bool GlesTextureCache::GetHostFormat(xenos::TextureFormat format,
                                     HostFormat& host_format_out) const {
  if (!IsSupportedTextureFormat(format)) {
    return false;
  }
  host_format_out.internal_format = GL_RGBA8;
  host_format_out.format = GL_RGBA;
  host_format_out.type = GL_UNSIGNED_BYTE;
  host_format_out.bytes_per_pixel = 4;
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
      host_format_out.host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
      break;
    case xenos::TextureFormat::k_8_8:
      host_format_out.host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
      break;
    default:
      host_format_out.host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
      break;
  }
  return true;
}

bool GlesTextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const { return false; }

uint32_t GlesTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  HostFormat host_format;
  if (!GetHostFormat(key.format, host_format)) {
    return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  return host_format.host_swizzle;
}

uint32_t GlesTextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  if (dimension == xenos::DataDimension::k3D) {
    return std::min(max_texture_width_, max_texture_depth_);
  }
  return max_texture_width_;
}

uint32_t GlesTextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  return max_texture_depth_;
}

std::unique_ptr<TextureCache::Texture> GlesTextureCache::CreateTexture(TextureKey key) {
  HostFormat host_format;
  if (!GetHostFormat(key.format, host_format)) {
    static uint32_t logged_format_mask = 0;
    if (!(logged_format_mask & (1u << (uint32_t(key.format) & 31)))) {
      logged_format_mask |= 1u << (uint32_t(key.format) & 31);
      REXGPU_WARN("GLES texture cache: unsupported texture format {}",
                  FormatInfo::Get(key.format)->name);
    }
    return nullptr;
  }
  GLenum target;
  switch (key.dimension) {
    case xenos::DataDimension::k3D:
      target = GL_TEXTURE_3D;
      break;
    case xenos::DataDimension::kCube:
      target = GL_TEXTURE_CUBE_MAP;
      break;
    default:
      target = GL_TEXTURE_2D_ARRAY;
      break;
  }
  // The Xbox reads only the layer a shader samples directly from guest memory;
  // the declared stack depth does not reserve that much guest RAM. Uploading
  // every declared layer eagerly can therefore allocate far more than the
  // device has (Feeding Frenzy declares a 1280x720 stack of 127 layers = 468 MB
  // even though the guest image cannot contain that). Keep small stacks intact
  // and clamp only stacks that exceed the configured per-texture budget.
  if (const char* limit_env = getenv("GLES_MAX_STACKED_TEXTURE_MB")) {
    const uint32_t limit_mb = uint32_t(atoi(limit_env));
    // Cube maps have a fixed six faces; clamping them would break sampling.
    const bool clammable_dimension = key.dimension == xenos::DataDimension::k2DOrStacked ||
                                     key.dimension == xenos::DataDimension::k3D;
    if (limit_mb && clammable_dimension && key.GetDepthOrArraySize() > 1) {
      const uint64_t layer_bytes = uint64_t(key.GetWidth()) * key.GetHeight() * 4;
      const uint64_t max_layers =
          std::max<uint64_t>(1, (uint64_t(limit_mb) << 20) / std::max<uint64_t>(layer_bytes, 1));
      if (uint64_t(key.GetDepthOrArraySize()) > max_layers) {
        REXGPU_WARN(
            "GLES texture cache: clamping stacked {}x{} texture from {} to {} layers "
            "({} MB budget)",
            key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(), uint32_t(max_layers),
            limit_mb);
        key.depth_or_array_size_minus_1 = uint32_t(max_layers) - 1;
      }
    }
  }
  return std::make_unique<GlesTexture>(*this, key, target);
}

void GlesTextureCache::ApplySwizzle(GLenum target, uint32_t host_swizzle) {
  static const GLenum kParameters[4] = {GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
                                        GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A};
  for (uint32_t i = 0; i < 4; ++i) {
    const GLenum component = SwizzleComponentToGL((host_swizzle >> (3 * i)) & 0b111);
    GetFunctions().TexParameteri(target, kParameters[i], GLint(component));
    const GLenum error = GetFunctions().GetError();
    if (error != GL_NO_ERROR) {
      REXGPU_ERROR("GLES swizzle: param 0x{:X} value 0x{:X} error 0x{:X} swizzle 0x{:X}",
                   uint32_t(kParameters[i]), uint32_t(component), uint32_t(error), host_swizzle);
      return;
    }
  }
}

bool GlesTextureCache::DecodeLevel(GlesTexture& texture, const HostFormat& host_format,
                                   uint32_t level, uint32_t level_base, bool packed,
                                   std::vector<uint8_t>& data_out, uint32_t& width_out,
                                   uint32_t& height_out, uint32_t& depth_out) {
  const TextureKey& key = texture.key();
  const texture_util::TextureGuestLayout& layout = texture.guest_layout();
  // For packed tails, the layout of the tail's first level describes the whole
  // tail.
  uint32_t stored_level = packed ? layout.packed_level : level;
  const texture_util::TextureGuestLayout::Level& level_layout =
      level == 0 ? layout.base : layout.mips[stored_level];

  width_out = std::max(key.GetWidth() >> level, 1u);
  height_out = std::max(key.GetHeight() >> level, 1u);
  uint32_t depth_or_array_size = std::max(key.GetDepthOrArraySize() >> level, 1u);

  const FormatInfo* format_info = FormatInfo::Get(key.format);
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (!bytes_per_block || format_info->type == FormatType::kCompressed) {
    return false;
  }
  const uint32_t bytes_per_block_log2 = rex::log2_floor(bytes_per_block);
  const bool tiled = key.tiled != 0;
  const uint32_t pitch_blocks =
      tiled ? level_layout.row_pitch_bytes / bytes_per_block : level_layout.row_pitch_bytes;

  const bool is_3d = key.dimension == xenos::DataDimension::k3D;
  const bool is_cube = key.dimension == xenos::DataDimension::kCube;
  // The game loads almost exclusively k_8_8_8_8 textures (including the
  // 1280x720 background reloaded every frame); keep the per-pixel switch out of
  // their inner loop.
  const bool is_rgba8 =
      key.format == xenos::TextureFormat::k_8_8_8_8 ||
      key.format == xenos::TextureFormat::k_8_8_8_8_A ||
      key.format == xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16 ||
      key.format == xenos::TextureFormat::k_8_8_8_8_GAMMA_EDRAM;
  const uint32_t array_size = is_cube ? 6 : (is_3d ? 1 : depth_or_array_size);
  const uint32_t depth = is_3d ? depth_or_array_size : 1;
  const uint32_t layer_count = is_cube ? 6 : array_size;
  // The caller passes an uninitialised variable here and uses it as the layer
  // count for 2D/stacked and cube textures and as the volume depth for 3D
  // textures. Leaving it unwritten made the upload use stack garbage as the
  // layer count (a 1280x720 single-layer texture became a 127-layer, 468 MB
  // TexImage3D on the R36S).
  depth_out = is_3d ? depth : layer_count;

  data_out.resize(size_t(width_out) * height_out * depth * layer_count *
                  host_format.bytes_per_pixel);
  std::memset(data_out.data(), 0, data_out.size());

  const uint8_t* guest_memory = memory_.TranslatePhysical(level_base);
  const uint32_t* const guest_memory_end = nullptr;
  (void)guest_memory_end;

  for (uint32_t layer = 0; layer < layer_count; ++layer) {
    // Cube faces and stacked 2D layers are stored as consecutive array slices;
    // 3D slices are addressed through the Z coordinate instead.
    const uint32_t slice = is_3d ? 0 : layer;
    for (uint32_t z = 0; z < depth; ++z) {
      for (uint32_t y = 0; y < height_out; ++y) {
        uint8_t* dest_row = data_out.data() +
                            ((size_t(layer) * depth + z) * height_out + y) * width_out *
                                host_format.bytes_per_pixel;
        for (uint32_t x = 0; x < width_out; ++x) {
          uint32_t guest_offset;
          if (tiled) {
            guest_offset = is_3d
                               ? uint32_t(texture_util::GetTiledOffset3D(
                                     int32_t(x), int32_t(y), int32_t(z), pitch_blocks,
                                     level_layout.z_slice_stride_block_rows, bytes_per_block_log2))
                               : uint32_t(texture_util::GetTiledOffset2D(
                                     int32_t(x), int32_t(y), pitch_blocks, bytes_per_block_log2));
            if (!is_3d) {
              guest_offset += slice * level_layout.array_slice_stride_bytes +
                              (z * level_layout.z_slice_stride_block_rows *
                               level_layout.row_pitch_bytes);
            }
          } else {
            guest_offset = (slice * level_layout.array_slice_stride_bytes +
                            z * level_layout.z_slice_stride_block_rows *
                                level_layout.row_pitch_bytes +
                            y * level_layout.row_pitch_bytes + x * bytes_per_block);
          }
          const uint8_t* source = guest_memory + guest_offset;
          uint8_t* dest = dest_row + size_t(x) * host_format.bytes_per_pixel;
          if (is_rgba8) {
            uint32_t value;
            std::memcpy(&value, source, 4);
            value = xenos::GpuSwap(value, key.endianness);
            dest[0] = uint8_t((value >> 16) & 0xFF);
            dest[1] = uint8_t((value >> 8) & 0xFF);
            dest[2] = uint8_t(value & 0xFF);
            dest[3] = uint8_t((value >> 24) & 0xFF);
            continue;
          }
          switch (key.format) {
            case xenos::TextureFormat::k_8:
            case xenos::TextureFormat::k_8_A:
            case xenos::TextureFormat::k_8_B: {
              dest[0] = source[0];
              dest[1] = 0;
              dest[2] = 0;
              dest[3] = 0;
            } break;
            case xenos::TextureFormat::k_8_8: {
              uint16_t value;
              std::memcpy(&value, source, 2);
              value = xenos::GpuSwap(value, key.endianness);
              dest[0] = uint8_t(value & 0xFF);
              dest[1] = uint8_t(value >> 8);
              dest[2] = dest[1];
              dest[3] = dest[1];
            } break;
            case xenos::TextureFormat::k_8_8_8_8:
            case xenos::TextureFormat::k_8_8_8_8_A:
            case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
            case xenos::TextureFormat::k_8_8_8_8_GAMMA_EDRAM: {
              uint32_t value;
              std::memcpy(&value, source, 4);
              value = xenos::GpuSwap(value, key.endianness);
              UnpackRGBA8(value, dest);
            } break;
            case xenos::TextureFormat::k_4_4_4_4: {
              uint16_t value;
              std::memcpy(&value, source, 2);
              value = xenos::GpuSwap(value, key.endianness);
              dest[0] = uint8_t((value & 0xF) * 17);
              dest[1] = uint8_t(((value >> 4) & 0xF) * 17);
              dest[2] = uint8_t(((value >> 8) & 0xF) * 17);
              dest[3] = uint8_t(((value >> 12) & 0xF) * 17);
            } break;
            case xenos::TextureFormat::k_5_6_5: {
              uint16_t value;
              std::memcpy(&value, source, 2);
              value = xenos::GpuSwap(value, key.endianness);
              dest[0] = uint8_t(((value >> 11) & 0x1F) * 255 / 31);
              dest[1] = uint8_t(((value >> 5) & 0x3F) * 255 / 63);
              dest[2] = uint8_t((value & 0x1F) * 255 / 31);
              dest[3] = 255;
            } break;
            case xenos::TextureFormat::k_1_5_5_5: {
              uint16_t value;
              std::memcpy(&value, source, 2);
              value = xenos::GpuSwap(value, key.endianness);
              dest[0] = uint8_t(((value >> 10) & 0x1F) * 255 / 31);
              dest[1] = uint8_t(((value >> 5) & 0x1F) * 255 / 31);
              dest[2] = uint8_t((value & 0x1F) * 255 / 31);
              dest[3] = (value & 0x8000) ? 255 : 0;
            } break;
            case xenos::TextureFormat::k_2_10_10_10:
            case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16: {
              uint32_t value;
              std::memcpy(&value, source, 4);
              value = xenos::GpuSwap(value, key.endianness);
              dest[0] = uint8_t((value & 0x3FF) * 255 / 1023);
              dest[1] = uint8_t(((value >> 10) & 0x3FF) * 255 / 1023);
              dest[2] = uint8_t(((value >> 20) & 0x3FF) * 255 / 1023);
              dest[3] = uint8_t(((value >> 30) & 0x3) * 85);
            } break;
            case xenos::TextureFormat::k_16: {
              uint16_t value;
              std::memcpy(&value, source, 2);
              value = xenos::GpuSwap(value, key.endianness);
              dest[0] = dest[1] = dest[2] = uint8_t(value >> 8);
              dest[3] = 255;
            } break;
            case xenos::TextureFormat::k_16_16: {
              uint16_t value[2];
              std::memcpy(value, source, 4);
              value[0] = xenos::GpuSwap(value[0], key.endianness);
              value[1] = xenos::GpuSwap(value[1], key.endianness);
              dest[0] = uint8_t(value[0] >> 8);
              dest[1] = uint8_t(value[1] >> 8);
              dest[2] = dest[1];
              dest[3] = 255;
            } break;
            case xenos::TextureFormat::k_16_16_16_16: {
              uint16_t value[4];
              std::memcpy(value, source, 8);
              for (uint32_t i = 0; i < 4; ++i) {
                dest[i] = uint8_t(xenos::GpuSwap(value[i], key.endianness) >> 8);
              }
            } break;
            default:
              return false;
          }
        }
      }
    }
  }

  (void)FloatToUNorm8;
  return true;
}

bool GlesTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                             bool load_mips) {
  const std::chrono::steady_clock::time_point load_start =
      std::chrono::steady_clock::now();
  struct LoadTimer {
    std::chrono::steady_clock::time_point start;
    ~LoadTimer() {
      g_texture_load_seconds += std::chrono::duration<double>(
          std::chrono::steady_clock::now() - start).count();
      ++g_texture_load_count;
    }
  } load_timer{load_start};
  GlesTexture& gles_texture = static_cast<GlesTexture&>(texture);
  const TextureKey& key = gles_texture.key();
  if (getenv("GLES_LOG_TEXTURES")) {
    static uint32_t logged_textures = 0;
    if (logged_textures < 200) {
      ++logged_textures;
      fprintf(stderr,
              "TEXTURE gl=%u %ux%ux%u fmt=%s mips=%u tiled=%d dim=%u endian=%u page=0x%X "
              "mip=0x%X\n",
              gles_texture.name(), key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(),
              FormatInfo::Get(key.format)->name, uint32_t(key.mip_max_level), int(key.tiled),
              uint32_t(key.dimension), uint32_t(key.endianness), key.base_page, key.mip_page);
      fflush(stderr);
    }
  }
  HostFormat host_format;
  if (!GetHostFormat(key.format, host_format) || !gles_texture.EnsureCreated()) {
    return false;
  }

  const texture_util::TextureGuestLayout& layout = gles_texture.guest_layout();
  const bool is_3d = key.dimension == xenos::DataDimension::k3D;
  const bool is_cube = key.dimension == xenos::DataDimension::kCube;
  const uint32_t level_first = load_base ? 0 : 1;
  const uint32_t level_last = load_mips ? key.mip_max_level : 0;
  if (level_first > level_last) {
    return true;
  }

  uint64_t host_memory_usage = 0;
  uint32_t uploaded_levels = 0;
  GLenum target = gles_texture.target();
  GetFunctions().ActiveTexture(GLenum(GL_TEXTURE0 + kScratchTextureUnit));
  GetFunctions().BindTexture(target, gles_texture.name());

  for (uint32_t level = level_first; level <= level_last; ++level) {
    const bool is_base = level == 0;
    if (is_base && !layout.base.level_data_extent_bytes) {
      continue;
    }
    const bool packed = !is_base && layout.packed_level != UINT32_MAX &&
                        level >= layout.packed_level;
    uint32_t level_address = is_base ? (key.base_page << 12) : (key.mip_page << 12);
    if (!is_base) {
      level_address += layout.mip_offsets_bytes[level];
    }
    std::vector<uint8_t> data;
    uint32_t width, height, depth;
    const auto decode_start = std::chrono::steady_clock::now();
    if (!DecodeLevel(gles_texture, host_format, level, level_address, packed, data, width, height,
                     depth)) {
      REXGPU_WARN("GLES texture cache: failed to decode level {} of {}x{}x{} {}", level,
                  key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(),
                  FormatInfo::Get(key.format)->name);
      return false;
    }
    const double level_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - decode_start).count();
    g_texture_decode_seconds += level_seconds;
    if (level_seconds > 0.020) {
      REXGPU_INFO("GLES texture cache: decode {}x{}x{} took {:.1f} ms (tiled {})", width, height,
                  depth, level_seconds * 1000.0, int(key.tiled));
    }
    const auto upload_start = std::chrono::steady_clock::now();
    if (depth > 1) {
      REXGPU_INFO("GLES texture cache: decoded {}x{}x{} (dim {}, page 0x{:X}, level {})", width,
                  height, depth, uint32_t(key.dimension), key.base_page, level);
    }
    g_texture_load_bytes += data.size();
    if (level == 0 && !is_cube && !is_3d) {
      if (const char* dump_dir = getenv("GLES_DUMP_TEXTURES")) {
        char path[512];
        snprintf(path, sizeof(path), "%s/tex_%08X_%ux%u_%s.ppm", dump_dir, key.base_page, width,
                 height, FormatInfo::Get(key.format)->name);
        if (FILE* dump_file = fopen(path, "wb")) {
          fprintf(dump_file, "P6\n%u %u\n255\n", width, height);
          for (size_t pixel = 0; pixel < size_t(width) * height; ++pixel) {
            fwrite(data.data() + pixel * 4, 1, 3, dump_file);
          }
          fclose(dump_file);
        }
      }
    }
    const uint32_t layer_count = is_cube ? 6 : (is_3d ? 1 : depth);
    const GLsizei gl_depth = GLsizei(is_3d ? depth : layer_count);
    if (is_cube) {
      // Upload faces individually.
      const size_t face_size = size_t(width) * height * host_format.bytes_per_pixel;
      for (uint32_t face = 0; face < 6; ++face) {
        GetFunctions().TexImage2D(GLenum(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face), GLint(level),
                         GLint(host_format.internal_format), GLsizei(width), GLsizei(height), 0,
                         host_format.format, host_format.type,
                         data.data() + face_size * face);
      }
    } else {
      GetFunctions().TexImage3D(target, GLint(level), GLint(host_format.internal_format), GLsizei(width),
                       GLsizei(height), gl_depth, 0, host_format.format, host_format.type,
                       data.data());
      const GLenum upload_error = GetFunctions().GetError();
      if (upload_error != GL_NO_ERROR) {
        REXGPU_ERROR(
            "GLES texture cache: upload failed (0x{:X}) for {}x{}x{} {} level {} "
            "(internal 0x{:X}, format 0x{:X}, type 0x{:X})",
            uint32_t(upload_error), width, height, depth, FormatInfo::Get(key.format)->name, level,
            uint32_t(host_format.internal_format), uint32_t(host_format.format),
            uint32_t(host_format.type));
      }
    }
    gles_texture.MarkLevelUploaded(level);
    g_texture_upload_seconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - upload_start).count();
    uploaded_levels = std::max(uploaded_levels, level + 1);
    host_memory_usage += uint64_t(width) * height * (is_3d ? depth : layer_count) *
                         host_format.bytes_per_pixel;
  }

  gles_texture.SetUploadedLevelCount(uploaded_levels ? uploaded_levels : 1);
  gles_texture.SetHostMemoryUsageFromCache(host_memory_usage);
  gles_texture.LogAction("Loaded");
  return true;
}

}  // namespace rex::graphics::gles
