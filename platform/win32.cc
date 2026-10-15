// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The Windows backend: the window and its messages are Win32, a buffer
// of pixels goes through GDI, the GPU through WGL, and sound through
// `waveOut`.
//
// `waveOut` rather than WASAPI: it is four calls, it is in every build of
// Windows since the ones nobody runs any more, and what this needs is a
// queue of frames played in order. WASAPI buys exclusive mode and low
// latency, and costs COM -- which is a trade to make when something
// needs it (D155).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mmreg.h>
#include <mmsystem.h>
#include <windowsx.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "platform/common.h"

using wolang::platform::Blank;
using wolang::platform::Events;
using wolang::platform::Ring;

namespace {

int32_t KeyFromVirtual(WPARAM key) {
  if (key >= 'A' && key <= 'Z') {
    return kWolangKeyA + static_cast<int32_t>(key - 'A');
  }
  if (key >= '0' && key <= '9') {
    return kWolangKey0 + static_cast<int32_t>(key - '0');
  }
  switch (key) {
    case VK_SPACE: return kWolangKeySpace;
    case VK_RETURN: return kWolangKeyEnter;
    case VK_ESCAPE: return kWolangKeyEscape;
    case VK_TAB: return kWolangKeyTab;
    case VK_BACK: return kWolangKeyBackspace;
    case VK_DELETE: return kWolangKeyDelete;
    case VK_LEFT: return kWolangKeyLeft;
    case VK_RIGHT: return kWolangKeyRight;
    case VK_UP: return kWolangKeyUp;
    case VK_DOWN: return kWolangKeyDown;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
      return kWolangKeyShift;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
      return kWolangKeyControl;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return kWolangKeyAlt;
    case VK_LWIN: case VK_RWIN: return kWolangKeySuper;
    case VK_F1: return kWolangKeyF1;
    case VK_F2: return kWolangKeyF2;
    case VK_F3: return kWolangKeyF3;
    case VK_F4: return kWolangKeyF4;
    case VK_F5: return kWolangKeyF5;
    case VK_F6: return kWolangKeyF6;
    case VK_F7: return kWolangKeyF7;
    case VK_F8: return kWolangKeyF8;
    case VK_F9: return kWolangKeyF9;
    case VK_F10: return kWolangKeyF10;
    case VK_F11: return kWolangKeyF11;
    case VK_F12: return kWolangKeyF12;
    case VK_OEM_MINUS: return kWolangKeyMinus;
    case VK_OEM_PLUS: return kWolangKeyEqual;
    case VK_OEM_COMMA: return kWolangKeyComma;
    case VK_OEM_PERIOD: return kWolangKeyPeriod;
    case VK_OEM_2: return kWolangKeySlash;
    default: return kWolangKeyUnknown;
  }
}

uint32_t ModifiersNow() {
  uint32_t held = 0;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    held |= kWolangModShift;
  }
  if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
    held |= kWolangModControl;
  }
  if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    held |= kWolangModAlt;
  }
  if (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0) {
    held |= kWolangModSuper;
  }
  return held;
}

std::wstring Wide(const char* text) {
  if (text == nullptr) {
    return L"";
  }
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  if (needed <= 0) {
    return L"";
  }
  std::wstring wide(static_cast<size_t>(needed - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), needed);
  return wide;
}

std::string Narrow(const wchar_t* text) {
  if (text == nullptr) {
    return "";
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                         nullptr, nullptr);
  if (needed <= 0) {
    return "";
  }
  std::string narrow(static_cast<size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), needed,
                      nullptr, nullptr);
  return narrow;
}

constexpr const wchar_t* kClassName = L"WolangWindow";

}  // namespace

struct WolangWindow {
  HWND handle = nullptr;
  HDC device = nullptr;
  HGLRC gl = nullptr;
  Events events;
  std::vector<uint32_t> pixels;
  int32_t pixel_width = 0;
  int32_t pixel_height = 0;
  int32_t width = 0;
  int32_t height = 0;
};

