// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The macOS backend: Cocoa for the window and the events, Core Graphics
// for a buffer of pixels, NSOpenGL for the GPU, and an audio unit for
// sound.
//
// Written against the frameworks rather than over a library that wraps
// them: this is the one place in the project that has to know what a
// system calls a window, and wrapping that in something else would mean
// two layers to debug and a dependency to ship (D155).

#include <AppKit/AppKit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <GameController/GameController.h>
#include <dlfcn.h>

#include <string>

#include "platform/common.h"
#include "platform/gamepad_decode.h"

using wolang::platform::Blank;
using wolang::platform::Events;
using wolang::platform::Ring;

// ---------------------------------------------------------------- keys ---

namespace {

// Virtual key codes on this system are the ones a keyboard sends, and
// these are the project's own: a table rather than arithmetic, because
// the layout of the two has nothing in common.
int32_t KeyFromCode(unsigned short code) {
  switch (code) {
    case 0: return kWolangKeyA;
    case 11: return kWolangKeyB;
    case 8: return kWolangKeyC;
    case 2: return kWolangKeyD;
    case 14: return kWolangKeyE;
    case 3: return kWolangKeyF;
    case 5: return kWolangKeyG;
    case 4: return kWolangKeyH;
    case 34: return kWolangKeyI;
    case 38: return kWolangKeyJ;
    case 40: return kWolangKeyK;
    case 37: return kWolangKeyL;
    case 46: return kWolangKeyM;
    case 45: return kWolangKeyN;
    case 31: return kWolangKeyO;
    case 35: return kWolangKeyP;
    case 12: return kWolangKeyQ;
    case 15: return kWolangKeyR;
    case 1: return kWolangKeyS;
    case 17: return kWolangKeyT;
    case 32: return kWolangKeyU;
    case 9: return kWolangKeyV;
    case 13: return kWolangKeyW;
    case 7: return kWolangKeyX;
    case 16: return kWolangKeyY;
    case 6: return kWolangKeyZ;
    case 29: return kWolangKey0;
    case 18: return kWolangKey1;
    case 19: return kWolangKey2;
    case 20: return kWolangKey3;
    case 21: return kWolangKey4;
    case 23: return kWolangKey5;
    case 22: return kWolangKey6;
    case 26: return kWolangKey7;
    case 28: return kWolangKey8;
    case 25: return kWolangKey9;
    case 49: return kWolangKeySpace;
    case 36: return kWolangKeyEnter;
    case 53: return kWolangKeyEscape;
    case 48: return kWolangKeyTab;
    case 51: return kWolangKeyBackspace;
    case 117: return kWolangKeyDelete;
    case 123: return kWolangKeyLeft;
    case 124: return kWolangKeyRight;
    case 126: return kWolangKeyUp;
    case 125: return kWolangKeyDown;
    case 56: case 60: return kWolangKeyShift;
    case 59: case 62: return kWolangKeyControl;
    case 58: case 61: return kWolangKeyAlt;
    case 55: case 54: return kWolangKeySuper;
    case 122: return kWolangKeyF1;
    case 120: return kWolangKeyF2;
    case 99: return kWolangKeyF3;
    case 118: return kWolangKeyF4;
    case 96: return kWolangKeyF5;
    case 97: return kWolangKeyF6;
    case 98: return kWolangKeyF7;
    case 100: return kWolangKeyF8;
    case 101: return kWolangKeyF9;
    case 109: return kWolangKeyF10;
    case 103: return kWolangKeyF11;
    case 111: return kWolangKeyF12;
    case 27: return kWolangKeyMinus;
    case 24: return kWolangKeyEqual;
    case 43: return kWolangKeyComma;
    case 47: return kWolangKeyPeriod;
    case 44: return kWolangKeySlash;
    default: return kWolangKeyUnknown;
  }
}

uint32_t ModifiersOf(NSEventModifierFlags flags) {
  uint32_t held = 0;
  if ((flags & NSEventModifierFlagShift) != 0) {
    held |= kWolangModShift;
  }
  if ((flags & NSEventModifierFlagControl) != 0) {
    held |= kWolangModControl;
  }
  if ((flags & NSEventModifierFlagOption) != 0) {
    held |= kWolangModAlt;
  }
  if ((flags & NSEventModifierFlagCommand) != 0) {
    held |= kWolangModSuper;
  }
  return held;
}

}  // namespace

// -------------------------------------------------------------- window ---

