/**
 * @file        graphics/gles/primitive_processor.cpp
 * @brief       Primitive processing with CPU-converted GLES index buffers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for more details.
 */

#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/gles/primitive_processor.h>
#include <rex/logging.h>

namespace rex::graphics::gles {

GlesPrimitiveProcessor::~GlesPrimitiveProcessor() { Shutdown(true); }

bool GlesPrimitiveProcessor::Initialize() {
  // GLES supports triangle fans, line loops and points as primitive types.
  // Quad lists are converted to triangle lists on the CPU, and rectangle lists
  // are expanded in the vertex shader.
  if (!InitializeCommon(true, true, true, false, true, false)) {
    Shutdown();
    return false;
  }
  GetFunctions().GenBuffers(1, &converted_index_buffer_);
  if (!converted_index_buffer_) {
    REXGPU_ERROR("GLES primitive processor: failed to create the converted index buffer");
    Shutdown();
    return false;
  }
  converted_staging_.resize(kMinRequiredConvertedIndexBufferSize + 64);
  GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, converted_index_buffer_);
  GetFunctions().BufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(converted_staging_.size() + 64), nullptr,
                   GL_STREAM_DRAW);
  GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  return true;
}

void GlesPrimitiveProcessor::Shutdown(bool from_destructor) {
  if (builtin_index_buffer_) {
    GetFunctions().DeleteBuffers(1, &builtin_index_buffer_);
    builtin_index_buffer_ = 0;
  }
  if (converted_index_buffer_) {
    GetFunctions().DeleteBuffers(1, &converted_index_buffer_);
    converted_index_buffer_ = 0;
  }
  converted_staging_.clear();
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool GlesPrimitiveProcessor::InitializeBuiltinIndexBuffer(size_t size_bytes,
                                                          std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_true(builtin_index_buffer_ == 0);
  std::vector<uint8_t> data(size_bytes);
  fill_callback(data.data());
  GetFunctions().GenBuffers(1, &builtin_index_buffer_);
  if (!builtin_index_buffer_) {
    REXGPU_ERROR("GLES primitive processor: failed to create the built-in index buffer");
    return false;
  }
  builtin_index_buffer_size_ = size_bytes;
  GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, builtin_index_buffer_);
  GetFunctions().BufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(size_bytes), data.data(), GL_STATIC_DRAW);
  GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  return true;
}

void* GlesPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  const size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t)
                                                                 : sizeof(uint32_t);
  const size_t needed = index_size * index_count +
                        (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0);
  if (converted_staging_used_ + needed > converted_staging_.size()) {
    REXGPU_ERROR("GLES primitive processor: converted index staging overflow");
    return nullptr;
  }
  uint8_t* mapping = converted_staging_.data() + converted_staging_used_;
  size_t handle = converted_staging_used_;
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset = GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    handle += size_t(coalignment_offset);
  }
  converted_staging_used_ += needed;
  backend_handle_out = handle;
  converted_dirty_ = true;
  return mapping;
}

bool GlesPrimitiveProcessor::UploadConvertedIndexBuffers() {
  if (!converted_dirty_ || !converted_staging_used_) {
    return true;
  }
  GetFunctions().BindBuffer(GL_ELEMENT_ARRAY_BUFFER, converted_index_buffer_);
  // Orphan the previous contents so the driver doesn't stall on them.
  GetFunctions().BufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(converted_staging_.size()), nullptr,
                   GL_STREAM_DRAW);
  GetFunctions().BufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, GLsizeiptr(converted_staging_used_),
                      converted_staging_.data());
  converted_dirty_ = false;
  return true;
}

}  // namespace rex::graphics::gles
