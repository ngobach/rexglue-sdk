/**
 * @file        graphics/gles/shared_memory.h
 * @brief       Shared guest memory bound as GLES shader storage buffers
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

#include <rex/graphics/shared_memory.h>

#include "gl.h"

namespace rex::graphics::gles {

using namespace ::rex::gles;

class GlesSharedMemory final : public SharedMemory {
 public:
  GlesSharedMemory(memory::Memory& memory, uint32_t storage_buffer_count_log2,
                   bool gpu_copy_enabled);
  ~GlesSharedMemory() override;

  bool Initialize();
  void Shutdown();

  // Whether a GPU (storage buffer) copy of guest memory is maintained. Disabled
  // on ES 3.0 hosts, where shaders read vertex data from per-draw textures.
  bool gpu_copy_enabled() const { return gpu_copy_enabled_; }

  // Number of storage buffers the shader translator expects (1 << log2).
  uint32_t storage_buffer_count() const { return 1u << storage_buffer_count_log2_; }
  uint32_t slice_size_log2() const { return kBufferSizeLog2 - storage_buffer_count_log2_; }

  // GL buffer containing the slice of guest physical memory at the address.
  GLuint buffer_for_address(uint32_t address) const {
    return buffers_[address >> slice_size_log2()];
  }
  // Byte offset of the address within the buffer returned above.
  GLintptr offset_for_address(uint32_t address) const {
    return GLintptr(address & ((uint32_t(1) << slice_size_log2()) - 1));
  }

  // Binds every slice to the storage buffer binding point the shaders use.
  void Bind();

  // Pushes a range of already-written guest memory into the storage buffer
  // mirror (used after CPU-side resolves).
  bool WriteRangeFromGuestMemory(uint32_t start, uint32_t length);

  void ClearCache() override;

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  uint32_t storage_buffer_count_log2_;
  bool gpu_copy_enabled_ = true;
  std::vector<GLuint> buffers_;
};

}  // namespace rex::graphics::gles
