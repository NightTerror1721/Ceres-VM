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

Three more names are aliases rather than distinct types:

| Alias | Resolves to |
| --- | --- |
| `char` | `u8` |
| `bool` | `u8` |
| `string` | An **unsized array of `u8`** (`u8[]`) |

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

- A **sized array** (`type[N]`) declares its element count as either a literal integer or a constant
  identifier. The array size cannot be zero.
- An **unsized array** (`type[]`) has no declared count and *must* have an initializer — the assembler
  infers the size from how many elements (or how many bytes, for a string) the initializer actually
  provides. An unsized array with no initializer is a hard error (there would be no way to know its
  size).
- **An initializer must fill the declaration exactly.** A sized array's initializer must supply
  exactly as many elements as declared — not fewer, not more.
- Array element literals may mix any combination the target scalar type accepts (see below), but
  every element must resolve to that same scalar type; an identifier used as an element must itself
  be a constant of a matching scalar type.

## String literals

```casm
let greeting: u8[16] = "Hello, CeresVM!"
```

A string literal is stored **null-terminated** — `"Hello, CeresVM!"` is 15 visible characters plus
one implicit `\0`, so it needs a `u8[16]` declaration (or `u8[]`, which infers 16 automatically). This
also means a string can only ever be declared with element type `u8` (or its `char`/`string`
aliases); using it to initialize an `i16[]` or similar is rejected.

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

## Constant expressions in literal contexts

```casm
const BLOCK = 16 * 4          // 64, folded at parse time
let buf: u8[BLOCK]            // array size given by a constant
```

See [Constants and expressions](13-Constants-and-Expressions.md) for exactly which expressions are
foldable and which are not (notably: an expression referencing another *identifier*, beyond using it
standalone, is not supported yet).

## Related pages

- [Constants and expressions](13-Constants-and-Expressions.md) — `const`, arithmetic folding, limitations.
- [Labels and symbols](12-Labels-and-Symbols.md) — how a variable's declared address becomes a usable symbol.
- [Memory](02-Memory.md) — the alignment rules these types feed into.
- [Language syntax](10-Language-Syntax.md) — the literal grammar (escapes, number bases, etc.) in full.
