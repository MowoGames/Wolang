// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/platform/gpu.h"

#include <string>
#include <vector>

#if defined(_WIN32)
#define WOLANG_GL_CALL __stdcall
#else
#define WOLANG_GL_CALL
#endif

typedef unsigned int GlUint;
typedef int GlInt;
typedef float GlFloat;
typedef intptr_t GlSize;

enum {
  kGlZero = 0,
  kGlOne = 1,
  kGlPoints = 0,
  kGlLines = 1,
  kGlLineStrip = 3,
  kGlTriangles = 4,
  kGlTriangleStrip = 5,
  kGlTriangleFan = 6,
  kGlDepthBufferBit = 0x100,
  kGlColorBufferBit = 0x4000,
  kGlFront = 0x0404,
  kGlBack = 0x0405,
  kGlCullFace = 0x0B44,
  kGlDepthTest = 0x0B71,
  kGlBlend = 0x0BE2,
  kGlScissorTest = 0x0C11,
  kGlUnpackAlignment = 0x0CF5,
  kGlPackAlignment = 0x0D05,
  kGlTexture2d = 0x0DE1,
  kGlUnsignedByte = 0x1401,
  kGlUnsignedInt = 0x1405,
  kGlFloat = 0x1406,
  kGlRgba = 0x1908,
  kGlVersion = 0x1F02,
  kGlNearest = 0x2600,
  kGlLinear = 0x2601,
  kGlTextureMagFilter = 0x2800,
  kGlTextureMinFilter = 0x2801,
  kGlTextureWrapS = 0x2802,
  kGlTextureWrapT = 0x2803,
  kGlRepeat = 0x2901,
  kGlClampToEdge = 0x812F,
  kGlTexture0 = 0x84C0,
  kGlArrayBuffer = 0x8892,
  kGlElementArrayBuffer = 0x8893,
  kGlStaticDraw = 0x88E4,
  kGlDynamicDraw = 0x88E8,
  kGlFragmentShader = 0x8B30,
  kGlVertexShader = 0x8B31,
  kGlCompileStatus = 0x8B81,
  kGlLinkStatus = 0x8B82,
  kGlInfoLogLength = 0x8B84,
  kGlFramebuffer = 0x8D40,
  kGlColorAttachment0 = 0x8CE0,
  kGlFramebufferComplete = 0x8CD5,
  kGlRgba8 = 0x8058,
};

