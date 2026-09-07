# Data types and literals

[← Back to index](README.md)

## Scalar types

From [`data_type.h`](../Ceres-ASM/src/assembler/data_type.h):

| Type | Size | Alignment | Notes |
| --- | --- | --- | --- |
| `u8` | 1 byte | 1 | Unsigned 8-bit. |
| `u16` | 2 bytes | 2 | Unsigned 16-bit. |
| `u32` | 4 bytes | 4 | Unsigned 32-bit. |
| `i8` | 1 byte | 1 | Signed 8-bit. |
| `i16` | 2 bytes | 2 | Signed 16-bit. |
| `i32` | 4 bytes | 4 | Signed 32-bit. |
| `f32` | 4 bytes | 4 | IEEE-754 single-precision float. |

## Aliases

Several more names are aliases rather than distinct types. An alias **is** the type it stands for —
a `ptr` loads with `ldr` like any `u32` — but the spelling is kept rather than thrown away at parse
time, so diagnostics name what you wrote:

| Alias | Resolves to | For |
| --- | --- | --- |
| `char` | `u8` | A character |
| `bool` | `u8` | `true` or `false`, and nothing else |
| `string` | `u8[]` | An unsized run of characters with a terminating zero |
| `ptr` | `u32` | A memory address |
| `port` | `u8` | An I/O port number |
| `irq` | `u8` | An interrupt vector number, `0`–`63` |
| `byte` | `u8` | The machine's own vocabulary, so a declaration reads like the |
| `half` | `u16` | `ldrb`/`ldrh`/`ldr` that uses it |
| `word` | `u32` | |

### Aliases that promise a range

Three of them promise something the underlying scalar does not, and the assembler holds them to it:

```casm
let vector:  irq  = 16      // OK
let vector:  irq  = 99      // ERROR: a value of 99 does not fit in a 'irq': the largest is 63
let flag:    bool = true    // OK
let flag:    bool = 5       // ERROR: a value of 5 does not fit in a 'bool': the largest is 1
let channel: port = 0x01    // OK; a port is already capped at 255 by u8
```

The `irq` bound is 63 because the interrupt vector table has 64 entries (see
[Interrupts and exceptions](08-Interrupts-and-Exceptions.md)). Every element of an array of a bounded
alias is checked, not just the first.

### Aliases cost identifiers

Every alias is a word that variables, constants and labels can no longer use — `word`, `port` and
`half` are all plausible names for something. That is the reason the list stops where it does rather
than growing to cover every idea.

## Declaring variables and constants

```casm
const NAME = expression      // a compile-time constant; occupies no memory
let   NAME: type = value     // a variable with an explicit type and initial value
let   NAME: type             // a variable with an explicit type and no initializer (@bss only)
let   NAME = value            // a variable whose type is inferred from its initializer
```

`const` **requires** an initializer and never lives inside a section — see
[Constants and expressions](13-Constants-and-Expressions.md). `let` requires a section to be active
(`@rodata`, `@data` or `@bss`; never `@text`), and the exact combination of type/initializer that's
legal depends on which section it's in:

| Section | Requires an initializer? | Notes |
| --- | --- | --- |
| `@rodata` | **Yes**, always | Read-only after load; makes no sense uninitialized. |
| `@data` | No, but usually has one | Without an initializer the bytes start as whatever the loader leaves them as — in practice, zero, since the emitter doesn't special-case this, but don't rely on that; prefer `@bss` for "starts at zero" data. |
| `@bss` | **No — never** | An initializer here is a hard error: the whole point of `@bss` is that it occupies no space in the `.cres` file and is zero-filled at load time (see [The `.cres` binary format](09-CRES-Binary-Format.md)). |

## Arrays

```casm
let scores:  i16[4]  = [42, -10, 0x1A, 0b10]   // sized array, explicit element count
let buffer:  u32[128]                          // sized array, no initializer (@bss)
let active:  u8[MAX_PLAYERS]                   // sized array, count given by a constant identifier
let welcome: u8[]    = "Welcome to CeresVM!\n"  // unsized array, size inferred from the initializer
```

- A **sized array** (`type[N]`) declares its element count as a literal integer, a constant, or any
  constant expression (see [Constants and expressions](13-Constants-and-Expressions.md)). The size
  cannot be zero.
- An **unsized array** (`type[]`) leaves the count to the initializer.
- **A declaration may be longer than its initializer**; the remainder is zero. It may not be
  shorter — an initializer with more elements than the declaration holds is an error.
- Array element literals may mix any combination the target scalar type accepts (see below), but
  every element must resolve to that same scalar type; an identifier used as an element must itself
  be a constant of a matching scalar type.

## Multidimensional arrays

```casm
@data
    let grid:  i32[2][3] = [[1, 2, 3], [4, 5, 6]]
    let a:     i32[][]   = [[1, 2, 3], [4, 5, 6]]    // both sizes worked out: 2 by 3
    let b:     i32[2][]  = [[1, 2, 3], [4, 5, 6]]    // only the inner one
    let c:     i32[][3]  = [[1, 2], [4, 5, 6]]       // outer worked out; row 0 padded to 3
    let names: u8[][8]   = ["ada", "grace"]          // a string fills a whole row
    let flat:  i32[2][3] = [1, 2, 3, 4, 5, 6]        // every size written, so one flat list works

@bss
    let tiles: u8[16][16]
    let cube:  i16[4][4][4]
```

Dimensions are written outermost first and elements are stored **row-major**, so `i32[2][3]` is two
rows of three. The rank is capped at four.

### Which sizes may be omitted

**Any** dimension may be left empty, not just the outermost. A size is only an error when the
initializer cannot supply it:

