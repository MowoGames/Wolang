# math 0.1.0

## math

### struct `Vec2`

```wolang
offer struct Vec2
```

#### field `x`

```wolang
var x: float
```

#### field `y`

```wolang
var y: float
```

#### method `Dot`

```wolang
fun Dot(other: Vec2) -> float =>
```

#### method `LengthSquared`

```wolang
fun LengthSquared() -> float =>
```

#### method `Length`

```wolang
fun Length() -> float uses unsafe
```

#### method `Normalized`

```wolang
fun Normalized() -> Vec2 uses unsafe
```

#### method `Scaled`

```wolang
fun Scaled(k: float) -> Vec2 =>
```

#### method `Divided`

```wolang
fun Divided(k: float) -> Vec2 =>
```

#### method `Distance`

```wolang
fun Distance(other: Vec2) -> float uses unsafe
```

#### method `Lerped`

```wolang
fun Lerped(other: Vec2, t: float) -> Vec2 =>
```

#### method `Clamped`

```wolang
fun Clamped(low: Vec2, high: Vec2) -> Vec2 =>
```

#### method `NearTo`

```wolang
fun NearTo(other: Vec2, tolerance: float) -> bool =>
```

### struct `Vec3`

```wolang
offer struct Vec3
```

#### field `x`

```wolang
var x: float
```

#### field `y`

```wolang
var y: float
```

#### field `z`

```wolang
var z: float
```

#### method `Dot`

```wolang
fun Dot(other: Vec3) -> float =>
```

#### method `Cross`

```wolang
fun Cross(other: Vec3) -> Vec3 =>
```

#### method `LengthSquared`

```wolang
fun LengthSquared() -> float =>
```

#### method `Length`

```wolang
fun Length() -> float uses unsafe
```

#### method `Normalized`

```wolang
fun Normalized() -> Vec3 uses unsafe
```

#### method `Scaled`

```wolang
fun Scaled(k: float) -> Vec3 =>
```

#### method `Divided`

```wolang
fun Divided(k: float) -> Vec3 =>
```

#### method `Distance`

```wolang
fun Distance(other: Vec3) -> float uses unsafe
```

#### method `Lerped`

```wolang
fun Lerped(other: Vec3, t: float) -> Vec3 =>
```

#### method `Clamped`

```wolang
fun Clamped(low: Vec3, high: Vec3) -> Vec3 =>
```

#### method `NearTo`

```wolang
fun NearTo(other: Vec3, tolerance: float) -> bool =>
```

### struct `Vec4`

```wolang
offer struct Vec4
```

#### field `x`

```wolang
var x: float
```

#### field `y`

```wolang
var y: float
```

#### field `z`

```wolang
var z: float
```

#### field `w`

```wolang
var w: float
```

#### method `Dot`

```wolang
fun Dot(other: Vec4) -> float =>
```

#### method `LengthSquared`

```wolang
fun LengthSquared() -> float =>
```

#### method `Length`

```wolang
fun Length() -> float uses unsafe
```

#### method `Normalized`

```wolang
fun Normalized() -> Vec4 uses unsafe
```

#### method `Scaled`

```wolang
fun Scaled(k: float) -> Vec4 =>
```

#### method `Divided`

```wolang
fun Divided(k: float) -> Vec4 =>
```

#### method `Lerped`

```wolang
fun Lerped(other: Vec4, t: float) -> Vec4 =>
```

#### method `NearTo`

```wolang
fun NearTo(other: Vec4, tolerance: float) -> bool =>
```

### struct `Mat3`

```wolang
offer struct Mat3
```

#### field `m00`

```wolang
var m00: float
```

#### field `m01`

```wolang
var m01: float
```

#### field `m02`

```wolang
var m02: float
```

#### field `m10`

```wolang
var m10: float
```

#### field `m11`

```wolang
var m11: float
```

#### field `m12`

```wolang
var m12: float
```

#### field `m20`

```wolang
var m20: float
```