struct WolangWindow {
  NSWindow* window = nil;
  id view = nil;
  NSOpenGLContext* gl = nil;
  Events events;
  // The pixels of the last `present`, kept because a draw can be asked
  // for at any time -- a window moved between displays redraws itself.
  std::vector<uint32_t> pixels;
  int32_t pixel_width = 0;
  int32_t pixel_height = 0;
  bool closed = false;
  // (D247)
  int32_t shape = 0;
  bool cursor_visible = true;
  bool locked = false;
  // Whether `[NSCursor hide]` is currently owed an unhide.
  bool cursor_hidden = false;
  std::vector<std::string> drops;
};

// The view: it draws the pixels and it is what the window makes first
// responder, so key events arrive here.
@interface WolangView : NSView
@property(nonatomic, assign) WolangWindow* owner;
@end

@implementation WolangView

- (BOOL)isOpaque {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)wantsUpdateLayer {
  return NO;
}

// (D247) The shape is a cursor rect over the whole view, which is how
// AppKit keeps it right as the pointer comes and goes.
- (void)resetCursorRects {
  WolangWindow* owner = self.owner;
  NSCursor* cursor = [NSCursor arrowCursor];
  if (owner != nullptr) {
    switch (owner->shape) {
      case kWolangCursorHand: cursor = [NSCursor pointingHandCursor]; break;
      case kWolangCursorText: cursor = [NSCursor IBeamCursor]; break;
      case kWolangCursorCrosshair: cursor = [NSCursor crosshairCursor]; break;
      case kWolangCursorResizeH:
        cursor = [NSCursor resizeLeftRightCursor];
        break;
      case kWolangCursorResizeV:
        cursor = [NSCursor resizeUpDownCursor];
        break;
      case kWolangCursorResizeAll: cursor = [NSCursor openHandCursor]; break;
      default: break;
    }
  }
  [self addCursorRect:self.bounds cursor:cursor];
}

// Files dragged onto the window.
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  (void)sender;
  return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  WolangWindow* owner = self.owner;
  if (owner == nullptr) {
    return NO;
  }
  NSArray* urls = [[sender draggingPasteboard]
      readObjectsForClasses:@[ [NSURL class] ]
                    options:@{
                      NSPasteboardURLReadingFileURLsOnlyKey : @YES
                    }];
  owner->drops.clear();
  for (NSURL* url in urls) {
    owner->drops.push_back([[url path] UTF8String]);
  }
  WolangEvent made = Blank(kWolangEventDropped);
  made.key = static_cast<int32_t>(owner->drops.size());
  owner->events.Push(made);
  return YES;
}

- (void)drawRect:(NSRect)dirty {
  (void)dirty;
  WolangWindow* owner = self.owner;
  if (owner == nullptr || owner->pixels.empty()) {
    [[NSColor blackColor] set];
    NSRectFill(self.bounds);
    return;
  }
  CGContextRef context =
      (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGDataProviderRef provider = CGDataProviderCreateWithData(
      nullptr, owner->pixels.data(),
      owner->pixels.size() * sizeof(uint32_t), nullptr);
  // Thirty-two bits in the machine's order with the alpha byte first:
  // which is what `0xAARRGGBB` is on a little-endian machine, and what
  // the header promises a caller.
  CGImageRef image = CGImageCreate(
      static_cast<size_t>(owner->pixel_width),
      static_cast<size_t>(owner->pixel_height), 8, 32,
      static_cast<size_t>(owner->pixel_width) * 4, space,
      kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
      nullptr, false, kCGRenderingIntentDefault);
  if (image != nullptr) {
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextDrawImage(context, self.bounds, image);
    CGImageRelease(image);
  }
  CGDataProviderRelease(provider);
  CGColorSpaceRelease(space);
}

@end

// The delegate: a window that is asked to close says so through the
// queue rather than closing itself, because what to do about it is the
// program's decision.
@interface WolangDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) WolangWindow* owner;
@end

@implementation WolangDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
  (void)sender;
  if (self.owner != nullptr) {
    self.owner->events.Push(Blank(kWolangEventClosed));
    self.owner->closed = true;
  }
  return NO;
}

- (void)windowDidResize:(NSNotification*)note {
  (void)note;
  if (self.owner == nullptr) {
    return;
  }
  const NSRect backing =
      [self.owner->view convertRectToBacking:[self.owner->view bounds]];
  WolangEvent event = Blank(kWolangEventResized);
  event.width = static_cast<int32_t>(backing.size.width);
  event.height = static_cast<int32_t>(backing.size.height);
  self.owner->events.Push(event);
  if (self.owner->gl != nil) {
    [self.owner->gl update];
  }
}

