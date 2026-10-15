# core

## core

### enum `Ordering`

```wolang
offer enum Ordering
```

#### method `Less`

```wolang
Less
```

#### method `Equal`

```wolang
Equal
```

#### method `Greater`

```wolang
Greater
```

### function `CompareInt`

```wolang
offer fun CompareInt(a: int, b: int) -> Ordering
```

### function `MinInt`

```wolang
offer fun MinInt(a: int, b: int) -> int
```

### function `MaxInt`

```wolang
offer fun MaxInt(a: int, b: int) -> int
```

### function `ClampInt`

```wolang
offer fun ClampInt(value: int, low: int, high: int) -> int
```

Keeps `value` within `low` and `high`, inclusive.
