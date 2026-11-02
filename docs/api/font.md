# font 0.1.0

## font

### struct `Slot`

```wolang
offer struct Slot
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

#### field `left`

```wolang
var left: int
```

#### field `top`

```wolang
var top: int
```

#### field `advance`

```wolang
var advance: float
```

### class `Atlas`

```wolang
offer class Atlas
```

#### field `pixels`

```wolang
var pixels: []mut u8
```

#### field `width`

```wolang
var width: int
```

#### field `height`

```wolang
var height: int
```

#### field `slots`

```wolang
var slots: Map<int, Slot>
```

#### field `cursor_x`

```wolang
var cursor_x: int = 0
```

#### field `cursor_y`

```wolang
var cursor_y: int = 0
```

#### field `row_height`

```wolang
var row_height: int = 0
```

#### method `deinit`

```wolang
deinit
```

#### method `Count`

```wolang
fun Count() -> int
```

#### method `At`

```wolang
fun At(x: int, y: int) -> int
```

#### method `Key`

```wolang
fun Key(glyph: int, size: float) -> int
```

#### method `Find`

```wolang
fun Find(glyph: int, size: float) -> Slot? uses unsafe
```

#### method `Get`

```wolang
fun Get(font: Font, glyph: int, size: float) -> Slot? uses unsafe
```

#### method `ToImage`

```wolang
fun ToImage() -> Image? uses unsafe
```

### function `AtlasNew`

```wolang
offer fun AtlasNew(width: int, height: int) -> Atlas!(BadSize, TooLarge) uses unsafe
```

### error `Missing`

```wolang
offer error Missing
```

### function `SystemFontFile`

```wolang
offer fun SystemFontFile(name: text) -> text? uses io, unsafe
```

### function `Available`

```wolang
offer fun Available() -> bool uses io, unsafe
```

### function `FontLoadFile`

```wolang
offer fun FontLoadFile(path: text) -> Font!(Malformed, TooLarge, Unsupported, Missing) uses io, unsafe
```

### function `SampleFont`

```wolang
offer fun SampleFont() -> Font!(Malformed, TooLarge, Unsupported) uses unsafe
```

### error `Malformed`

```wolang
offer error Malformed
```

### error `Unsupported`

```wolang
offer error Unsupported
```

### error `TooLarge`

```wolang
offer error TooLarge
```

### constant `kMaxFontBytes`

```wolang
offer const kMaxFontBytes: int = 67108864
```

### constant `kMaxPoints`

```wolang
offer const kMaxPoints: int = 16384
```

### struct `Affine`

```wolang
offer struct Affine
```

#### field `a`

```wolang
var a: float
```

#### field `b`

```wolang
var b: float
```

#### field `c`

```wolang
var c: float
```

#### field `d`

```wolang
var d: float
```

#### field `e`

```wolang
var e: float
```

#### field `f`

```wolang
var f: float
```

#### method `Then`

```wolang
fun Then(child: Affine) -> Affine
```

### function `AffineIdentity`

```wolang
offer fun AffineIdentity() -> Affine
```

### class `Outline`

```wolang
offer class Outline
```

#### field `xs`

```wolang
var xs: List<float>
```

#### field `ys`

```wolang
var ys: List<float>
```

#### field `on`

```wolang
var on: List<bool>
```

#### field `ends`

```wolang
var ends: List<int>
```

#### method `Contours`

```wolang
fun Contours() -> int
```

#### method `Points`

```wolang
fun Points() -> int
```

### function `OutlineNew`

```wolang
offer fun OutlineNew() -> Outline uses unsafe
```

### class `Font`

```wolang
offer class Font
```

#### field `data`

```wolang
var data: []mut u8
```

#### field `units`

```wolang
var units: int = 0
```

#### field `glyphs`

```wolang
var glyphs: int = 0
```

#### field `long_loca`

```wolang
var long_loca: bool = false
```

#### field `ascent`

```wolang
var ascent: int = 0
```

#### field `descent`

```wolang
var descent: int = 0
```

#### field `gap`

```wolang
var gap: int = 0
```

#### field `metrics`

```wolang
var metrics: int = 0
```

#### field `hmtx`

```wolang
var hmtx: int = 0
```

#### field `loca`

```wolang
var loca: int = 0
```

#### field `glyf`

```wolang
var glyf: int = 0
```

#### field `glyf_len`

```wolang
var glyf_len: int = 0
```

#### field `cmap`

```wolang
var cmap: int = 0
```

#### field `cmap_format`

```wolang
var cmap_format: int = 0
```

#### field `kern`

```wolang
var kern: int = 0
```

#### field `kern_pairs`

```wolang
var kern_pairs: int = 0
```

#### method `deinit`

```wolang
deinit
```

#### method `UnitsPerEm`

```wolang
fun UnitsPerEm() -> int
```

#### method `GlyphCount`

```wolang
fun GlyphCount() -> int
```

#### method `Ascent`

```wolang
fun Ascent() -> int
```

#### method `Descent`

```wolang
fun Descent() -> int
```

#### method `LineGap`

```wolang
fun LineGap() -> int
```

#### method `Scale`

```wolang
fun Scale(size: float) -> float
```

#### method `Advance`

```wolang
fun Advance(glyph: int) -> int
```

#### method `LeftBearing`

```wolang
fun LeftBearing(glyph: int) -> int
```

#### method `GlyphIndex`

```wolang
fun GlyphIndex(code: int) -> int
```

#### method `LookupFormat4`

```wolang
fun LookupFormat4(code: int) -> int
```

#### method `LookupFormat12`

```wolang
fun LookupFormat12(code: int) -> int
```

#### method `Kerning`

```wolang
fun Kerning(left: int, right: int) -> int
```

#### method `GlyphStart`

```wolang
fun GlyphStart(glyph: int) -> int
```

#### method `GlyphEnd`

```wolang
fun GlyphEnd(glyph: int) -> int
```

#### method `GlyphOutline`

```wolang
fun GlyphOutline(glyph: int) -> Outline!(Malformed) uses unsafe
```

#### method `Collect`

```wolang
fun Collect(glyph: int, depth: int, t: Affine, out: Outline) -> bool uses unsafe
```

#### method `CollectSimple`

```wolang
fun CollectSimple(at: int, limit: int, contours: int, t: Affine, out: Outline) -> bool uses unsafe
```

#### method `CollectComposite`

```wolang
fun CollectComposite(first: int, limit: int, depth: int, t: Affine, out: Outline) -> bool uses unsafe
```

### function `FontLoad`

```wolang
offer fun FontLoad(bytes: []u8) -> Font!(Malformed, TooLarge, Unsupported) uses unsafe
```

### struct `Placement`

```wolang
offer struct Placement
```

#### field `glyph`

```wolang
var glyph: int
```

#### field `code`

```wolang
var code: int
```

#### field `x`

```wolang
var x: float
```

#### field `y`

```wolang
var y: float
```

#### field `line`

```wolang
var line: int
```

### class `Layout`

```wolang
offer class Layout
```

#### field `items`

```wolang
var items: List<Placement>
```

#### field `width`

```wolang
var width: float
```

#### field `height`

```wolang
var height: float
```

#### field `lines`

```wolang
var lines: int
```

#### field `line_height`

```wolang
var line_height: float
```

#### method `Count`

```wolang
fun Count() -> int
```

#### method `At`

```wolang
fun At(index: int) -> Placement
```

### function `LineHeight`

```wolang
offer fun LineHeight(font: Font, size: float) -> float
```

### function `LayoutText`

```wolang
offer fun LayoutText(font: Font, value: text, size: float, wrap: float) -> Layout uses unsafe
```

### function `Measure`

```wolang
offer fun Measure(font: Font, value: text, size: float) -> float uses unsafe
```

### function `DrawText`

```wolang
offer fun DrawText(target: Image, font: Font, value: text, size: float, x: int, y: int, colour: u32) uses unsafe
```

### error `BadSize`

```wolang
offer error BadSize
```

### constant `kMaxBitmapSide`

```wolang
offer const kMaxBitmapSide: int = 4096
```

### class `Glyph`

```wolang
offer class Glyph
```

#### field `coverage`

```wolang
var coverage: []mut u8
```

#### field `width`

```wolang
var width: int
```

#### field `height`

```wolang
var height: int
```

#### field `left`

```wolang
var left: int
```

#### field `top`

```wolang
var top: int
```

#### field `advance`

```wolang
var advance: float
```

#### method `deinit`

```wolang
deinit
```

#### method `At`

```wolang
fun At(x: int, y: int) -> int
```

#### method `Total`

```wolang
fun Total() -> int
```

#### method `ToImage`

```wolang
fun ToImage(red: int, green: int, blue: int) -> Image? uses unsafe
```

### function `Flatten`

```wolang
offer fun Flatten(outline: Outline, scale: float, edges: List<float>) uses unsafe
```

### function `AccumulateLine`

```wolang
offer fun AccumulateLine(acc: []mut float, w: int, h: int, ax: float, ay: float, bx: float, by: float)
```

### function `Rasterize`

```wolang
offer fun Rasterize(font: Font, glyph: int, size: float) -> Glyph!(Malformed, TooLarge, BadSize) uses unsafe
```

### function `DrawGlyph`

```wolang
offer fun DrawGlyph(target: Image, glyph: Glyph, x: int, y: int, colour: u32)
```

### function `BlendPixel`

```wolang
offer fun BlendPixel(target: Image, x: int, y: int, red: int, green: int, blue: int, alpha: int)
```