- (void)windowDidChangeBackingProperties:(NSNotification*)note {
  (void)note;
  if (self.owner == nullptr) {
    return;
  }
  WolangEvent event = Blank(kWolangEventScaleChanged);
  event.scroll_x = [self.owner->window backingScaleFactor];
  self.owner->events.Push(event);
}

- (void)windowDidBecomeKey:(NSNotification*)note {
  (void)note;
  if (self.owner != nullptr) {
    self.owner->events.Push(Blank(kWolangEventFocus));
  }
}

- (void)windowDidResignKey:(NSNotification*)note {
  (void)note;
  if (self.owner != nullptr) {
    self.owner->events.Push(Blank(kWolangEventBlur));
  }
}

@end

namespace {

// The application object, made once.
//
// A process that was not started as a bundle has no application until
// something asks for one, and a window in a process with no application
// never comes to the front. `kProcessTransformToForegroundApplication`
// is what makes a plain executable into something that can own a window.
void StartApplication() {
  static bool started = false;
  if (started) {
    return;
  }
  started = true;
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp finishLaunching];
  [NSApp activateIgnoringOtherApps:YES];
}

WolangWindow* g_last_window = nullptr;

}  // namespace

extern "C" int32_t wolang_platform_available(void) { return 1; }

extern "C" const char* wolang_platform_backend(void) { return "cocoa"; }

