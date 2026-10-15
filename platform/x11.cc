// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The Linux backend: Xlib for the window and the events, an `XImage` for
// a buffer of pixels, GLX for the GPU and ALSA for sound.
//
// Xlib is linked, because its events are a union that cannot be declared
// from the outside without copying its headers. GLX and ALSA are opened
// at run time and their entry points looked up by name: a machine
// without them still builds and still runs everything else, and a build
// needs neither of their headers (D155).

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <sys/time.h>

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

// ------------------------------------------------------------- GLX ---
//
// Declared here rather than included: these four calls are all a window
// needs, and copying four prototypes is cheaper than a build dependency
// on the GL headers.
using GLXContext = struct __GLXcontextRec*;
using ChooseVisual = XVisualInfo* (*)(Display*, int, int*);
using CreateContext = GLXContext (*)(Display*, XVisualInfo*, GLXContext,
                                     Bool);
using MakeCurrent = Bool (*)(Display*, Window, GLXContext);
using SwapBuffers = void (*)(Display*, Window);
using GetProc = void* (*)(const unsigned char*);
using DestroyContext = void (*)(Display*, GLXContext);
using SwapInterval = void (*)(Display*, Window, int);

struct Gl {
  void* library = nullptr;
  ChooseVisual choose = nullptr;
  CreateContext create = nullptr;
  MakeCurrent current = nullptr;
  SwapBuffers swap = nullptr;
  GetProc proc = nullptr;
  DestroyContext destroy = nullptr;
  SwapInterval interval = nullptr;
};

Gl& OpenGl() {
  static Gl gl;
  static bool tried = false;
  if (tried) {
    return gl;
  }
  tried = true;
  gl.library = ::dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (gl.library == nullptr) {
    gl.library = ::dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
  }
  if (gl.library == nullptr) {
    return gl;
  }
  const auto find = [&](const char* name) {
    return ::dlsym(gl.library, name);
  };
  gl.choose = reinterpret_cast<ChooseVisual>(find("glXChooseVisual"));
  gl.create = reinterpret_cast<CreateContext>(find("glXCreateContext"));
  gl.current = reinterpret_cast<MakeCurrent>(find("glXMakeCurrent"));
  gl.swap = reinterpret_cast<SwapBuffers>(find("glXSwapBuffers"));
  gl.proc = reinterpret_cast<GetProc>(find("glXGetProcAddressARB"));
  if (gl.proc == nullptr) {
    gl.proc = reinterpret_cast<GetProc>(find("glXGetProcAddress"));
  }
  gl.destroy =
      reinterpret_cast<DestroyContext>(find("glXDestroyContext"));
  gl.interval =
      reinterpret_cast<SwapInterval>(find("glXSwapIntervalEXT"));
  return gl;
}

// -------------------------------------------------------------- keys ---

int32_t KeyFromSym(KeySym sym) {
  if (sym >= XK_a && sym <= XK_z) {
    return kWolangKeyA + static_cast<int32_t>(sym - XK_a);
  }
  if (sym >= XK_A && sym <= XK_Z) {
    return kWolangKeyA + static_cast<int32_t>(sym - XK_A);
  }
  if (sym >= XK_0 && sym <= XK_9) {
    return kWolangKey0 + static_cast<int32_t>(sym - XK_0);
  }
  switch (sym) {
    case XK_space: return kWolangKeySpace;
    case XK_Return: case XK_KP_Enter: return kWolangKeyEnter;
    case XK_Escape: return kWolangKeyEscape;
    case XK_Tab: return kWolangKeyTab;
    case XK_BackSpace: return kWolangKeyBackspace;
    case XK_Delete: return kWolangKeyDelete;
    case XK_Left: return kWolangKeyLeft;
    case XK_Right: return kWolangKeyRight;
    case XK_Up: return kWolangKeyUp;
    case XK_Down: return kWolangKeyDown;
    case XK_Shift_L: case XK_Shift_R: return kWolangKeyShift;
    case XK_Control_L: case XK_Control_R: return kWolangKeyControl;
    case XK_Alt_L: case XK_Alt_R: return kWolangKeyAlt;
    case XK_Super_L: case XK_Super_R: return kWolangKeySuper;
    case XK_F1: return kWolangKeyF1;
    case XK_F2: return kWolangKeyF2;
    case XK_F3: return kWolangKeyF3;
    case XK_F4: return kWolangKeyF4;
    case XK_F5: return kWolangKeyF5;
    case XK_F6: return kWolangKeyF6;
    case XK_F7: return kWolangKeyF7;
    case XK_F8: return kWolangKeyF8;
    case XK_F9: return kWolangKeyF9;
    case XK_F10: return kWolangKeyF10;
    case XK_F11: return kWolangKeyF11;
    case XK_F12: return kWolangKeyF12;
    case XK_minus: return kWolangKeyMinus;
    case XK_equal: return kWolangKeyEqual;
    case XK_comma: return kWolangKeyComma;
    case XK_period: return kWolangKeyPeriod;
    case XK_slash: return kWolangKeySlash;
    default: return kWolangKeyUnknown;
  }
}