| Situation | What happens |
| --- | --- |
| Dimension **declared** | It wins. A short row is padded with zeroes; a long one is an error. |
| Dimension **omitted** | Read off the initializer. Every row at that level must be the same length. |
| Nesting depth | Must equal the declared rank, unless every size is written and the initializer is flat. |
| **No initializer** | Nothing to read: every dimension needs a size. |
| Inferred to `0` | An error, like a written `[0]`. |

The asymmetry is the point. A **declared** dimension gives something to pad against, so an irregular
initializer is fine there. An **omitted** one does not, and the irregularity is precisely what makes
its size unknowable:

```casm
let a: i32[][3] = [[1, 2], [4, 5, 6]]   // OK: outer is 2, row 0 becomes [1, 2, 0]
let b: i32[][]  = [[1, 2], [3, 4, 5]]   // ERROR: cannot work out dimension 1:
                                        //        this level has rows of 2 and of 3 elements
let c: u8[][16]                         // ERROR: without an initialiser every dimension needs a size
```

### Recovering a size that was never written

An inferred dimension leaves its number nowhere in the source. `countof`, `sizeof` and `dimof` are
how you get it back — see
[Constants and expressions](13-Constants-and-Expressions.md#size-queries):

```casm
    li r1, dimof(grid, 0)   // 2   - the outer dimension
    li r2, dimof(grid, 1)   // 3   - the inner one
    li r3, countof(grid)    // 6   - total elements
    li r4, sizeof(grid)     // 24  - total bytes
```

### There is no indexing

`grid[1][2]` is not an operand. The language gives the *layout*; walking it is still arithmetic you
write yourself, which is what makes `dimof` worth having:

```casm
    la  r1, grid                // base
    li  r2, 1                   // row
    mul r2, r2, dimof(grid, 1)  // row * columns
    add r2, r2, 2               // + column
    mul r2, r2, 4               // * sizeof an i32
    add r1, r1, r2
    ldr r3, [r1]                // grid[1][2]
```

## String literals

```casm
let greeting: u8[16] = "Hello, CeresVM!"
```

A string literal is stored **null-terminated** — `"Hello, CeresVM!"` is 15 visible characters plus
one implicit `\0`, so it needs a `u8[16]` declaration (or `u8[]`, which infers 16 automatically). This
also means a string can only ever be declared with element type `u8` (or its `char`/`string`
aliases); using it to initialize an `i16[]` or similar is rejected.

A declaration longer than the string is padded with zeroes, so `u8[32] = "hi"` is legal and leaves 29
zero bytes after the terminator.

**Inside an array a string is one element, not many** — it fills a whole trailing dimension. That is
what makes a table of fixed-width strings expressible:

```casm
let names: u8[][8] = ["ada", "grace"]   // 2 rows of 8 bytes: "ada\0\0\0\0\0", "grace\0\0\0"
let bad:   u8[][]  = ["ada", "grace"]   // ERROR: rows of 4 and 6 bytes; declare the width
```

## Integer literals are untyped

An integer literal (`42`, `0x1A`, `0b10`, `-10`) carries **no width or signedness of its own** until
it's used somewhere that supplies one — a variable's declared scalar type, or the operand slot an
instruction expects. When context does supply a type, the literal is **re-tagged and range-checked**
against it:

```casm
let small: i8 = 200        // ERROR: 200 does not fit in a signed 8-bit value
let small: u8 = 200        // OK: 200 fits in an unsigned 8-bit value
let small: i8 = -10        // OK
```

The check (`LiteralScalar::coerceTo` / `fitsInBits`, in
[`literal_scalar.h`](../Ceres-ASM/src/assembler/literal_scalar.h)) accepts a value whenever
truncating it to the target width loses no information under *either* a signed or an unsigned
reading — which is exactly what lets `-10` be written where an `i16` is expected, and rejects
`70000` where a `u16` is expected. There is no implicit conversion between integers and `f32` in
either direction: an integer literal used where `f32` is expected (or vice versa) is a type error,
not a coercion.

## Alignment

Every scalar's *natural alignment* equals its size (1/2/4 bytes — see the table above). The
translation-unit builder pads the current section offset up to a variable's natural alignment before
recording its address, so the symbol table and the emitted bytes always agree on exactly where a
variable starts (see
[`TranslationUnitBuilder::alignCurrentOffset`](../Ceres-ASM/src/assembler/translation_unit.cpp)).
This padding is invisible in source — you never write it yourself — but it does mean the *size* of a
section can be a few bytes larger than the sum of its declarations' sizes.

Misaligned 16-/32-bit *memory accesses at run time* (not declarations) raise `AlignmentFault` — see
[Memory → Alignment](02-Memory.md#alignment) and
[Interrupts and exceptions](08-Interrupts-and-Exceptions.md).

## Boolean literals

`true` and `false` are literals of their own, and the natural way to initialise a `bool`:

```casm
let ready: bool = true
let done:  bool = false
```

## Constant expressions in literal contexts

```casm
const BLOCK  = 16 * 4         // 64
const MARGIN = BLOCK / 8      // constants may refer to earlier constants
let buf: u8[BLOCK * 2]        // an array size is a constant expression like any other
```

See [Constants and expressions](13-Constants-and-Expressions.md) for the full grammar, the size
queries, and why a constant can only refer to one declared above it.

## Related pages

- [Constants and expressions](13-Constants-and-Expressions.md) — `const`, arithmetic folding, limitations.
- [Labels and symbols](12-Labels-and-Symbols.md) — how a variable's declared address becomes a usable symbol.
- [Memory](02-Memory.md) — the alignment rules these types feed into.
- [Language syntax](10-Language-Syntax.md) — the literal grammar (escapes, number bases, etc.) in full.