extern "C" WolangWindow* wolang_window_open(const char* title,
                                            int32_t width, int32_t height,
                                            uint32_t flags) {
  @autoreleasepool {
    StartApplication();
    if (width <= 0 || height <= 0) {
      return nullptr;
    }
    WolangWindow* held = new WolangWindow();

    NSUInteger style = NSWindowStyleMaskTitled |
                       NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable;
    if ((flags & kWolangWindowResizable) != 0) {
      style |= NSWindowStyleMaskResizable;
    }
    const NSRect frame = NSMakeRect(0, 0, width, height);
    held->window =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:style
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    if (held->window == nil) {
      delete held;
      return nullptr;
    }
    [held->window setTitle:[NSString stringWithUTF8String:
                                         title != nullptr ? title
                                                          : "wolang"]];
    [held->window center];

    WolangView* view = [[WolangView alloc] initWithFrame:frame];
    view.owner = held;
    [view setWantsBestResolutionOpenGLSurface:YES];
    held->view = view;
    [held->window setContentView:view];
    [held->window makeFirstResponder:view];
    [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];

    WolangDelegate* delegate = [[WolangDelegate alloc] init];
    delegate.owner = held;
    [held->window setDelegate:delegate];

    if ((flags & kWolangWindowGl) != 0) {
      // The newest profile this system offers through this interface:
      // 4.1 core, which is where Apple's own OpenGL stopped. A program
      // that wants more than that wants Metal, and can reach it the same
      // way this file reaches Cocoa.
      NSOpenGLPixelFormatAttribute attributes[] = {
          NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
          NSOpenGLPFAColorSize, 24,
          NSOpenGLPFAAlphaSize, 8,
          NSOpenGLPFADepthSize, 24,
          NSOpenGLPFADoubleBuffer,
          NSOpenGLPFAAccelerated,
          0,
      };
      NSOpenGLPixelFormat* format =
          [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
      if (format != nil) {
        held->gl = [[NSOpenGLContext alloc] initWithFormat:format
                                              shareContext:nil];
        [held->gl setView:view];
      }
    }

    if ((flags & kWolangWindowHidden) == 0) {
      [held->window makeKeyAndOrderFront:nil];
    }
    g_last_window = held;
    return held;
  }
}

extern "C" void wolang_window_close(WolangWindow* window) {
  if (window == nullptr) {
    return;
  }
  @autoreleasepool {
    if (window->locked) {
      CGAssociateMouseAndMouseCursorPosition(true);
    }
    if (window->cursor_hidden) {
      [NSCursor unhide];
    }
    [window->window setDelegate:nil];
    [window->window close];
  }
  if (g_last_window == window) {
    g_last_window = nullptr;
  }
  delete window;
}

extern "C" int32_t wolang_window_poll(WolangWindow* window,
                                      WolangEvent* out) {
  if (window == nullptr || out == nullptr) {
    return 0;
  }
  @autoreleasepool {
    // Everything the system has, turned into this project's events and
    // then handed on: a window only drags, resizes and shows a menu if
    // the application still sees the events it was sent.
    for (;;) {
      NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:[NSDate distantPast]
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
      if (event == nil) {
        break;
      }
      const NSEventType kind = [event type];
      const uint32_t modifiers = ModifiersOf([event modifierFlags]);
      NSPoint where = NSZeroPoint;
      if (kind == NSEventTypeMouseMoved ||
          kind == NSEventTypeLeftMouseDown ||
          kind == NSEventTypeLeftMouseUp ||
          kind == NSEventTypeRightMouseDown ||
          kind == NSEventTypeRightMouseUp ||
          kind == NSEventTypeOtherMouseDown ||
          kind == NSEventTypeOtherMouseUp ||
          kind == NSEventTypeLeftMouseDragged ||
          kind == NSEventTypeRightMouseDragged ||
          kind == NSEventTypeScrollWheel) {
        const NSPoint in_view =
            [window->view convertPoint:[event locationInWindow]
                              fromView:nil];
        const NSRect backing =
            [window->view convertRectToBacking:
                              NSMakeRect(in_view.x, in_view.y, 1, 1)];
        const NSRect bounds =
            [window->view convertRectToBacking:[window->view bounds]];
        where.x = backing.origin.x;
        // Cocoa counts from the bottom and everything else counts from
        // the top.
        where.y = bounds.size.height - backing.origin.y;
      }

      switch (kind) {
        case NSEventTypeKeyDown:
        case NSEventTypeKeyUp: {
          WolangEvent made = Blank(kind == NSEventTypeKeyDown
                                       ? kWolangEventKeyDown
                                       : kWolangEventKeyUp);
          made.key = KeyFromCode([event keyCode]);
          made.modifiers = modifiers;
          made.repeat = kind == NSEventTypeKeyDown && [event isARepeat];
          window->events.Push(made);
          if (kind == NSEventTypeKeyDown) {
            NSString* typed = [event characters];
            for (NSUInteger i = 0; i < [typed length]; ++i) {
              const unichar unit = [typed characterAtIndex:i];
              // A control character is a key, not text.
              if (unit < 0x20 || unit == 0x7F) {
                continue;
              }
              WolangEvent text = Blank(kWolangEventText);
              text.code_point = unit;
              text.modifiers = modifiers;
              window->events.Push(text);
            }
          }
          break;
        }
        case NSEventTypeLeftMouseDown:
        case NSEventTypeRightMouseDown:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseUp: {
          const bool down = kind == NSEventTypeLeftMouseDown ||
                            kind == NSEventTypeRightMouseDown ||
                            kind == NSEventTypeOtherMouseDown;
          WolangEvent made = Blank(down ? kWolangEventMouseDown
                                        : kWolangEventMouseUp);
          made.key = kind == NSEventTypeLeftMouseDown ||
                             kind == NSEventTypeLeftMouseUp
                         ? kWolangMouseLeft
                         : (kind == NSEventTypeRightMouseDown ||
                            kind == NSEventTypeRightMouseUp
                                ? kWolangMouseRight
                                : kWolangMouseMiddle);
          made.x = static_cast<int32_t>(where.x);
          made.y = static_cast<int32_t>(where.y);
          made.modifiers = modifiers;
          window->events.Push(made);
          break;
        }
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged: {
          WolangEvent made = Blank(kWolangEventMouseMoved);
          made.x = static_cast<int32_t>(where.x);
          made.y = static_cast<int32_t>(where.y);
          // Movement in pixels; it keeps arriving while the pointer is
          // detached from the cursor, which is what mouse-look needs
          // (D247).
          const double dense = [window->window backingScaleFactor];
          made.scroll_x = [event deltaX] * dense;
          made.scroll_y = [event deltaY] * dense;
          made.modifiers = modifiers;
          window->events.Push(made);
          break;
        }
        case NSEventTypeScrollWheel: {
          WolangEvent made = Blank(kWolangEventScrolled);
          made.scroll_x = [event scrollingDeltaX];
          made.scroll_y = [event scrollingDeltaY];
          made.x = static_cast<int32_t>(where.x);
          made.y = static_cast<int32_t>(where.y);
          made.modifiers = modifiers;
          window->events.Push(made);
          break;
        }
        default:
          break;
      }
      [NSApp sendEvent:event];
    }
  }
  return window->events.Take(out) ? 1 : 0;
}

extern "C" void wolang_window_size(WolangWindow* window, int32_t* width,
                                   int32_t* height) {
  if (window == nullptr) {
    return;
  }
  @autoreleasepool {
    const NSRect backing =
        [window->view convertRectToBacking:[window->view bounds]];
    if (width != nullptr) {
      *width = static_cast<int32_t>(backing.size.width);
    }
    if (height != nullptr) {
      *height = static_cast<int32_t>(backing.size.height);
    }
  }
}

extern "C" double wolang_window_scale(WolangWindow* window) {
  if (window == nullptr) {
    return 1.0;
  }
  @autoreleasepool {
    return [window->window backingScaleFactor];
  }
}

// ------------------------------------------------- cursor and window ---
//
// (D247)

namespace {

// Hide and unhide must balance, so the state is applied from the facts
// rather than toggled by each call.
void ApplyCursorVisibility(WolangWindow* window) {
  const bool want_hidden = !window->cursor_visible || window->locked;
  if (want_hidden && !window->cursor_hidden) {
    [NSCursor hide];
    window->cursor_hidden = true;
  } else if (!want_hidden && window->cursor_hidden) {
    [NSCursor unhide];
    window->cursor_hidden = false;
  }
}

double ScreenHeight() {
  return [[NSScreen screens] firstObject].frame.size.height;
}

}  // namespace

extern "C" void wolang_window_cursor_visible(WolangWindow* window,
                                             int32_t visible) {
  if (window == nullptr) {
    return;
  }
  @autoreleasepool {
    window->cursor_visible = visible != 0;
    ApplyCursorVisibility(window);
  }
}

extern "C" void wolang_window_cursor_shape(WolangWindow* window,
                                           int32_t shape) {
  if (window == nullptr || shape < 0 || shape > 6) {
    return;
  }
  @autoreleasepool {
    window->shape = shape;
    [window->window invalidateCursorRectsForView:window->view];
  }
}

extern "C" int32_t wolang_window_cursor_lock(WolangWindow* window,
                                             int32_t locked) {
  if (window == nullptr) {
    return 0;
  }
  @autoreleasepool {
    if ((locked != 0) == window->locked) {
      return 1;
    }
    window->locked = locked != 0;
    CGAssociateMouseAndMouseCursorPosition(!window->locked);
    ApplyCursorVisibility(window);
  }
  return 1;
}

// The system's own full screen, which gets its own Space; a borderless
// window made by hand cannot become key and loses the keyboard.
extern "C" void wolang_window_set_fullscreen(WolangWindow* window,
                                             int32_t fullscreen) {
  if (window == nullptr) {
    return;
  }
  @autoreleasepool {
    const bool now = ([window->window styleMask] &
                      NSWindowStyleMaskFullScreen) != 0;
    if (now != (fullscreen != 0)) {
      [window->window toggleFullScreen:nil];
    }
  }
}

extern "C" int32_t wolang_window_is_fullscreen(WolangWindow* window) {
  if (window == nullptr) {
    return 0;
  }
  @autoreleasepool {
    return ([window->window styleMask] & NSWindowStyleMaskFullScreen) != 0
               ? 1
               : 0;
  }
}

extern "C" void wolang_window_set_size(WolangWindow* window, int32_t width,
                                       int32_t height) {
  if (window == nullptr || width <= 0 || height <= 0) {
    return;
  }
  @autoreleasepool {
    // Points, where the argument is pixels.
    const double dense = [window->window backingScaleFactor];
    [window->window setContentSize:NSMakeSize(width / dense, height / dense)];
  }
}

extern "C" void wolang_window_set_min_size(WolangWindow* window,
                                           int32_t width, int32_t height) {
  if (window == nullptr) {
    return;
  }
  @autoreleasepool {
    const double dense = [window->window backingScaleFactor];
    [window->window setContentMinSize:NSMakeSize(width / dense,
                                                 height / dense)];
  }
}

extern "C" void wolang_window_set_position(WolangWindow* window, int32_t x,
                                           int32_t y) {
  if (window == nullptr) {
    return;
  }
  @autoreleasepool {
    // Cocoa counts from the bottom left of the screen, to the bottom of
    // the window.
    const NSRect frame = [window->window frame];
    [window->window setFrameOrigin:NSMakePoint(
                                       x, ScreenHeight() - y -
                                              frame.size.height)];
  }
}

extern "C" void wolang_window_position(WolangWindow* window, int32_t* x,
                                       int32_t* y) {
  NSRect frame = NSZeroRect;
  if (window != nullptr) {
    @autoreleasepool {
      frame = [window->window frame];
    }
  }
  if (x != nullptr) {
    *x = static_cast<int32_t>(frame.origin.x);
  }
  if (y != nullptr) {
    *y = window != nullptr
             ? static_cast<int32_t>(ScreenHeight() - frame.origin.y -
                                    frame.size.height)
             : 0;
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
  @autoreleasepool {
    [window->window setTitle:[NSString stringWithUTF8String:title]];
  }
}

extern "C" void wolang_window_present(WolangWindow* window,
                                      const uint32_t* pixels,
                                      int32_t width, int32_t height) {
  if (window == nullptr || pixels == nullptr || width <= 0 ||
      height <= 0) {
    return;
  }
  const size_t count = static_cast<size_t>(width) * height;
  window->pixels.assign(pixels, pixels + count);
  window->pixel_width = width;
  window->pixel_height = height;
  @autoreleasepool {
    // Drawn now rather than when the system next asks: a frame loop that
    // presents expects to see it, and `setNeedsDisplay` would wait for a
    // run loop this program may never return to.
    [window->view display];
  }
}

extern "C" int32_t wolang_window_gl_start(WolangWindow* window) {
  if (window == nullptr || window->gl == nil) {
    return 0;
  }
  @autoreleasepool {
    [window->gl makeCurrentContext];
  }
  return 1;
}

extern "C" void wolang_window_swap(WolangWindow* window,
                                   int32_t interval) {
  if (window == nullptr || window->gl == nil) {
    return;
  }
  @autoreleasepool {
    GLint wanted = interval != 0 ? 1 : 0;
    [window->gl setValues:&wanted forParameter:NSOpenGLContextParameterSwapInterval];
    [window->gl flushBuffer];
  }
}

extern "C" void* wolang_gl_proc(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  // The framework's symbols are in the process once a context exists, so
  // the loader is the lookup -- this system has no `glXGetProcAddress`.
  return ::dlsym(RTLD_DEFAULT, name);
}

extern "C" const char* wolang_clipboard_text(void) {
  static std::string held;
  @autoreleasepool {
    NSString* text =
        [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    held = text != nil ? [text UTF8String] : "";
  }
  return held.c_str();
}

extern "C" void wolang_clipboard_set(const char* text) {
  if (text == nullptr) {
    return;
  }
  @autoreleasepool {
    NSPasteboard* board = [NSPasteboard generalPasteboard];
    [board clearContents];
    [board setString:[NSString stringWithUTF8String:text]
             forType:NSPasteboardTypeString];
  }
}

// --------------------------------------------------------------- sound ---

struct WolangAudio {
  AudioUnit unit = nullptr;
  Ring ring;
  int32_t rate = 48000;
};

namespace {

OSStatus Render(void* user, AudioUnitRenderActionFlags* flags,
                const AudioTimeStamp* when, UInt32 bus, UInt32 frames,
                AudioBufferList* buffers) {
  (void)flags;
  (void)when;
  (void)bus;
  WolangAudio* audio = static_cast<WolangAudio*>(user);
  if (audio == nullptr || buffers == nullptr ||
      buffers->mNumberBuffers == 0) {
    return noErr;
  }
  float* into = static_cast<float*>(buffers->mBuffers[0].mData);
  audio->ring.Read(into, static_cast<int32_t>(frames));
  return noErr;
}

}  // namespace

extern "C" WolangAudio* wolang_audio_open(int32_t rate,
                                          int32_t channels) {
  if (rate <= 0) {
    rate = 48000;
  }
  if (channels != 1 && channels != 2) {
    channels = 2;
  }
  WolangAudio* audio = new WolangAudio();
  audio->rate = rate;
  // Half a second of room: enough that a frame that ran long does not
  // become a gap, and little enough that a sound starts when it is
  // pushed.
  audio->ring.Open(rate / 2, channels);

  AudioComponentDescription want = {};
  want.componentType = kAudioUnitType_Output;
  want.componentSubType = kAudioUnitSubType_DefaultOutput;
  want.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &want);
  if (component == nullptr ||
      AudioComponentInstanceNew(component, &audio->unit) != noErr) {
    delete audio;
    return nullptr;
  }

  AudioStreamBasicDescription format = {};
  format.mSampleRate = rate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagIsFloat |
                        kAudioFormatFlagIsPacked;
  format.mChannelsPerFrame = static_cast<UInt32>(channels);
  format.mBitsPerChannel = 32;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = 4 * static_cast<UInt32>(channels);
  format.mBytesPerPacket = format.mBytesPerFrame;
  if (AudioUnitSetProperty(audio->unit, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Input, 0, &format,
                           sizeof(format)) != noErr) {
    AudioComponentInstanceDispose(audio->unit);
    delete audio;
    return nullptr;
  }

  AURenderCallbackStruct callback = {};
  callback.inputProc = &Render;
  callback.inputProcRefCon = audio;
  if (AudioUnitSetProperty(audio->unit,
                           kAudioUnitProperty_SetRenderCallback,
                           kAudioUnitScope_Input, 0, &callback,
                           sizeof(callback)) != noErr ||
      AudioUnitInitialize(audio->unit) != noErr ||
      AudioOutputUnitStart(audio->unit) != noErr) {
    AudioComponentInstanceDispose(audio->unit);
    delete audio;
    return nullptr;
  }
  return audio;
}

extern "C" void wolang_audio_close(WolangAudio* audio) {
  if (audio == nullptr) {
    return;
  }
  if (audio->unit != nullptr) {
    AudioOutputUnitStop(audio->unit);
    AudioUnitUninitialize(audio->unit);
    AudioComponentInstanceDispose(audio->unit);
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
// GameController.framework rather than IOKit HID: it already knows the
// layouts of the pads people own, so there is no table to keep (D247).

namespace {

struct MacPads {
  // A controller per slot, or nil. Strong references under ARC.
  GCController* slots[wolang::platform::kMaxGamepads] = {};
  std::vector<WolangEvent> pending;
  bool started = false;
};

MacPads& HeldPads() {
  static MacPads pads;
  return pads;
}

}  // namespace

extern "C" int32_t wolang_gamepad_poll(WolangEvent* out) {
  using namespace wolang::platform;
  MacPads& held = HeldPads();
  @autoreleasepool {
    if (!held.started) {
      held.started = true;
      if (@available(macOS 11.3, *)) {
        // Without this a pad goes quiet whenever another window has
        // focus, which is surprising in a game on a second monitor.
        GCController.shouldMonitorBackgroundEvents = YES;
      }
    }
    if (held.pending.empty()) {
      NSArray<GCController*>* present = [GCController controllers];
      for (int32_t i = 0; i < kMaxGamepads; ++i) {
        if (held.slots[i] != nil && ![present containsObject:held.slots[i]]) {
          held.slots[i] = nil;
          WolangEvent made = Blank(kWolangEventGamepadDisconnected);
          made.key = i;
          held.pending.push_back(made);
        }
      }
      for (GCController* pad in present) {
        if (pad.extendedGamepad == nil) {
          continue;
        }
        bool known = false;
        int32_t free_slot = -1;
        for (int32_t i = 0; i < kMaxGamepads; ++i) {
          if (held.slots[i] == pad) {
            known = true;
          } else if (held.slots[i] == nil && free_slot < 0) {
            free_slot = i;
          }
        }
        if (!known && free_slot >= 0) {
          held.slots[free_slot] = pad;
          WolangEvent made = Blank(kWolangEventGamepadConnected);
          made.key = free_slot;
          held.pending.push_back(made);
        }
      }
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
                 HeldPads().slots[index] != nil
             ? 1
             : 0;
}

extern "C" const char* wolang_gamepad_name(int32_t index) {
  static std::string held;
  held.clear();
  if (wolang_gamepad_connected(index) != 0) {
    @autoreleasepool {
      NSString* name = HeldPads().slots[index].vendorName;
      held = name != nil ? [name UTF8String] : "Controller";
    }
  }
  return held.c_str();
}

extern "C" int32_t wolang_gamepad_button(int32_t index, int32_t button) {
  using namespace wolang::platform;
  if (wolang_gamepad_connected(index) == 0 || button < 0 ||
      button >= kPadButtonCount) {
    return 0;
  }
  @autoreleasepool {
    GCExtendedGamepad* pad = HeldPads().slots[index].extendedGamepad;
    if (pad == nil) {
      return 0;
    }
    switch (button) {
      case kPadA: return pad.buttonA.isPressed;
      case kPadB: return pad.buttonB.isPressed;
      case kPadX: return pad.buttonX.isPressed;
      case kPadY: return pad.buttonY.isPressed;
      case kPadLeftBumper: return pad.leftShoulder.isPressed;
      case kPadRightBumper: return pad.rightShoulder.isPressed;
      case kPadBack: return pad.buttonOptions.isPressed;
      case kPadStart: return pad.buttonMenu.isPressed;
      case kPadLeftStick: return pad.leftThumbstickButton.isPressed;
      case kPadRightStick: return pad.rightThumbstickButton.isPressed;
      case kPadDpadUp: return pad.dpad.up.isPressed;
      case kPadDpadDown: return pad.dpad.down.isPressed;
      case kPadDpadLeft: return pad.dpad.left.isPressed;
      case kPadDpadRight: return pad.dpad.right.isPressed;
      default: return 0;
    }
  }
}

extern "C" double wolang_gamepad_axis(int32_t index, int32_t axis) {
  using namespace wolang::platform;
  if (wolang_gamepad_connected(index) == 0 || axis < 0 ||
      axis >= kGamepadAxes) {
    return 0.0;
  }
  @autoreleasepool {
    GCExtendedGamepad* pad = HeldPads().slots[index].extendedGamepad;
    if (pad == nil) {
      return 0.0;
    }
    // The framework counts up as positive; the layout counts it negative.
    switch (axis) {
      case kPadLeftX: return pad.leftThumbstick.xAxis.value;
      case kPadLeftY: return -pad.leftThumbstick.yAxis.value;
      case kPadRightX: return pad.rightThumbstick.xAxis.value;
      case kPadRightY: return -pad.rightThumbstick.yAxis.value;
      case kPadLeftTrigger: return pad.leftTrigger.value;
      case kPadRightTrigger: return pad.rightTrigger.value;
      default: return 0.0;
    }
  }
}

// ----------------------------------------------------------- dialogs ---
//
// (D247) AppKit's own panels, run modally.

extern "C" int32_t wolang_dialog_available(void) { return 1; }

extern "C" int32_t wolang_dialog_message(const char* title, const char* text,
                                         int32_t level, int32_t buttons) {
  @autoreleasepool {
    StartApplication();
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:[NSString stringWithUTF8String:title != nullptr
                                                             ? title
                                                             : ""]];
    [alert setInformativeText:[NSString stringWithUTF8String:text != nullptr
                                                                 ? text
                                                                 : ""]];
    [alert setAlertStyle:level == kWolangDialogError
                             ? NSAlertStyleCritical
                             : (level == kWolangDialogWarning
                                    ? NSAlertStyleWarning
                                    : NSAlertStyleInformational)];
    if (buttons == kWolangButtonsYesNo) {
      [alert addButtonWithTitle:@"Yes"];
      [alert addButtonWithTitle:@"No"];
    } else {
      [alert addButtonWithTitle:@"OK"];
      if (buttons == kWolangButtonsOkCancel) {
        [alert addButtonWithTitle:@"Cancel"];
      }
    }
    const bool first = [alert runModal] == NSAlertFirstButtonReturn;
    if (buttons == kWolangButtonsYesNo) {
      return first ? kWolangAnswerYes : kWolangAnswerNo;
    }
    if (buttons == kWolangButtonsOkCancel) {
      return first ? kWolangAnswerOk : kWolangAnswerCancel;
    }
    return kWolangAnswerOk;
  }
}

namespace {

const char* RunPanel(NSSavePanel* panel, const char* title) {
  static std::string held;
  StartApplication();
  if (title != nullptr) {
    [panel setMessage:[NSString stringWithUTF8String:title]];
  }
  if ([panel runModal] != NSModalResponseOK || [panel URL] == nil) {
    return nullptr;
  }
  held = [[[panel URL] path] UTF8String];
  return held.c_str();
}

}  // namespace

extern "C" const char* wolang_dialog_open_file(const char* title) {
  @autoreleasepool {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];
    return RunPanel(panel, title);
  }
}

extern "C" const char* wolang_dialog_save_file(const char* title,
                                               const char* start_name) {
  @autoreleasepool {
    NSSavePanel* panel = [NSSavePanel savePanel];
    if (start_name != nullptr && start_name[0] != '\0') {
      [panel setNameFieldStringValue:[NSString
                                         stringWithUTF8String:start_name]];
    }
    return RunPanel(panel, title);
  }
}
