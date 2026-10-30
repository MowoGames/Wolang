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
#include <shellapi.h>
#include <commdlg.h>
#include <windowsx.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "platform/common.h"
#include "platform/gamepad_decode.h"

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
  // (D247)
  int32_t shape = 0;
  bool cursor_visible = true;
  bool locked = false;
  int32_t last_x = -1;
  int32_t last_y = -1;
  bool fullscreen = false;
  DWORD saved_style = 0;
  WINDOWPLACEMENT saved_placement = {};
  int32_t min_width = 0;
  int32_t min_height = 0;
  std::vector<std::string> drops;
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
      // While locked the raw input below is the truth: the pointer is
      // pinned and this position never changes (D247).
      if (owner->locked) {
        return 0;
      }
      WolangEvent made = Blank(kWolangEventMouseMoved);
      made.x = GET_X_LPARAM(l);
      made.y = GET_Y_LPARAM(l);
      if (owner->last_x >= 0) {
        made.scroll_x = made.x - owner->last_x;
        made.scroll_y = made.y - owner->last_y;
      }
      owner->last_x = made.x;
      owner->last_y = made.y;
      made.modifiers = ModifiersNow();
      owner->events.Push(made);
      return 0;
    }
    case WM_INPUT: {
      if (!owner->locked) {
        break;
      }
      RAWINPUT raw;
      UINT size = sizeof(raw);
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &raw,
                          &size, sizeof(RAWINPUTHEADER)) ==
              static_cast<UINT>(-1) ||
          raw.header.dwType != RIM_TYPEMOUSE) {
        break;
      }
      if ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0 ||
          (raw.data.mouse.lLastX == 0 && raw.data.mouse.lLastY == 0)) {
        break;
      }
      WolangEvent made = Blank(kWolangEventMouseMoved);
      made.scroll_x = raw.data.mouse.lLastX;
      made.scroll_y = raw.data.mouse.lLastY;
      made.modifiers = ModifiersNow();
      owner->events.Push(made);
      break;
    }
    case WM_SETCURSOR: {
      if (LOWORD(l) != HTCLIENT) {
        break;
      }
      // The wide forms by number: the `IDC_*` macros follow `UNICODE`,
      // which this file does not define, and would be narrow (D247).
      static const LPCWSTR kShapes[] = {
          MAKEINTRESOURCEW(32512), MAKEINTRESOURCEW(32649),  // arrow, hand
          MAKEINTRESOURCEW(32513), MAKEINTRESOURCEW(32515),  // ibeam, cross
          MAKEINTRESOURCEW(32644), MAKEINTRESOURCEW(32645),  // size we, ns
          MAKEINTRESOURCEW(32646)};                          // size all
      SetCursor(owner->cursor_visible && !owner->locked
                    ? LoadCursorW(nullptr, kShapes[owner->shape])
                    : nullptr);
      return TRUE;
    }
    case WM_GETMINMAXINFO: {
      if (owner->min_width <= 0 && owner->min_height <= 0) {
        break;
      }
      RECT frame = {0, 0, owner->min_width, owner->min_height};
      AdjustWindowRect(&frame,
                       static_cast<DWORD>(GetWindowLongPtrW(handle,
                                                            GWL_STYLE)),
                       FALSE);
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(l);
      info->ptMinTrackSize.x = frame.right - frame.left;
      info->ptMinTrackSize.y = frame.bottom - frame.top;
      return 0;
    }
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(w);
      const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      owner->drops.clear();
      for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        path.resize(length);
        owner->drops.push_back(Narrow(path.c_str()));
      }
      DragFinish(drop);
      WolangEvent made = Blank(kWolangEventDropped);
      made.key = static_cast<int32_t>(owner->drops.size());
      owner->events.Push(made);
      return 0;
    }
    case WM_DPICHANGED: {
      WolangEvent made = Blank(kWolangEventScaleChanged);
      made.scroll_x = static_cast<double>(LOWORD(w)) / 96.0;
      owner->events.Push(made);
      // The rectangle Windows suggests keeps the window the same size
      // to the eye on the new display.
      const RECT* suggested = reinterpret_cast<const RECT*>(l);
      if (!owner->fullscreen) {
        SetWindowPos(handle, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
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
  DragAcceptFiles(held->handle, TRUE);
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

// ------------------------------------------------- cursor and window ---
//
// (D247)

extern "C" void wolang_window_cursor_visible(WolangWindow* window,
                                             int32_t visible) {
  if (window == nullptr) {
    return;
  }
  window->cursor_visible = visible != 0;
  SendMessageW(window->handle, WM_SETCURSOR,
               reinterpret_cast<WPARAM>(window->handle), HTCLIENT);
}

extern "C" void wolang_window_cursor_shape(WolangWindow* window,
                                           int32_t shape) {
  if (window == nullptr || shape < 0 || shape > 6) {
    return;
  }
  window->shape = shape;
  SendMessageW(window->handle, WM_SETCURSOR,
               reinterpret_cast<WPARAM>(window->handle), HTCLIENT);
}

// Raw input gives movement that does not stop at the edge of the
// screen, and the clip keeps the pointer inside the window. Clipped to
// one point at the centre so that it cannot drift off a multi-monitor
// edge.
extern "C" int32_t wolang_window_cursor_lock(WolangWindow* window,
                                             int32_t locked) {
  if (window == nullptr) {
    return 0;
  }
  if (locked == 0) {
    if (window->locked) {
      window->locked = false;
      ClipCursor(nullptr);
      RAWINPUTDEVICE device = {0x01, 0x02, RIDEV_REMOVE, nullptr};
      RegisterRawInputDevices(&device, 1, sizeof(device));
      window->last_x = -1;
    }
    return 1;
  }
  if (window->locked) {
    return 1;
  }
  RAWINPUTDEVICE device = {0x01, 0x02, 0, window->handle};
  if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
    return 0;
  }
  RECT client;
  GetClientRect(window->handle, &client);
  POINT centre = {(client.left + client.right) / 2,
                  (client.top + client.bottom) / 2};
  ClientToScreen(window->handle, &centre);
  RECT point = {centre.x, centre.y, centre.x + 1, centre.y + 1};
  ClipCursor(&point);
  window->locked = true;
  return 1;
}

extern "C" void wolang_window_set_fullscreen(WolangWindow* window,
                                             int32_t fullscreen) {
  if (window == nullptr || (fullscreen != 0) == window->fullscreen) {
    return;
  }
  if (fullscreen != 0) {
    window->saved_style =
        static_cast<DWORD>(GetWindowLongPtrW(window->handle, GWL_STYLE));
    window->saved_placement.length = sizeof(window->saved_placement);
    GetWindowPlacement(window->handle, &window->saved_placement);
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(window->handle,
                                      MONITOR_DEFAULTTONEAREST),
                    &monitor);
    SetWindowLongPtrW(window->handle, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(window->handle, HWND_TOP, monitor.rcMonitor.left,
                 monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  } else {
    SetWindowLongPtrW(window->handle, GWL_STYLE, window->saved_style);
    SetWindowPlacement(window->handle, &window->saved_placement);
    SetWindowPos(window->handle, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
  }
  window->fullscreen = fullscreen != 0;
}

extern "C" int32_t wolang_window_is_fullscreen(WolangWindow* window) {
  return window != nullptr && window->fullscreen ? 1 : 0;
}

extern "C" void wolang_window_set_size(WolangWindow* window, int32_t width,
                                       int32_t height) {
  if (window == nullptr || width <= 0 || height <= 0) {
    return;
  }
  RECT wanted = {0, 0, width, height};
  AdjustWindowRect(&wanted,
                   static_cast<DWORD>(
                       GetWindowLongPtrW(window->handle, GWL_STYLE)),
                   FALSE);
  SetWindowPos(window->handle, nullptr, 0, 0, wanted.right - wanted.left,
               wanted.bottom - wanted.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

extern "C" void wolang_window_set_min_size(WolangWindow* window,
                                           int32_t width, int32_t height) {
  if (window == nullptr) {
    return;
  }
  window->min_width = width > 0 ? width : 0;
  window->min_height = height > 0 ? height : 0;
}

extern "C" void wolang_window_set_position(WolangWindow* window, int32_t x,
                                           int32_t y) {
  if (window == nullptr) {
    return;
  }
  SetWindowPos(window->handle, nullptr, x, y, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

extern "C" void wolang_window_position(WolangWindow* window, int32_t* x,
                                       int32_t* y) {
  RECT frame = {};
  if (window != nullptr) {
    GetWindowRect(window->handle, &frame);
  }
  if (x != nullptr) {
    *x = frame.left;
  }
  if (y != nullptr) {
    *y = frame.top;
  }
}

extern "C" const char* wolang_window_drop_path(WolangWindow* window,
                                               int32_t index) {
  if (window == nullptr || index < 0 ||
      static_cast<size_t>(index) >= window->drops.size()) {
    return nullptr;
  }
  return window->drops[static_cast<size_t>(index)].c_str();
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

// ---------------------------------------------------------- gamepads ---
//
// XInput, loaded at run time: the DLL is not on every install (N
// editions of Windows) and a link-time dependency would stop the whole
// library loading there (D247). Anything that is not an XInput pad does
// not show up, which is the cost of the smaller of the two APIs.

namespace {

struct XInputGamepad {
  WORD buttons;
  BYTE left_trigger;
  BYTE right_trigger;
  SHORT lx, ly, rx, ry;
};
struct XInputState {
  DWORD packet;
  XInputGamepad gamepad;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XInputState*);

struct Pad {
  bool connected = false;
  wolang::platform::PadState state;
};

struct Pads {
  Pad pads[wolang::platform::kMaxGamepads];
  std::vector<WolangEvent> pending;
  double next_probe = 0.0;
  XInputGetStateFn get_state = nullptr;
  bool tried = false;
};

Pads& HeldPads() {
  static Pads pads;
  return pads;
}

void ButtonsFromXInput(wolang::platform::PadState* state, WORD bits) {
  using namespace wolang::platform;
  struct Map {
    WORD mask;
    int32_t button;
  };
  static const Map kMap[] = {
      {0x1000, kPadA},          {0x2000, kPadB},
      {0x4000, kPadX},          {0x8000, kPadY},
      {0x0100, kPadLeftBumper}, {0x0200, kPadRightBumper},
      {0x0020, kPadBack},       {0x0010, kPadStart},
      {0x0040, kPadLeftStick},  {0x0080, kPadRightStick},
      {0x0001, kPadDpadUp},     {0x0002, kPadDpadDown},
      {0x0004, kPadDpadLeft},   {0x0008, kPadDpadRight},
  };
  for (const Map& m : kMap) {
    SetButton(state, m.button, (bits & m.mask) != 0);
  }
}

}  // namespace

extern "C" int32_t wolang_gamepad_poll(WolangEvent* out) {
  using namespace wolang::platform;
  Pads& held = HeldPads();
  if (!held.tried) {
    held.tried = true;
    HMODULE library = LoadLibraryW(L"xinput1_4.dll");
    if (library == nullptr) {
      library = LoadLibraryW(L"xinput9_1_0.dll");
    }
    if (library != nullptr) {
      held.get_state = reinterpret_cast<XInputGetStateFn>(
          GetProcAddress(library, "XInputGetState"));
    }
  }
  if (held.get_state != nullptr && held.pending.empty()) {
    // Asking after a pad that is not there is slow, so absent slots are
    // only probed once a second.
    const double now = Monotonic();
    const bool probe = now >= held.next_probe;
    if (probe) {
      held.next_probe = now + 1.0;
    }
    for (int32_t i = 0; i < kMaxGamepads; ++i) {
      Pad& pad = held.pads[i];
      if (!pad.connected && !probe) {
        continue;
      }
      XInputState raw = {};
      const bool present = held.get_state(static_cast<DWORD>(i), &raw) == 0;
      if (present != pad.connected) {
        pad.connected = present;
        pad.state = PadState();
        WolangEvent made = Blank(present ? kWolangEventGamepadConnected
                                         : kWolangEventGamepadDisconnected);
        made.key = i;
        held.pending.push_back(made);
      }
      if (!present) {
        continue;
      }
      const XInputGamepad& g = raw.gamepad;
      ButtonsFromXInput(&pad.state, g.buttons);
      pad.state.axes[kPadLeftX] = NormalizeAxis(g.lx, -32768, 32767, false);
      // XInput counts up as positive; the layout counts it negative.
      pad.state.axes[kPadLeftY] = -NormalizeAxis(g.ly, -32768, 32767, false);
      pad.state.axes[kPadRightX] = NormalizeAxis(g.rx, -32768, 32767, false);
      pad.state.axes[kPadRightY] =
          -NormalizeAxis(g.ry, -32768, 32767, false);
      pad.state.axes[kPadLeftTrigger] = NormalizeAxis(g.left_trigger, 0, 255, true);
      pad.state.axes[kPadRightTrigger] =
          NormalizeAxis(g.right_trigger, 0, 255, true);
    }
  }
  if (held.pending.empty() || out == nullptr) {
    return 0;
  }
  *out = held.pending.front();
  held.pending.erase(held.pending.begin());
  return 1;
}

extern "C" int32_t wolang_gamepad_connected(int32_t index) {
  return index >= 0 && index < wolang::platform::kMaxGamepads &&
                 HeldPads().pads[index].connected
             ? 1
             : 0;
}

extern "C" const char* wolang_gamepad_name(int32_t index) {
  return wolang_gamepad_connected(index) != 0 ? "XInput controller" : "";
}

extern "C" int32_t wolang_gamepad_button(int32_t index, int32_t button) {
  if (wolang_gamepad_connected(index) == 0 || button < 0 ||
      button >= wolang::platform::kPadButtonCount) {
    return 0;
  }
  return (HeldPads().pads[index].state.buttons >> button) & 1u;
}

extern "C" double wolang_gamepad_axis(int32_t index, int32_t axis) {
  if (wolang_gamepad_connected(index) == 0 || axis < 0 ||
      axis >= wolang::platform::kGamepadAxes) {
    return 0.0;
  }
  return HeldPads().pads[index].state.axes[axis];
}

// ----------------------------------------------------------- dialogs ---
//
// (D247) The system's own, which are always there.

namespace {

std::string& HeldPath() {
  static std::string held;
  return held;
}

const char* PickFile(const char* title, const char* start_name, bool save) {
  wchar_t buffer[MAX_PATH * 4] = {};
  if (start_name != nullptr) {
    const std::wstring name = Wide(start_name);
    wcsncpy_s(buffer, name.c_str(), _TRUNCATE);
  }
  const std::wstring heading = Wide(title != nullptr ? title : "");
  OPENFILENAMEW request = {};
  request.lStructSize = sizeof(request);
  request.lpstrFile = buffer;
  request.nMaxFile = MAX_PATH * 4;
  request.lpstrTitle = heading.c_str();
  request.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER;
  request.Flags |= save ? OFN_OVERWRITEPROMPT
                        : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
  const BOOL chosen =
      save ? GetSaveFileNameW(&request) : GetOpenFileNameW(&request);
  if (!chosen) {
    return nullptr;
  }
  HeldPath() = Narrow(buffer);
  return HeldPath().c_str();
}

}  // namespace

extern "C" int32_t wolang_dialog_available(void) { return 1; }

extern "C" int32_t wolang_dialog_message(const char* title, const char* text,
                                         int32_t level, int32_t buttons) {
  UINT flags = level == kWolangDialogError     ? MB_ICONERROR
               : level == kWolangDialogWarning ? MB_ICONWARNING
                                               : MB_ICONINFORMATION;
  flags |= buttons == kWolangButtonsYesNo      ? MB_YESNO
           : buttons == kWolangButtonsOkCancel ? MB_OKCANCEL
                                               : MB_OK;
  const int answer = MessageBoxW(nullptr, Wide(text != nullptr ? text : "").c_str(),
                                 Wide(title != nullptr ? title : "").c_str(),
                                 flags);
  switch (answer) {
    case IDOK: return kWolangAnswerOk;
    case IDCANCEL: return kWolangAnswerCancel;
    case IDYES: return kWolangAnswerYes;
    case IDNO: return kWolangAnswerNo;
    default: return kWolangAnswerUnavailable;
  }
}

extern "C" const char* wolang_dialog_open_file(const char* title) {
  return PickFile(title, nullptr, false);
}

extern "C" const char* wolang_dialog_save_file(const char* title,
                                               const char* start_name) {
  return PickFile(title, start_name, true);
}
