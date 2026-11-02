# gfx

## gfx

### function `Available`

```wolang
offer fun Available() -> bool uses unsafe
```

Whether this build can open a window at all.

### function `Backend`

```wolang
offer fun Backend() -> text uses unsafe
```

Which backend it is: "cocoa", "win32", "x11" or "none".

### function `Time`

```wolang
offer fun Time() -> f64 uses unsafe
```

Seconds from a clock that does not jump.

### function `Sleep`

```wolang
offer fun Sleep(seconds: f64) uses unsafe
```

Waits, which is what a frame loop does with the time it has left.

### struct `Event`

```wolang
offer struct Event
```

What happened.

One struct for every kind, laid out exactly as the C one: a flat
shape costs a few unused fields and saves every program from taking
a tagged union apart.

#### field `kind`

```wolang
var kind: s32 = 0
```

#### field `key`

```wolang
var key: s32 = 0
```

A key for a key event, a button for a mouse one.

#### field `x`

```wolang
var x: s32 = 0
```

#### field `y`

```wolang
var y: s32 = 0
```

#### field `scroll_x`

```wolang
var scroll_x: f64 = 0.0
```

#### field `scroll_y`

```wolang
var scroll_y: f64 = 0.0
```

#### field `width`

```wolang
var width: s32 = 0
```

#### field `height`

```wolang
var height: s32 = 0
```

#### field `modifiers`

```wolang
var modifiers: u32 = 0
```

#### field `code_point`

```wolang
var code_point: u32 = 0
```

What was typed, for a `kText` event.

#### field `repeat`

```wolang
var repeat: s32 = 0
```

### constant `kNone`

```wolang
offer const kNone: s32 = 0
```

### constant `kClosed`

```wolang
offer const kClosed: s32 = 1
```

### constant `kResized`

```wolang
offer const kResized: s32 = 2
```

### constant `kPressed`

```wolang
offer const kPressed: s32 = 3
```

### constant `kReleased`

```wolang
offer const kReleased: s32 = 4
```

### constant `kMouseDown`

```wolang
offer const kMouseDown: s32 = 5
```

### constant `kMouseUp`

```wolang
offer const kMouseUp: s32 = 6
```

### constant `kMouseMoved`

```wolang
offer const kMouseMoved: s32 = 7
```

### constant `kScrolled`

```wolang
offer const kScrolled: s32 = 8
```

### constant `kText`

```wolang
offer const kText: s32 = 9
```

### constant `kFocus`

```wolang
offer const kFocus: s32 = 10
```

### constant `kBlur`

```wolang
offer const kBlur: s32 = 11
```

### constant `kDropped`

```wolang
offer const kDropped: s32 = 12
```

Files were dropped on the window: `key` is how many, and
`Window.DropPath` answers each one.

### constant `kScaleChanged`

```wolang
offer const kScaleChanged: s32 = 13
```

The pixel scale changed (another display, a new setting): `scroll_x`
is the new scale.

### constant `kGamepadConnected`

```wolang
offer const kGamepadConnected: s32 = 14
```

A pad appeared or went away: `key` is its slot, 0 to 3.

### constant `kGamepadDisconnected`

```wolang
offer const kGamepadDisconnected: s32 = 15
```

### constant `kMouseLeft`

```wolang
offer const kMouseLeft: s32 = 1
```

### constant `kMouseRight`

```wolang
offer const kMouseRight: s32 = 2
```

### constant `kMouseMiddle`

```wolang
offer const kMouseMiddle: s32 = 3
```

### constant `kModShift`

```wolang
offer const kModShift: u32 = 1
```

### constant `kModControl`

```wolang
offer const kModControl: u32 = 2
```

### constant `kModAlt`

```wolang
offer const kModAlt: u32 = 4
```

### constant `kModSuper`

```wolang
offer const kModSuper: u32 = 8
```

### constant `kKeyA`

```wolang
offer const kKeyA: s32 = 1
```

### constant `kKeyB`

```wolang
offer const kKeyB: s32 = 2
```

### constant `kKeyC`

```wolang
offer const kKeyC: s32 = 3
```

### constant `kKeyD`

```wolang
offer const kKeyD: s32 = 4
```

### constant `kKeyE`

```wolang
offer const kKeyE: s32 = 5
```

### constant `kKeyF`

```wolang
offer const kKeyF: s32 = 6
```

### constant `kKeyG`

```wolang
offer const kKeyG: s32 = 7
```

### constant `kKeyH`