uint32_t ModifiersOf(unsigned int state) {
  uint32_t held = 0;
  if ((state & ShiftMask) != 0) {
    held |= kWolangModShift;
  }
  if ((state & ControlMask) != 0) {
    held |= kWolangModControl;
  }
  if ((state & Mod1Mask) != 0) {
    held |= kWolangModAlt;
  }
  if ((state & Mod4Mask) != 0) {
    held |= kWolangModSuper;
  }
  return held;
}

std::string& ClipboardHeld() {
  static std::string held;
  return held;
}

}  // namespace

// ------------------------------------------------------------ window ---

struct WolangWindow {
  Display* display = nullptr;
  Window window = 0;
  GC context = nullptr;
  Atom delete_message = 0;
  XVisualInfo* visual = nullptr;
  GLXContext gl = nullptr;
  Events events;
  int32_t width = 0;
  int32_t height = 0;
  // The pixels as the server wants them, which is this window's size
  // rather than whatever the program drew at.
  std::vector<uint32_t> scaled;
};

extern "C" int32_t wolang_platform_available(void) {
  // A display that opens is the only honest test: a build with X11 in it
  // on a machine with no server running has to answer no.
  static int32_t answer = -1;
  if (answer < 0) {
    Display* display = XOpenDisplay(nullptr);
    answer = display != nullptr ? 1 : 0;
    if (display != nullptr) {
      XCloseDisplay(display);
    }
  }
  return answer;
}

extern "C" const char* wolang_platform_backend(void) {
  return wolang_platform_available() != 0 ? "x11" : "none";
}

extern "C" WolangWindow* wolang_window_open(const char* title,
                                            int32_t width, int32_t height,
                                            uint32_t flags) {
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    return nullptr;
  }
  WolangWindow* held = new WolangWindow();
  held->display = display;
  held->width = width;
  held->height = height;

  const int screen = DefaultScreen(display);
  Visual* visual = DefaultVisual(display, screen);
  int depth = DefaultDepth(display, screen);
  Window root = RootWindow(display, screen);

  if ((flags & kWolangWindowGl) != 0) {
    Gl& gl = OpenGl();
    if (gl.choose != nullptr) {
      int attributes[] = {4 /*GLX_RGBA*/,
                          8 /*GLX_RED_SIZE*/, 8,
                          9 /*GLX_GREEN_SIZE*/, 8,
                          10 /*GLX_BLUE_SIZE*/, 8,
                          12 /*GLX_DEPTH_SIZE*/, 24,
                          5 /*GLX_DOUBLEBUFFER*/,
                          0};
      held->visual = gl.choose(display, screen, attributes);
      if (held->visual != nullptr) {
        visual = held->visual->visual;
        depth = held->visual->depth;
      }
    }
  }

  XSetWindowAttributes set = {};
  set.background_pixel = BlackPixel(display, screen);
  set.border_pixel = BlackPixel(display, screen);
  set.colormap = XCreateColormap(display, root, visual, AllocNone);
  set.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                   ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | StructureNotifyMask |
                   FocusChangeMask;
  held->window = XCreateWindow(
      display, root, 0, 0, static_cast<unsigned>(width),
      static_cast<unsigned>(height), 0, depth, InputOutput, visual,
      CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &set);
  if (held->window == 0) {
    XCloseDisplay(display);
    delete held;
    return nullptr;
  }
  XStoreName(display, held->window, title != nullptr ? title : "wolang");

  // The window manager's close button sends a message rather than
  // destroying the window, so that a program decides what to do about
  // it.
  held->delete_message = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, held->window, &held->delete_message, 1);

  if ((flags & kWolangWindowResizable) == 0) {
    XSizeHints* hints = XAllocSizeHints();
    if (hints != nullptr) {
      hints->flags = PMinSize | PMaxSize;
      hints->min_width = width;
      hints->max_width = width;
      hints->min_height = height;
      hints->max_height = height;
      XSetWMNormalHints(display, held->window, hints);
      XFree(hints);
    }
  }

  held->context = XCreateGC(display, held->window, 0, nullptr);
  if ((flags & kWolangWindowHidden) == 0) {
    XMapWindow(display, held->window);
  }

  if ((flags & kWolangWindowGl) != 0 && held->visual != nullptr) {
    Gl& gl = OpenGl();
    if (gl.create != nullptr) {
      held->gl = gl.create(display, held->visual, nullptr, True);
    }
  }
  XFlush(display);
  return held;
}

