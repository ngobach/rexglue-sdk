/**
 * @file        graphics/gles/shared_memory.cpp
 * @brief       Shared guest memory bound as GLES shader storage buffers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <cstring>

#include <rex/graphics/gles/shared_memory.h>
#include <rex/logging.h>

namespace rex::graphics::gles {

GlesSharedMemory::GlesSharedMemory(memory::Memory& memory, uint32_t storage_buffer_count_log2,
                                   bool gpu_copy_enabled)
    : SharedMemory(memory),
      storage_buffer_count_log2_(storage_buffer_count_log2),
      gpu_copy_enabled_(gpu_copy_enabled) {}

GlesSharedMemory::~GlesSharedMemory() { Shutdown(); }

bool GlesSharedMemory::Initialize() {
  InitializeCommon();

  if (!gpu_copy_enabled_) {
    return true;
  }

  buffers_.resize(storage_buffer_count(), 0);

  // Slices are allocated when a draw first needs them. The R36S has 1 GB of
  // RAM and its Mali blob refuses large SSBO allocations, so reserving the
  // entire 512 MB mirror up front makes the command processor fail to start.
  REXGPU_INFO("GLES shared memory: {} x {} MB storage buffers (allocated on demand)",
              uint32_t(buffers_.size()), uint32_t(slice_size_log2() ?
                  (uint32_t(1) << slice_size_log2()) >> 20 : 0));
  return true;
}

bool GlesSharedMemory::EnsureBuffer(uint32_t slice_index) {
  if (slice_index >= buffers_.size()) {
    return false;
  }
  if (buffers_[slice_index]) {
    return true;
  }
  GLuint buffer = 0;
  GetFunctions().GenBuffers(1, &buffer);
  if (!buffer) {
    REXGPU_ERROR("GLES shared memory: glGenBuffers failed for slice {}", slice_index);
    return false;
  }
  const GLsizeiptr slice_size = GLsizeiptr(uint32_t(1) << slice_size_log2());
  GetFunctions().BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
  GetFunctions().BufferData(GL_SHADER_STORAGE_BUFFER, slice_size, nullptr, GL_DYNAMIC_DRAW);
  const GLenum error = GetFunctions().GetError();
  GetFunctions().BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  if (error != GL_NO_ERROR) {
    REXGPU_ERROR("GLES shared memory: failed to allocate slice {} ({} MB), GL error 0x{:X}",
                 slice_index, uint32_t(slice_size >> 20), uint32_t(error));
    GetFunctions().DeleteBuffers(1, &buffer);
    return false;
  }
  buffers_[slice_index] = buffer;
  REXGPU_INFO("GLES shared memory: allocated slice {} ({} MB)", slice_index,
              uint32_t(slice_size >> 20));
  return true;
}

void GlesSharedMemory::Shutdown() {
  if (!buffers_.empty()) {
    GetFunctions().DeleteBuffers(GLsizei(buffers_.size()), buffers_.data());
    buffers_.clear();
  }
  ShutdownCommon();
}

void GlesSharedMemory::ClearCache() {
  SharedMemory::ClearCache();
  InvalidateAllPages();
}

void GlesSharedMemory::Bind() {
  for (size_t i = 0; i < buffers_.size(); ++i) {
    GetFunctions().BindBufferBase(GL_SHADER_STORAGE_BUFFER, GLuint(i), buffers_[i]);
  }
}

bool GlesSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (!gpu_copy_enabled_) {
    return true;
  }
  const uint32_t slice_size = uint32_t(1) << slice_size_log2();
  const uint32_t slice_mask = slice_size - 1;
  for (const std::pair<uint32_t, uint32_t>& page_range : upload_page_ranges) {
    uint32_t address = page_range.first << page_size_log2();
    uint32_t remaining = page_range.second << page_size_log2();
    while (remaining) {
      uint32_t slice_index = address >> slice_size_log2();
      uint32_t slice_offset = address & slice_mask;
      uint32_t chunk = std::min(remaining, slice_size - slice_offset);
      if (slice_index >= buffers_.size()) {
        REXGPU_ERROR("GLES shared memory: upload outside the buffer at 0x{:08X}", address);
        return false;
      }
      if (!EnsureBuffer(slice_index)) {
        return false;
      }
      // Mark valid before touching the data so CPU invalidation racing with the
      // memcpy is not lost (same contract as the Vulkan backend).
      MakeRangeValid(address, chunk, false);
      GetFunctions().BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers_[slice_index]);
      GetFunctions().BufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(slice_offset), GLsizeiptr(chunk),
                          memory().TranslatePhysical(address));
      address += chunk;
      remaining -= chunk;
    }
  }
  GetFunctions().BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return true;
}

bool GlesSharedMemory::WriteRangeFromGuestMemory(uint32_t start, uint32_t length) {
  if (!gpu_copy_enabled_) {
    return true;
  }
  if (!length || start >= kBufferSize) {
    return false;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t slice_size = uint32_t(1) << slice_size_log2();
  const uint32_t slice_mask = slice_size - 1;
  while (length) {
    const uint32_t slice_index = start >> slice_size_log2();
    const uint32_t slice_offset = start & slice_mask;
    const uint32_t chunk = std::min(length, slice_size - slice_offset);
    if (slice_index >= buffers_.size()) {
      return false;
    }
    if (!EnsureBuffer(slice_index)) {
      return false;
    }
    GetFunctions().BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers_[slice_index]);
    GetFunctions().BufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(slice_offset), GLsizeiptr(chunk),
                        memory().TranslatePhysical(start));
    start += chunk;
    length -= chunk;
  }
  GetFunctions().BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return true;
}

}  // namespace rex::graphics::gles
