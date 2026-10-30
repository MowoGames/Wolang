// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// A window, what a person does to it, pixels, the GPU and sound.
//
// This is the floor the `gfx` package is written against: one C API, one
// backend per system -- Cocoa, Win32, X11 -- and a stub that says so
// where there is none. It ships as its own shared library rather than
// inside the runtime, so that a program which draws nothing links
// nothing: a compiler that pulled in Cocoa to print a number would be
// paying for a window it never opens (D155).
//
// Two ways to put something on the screen, and the same window does
// either. `wolang_window_present` hands over a buffer of pixels, which
// is what a tool, a plotter or a software renderer wants. A GL context
// is the other: `wolang_window_gl_start` makes one current and
// `wolang_window_swap` shows what was drawn, and in between a program
// calls OpenGL itself -- which is the GPU, without this file inventing
// an abstraction over it.

#ifndef WOLANG_PLATFORM_PLATFORM_H_
#define WOLANG_PLATFORM_PLATFORM_H_

#include <cstdint>
#include <stdint.h>

#if defined(_WIN32)
#define WOLANG_PLATFORM_EXPORT __declspec(dllexport)
#else
#define WOLANG_PLATFORM_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// What a window is opened with. Bits, because a program says what it
// wants and the backend says what it managed.
enum {
  kWolangWindowResizable = 1u << 0,
  // Ask for a GL context. Without this the window is for pixels.
  kWolangWindowGl = 1u << 1,
  kWolangWindowHidden = 1u << 2,
};

// What happened. One kind per thing a program has to react to, and no
// union: a few unused fields cost nothing next to a tagged shape that
// every binding then has to take apart.
enum {
  kWolangEventNone = 0,
  kWolangEventClosed = 1,
  kWolangEventResized = 2,
  kWolangEventKeyDown = 3,
  kWolangEventKeyUp = 4,
  kWolangEventMouseDown = 5,
  kWolangEventMouseUp = 6,
  kWolangEventMouseMoved = 7,
  kWolangEventScrolled = 8,
  kWolangEventText = 9,
  kWolangEventFocus = 10,
  kWolangEventBlur = 11,
  // Files were dropped on the window: `key` is how many, and
  // `wolang_window_drop_path` answers each one (D247).
  kWolangEventDropped = 12,
  // The pixel scale changed, for instance by moving to another display:
  // `scroll_x` is the new scale (D247).
  kWolangEventScaleChanged = 13,
  // A pad appeared or went away: `key` is its slot, 0 to 3 (D247).
  kWolangEventGamepadConnected = 14,
  kWolangEventGamepadDisconnected = 15,
};

// Where a key is, not what it prints: a layout is the person's business
// and `kWolangEventText` is what carries what they typed. The numbers
// are this project's own, so that every backend answers the same ones.
enum {
  kWolangKeyUnknown = 0,
  kWolangKeyA = 1, kWolangKeyB = 2, kWolangKeyC = 3, kWolangKeyD = 4,
  kWolangKeyE = 5, kWolangKeyF = 6, kWolangKeyG = 7, kWolangKeyH = 8,
  kWolangKeyI = 9, kWolangKeyJ = 10, kWolangKeyK = 11, kWolangKeyL = 12,
  kWolangKeyM = 13, kWolangKeyN = 14, kWolangKeyO = 15, kWolangKeyP = 16,
  kWolangKeyQ = 17, kWolangKeyR = 18, kWolangKeyS = 19, kWolangKeyT = 20,
  kWolangKeyU = 21, kWolangKeyV = 22, kWolangKeyW = 23, kWolangKeyX = 24,
  kWolangKeyY = 25, kWolangKeyZ = 26,
  kWolangKey0 = 27, kWolangKey1 = 28, kWolangKey2 = 29, kWolangKey3 = 30,
  kWolangKey4 = 31, kWolangKey5 = 32, kWolangKey6 = 33, kWolangKey7 = 34,
  kWolangKey8 = 35, kWolangKey9 = 36,
  kWolangKeySpace = 37, kWolangKeyEnter = 38, kWolangKeyEscape = 39,
  kWolangKeyTab = 40, kWolangKeyBackspace = 41, kWolangKeyDelete = 42,
  kWolangKeyLeft = 43, kWolangKeyRight = 44, kWolangKeyUp = 45,
  kWolangKeyDown = 46, kWolangKeyShift = 47, kWolangKeyControl = 48,
  kWolangKeyAlt = 49, kWolangKeySuper = 50,
  kWolangKeyF1 = 51, kWolangKeyF2 = 52, kWolangKeyF3 = 53,
  kWolangKeyF4 = 54, kWolangKeyF5 = 55, kWolangKeyF6 = 56,
  kWolangKeyF7 = 57, kWolangKeyF8 = 58, kWolangKeyF9 = 59,
  kWolangKeyF10 = 60, kWolangKeyF11 = 61, kWolangKeyF12 = 62,
  kWolangKeyMinus = 63, kWolangKeyEqual = 64, kWolangKeyComma = 65,
  kWolangKeyPeriod = 66, kWolangKeySlash = 67,
};

