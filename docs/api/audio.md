# audio 0.1.0

## audio

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

### constant `kMaxSamples`

```wolang
offer const kMaxSamples: int = 134217728
```

### constant `kPi`

```wolang
offer const kPi: float = 3.14159265358979
```

### class `Clip`

```wolang
offer class Clip
```

#### field `samples`

```wolang
var samples: []mut f32
```

#### field `rate`

```wolang
var rate: int = 48000
```

#### field `channels`

```wolang
var channels: int = 1
```

#### method `deinit`

```wolang
deinit
```

#### method `Frames`

```wolang
fun Frames() -> int
```

#### method `Seconds`

```wolang
fun Seconds() -> float
```

#### method `Get`

```wolang
fun Get(frame: int, channel: int) -> float
```

#### method `Set`

```wolang
fun Set(frame: int, channel: int, value: float)
```

### function `ClipNew`

```wolang
offer fun ClipNew(frames: int, channels: int, rate: int) -> Clip!(Malformed, TooLarge) uses unsafe
```

### function `WavDecode`

```wolang
offer fun WavDecode(data: []u8) -> Clip!(Malformed, Unsupported, TooLarge) uses unsafe
```

### function `WavEncode`

```wolang
offer fun WavEncode(clip: Clip, bits: int) -> []u8!(Unsupported) uses unsafe
```

### function `Resample`

```wolang
offer fun Resample(clip: Clip, rate: int) -> Clip!(Malformed, TooLarge) uses unsafe
```

### function `Remix`

```wolang
offer fun Remix(clip: Clip, channels: int) -> Clip!(Malformed, TooLarge) uses unsafe
```

### class `Voice`

```wolang
offer class Voice
```

#### field `clip`

```wolang
var clip: Clip
```

#### field `position`

```wolang
var position: f64 = 0.0
```

#### field `volume`

```wolang
var volume: float = 1.0
```

#### field `pan`

```wolang
var pan: float = 0.0
```

#### field `pitch`

```wolang
var pitch: float = 1.0
```

#### field `looped`

```wolang
var looped: bool = false
```

#### field `bus`

```wolang
var bus: int = 0
```

#### field `id`

```wolang
var id: int = 0
```

#### field `done`

```wolang
var done: bool = false
```

### class `Mixer`

```wolang
offer class Mixer
```

#### field `rate`

```wolang
var rate: int
```

#### field `channels`

```wolang
var channels: int
```

#### field `voices`

```wolang
var voices: List<Voice>
```

#### field `buses`

```wolang
var buses: List<float>
```

#### field `master`

```wolang
var master: float = 1.0
```

#### field `next_id`

```wolang
var next_id: int = 1
```

#### method `Voices`

```wolang
fun Voices() -> int
```

#### method `Play`

```wolang
fun Play(clip: Clip, volume: float, pan: float, looped: bool) -> int uses unsafe
```

#### method `Find`

```wolang
fun Find(id: int) -> Voice?
```

#### method `IsPlaying`

```wolang
fun IsPlaying(id: int) -> bool
```

#### method `Stop`

```wolang
fun Stop(id: int)
```

#### method `StopAll`

```wolang
fun StopAll()
```

#### method `SetVolume`

```wolang
fun SetVolume(id: int, volume: float)
```

#### method `SetPan`

```wolang
fun SetPan(id: int, pan: float)
```

#### method `SetPitch`

```wolang
fun SetPitch(id: int, pitch: float)
```

#### method `SetBus`

```wolang
fun SetBus(id: int, bus: int)
```

#### method `BusNew`

```wolang
fun BusNew(gain: float) -> int uses unsafe
```

#### method `SetBusGain`

```wolang
fun SetBusGain(bus: int, gain: float)
```

#### method `BusGain`

```wolang
fun BusGain(bus: int) -> float
```

#### method `SetMaster`

```wolang
fun SetMaster(gain: float)
```

#### method `Mix`

```wolang
fun Mix(voice: Voice, out: []mut f32, frames: int) uses unsafe
```

#### method `Render`

```wolang
fun Render(out: []mut f32) uses unsafe
```

### function `MixerNew`

```wolang
offer fun MixerNew(rate: int, channels: int) -> Mixer!(Malformed) uses unsafe
```

### function `DeviceAvailable`

```wolang
offer fun DeviceAvailable() -> bool uses unsafe
```

### class `Output`

```wolang
offer class Output
```

#### field `device`

```wolang
var device: Audio
```

#### field `mixer`

```wolang
var mixer: Mixer
```

#### method `IsOpen`

```wolang
fun IsOpen() -> bool
```

#### method `Pump`

```wolang
fun Pump() -> int uses unsafe
```

#### method `Close`

```wolang
fun Close() uses unsafe
```

### function `OutputOpen`

```wolang
offer fun OutputOpen(rate: int, channels: int) -> Output!(Malformed) uses unsafe
```

### class `VorbisInfo`

```wolang
offer class VorbisInfo
```

#### field `channels`

```wolang
var channels: int = 0
```

#### field `rate`

```wolang
var rate: int = 0
```

#### field `bitrate`

```wolang
var bitrate: int = 0
```

#### field `frames`

```wolang
var frames: int = 0
```

#### field `vendor`

```wolang
var vendor: text = ""
```

#### field `comments`

```wolang
var comments: List<text>
```

#### method `Seconds`

```wolang
fun Seconds() -> float
```

#### method `Comment`

```wolang
fun Comment(key: text) -> text? uses unsafe
```

### function `OggVorbisInfo`

```wolang
offer fun OggVorbisInfo(data: []u8) -> VorbisInfo!(Malformed, Unsupported) uses unsafe
```
