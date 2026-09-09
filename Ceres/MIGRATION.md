# Migrating `Ceres-ASM-old/` to `Ceres/`

The new tree is set up and builds from day one, with its directories still
empty. The idea is to move the code over in chunks, in commits that compile, without leaving the
branch broken along the way.

## Building

```sh
cmake --preset msvc && cmake --build --preset msvc-debug
cmake --preset gcc  && cmake --build --preset gcc-debug && ctest --preset gcc-debug
```

The `msvc` preset doesn't pin the generator's version on purpose: this machine has Visual
Studio 18 and CI has 17, and CMake already picks the newest one it finds. VS 17-and-later
generators compile x64 by default, which is the only thing this project has ever really
supported — only the x64 configurations had `stdcpp23` and the includes directory.

When configuring, CMake says what's missing:

```
-- Ceres 0.1.0 - C++23, GNU 15.2.0
--   to migrate  : core, vm, devices, asm, debug
--   executable  : no apps/cli/src/main.cpp yet
```

## The new tree's rules

**`include/` is the API, `src/` isn't.** Whatever is under `libs/<x>/include/ceres/<x>/` can be
included by anyone; whatever is in `libs/<x>/src/` is invisible from outside, and the
compiler enforces it.

**Almost every header ends up in `include/`, and that's not a sloppy split.** The transitive
closure of the `#include`s from headers used outside their own folder says as much:

| Library | Headers | Reachable from a public one | Can stay in `src/` today |
| --- | ---: | ---: | --- |
| `asm`   | 30 | 29 | only `scope.h` |
| `vm`    |  6 |  6 | none |
| `debug` |  6 |  6 | none |

`assembler.h` directly includes `parser.h`, `translation_unit.h`, `linker.h`,
`binary_emitter.h`, `object_file.h` and `assembly_state.h`, and those drag in the rest. `ceresvm.h`
includes `execution_engine.h`, which drags in the other four. `debug_session.h` drags in
`expression.h` and `history.h`, and `debug_server.h` drags in `json.h`.

So **move every header to `include/` and only the `.cpp` files to `src/`**. Thinning out the
public headers is the next phase, and it needs to happen separately: if you move and thin at
the same time, when something breaks you won't know which of the two caused it.

The recipe for thinning, afterward: drop a header down to `src/`, rebuild, and if it fails, fix
the public header that was dragging it in. The compiler says exactly when you've cut too much.

**Public includes use angle brackets and the full path.** `#include "vm/opcodes.h"` becomes
`#include <ceres/core/isa/opcodes.h>`. Between private headers of the same library
`#include "lexer.h"` still works as before.

**Nobody adds a dependency without writing it down.** If `ceres-asm` needs something from
`ceres-vm`, it won't compile until someone puts `ceres::vm` in `libs/asm/CMakeLists.txt`'s
`DEPENDS`. That's the whole point of the separation: that it's a decision, not an oversight.

**Files are discovered on their own, for now.** Moving a `.cpp` doesn't require touching any
CMakeLists. Once the migration is done the globs in `cmake/CeresLibrary.cmake` need to become
explicit lists — a glob can't warn you that someone left a file out.

---

## Destination map

### `libs/core` — the shared contract

| From `Ceres-ASM-old/src/` | To |
| --- | --- |
| `common/types.h` `assert.h` `config.h` `logs.h` `int24.h` `fixed_vector.h` `memory.h` `string_utils.h` | `libs/core/include/ceres/core/base/` |
| `vm/opcodes.h` `instructions.h` `registers.h` `fregisters.h` `address.h` `interrupts.h` `disassembler.h` | `libs/core/include/ceres/core/isa/` |
| `vm/program.h` | `libs/core/include/ceres/core/format/program.h` |
| `vm/program.cpp` | `libs/core/src/format/program.cpp` |
| `debug/debug_info.h` | `libs/core/include/ceres/core/format/debug_info.h` |
| `debug/debug_info.cpp` | `libs/core/src/format/debug_info.cpp` |
| *(new)* memory map constants | `libs/core/include/ceres/core/format/memory_map.h` |

`common/config.h` defines `forceinline` and reads `CERES_DEBUG`. The build still sets that
macro, now from `cmake/CeresSettings.cmake` and on the public target, so it also reaches
whoever includes the header.

### `libs/vm` — the machine

