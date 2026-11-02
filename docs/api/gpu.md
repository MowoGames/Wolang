# gpu 0.1.0

## gpu

### error `CompileFailed`

```wolang
offer error CompileFailed(log: text)
```

### constant `kClearColour`

```wolang
offer const kClearColour: int = 1
```

### constant `kClearDepth`

```wolang
offer const kClearDepth: int = 2
```

### constant `kTriangles`

```wolang
offer const kTriangles: int = 0
```

### constant `kLines`

```wolang
offer const kLines: int = 1
```

### constant `kPoints`

```wolang
offer const kPoints: int = 2
```

### constant `kTriangleStrip`

```wolang
offer const kTriangleStrip: int = 3
```

### constant `kTriangleFan`

```wolang
offer const kTriangleFan: int = 4
```

### constant `kLineStrip`

```wolang
offer const kLineStrip: int = 5
```

### constant `kZero`

```wolang
offer const kZero: int = 0
```

### constant `kOne`

```wolang
offer const kOne: int = 1
```

### constant `kSourceAlpha`

```wolang
offer const kSourceAlpha: int = 2
```

### constant `kOneMinusSourceAlpha`

```wolang
offer const kOneMinusSourceAlpha: int = 3
```

### constant `kDestinationAlpha`

```wolang
offer const kDestinationAlpha: int = 4
```

### constant `kOneMinusDestinationAlpha`

```wolang
offer const kOneMinusDestinationAlpha: int = 5
```

### constant `kNever`

```wolang
offer const kNever: int = 0
```

### constant `kLess`

```wolang
offer const kLess: int = 1
```

### constant `kEqual`

```wolang
offer const kEqual: int = 2
```

### constant `kLessOrEqual`

```wolang
offer const kLessOrEqual: int = 3
```

### constant `kGreater`

```wolang
offer const kGreater: int = 4
```

### constant `kNotEqual`

```wolang
offer const kNotEqual: int = 5
```

### constant `kGreaterOrEqual`

```wolang
offer const kGreaterOrEqual: int = 6
```

### constant `kAlways`

```wolang
offer const kAlways: int = 7
```

### constant `kCullNone`

```wolang
offer const kCullNone: int = 0
```

### constant `kCullBack`

```wolang
offer const kCullBack: int = 1
```

### constant `kCullFront`

```wolang
offer const kCullFront: int = 2
```

### constant `kSpriteVertex`

```wolang
offer const kSpriteVertex: text = r"#version 330 core
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_colour;
uniform mat4 u_view;
out vec2 v_uv;
out vec4 v_colour;
void main() {
  v_uv = a_uv;
  v_colour = a_colour;
  gl_Position = u_view * vec4(a_position, 0.0, 1.0);
}
"
```

### constant `kSpriteFragment`

```wolang
offer const kSpriteFragment: text = r"#version 330 core
in vec2 v_uv;
in vec4 v_colour;
uniform sampler2D u_texture;
out vec4 o_colour;
void main() {
  o_colour = texture(u_texture, v_uv) * v_colour;
}
"
```

### constant `kSolidFragment`

```wolang
offer const kSolidFragment: text = r"#version 330 core
in vec2 v_uv;
in vec4 v_colour;
out vec4 o_colour;
void main() {
  o_colour = v_colour;
}
"
```

### function `WithHeader`

```wolang
offer fun WithHeader(source: text) -> text uses unsafe
```

### struct `Buffer`

```wolang
offer struct Buffer
```

#### field `id`

```wolang
var id: int
```

#### field `count`

```wolang
var count: int
```

### struct `Layout`

```wolang
offer struct Layout
```

#### field `id`

```wolang
var id: int
```

### struct `Program`

```wolang
offer struct Program
```

#### field `id`

```wolang
var id: int
```

### struct `Texture`

```wolang
offer struct Texture
```

#### field `id`

```wolang
var id: int
```

#### field `width`

```wolang
var width: int
```

#### field `height`

```wolang
var height: int
```

### struct `Target`

```wolang
offer struct Target
```

#### field `id`

```wolang
var id: int
```

#### field `texture`

```wolang
var texture: Texture
```

### ability `Backend`

```wolang
offer ability Backend
```

#### method `Name`

```wolang
fun Name() -> text
```

#### method `Clear`

```wolang
fun Clear(red: float, green: float, blue: float, alpha: float, depth: float, mask: int) uses unsafe
```

#### method `Viewport`

```wolang
fun Viewport(x: int, y: int, width: int, height: int) uses unsafe
```

#### method `Blend`

```wolang
fun Blend(enabled: bool, source: int, destination: int) uses unsafe
```

#### method `Depth`

```wolang
fun Depth(test: bool, write: bool, compare: int) uses unsafe
```

#### method `Cull`

```wolang
fun Cull(mode: int) uses unsafe
```

