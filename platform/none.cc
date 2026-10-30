// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The backend for a system this build has none for.
//
// Every entry point is here and every one of them says no: a program
// that asks `Available()` first gets an answer it can act on, and one
// that does not gets a null window rather than a missing symbol. A link
// error for a window would mean a program that draws cannot even be
// built for a server, which is the wrong trade (D155).

#include "platform/common.h"
#include <cstdint>


extern "C" int32_t wolang_platform_available(void) { return 0; }

extern "C" const char* wolang_platform_backend(void) { return "none"; }

extern "C" WolangWindow* wolang_window_open(const char* title,
                                            int32_t width, int32_t height,
                                            uint32_t flags) {
  (void)title;
  (void)width;
  (void)height;
  (void)flags;
  return nullptr;
}

extern "C" void wolang_window_close(WolangWindow* window) { (void)window; }

extern "C" int32_t wolang_window_poll(WolangWindow* window,
                                      WolangEvent* out) {
  (void)window;
  (void)out;
  return 0;
}

extern "C" void wolang_window_size(WolangWindow* window, int32_t* width,
                                   int32_t* height) {
  (void)window;
  if (width != nullptr) {
    *width = 0;
  }
  if (height != nullptr) {
    *height = 0;
  }
}

extern "C" double wolang_window_scale(WolangWindow* window) {
  (void)window;
  return 1.0;
}

extern "C" void wolang_window_set_title(WolangWindow* window,
                                        const char* title) {
  (void)window;
  (void)title;
}

extern "C" void wolang_window_present(WolangWindow* window,
                                      const uint32_t* pixels,
                                      int32_t width, int32_t height) {
  (void)window;
  (void)pixels;
  (void)width;
  (void)height;
}

extern "C" int32_t wolang_window_gl_start(WolangWindow* window) {
  (void)window;
  return 0;
}

extern "C" void wolang_window_swap(WolangWindow* window,
                                   int32_t interval) {
  (void)window;
  (void)interval;
}

extern "C" void* wolang_gl_proc(const char* name) {
  (void)name;
  return nullptr;
}

extern "C" const char* wolang_clipboard_text(void) { return ""; }

extern "C" void wolang_clipboard_set(const char* text) { (void)text; }

extern "C" WolangAudio* wolang_audio_open(int32_t rate, int32_t channels) {
  (void)rate;
  (void)channels;
  return nullptr;
}

extern "C" void wolang_audio_close(WolangAudio* audio) { (void)audio; }

extern "C" int32_t wolang_audio_queue(WolangAudio* audio,
                                      const float* samples,
                                      int32_t frames) {
  (void)audio;
  (void)samples;
  (void)frames;
  return 0;
}

extern "C" int32_t wolang_audio_queued(WolangAudio* audio) {
  (void)audio;
  return 0;
}

extern "C" int32_t wolang_audio_room(WolangAudio* audio) {
  (void)audio;
  return 0;
}

// (D247) The window, cursor, pad and dialog additions: all of them no.

extern "C" void wolang_window_cursor_visible(WolangWindow* window,
                                             int32_t visible) {
  (void)window;
  (void)visible;
}

extern "C" void wolang_window_cursor_shape(WolangWindow* window,
                                           int32_t shape) {
  (void)window;
  (void)shape;
}

extern "C" int32_t wolang_window_cursor_lock(WolangWindow* window,
                                             int32_t locked) {
  (void)window;
  (void)locked;
  return 0;
}

extern "C" void wolang_window_set_fullscreen(WolangWindow* window,
                                             int32_t fullscreen) {
  (void)window;
  (void)fullscreen;
}

extern "C" int32_t wolang_window_is_fullscreen(WolangWindow* window) {
  (void)window;
  return 0;
}

extern "C" void wolang_window_set_size(WolangWindow* window, int32_t width,
                                       int32_t height) {
  (void)window;
  (void)width;
  (void)height;
}

extern "C" void wolang_window_set_min_size(WolangWindow* window,
                                           int32_t width, int32_t height) {
  (void)window;
  (void)width;
  (void)height;
}

extern "C" void wolang_window_set_position(WolangWindow* window, int32_t x,
                                           int32_t y) {
  (void)window;
  (void)x;
  (void)y;
}

extern "C" void wolang_window_position(WolangWindow* window, int32_t* x,
                                       int32_t* y) {
  (void)window;
  if (x != nullptr) {
    *x = 0;
  }
  if (y != nullptr) {
    *y = 0;
  }
}

extern "C" const char* wolang_window_drop_path(WolangWindow* window,
                                               int32_t index) {
  (void)window;
  (void)index;
  return nullptr;
}

extern "C" int32_t wolang_gamepad_poll(WolangEvent* out) {
  (void)out;
  return 0;
}

extern "C" int32_t wolang_gamepad_connected(int32_t index) {
  (void)index;
  return 0;
}

extern "C" const char* wolang_gamepad_name(int32_t index) {
  (void)index;
  return "";
}

extern "C" int32_t wolang_gamepad_button(int32_t index, int32_t button) {
  (void)index;
  (void)button;
  return 0;
}

extern "C" double wolang_gamepad_axis(int32_t index, int32_t axis) {
  (void)index;
  (void)axis;
  return 0.0;
}

extern "C" int32_t wolang_dialog_available(void) { return 0; }

extern "C" int32_t wolang_dialog_message(const char* title, const char* text,
                                         int32_t level, int32_t buttons) {
  (void)title;
  (void)text;
  (void)level;
  (void)buttons;
  return kWolangAnswerUnavailable;
}

extern "C" const char* wolang_dialog_open_file(const char* title) {
  (void)title;
  return nullptr;
}

extern "C" const char* wolang_dialog_save_file(const char* title,
                                               const char* start_name) {
  (void)title;
  (void)start_name;
  return nullptr;
}
