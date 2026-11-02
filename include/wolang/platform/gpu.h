// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// OpenGL 3.3 core for the `gpu` package, as plain functions.
//
// `wolang_gl_proc` hands back an address, and Wolang has no way to call
// an address it was handed, so the calls the `gpu` package needs live
// here, where the entry points are looked up once and called from C++
// (D280). Everything is a number or a pointer; a buffer, a texture, a
// program, a layout and a render target are the GL names, and 0 is
// "none". Nothing here owns anything: the package above deletes what it
// made. With no context every call is a no-op and `wolang_gpu_ready`
// answers 0, which is how a machine without a GPU says so.

#ifndef WOLANG_PLATFORM_GPU_H_
#define WOLANG_PLATFORM_GPU_H_

#include <stdint.h>

#include "wolang/platform/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

WOLANG_PLATFORM_EXPORT int32_t wolang_gpu_ready(void);

WOLANG_PLATFORM_EXPORT const char* wolang_gpu_version(void);

WOLANG_PLATFORM_EXPORT int32_t wolang_gpu_error(void);

WOLANG_PLATFORM_EXPORT void wolang_gpu_clear(float red, float green,
                                             float blue, float alpha,
                                             float depth, int32_t mask);

WOLANG_PLATFORM_EXPORT void wolang_gpu_viewport(int32_t x, int32_t y,
                                                int32_t width,
                                                int32_t height);

WOLANG_PLATFORM_EXPORT void wolang_gpu_blend(int32_t on, int32_t source,
                                             int32_t destination);

WOLANG_PLATFORM_EXPORT void wolang_gpu_depth(int32_t test, int32_t write,
                                             int32_t function);

WOLANG_PLATFORM_EXPORT void wolang_gpu_cull(int32_t mode);

WOLANG_PLATFORM_EXPORT void wolang_gpu_scissor(int32_t on, int32_t x,
                                               int32_t y, int32_t width,
                                               int32_t height);

WOLANG_PLATFORM_EXPORT uint32_t wolang_gpu_buffer_new(void);

WOLANG_PLATFORM_EXPORT void wolang_gpu_vertex_data(uint32_t buffer,
                                                   const float* data,
                                                   int64_t count,
                                                   int32_t dynamic);

WOLANG_PLATFORM_EXPORT void wolang_gpu_index_data(uint32_t buffer,
                                                  const uint32_t* data,
                                                  int64_t count,
                                                  int32_t dynamic);

WOLANG_PLATFORM_EXPORT void wolang_gpu_buffer_delete(uint32_t buffer);

WOLANG_PLATFORM_EXPORT uint32_t wolang_gpu_layout_new(void);

WOLANG_PLATFORM_EXPORT void wolang_gpu_layout_attribute(uint32_t layout,
                                                        uint32_t buffer,
                                                        int32_t index,
                                                        int32_t count,
                                                        int32_t stride,
                                                        int32_t offset);

WOLANG_PLATFORM_EXPORT void wolang_gpu_layout_indices(uint32_t layout,
                                                      uint32_t buffer);

WOLANG_PLATFORM_EXPORT void wolang_gpu_layout_delete(uint32_t layout);

WOLANG_PLATFORM_EXPORT uint32_t wolang_gpu_program_new(const char* vertex,
                                                       const char* fragment);

WOLANG_PLATFORM_EXPORT const char* wolang_gpu_program_log(void);

WOLANG_PLATFORM_EXPORT void wolang_gpu_program_use(uint32_t program);

WOLANG_PLATFORM_EXPORT void wolang_gpu_program_delete(uint32_t program);

WOLANG_PLATFORM_EXPORT int32_t wolang_gpu_uniform(uint32_t program,
                                                  const char* name);

WOLANG_PLATFORM_EXPORT void wolang_gpu_uniform_float(int32_t location,
                                                     int32_t count, float a,
                                                     float b, float c,
                                                     float d);

WOLANG_PLATFORM_EXPORT void wolang_gpu_uniform_int(int32_t location,
                                                   int32_t value);

WOLANG_PLATFORM_EXPORT void wolang_gpu_uniform_matrix(int32_t location,
                                                      const float* matrix);

WOLANG_PLATFORM_EXPORT uint32_t wolang_gpu_texture_new(int32_t width,
                                                       int32_t height,
                                                       const void* rgba,
                                                       int32_t filter,
                                                       int32_t wrap);

WOLANG_PLATFORM_EXPORT void wolang_gpu_texture_update(uint32_t texture,
                                                      int32_t x, int32_t y,
                                                      int32_t width,
                                                      int32_t height,
                                                      const void* rgba);

WOLANG_PLATFORM_EXPORT void wolang_gpu_texture_bind(int32_t unit,
                                                    uint32_t texture);

WOLANG_PLATFORM_EXPORT void wolang_gpu_texture_delete(uint32_t texture);

WOLANG_PLATFORM_EXPORT uint32_t wolang_gpu_target_new(uint32_t texture);

WOLANG_PLATFORM_EXPORT void wolang_gpu_target_bind(uint32_t target);

WOLANG_PLATFORM_EXPORT void wolang_gpu_target_delete(uint32_t target);

WOLANG_PLATFORM_EXPORT void wolang_gpu_draw(uint32_t layout, int32_t mode,
                                            int32_t first, int32_t count);

WOLANG_PLATFORM_EXPORT void wolang_gpu_draw_indexed(uint32_t layout,
                                                    int32_t mode,
                                                    int32_t count,
                                                    int32_t first);

WOLANG_PLATFORM_EXPORT int32_t wolang_gpu_read(int32_t x, int32_t y,
                                               int32_t width, int32_t height,
                                               void* rgba);

#ifdef __cplusplus
}
#endif

#endif  // WOLANG_PLATFORM_GPU_H_
