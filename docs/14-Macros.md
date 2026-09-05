# Macros

[← Back to index](README.md)

## Declaration and call syntax

```casm
macro print_char $reg, $code
    li $reg, $code
    outb 0x01, $reg
endmacro

@text
global main:
    print_char r1, 72     // expands to: li r1, 72 / outb 0x01, r1
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
(in [`macro_table.h`](../Ceres-ASM/src/assembler/macro_table.h)) — so `log x` and `log x, y` call two
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
(`'${}' is not a parameter of macro '{}'`), caught during expansion, not silently treated as a normal
identifier.

## Worked example: a calling convention built out of macros

[Known limitations](19-Known-Limitations.md) points out that the VM enforces **no** calling
convention at all — no register is hardwired as caller-saved or callee-saved, `fp` is just a name
with no special behaviour, and only `r12` is ever clobbered automatically (by `ldv`/`stv`, see
[Pseudo-instructions](06-Pseudo-Instructions.md)). Macros are exactly the tool this project expects
you to reach for to fill that gap: they let you write the convention once and apply it everywhere by
name, instead of repeating the same `push`/`pop` boilerplate — and forgetting it — in every
subroutine.

Here is a small, complete convention: **`r0` is the return value, `r1`–`r3` are arguments, and
`r4`–`r9` are callee-saved** (a subroutine that touches them must restore them before returning,
exactly like `rbx`/`rbp`/`r12`–`r15` on x86-64's System V ABI).

```casm
// --- the convention, expressed as two macros ---

macro proc_enter
    push r4
    push r5
    push r6
    push r7
    push r8
    push r9
endmacro

macro proc_leave
    pop r9
    pop r8
    pop r7
    pop r6
    pop r5
    pop r4
    ret
endmacro
```

`proc_leave` pops in the exact reverse order `proc_enter` pushed in — required, since the stack is
last-in-first-out (see [Memory → The stack](02-Memory.md#the-stack)) — and ends with the `ret` itself,
so every subroutine written against this convention has a single matching exit point instead of a
`ret` that's easy to forget to place after manually restoring registers.

A subroutine written against the convention:

```casm
// r1 = a, r2 = b -> r0 = (a + b) * 2, per the convention above
add_and_double:
    proc_enter
    mov r4, r1        // callee-saved scratch space is now safe to use...
    add r0, r1, r2    // r0 = a + b
    add r0, r0, r0    // r0 = (a + b) * 2
    proc_leave         // restores r4-r9, then returns
```

And a caller using it — note the caller doesn't need to know or care that `add_and_double` touches
`r4` internally, because `proc_enter`/`proc_leave` already made that invisible:

```casm
@text
global main:
    li r4, 0xDEAD     // something the caller needs r4 to keep holding across the call
    li r1, 10          // argument a
    li r2, 32          // argument b
    call add_and_double
    // r0 == 84 here, and r4 == 0xDEAD still, exactly as before the call
    halt
```

This is the whole mechanism: the VM itself never checks any of this — nothing stops a subroutine from
skipping `proc_enter`/`proc_leave` and clobbering `r4`–`r9` anyway. The convention only holds because
every subroutine and every caller consistently uses the same two macros, which is precisely why
expressing it as macros (rather than as a comment reminding people what to do) is worth doing: the
assembler enforces that `proc_enter` and `proc_leave` always expand to the same fixed instruction
sequence, so the convention can't silently drift out of sync between subroutines the way hand-written
prologues eventually would.

## Related pages

- [Language syntax](10-Language-Syntax.md) — how the parser distinguishes an instruction from a macro call.
- [Labels and symbols](12-Labels-and-Symbols.md) — ordinary label scoping, which `%%label` deliberately works around.
- [Modules and `import`](15-Modules-and-Import.md) — macros defined in one file become usable in another via `import`.
- [Registers and flags](03-Registers-and-Flags.md) — the registers the worked example above chooses to treat as callee-saved.
- [Known limitations](19-Known-Limitations.md) — why the VM itself enforces no calling convention at all.