```wolang
offer const kKeyH: s32 = 8
```

### constant `kKeyI`

```wolang
offer const kKeyI: s32 = 9
```

### constant `kKeyJ`

```wolang
offer const kKeyJ: s32 = 10
```

### constant `kKeyK`

```wolang
offer const kKeyK: s32 = 11
```

### constant `kKeyL`

```wolang
offer const kKeyL: s32 = 12
```

### constant `kKeyM`

```wolang
offer const kKeyM: s32 = 13
```

### constant `kKeyN`

```wolang
offer const kKeyN: s32 = 14
```

### constant `kKeyO`

```wolang
offer const kKeyO: s32 = 15
```

### constant `kKeyP`

```wolang
offer const kKeyP: s32 = 16
```

### constant `kKeyQ`

```wolang
offer const kKeyQ: s32 = 17
```

### constant `kKeyR`

```wolang
offer const kKeyR: s32 = 18
```

### constant `kKeyS`

```wolang
offer const kKeyS: s32 = 19
```

### constant `kKeyT`

```wolang
offer const kKeyT: s32 = 20
```

### constant `kKeyU`

```wolang
offer const kKeyU: s32 = 21
```

### constant `kKeyV`

```wolang
offer const kKeyV: s32 = 22
```

### constant `kKeyW`

```wolang
offer const kKeyW: s32 = 23
```

### constant `kKeyX`

```wolang
offer const kKeyX: s32 = 24
```

### constant `kKeyY`

```wolang
offer const kKeyY: s32 = 25
```

### constant `kKeyZ`

```wolang
offer const kKeyZ: s32 = 26
```

### constant `kKey0`

```wolang
offer const kKey0: s32 = 27
```

### constant `kKey1`

```wolang
offer const kKey1: s32 = 28
```

### constant `kKey2`

```wolang
offer const kKey2: s32 = 29
```

### constant `kKey3`

```wolang
offer const kKey3: s32 = 30
```

### constant `kKey4`

```wolang
offer const kKey4: s32 = 31
```

### constant `kKey5`

```wolang
offer const kKey5: s32 = 32
```

### constant `kKey6`

```wolang
offer const kKey6: s32 = 33
```

### constant `kKey7`

```wolang
offer const kKey7: s32 = 34
```

### constant `kKey8`

```wolang
offer const kKey8: s32 = 35
```

### constant `kKey9`

```wolang
offer const kKey9: s32 = 36
```

### constant `kKeySpace`

```wolang
offer const kKeySpace: s32 = 37
```

### constant `kKeyEnter`

```wolang
offer const kKeyEnter: s32 = 38
```

### constant `kKeyEscape`

```wolang
offer const kKeyEscape: s32 = 39
```

### constant `kKeyTab`

```wolang
offer const kKeyTab: s32 = 40
```

### constant `kKeyBackspace`

```wolang
offer const kKeyBackspace: s32 = 41
```

### constant `kKeyDelete`

```wolang
offer const kKeyDelete: s32 = 42
```

### constant `kKeyLeft`

```wolang
offer const kKeyLeft: s32 = 43
```

### constant `kKeyRight`

```wolang
offer const kKeyRight: s32 = 44
```

### constant `kKeyUp`

```wolang
offer const kKeyUp: s32 = 45
```

### constant `kKeyDown`

```wolang
offer const kKeyDown: s32 = 46
```

### constant `kKeyShift`

```wolang
offer const kKeyShift: s32 = 47
```

### constant `kKeyControl`

```wolang
offer const kKeyControl: s32 = 48
```

### constant `kKeyAlt`

```wolang
offer const kKeyAlt: s32 = 49
```

### constant `kKeySuper`

```wolang
offer const kKeySuper: s32 = 50
```

### constant `kKeyF1`

```wolang
offer const kKeyF1: s32 = 51
```

### constant `kKeyF2`

```wolang
offer const kKeyF2: s32 = 52
```

### constant `kKeyF3`

```wolang
offer const kKeyF3: s32 = 53
```

### constant `kKeyF4`

```wolang
offer const kKeyF4: s32 = 54
```

### constant `kKeyF5`

```wolang
offer const kKeyF5: s32 = 55
```

### constant `kKeyF6`

```wolang
offer const kKeyF6: s32 = 56
```

### constant `kKeyF7`

```wolang
offer const kKeyF7: s32 = 57
```

### constant `kKeyF8`

```wolang
offer const kKeyF8: s32 = 58
```

### constant `kKeyF9`

