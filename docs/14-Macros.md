# Macros

[← Back to index](README.md)

## Declaration and call syntax

```casm
const TERM_OUT = 0xFF000004

macro print_char $reg, $code
    li $reg, $code
    la r13, TERM_OUT
    strb [r13 + 0], $reg
endmacro

@text
global main:
    print_char r1, 72     // expands to: li r1, 72 / la r13, TERM_OUT / strb [r13 + 0], r1
```

A macro declaration is `macro name $param1, $param2, ... <body statements> endmacro`. Parameters are
written with a leading `$` in the header; inside the body, `$param` refers to whatever operand the
call site passed for that position. A macro call looks exactly like an instruction — `print_char r1,
72` — which is precisely how the parser tells them apart: **any identifier at the start of a
statement that is not a recognized mnemonic is parsed as a macro call** (see
[Language syntax](10-Language-Syntax.md)).

## Macros are overloaded by arity

```casm
macro log $value          // 1 parameter
    ...
endmacro

macro log $value, $level  // 2 parameters -- a DIFFERENT macro, same name
    ...
endmacro
```

`MacroTable` keys macros by `MacroSignature { name, parameterCount }`
(in [`macro_table.h`](../Ceres/libs/asm/include/ceres/asm/macro_table.h)) — so `log x` and `log x, y` call two
independently-defined macros that merely share a name. Declaring two macros with the *same* name and
*same* parameter count is a hard error ("Macro redefinition"), but you're free to give the same name
different bodies for different arities, similar to overloading by parameter count in general-purpose
languages.

A call site whose argument count doesn't match any declared arity for that name fails with
`Unknown mnemonic or macro '{name}' taking {N} operand(s)` — this is also the error you'll see for a
genuinely misspelled instruction mnemonic, since the parser can't distinguish the two cases ahead of
time.

## Hygienic labels: `%%label`

```casm
macro count_down $reg, $from
    li $reg, $from
%%loop:                        // unique to each expansion of this macro
    sub $reg, $reg, 1
    cmp $reg, 0
    jnz %%loop
endmacro

@text
global main:
    count_down r2, 3
    count_down r3, 5     // a second use of the same macro in the same scope
```

Without hygienic labels, both expansions above would try to define a label named `loop`, colliding
with each other (and with any real `.loop` in the surrounding code). `%%loop` avoids this: every time
a macro is expanded, `TranslationUnitBuilder` assigns the expansion an incrementing **instance ID**,
and every `%%label` used anywhere in that expansion — as a declaration or as a `jp`/`jz`/etc. target —
is rewritten to a name that includes it:

```cpp
// translation_unit.cpp
return _translationUnit.state().stringPool().makeIdentifier(
    std::format("%%{}#{}", macroLabel.view(), instanceId));
```

The generated name (literally containing `%%` and `#`) is not something the source grammar can
produce on its own, so it's guaranteed never to collide with a name the programmer could have
written by hand.

`%%label:` is **only** valid as a statement inside a macro body — using it anywhere else is rejected
with "Macro label used outside a macro body", since outside a macro expansion there is no instance ID
to make it hygienic against.

## Nesting

```casm
macro inner $x
    li r0, $x
endmacro

macro outer $x
    inner $x        // a macro calling another macro
endmacro
```

Macro calls inside a macro body are expanded recursively — `expandMacroCall` is called again for each
nested call, passing along an incremented expansion depth. A macro that (directly or indirectly)
expands into a call to itself is caught rather than hanging the assembler: expansion depth is capped
at `MaxMacroExpansionDepth = 32`, past which assembly fails with "Macro expansion nested more than 32
levels deep; '{name}' is probably recursive".

## Parameter substitution details

Inside a macro body, `$param` is replaced by **whatever operand the call site passed**, verbatim —
not evaluated or coerced. If the call passed a register, `$param` behaves like that register
everywhere it's used in the body; if it passed an immediate, likewise. Using `$param` where the
matching call-site operand doesn't fit the instruction it appears in fails exactly the way passing
that operand directly to that instruction would.

Referencing a `$name` that isn't one of the macro's declared parameters is a compile error
(`'$name' is not a parameter of macro 'foo'`), caught during expansion, not silently treated as a
normal identifier.

### A parameter inside a memory operand

A parameter can be the base of a memory operand, its offset, or both:

```casm
macro load_at $dst, $base, $off
    ldr $dst, [$base + $off]
endmacro

macro save_byte $base, $src
    str u8[$base + 4], $src
endmacro
```

This is worth spelling out because the body is parsed **once, where it is written** — long before
anyone knows which register `$base` is. `[$base + 4]` has to parse with the base still unknown, so
the operand is built with a hole in it and the hole is filled at expansion. Everything else about it
was decided by the source: the brackets, the access type, whether there is a displacement at all.

What the offset *means* is decided by the argument, exactly as if the three calls had been written
out by hand:

