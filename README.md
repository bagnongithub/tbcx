# TBCX — Precompiled Tcl 9.1+ Bytecode (save / load / dump)

TBCX is a tiny C extension for Tcl 9.1+ that can **serialize** compiled Tcl bytecode (plus enough metadata to reconstruct procs, OO classes/methods, and selected namespace‑eval bodies) into a compact `.tbcx` file — and later **load** that file into another interpreter for instant execution, without re‑parsing or re‑compiling the source. There’s also a **disassembler** that renders human‑readable dumps of `.tbcx` content.

---

## Getting Started

```tcl
# Install (example: build locally, then package require)
# See the Build section below to compile tbcx.so / tbcx.dll, then:
package require tbcx 1.0

# Save a script to a TBCX artifact
tbcx::savefile ./app.tcl ./app.tbcx

# Load it later (installs procs/classes; runs top-level)
namespace eval ::myapp {
    tbcx::loadfile ./app.tbcx
}

# Inspect the artifact (human-readable dump)
puts [tbcx::dumpfile ./app.tbcx]
```

---

## Features

- **Save**: Compile a script and write a `.tbcx` artifact containing:
  - Top-level bytecode (with literals, AuxData, exception ranges)
  - All discovered `proc`s (static and dynamic), each as precompiled bytecode
  - TclOO classes (including superclass lists) and methods/constructors/destructors as precompiled bytecode
  - Bodies found in `namespace eval $ns { ... }` (compiled and bound to that namespace)
- **Load**: Read a `.tbcx`, create procs, classes and methods, patch bytecode with the current interpreter/namespace, and execute the top level.
- **Dump**: Pretty-print / disassemble `.tbcx` contents (header, literals, AuxData summaries, exception ranges, full instruction streams).
- **Tcl 9.1 aware**: Uses Tcl 9.1 internal bytecode structures, literal encodings, and AuxData types; exposes them via a stable binary format header (`TBCX_FORMAT = 9`).

---

## Commands

The package registers a `tbcx` command namespace with six subcommands:

- `tbcx::save script out.tbcx` — compile `script` (a `Tcl_Obj`) and write to a new file
- `tbcx::savechan script channel` — write to an existing channel (binary)
- `tbcx::savefile in.tcl out.tbcx` — read a `.tcl` file, compile, and save a `.tbcx` (returns normalized path)
- `tbcx::loadfile path` — load a `.tbcx` file into the **current namespace**, materializing procs/classes/methods and running the top-level
- `tbcx::loadchan channel` — load from an open channel
- `tbcx::dumpfile path` — produce a human-readable disassembly string of a `.tbcx` file

### Quick examples

```tcl
package require tbcx

# 1) Save straight from source file
tbcx::savefile ./app.tcl ./app.tbcx

# 2) Load later (installs procs/classes and executes top-level)
namespace eval ::myapp {
    tbcx::loadfile ./app.tbcx
}

# 3) Inspect a TBCX artifact
puts [tbcx::dumpfile ./app.tbcx]
```

---

## How saving works

`tbcx::save*` compiles the given script and **captures** definitions in several passes:

- **Static parse**: Walk tokens to find literal `proc` forms and `namespace eval` blocks; recurse into bodies under the correct namespace prefix.
- **Dynamic probe**: Evaluate the script inside a **child interpreter** with shimmed commands that record (but do not execute) side effects (`proc`, `oo::class create`, `oo::define … method/constructor/destructor/superclass`, with `unknown` guarded). This discovers procs/classes/methods created via control flow.
- **Merge & filter**: Deduplicate by fully-qualified name; remove duplicate definitions from the top-level script; peephole-rewrite proc-creating instruction sequences inside compiled bodies to safe NOPs when they match captured procs.
- **Serialize**: Emit:
  - A **header** (magic/format/flags/codeLen/numCmds/numExcept/numLiterals/numAux/numLocals/maxStackDepth), then top-level `code`, `literals`, `aux`, `except`
  - A table of **procs**: `name`, `namespace`, `argSpec`, then compiled body
  - **Classes** + superclasses, then **methods** (class, kind=method/ctor/dtor, name, argSpec, original body string, compiled body)
  - Compiled bodies associated with **`namespace eval` literals** referenced from top-level bytecode