```wolang
offer const kKeyF9: s32 = 59
```

### constant `kKeyF10`

```wolang
offer const kKeyF10: s32 = 60
```

### constant `kKeyF11`

```wolang
offer const kKeyF11: s32 = 61
```

### constant `kKeyF12`

```wolang
offer const kKeyF12: s32 = 62
```

### constant `kResizable`

```wolang
offer const kResizable: u32 = 1
```

### constant `kGl`

```wolang
offer const kGl: u32 = 2
```

Ask for a GL context. Without it the window is for pixels.

### constant `kHidden`

```wolang
offer const kHidden: u32 = 4
```

### class `Window`

```wolang
offer class Window
```

A window, and the only thing that owns one.

`IsOpen` says whether there is one: a machine with no display answers
false rather than failing, which is what lets the same program run on
a server and say so.

#### field `handle`

```wolang
var handle: *mut u8
```

#### method `deinit`

```wolang
deinit
```

#### method `IsOpen`

```wolang
fun IsOpen() -> bool
```

#### method `Poll`

```wolang
fun Poll() -> Event uses unsafe
```

The next thing that happened, or a `kNone` event when there is
nothing left this turn.

#### method `ShowCursor`

```wolang
fun ShowCursor(visible: bool) uses unsafe
```

Shows or hides the pointer over this window.

#### method `SetCursor`

```wolang
fun SetCursor(shape: s32) uses unsafe
```

One of the `kCursor*` shapes.

#### method `LockCursor`

```wolang
fun LockCursor(locked: bool) -> bool uses unsafe
```

Pins the pointer and reports only how far it moved, for mouse-look:
`kMouseMoved` events then carry the movement in `scroll_x` and
`scroll_y`. Says whether the system allowed it.

#### method `SetFullscreen`

```wolang
fun SetFullscreen(on: bool) uses unsafe
```

Full screen on and off.

#### method `IsFullscreen`

```wolang
fun IsFullscreen() -> bool uses unsafe
```

#### method `SetSize`

```wolang
fun SetSize(width: int, height: int) uses unsafe
```

Resizes the content, in pixels.

#### method `SetMinSize`

```wolang
fun SetMinSize(width: int, height: int) uses unsafe
```

The smallest content size a person can drag the window to; zero
for none.

#### method `SetPosition`

```wolang
fun SetPosition(x: int, y: int) uses unsafe
```

#### method `X`

```wolang
fun X() -> int uses unsafe
```

#### method `Y`

```wolang
fun Y() -> int uses unsafe
```

#### method `DropPath`

```wolang
fun DropPath(index: int) -> text uses unsafe
```

A path from the last `kDropped` event, or an empty text past the
end.

#### method `Width`

```wolang
fun Width() -> int uses unsafe
```

The content size in pixels, which on a dense display is not the
size in points.

#### method `Height`

```wolang
fun Height() -> int uses unsafe
```

#### method `Scale`

```wolang
fun Scale() -> f64 uses unsafe
```

#### method `SetTitle`

```wolang
fun SetTitle(title: text) uses unsafe
```

#### method `Present`

```wolang
fun Present(canvas: &Canvas) uses unsafe
```

Puts a canvas on the screen, stretched to the window.

#### method `StartGl`

```wolang
fun StartGl() -> bool uses unsafe
```

Makes this window's GL context current, and says whether there is
one. Only a window opened with `kGl` has one.

#### method `Swap`

```wolang
fun Swap(hold: bool) uses unsafe
```

Shows what GL drew. `hold` is whether to wait for the display,
which is what keeps a frame loop at the refresh rate.

#### method `Close`

```wolang
fun Close() uses unsafe
```

### function `Open`

```wolang
offer fun Open(title: text, width: int, height: int, flags: u32) -> Window uses unsafe
```

Opens a window. A closed one comes back when there is no backend or
the system refused.

### function `GlProc`

```wolang
offer fun GlProc(name: text) -> *mut u8 uses unsafe
```

The address of a GL entry point, for a program that binds OpenGL
itself. Zero when there is none.

### function `Clipboard`

```wolang
offer fun Clipboard() -> text uses unsafe
```

What the clipboard holds, and how to put something there.

### function `SetClipboard`

```wolang
offer fun SetClipboard(value: text) uses unsafe
```

### class `Canvas`

```wolang
offer class Canvas
```

A buffer of pixels, and what can be drawn into it.

`0xAARRGGBB`, one word per pixel, rows top to bottom: the one format
every backend here takes without converting. A canvas owns its
storage and gives it back when it goes out of scope.

