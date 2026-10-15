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
#include <dlfcn.h>

#include <string>

#include "platform/common.h"

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