| Argument | What `[$base + $off]` becomes |
| --- | --- |
| a number | a displacement — `ldr r1, [r2 + 8]` |
| a register | an **index**, which is a different opcode — `ldrx r1, [r2 + r3]` |
| a name | a symbolic displacement, resolved like any constant |

Two rules follow from the base having to end up as a register:

- **The base must be given a general-purpose register.** An immediate or a float register is an
  error naming the parameter and the macro, since the mistake is at the call site and the line that
  fails is inside the body.
- **A parameter cannot be subtracted**: write `[$base + $off]` and pass the negative value. `-$off`
  would have to mean "negate whatever this turns out to be", and for a register index there is
  nothing to negate.

## Worked example: the calling convention

[Known limitations](19-Known-Limitations.md) points out that the VM enforces **no** calling
convention at all — no register is hardwired as caller-saved or callee-saved, `fp` is just a name
with no special behaviour, and only `at` (`r13`) is ever clobbered automatically (by `stv`, see
[Pseudo-instructions](06-Pseudo-Instructions.md)). Macros are exactly the tool this project expects
you to reach for to fill that gap: they let you write the convention once and apply it everywhere by
name, instead of repeating the same `push`/`pop` boilerplate — and forgetting it — in every
subroutine.

Here is a small, complete convention: **`r0` is the return value, `r1`–`r3` are arguments, and
`r4`–`r9` are callee-saved** (a subroutine that touches them must restore them before returning,
exactly like `rbx`/`rbp`/`r12`–`r15` on x86-64's System V ABI).

```casm
// lib/call.casm
global macro proc_enter $frame_size
    enter
    sub sp, sp, $frame_size
endmacro

global macro proc_leave
    leave
    ret
endmacro
```

Two macros, and between them the whole prologue and epilogue of every subroutine in a program. They
are worth writing as macros rather than as a comment reminding people what to do, for the reason this
page is about: the assembler guarantees they expand to the same instructions every time, so the
convention cannot silently drift out of sync between one subroutine and the next the way hand-written
prologues eventually would.

A subroutine written against them:

```casm
import "lib/call.casm"

struct FactFrame
    saved_r8: u32
endstruct

@text
factorial:
    proc_enter FactFrame
    str r8, [sp + FactFrame.saved_r8]   // r8 is callee-saved, and we are about to use it

    mov  r8, r0
    ifle r8, 1, .base
    sub  r0, r8, 1
    call factorial                       // r8 survives the call; r0-r7 do not
    mul  r0, r0, r8
    jp   .done
.base:
    li r0, 1
.done:
    ldr r8, [sp + FactFrame.saved_r8]
    proc_leave                           // leave, then ret
```

`proc_leave` ends with the `ret` itself, so a subroutine can have as many exit points as it likes and
each one restores the frame identically. `leave` recovers `sp` from `fp`, which is why the frame size
appears once, in the prologue, and never again.

Note that `global macro` is doing real work here: without it these would be invisible to every file
that imports the library (see [Modules and `import`](15-Modules-and-Import.md)).

The register roles, the frame layout and how arguments are passed are the rest of the convention —
see [A calling convention](24-Calling-Convention.md), which this library exists to serve.

## Visibility

A macro is private to the file that declares it unless it carries `global`:

```casm
global macro print_char $reg, $code     // usable by any file importing this one
    li $reg, $code
    la r13, TERM_OUT
    strb [r13 + 0], $reg
endmacro

macro internal_helper $r                // private to this file
    ...
endmacro
```

This is a change from how macros used to work: every macro defined anywhere in an imported file
became visible to the importer regardless of any marking. Now they follow the same rule as everything
else (see [Labels and symbols](12-Labels-and-Symbols.md)). Calling one a module keeps private says
so:

```
Macro 'internal_helper' is declared in 'lib/util.casm' but is not global, so it is not visible here
```

A macro reached through a **named import** is called by its qualified name, which is how two modules
that both export a `clamp` stay usable in one file:

```casm
import "lib/math.casm" as math

    math.clamp r1, r2
```

A private macro that nothing in its own file calls is reported as a warning, like any other private
declaration — see [Errors and diagnostics](17-Errors-and-Diagnostics.md#warnings).

## Where this is actually used

The calling convention's prologue and epilogue are two exported macros in
[`lib/call.casm`](../Ceres/stdlib/call.casm) — a small, real example of a `global macro` that a
project imports everywhere. See [A calling convention](24-Calling-Convention.md).

## Related pages

- [Language syntax](10-Language-Syntax.md) — how the parser distinguishes an instruction from a macro call.
- [Labels and symbols](12-Labels-and-Symbols.md) — ordinary label scoping, which `%%label` deliberately works around.
- [Modules and `import`](15-Modules-and-Import.md) — how a `global macro` reaches another file, and how to call one by a qualified name.
- [Registers and flags](03-Registers-and-Flags.md) — the registers the worked example above chooses to treat as callee-saved.
- [Known limitations](19-Known-Limitations.md) — why the VM itself enforces no calling convention at all.