#### method `Scissor`

```wolang
fun Scissor(enabled: bool, x: int, y: int, width: int, height: int) uses unsafe
```

#### method `BufferNew`

```wolang
fun BufferNew() -> int uses unsafe
```

#### method `VertexData`

```wolang
fun VertexData(buffer: int, data: []f32, dynamic: bool) uses unsafe
```

#### method `IndexData`

```wolang
fun IndexData(buffer: int, data: []u32, dynamic: bool) uses unsafe
```

#### method `BufferDelete`

```wolang
fun BufferDelete(buffer: int) uses unsafe
```

#### method `LayoutNew`

```wolang
fun LayoutNew() -> int uses unsafe
```

#### method `LayoutAttribute`

```wolang
fun LayoutAttribute(layout: int, buffer: int, index: int, count: int, stride: int, offset: int) uses unsafe
```

#### method `LayoutIndices`

```wolang
fun LayoutIndices(layout: int, buffer: int) uses unsafe
```

#### method `LayoutDelete`

```wolang
fun LayoutDelete(layout: int) uses unsafe
```

#### method `ProgramNew`

```wolang
fun ProgramNew(vertex: text, fragment: text) -> int uses unsafe
```

#### method `ProgramLog`

```wolang
fun ProgramLog() -> text uses unsafe
```

#### method `ProgramUse`

```wolang
fun ProgramUse(program: int) uses unsafe
```

#### method `ProgramDelete`

```wolang
fun ProgramDelete(program: int) uses unsafe
```

#### method `Uniform`

```wolang
fun Uniform(program: int, name: text) -> int uses unsafe
```

#### method `UniformFloat`

```wolang
fun UniformFloat(location: int, count: int, a: float, b: float, c: float, d: float) uses unsafe
```

#### method `UniformInt`

```wolang
fun UniformInt(location: int, value: int) uses unsafe
```

#### method `UniformMatrix`

```wolang
fun UniformMatrix(location: int, values: []f32) uses unsafe
```

#### method `TextureNew`

```wolang
fun TextureNew(width: int, height: int, rgba: []u8, linear: bool, repeat: bool) -> int uses unsafe
```

#### method `TextureUpdate`

```wolang
fun TextureUpdate(texture: int, x: int, y: int, width: int, height: int, rgba: []u8) uses unsafe
```

#### method `TextureBind`

```wolang
fun TextureBind(unit: int, texture: int) uses unsafe
```

#### method `TextureDelete`

```wolang
fun TextureDelete(texture: int) uses unsafe
```

#### method `TargetNew`

```wolang
fun TargetNew(texture: int) -> int uses unsafe
```

#### method `TargetBind`

```wolang
fun TargetBind(target: int) uses unsafe
```

#### method `TargetDelete`

```wolang
fun TargetDelete(target: int) uses unsafe
```

#### method `Draw`

```wolang
fun Draw(layout: int, mode: int, first: int, count: int) uses unsafe
```

#### method `DrawIndexed`

```wolang
fun DrawIndexed(layout: int, mode: int, count: int, first: int) uses unsafe
```

#### method `Read`

```wolang
fun Read(x: int, y: int, width: int, height: int, out: []mut u8) -> bool uses unsafe
```

#### method `Error`

```wolang
fun Error() -> int uses unsafe
```

### class `OpenGl`

```wolang
offer class OpenGl
```

#### field `version`

```wolang
var version: text = ""
```

### function `OpenGlStart`

```wolang
offer fun OpenGlStart(window: Window) -> Backend? uses unsafe
```

### class `Null`

```wolang
offer class Null
```

#### field `next`

```wolang
var next: int = 1
```

#### field `buffers`

```wolang
var buffers: List<int>
```

#### field `layouts`

```wolang
var layouts: List<int>
```

#### field `programs`

```wolang
var programs: List<int>
```

#### field `textures`

```wolang
var textures: List<int>
```

#### field `targets`

```wolang
var targets: List<int>
```

#### field `log`

```wolang
var log: List<text>
```

#### field `errors`

```wolang
var errors: int = 0
```

#### field `failure`

```wolang
var failure: text = ""
```

#### field `program_sources`

```wolang
var program_sources: List<text>
```

#### field `bound_target`

```wolang
var bound_target: int = 0
```

#### field `bound_program`

```wolang
var bound_program: int = 0
```

#### field `clear_red`

```wolang
var clear_red: float = 0.0
```

#### field `clear_green`

```wolang
var clear_green: float = 0.0
```

#### field `clear_blue`

```wolang
var clear_blue: float = 0.0
```

#### field `clear_alpha`

```wolang
var clear_alpha: float = 0.0
```

#### field `uniforms_set`

```wolang
var uniforms_set: int = 0
```

#### method `Record`

```wolang
fun Record(line: text) uses unsafe
```

#### method `Take`