extern "C" void wolang_window_close(WolangWindow* window) {
  if (window == nullptr) {
    return;
  }
  if (window->gl != nullptr) {
    Gl& gl = OpenGl();
    if (gl.destroy != nullptr) {
      gl.destroy(window->display, window->gl);
    }
  }
  if (window->visual != nullptr) {
    XFree(window->visual);
  }
  if (window->context != nullptr) {
    XFreeGC(window->display, window->context);
  }
  if (window->window != 0) {
    XDestroyWindow(window->display, window->window);
  }
  XCloseDisplay(window->display);
  delete window;
}

extern "C" int32_t wolang_window_poll(WolangWindow* window,
                                      WolangEvent* out) {
  if (window == nullptr || out == nullptr) {
    return 0;
  }
  while (XPending(window->display) > 0) {
    XEvent event;
    XNextEvent(window->display, &event);
    switch (event.type) {
      case ClientMessage:
        if (static_cast<Atom>(event.xclient.data.l[0]) ==
            window->delete_message) {
          window->events.Push(Blank(kWolangEventClosed));
        }
        break;
      case ConfigureNotify: {
        if (event.xconfigure.width == window->width &&
            event.xconfigure.height == window->height) {
          break;
        }
        window->width = event.xconfigure.width;
        window->height = event.xconfigure.height;
        WolangEvent made = Blank(kWolangEventResized);
        made.width = window->width;
        made.height = window->height;
        window->events.Push(made);
        break;
      }
      case KeyPress:
      case KeyRelease: {
        const bool down = event.type == KeyPress;
        KeySym sym = XkbKeycodeToKeysym(
            window->display, event.xkey.keycode, 0,
            (event.xkey.state & ShiftMask) != 0 ? 1 : 0);
        WolangEvent made = Blank(down ? kWolangEventKeyDown
                                      : kWolangEventKeyUp);
        made.key = KeyFromSym(sym);
        made.modifiers = ModifiersOf(event.xkey.state);
        window->events.Push(made);
        if (down) {
          char typed[16] = {};
          KeySym ignored = 0;
          const int written = XLookupString(&event.xkey, typed,
                                            sizeof(typed), &ignored,
                                            nullptr);
          for (int i = 0; i < written; ++i) {
            const unsigned char byte =
                static_cast<unsigned char>(typed[i]);
            if (byte < 0x20 || byte == 0x7F) {
              continue;
            }
            WolangEvent text = Blank(kWolangEventText);
            text.code_point = byte;
            text.modifiers = made.modifiers;
            window->events.Push(text);
          }
        }
        break;
      }
      case ButtonPress:
      case ButtonRelease: {
        const bool down = event.type == ButtonPress;
        // Four and five are the wheel on this system, not buttons.
        if (event.xbutton.button == 4 || event.xbutton.button == 5) {
          if (!down) {
            break;
          }
          WolangEvent made = Blank(kWolangEventScrolled);
          made.scroll_y = event.xbutton.button == 4 ? 1.0 : -1.0;
          made.x = event.xbutton.x;
          made.y = event.xbutton.y;
          made.modifiers = ModifiersOf(event.xbutton.state);
          window->events.Push(made);
          break;
        }
        WolangEvent made = Blank(down ? kWolangEventMouseDown
                                      : kWolangEventMouseUp);
        made.key = event.xbutton.button == Button1   ? kWolangMouseLeft
                   : event.xbutton.button == Button3 ? kWolangMouseRight
                                                     : kWolangMouseMiddle;
        made.x = event.xbutton.x;
        made.y = event.xbutton.y;
        made.modifiers = ModifiersOf(event.xbutton.state);
        window->events.Push(made);
        break;
      }
      case MotionNotify: {
        WolangEvent made = Blank(kWolangEventMouseMoved);
        made.x = event.xmotion.x;
        made.y = event.xmotion.y;
        made.modifiers = ModifiersOf(event.xmotion.state);
        window->events.Push(made);
        break;
      }
      case FocusIn:
        window->events.Push(Blank(kWolangEventFocus));
        break;
      case FocusOut:
        window->events.Push(Blank(kWolangEventBlur));
        break;
      default:
        break;
    }
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
  (void)window;
  // X says nothing reliable about this, and a number this backend made
  // up would be worse than one.
  return 1.0;
}

extern "C" void wolang_window_set_title(WolangWindow* window,
                                        const char* title) {
  if (window == nullptr || title == nullptr) {
    return;
  }
  XStoreName(window->display, window->window, title);
  XFlush(window->display);
}

extern "C" void wolang_window_present(WolangWindow* window,
                                      const uint32_t* pixels,
                                      int32_t width, int32_t height) {
  if (window == nullptr || pixels == nullptr || width <= 0 ||
      height <= 0) {
    return;
  }
  // `XPutImage` does not scale, so a buffer that is not the window's
  // size is stretched here -- nearest neighbour, which is what a pixel
  // buffer wants and costs one multiply a pixel.
  const int32_t target_width = window->width;
  const int32_t target_height = window->height;
  window->scaled.resize(static_cast<size_t>(target_width) *
                        target_height);
  for (int32_t y = 0; y < target_height; ++y) {
    const int32_t source_y = height == target_height
                                 ? y
                                 : y * height / target_height;
    const uint32_t* row = pixels + static_cast<size_t>(source_y) * width;
    uint32_t* into =
        window->scaled.data() + static_cast<size_t>(y) * target_width;
    if (width == target_width) {
      std::memcpy(into, row, static_cast<size_t>(width) * 4);
      continue;
    }
    for (int32_t x = 0; x < target_width; ++x) {
      into[x] = row[x * width / target_width];
    }
  }

  const int screen = DefaultScreen(window->display);
  XImage* image = XCreateImage(
      window->display, DefaultVisual(window->display, screen), 24,
      ZPixmap, 0, reinterpret_cast<char*>(window->scaled.data()),
      static_cast<unsigned>(target_width),
      static_cast<unsigned>(target_height), 32, 0);
  if (image == nullptr) {
    return;
  }
  XPutImage(window->display, window->window, window->context, image, 0, 0,
            0, 0, static_cast<unsigned>(target_width),
            static_cast<unsigned>(target_height));
  // The pixels belong to this window, so the image must not free them.
  image->data = nullptr;
  XDestroyImage(image);
  XFlush(window->display);
}

extern "C" int32_t wolang_window_gl_start(WolangWindow* window) {
  if (window == nullptr || window->gl == nullptr) {
    return 0;
  }
  Gl& gl = OpenGl();
  if (gl.current == nullptr) {
    return 0;
  }
  return gl.current(window->display, window->window, window->gl) != 0 ? 1
                                                                     : 0;
}

extern "C" void wolang_window_swap(WolangWindow* window,
                                   int32_t interval) {
  if (window == nullptr || window->gl == nullptr) {
    return;
  }
  Gl& gl = OpenGl();
  if (gl.interval != nullptr) {
    gl.interval(window->display, window->window, interval != 0 ? 1 : 0);
  }
  if (gl.swap != nullptr) {
    gl.swap(window->display, window->window);
  }
}

extern "C" void* wolang_gl_proc(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  Gl& gl = OpenGl();
  if (gl.proc != nullptr) {
    if (void* found = gl.proc(
            reinterpret_cast<const unsigned char*>(name));
        found != nullptr) {
      return found;
    }
  }
  if (gl.library != nullptr) {
    return ::dlsym(gl.library, name);
  }
  return nullptr;
}

extern "C" const char* wolang_clipboard_text(void) {
  // What this needs is a selection owner to answer, which means a round
  // trip and a window to answer into. Until that is written, what this
  // process last put there is the honest answer.
  return ClipboardHeld().c_str();
}

extern "C" void wolang_clipboard_set(const char* text) {
  if (text != nullptr) {
    ClipboardHeld() = text;
  }
}

// ------------------------------------------------------------- sound ---
//
// ALSA, opened at run time: six calls, declared here so that a build
// needs none of its headers.

namespace {

using PcmOpen = int (*)(void**, const char*, int, int);
using PcmSetParams = int (*)(void*, int, int, unsigned, unsigned, int,
                             unsigned);
using PcmWrite = long (*)(void*, const void*, unsigned long);
using PcmRecover = int (*)(void*, int, int);
using PcmClose = int (*)(void*);
using PcmDrain = int (*)(void*);

struct Alsa {
  void* library = nullptr;
  PcmOpen open = nullptr;
  PcmSetParams set = nullptr;
  PcmWrite write = nullptr;
  PcmRecover recover = nullptr;
  PcmClose close = nullptr;
  PcmDrain drain = nullptr;
};

Alsa& OpenAlsa() {
  static Alsa alsa;
  static bool tried = false;
  if (tried) {
    return alsa;
  }
  tried = true;
  alsa.library = ::dlopen("libasound.so.2", RTLD_NOW);
  if (alsa.library == nullptr) {
    alsa.library = ::dlopen("libasound.so", RTLD_NOW);
  }
  if (alsa.library == nullptr) {
    return alsa;
  }
  const auto find = [&](const char* name) {
    return ::dlsym(alsa.library, name);
  };
  alsa.open = reinterpret_cast<PcmOpen>(find("snd_pcm_open"));
  alsa.set = reinterpret_cast<PcmSetParams>(find("snd_pcm_set_params"));
  alsa.write = reinterpret_cast<PcmWrite>(find("snd_pcm_writei"));
  alsa.recover = reinterpret_cast<PcmRecover>(find("snd_pcm_recover"));
  alsa.close = reinterpret_cast<PcmClose>(find("snd_pcm_close"));
  alsa.drain = reinterpret_cast<PcmDrain>(find("snd_pcm_drain"));
  return alsa;
}

}  // namespace

struct WolangAudio {
  void* pcm = nullptr;
  Ring ring;
  std::thread worker;
  std::atomic<bool> running{false};
  int32_t rate = 48000;
  int32_t channels = 2;
};

extern "C" WolangAudio* wolang_audio_open(int32_t rate,
                                          int32_t channels) {
  Alsa& alsa = OpenAlsa();
  if (alsa.open == nullptr || alsa.set == nullptr ||
      alsa.write == nullptr) {
    return nullptr;
  }
  if (rate <= 0) {
    rate = 48000;
  }
  if (channels != 1 && channels != 2) {
    channels = 2;
  }
  WolangAudio* audio = new WolangAudio();
  audio->rate = rate;
  audio->channels = channels;
  audio->ring.Open(rate / 2, channels);

  // `SND_PCM_STREAM_PLAYBACK` is zero, and the last argument is how far
  // behind the output may fall, in microseconds.
  if (alsa.open(&audio->pcm, "default", 0, 0) < 0 ||
      alsa.set(audio->pcm, /*SND_PCM_FORMAT_FLOAT_LE=*/14,
               /*SND_PCM_ACCESS_RW_INTERLEAVED=*/3,
               static_cast<unsigned>(channels),
               static_cast<unsigned>(rate), 1, 50000) < 0) {
    if (audio->pcm != nullptr && alsa.close != nullptr) {
      alsa.close(audio->pcm);
    }
    delete audio;
    return nullptr;
  }

  // A thread that moves frames from the ring to the device: ALSA's own
  // writes block, and a frame loop must not.
  audio->running.store(true);
  audio->worker = std::thread([audio]() {
    Alsa& alsa = OpenAlsa();
    std::vector<float> block(256 *
                             static_cast<size_t>(audio->channels));
    while (audio->running.load()) {
      audio->ring.Read(block.data(), 256);
      const long written =
          alsa.write(audio->pcm, block.data(), 256);
      if (written < 0 && alsa.recover != nullptr) {
        alsa.recover(audio->pcm, static_cast<int>(written), 1);
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
  Alsa& alsa = OpenAlsa();
  if (audio->pcm != nullptr && alsa.close != nullptr) {
    alsa.close(audio->pcm);
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