struct GlTable {
  void(WOLANG_GL_CALL* ClearColor)(GlFloat, GlFloat, GlFloat, GlFloat) = nullptr;
  void(WOLANG_GL_CALL* ClearDepth)(double) = nullptr;
  void(WOLANG_GL_CALL* Clear)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* Viewport)(GlInt, GlInt, GlInt, GlInt) = nullptr;
  void(WOLANG_GL_CALL* Scissor)(GlInt, GlInt, GlInt, GlInt) = nullptr;
  void(WOLANG_GL_CALL* Enable)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* Disable)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* BlendFunc)(GlUint, GlUint) = nullptr;
  void(WOLANG_GL_CALL* DepthFunc)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* DepthMask)(unsigned char) = nullptr;
  void(WOLANG_GL_CALL* CullFace)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* GenBuffers)(GlInt, GlUint*) = nullptr;
  void(WOLANG_GL_CALL* BindBuffer)(GlUint, GlUint) = nullptr;
  void(WOLANG_GL_CALL* BufferData)(GlUint, GlSize, const void*, GlUint) = nullptr;
  void(WOLANG_GL_CALL* DeleteBuffers)(GlInt, const GlUint*) = nullptr;
  void(WOLANG_GL_CALL* GenVertexArrays)(GlInt, GlUint*) = nullptr;
  void(WOLANG_GL_CALL* BindVertexArray)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* DeleteVertexArrays)(GlInt, const GlUint*) = nullptr;
  void(WOLANG_GL_CALL* VertexAttribPointer)(GlUint, GlInt, GlUint, unsigned char, GlInt, const void*) = nullptr;
  void(WOLANG_GL_CALL* EnableVertexAttribArray)(GlUint) = nullptr;
  GlUint(WOLANG_GL_CALL* CreateShader)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* ShaderSource)(GlUint, GlInt, const char* const*, const GlInt*) = nullptr;
  void(WOLANG_GL_CALL* CompileShader)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* GetShaderiv)(GlUint, GlUint, GlInt*) = nullptr;
  void(WOLANG_GL_CALL* GetShaderInfoLog)(GlUint, GlInt, GlInt*, char*) = nullptr;
  void(WOLANG_GL_CALL* DeleteShader)(GlUint) = nullptr;
  GlUint(WOLANG_GL_CALL* CreateProgram)() = nullptr;
  void(WOLANG_GL_CALL* AttachShader)(GlUint, GlUint) = nullptr;
  void(WOLANG_GL_CALL* LinkProgram)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* GetProgramiv)(GlUint, GlUint, GlInt*) = nullptr;
  void(WOLANG_GL_CALL* GetProgramInfoLog)(GlUint, GlInt, GlInt*, char*) = nullptr;
  void(WOLANG_GL_CALL* UseProgram)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* DeleteProgram)(GlUint) = nullptr;
  GlInt(WOLANG_GL_CALL* GetUniformLocation)(GlUint, const char*) = nullptr;
  void(WOLANG_GL_CALL* Uniform1f)(GlInt, GlFloat) = nullptr;
  void(WOLANG_GL_CALL* Uniform2f)(GlInt, GlFloat, GlFloat) = nullptr;
  void(WOLANG_GL_CALL* Uniform3f)(GlInt, GlFloat, GlFloat, GlFloat) = nullptr;
  void(WOLANG_GL_CALL* Uniform4f)(GlInt, GlFloat, GlFloat, GlFloat, GlFloat) = nullptr;
  void(WOLANG_GL_CALL* Uniform1i)(GlInt, GlInt) = nullptr;
  void(WOLANG_GL_CALL* UniformMatrix4fv)(GlInt, GlInt, unsigned char, const GlFloat*) = nullptr;
  void(WOLANG_GL_CALL* GenTextures)(GlInt, GlUint*) = nullptr;
  void(WOLANG_GL_CALL* BindTexture)(GlUint, GlUint) = nullptr;
  void(WOLANG_GL_CALL* TexImage2D)(GlUint, GlInt, GlInt, GlInt, GlInt, GlInt, GlUint, GlUint, const void*) = nullptr;
  void(WOLANG_GL_CALL* TexSubImage2D)(GlUint, GlInt, GlInt, GlInt, GlInt, GlInt, GlUint, GlUint, const void*) = nullptr;
  void(WOLANG_GL_CALL* TexParameteri)(GlUint, GlUint, GlInt) = nullptr;
  void(WOLANG_GL_CALL* ActiveTexture)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* DeleteTextures)(GlInt, const GlUint*) = nullptr;
  void(WOLANG_GL_CALL* PixelStorei)(GlUint, GlInt) = nullptr;
  void(WOLANG_GL_CALL* DrawArrays)(GlUint, GlInt, GlInt) = nullptr;
  void(WOLANG_GL_CALL* DrawElements)(GlUint, GlInt, GlUint, const void*) = nullptr;
  void(WOLANG_GL_CALL* GenFramebuffers)(GlInt, GlUint*) = nullptr;
  void(WOLANG_GL_CALL* BindFramebuffer)(GlUint, GlUint) = nullptr;
  void(WOLANG_GL_CALL* FramebufferTexture2D)(GlUint, GlUint, GlUint, GlUint, GlInt) = nullptr;
  GlUint(WOLANG_GL_CALL* CheckFramebufferStatus)(GlUint) = nullptr;
  void(WOLANG_GL_CALL* DeleteFramebuffers)(GlInt, const GlUint*) = nullptr;
  void(WOLANG_GL_CALL* ReadPixels)(GlInt, GlInt, GlInt, GlInt, GlUint, GlUint, void*) = nullptr;
  GlUint(WOLANG_GL_CALL* GetError)() = nullptr;
  const unsigned char*(WOLANG_GL_CALL* GetString)(GlUint) = nullptr;
};

struct Gpu {
  static GlTable sm_gl;
  static bool sm_ready;
  static std::string sm_log;
  static std::string sm_version;
};

