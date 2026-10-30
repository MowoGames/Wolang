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
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/time.h>
#include <thread>
#include <vector>


#include "platform/common.h"
#include "platform/uri_list.h"

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

  // (D247) Cursor: one cursor per shape, made when first asked for.
  Cursor shapes[7] = {};
  Cursor blank = 0;
  int32_t shape = 0;
  bool cursor_visible = true;
  bool locked = false;
  int32_t last_x = -1;
  int32_t last_y = -1;
  // Window state.
  bool fullscreen = false;
  bool fixed = false;
  int32_t min_width = 0;
  int32_t min_height = 0;
  double scale = 1.0;
  // XDND: who is dragging, whether they offer files, and what landed.
  Window drop_source = 0;
  bool drop_ok = false;
  long drop_version = 0;
  std::vector<std::string> drops;
};

namespace {

// (D247) Atoms the extras need.
struct Atoms {
  Atom wm_state, wm_fullscreen, aware, enter, position, status, leave, drop,
      finished, selection, copy, type_list, uri_list, property;
};

Atoms AtomsOf(Display* display) {
  Atoms a;
  a.wm_state = XInternAtom(display, "_NET_WM_STATE", False);
  a.wm_fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
  a.aware = XInternAtom(display, "XdndAware", False);
  a.enter = XInternAtom(display, "XdndEnter", False);
  a.position = XInternAtom(display, "XdndPosition", False);
  a.status = XInternAtom(display, "XdndStatus", False);
  a.leave = XInternAtom(display, "XdndLeave", False);
  a.drop = XInternAtom(display, "XdndDrop", False);
  a.finished = XInternAtom(display, "XdndFinished", False);
  a.selection = XInternAtom(display, "XdndSelection", False);
  a.copy = XInternAtom(display, "XdndActionCopy", False);
  a.type_list = XInternAtom(display, "XdndTypeList", False);
  a.uri_list = XInternAtom(display, "text/uri-list", False);
  a.property = XInternAtom(display, "WOLANG_DROP", False);
  return a;
}

// X says nothing about scale directly; the desktop's `Xft.dpi` is what
// toolkits follow, so this does too. Read from the root property rather
// than Xlib's cached string, which does not change after connecting.
double ReadScale(Display* display) {
  Window root = DefaultRootWindow(display);
  Atom type = 0;
  int format = 0;
  unsigned long count = 0;
  unsigned long after = 0;
  unsigned char* data = nullptr;
  double scale = 1.0;
  if (XGetWindowProperty(display, root, XA_RESOURCE_MANAGER, 0, 65536,
                         False, XA_STRING, &type, &format, &count, &after,
                         &data) == Success &&
      data != nullptr) {
    const std::string text(reinterpret_cast<char*>(data), count);
    const size_t at = text.find("Xft.dpi:");
    if (at != std::string::npos) {
      const double dpi = std::atof(text.c_str() + at + 8);
      if (dpi >= 48.0 && dpi <= 960.0) {
        scale = dpi / 96.0;
      }
    }
  }
  if (data != nullptr) {
    XFree(data);
  }
  return scale;
}

void ApplyHints(WolangWindow* w) {
  XSizeHints* hints = XAllocSizeHints();
  if (hints == nullptr) {
    return;
  }
  if (w->fixed) {
    hints->flags = PMinSize | PMaxSize;
    hints->min_width = hints->max_width = w->width;
    hints->min_height = hints->max_height = w->height;
  } else if (w->min_width > 0 || w->min_height > 0) {
    hints->flags = PMinSize;
    hints->min_width = w->min_width;
    hints->min_height = w->min_height;
  }
  XSetWMNormalHints(w->display, w->window, hints);
  XFree(hints);
}

Cursor BlankCursor(WolangWindow* w) {
  if (w->blank == 0) {
    static const char kBits[1] = {0};
    Pixmap empty = XCreateBitmapFromData(w->display, w->window, kBits, 1, 1);
    XColor black = {};
    w->blank = XCreatePixmapCursor(w->display, empty, empty, &black, &black,
                                   0, 0);
    XFreePixmap(w->display, empty);
  }
  return w->blank;
}

void ApplyCursor(WolangWindow* w) {
  Cursor cursor = 0;
  if (!w->cursor_visible || w->locked) {
    cursor = BlankCursor(w);
  } else {
    const unsigned shapes[] = {XC_left_ptr,          XC_hand2,
                               XC_xterm,             XC_crosshair,
                               XC_sb_h_double_arrow, XC_sb_v_double_arrow,
                               XC_fleur};
    Cursor& made = w->shapes[w->shape];
    if (made == 0) {
      made = XCreateFontCursor(w->display, shapes[w->shape]);
    }
    cursor = made;
  }
  XDefineCursor(w->display, w->window, cursor);
  XFlush(w->display);
}

void SendDnd(WolangWindow* w, Atom message, long accepted, long action) {
  XEvent reply = {};
  reply.xclient.type = ClientMessage;
  reply.xclient.display = w->display;
  reply.xclient.window = w->drop_source;
  reply.xclient.message_type = message;
  reply.xclient.format = 32;
  reply.xclient.data.l[0] = static_cast<long>(w->window);
  reply.xclient.data.l[1] = accepted;
  if (message == XInternAtom(w->display, "XdndFinished", False)) {
    reply.xclient.data.l[2] = action;
  } else {
    reply.xclient.data.l[4] = action;
  }
  XSendEvent(w->display, w->drop_source, False, NoEventMask, &reply);
}

// Whether the source offers `text/uri-list`: in the message when it has
// three types or fewer, in a property on the source otherwise.
bool OffersFiles(WolangWindow* w, const Atoms& a,
                 const XClientMessageEvent& e) {
  if ((e.data.l[1] & 1) == 0) {
    for (int i = 2; i < 5; ++i) {
      if (static_cast<Atom>(e.data.l[i]) == a.uri_list) {
        return true;
      }
    }
    return false;
  }
  Atom type = 0;
  int format = 0;
  unsigned long count = 0;
  unsigned long after = 0;
  unsigned char* data = nullptr;
  bool found = false;
  if (XGetWindowProperty(w->display, w->drop_source, a.type_list, 0, 1024,
                         False, XA_ATOM, &type, &format, &count, &after,
                         &data) == Success &&
      data != nullptr) {
    const Atom* atoms = reinterpret_cast<Atom*>(data);
    for (unsigned long i = 0; i < count; ++i) {
      if (atoms[i] == a.uri_list) {
        found = true;
      }
    }
  }
  if (data != nullptr) {
    XFree(data);
  }
  return found;
}

}  // namespace

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

  held->fixed = (flags & kWolangWindowResizable) == 0;

  // (D247) Take file drops (XDND version 5) and watch the desktop's
  // resources for a scale change.
  const Atoms atoms = AtomsOf(display);
  const Atom version = 5;
  XChangeProperty(display, held->window, atoms.aware, XA_ATOM, 32,
                  PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&version), 1);
  XSelectInput(display, root, PropertyChangeMask);
  held->scale = ReadScale(display);

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
  if (window->locked) {
    XUngrabPointer(window->display, CurrentTime);
  }
  for (Cursor cursor : window->shapes) {
    if (cursor != 0) {
      XFreeCursor(window->display, cursor);
    }
  }
  if (window->blank != 0) {
    XFreeCursor(window->display, window->blank);
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
      case ClientMessage: {
        const Atoms a = AtomsOf(window->display);
        const Atom message = event.xclient.message_type;
        if (message == a.enter) {
          window->drop_source = static_cast<Window>(event.xclient.data.l[0]);
          window->drop_version = event.xclient.data.l[1] >> 24;
          window->drop_ok = OffersFiles(window, a, event.xclient);
        } else if (message == a.position) {
          SendDnd(window, a.status, window->drop_ok ? 1 : 0,
                  window->drop_ok ? static_cast<long>(a.copy) : 0);
        } else if (message == a.leave) {
          window->drop_ok = false;
        } else if (message == a.drop) {
          if (window->drop_ok) {
            const Time when = window->drop_version >= 1
                                  ? static_cast<Time>(
                                        event.xclient.data.l[2])
                                  : CurrentTime;
            XConvertSelection(window->display, a.selection, a.uri_list,
                              a.property, window->window, when);
          } else {
            SendDnd(window, a.finished, 0, 0);
          }
        } else if (static_cast<Atom>(event.xclient.data.l[0]) ==
                   window->delete_message) {
          window->events.Push(Blank(kWolangEventClosed));
        }
        break;
      }
      case SelectionNotify: {
        const Atoms a = AtomsOf(window->display);
        if (event.xselection.property != a.property) {
          break;
        }
        Atom type = 0;
        int format = 0;
        unsigned long count = 0;
        unsigned long after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(window->display, window->window, a.property,
                               0, 1 << 20, True, AnyPropertyType, &type,
                               &format, &count, &after, &data) == Success &&
            data != nullptr) {
          window->drops = wolang::platform::PathsFromUriList(
              std::string(reinterpret_cast<char*>(data), count));
          XFree(data);
          WolangEvent made = Blank(kWolangEventDropped);
          made.key = static_cast<int32_t>(window->drops.size());
          if (made.key > 0) {
            window->events.Push(made);
          }
        }
        SendDnd(window, a.finished, 1, static_cast<long>(a.copy));
        window->drop_ok = false;
        break;
      }
      case PropertyNotify:
        if (event.xproperty.window == DefaultRootWindow(window->display) &&
            event.xproperty.atom == XA_RESOURCE_MANAGER) {
          const double now = ReadScale(window->display);
          if (now != window->scale) {
            window->scale = now;
            WolangEvent made = Blank(kWolangEventScaleChanged);
            made.scroll_x = now;
            window->events.Push(made);
          }
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
        if (window->locked) {
          // Movement is measured from the centre the pointer is pulled
          // back to; the pull itself arrives as a motion to the centre
          // and is not the person's (D247).
          const int cx = window->width / 2;
          const int cy = window->height / 2;
          if (event.xmotion.x == cx && event.xmotion.y == cy) {
            break;
          }
          made.scroll_x = event.xmotion.x - cx;
          made.scroll_y = event.xmotion.y - cy;
          XWarpPointer(window->display, None, window->window, 0, 0, 0, 0,
                       cx, cy);
          XFlush(window->display);
        } else if (window->last_x >= 0) {
          made.scroll_x = event.xmotion.x - window->last_x;
          made.scroll_y = event.xmotion.y - window->last_y;
        }
        window->last_x = event.xmotion.x;
        window->last_y = event.xmotion.y;
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
  // From the desktop's `Xft.dpi`; one when it says nothing (D247).
  return window != nullptr ? window->scale : 1.0;
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

// ------------------------------------------------- cursor and window ---
//
// (D247)

extern "C" void wolang_window_cursor_visible(WolangWindow* window,
                                             int32_t visible) {
  if (window == nullptr) {
    return;
  }
  window->cursor_visible = visible != 0;
  ApplyCursor(window);
}

extern "C" void wolang_window_cursor_shape(WolangWindow* window,
                                           int32_t shape) {
  if (window == nullptr || shape < 0 || shape > 6) {
    return;
  }
  window->shape = shape;
  ApplyCursor(window);
}

// A pointer grab plus a pull back to the middle after every move: raw
// XInput2 deltas would need a second library, and a grab-and-warp gives
// the same unbounded movement with the Xlib already linked.
extern "C" int32_t wolang_window_cursor_lock(WolangWindow* window,
                                             int32_t locked) {
  if (window == nullptr) {
    return 0;
  }
  if (locked == 0) {
    if (window->locked) {
      XUngrabPointer(window->display, CurrentTime);
      window->locked = false;
      window->last_x = -1;
      ApplyCursor(window);
    }
    return 1;
  }
  if (window->locked) {
    return 1;
  }
  const int result = XGrabPointer(
      window->display, window->window, True,
      ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
      GrabModeAsync, GrabModeAsync, window->window, BlankCursor(window),
      CurrentTime);
  if (result != GrabSuccess) {
    return 0;
  }
  window->locked = true;
  XWarpPointer(window->display, None, window->window, 0, 0, 0, 0,
               window->width / 2, window->height / 2);
  ApplyCursor(window);
  return 1;
}

extern "C" void wolang_window_set_fullscreen(WolangWindow* window,
                                             int32_t fullscreen) {
  if (window == nullptr) {
    return;
  }
  const Atoms a = AtomsOf(window->display);
  XEvent message = {};
  message.xclient.type = ClientMessage;
  message.xclient.window = window->window;
  message.xclient.message_type = a.wm_state;
  message.xclient.format = 32;
  message.xclient.data.l[0] = fullscreen != 0 ? 1 : 0;
  message.xclient.data.l[1] = static_cast<long>(a.wm_fullscreen);
  message.xclient.data.l[3] = 1;
  XSendEvent(window->display, DefaultRootWindow(window->display), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &message);
  XFlush(window->display);
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
  if (window->fixed) {
    // A window that cannot be resized by hand pins its size in the
    // hints, so those move first or the server refuses.
    window->width = width;
    window->height = height;
    ApplyHints(window);
  }
  XResizeWindow(window->display, window->window,
                static_cast<unsigned>(width), static_cast<unsigned>(height));
  XFlush(window->display);
}

extern "C" void wolang_window_set_min_size(WolangWindow* window,
                                           int32_t width, int32_t height) {
  if (window == nullptr) {
    return;
  }
  window->min_width = width > 0 ? width : 0;
  window->min_height = height > 0 ? height : 0;
  ApplyHints(window);
  XFlush(window->display);
}

extern "C" void wolang_window_set_position(WolangWindow* window, int32_t x,
                                           int32_t y) {
  if (window == nullptr) {
    return;
  }
  XMoveWindow(window->display, window->window, x, y);
  XFlush(window->display);
}

extern "C" void wolang_window_position(WolangWindow* window, int32_t* x,
                                       int32_t* y) {
  int px = 0;
  int py = 0;
  if (window != nullptr) {
    Window child = 0;
    XTranslateCoordinates(window->display, window->window,
                          DefaultRootWindow(window->display), 0, 0, &px,
                          &py, &child);
  }
  if (x != nullptr) {
    *x = px;
  }
  if (y != nullptr) {
    *y = py;
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
