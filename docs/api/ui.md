# ui 0.1.0

## ui

### function `FillRect`

```wolang
offer fun FillRect(target: Image, x: int, y: int, w: int, h: int, colour: u32)
```

### function `Render`

```wolang
offer fun Render(target: Image, ui: Ui) uses unsafe
```

### struct `Rect`

```wolang
offer struct Rect
```

#### field `x`

```wolang
var x: int
```

#### field `y`

```wolang
var y: int
```

#### field `width`

```wolang
var width: int
```

#### field `height`

```wolang
var height: int
```

#### method `Contains`

```wolang
fun Contains(px: int, py: int) -> bool
```

#### method `Right`

```wolang
fun Right() -> int
```

#### method `Bottom`

```wolang
fun Bottom() -> int
```

### struct `Style`

```wolang
offer struct Style
```

#### field `background`

```wolang
var background: u32
```

#### field `panel`

```wolang
var panel: u32
```

#### field `text`

```wolang
var text: u32
```

#### field `button`

```wolang
var button: u32
```

#### field `hover`

```wolang
var hover: u32
```

#### field `active`

```wolang
var active: u32
```

#### field `accent`

```wolang
var accent: u32
```

#### field `track`

```wolang
var track: u32
```

#### field `size`

```wolang
var size: float
```

#### field `padding`

```wolang
var padding: int
```

#### field `spacing`

```wolang
var spacing: int
```

### function `StyleDefault`

```wolang
offer fun StyleDefault() -> Style
```

### class `Input`

```wolang
offer class Input
```

#### field `mouse_x`

```wolang
var mouse_x: int = 0
```

#### field `mouse_y`

```wolang
var mouse_y: int = 0
```

#### field `down`

```wolang
var down: bool = false
```

#### field `pressed`

```wolang
var pressed: bool = false
```

#### field `released`

```wolang
var released: bool = false
```

#### field `scroll`

```wolang
var scroll: float = 0.0
```

#### field `typed`

```wolang
var typed: List<int>
```

#### field `keys`

```wolang
var keys: List<int>
```

#### method `Feed`

```wolang
fun Feed(event: Event) uses unsafe
```

#### method `MoveTo`

```wolang
fun MoveTo(x: int, y: int)
```

#### method `Press`

```wolang
fun Press()
```

#### method `Release`

```wolang
fun Release()
```

#### method `Type`

```wolang
fun Type(code: int) uses unsafe
```

#### method `Key`

```wolang
fun Key(key: int) uses unsafe
```

#### method `KeyPressed`

```wolang
fun KeyPressed(key: int) -> bool
```

#### method `EndFrame`

```wolang
fun EndFrame()
```

### function `InputNew`

```wolang
offer fun InputNew() -> Input uses unsafe
```

### class `TextEdit`

```wolang
offer class TextEdit
```

#### field `codes`

```wolang
var codes: List<int>
```

#### field `cursor`

```wolang
var cursor: int = 0
```

#### method `Len`

```wolang
fun Len() -> int
```

#### method `Insert`

```wolang
fun Insert(code: int) uses unsafe
```

#### method `Backspace`

```wolang
fun Backspace() uses unsafe
```

#### method `Delete`

```wolang
fun Delete() uses unsafe
```

#### method `Remove`

```wolang
fun Remove(at: int) uses unsafe
```

#### method `Left`

```wolang
fun Left()
```

#### method `Right`

```wolang
fun Right()
```

#### method `Value`

```wolang
fun Value() -> Text uses unsafe
```

#### method `Before`

```wolang
fun Before() -> Text uses unsafe
```

### function `TextEditNew`

```wolang
offer fun TextEditNew(initial: text) -> TextEdit uses unsafe
```

### constant `kDrawFill`

```wolang
offer const kDrawFill: int = 0
```

### constant `kDrawText`

```wolang
offer const kDrawText: int = 1
```

### class `DrawCommand`

```wolang
offer class DrawCommand
```

#### field `kind`

```wolang
var kind: int
```

#### field `x`

```wolang
var x: int
```

#### field `y`

```wolang
var y: int
```

#### field `width`

```wolang
var width: int
```

#### field `height`

```wolang
var height: int
```

#### field `colour`

```wolang
var colour: u32
```

#### field `start`

```wolang
var start: int
```

#### field `length`

```wolang
var length: int
```

#### field `size`

```wolang
var size: float
```

### class `Ui`

```wolang
offer class Ui
```

#### field `font`

```wolang
var font: Font
```

#### field `style`

```wolang
var style: Style
```

#### field `input`

```wolang
var input: Input
```

#### field `commands`

```wolang
var commands: List<DrawCommand>
```

#### field `labels`

```wolang
var labels: Builder
```

#### field `next_id`

```wolang
var next_id: int = 0
```

#### field `hot`

```wolang
var hot: int = 0
```

#### field `active`

```wolang
var active: int = 0
```

#### field `focus`

```wolang
var focus: int = 0
```

#### field `left`

```wolang
var left: int = 0
```

#### field `cursor`

```wolang
var cursor: int = 0
```

#### field `width`

```wolang
var width: int = 0
```

#### field `frames`

```wolang
var frames: int = 0
```

#### method `Begin`

```wolang
fun Begin()
```

#### method `End`

```wolang
fun End()
```

#### method `Place`

```wolang
fun Place(x: int, y: int, w: int)
```

#### method `RowHeight`

```wolang
fun RowHeight() -> int uses unsafe
```

#### method `Fill`

```wolang
fun Fill(x: int, y: int, w: int, h: int, colour: u32) uses unsafe
```

#### method `Write`

```wolang
fun Write(x: int, y: int, label: text, colour: u32) uses unsafe
```

#### method `Panel`

```wolang
fun Panel(x: int, y: int, w: int, h: int) uses unsafe
```

#### method `LabelOf`

```wolang
fun LabelOf(command: DrawCommand) -> text uses unsafe
```

#### method `Spacer`

```wolang
fun Spacer(pixels: int)
```

#### method `NextRect`

```wolang
fun NextRect() -> Rect uses unsafe
```

#### method `Behave`

```wolang
fun Behave(id: int, rect: Rect) -> bool
```

#### method `Shade`

```wolang
fun Shade(id: int, rect: Rect) -> u32
```

#### method `Label`

```wolang
fun Label(label: text) uses unsafe
```

#### method `Button`

```wolang
fun Button(label: text) -> bool uses unsafe
```

#### method `Checkbox`

```wolang
fun Checkbox(label: text, value: bool) -> bool uses unsafe
```

#### method `Slider`

```wolang
fun Slider(value: float, low: float, high: float) -> float uses unsafe
```

#### method `TextField`

```wolang
fun TextField(edit: TextEdit) -> bool uses unsafe
```

#### method `ListBox`

```wolang
fun ListBox(items: List<text>, selected: int) -> int uses unsafe
```

### function `UiNew`

```wolang
offer fun UiNew(font: Font, style: Style) -> Ui uses unsafe
```