GlTable Gpu::sm_gl;
bool Gpu::sm_ready = false;
std::string Gpu::sm_log;
std::string Gpu::sm_version;

template <typename T>
static bool Load(T* slot, const char* name) {
  *slot = reinterpret_cast<T>(wolang_gl_proc(name));
  return *slot != nullptr;
}

static bool LoadAll() {
  GlTable& gl = Gpu::sm_gl;
  bool ok = true;
  ok &= Load(&gl.ClearColor, "glClearColor");
  ok &= Load(&gl.ClearDepth, "glClearDepth");
  ok &= Load(&gl.Clear, "glClear");
  ok &= Load(&gl.Viewport, "glViewport");
  ok &= Load(&gl.Scissor, "glScissor");
  ok &= Load(&gl.Enable, "glEnable");
  ok &= Load(&gl.Disable, "glDisable");
  ok &= Load(&gl.BlendFunc, "glBlendFunc");
  ok &= Load(&gl.DepthFunc, "glDepthFunc");
  ok &= Load(&gl.DepthMask, "glDepthMask");
  ok &= Load(&gl.CullFace, "glCullFace");
  ok &= Load(&gl.GenBuffers, "glGenBuffers");
  ok &= Load(&gl.BindBuffer, "glBindBuffer");
  ok &= Load(&gl.BufferData, "glBufferData");
  ok &= Load(&gl.DeleteBuffers, "glDeleteBuffers");
  ok &= Load(&gl.GenVertexArrays, "glGenVertexArrays");
  ok &= Load(&gl.BindVertexArray, "glBindVertexArray");
  ok &= Load(&gl.DeleteVertexArrays, "glDeleteVertexArrays");
  ok &= Load(&gl.VertexAttribPointer, "glVertexAttribPointer");
  ok &= Load(&gl.EnableVertexAttribArray, "glEnableVertexAttribArray");
  ok &= Load(&gl.CreateShader, "glCreateShader");
  ok &= Load(&gl.ShaderSource, "glShaderSource");
  ok &= Load(&gl.CompileShader, "glCompileShader");
  ok &= Load(&gl.GetShaderiv, "glGetShaderiv");
  ok &= Load(&gl.GetShaderInfoLog, "glGetShaderInfoLog");
  ok &= Load(&gl.DeleteShader, "glDeleteShader");
  ok &= Load(&gl.CreateProgram, "glCreateProgram");
  ok &= Load(&gl.AttachShader, "glAttachShader");
  ok &= Load(&gl.LinkProgram, "glLinkProgram");
  ok &= Load(&gl.GetProgramiv, "glGetProgramiv");
  ok &= Load(&gl.GetProgramInfoLog, "glGetProgramInfoLog");
  ok &= Load(&gl.UseProgram, "glUseProgram");
  ok &= Load(&gl.DeleteProgram, "glDeleteProgram");
  ok &= Load(&gl.GetUniformLocation, "glGetUniformLocation");
  ok &= Load(&gl.Uniform1f, "glUniform1f");
  ok &= Load(&gl.Uniform2f, "glUniform2f");
  ok &= Load(&gl.Uniform3f, "glUniform3f");
  ok &= Load(&gl.Uniform4f, "glUniform4f");
  ok &= Load(&gl.Uniform1i, "glUniform1i");
  ok &= Load(&gl.UniformMatrix4fv, "glUniformMatrix4fv");
  ok &= Load(&gl.GenTextures, "glGenTextures");
  ok &= Load(&gl.BindTexture, "glBindTexture");
  ok &= Load(&gl.TexImage2D, "glTexImage2D");
  ok &= Load(&gl.TexSubImage2D, "glTexSubImage2D");
  ok &= Load(&gl.TexParameteri, "glTexParameteri");
  ok &= Load(&gl.ActiveTexture, "glActiveTexture");
  ok &= Load(&gl.DeleteTextures, "glDeleteTextures");
  ok &= Load(&gl.PixelStorei, "glPixelStorei");
  ok &= Load(&gl.DrawArrays, "glDrawArrays");
  ok &= Load(&gl.DrawElements, "glDrawElements");
  ok &= Load(&gl.GenFramebuffers, "glGenFramebuffers");
  ok &= Load(&gl.BindFramebuffer, "glBindFramebuffer");
  ok &= Load(&gl.FramebufferTexture2D, "glFramebufferTexture2D");
  ok &= Load(&gl.CheckFramebufferStatus, "glCheckFramebufferStatus");
  ok &= Load(&gl.DeleteFramebuffers, "glDeleteFramebuffers");
  ok &= Load(&gl.ReadPixels, "glReadPixels");
  ok &= Load(&gl.GetError, "glGetError");
  ok &= Load(&gl.GetString, "glGetString");
  return ok;
}

