# Modules and `import`

[← Back to index](README.md)

## Syntax

```casm
import "lib/math.casm"
```

`import` takes a single string literal naming another `.casm` file. There is no `import`-with-alias,
no selective import of individual symbols, and no wildcard form — importing a file makes **all** of
its constants and macros visible (see below for exactly what "all" covers).

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

Only **constants and macros** cross the `import` boundary — not labels, not variables, and not
instructions:

```cpp
symbolTable.importSymbols(moduleUnit->get());
macroTable.importMacros(moduleUnit->get());
```

`SymbolTable::importSymbols` only pulls in symbols the imported unit itself marked `global`, so a
`const` or a label declared without `global` in the imported file stays private to it even after an
`import`. Every macro defined anywhere in the imported file, however, becomes available in the
importing file regardless of any `global` marking (macros don't have a `global`/file-level
distinction the way labels and data do — see [Labels and symbols](12-Labels-and-Symbols.md)).
Importing the same macro name+arity that's already defined locally is a "Macro redefinition" error,
exactly as if you'd declared it twice in one file.

## Transitive imports

```casm
// a.casm
import "b.casm"
```
```casm
// b.casm
import "c.casm"
```

If `a.casm` imports `b.casm`, and `b.casm` imports `c.casm`, then `a.casm` also gets `c.casm`'s
constants and macros — `TranslationUnitBuilder` propagates `b`'s own list of imported modules onto
`a`'s, so transitive imports are followed automatically. Each module is only ever actually parsed and
processed **once** per assembly run, even if multiple files import it — `AssemblyState` caches
translation units by their resolved path, and a file that's already imported is simply skipped the
second time (`hasImportedModule`), rather than being re-processed or re-merged.

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

## Global symbol uniqueness still applies across imports

Because imported constants merge into the importing file's own symbol table as if declared locally,
the same "no redefinition" rule from [Labels and symbols](12-Labels-and-Symbols.md) applies: if two
files that both get imported (directly or transitively) into the same translation unit each declare a
`global` constant with the same name, that's a redefinition error at import time, not something
deferred to the linker.

## Related pages

- [Constants and expressions](13-Constants-and-Expressions.md) — what a `const` actually is once imported.
- [Macros](14-Macros.md) — macro definitions, which import in full regardless of any `global` marking.
- [Labels and symbols](12-Labels-and-Symbols.md) — the global/file/local distinction that governs what a `global` constant or label is.
- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — how multiple files (via `import` or via multiple command-line inputs) become one linked program.