// Which button, and which modifiers were held.
enum {
  kWolangMouseLeft = 1,
  kWolangMouseRight = 2,
  kWolangMouseMiddle = 3,
};
enum {
  kWolangModShift = 1u << 0,
  kWolangModControl = 1u << 1,
  kWolangModAlt = 1u << 2,
  kWolangModSuper = 1u << 3,
};

// A thing that happened, flat so that a binding is a struct and not a
// decoder.
typedef struct WolangEvent {
  int32_t kind;
  // A key for a key event, a button for a mouse one.
  int32_t key;
  // Where the pointer is, in pixels from the top left of the content.
  int32_t x;
  int32_t y;
  // How far a wheel turned, and the size a resize arrived at. For
  // `kWolangEventMouseMoved` these are instead the movement since the
  // last such event, which is the only thing that means anything while
  // the cursor is locked (D247).
  double scroll_x;
  double scroll_y;
  int32_t width;
  int32_t height;
  uint32_t modifiers;
  // The code point typed, for `kWolangEventText`.
  uint32_t code_point;
  // Whether a key event is a repeat the system generated.
  int32_t repeat;
} WolangEvent;

typedef struct WolangWindow WolangWindow;
typedef struct WolangAudio WolangAudio;

// Whether this build has a backend at all. A program can ask before it
// tries, and the answer is a build-time fact.
WOLANG_PLATFORM_EXPORT int32_t wolang_platform_available(void);

// The name of the backend: "cocoa", "win32", "x11" or "none".
WOLANG_PLATFORM_EXPORT const char* wolang_platform_backend(void);

// Opens a window. Null when there is no backend or the system refused.
WOLANG_PLATFORM_EXPORT WolangWindow* wolang_window_open(const char* title,
                                                        int32_t width,
                                                        int32_t height,
                                                        uint32_t flags);
WOLANG_PLATFORM_EXPORT void wolang_window_close(WolangWindow* window);

// The next thing that happened, or zero when there is nothing left this
// turn. A program calls it until it answers zero.
WOLANG_PLATFORM_EXPORT int32_t wolang_window_poll(WolangWindow* window,
                                                  WolangEvent* out);

// The content size in pixels, which is not the size in points on a
// display that scales.
WOLANG_PLATFORM_EXPORT void wolang_window_size(WolangWindow* window,
                                               int32_t* width,
                                               int32_t* height);

// How much bigger a pixel is than a point. One on a plain display, two
// on a dense one.
WOLANG_PLATFORM_EXPORT double wolang_window_scale(WolangWindow* window);

WOLANG_PLATFORM_EXPORT void wolang_window_set_title(WolangWindow* window,
                                                    const char* title);

// Puts |pixels| on the screen, stretched to the window. Each pixel is
// 0xAARRGGBB in the machine's own order, |width| * |height| of them,
// rows top to bottom.
WOLANG_PLATFORM_EXPORT void wolang_window_present(WolangWindow* window,
                                                  const uint32_t* pixels,
                                                  int32_t width,
                                                  int32_t height);

// Makes this window's GL context current on the calling thread, and
// answers whether there is one. Only a window opened with
// `kWolangWindowGl` has one.
WOLANG_PLATFORM_EXPORT int32_t wolang_window_gl_start(WolangWindow* window);

// Shows what GL drew, and waits for the display when |interval| is one.
WOLANG_PLATFORM_EXPORT void wolang_window_swap(WolangWindow* window,
                                               int32_t interval);

// The address of a GL entry point, for a program binding OpenGL itself.
WOLANG_PLATFORM_EXPORT void* wolang_gl_proc(const char* name);

// Seconds since this library was loaded, from a clock that does not jump.
WOLANG_PLATFORM_EXPORT double wolang_platform_time(void);

// Puts the calling thread to sleep.
WOLANG_PLATFORM_EXPORT void wolang_platform_sleep(double seconds);

// What the clipboard holds, as UTF-8. The text belongs to the library
// and lives until the next call.
WOLANG_PLATFORM_EXPORT const char* wolang_clipboard_text(void);
WOLANG_PLATFORM_EXPORT void wolang_clipboard_set(const char* text);

// ------------------------------------------------------------- sound ---
//
// A queue rather than a callback: a callback from the system's audio
// thread into Wolang would be a second thread calling into a tier that
// may still be compiling, and every program that makes sound has a
// frame loop to push from anyway. The backend plays from a ring buffer
// and tells the program how much room is left (D155).

// Opens the default output. |rate| is frames a second (44100 or 48000),
// |channels| is one or two.
WOLANG_PLATFORM_EXPORT WolangAudio* wolang_audio_open(int32_t rate,
                                                      int32_t channels);
WOLANG_PLATFORM_EXPORT void wolang_audio_close(WolangAudio* audio);

// Adds frames to the queue, interleaved, each sample between -1 and 1.
// Answers how many frames it took, which is fewer than |frames| when the
// queue is full.
WOLANG_PLATFORM_EXPORT int32_t wolang_audio_queue(WolangAudio* audio,
                                                  const float* samples,
                                                  int32_t frames);