static GlUint BlendFactor(int32_t factor) {
  static const GlUint factors[] = {0, 1, 0x0302, 0x0303, 0x0304, 0x0305};
  if (factor < 0 || factor > 5) {
    return kGlOne;
  }
  return factors[factor];
}

static GlUint DrawMode(int32_t mode) {
  static const GlUint modes[] = {kGlTriangles, kGlLines, kGlPoints, kGlTriangleStrip, kGlTriangleFan, kGlLineStrip};
  if (mode < 0 || mode > 5) {
    return kGlTriangles;
  }
  return modes[mode];
}

static GlUint CompileStage(GlUint kind, const char* source) {
  GlTable& gl = Gpu::sm_gl;
  const GlUint shader = gl.CreateShader(kind);
  gl.ShaderSource(shader, 1, &source, nullptr);
  gl.CompileShader(shader);
  GlInt status = 0;
  gl.GetShaderiv(shader, kGlCompileStatus, &status);
  if (status != 0) {
    return shader;
  }

  GlInt length = 0;
  gl.GetShaderiv(shader, kGlInfoLogLength, &length);
  std::vector<char> text(length > 1 ? length : 1, 0);
  gl.GetShaderInfoLog(shader, length, nullptr, text.data());
  Gpu::sm_log = std::string(kind == kGlVertexShader ? "vertex: " : "fragment: ") + text.data();
  gl.DeleteShader(shader);
  return 0;
}

extern "C" int32_t wolang_gpu_ready(void) {
  if (Gpu::sm_ready) {
    return 1;
  }
  Gpu::sm_ready = LoadAll();
  if (Gpu::sm_ready) {
    const unsigned char* text = Gpu::sm_gl.GetString(kGlVersion);
    Gpu::sm_version = text != nullptr ? reinterpret_cast<const char*>(text) : "";
  }
  return Gpu::sm_ready ? 1 : 0;
}

extern "C" const char* wolang_gpu_version(void) { return Gpu::sm_version.c_str(); }

extern "C" int32_t wolang_gpu_error(void) {
  if (!Gpu::sm_ready) {
    return 0;
  }
  return static_cast<int32_t>(Gpu::sm_gl.GetError());
}

extern "C" void wolang_gpu_clear(float red, float green, float blue, float alpha, float depth, int32_t mask) {
  if (!Gpu::sm_ready) {
    return;
  }

  GlTable& gl = Gpu::sm_gl;
  GlUint bits = 0;
  if ((mask & 1) != 0) {
    gl.ClearColor(red, green, blue, alpha);
    bits |= kGlColorBufferBit;
  }
  if ((mask & 2) != 0) {
    gl.ClearDepth(depth);
    gl.DepthMask(1);
    bits |= kGlDepthBufferBit;
  }
  gl.Clear(bits);
}

extern "C" void wolang_gpu_viewport(int32_t x, int32_t y, int32_t width, int32_t height) {
  if (Gpu::sm_ready) {
    Gpu::sm_gl.Viewport(x, y, width, height);
  }
}

extern "C" void wolang_gpu_blend(int32_t on, int32_t source, int32_t destination) {
  if (!Gpu::sm_ready) {
    return;
  }

  if (on == 0) {
    Gpu::sm_gl.Disable(kGlBlend);
    return;
  }

  Gpu::sm_gl.Enable(kGlBlend);
  Gpu::sm_gl.BlendFunc(BlendFactor(source), BlendFactor(destination));
}

