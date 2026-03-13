# TBCX — Precompiled Tcl 9.1+ Bytecode (save / load / dump)

TBCX is a C extension for Tcl 9.1+ that **serializes** compiled Tcl bytecode (plus enough metadata to reconstruct `proc`s, TclOO methods, and **lambda constructs**) into a compact `.tbcx` file — and later **loads** that file into another interpreter for fast startup without re‑parsing or re‑compiling the original source. There's also a **disassembler** for human‑readable inspection.

> Status: draft implementation; interfaces are still evolving. We optimize for **simplicity** (no backward compatibility guarantees yet) and strict **Tcl 9.1** compliance throughout.

For versions prior to Tcl 9.1+, please check

-  [tclcompiler](https://github.com/tcltk-depot/tclcompiler)
-  [tbcload](https://github.com/tcltk-depot/tbcload)

---

## Quick Start

```tcl
package require tbcx

# Save: source text → .tbcx
# - in:  Tcl value (script text) | open readable channel | path to a readable .tcl file
# - out: open writable binary channel | path to a new .tbcx file

tbcx::save ./hello.tcl ./hello.tbcx

# Load: .tbcx → installs procs/methods/lambdas and executes top level
# - in:  open readable binary channel | path to a .tbcx file

tbcx::load ./hello.tbcx

# Dump: pretty disassembly

puts [tbcx::dump ./hello.tbcx]
```
---

## Features

- **Save**: Compile a script and write a `.tbcx` artifact containing:
  - Top-level bytecode (with literals, AuxData, exception ranges, local names)
  - All discovered `proc`s, each as precompiled bytecode
  - TclOO classes (including superclass lists) and methods/constructors/destructors as precompiled bytecode
  - Lambda literals (`apply {args body ?ns?}` forms) compiled and serialized as lambda‑bytecode literals
  - Bodies found in `namespace eval $ns { ... }` and other script-body contexts (compiled and bound to the correct namespace)
- **Load**: Read a `.tbcx`, create procs, classes and methods, patch bytecode with the current interpreter/namespace, and execute the top level.
- **Dump**: Pretty-print / disassemble `.tbcx` contents (header, literals, AuxData summaries, exception ranges, full instruction streams).
- **Safe interp support**: `tbcx_SafeInit` exposes only `tbcx::dump` (read‑only) in safe interpreters.
- **Tcl 9.1 aware**: Uses Tcl 9.1 internal bytecode structures, literal encodings, and AuxData types; exposes them via a stable binary format header (`TBCX_FORMAT = 91`).

---

## Commands (4)

### `tbcx::save in out`
Compile and serialize to `.tbcx`.

- **`in`** may be:
  - a **Tcl value** containing the script text (no file I/O needed),
  - an **open readable channel** (encoding is left as-is — the caller controls it),
  - a **path** to a readable file (opened in text mode with default UTF-8 encoding, read, closed by TBCX).
- **`out`** may be:
  - an **open writable channel** (binary mode is enforced; channel is *not* closed),
  - a **path** to write a new `.tbcx` file (created/truncated; channel is closed on return).
- **Result**: returns the output channel handle or normalized output path.

What gets saved:

- The **top‑level** compiled block of the input script (code, literal pool, AuxData, exception ranges, local names/temps).
- All discovered **`proc`** bodies, precompiled with correct namespace bindings. Conflicting definitions across `if`/`else` branches are handled via indexed proc markers.
- TclOO **methods/constructors/destructors**, precompiled.
- **Lambda literals** appearing in the script (e.g. `apply {args body ?ns?}` forms) are compiled and serialized as **lambda‑bytecode literals** so they do **not** recompile on first use after load.
- **Namespace eval bodies** and other script-body literals (try, foreach, while, for, catch, if/elseif/else bodies) are detected and pre-compiled to bytecode when safe to do so.

### `tbcx::load in`
Load a `.tbcx` artifact, materialize procs and OO methods, rehydrate lambda bytecode literals, and execute the top‑level block with `TCL_EVAL_GLOBAL`.

- **`in`** may be an **open readable binary channel** or a **path** to a `.tbcx` file.
- **Result**: the top‑level executes (like `source`), procs, OO methods, and embedded lambda literals become available without re‑compilation.

### `tbcx::dump filename`
Produce a human‑readable string describing the artifact, including a **disassembly** of each compiled block and any **lambda literals**.

- **`filename`** must be a path to a readable `.tbcx` file.
- **Output**: header, summaries, literal listings, AuxData and exception info, plus disassembly of the top‑level/proc/method/lambda bytecode.

### `tbcx::gc`
Explicitly purge stale entries from the per‑interpreter lambda shimmer‑recovery registry (the ApplyShim). This is normally not needed — stale entries are purged lazily on each `tbcx::load` call — but can be useful in long‑running interpreters that load many `.tbcx` files and want to reclaim memory sooner.

- Takes no arguments.

---

## How saving works

`tbcx::save` compiles the given script and **captures** definitions in a single pass:

- **Capture and rewrite**: `CaptureAndRewriteScript` walks the script's token tree once, extracting `proc`, `namespace eval`, `oo::class create`, `oo::define` (method/constructor/destructor), and `oo::objdefine` forms. It simultaneously produces a **rewritten** script where captured method/constructor/destructor bodies are replaced with indexed stubs, ensuring the top-level bytecode doesn't redundantly contain their full source.
- **Namespace body scanning**: The rewritten script and captured definition bodies are scanned (`ScanScriptBodiesRec`) for nested script-body patterns — `namespace eval`, `try`/`on`/`trap`/`finally`, `foreach`, `while`, `for`, `catch`, `if`/`elseif`/`else`, `uplevel`, `eval`, `dict for`/`with`, `lsort -command` — building a mapping from body text to namespace FQN.
- **Pre‑compilation**: Matched namespace eval body literals in the top-level literal pool are compiled into a **side table** (never modifying the pool itself) so they serialize as bytecode rather than source text.
- **Serialize**: Emit:
  - A **header** (magic, format version, Tcl version, code length, exception/literal/AuxData/local counts, max stack depth).
  - The **top-level compiled block** (code bytes, literals, AuxData, exception ranges, locals epilogue). Captured proc bodies are stripped during this phase.
  - A table of **procs**: name FQN, namespace, argument spec, then the separately compiled body block.
  - **Classes** (FQN + superclass count), then **methods** (class FQN, kind, name, argument spec, compiled body block).
  - A final flush of any buffered output.

**Runaway protection**: The serializer tracks total literal calls, block calls, recursion depth, and output bytes. If any limit is exceeded (2M literals, 256K blocks, depth 64, or 256 MB output), serialization aborts with a diagnostic error.

Supported literal kinds: **bignum, boolean, bytearray, dict** (insertion order preserved), **double, list, string, wideint, wideuint, lambda‑bytecode, embedded bytecode**.

Supported AuxData: **jump tables (string and numeric), dict-update, NewForeachInfo**.

---

## How loading works

`tbcx::load` reads the header, validates magic/format/Tcl‑version compatibility, then deserializes sections:

1. **Top-level block**: Deserialized and marked `TCL_BYTECODE_PRECOMPILED` so Tcl skips compile-epoch checks and executes the bytecode directly.
2. **Procs**: A temporary **ProcShim** intercepts the `proc` command (both `objProc2` and `nreProc2` dispatch paths). When the top-level block evaluates a `proc` call matching a saved definition (by FQN + argument signature, or by indexed marker for conflicting definitions), the shim substitutes the precompiled body. Unmatched `proc` calls pass through to Tcl's original handler.
3. **Classes and methods**: An **OOShim** temporarily renames `oo::define` (and `oo::objdefine` when available) to intercept method/constructor/destructor installations. Matching definitions receive precompiled bodies; constructors and destructors are handled specially to preserve TclOO's dispatch chain (e.g. `next` routing through the constructor chain).
4. **Lambda recovery**: An **ApplyShim** is installed as persistent per-interpreter `AssocData`. When a precompiled lambda's `lambdaExpr` internal rep gets evicted by shimmer, the shim detects the missing rep on the next `[apply]` call and re-installs the precompiled `Proc*` from its registry before forwarding to Tcl's real `[apply]`.
5. **Top-level execution**: The precompiled top-level block is evaluated via `Tcl_EvalObjEx` with `TCL_EVAL_GLOBAL`. Compiled locals for the top-level frame are set up by linking named variables to existing globals (via `TopLocals_Begin`/`TopLocals_End`). `TCL_RETURN` is handled the same way `source` does — converting it to `TCL_OK` with the return value as the result.
6. **Cleanup**: The ProcShim and OOShim are removed (original command handlers restored). The ApplyShim persists for the interpreter's lifetime to support lambda shimmer recovery.

Endianness is detected and handled so that hosts read/write a consistent little-endian format on disk.

---

## File Format (overview)

**Header** (compact, binary, little‑endian):

| Field | Type | Description |
|-------|------|-------------|
| `magic` | u32 | `0x58434254` ("TBCX") |
| `format` | u32 | `91` (Tcl 9.1) |
| `tcl_version` | u32 | `maj<<24 \| min<<16 \| patch<<8 \| type` |
| `codeLenTop` | u64 | Code byte count for top-level block |
| `numExceptTop` | u32 | Exception range count |
| `numLitsTop` | u32 | Literal count |
| `numAuxTop` | u32 | AuxData count |
| `numLocalsTop` | u32 | Local variable count |
| `maxStackTop` | u32 | Maximum stack depth |

**Sections (in order):**
1. **Top‑level block** — code bytes, literal array, AuxData array, exception ranges, epilogue (maxStack, reserved, numLocals, local names).
2. **Procs** — u32 count, then repeated tuples: name FQN (LPString), namespace (LPString), argument spec (LPString), compiled block.
3. **Classes** *(advisory)* — u32 count, then class FQN + superclass list; creation occurs by evaluating the rewritten script at load time.
4. **Methods** — u32 count, then repeated tuples: class FQN, kind (u8: 0=inst, 1=class, 2=ctor, 3=dtor), name, argument spec, compiled block.

**Literal tags** (u32):
| Tag | Kind | Payload |
|-----|------|---------|
| 0 | BIGNUM | u8 sign, u32 magLen, LE magnitude bytes |
| 1 | BOOLEAN | u8 (0/1) |
| 2 | BYTEARR | u32 length + raw bytes |
| 3 | DICT | u32 pair count, then key/value literal pairs (insertion order) |
| 4 | DOUBLE | 64-bit IEEE-754 as u64 |
| 5 | LIST | u32 count, then nested literals |
| 6 | STRING | LPString (u32 length + bytes) |
| 7 | WIDEINT | signed 64-bit as u64 |
| 8 | WIDEUINT | unsigned 64-bit as u64 |
| 9 | LAMBDA_BC | ns FQN, args, compiled block, body source text |
| 10 | BYTECODE | ns FQN + compiled block |

**AuxData tags** (u32):
| Tag | Kind | Payload |
|-----|------|---------|
| 0 | JT_STR | u32 count; key LPString + u32 offset per entry |
| 1 | JT_NUM | u32 count; u64 key + u32 offset per entry |
| 2 | DICTUPD | u32 length; local indices |
| 3 | NEWFORE | u32 numLists, u32 loopCtTemp, u32 firstValueTemp, u32 numLists (dup), then per-list var indices |

**LPString**: a u32 byte-length followed by that many raw bytes (no NUL terminator on disk).

---

## Guarantees & Semantics

- **Functional equivalence**: `tbcx::load` aims to be *functionally identical* to `source` of the original script. Differences are limited to avoiding re‑parse/re‑compile time.
- **Namespaces**: The top-level block is executed with `TCL_EVAL_GLOBAL`. Saved blocks carry namespace metadata to bind compiled code correctly. Lambda literals that include a namespace element keep that association.
- **Version check**: The loader requires an exact major.minor Tcl version match (e.g. 9.1). Bytecode instruction semantics can change between minor versions.
- **Sanity limits**: Code ≤ 1 GiB; literal/AuxData/exception pools ≤ 64M entries; LPString ≤ 16 MiB; output ≤ 256 MB; recursion depth ≤ 64.

---

## Build

This package uses **Tcl 9.1+** APIs and selected internals (e.g., `tclInt.h`, `tclCompile.h`).
You'll need Tcl 9.1 headers/libs on your include path and to build as a standard loadable extension.
The entry point `tbcx_Init` registers commands and provides `tbcx 1.0`.
The safe entry point `tbcx_SafeInit` registers only `tbcx::dump`.

Example (TEA Linux/macOS):

```sh
./configure
make install
make test
```

---

## Usage notes & caveats

- **Security**: Loading a `.tbcx` executes code (top-level) and installs commands/classes. Only load artifacts you trust.
- **Compatibility**: `TBCX_FORMAT` is `91` (Tcl 9.1). Different formats are rejected during load. An exact major.minor Tcl version match is required.
- **AuxData coverage**: The saver asserts that all AuxData items in a block are of known kinds (jump tables, dict-update, NewForeachInfo). Unknown kinds cause the save to abort.
- **OO coverage**: Supports `oo::class create`, `oo::define` (method/classmethod/constructor/destructor plus declarative keywords like variable/superclass/mixin/filter/forward), and `oo::objdefine`. Builder-form class bodies are expanded into multi-word stubs for correct load-time reconstruction.
- **Lambda shimmer recovery**: Precompiled lambdas are registered in a persistent per-interpreter ApplyShim. If the `lambdaExpr` internal rep is evicted by shimmer, the shim transparently re-installs it on the next `[apply]` call.
- **Conflicting proc definitions**: When multiple branches define a proc with the same name (e.g. `if {$cond} {proc p ...} else {proc p ...}`), the saver emits indexed markers so the loader matches by position rather than by FQN alone.
- **Endianness**: Host endianness is detected at runtime; streams are always little-endian on disk.

---

## Project layout

- `tbcx.h` — shared definitions (header layout, tags, limits, buffered I/O types)
- `tbcx.c` — package init, byte‑order detection, type discovery, command registration, safe init
- `tbcxsave.c` — capture, rewrite, compile, and serialize
- `tbcxload.c` — deserialize, shim, materialize, and execute
- `tbcxdump.c` — disassembler/dumper

---

## Contributing

Issues and PRs are welcome.
Given the reliance on Tcl internals, please include **Tcl 9.1** details (commit/tag, platform, compiler) with any bug reports.

---

## License

MIT License. Copyright © 2025 Miguel Bañón.

---

## Acknowledgements

Built on top of Tcl 9.1's bytecode engine, object types, AuxData, and TclOO.

Thanks to the Tcl/Tk community.
