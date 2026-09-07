# Modules and `import`

[← Back to index](README.md)

## Syntax

```casm
import "lib/math.casm"
```

`import` takes a single string literal naming another `.casm` file. There is no `import`-with-alias,
no selective import of individual symbols, and no wildcard form — importing a file makes visible
everything that file declares `global`, and nothing else.

## Path resolution

A relative import path resolves **against the file that contains the `import` statement**, not
against the current working directory the `ceres` binary happens to run from:

```cpp
// translation_unit.cpp
std::filesystem::path modulePath{ imp.moduleName.str() };
if (modulePath.is_relative() && !_sourcePath.empty())
    modulePath = _sourcePath.parent_path() / modulePath;
```

This means a library of `.casm` files can `import` each other using paths relative to their own
location, and moving the whole library to a different directory (or being imported from a project at
a different depth) doesn't break those internal imports. The resolved path is then normalized
(`lexically_normal()`) before being used as the module's identity for caching and cycle detection.

## What actually gets imported

Only what the module declares `global` crosses the boundary — constants, variables and macros:

```casm
// lib/rules.casm
global const MAX_PLAYERS = 4        // exported
const INTERNAL_SLACK    = 8         // private to this file

@data
    global let scoreboard: u32[8]   // exported, address and all
    let scratch:           u32[8]   // private

global macro clamp $reg, $hi        // exported
    ...
endmacro
```

Labels keep their own `global`/file-level/local distinction, which works the same way — see
[Labels and symbols](12-Labels-and-Symbols.md).

Referring to a name a module declares without `global` says exactly that, rather than reporting it
as unresolved:

```
'MAX_PLAYERS' is declared in 'lib/rules.casm' but is not global, so it is not visible here.
```

## An import is a reference, not a copy

Importing a module records it as a place to look. The module's declarations stay in its own tables
and are found by walking the importing unit's list of imports: its own table first, then the
exported surface of each module it imports.

Nothing is copied, which is what makes importing idempotent. A module is read, parsed and built
exactly **once** per run (`AssemblyState` caches units by resolved path), and importing it a second
time — directly, or by arriving through a second route — is a plain no-op. There is an implicit
"pragma once" and nothing to merge twice.

## Transitive imports

```casm
// a.casm
import "b.casm"
```
```casm
// b.casm
import "c.casm"
```

`a.casm` sees whatever `c.casm` declares `global`: a global declaration is genuinely exported and
keeps travelling however many imports it passes through. What `c.casm` keeps private stays private
at every level, including from `b.casm` itself.

A diamond — `a` imports `b` and `c`, both of which import `d` — resolves `d` once. The walk carries
a visited set, so reaching the same module twice is not mistaken for two declarations of the same
name.

## Import cycles

```casm
// a.casm
import "b.casm"
```
```casm
// b.casm
import "a.casm"     // ERROR: import cycle
```

While a file is in the middle of being parsed and built, `AssemblyState::isBeingLoaded()` marks it as
"in progress". If an `import` is encountered for a file that's already in progress, assembly fails
immediately with `Import cycle: '{name}' is already being assembled`, instead of recursing until the
call stack overflows.

## When two modules export the same name

```casm
import "left.casm"      // global const LIMIT = 1
import "right.casm"     // global const LIMIT = 2

    li r1, LIMIT        // ERROR
```

```
'LIMIT' is exported by both 'left.casm' and 'right.casm'
```

The clash is reported **at the use**, not at the import: two modules may each declare a name as long
as no file is forced to choose between them. A global *constant* is not published to the linker's
global symbol table — it occupies no memory and is substituted at its point of use — so two
independent libraries can each declare `global const MAX` without colliding. Global labels and
variables do link, and defining one of those twice is still a linker error.

## Related pages

- [Constants and expressions](13-Constants-and-Expressions.md) — what a `const` actually is once imported.
- [Macros](14-Macros.md) — macro definitions, which cross an import only when declared `global`.
- [Labels and symbols](12-Labels-and-Symbols.md) — the global/file/local distinction that governs what a `global` constant or label is.
- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — how multiple files (via `import` or via multiple command-line inputs) become one linked program.