extern "C" void wolang_gpu_depth(int32_t test, int32_t write, int32_t function) {
  if (!Gpu::sm_ready) {
    return;
  }

  if (test == 0) {
    Gpu::sm_gl.Disable(kGlDepthTest);
  } else {
    Gpu::sm_gl.Enable(kGlDepthTest);
  }
  if (function < 0 || function > 7) {
    function = 1;
  }
  Gpu::sm_gl.DepthFunc(0x0200 + static_cast<GlUint>(function));
  Gpu::sm_gl.DepthMask(write != 0 ? 1 : 0);
}

extern "C" void wolang_gpu_cull(int32_t mode) {
  if (!Gpu::sm_ready) {
    return;
  }

  if (mode == 0) {
    Gpu::sm_gl.Disable(kGlCullFace);
    return;
  }

  Gpu::sm_gl.Enable(kGlCullFace);
  Gpu::sm_gl.CullFace(mode == 2 ? kGlFront : kGlBack);
}

extern "C" void wolang_gpu_scissor(int32_t on, int32_t x, int32_t y, int32_t width, int32_t height) {
  if (!Gpu::sm_ready) {
    return;
  }

  if (on == 0) {
    Gpu::sm_gl.Disable(kGlScissorTest);
    return;
  }

  Gpu::sm_gl.Enable(kGlScissorTest);
  Gpu::sm_gl.Scissor(x, y, width, height);
}

extern "C" uint32_t wolang_gpu_buffer_new(void) {
  GlUint name = 0;
  if (Gpu::sm_ready) {
    Gpu::sm_gl.GenBuffers(1, &name);
  }
  return name;
}

static void Upload(uint32_t buffer, const void* data, int64_t bytes, int32_t dynamic) {
  if (!Gpu::sm_ready || buffer == 0 || bytes < 0) {
    return;
  }

  Gpu::sm_gl.BindBuffer(kGlArrayBuffer, buffer);
  Gpu::sm_gl.BufferData(kGlArrayBuffer, static_cast<GlSize>(bytes), data, dynamic != 0 ? kGlDynamicDraw : kGlStaticDraw);
  Gpu::sm_gl.BindBuffer(kGlArrayBuffer, 0);
}

extern "C" void wolang_gpu_vertex_data(uint32_t buffer, const float* data, int64_t count, int32_t dynamic) {
  Upload(buffer, data, count * 4, dynamic);
}

// Indices upload through the array binding too (D280): the element binding
// belongs to a layout, and core profile refuses it with none bound.
extern "C" void wolang_gpu_index_data(uint32_t buffer, const uint32_t* data, int64_t count, int32_t dynamic) {
  Upload(buffer, data, count * 4, dynamic);
}

extern "C" void wolang_gpu_buffer_delete(uint32_t buffer) {
  if (Gpu::sm_ready && buffer != 0) {
    Gpu::sm_gl.DeleteBuffers(1, &buffer);
  }
}

extern "C" uint32_t wolang_gpu_layout_new(void) {
  GlUint name = 0;
  if (Gpu::sm_ready) {
    Gpu::sm_gl.GenVertexArrays(1, &name);
  }
  return name;
}

extern "C" void wolang_gpu_layout_attribute(uint32_t layout, uint32_t buffer, int32_t index, int32_t count, int32_t stride, int32_t offset) {
  if (!Gpu::sm_ready || layout == 0 || buffer == 0 || index < 0) {
    return;
  }

  GlTable& gl = Gpu::sm_gl;
  gl.BindVertexArray(layout);
  gl.BindBuffer(kGlArrayBuffer, buffer);
  gl.VertexAttribPointer(static_cast<GlUint>(index), count, kGlFloat, 0, stride, reinterpret_cast<const void*>(static_cast<intptr_t>(offset)));
  gl.EnableVertexAttribArray(static_cast<GlUint>(index));
  gl.BindBuffer(kGlArrayBuffer, 0);
  gl.BindVertexArray(0);
}

extern "C" void wolang_gpu_layout_indices(uint32_t layout, uint32_t buffer) {
  if (!Gpu::sm_ready || layout == 0) {
    return;
  }

  Gpu::sm_gl.BindVertexArray(layout);
  Gpu::sm_gl.BindBuffer(kGlElementArrayBuffer, buffer);
  Gpu::sm_gl.BindVertexArray(0);
}

