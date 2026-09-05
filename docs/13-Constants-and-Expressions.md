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

The parser folds a constant arithmetic expression **at parse time**, before the value is even
attached to a name — this is why the fold logic lives in
[`parser.cpp`](../Ceres-ASM/src/assembler/parser.cpp) rather than in the translation-unit builder.
The supported grammar, with the usual precedence:

```
expression := term (('+' | '-') term)*
term       := factor (('*' | '/') factor)*
factor     := '-' factor | '+' factor | integer-literal | char-literal
```

```casm
const A = 2 + 3 * 4        // 14 -- '*' binds tighter than '+'
const B = (2 + 3) * 4      // NOT SUPPORTED -- there are no parentheses in this grammar
const C = -5 + 2           // -3
const D = 10 / 3           // 3 -- integer division, signed, truncates toward zero
const E = 10 / 0           // ERROR at parse time: "Division by zero in constant expression"
```

**There are no parentheses.** The grammar above is the entire supported set — if you need a
different evaluation order than `*`/`/` before `+`/`-` gives you, you have to restructure the
expression, since grouping isn't available.

All arithmetic here is performed on `u32` values reinterpreted through `i32` for the actual add/
subtract/multiply/divide, matching how the VM itself would compute the same operation at run time.

## Identifiers are not foldable — yet

```casm
const BASE = 0x1000
const OFFSET = BASE + 4      // ERROR: "Constant expressions cannot reference 'BASE' yet:
                              //         identifiers are only usable on their own"

const ALIAS = BASE           // OK -- an identifier used completely on its own is fine
```

This is a known, explicitly documented gap (see
[Known limitations](19-Known-Limitations.md)): the parser folds constant expressions *before*
constants are resolved by the translation-unit builder, so at fold time it has no way to know what
another identifier's value even is. An identifier is only accepted when it is the **entire**
right-hand side, with no operator attached — in that case resolution happens later, during the
translation-unit pass, exactly like any other identifier reference.

## Where a constant expression is legal

Besides a `const` declaration's own right-hand side, the same folding applies wherever an **immediate
operand** to an instruction is parsed:

```casm
li r1, 2 + 2          // assembles as `li r1, 4`
la r1, BASE_ADDR + 8  // NOT the same thing -- this is memory-operand syntax, see below
```

Careful: `2 + 2` as a plain immediate operand goes through `Parser::parseConstantExpression`, but the
`[base + offset]` syntax used inside memory operands (`ldr r1, [r2 + OFFSET]`) is parsed by a
*different* code path (`Parser::parseOperand`'s memory-operand branch) that accepts only a single
literal integer or a single identifier after the `+`/`-` — not a full expression. Compound arithmetic
inside brackets (`[r2 + 4 + 4]`) is not supported.

## Array sizes given by a constant

```casm
const MAX_PLAYERS = 4

@bss
    let active: u8[MAX_PLAYERS]
```

An array's declared size may be a constant identifier instead of a literal integer (see
[Data types and literals](11-Data-Types-and-Literals.md)). The constant is resolved during the
translation-unit pass, and must itself be an integer scalar constant — a float constant or an array
constant used as an array size is rejected.

## Related pages

- [Data types and literals](11-Data-Types-and-Literals.md) — how a literal is range-checked against the type context a constant is used in.
- [Labels and symbols](12-Labels-and-Symbols.md) — constants share the same symbol table as labels and variables.
- [Known limitations](19-Known-Limitations.md) — the `2 * BASE`-style expression gap, tracked explicitly.
