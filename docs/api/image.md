# image 0.1.0

## image

### error `Malformed`

```wolang
offer error Malformed
```

The file is not what it says it is: bad signature, bad checksum, a
length that runs past the end, or data that stops early.

### error `Unsupported`

```wolang
offer error Unsupported
```

A real format feature this reader does not do (compressed BMP,
a PNG with an unknown critical chunk, ...).

### error `TooLarge`

```wolang
offer error TooLarge
```

The picture is bigger than `kMaxSide` on a side or `kMaxPixels` in all.

### constant `kMaxSide`

```wolang
offer const kMaxSide: int = 16384
```

The longest side a picture may have.

### constant `kMaxPixels`

```wolang
offer const kMaxPixels: int = 67108864
```

The most pixels a picture may have (64 Mi, so 256 MiB of RGBA): a
header can claim any size, and the check happens before the buffer.

### class `Image`

```wolang
offer class Image
```

A picture: `width * height` pixels of RGBA, four bytes each.

Owns its storage and gives it back when it goes out of scope.

#### field `pixels`

```wolang
var pixels: []mut u8
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

#### method `Contains`

```wolang
fun Contains(x: int, y: int) -> bool
```

Whether (x, y) is inside the picture.

#### method `Get`

```wolang
fun Get(x: int, y: int) -> u32
```

The pixel as `0xAARRGGBB`, or 0 outside the picture.

#### method `Set`

```wolang
fun Set(x: int, y: int, argb: u32)
```

Sets a pixel from `0xAARRGGBB`; outside the picture does nothing.

### function `ImageNew`

```wolang
offer fun ImageNew(width: int, height: int) -> Image!(Malformed, TooLarge) uses unsafe
```

A transparent black picture of the given size, or `TooLarge`, or
`Malformed` when a side is not positive.

### function `PngDecode`

```wolang
offer fun PngDecode(data: []u8) -> Image!(Malformed, TooLarge, Unsupported) uses unsafe
```

Reads a PNG: every colour type, bit depths 1 to 16 (16 is cut to 8),
all five filters, Adam7, `PLTE` and `tRNS`. Chunk checksums, the zlib
header and its Adler-32 are checked, and the inflater is held to the
size the header implies, so a small file cannot inflate into a large
one (D227, D245).

### function `PngEncode`

```wolang
offer fun PngEncode(image: &Image) -> []u8 uses unsafe
```

Writes an RGBA8 PNG, choosing for each row whichever of the five
filters leaves the smallest sum of magnitudes (the usual heuristic),
then deflating. The caller owns the result.

### function `QoiDecode`

```wolang
offer fun QoiDecode(data: []u8) -> Image!(Malformed, TooLarge) uses unsafe
```

Reads a QOI file, all six operations. Anything after the last pixel
is ignored; running out of bytes before the last pixel is `Malformed`.

### function `QoiEncode`

```wolang
offer fun QoiEncode(image: &Image) -> []u8 uses unsafe
```

Writes a QOI file (four channels, sRGB) with the run, index, diff,
luma and literal operations. The caller owns the result.

### function `BmpDecode`

```wolang
offer fun BmpDecode(data: []u8) -> Image!(Malformed, TooLarge, Unsupported) uses unsafe
```

Reads a BMP: 24 or 32 bits, uncompressed, bottom-up or top-down, with
a 40-byte or larger header. A 32-bit file's fourth byte is taken as
alpha. Palettes, RLE and bit fields are `Unsupported`.

### function `BmpEncode`

```wolang
offer fun BmpEncode(image: &Image) -> []u8 uses unsafe
```

Writes a 32-bit uncompressed bottom-up BMP, alpha in the fourth byte.
The caller owns the result.