extern "C" void wolang_gpu_layout_delete(uint32_t layout) {
  if (Gpu::sm_ready && layout != 0) {
    Gpu::sm_gl.DeleteVertexArrays(1, &layout);
  }
}

extern "C" uint32_t wolang_gpu_program_new(const char* vertex, const char* fragment) {
  Gpu::sm_log.clear();
  if (!Gpu::sm_ready || vertex == nullptr || fragment == nullptr) {
    Gpu::sm_log = "no GL context";
    return 0;
  }

  GlTable& gl = Gpu::sm_gl;
  const GlUint vs = CompileStage(kGlVertexShader, vertex);
  if (vs == 0) {
    return 0;
  }

  const GlUint fs = CompileStage(kGlFragmentShader, fragment);
  if (fs == 0) {
    gl.DeleteShader(vs);
    return 0;
  }

  const GlUint program = gl.CreateProgram();
  gl.AttachShader(program, vs);
  gl.AttachShader(program, fs);
  gl.LinkProgram(program);
  gl.DeleteShader(vs);
  gl.DeleteShader(fs);

  GlInt status = 0;
  gl.GetProgramiv(program, kGlLinkStatus, &status);
  if (status != 0) {
    return program;
  }

  GlInt length = 0;
  gl.GetProgramiv(program, kGlInfoLogLength, &length);
  std::vector<char> text(length > 1 ? length : 1, 0);
  gl.GetProgramInfoLog(program, length, nullptr, text.data());
  Gpu::sm_log = std::string("link: ") + text.data();
  gl.DeleteProgram(program);
  return 0;
}

extern "C" const char* wolang_gpu_program_log(void) { return Gpu::sm_log.c_str(); }

extern "C" void wolang_gpu_program_use(uint32_t program) {
  if (Gpu::sm_ready) {
    Gpu::sm_gl.UseProgram(program);
  }
}

extern "C" void wolang_gpu_program_delete(uint32_t program) {
  if (Gpu::sm_ready && program != 0) {
    Gpu::sm_gl.DeleteProgram(program);
  }
}

extern "C" int32_t wolang_gpu_uniform(uint32_t program, const char* name) {
  if (!Gpu::sm_ready || program == 0 || name == nullptr) {
    return -1;
  }
  return Gpu::sm_gl.GetUniformLocation(program, name);
}

extern "C" void wolang_gpu_uniform_float(int32_t location, int32_t count, float a, float b, float c, float d) {
  if (!Gpu::sm_ready || location < 0) {
    return;
  }

  GlTable& gl = Gpu::sm_gl;
  switch (count) {
    case 1:
      gl.Uniform1f(location, a);
      break;
    case 2:
      gl.Uniform2f(location, a, b);
      break;
    case 3:
      gl.Uniform3f(location, a, b, c);
      break;
    default:
      gl.Uniform4f(location, a, b, c, d);
      break;
  }
}

extern "C" void wolang_gpu_uniform_int(int32_t location, int32_t value) {
  if (Gpu::sm_ready && location >= 0) {
    Gpu::sm_gl.Uniform1i(location, value);
  }
}

extern "C" void wolang_gpu_uniform_matrix(int32_t location, const float* matrix) {
  if (Gpu::sm_ready && location >= 0 && matrix != nullptr) {
    Gpu::sm_gl.UniformMatrix4fv(location, 1, 0, matrix);
  }
}

extern "C" uint32_t wolang_gpu_texture_new(int32_t width, int32_t height, const void* rgba, int32_t filter, int32_t wrap) {
  if (!Gpu::sm_ready || width < 1 || height < 1) {
    return 0;
  }

  GlTable& gl = Gpu::sm_gl;
  GlUint name = 0;
  gl.GenTextures(1, &name);
  gl.BindTexture(kGlTexture2d, name);
  gl.PixelStorei(kGlUnpackAlignment, 1);
  gl.TexImage2D(kGlTexture2d, 0, kGlRgba8, width, height, 0, kGlRgba, kGlUnsignedByte, rgba);
  const GlInt sampling = filter != 0 ? kGlLinear : kGlNearest;
  const GlInt edge = wrap != 0 ? kGlRepeat : kGlClampToEdge;
  gl.TexParameteri(kGlTexture2d, kGlTextureMinFilter, sampling);
  gl.TexParameteri(kGlTexture2d, kGlTextureMagFilter, sampling);
  gl.TexParameteri(kGlTexture2d, kGlTextureWrapS, edge);
  gl.TexParameteri(kGlTexture2d, kGlTextureWrapT, edge);
  gl.BindTexture(kGlTexture2d, 0);
  return name;
}