| From | To |
| --- | --- |
| `vm/ceresvm.h` `execution_engine.h` `memory.h` `interrupt_controller.h` `io_ports.h` `bios.h` | `libs/vm/include/ceres/vm/` |
| `vm/ceresvm.cpp` `execution_engine.cpp` | `libs/vm/src/` |
| `tests/test_vm.cpp` | `libs/vm/tests/` |

`vm/memory.h` stays here without the map constants, which move up to `core/format/memory_map.h`.
Don't confuse it with `common/memory.h`, which holds utilities and goes to `core/base/`.

`ceres-vm` has no private header, and the reason is one line: `ceresvm.h` includes
`execution_engine.h` — 1,647 lines — which in turn drags in the other four. That's the place to
ask, later, whether `ceres-vm`'s API is all of that or just `CeresVM`. Not now: move the six
into `include/`, get it to compile, and leave that for its own commit.

`test_vm.cpp` is the only one of the fifteen tests that stands on the machine alone — it doesn't
go through `assemble_helper.h` — so it gives the new tree's first green suite.

### `libs/devices` — the peripherals

| From | To |
| --- | --- |
| `vm/devices.h` | `libs/devices/include/ceres/devices/devices.h` |
| `vm/storage_devices.h` | `libs/devices/include/ceres/devices/storage_devices.h` |

### `libs/asm` — assembler and linker

The 45 entries under `assembler/` move together: they can't be split up, because the headers
include each other.

| | |
| --- | --- |
| **`include/ceres/asm/`** | the 30 headers **minus** `scope.h` |
| **`src/`** | the 15 `.cpp` files, plus `scope.h` |

`scope.h` is the only assembler header no public one reaches, so it's the only one that can
start in `src/`. The rest rises through the transitive closure of `assembler.h` and
`object_linker.h`, measured, not estimated.

Both cuts apply here. If `ceres_asm` compiles, both are done: its include path contains
neither `debug/` nor `vm/`, so there's no way a stray edge is left.

### `libs/debug` — the debugger

| From `debug/` | To |
| --- | --- |
| `debug_session.h` `debug_cli.h` `debug_server.h` `expression.h` `history.h` `json.h` | `libs/debug/include/ceres/debug/` |
| the corresponding six `.cpp` files | `libs/debug/src/` |
| `debug_info.h` `debug_info.cpp` | **no**: they go to `ceres-core` |
| `tests/test_debugger.cpp` `test_expression.cpp` `test_history.cpp` `test_json.cpp` | `libs/debug/tests/` |

The last three headers go public by drag-along: `debug_session.h` includes `expression.h`
and `history.h`, and `debug_server.h` includes `json.h`. When they move down to `src/`, their
tests will still see them: `ceres_add_tests()` gives each library's suite access to its own
`src/`. It's the only exception to the boundary, and it's deliberate — a unit test that can only
touch the public API isn't unitary.

`debug_session.cpp` includes `assembler/assembler.h` because the session assembles source on the
fly. It's not a cycle and doesn't need touching: it becomes `<ceres/asm/assembler.h>` and that's
it.

### `apps/cli`

| From | To |
| --- | --- |
| `src/main.cpp` | `apps/cli/src/main.cpp` |

### Tests

| From `tests/` | To | Why |
| --- | --- | --- |
| `framework.h` `main.cpp` | `Ceres/tests/framework/` | The scaffolding, no dependencies |
| `assemble_helper.h` | `Ceres/tests/e2e/` | Drags in the whole assembler |
| `test_encoding` `test_language` `test_macros` `test_modules` `test_pipeline` `test_robustness` `test_devices` `test_debug_info` `test_program_file` `test_objects` | `Ceres/tests/e2e/` | Assemble source and run it |
| `test_vm` | `libs/vm/tests/` | Only needs the machine |
| `test_debugger` `test_expression` `test_history` `test_json` | `libs/debug/tests/` | `ceres-debug` already depends on `asm` and `vm` |
| `build.sh` | — | Replaced by `cmake --build --preset` and `ctest --preset` |

