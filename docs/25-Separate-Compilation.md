# 25 · Separate compilation: objects, archives and `ceres link`

A program can be built in one go — every source file read, laid out and encoded in a single pass —
or one file at a time, into pieces that a later step joins. The first is what
[`ceres asm`](16-CLI-and-Assembly-Pipeline.md) has always done. This page is about the second.

```bash
ceres asm -c lib.casm  -o lib.cobj      # one unit, on its own
ceres asm -c main.casm -o main.cobj
ceres link main.cobj lib.cobj -o program.cres
ceres run program.cres
```

## What a unit knows on its own, and what it does not

An assembler resolves a name to a number and writes that number into a field. When every unit is
assembled together, that number is final — which is why the whole-program path has never needed to
record anything about it.

Assembled alone, a unit knows the *shape* of every access it makes and the *offset* of everything it
declares. It does not know two things:

- **where its own sections will be placed.** Its `.text` starts at zero because that is all it can
  honestly say; the program's other objects will decide what comes before it.
- **the address of anything another unit defines.** Not even approximately.

So every field that would have held an address is written as zero, and a **relocation** is recorded
beside it: which field, how the value goes into it, and what the value is *of*. A relocation is the
difference between an object and a program.

## What is in a `.cobj`

| Part | What it holds |
| --- | --- |
| Sections | The `.text`, `.rodata` and `.data` bytes this unit emits, plus how much `.bss` it needs. |
| Symbols | The names this unit publishes — its `global` labels and variables — each with a section and an offset within it. |
| Relocations | One per field left blank: the offset of the word, the field, the shift, whether it is PC-relative, and the target — either a section and an offset, or a name. |
| Interrupt bindings | One per `interrupt NUMBER: handler` this unit declares: the number (already final — it's a constant) and the handler, as a target described exactly like a relocation's — a section and an offset, or a name. See [Interrupt vector binding](26-Interrupt-Vector-Binding.md). |
| Debug tables | The line and symbol tables, addresses still relative to this unit's own sections. Only with `--debug`. |

Nothing about types travels in an object. A caller learns what a symbol *is* from the source it
imports; the link only needs to know where it ended up.

## Imports are declarations, not copies

`import "lib.casm"` in a unit being assembled on its own is read for what it *declares* — types,
sizes, macros, constants — and contributes no bytes. Its globals become names this object asks for.

That is not a shortcut, it is the only arrangement that works. If an import emitted its code into
every object that imported it, two objects that both use the same library would each carry a copy,
and the link would find every one of that library's names defined twice.

It is also where the *types* come from. Ceres picks an opcode from the shape of an instruction's
operands — `mov r1, counter` is a different instruction depending on what `counter` is — so the
assembler has to know the shape of a symbol long before anything has an address. A header in C does
the same job for the same reason.

The consequence is the familiar one: the source of a library is what you write against, and the
object is what you link against. They have to agree, and nothing checks that they do.

## `ceres link`

```bash
ceres link main.cobj lib.cobj libstuff.car -o program.cres [--debug]
```

The link places all the `.text` of every object, then all the `.rodata`, then `.data`, then `.bss` —
the same order and the same 4-byte alignment the whole-program linker uses, so a program built
either way has the same shape of memory map. Then it:

1. builds one table of every name every object publishes, and reports any name that two of them
   define;
2. adds the addresses only it can know — `__text_start`, `__bss_end`, `__heap_start` and the rest
   (see [Labels and symbols](12-Labels-and-Symbols.md)); an object that mentions one of these simply
   has a relocation for it, like any other name it does not define;
3. fills in every relocated field, refusing exactly what the assembler would have refused: an
   address that does not fit its field, a branch out of range;
4. resolves every object's interrupt bindings the same way, then checks the one thing no single
   object could: that two of them didn't bind the same number;
5. finds the global `main`, and complains if no object has one.

## Archives

```bash
ceres ar libstuff.car strings.cobj math.cobj sort.cobj
ceres link main.cobj libstuff.car -o program.cres
```

An archive is several objects in one file. Objects named on the command line are part of the program
whether or not anything calls them; **archive members are pulled in only when they answer a name
nothing else does**, and pulling one in can open names of its own, so the link goes round until a
pass adds nothing. A program that calls one routine from a library of forty carries one.

## What separate compilation gives up

**Relaxation.** The whole-program linker rewrites a three-word `ldv` into the one-word `ldvp` when
the variable turns out to be within reach — see [Pseudo-instructions](06-Pseudo-Instructions.md). An
object cannot: "within reach" is a distance from an instruction to a variable, and every other
object's `.text` is going to be placed between this unit's code and its data. So `ldv`/`stv` keep the
three words they reserved, and a program built from objects is a little larger than the same sources
assembled whole. Writing `ldvp` by hand still works — it becomes a relocation, and the link either
finds it in range or says how far out it was.

**Errors move.** Anything that depends on a final address — a PC-relative access out of reach, an
address that does not fit a 16-bit field — is a link error rather than an assembly error. The
message names the object and the distance instead of the line, which is a real loss; the source line
is one `ceres asm` away.

## Related pages

- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — every command and option.
- [Modules and `import`](15-Modules-and-Import.md) — what an import does in a whole-program build.
- [Labels and symbols](12-Labels-and-Symbols.md) — `global`, and the symbols the linker defines.
- [The `.cres` binary format](09-CRES-Binary-Format.md) — what a link produces.
- [Interrupt vector binding](26-Interrupt-Vector-Binding.md) — `interrupt NUMBER: handler`, and how it survives being split across objects.
- [Debug information](21-Debug-Information.md) — the tables an object carries and the link merges.
