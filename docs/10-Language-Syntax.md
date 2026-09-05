# Language syntax

[← Back to index](README.md)

This page covers the lexical and statement-level grammar of `.casm`, as implemented by
[`lexer.cpp`](../Ceres-ASM/src/assembler/lexer.cpp) and
[`parser.cpp`](../Ceres-ASM/src/assembler/parser.cpp).

## Lines and statements

A `.casm` file is a sequence of lines; each non-empty line is exactly one statement, one of:

- A section declaration (`@text`, `@rodata`, `@data`, `@bss`)
- A label declaration (`name:`, `global name:`, `.name:`)
- A data declaration (`let ...` / `const ...`)
- An import (`import "..."`)
- A macro declaration (`macro ... endmacro`, spanning multiple lines)
- A macro label (`%%name:`, only valid inside a macro body)
- An instruction or a macro call (an identifier followed by zero or more comma-separated operands)

The parser requires each statement to end at a newline or end-of-file; there is no statement
separator for multiple statements on one line.

## Comments

```casm
// a line comment, runs to the end of the line
/* a block comment,
   can span multiple lines */
```

Both forms are stripped by the lexer before tokenizing; there's no nesting for block comments.

## Identifiers and keywords

An identifier starts with a letter or underscore and continues with letters, digits or underscores.
Six words are reserved keywords and cannot be used as identifiers: `let`, `const`, `global`,
`import`, `macro`, `endmacro`.

Two special identifier forms exist for macros only (see [Macros](14-Macros.md)):

- `$name` — a macro parameter reference.
- `%%name` — a macro-local label, hygienically renamed on every expansion.

## Numeric literals

```casm
42          // decimal
-10         // signed decimal
0x1A        // hexadecimal
0b1010      // binary
3.14159     // float (requires a decimal point or an exponent)
-9.81       // signed float
1.5e3       // float with exponent
```

An integer literal has no type of its own — it's untyped until context (a declared variable type, an
expected operand width) gives it one; see
[Data types and literals → Integer literals are untyped](11-Data-Types-and-Literals.md#integer-literals-are-untyped).
Only base-10 literals may have a fractional part or exponent; hex/binary literals are always
integers.

## Character and string literals

```casm
'A'          // a character literal, u8-valued
'\n' '\t' '\r' '\\' '\'' '\"' '\0'   // recognized escapes
'\x41'       // exactly two hex digits after \x -> 'A'

"Hello, CeresVM!"    // a string literal
"line1\nline2"       // escapes work the same as in char literals
```

A string literal cannot contain a literal, unescaped newline — write `\n` instead. Every string
literal is stored **null-terminated**: a declaration's array size must include room for that
trailing zero (see [Data types and literals](11-Data-Types-and-Literals.md)).

## Sections

```casm
@text     // executable instructions
@rodata   // immutable data (string literals, constant tables, …)
@data     // initialized mutable data
@bss      // zero-filled at load time; occupies no space in the .cres file
```

A section declaration switches which section subsequent labels/data/instructions belong to, for the
rest of the file (or until the next `@...` line). Instructions are only legal inside `@text`; a
variable `let` (non-`const`) is only legal inside `@rodata`, `@data` or `@bss` — never `@text`.
`const` declarations are the only kind of `let`/`const` statement legal with no section active at
all, since a constant occupies no memory.

## Labels

```casm
global main:      // exported across translation units; `main` doubles as the required entry point
helper:           // visible within this file only
.loop:             // local to the nearest preceding non-local label
```

See [Labels and symbols](12-Labels-and-Symbols.md) for the full scoping rules.

## Addressing (memory operands)

```casm
ldr r1, [r2]              // base register, no offset
ldr r1, [r2 + 4]           // base + literal offset
ldr r1, [r2 - 8]           // base + negative offset
ldr r1, [r2 + OFFSET]      // base + a constant identifier
```

The base register must be a general-purpose integer register — a floating-point register (`f0`–`f15`)
is never valid as a memory base, only as the value being loaded into/stored from. **The `+`/`-` and
the surrounding whitespace are mandatory**: `[r5+0]` is lexed as the register token followed by the
*signed number literal* `+0`, which the memory-operand grammar does not accept in that position (it
expects `+`/`-` as its own punctuation token, then a separate offset). Always write `[r5 + 0]`.

## Instruction and macro-call syntax

```casm
mnemonic
mnemonic operand
mnemonic operand1, operand2
mnemonic operand1, operand2, operand3
```

Any identifier at the start of a statement that **is not** a recognized mnemonic (see
[`mnemonic.h`](../Ceres-ASM/src/assembler/mnemonic.h)) is parsed as a **macro call** instead — this
is also how a misspelled instruction name gets caught: it fails later, during macro expansion, with
"Unknown mnemonic or macro '...'", rather than silently doing nothing.

## Operand kinds the parser recognizes

| Syntax | Parsed as |
| --- | --- |
| `r0`–`r15`, `sp`, `fp`, `lr` | Integer register |
| `f0`–`f15` | Floating-point register |
| `42`, `0x2A`, `'A'` | Immediate value (constant-folded — see [Constants and expressions](13-Constants-and-Expressions.md)) |
| `[reg]`, `[reg + N]`, `[reg + ident]` | Memory operand |
| `label_name`, `.local_label` | Identifier (resolved to a label/variable/constant address later) |
| `$param` | Macro parameter (only valid inside a macro body) |
| `%%label` | Macro-local label reference (only valid inside a macro body) |

## Related pages

- [Data types and literals](11-Data-Types-and-Literals.md) — the full literal/type grammar for `let`/`const`.
- [Labels and symbols](12-Labels-and-Symbols.md) — global/file/local label scoping.
- [Macros](14-Macros.md) — `$param`, `%%label`, and macro-call syntax in depth.
- [Instruction set](05-Instruction-Set.md) — every mnemonic and its accepted operand shapes.
