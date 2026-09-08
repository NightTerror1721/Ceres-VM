# Structs

[← Back to index](README.md)

```casm
struct Entity
    x:      i32
    y:      i32
    health: u16
    flags:  u8
endstruct
```

## What a struct actually is

A `struct` **reserves no storage and declares no type**. It is a generator of constants:

| Generated constant | Value |
| --- | --- |
| `Entity.x` | `0` |
| `Entity.y` | `4` |
| `Entity.health` | `8` |
| `Entity.flags` | `10` |
| `Entity` | `12` — the total size |

That is deliberate, and it is what makes the feature cheap: `[r1 + Entity.y]` is an ordinary
constant displacement, and `u8[32][Entity]` an ordinary array. Nothing below the parser has to learn
what a record is — not the symbol table, not the linker, not the emitter, not the VM.

The trade is that a struct is not a *type*: you cannot write `let player: Entity`. You allocate the
bytes and address them through the offsets.

## Allocating one

```casm
@bss
    let player: u8[Entity]          // one entity: 12 bytes
    let mobs:   u8[32][Entity]      // 32 of them: 384 bytes
```

`Entity` is a constant holding the size, so `u8[Entity]` is just an array of that many bytes, and
`u8[32][Entity]` is a two-dimensional one — 32 rows of 12 (see
[Data types and literals](11-Data-Types-and-Literals.md#multidimensional-arrays)).

## A struct name is a type

`Entity` in the place a type goes means `u8[Entity]`, and `Entity[4]` means `u8[4][Entity]` — the
struct is always the innermost dimension, and whatever brackets follow it are instance counts:

```casm
@bss
    let player: Entity              // 12 bytes, one Entity
    let squad:  Entity[4]           // four of them

@data
    let spawn:  Entity   = [10, 20, 100, 1]
    let mobs:   Entity[2] = [[1, 2, 3, 4], [5, 6, 7, 8]]
```

It is a spelling, not a type system. A struct still reserves no storage of its own, `Entity.y` is
still an ordinary constant, and nothing checks that the `r2` in `ldr r1, [r2 + Entity.y]` points at
an Entity. `u8[Entity]` keeps working and means the same thing.

Only a struct can be written this way. A constant cannot, because `let p: MAX_PLAYERS` would
otherwise quietly be four bytes of nothing in particular:

```
'MAX_PLAYERS' is not a struct, so it cannot be written as a type. For a byte array of that size,
write u8[MAX_PLAYERS]
```

## Fields that are structs

A field may be a struct, or an array of them, and its initialiser nests to match:

```casm
struct Point
    x: i32
    y: i32
endstruct

struct Poly
    corners: Point[3]
    tags:    u16[2]
    id:      u8
endstruct

@data
    let p: Poly = [[[1, 2], [3, 4], [5, 6]], [7, 8], 9]
    //             └─ corners ──────────────┘  └ tags ┘  └ id
```

Each nested group is read as **that struct's fields**, not as the bytes its type resolved to. This
is the one thing worth checking when a struct contains another: `[1, 2]` for a `Point` field is the
two `i32`s, and writing `[1, 2]` where a nested group was expected is an error rather than two bytes.

## Initializing one

A byte array dimensioned by a struct accepts a **positional** initializer: values map to fields
in declaration order, each checked against its field's type, with padding zero-filled:

```casm
@data
    let player: u8[Entity] = [10, 20, 100, 1]   // x, y, health, flags; trailing pad byte is 0
    let origin: u8[Entity] = [7]                // the rest zero-fills, like arrays
```

An array field takes a nested group, and an array of structs takes one group per instance:

```casm
struct Tile
    corners: i16[4]
    id:      u32
endstruct

@data
    let t:   u8[Tile]      = [[1, 2, 3, 4], 99]
    let pts: u8[2][Point]  = [[1, 2], [3, 4]]
```

An initialiser takes **at most one instance count**: `u8[2][Point]` is two Points, and
`u8[2][2][Point]` is refused rather than guessed at — nesting deeper would have to mean what it
means for an ordinary array (`[[a, b], [c, d]]`), and that is not what it did.

A **string is bytes**, not fields: `let s: u8[Entity] = "abc"` is the same `u8[N]` string it
would be without the struct, terminating zero and all.

Too many values is an error, as is a value that does not fit its field's type (`70000` where a
`u16` field is expected fails even though the storage is `u8` bytes). There is no nominal
`{field: value}` form — like `MASM`'s `<>` and `NASM`'s `istruc/at`, order is the contract.
`@bss` still forbids any initializer; `@rodata` still requires one.

## Reading and writing fields

```casm
@text
global main:
    la  r1, player
    ldr r2, [r1 + Entity.x]         // player.x
    ldr r3, [r1 + Entity.y]         // player.y
    ldrh r4, [r1 + Entity.health]   // a u16 field, so a halfword load
    ldrb r5, [r1 + Entity.flags]

    li  r6, 100
    strh r6, [r1 + Entity.health]   // player.health = 100
    ret
```

The load or store width is **yours to choose** — the offset constant says where the field is, not
how wide it is. Reading a `u16` field with `ldr` reads four bytes and picks up the next field with
it. This is the main thing to be careful about.

## Walking an array of them

```casm
alias base  = r1
alias index = r2
alias entry = r3

@text
    la  base, mobs
    li  index, 5

    mul entry, index, Entity        // 5 * 12
    add entry, base, entry          // &mobs[5]
    ldr r4, [entry + Entity.x]      // mobs[5].x
```

Multiplying the index by the struct's own name is the idiom: `Entity` *is* the stride.

## Alignment

Fields are laid out in declaration order, each padded up to its own natural alignment, and the total
is rounded up to the **widest** field's alignment so an array of them stays aligned:

```casm
struct Entity          //  offset
    x:      i32        //  0
    y:      i32        //  4
    health: u16        //  8
    flags:  u8         //  10
endstruct              //  11 bytes of fields, rounded up to 12 (widest field is 4-byte)
```

Declaring the wide fields first, as above, wastes the least. Interleaving them costs padding:

```casm
struct Wasteful
    flags:  u8         //  0
    x:      i32        //  4  -- three bytes of padding before it
    health: u16        //  8
endstruct              //  12 bytes, of which 3 are padding
```

## Array fields

A field may be an array, including a multidimensional one:

```casm
struct Tile
    corners: i16[4]    //  0, 8 bytes
    id:      u32       //  8
endstruct              //  12
```

## Visibility

`global struct` exports **all** of the constants it generates — the field offsets and the size:

```casm
// lib/entity.casm
global struct Entity
    x: i32
    y: i32
endstruct
```

```casm
import "lib/entity.casm"
    ldr r2, [r1 + Entity.y]
```

Without `global` the whole set stays inside the file that declares it, like any other constant (see
[Modules and import](15-Modules-and-Import.md)).

An imported struct can also be reached through a named import, which is how two libraries that both
declare an `Entity` stay usable in one file:

```casm
import "lib/entity.casm" as game
import "lib/gfx.casm"    as gfx

    ldr r2, [r1 + game.Entity.y]
```

## Unused fields are not warned about

A private declaration nothing names is normally reported (see
[Errors and diagnostics](17-Errors-and-Diagnostics.md#warnings)). Struct field offsets are exempt:
they are generated rather than written, and warning about each unused field of a record would drown
the warnings that matter.

## Related pages

- [Data types and literals](11-Data-Types-and-Literals.md) — the field types, and the arrays a struct is allocated through.
- [Constants and expressions](13-Constants-and-Expressions.md) — what the generated offsets are, and `sizeof`.
- [Instruction set → Memory access](05-Instruction-Set.md#memory-access--0x400x4e) — the load and store widths to address fields with.
- [Modules and import](15-Modules-and-Import.md) — `global struct`, and reaching one through a named import.