#### field `pixels`

```wolang
var pixels: []mut u32
```

#### field `width`

```wolang
var width: int = 0
```

#### field `height`

```wolang
var height: int = 0
```

#### method `deinit`

```wolang
deinit
```

#### method `Clear`

```wolang
fun Clear(colour: u32)
```

#### method `Set`

```wolang
fun Set(x: int, y: int, colour: u32)
```

One pixel, ignored when it is outside: a drawing routine that
clamps is one that can be called with anything.

#### method `At`

```wolang
fun At(x: int, y: int) -> u32
```

#### method `Rect`

```wolang
fun Rect(x: int, y: int, w: int, h: int, colour: u32)
```

A filled rectangle, clipped to the canvas.

#### method `Frame`

```wolang
fun Frame(x: int, y: int, w: int, h: int, colour: u32)
```

The outline of a rectangle.

#### method `Line`

```wolang
fun Line(x0: int, y0: int, x1: int, y1: int, colour: u32)
```

A line, by the oldest algorithm there is: integers only, one pixel
a step, no gaps.

#### method `Circle`

```wolang
fun Circle(cx: int, cy: int, radius: int, colour: u32)
```

A filled circle, by the distance test: a radius this is used at is
small, and the obvious loop is fast enough and obviously right.

#### method `Blit`

```wolang
fun Blit(source: &Canvas, x: int, y: int)
```

Copies another canvas in at `x, y`, skipping fully transparent
pixels.

### function `CanvasNew`

```wolang
offer fun CanvasNew(width: int, height: int) -> Canvas uses unsafe
```

A canvas of `width` by `height`, all of it opaque black.

### function `Rgb`

```wolang
offer fun Rgb(red: int, green: int, blue: int) -> u32
```

A colour from parts, each 0 to 255.

### function `Rgba`

```wolang
offer fun Rgba(red: int, green: int, blue: int, alpha: int) -> u32
```

### class `Audio`

```wolang
offer class Audio
```

The default output, played from a queue.

A queue rather than a callback: a callback would be the system's
audio thread calling into a tier that may still be compiling, and a
program that makes sound has a frame loop to push from anyway (D155).

#### field `handle`

```wolang
var handle: *mut u8
```

#### field `rate`

```wolang
var rate: int = 48000
```

#### field `channels`

```wolang
var channels: int = 2
```

#### method `deinit`

```wolang
deinit
```

#### method `IsOpen`

```wolang
fun IsOpen() -> bool
```

#### method `Queue`

```wolang
fun Queue(samples: []f32) -> int uses unsafe
```

Adds interleaved frames, each sample between -1 and 1, and answers
how many it took.

#### method `Queued`

```wolang
fun Queued() -> int uses unsafe
```

How many frames are waiting, and how many more fit.

#### method `Room`

```wolang
fun Room() -> int uses unsafe
```

#### method `Close`

```wolang
fun Close() uses unsafe
```

### function `OpenAudio`

```wolang
offer fun OpenAudio(rate: int, channels: int) -> Audio uses unsafe
```

Opens the default output. 48000 and two channels is what to ask for
unless there is a reason not to.

### constant `kCursorArrow`

```wolang
offer const kCursorArrow: s32 = 0
```

### constant `kCursorHand`

```wolang
offer const kCursorHand: s32 = 1
```

### constant `kCursorText`

```wolang
offer const kCursorText: s32 = 2
```

### constant `kCursorCrosshair`

```wolang
offer const kCursorCrosshair: s32 = 3
```

### constant `kCursorResizeH`

```wolang
offer const kCursorResizeH: s32 = 4
```

### constant `kCursorResizeV`

```wolang
offer const kCursorResizeV: s32 = 5
```

### constant `kCursorResizeAll`

```wolang
offer const kCursorResizeAll: s32 = 6
```

### constant `kMaxGamepads`

```wolang
offer const kMaxGamepads: int = 4
```

### constant `kPadA`

```wolang
offer const kPadA: s32 = 0
```

### constant `kPadB`

```wolang
offer const kPadB: s32 = 1
```

### constant `kPadX`

```wolang
offer const kPadX: s32 = 2
```

### constant `kPadY`

```wolang
offer const kPadY: s32 = 3
```

### constant `kPadLeftBumper`

```wolang
offer const kPadLeftBumper: s32 = 4
```

### constant `kPadRightBumper`

```wolang
offer const kPadRightBumper: s32 = 5
```

### constant `kPadBack`

