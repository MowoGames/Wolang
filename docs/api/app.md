# app 0.1.0

## app

### constant `kStepsPerFrameLimit`

```wolang
offer const kStepsPerFrameLimit: int = 8
```

### struct `Config`

```wolang
offer struct Config
```

#### field `title`

```wolang
var title: text
```

#### field `width`

```wolang
var width: int
```

#### field `height`

```wolang
var height: int
```

#### field `resizable`

```wolang
var resizable: bool
```

#### field `vsync`

```wolang
var vsync: bool
```

#### field `gl`

```wolang
var gl: bool
```

#### field `step`

```wolang
var step: float
```

#### field `max_frame`

```wolang
var max_frame: float
```

#### field `sample_rate`

```wolang
var sample_rate: int
```

#### field `sound`

```wolang
var sound: bool
```

#### field `hidden`

```wolang
var hidden: bool
```

### function `ConfigNew`

```wolang
offer fun ConfigNew(title: text, width: int, height: int) -> Config
```

### function `ConfigParse`

```wolang
offer fun ConfigParse(input: text, base: Config) -> Config uses unsafe
```

### function `ConfigText`

```wolang
offer fun ConfigText(config: Config) -> text
```

### function `ConfigDirectory`

```wolang
offer fun ConfigDirectory(name: text) -> text uses io, unsafe
```

### function `ConfigPath`

```wolang
offer fun ConfigPath(name: text) -> text uses io, unsafe
```

### class `Clock`

```wolang
offer class Clock
```

#### field `step`

```wolang
var step: float
```

#### field `max_frame`

```wolang
var max_frame: float
```

#### field `accumulator`

```wolang
var accumulator: float = 0.0
```

#### field `last`

```wolang
var last: float = 0.0
```

#### field `frames`

```wolang
var frames: int = 0
```

#### field `steps`

```wolang
var steps: int = 0
```

#### method `Advance`

```wolang
fun Advance(frame: float) -> int
```

#### method `Delta`

```wolang
fun Delta() -> float
```

#### method `Alpha`

```wolang
fun Alpha() -> float
```

### function `ClockNew`

```wolang
offer fun ClockNew(step: float, max_frame: float) -> Clock
```

### ability `Game`

```wolang
offer ability Game
```

#### method `Update`

```wolang
fun Update(dt: float) uses unsafe
```

#### method `Draw`

```wolang
fun Draw(alpha: float) uses unsafe
```

#### method `OnEvent`

```wolang
fun OnEvent(event: Event) -> bool uses unsafe
```

### class `App`

```wolang
offer class App
```

#### field `config`

```wolang
var config: Config
```

#### field `window`

```wolang
var window: Window
```

#### field `output`

```wolang
var output: Output
```

#### field `device`

```wolang
var device: Device
```

#### field `clock`

```wolang
var clock: Clock
```

#### field `running`

```wolang
var running: bool = true
```

#### method `HasWindow`

```wolang
fun HasWindow() -> bool
```

#### method `HasSound`

```wolang
fun HasSound() -> bool
```

#### method `Quit`

```wolang
fun Quit()
```

#### method `Frame`

```wolang
fun Frame(game: Game, frame: float) uses unsafe
```

#### method `RunFrames`

```wolang
fun RunFrames(game: Game, frames: int, frame: float) -> int uses unsafe
```

#### method `Run`

```wolang
fun Run(game: Game) -> int uses unsafe
```

#### method `Close`

```wolang
fun Close() uses unsafe
```

### function `AppNew`

```wolang
offer fun AppNew(config: Config) -> App!(Malformed) uses unsafe
```