namespace {

// Everything the window procedure needs, found from the handle: a window
// created here keeps a pointer to its own record.
WolangWindow* OwnerOf(HWND handle) {
  return reinterpret_cast<WolangWindow*>(
      GetWindowLongPtrW(handle, GWLP_USERDATA));
}

LRESULT CALLBACK Procedure(HWND handle, UINT message, WPARAM w,
                           LPARAM l) {
  WolangWindow* owner = OwnerOf(handle);
  if (owner == nullptr) {
    return DefWindowProcW(handle, message, w, l);
  }
  switch (message) {
    case WM_CLOSE:
      // Said rather than done: what to do about it is the program's
      // decision.
      owner->events.Push(Blank(kWolangEventClosed));
      return 0;
    case WM_SIZE: {
      owner->width = LOWORD(l);
      owner->height = HIWORD(l);
      WolangEvent made = Blank(kWolangEventResized);
      made.width = owner->width;
      made.height = owner->height;
      owner->events.Push(made);
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
      WolangEvent made =
          Blank(down ? kWolangEventKeyDown : kWolangEventKeyUp);
      made.key = KeyFromVirtual(w);
      made.modifiers = ModifiersNow();
      made.repeat = down && (l & (1 << 30)) != 0 ? 1 : 0;
      owner->events.Push(made);
      return 0;
    }
    case WM_CHAR: {
      if (w < 0x20 || w == 0x7F) {
        return 0;
      }
      WolangEvent made = Blank(kWolangEventText);
      made.code_point = static_cast<uint32_t>(w);
      made.modifiers = ModifiersNow();
      owner->events.Push(made);
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
      const bool down = message == WM_LBUTTONDOWN ||
                        message == WM_RBUTTONDOWN ||
                        message == WM_MBUTTONDOWN;
      WolangEvent made =
          Blank(down ? kWolangEventMouseDown : kWolangEventMouseUp);
      made.key = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)
                     ? kWolangMouseLeft
                 : (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
                     ? kWolangMouseRight
                     : kWolangMouseMiddle;
      made.x = GET_X_LPARAM(l);
      made.y = GET_Y_LPARAM(l);
      made.modifiers = ModifiersNow();
      owner->events.Push(made);
      return 0;
    }
    case WM_MOUSEMOVE: {
      WolangEvent made = Blank(kWolangEventMouseMoved);
      made.x = GET_X_LPARAM(l);
      made.y = GET_Y_LPARAM(l);
      made.modifiers = ModifiersNow();
      owner->events.Push(made);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      WolangEvent made = Blank(kWolangEventScrolled);
      made.scroll_y =
          static_cast<double>(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA;
      made.modifiers = ModifiersNow();
      owner->events.Push(made);
      return 0;
    }
    case WM_SETFOCUS:
      owner->events.Push(Blank(kWolangEventFocus));
      return 0;
    case WM_KILLFOCUS:
      owner->events.Push(Blank(kWolangEventBlur));
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint;
      HDC device = BeginPaint(handle, &paint);
      if (!owner->pixels.empty()) {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = owner->pixel_width;
        // Negative, because these rows go top to bottom and this
        // interface counts from the bottom by default.
        info.bmiHeader.biHeight = -owner->pixel_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(device, 0, 0, owner->width, owner->height, 0, 0,
                      owner->pixel_width, owner->pixel_height,
                      owner->pixels.data(), &info, DIB_RGB_COLORS,
                      SRCCOPY);
      }
      EndPaint(handle, &paint);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(handle, message, w, l);
}

void RegisterOnce() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  WNDCLASSEXW description = {};
  description.cbSize = sizeof(description);
  description.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  description.lpfnWndProc = &Procedure;
  description.hInstance = GetModuleHandleW(nullptr);
  // `IDC_ARROW` is a number pretending to be a string, and the wide
  // call wants it pretending to be a wide one.
  description.hCursor =
      LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  description.lpszClassName = kClassName;
  RegisterClassExW(&description);
}

}  // namespace

extern "C" int32_t wolang_platform_available(void) { return 1; }

extern "C" const char* wolang_platform_backend(void) { return "win32"; }

extern "C" WolangWindow* wolang_window_open(const char* title,
                                            int32_t width, int32_t height,
                                            uint32_t flags) {
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  RegisterOnce();
  WolangWindow* held = new WolangWindow();
  held->width = width;
  held->height = height;

  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                WS_MINIMIZEBOX;
  if ((flags & kWolangWindowResizable) != 0) {
    style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
  }
  // The size asked for is the content's, so the frame is added to it.
  RECT wanted = {0, 0, width, height};
  AdjustWindowRect(&wanted, style, FALSE);

  const std::wstring wide = Wide(title != nullptr ? title : "wolang");
  held->handle = CreateWindowExW(
      0, kClassName, wide.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
      wanted.right - wanted.left, wanted.bottom - wanted.top, nullptr,
      nullptr, GetModuleHandleW(nullptr), nullptr);
  if (held->handle == nullptr) {
    delete held;
    return nullptr;
  }
  SetWindowLongPtrW(held->handle, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(held));
  held->device = GetDC(held->handle);

  if ((flags & kWolangWindowGl) != 0 && held->device != nullptr) {
    PIXELFORMATDESCRIPTOR format = {};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                     PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    const int chosen = ChoosePixelFormat(held->device, &format);
    if (chosen != 0 && SetPixelFormat(held->device, chosen, &format)) {
      held->gl = wglCreateContext(held->device);
    }
  }

  if ((flags & kWolangWindowHidden) == 0) {
    ShowWindow(held->handle, SW_SHOW);
    UpdateWindow(held->handle);
  }
  return held;
}

extern "C" void wolang_window_close(WolangWindow* window) {
  if (window == nullptr) {
    return;
  }
  if (window->gl != nullptr) {
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(window->gl);
  }
  if (window->device != nullptr) {
    ReleaseDC(window->handle, window->device);
  }
  if (window->handle != nullptr) {
    SetWindowLongPtrW(window->handle, GWLP_USERDATA, 0);
    DestroyWindow(window->handle);
  }
  delete window;
}

extern "C" int32_t wolang_window_poll(WolangWindow* window,
                                      WolangEvent* out) {
  if (window == nullptr || out == nullptr) {
    return 0;
  }
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return window->events.Take(out) ? 1 : 0;
}

extern "C" void wolang_window_size(WolangWindow* window, int32_t* width,
                                   int32_t* height) {
  if (window == nullptr) {
    return;
  }
  if (width != nullptr) {
    *width = window->width;
  }
  if (height != nullptr) {
    *height = window->height;
  }
}

extern "C" double wolang_window_scale(WolangWindow* window) {
  if (window == nullptr || window->handle == nullptr) {
    return 1.0;
  }
  const UINT dots = GetDpiForWindow(window->handle);
  return dots == 0 ? 1.0 : static_cast<double>(dots) / 96.0;
}

extern "C" void wolang_window_set_title(WolangWindow* window,
                                        const char* title) {
  if (window == nullptr || title == nullptr) {
    return;
  }
  SetWindowTextW(window->handle, Wide(title).c_str());
}

extern "C" void wolang_window_present(WolangWindow* window,
                                      const uint32_t* pixels,
                                      int32_t width, int32_t height) {
  if (window == nullptr || pixels == nullptr || width <= 0 ||
      height <= 0) {
    return;
  }
  window->pixels.assign(pixels,
                        pixels + static_cast<size_t>(width) * height);
  window->pixel_width = width;
  window->pixel_height = height;
  // Drawn now rather than when the system next asks, for the same reason
  // as the other backends: a frame loop that presents expects to see it.
  InvalidateRect(window->handle, nullptr, FALSE);
  UpdateWindow(window->handle);
}

extern "C" int32_t wolang_window_gl_start(WolangWindow* window) {
  if (window == nullptr || window->gl == nullptr) {
    return 0;
  }
  return wglMakeCurrent(window->device, window->gl) ? 1 : 0;
}

extern "C" void wolang_window_swap(WolangWindow* window,
                                   int32_t interval) {
  if (window == nullptr || window->device == nullptr) {
    return;
  }
  using SwapInterval = BOOL(WINAPI*)(int);
  static SwapInterval set_interval = reinterpret_cast<SwapInterval>(
      wglGetProcAddress("wglSwapIntervalEXT"));
  if (set_interval != nullptr) {
    set_interval(interval != 0 ? 1 : 0);
  }
  SwapBuffers(window->device);
}

extern "C" void* wolang_gl_proc(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  // The newer entry points come from the driver and the older ones from
  // the system's own library, and only asking both finds all of them.
  if (void* found = reinterpret_cast<void*>(wglGetProcAddress(name));
      found != nullptr) {
    return found;
  }
  static HMODULE library = LoadLibraryW(L"opengl32.dll");
  if (library == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<void*>(GetProcAddress(library, name));
}

extern "C" const char* wolang_clipboard_text(void) {
  static std::string held;
  held.clear();
  if (!OpenClipboard(nullptr)) {
    return held.c_str();
  }
  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data != nullptr) {
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (text != nullptr) {
      held = Narrow(text);
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return held.c_str();
}

extern "C" void wolang_clipboard_set(const char* text) {
  if (text == nullptr || !OpenClipboard(nullptr)) {
    return;
  }
  EmptyClipboard();
  const std::wstring wide = Wide(text);
  const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
  HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (block != nullptr) {
    void* into = GlobalLock(block);
    if (into != nullptr) {
      std::memcpy(into, wide.c_str(), bytes);
      GlobalUnlock(block);
      SetClipboardData(CF_UNICODETEXT, block);
    }
  }
  CloseClipboard();
}

// ------------------------------------------------------------- sound ---

struct WolangAudio {
  HWAVEOUT device = nullptr;
  Ring ring;
  std::thread worker;
  std::atomic<bool> running{false};
  int32_t channels = 2;
  // Four blocks in flight: enough that one being written does not leave
  // the device with nothing to play.
  static constexpr int kBlocks = 4;
  static constexpr int kFramesPerBlock = 512;
  WAVEHDR headers[kBlocks] = {};
  std::vector<float> blocks[kBlocks];
};

extern "C" WolangAudio* wolang_audio_open(int32_t rate,
                                          int32_t channels) {
  if (rate <= 0) {
    rate = 48000;
  }
  if (channels != 1 && channels != 2) {
    channels = 2;
  }
  WolangAudio* audio = new WolangAudio();
  audio->channels = channels;
  audio->ring.Open(rate / 2, channels);

  WAVEFORMATEX format = {};
  format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  format.nChannels = static_cast<WORD>(channels);
  format.nSamplesPerSec = static_cast<DWORD>(rate);
  format.wBitsPerSample = 32;
  format.nBlockAlign = static_cast<WORD>(channels * 4);
  format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
  if (waveOutOpen(&audio->device, WAVE_MAPPER, &format, 0, 0,
                  CALLBACK_NULL) != MMSYSERR_NOERROR) {
    delete audio;
    return nullptr;
  }

  for (int i = 0; i < WolangAudio::kBlocks; ++i) {
    audio->blocks[i].assign(
        static_cast<size_t>(WolangAudio::kFramesPerBlock) * channels,
        0.0f);
    audio->headers[i].lpData =
        reinterpret_cast<LPSTR>(audio->blocks[i].data());
    audio->headers[i].dwBufferLength = static_cast<DWORD>(
        audio->blocks[i].size() * sizeof(float));
    waveOutPrepareHeader(audio->device, &audio->headers[i],
                         sizeof(WAVEHDR));
    audio->headers[i].dwFlags |= WHDR_DONE;
  }

  audio->running.store(true);
  audio->worker = std::thread([audio]() {
    while (audio->running.load()) {
      bool idle = true;
      for (int i = 0; i < WolangAudio::kBlocks; ++i) {
        if ((audio->headers[i].dwFlags & WHDR_DONE) == 0) {
          continue;
        }
        idle = false;
        audio->ring.Read(audio->blocks[i].data(),
                         WolangAudio::kFramesPerBlock);
        audio->headers[i].dwFlags &= ~WHDR_DONE;
        waveOutWrite(audio->device, &audio->headers[i],
                     sizeof(WAVEHDR));
      }
      if (idle) {
        Sleep(2);
      }
    }
  });
  return audio;
}

extern "C" void wolang_audio_close(WolangAudio* audio) {
  if (audio == nullptr) {
    return;
  }
  audio->running.store(false);
  if (audio->worker.joinable()) {
    audio->worker.join();
  }
  if (audio->device != nullptr) {
    waveOutReset(audio->device);
    for (int i = 0; i < WolangAudio::kBlocks; ++i) {
      waveOutUnprepareHeader(audio->device, &audio->headers[i],
                             sizeof(WAVEHDR));
    }
    waveOutClose(audio->device);
  }
  delete audio;
}

extern "C" int32_t wolang_audio_queue(WolangAudio* audio,
                                      const float* samples,
                                      int32_t frames) {
  if (audio == nullptr || samples == nullptr || frames <= 0) {
    return 0;
  }
  return audio->ring.Write(samples, frames);
}

extern "C" int32_t wolang_audio_queued(WolangAudio* audio) {
  return audio == nullptr ? 0 : audio->ring.Queued();
}

extern "C" int32_t wolang_audio_room(WolangAudio* audio) {
  return audio == nullptr ? 0 : audio->ring.Room();
}