extern "C" void wolang_gpu_texture_update(uint32_t texture, int32_t x, int32_t y, int32_t width, int32_t height, const void* rgba) {
  if (!Gpu::sm_ready || texture == 0 || rgba == nullptr) {
    return;
  }

  GlTable& gl = Gpu::sm_gl;
  gl.BindTexture(kGlTexture2d, texture);
  gl.PixelStorei(kGlUnpackAlignment, 1);
  gl.TexSubImage2D(kGlTexture2d, 0, x, y, width, height, kGlRgba, kGlUnsignedByte, rgba);
  gl.BindTexture(kGlTexture2d, 0);
}

extern "C" void wolang_gpu_texture_bind(int32_t unit, uint32_t texture) {
  if (!Gpu::sm_ready || unit < 0) {
    return;
  }

  Gpu::sm_gl.ActiveTexture(kGlTexture0 + static_cast<GlUint>(unit));
  Gpu::sm_gl.BindTexture(kGlTexture2d, texture);
}

extern "C" void wolang_gpu_texture_delete(uint32_t texture) {
  if (Gpu::sm_ready && texture != 0) {
    Gpu::sm_gl.DeleteTextures(1, &texture);
  }
}

extern "C" uint32_t wolang_gpu_target_new(uint32_t texture) {
  if (!Gpu::sm_ready || texture == 0) {
    return 0;
  }

  GlTable& gl = Gpu::sm_gl;
  GlUint name = 0;
  gl.GenFramebuffers(1, &name);
  gl.BindFramebuffer(kGlFramebuffer, name);
  gl.FramebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, kGlTexture2d, texture, 0);
  const GlUint status = gl.CheckFramebufferStatus(kGlFramebuffer);
  gl.BindFramebuffer(kGlFramebuffer, 0);
  if (status != kGlFramebufferComplete) {
    gl.DeleteFramebuffers(1, &name);
    return 0;
  }
  return name;
}

extern "C" void wolang_gpu_target_bind(uint32_t target) {
  if (Gpu::sm_ready) {
    Gpu::sm_gl.BindFramebuffer(kGlFramebuffer, target);
  }
}

extern "C" void wolang_gpu_target_delete(uint32_t target) {
  if (Gpu::sm_ready && target != 0) {
    Gpu::sm_gl.DeleteFramebuffers(1, &target);
  }
}

extern "C" void wolang_gpu_draw(uint32_t layout, int32_t mode, int32_t first, int32_t count) {
  if (!Gpu::sm_ready || layout == 0 || count < 1) {
    return;
  }

  Gpu::sm_gl.BindVertexArray(layout);
  Gpu::sm_gl.DrawArrays(DrawMode(mode), first, count);
  Gpu::sm_gl.BindVertexArray(0);
}

extern "C" void wolang_gpu_draw_indexed(uint32_t layout, int32_t mode, int32_t count, int32_t first) {
  if (!Gpu::sm_ready || layout == 0 || count < 1) {
    return;
  }

  Gpu::sm_gl.BindVertexArray(layout);
  Gpu::sm_gl.DrawElements(DrawMode(mode), count, kGlUnsignedInt, reinterpret_cast<const void*>(static_cast<intptr_t>(first) * 4));
  Gpu::sm_gl.BindVertexArray(0);
}

extern "C" int32_t wolang_gpu_read(int32_t x, int32_t y, int32_t width, int32_t height, void* rgba) {
  if (!Gpu::sm_ready || rgba == nullptr || width < 1 || height < 1) {
    return 0;
  }

  Gpu::sm_gl.PixelStorei(kGlPackAlignment, 1);
  Gpu::sm_gl.ReadPixels(x, y, width, height, kGlRgba, kGlUnsignedByte, rgba);
  return 1;
}
