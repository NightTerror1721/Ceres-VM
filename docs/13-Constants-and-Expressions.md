# Constants and constant expressions

[← Back to index](README.md)

## Declaring a constant

```casm
const MAX_PLAYERS = 4
const BLOCK        = 16 * 4        // 64
const SYS_CTRL      = 0xFF
```

`const` always requires an initializer (`Parser::parseDataDeclaration` raises "Expected '=' and
initializer for constant data declaration" otherwise) and never occupies memory — there's no address
associated with a constant symbol, only a resolved value stored directly in the symbol table (see
[Labels and symbols](12-Labels-and-Symbols.md)). A constant can be declared with no section active at
all, unlike `let`.

A constant is substituted at every point it's used — anywhere an identifier operand or an array size
resolves to a constant, the constant's value is what ends up in the final encoding, not a reference
to the constant itself.

## What can appear on the right-hand side

A constant expression is kept as a **tree** by the parser and evaluated later, once the symbol table
exists (`ConstExpr` in [`const_expr.h`](../Ceres/libs/asm/include/ceres/asm/const_expr.h), evaluated by
[`const_expr_eval.cpp`](../Ceres/libs/asm/src/const_expr_eval.cpp)). That is what lets one
constant refer to another: folding on the spot, as the parser used to, only ever works for literals,
because at parse time no constant has a value yet.

The grammar, with the usual precedence:

```
expression := term (('+' | '-') term)*
term       := factor (('*' | '/') factor)*
factor     := '-' factor | '+' factor
            | '(' expression ')'
            | integer-literal | float-literal | char-literal | bool-literal
            | identifier
            | 'sizeof' '(' identifier ')'
            | 'countof' '(' identifier ')'
            | 'dimof' '(' identifier ',' integer-literal ')'
```

```casm
const A = 2 + 3 * 4          // 14 -- '*' binds tighter than '+'
const B = (2 + 3) * 4        // 20 -- parentheses group
const C = -5 + 2             // -3
const D = 10 / 3             // 3  -- integer division, signed, truncates toward zero
const E = 10 / 0             // ERROR: "Division by zero in constant expression"
```

Integer arithmetic is done through `i32`, matching how the VM would compute the same operation at
run time. A float on either side makes the whole operation floating point, so `PI * 2` stays a float
instead of being truncated on the way through.

## One constant from another

```casm
const BLOCK   = 64
const HEADER  = 8
const PAYLOAD = BLOCK - HEADER      // 56
const DOUBLE  = PAYLOAD * 2         // 112
```

A constant is evaluated **when its own declaration is reached**, in source order. That has one
consequence worth stating plainly: a constant can only refer to one declared *above* it.

```casm
const A = B + 1     // ERROR: 'B' is not declared
const B = 2
```

It is also why cycles are impossible rather than something the assembler has to detect — by the time
a name can be referred to, it already has a value.

## Size queries

Three functions ask the symbol table about a declared variable:

| Written | Gives |
| --- | --- |
| `sizeof(name)` | The total number of **bytes** the variable occupies |
| `countof(name)` | The total number of **scalar elements** |
| `dimof(name, n)` | The length of dimension `n`, counted from the outside in |

```casm
@bss
    let values: u32[5]
    let grid:   i32[2][3]

@text
    li r1, countof(values)   // 5
    li r2, sizeof(values)    // 20
    li r3, dimof(grid, 0)    // 2
    li r4, dimof(grid, 1)    // 3
    li r5, countof(grid)     // 6
```

They exist because a multidimensional array whose sizes were **inferred** leaves those numbers
written nowhere in the source (see
[Data types and literals](11-Data-Types-and-Literals.md#multidimensional-arrays)) — `dimof` is the
only way to get one back. `dimof` on a dimension the type does not have is an error naming the type,
and the index is a literal, not an expression.

They also keep a program from repeating a size it already stated:

```casm
@bss
    let buffer: u8[256]

@text
    li  r2, sizeof(buffer)   // says 256 once, in the declaration
    la  r13, DISK_BLOCK_LEN
    str [r13 + 0], r2
```

## Where a constant expression is legal

Anywhere a value is expected, which after the expression work means rather more places than before:

```casm
const N = 3

@bss
    let buf:  u8[N * 16]        // array sizes
    let grid: i32[N][N + 1]     // any dimension of a multidimensional one

@text
    li r1, 2 + N * 4            // immediate operands
    li r2, sizeof(buf)
    la r3, 0x10000 + N          // including a full 32-bit one
    ifge r4, N * 2, .enough     // the immediate of an ifXX
```

An expression that names nothing is folded as it is parsed; one that names a constant or asks a size
query travels as far as the symbol table and is replaced there.

**Memory operands are the exception.** `[base + offset]` accepts a single literal, a single constant,
or a qualified name like `Entity.y` — but not compound arithmetic:

```casm
    ldr r1, [r2 + OFFSET]        // OK
    ldr r1, [r2 + Entity.y]      // OK -- a struct field offset is a constant
    ldr r1, [r2 + 4 + 4]         // NOT supported
```

## Constants and visibility

A constant is private to the file that declares it unless it carries `global`:

```casm
global const MAX_PLAYERS = 4    // visible to any file importing this one
const INTERNAL_SLACK    = 8     // private
```

A **global constant is deliberately not published to the linker's global symbol table** — it occupies
no memory and is substituted at its point of use, so it has nothing to link. It travels by `import`
instead, which is why two independent libraries can each declare `global const MAX` without
colliding; the clash is only reported if some file imports both and uses the name. See
[Modules and import](15-Modules-and-Import.md#when-two-modules-export-the-same-name).

A private constant that nothing in its own file names is reported as a warning — it cannot be reached
from anywhere else, so it is dead with certainty. See
[Errors and diagnostics](17-Errors-and-Diagnostics.md#warnings).

## Related pages

- [Data types and literals](11-Data-Types-and-Literals.md) — how a literal is range-checked against the type context a constant is used in.
- [Labels and symbols](12-Labels-and-Symbols.md) — constants share the same symbol table as labels and variables.
- [Modules and import](15-Modules-and-Import.md) — how a `global const` reaches another file.
- [Structs](23-Structs.md) — field offsets are generated constants, and use this same grammar.