// How many frames are waiting to be played, and how many more fit.
WOLANG_PLATFORM_EXPORT int32_t wolang_audio_queued(WolangAudio* audio);
WOLANG_PLATFORM_EXPORT int32_t wolang_audio_room(WolangAudio* audio);

// ------------------------------------------------------------ cursor ---
//
// (D247) Everything from here to the end is optional in the sense that a
// backend may answer no; none of it is required to open a window.

enum {
  kWolangCursorArrow = 0,
  kWolangCursorHand = 1,
  kWolangCursorText = 2,
  kWolangCursorCrosshair = 3,
  kWolangCursorResizeH = 4,
  kWolangCursorResizeV = 5,
  kWolangCursorResizeAll = 6,
};

WOLANG_PLATFORM_EXPORT void wolang_window_cursor_visible(WolangWindow* window,
                                                         int32_t visible);
WOLANG_PLATFORM_EXPORT void wolang_window_cursor_shape(WolangWindow* window,
                                                       int32_t shape);
// Locks the pointer to the window and reports only its movement, for
// mouse-look. Answers whether the system allowed it.
WOLANG_PLATFORM_EXPORT int32_t wolang_window_cursor_lock(
    WolangWindow* window, int32_t locked);

// ------------------------------------------------------------ window ---

WOLANG_PLATFORM_EXPORT void wolang_window_set_fullscreen(
    WolangWindow* window, int32_t fullscreen);
WOLANG_PLATFORM_EXPORT int32_t wolang_window_is_fullscreen(
    WolangWindow* window);
// Content size in pixels. The minimum of 0 by 0 means none.
WOLANG_PLATFORM_EXPORT void wolang_window_set_size(WolangWindow* window,
                                                   int32_t width,
                                                   int32_t height);
WOLANG_PLATFORM_EXPORT void wolang_window_set_min_size(WolangWindow* window,
                                                       int32_t width,
                                                       int32_t height);
WOLANG_PLATFORM_EXPORT void wolang_window_set_position(WolangWindow* window,
                                                       int32_t x, int32_t y);
WOLANG_PLATFORM_EXPORT void wolang_window_position(WolangWindow* window,
                                                   int32_t* x, int32_t* y);
// The paths of the last drop, UTF-8, owned by the window until the next
// drop. Null past the end.
WOLANG_PLATFORM_EXPORT const char* wolang_window_drop_path(
    WolangWindow* window, int32_t index);

// ---------------------------------------------------------- gamepads ---
//
// Up to four pads in one standard layout whatever the system calls them.
// Buttons: 0 A, 1 B, 2 X, 3 Y, 4 left bumper, 5 right bumper, 6 back,
// 7 start, 8 guide, 9 left stick, 10 right stick, 11 to 14 dpad up, down,
// left, right. Axes: 0 left x, 1 left y, 2 right x, 3 right y (up and
// left are negative), 4 left trigger, 5 right trigger (0 to 1). No
// deadzone is applied: that is the program's call (D247).

// Looks for pads coming and going and reads what they did. Answers the
// next connect or disconnect as `kWolangEventGamepad*` in |out|, or 0.
WOLANG_PLATFORM_EXPORT int32_t wolang_gamepad_poll(WolangEvent* out);
WOLANG_PLATFORM_EXPORT int32_t wolang_gamepad_connected(int32_t index);
WOLANG_PLATFORM_EXPORT const char* wolang_gamepad_name(int32_t index);
WOLANG_PLATFORM_EXPORT int32_t wolang_gamepad_button(int32_t index,
                                                     int32_t button);
WOLANG_PLATFORM_EXPORT double wolang_gamepad_axis(int32_t index,
                                                  int32_t axis);

// ----------------------------------------------------------- dialogs ---
//
// Native dialogs. They block until answered (D247).

enum {
  kWolangDialogInfo = 0,
  kWolangDialogWarning = 1,
  kWolangDialogError = 2,
};
enum {
  kWolangButtonsOk = 0,
  kWolangButtonsOkCancel = 1,
  kWolangButtonsYesNo = 2,
};
// What the person pressed; zero means no dialog could be shown.
enum {
  kWolangAnswerUnavailable = 0,
  kWolangAnswerOk = 1,
  kWolangAnswerCancel = 2,
  kWolangAnswerYes = 3,
  kWolangAnswerNo = 4,
};

// Whether the dialogs below can work here. On Linux that needs `zenity`
// or `kdialog` on the PATH.
WOLANG_PLATFORM_EXPORT int32_t wolang_dialog_available(void);
WOLANG_PLATFORM_EXPORT int32_t wolang_dialog_message(const char* title,
                                                     const char* text,
                                                     int32_t level,
                                                     int32_t buttons);
// The chosen path as UTF-8 (owned by the library until the next call), or
// null when cancelled or unavailable. |start_name| may be null.
WOLANG_PLATFORM_EXPORT const char* wolang_dialog_open_file(
    const char* title);
WOLANG_PLATFORM_EXPORT const char* wolang_dialog_save_file(
    const char* title, const char* start_name);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // WOLANG_PLATFORM_PLATFORM_H_