#### field `m21`

```wolang
var m21: float
```

#### field `m22`

```wolang
var m22: float
```

#### method `Transform`

```wolang
fun Transform(v: Vec3) -> Vec3 =>
```

#### method `Transposed`

```wolang
fun Transposed() -> Mat3 =>
```

#### method `Determinant`

```wolang
fun Determinant() -> float =>
```

#### method `NearTo`

```wolang
fun NearTo(other: Mat3, tolerance: float) -> bool =>
```

### function `Mat3Identity`

```wolang
offer fun Mat3Identity() -> Mat3 =>
```

### struct `Mat4`

```wolang
offer struct Mat4
```

#### field `m00`

```wolang
var m00: float
```

#### field `m01`

```wolang
var m01: float
```

#### field `m02`

```wolang
var m02: float
```

#### field `m03`

```wolang
var m03: float
```

#### field `m10`

```wolang
var m10: float
```

#### field `m11`

```wolang
var m11: float
```

#### field `m12`

```wolang
var m12: float
```

#### field `m13`

```wolang
var m13: float
```

#### field `m20`

```wolang
var m20: float
```

#### field `m21`

```wolang
var m21: float
```

#### field `m22`

```wolang
var m22: float
```

#### field `m23`

```wolang
var m23: float
```

#### field `m30`

```wolang
var m30: float
```

#### field `m31`

```wolang
var m31: float
```

#### field `m32`

```wolang
var m32: float
```

#### field `m33`

```wolang
var m33: float
```

#### method `TransformPoint`

```wolang
fun TransformPoint(p: Vec3) -> Vec3 =>
```

#### method `TransformDirection`

```wolang
fun TransformDirection(d: Vec3) -> Vec3 =>
```

#### method `TransformVec4`

```wolang
fun TransformVec4(v: Vec4) -> Vec4 =>
```

#### method `Transposed`

```wolang
fun Transposed() -> Mat4 =>
```

#### method `Upper`

```wolang
fun Upper() -> Mat3 =>
```

#### method `NearTo`

```wolang
fun NearTo(other: Mat4, tolerance: float) -> bool =>
```

### function `Mat4Identity`

```wolang
offer fun Mat4Identity() -> Mat4 =>
```

### function `Mat4Translation`

```wolang
offer fun Mat4Translation(by: Vec3) -> Mat4 =>
```

### function `Mat4Scale`

```wolang
offer fun Mat4Scale(by: Vec3) -> Mat4 =>
```

### function `Mat4FromMat3`

```wolang
offer fun Mat4FromMat3(m: Mat3) -> Mat4 =>
```

### struct `Quat`

```wolang
offer struct Quat
```

#### field `x`

```wolang
var x: float
```

#### field `y`

```wolang
var y: float
```

#### field `z`

```wolang
var z: float
```

#### field `w`

```wolang
var w: float
```

#### method `Dot`

```wolang
fun Dot(other: Quat) -> float =>
```

#### method `Length`

```wolang
fun Length() -> float uses unsafe
```

#### method `Normalized`

```wolang
fun Normalized() -> Quat uses unsafe
```

#### method `Conjugate`

```wolang
fun Conjugate() -> Quat =>
```

#### method `Rotate`

```wolang
fun Rotate(v: Vec3) -> Vec3
```

#### method `Slerp`

```wolang
fun Slerp(other: Quat, t: float) -> Quat uses unsafe
```

#### method `ToMat3`

```wolang
fun ToMat3() -> Mat3
```

#### method `ToMat4`

```wolang
fun ToMat4() -> Mat4 =>
```

#### method `NearTo`

```wolang
fun NearTo(other: Quat, tolerance: float) -> bool =>
```

### function `QuatIdentity`

```wolang
offer fun QuatIdentity() -> Quat =>
```

### function `QuatAxisAngle`

```wolang
offer fun QuatAxisAngle(axis: Vec3, radians: float) -> Quat uses unsafe
```
