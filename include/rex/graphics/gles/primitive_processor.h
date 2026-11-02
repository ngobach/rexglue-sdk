/**
 * @file        graphics/gles/primitive_processor.h
 * @brief       Primitive processing with CPU-converted GLES index buffers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <rex/graphics/primitive_processor.h>

#include "gl.h"

namespace rex::graphics::gles {

using namespace ::rex::gles;

class GlesPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  GlesPrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                         SharedMemory& shared_memory)
      : PrimitiveProcessor(register_file, memory, shared_memory) {}
  ~GlesPrimitiveProcessor() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void BeginFrame() { ClearPerFrameCache(); }

  std::pair<GLuint, GLintptr> GetBuiltinIndexBuffer(size_t handle) const {
    return std::make_pair(builtin_index_buffer_, GLintptr(GetBuiltinIndexBufferOffsetBytes(handle)));
  }
  std::pair<GLuint, GLintptr> GetConvertedIndexBuffer(size_t handle) const {
    return std::make_pair(converted_index_buffer_, GLintptr(handle));
  }
  // Uploads the host-converted indices written this frame to the GL element
  // buffer. Must be called before drawing when the index buffer type is
  // kHostConverted.
  bool UploadConvertedIndexBuffers();

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count,
                                                       bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  GLuint builtin_index_buffer_ = 0;
  size_t builtin_index_buffer_size_ = 0;

  GLuint converted_index_buffer_ = 0;
  std::vector<uint8_t> converted_staging_;
  size_t converted_staging_used_ = 0;
  bool converted_dirty_ = false;
};

}  // namespace rex::graphics::gles