Supported literal kinds include **bignum, boolean, bytearray, dict, double, list, string, wideint, wideuint**.
Supported AuxData includes **jump tables (string/num), dict-update, foreach (new/legacy)**.

---

## How loading works

`tbcx::load*` reads the header, deserializes top-level and nested bytecode blocks, and **finalizes** them as precompiled for the target interpreter:

- Top-level bytecode becomes a temporary proc executed once, then deleted, to mimic “run the script now.”
- Each saved `proc` is recreated and its body installed as a precompiled `ByteCode`.
- `oo::class create` is issued, `oo::define … superclass` applied, then each method/ctor/dtor is installed with its **compiled** body (and original body string kept).
- Namespace-eval bodies recorded during save are attached back to the literal sites in the top-level literal array, preserving semantics.

Endianness is detected and handled so that hosts read/write a consistent little-endian on disk.

---

## File format (nutshell)

Header (`TbcxHeader`, little-endian fields):

```
magic="TBCX", format=9, flags=0,
codeLen, numCmds, numExcept, numLiterals, numAux, numLocals, maxStackDepth
```

Followed by top-level: `code`, `literals[]`, `aux[]`, `except[]`
Then sections for **procs**, **classes**, **methods**, and **namespace bodies**.

**Limits (sanity caps):**
- `TBCX_MAX_CODE` (≈1 GiB)
- `TBCX_MAX_LITERALS`
- `TBCX_MAX_AUX`
- `TBCX_MAX_EXCEPT`

---

## Build

This package uses **Tcl 9.1+** APIs and selected internals (e.g., `tclInt.h`, `tclCompile.h`).
You’ll need Tcl 9.1 headers/libs on your include path and to build as a standard loadable extension.
The entry point `tbcx_Init` registers the commands and provides `tbcx 1.0`.

Example (Linux/macOS):

```sh
cc -fPIC -O2 -I/path/to/tcl9.1/generic -I/path/to/tcl9.1/unix   -shared -o tbcx.so   tbcx.c tbcxsave.c tbcxload.c tbcxdump.c
```

> Notes
> • Link (or stub) against Tcl 9.1 and libtommath as needed.
> • Because Tcl internals are used, **exact Tcl version alignment matters**.

---

## Usage notes & caveats

- **Security**: Loading a `.tbcx` executes code (top-level) and installs commands/classes. Only load artifacts you trust.
- **Compatibility**: `TBCX_FORMAT` is `9` (Tcl 9.1). Different formats are rejected during read.
- **AuxData coverage**: Save asserts that all AuxData items in a block are of supported kinds.
- **OO coverage**: Supports `oo::class create`, `oo::define` (method/constructor/destructor/superclass/eval); merges static and dynamic captures with “last wins” semantics.
- **Namespace bodies**: `namespace eval` body literals called from top-level bytecode are detected and serialized separately.
- **Endianness**: Host endianness is detected at runtime and helpers produce/read little-endian on disk.

---

## Disassembler output

`tbcx::dumpfile` prints:

- Header values and top-level block summary
- Literals (type/name), AuxData summaries, exception ranges
- Full disassembly with instruction names, operands, label targets, and stack-effect hints
- Per-proc and per-method compiled block dumps (plus class/method metadata)

---

## Project layout

- `tbcx.h` — public/shared definitions (header layout, tags, helpers, limits)
- `tbcx.c` — package init, type discovery, endian setup, command registration
- `tbcxsave.c` — serializer & capture logic (proc/OO/namespace detection, filtering, rewrites, emitters)
- `tbcxload.c` — deserializer/loader (materialize procs/classes/methods; run top-level wrapper)
- `tbcxdump.c` — dumper & disassembler

---

## Contributing

Issues and PRs are welcome.
Given the reliance on Tcl internals, please include **Tcl 9.1** details (commit/tag, platform, compiler) with any bug reports.

---

## License

MIT license

---

## Acknowledgements

Built on top of Tcl 9.1’s bytecode engine, object types, AuxData, and TclOO.
Thanks to the Tcl/Tk community.