```wolang
fun Take(list: List<int>) -> int uses unsafe
```

#### method `Live`

```wolang
fun Live(list: List<int>, id: int) -> bool
```

#### method `Release`

```wolang
fun Release(list: List<int>, id: int, what: text) uses unsafe
```

#### method `Need`

```wolang
fun Need(list: List<int>, id: int, what: text) uses unsafe
```

#### method `Count`

```wolang
fun Count(list: List<int>) -> int
```

### function `NullNew`

```wolang
offer fun NullNew() -> Null uses unsafe
```

### class `Device`

```wolang
offer class Device
```

#### field `backend`

```wolang
var backend: Backend
```

#### method `Name`

```wolang
fun Name() -> text
```

#### method `VertexBuffer`

```wolang
fun VertexBuffer(data: []f32, dynamic: bool) -> Buffer uses unsafe
```

#### method `IndexBuffer`

```wolang
fun IndexBuffer(data: []u32, dynamic: bool) -> Buffer uses unsafe
```

#### method `BufferDelete`

```wolang
fun BufferDelete(buffer: Buffer) uses unsafe
```

#### method `LayoutNew`

```wolang
fun LayoutNew() -> Layout uses unsafe
```

#### method `LayoutAttribute`

```wolang
fun LayoutAttribute(layout: Layout, buffer: Buffer, index: int, count: int, stride: int, offset: int) uses unsafe
```

#### method `LayoutIndices`

```wolang
fun LayoutIndices(layout: Layout, buffer: Buffer) uses unsafe
```

#### method `LayoutDelete`

```wolang
fun LayoutDelete(layout: Layout) uses unsafe
```

#### method `ProgramNew`

```wolang
fun ProgramNew(vertex: text, fragment: text) -> Program!(CompileFailed) uses unsafe
```

#### method `ProgramUse`

```wolang
fun ProgramUse(program: Program) uses unsafe
```

#### method `ProgramDelete`

```wolang
fun ProgramDelete(program: Program) uses unsafe
```

#### method `Uniform`

```wolang
fun Uniform(program: Program, name: text) -> int uses unsafe
```

#### method `SetFloat`

```wolang
fun SetFloat(location: int, a: float) uses unsafe
```

#### method `SetVec2`

```wolang
fun SetVec2(location: int, a: float, b: float) uses unsafe
```

#### method `SetVec3`

```wolang
fun SetVec3(location: int, a: float, b: float, c: float) uses unsafe
```

#### method `SetVec4`

```wolang
fun SetVec4(location: int, a: float, b: float, c: float, d: float) uses unsafe
```

#### method `SetInt`

```wolang
fun SetInt(location: int, value: int) uses unsafe
```

#### method `SetMatrix`

```wolang
fun SetMatrix(location: int, values: []f32) uses unsafe
```

#### method `TextureNew`

```wolang
fun TextureNew(width: int, height: int, rgba: []u8, linear: bool, repeat: bool) -> Texture uses unsafe
```

#### method `TextureUpdate`

```wolang
fun TextureUpdate(texture: Texture, x: int, y: int, width: int, height: int, rgba: []u8) uses unsafe
```

#### method `TextureBind`

```wolang
fun TextureBind(unit: int, texture: Texture) uses unsafe
```

#### method `TextureDelete`

```wolang
fun TextureDelete(texture: Texture) uses unsafe
```

#### method `TargetNew`

```wolang
fun TargetNew(texture: Texture) -> Target? uses unsafe
```

#### method `TargetDelete`

```wolang
fun TargetDelete(target: Target) uses unsafe
```

#### method `Begin`

```wolang
fun Begin(target: Target?, width: int, height: int, red: float, green: float, blue: float, alpha: float, clear_depth: bool) uses unsafe
```

#### method `End`

```wolang
fun End() uses unsafe
```

#### method `Blend`

```wolang
fun Blend(enabled: bool, source: int, destination: int) uses unsafe
```

#### method `Depth`

```wolang
fun Depth(test: bool, write: bool, compare: int) uses unsafe
```

#### method `Cull`

```wolang
fun Cull(mode: int) uses unsafe
```

#### method `Scissor`

```wolang
fun Scissor(enabled: bool, x: int, y: int, width: int, height: int) uses unsafe
```

#### method `Draw`

```wolang
fun Draw(layout: Layout, mode: int, first: int, count: int) uses unsafe
```

#### method `DrawIndexed`

```wolang
fun DrawIndexed(layout: Layout, mode: int, count: int, first: int) uses unsafe
```

#### method `Read`

```wolang
fun Read(x: int, y: int, width: int, height: int) -> []mut u8 uses unsafe
```

#### method `Error`

```wolang
fun Error() -> int uses unsafe
```

### function `DeviceNew`

```wolang
offer fun DeviceNew(backend: Backend) -> Device
```