```wolang
offer const kPadBack: s32 = 6
```

### constant `kPadStart`

```wolang
offer const kPadStart: s32 = 7
```

### constant `kPadGuide`

```wolang
offer const kPadGuide: s32 = 8
```

### constant `kPadLeftStick`

```wolang
offer const kPadLeftStick: s32 = 9
```

### constant `kPadRightStick`

```wolang
offer const kPadRightStick: s32 = 10
```

### constant `kPadDpadUp`

```wolang
offer const kPadDpadUp: s32 = 11
```

### constant `kPadDpadDown`

```wolang
offer const kPadDpadDown: s32 = 12
```

### constant `kPadDpadLeft`

```wolang
offer const kPadDpadLeft: s32 = 13
```

### constant `kPadDpadRight`

```wolang
offer const kPadDpadRight: s32 = 14
```

### constant `kAxisLeftX`

```wolang
offer const kAxisLeftX: s32 = 0
```

### constant `kAxisLeftY`

```wolang
offer const kAxisLeftY: s32 = 1
```

### constant `kAxisRightX`

```wolang
offer const kAxisRightX: s32 = 2
```

### constant `kAxisRightY`

```wolang
offer const kAxisRightY: s32 = 3
```

### constant `kAxisLeftTrigger`

```wolang
offer const kAxisLeftTrigger: s32 = 4
```

### constant `kAxisRightTrigger`

```wolang
offer const kAxisRightTrigger: s32 = 5
```

### function `GamepadConnected`

```wolang
offer fun GamepadConnected(index: int) -> bool uses unsafe
```

Whether pad `index` (0 to 3) is plugged in. Events from `Poll` say
when that changes.

### function `GamepadName`

```wolang
offer fun GamepadName(index: int) -> text uses unsafe
```

### function `GamepadButton`

```wolang
offer fun GamepadButton(index: int, button: s32) -> bool uses unsafe
```

### function `GamepadAxis`

```wolang
offer fun GamepadAxis(index: int, axis: s32) -> f64 uses unsafe
```

### function `AxisNormalize`

```wolang
offer fun AxisNormalize(raw: int, low: int, high: int) -> f64
```

Scales a raw reading from `[low, high]` to `[-1, 1]`, clamped; zero
when the range is empty. The same arithmetic every backend does.

### function `Deadzone`

```wolang
offer fun Deadzone(value: f64, dead: f64) -> f64
```

Ignores a stick resting near the middle: anything inside `dead` is
zero, and the rest is stretched so the output still reaches 1 and
does not jump at the edge of the zone.

### function `IsGamepadEvent`

```wolang
offer fun IsGamepadEvent(kind: s32) -> bool
```

Whether an event kind belongs to a pad.

### constant `kDialogInfo`

```wolang
offer const kDialogInfo: s32 = 0
```

### constant `kDialogWarning`

```wolang
offer const kDialogWarning: s32 = 1
```

### constant `kDialogError`

```wolang
offer const kDialogError: s32 = 2
```

### constant `kButtonsOk`

```wolang
offer const kButtonsOk: s32 = 0
```

### constant `kButtonsOkCancel`

```wolang
offer const kButtonsOkCancel: s32 = 1
```

### constant `kButtonsYesNo`

```wolang
offer const kButtonsYesNo: s32 = 2
```

### constant `kAnswerUnavailable`

```wolang
offer const kAnswerUnavailable: s32 = 0
```

### constant `kAnswerOk`

```wolang
offer const kAnswerOk: s32 = 1
```

### constant `kAnswerCancel`

```wolang
offer const kAnswerCancel: s32 = 2
```

### constant `kAnswerYes`

```wolang
offer const kAnswerYes: s32 = 3
```

### constant `kAnswerNo`

```wolang
offer const kAnswerNo: s32 = 4
```

### function `DialogsAvailable`

```wolang
offer fun DialogsAvailable() -> bool uses unsafe
```

### function `MessageBox`

```wolang
offer fun MessageBox(title: text, body: text, level: s32, buttons: s32) -> s32 uses unsafe
```

Shows a message and answers one of the `kAnswer*` constants.

### function `OpenFileDialog`

```wolang
offer fun OpenFileDialog(title: text) -> text? uses unsafe
```

Asks for a file to open. Null when cancelled or unavailable.

### function `SaveFileDialog`

```wolang
offer fun SaveFileDialog(title: text, name: text) -> text? uses unsafe
```

Asks where to save, starting from `name`. Null when cancelled or
unavailable.