Move `framework.h` and `main.cpp` **first**: until they're in place, CMake won't build any
suite (it says so at configure time, it doesn't fail at link time) and you'll have nothing to
check the rest with.

Nine of the fifteen files go through `assemble_helper.h`, so **splitting up the libraries
doesn't split up the tests**. They don't need rewriting: they're good. They just need to be
called what they are.

`test_program_file.cpp` is the best candidate to become a unit test of `core`: it only uses
`assemble_helper` to produce a `.cres`, and that can be built by hand.

### Everything else

| From | To |
| --- | --- |
| `Ceres-ASM-old/examples/` | `Ceres/examples/` |
| `Ceres-ASM-old/lib/call.casm` | `Ceres/stdlib/call.casm` |

`lib/` becomes `stdlib/` so it isn't confused with `libs/`, which is something else. Both are
candidates to move out into their own repository (`ceres-lang`) in phase 04.

---

## The order to move things in

Bottom-up, following the graph. Each step leaves the tree compiling, so if something
breaks you know exactly what caused it. Check after each one:

```sh
cmake --build --preset gcc-debug
```

1. **The tests' scaffolding** — `framework.h` and `main.cpp` to `tests/framework/`.
2. **`core/base`** — the whole `common/` folder. Depends on nothing.
3. **`core/isa`** — the seven files of the instruction set.
4. **`core/format`** — `program.*`, `debug_info.*`, and this is where you write `memory_map.h`.
5. **`vm`**, then **`devices`**. When done, `test_vm.cpp` moves down to `libs/vm/tests/` and gives
   the first green suite.
6. **`asm`** — all 45 at once, applying both cuts. If it compiles, they're done.
7. **`debug`** and **`cli`** — this is where the new tree's `ceres` first appears.
8. **The tests and the data** — `tests/e2e/`, then the unit ones, then `examples/` and `stdlib/`.
   Finishes with `ctest --preset gcc-debug`.

Use `git mv`, not copy-and-delete: with 106 files git detects the renames by similarity and
`git log --follow` and `git blame` keep working afterward.

---

## The two cuts, with their exact locations

They need to be applied **while moving**, not afterward, or the new tree inherits the same
edges.

**1. The assembler includes the debugger.** Three headers under `assembler/` include
`debug/debug_info.h`:

```
assembler/assembler.h
assembler/binary_emitter.h
assembler/object_linker.h
```

Since `debug_info` moves to `core`, all three switch to `#include <ceres/core/format/debug_info.h>`
and the edge disappears on its own.

**2. The linker knows about RAM.** Two uses of a constant that lives inside `vm::Memory`:

```
assembler/linker.cpp:416        memoryMap.textStart = vm::Memory::UnrestrictedSegmentStart;
assembler/object_linker.cpp:161 const u32 textStart = vm::Memory::UnrestrictedSegmentStart.value();
```

The memory map constants need to come out of `vm/memory.h` into
`core/format/memory_map.h`, and be included from both places. `Memory` — the byte array of
a running machine — stays in `ceres-vm`, and `libs/asm/CMakeLists.txt` doesn't gain any
`DEPENDS`.

To check the cut is done there's no need to read anything: if `ceres_asm` compiles, it's
done, because its include path contains neither `debug/` nor `vm/`.

---

## Out of scope for this phase

The following still points at the old tree and needs redoing once `Ceres/` has code:

- **`.github/workflows/ci.yml`** — two `msbuild` paths, the test executable's path and a
  `g++` line that lists `vm/*.cpp assembler/*.cpp debug/*.cpp`. Gets replaced entirely by
  `cmake --preset` + `cmake --build --preset` + `ctest --preset`.
- **`tests/build.sh`** — the same globs and the sixteen test files by hand. Goes away.
- **`Ceres-ASM-old/*.vcxproj`** — 87 duplicated entries. CMake generates these.
- **`editors/vscode-ceresasm/client/src/compilerPath.ts`** and **`server/src/compiler.ts`** —
  they search for `Ceres-ASM/src/ceres.exe` climbing up to eight directories. The binary now lands
  in `Ceres/build/<preset>/bin/`.
- **`README.md`** — the build instructions are the `g++` line and the `.vcxproj` path.
- **`.gitnexus/`** — the index is built on the old paths: `node .gitnexus/run.cjs
  analyze` once the new tree has code.

Two `.gitignore` traps are already resolved, and it's worth knowing they existed:

- Visual Studio's `[Dd]ebug/` rule swallows `Ceres/libs/debug/`, just like it used to swallow
  `Ceres-ASM/src/debug/`. There are explicit negations for both.
- The `/ceres` rule for the compiled binary also captured the `Ceres/` directory, because on
  Windows git compares patterns case-insensitively — the whole tree was invisible to
  `git status`. A trailing-slash `!/Ceres/`, which only applies to directories, fixes it.
