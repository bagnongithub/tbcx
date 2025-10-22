# TBCX — Precompiled Tcl 9.1+ Bytecode (save / load / dump)

TBCX is a C extension for Tcl 9.1+ that **serializes** compiled Tcl bytecode (plus enough metadata to reconstruct `proc`s, TclOO methods, and **lambda constructs**) into a compact `.tbcx` file — and later **loads** that file into another interpreter for fast startup without re‑parsing or re‑compiling the original source. There’s also a **disassembler** for human‑readable inspection.

> Status: draft implementation; interfaces are still evolving. We optimize for **simplicity** (no backward compatibility guarantees yet) and strict **Tcl 9.1** compliance throughout.

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
  - Top-level bytecode (with literals, AuxData, exception ranges)
  - All discovered `proc`s (static and dynamic), each as precompiled bytecode
  - TclOO classes (including superclass lists) and methods/constructors/destructors as precompiled bytecode
  - Bodies found in `namespace eval $ns { ... }` (compiled and bound to that namespace)
- **Load**: Read a `.tbcx`, create procs, classes and methods, patch bytecode with the current interpreter/namespace, and execute the top level.
- **Dump**: Pretty-print / disassemble `.tbcx` contents (header, literals, AuxData summaries, exception ranges, full instruction streams).
- **Tcl 9.1 aware**: Uses Tcl 9.1 internal bytecode structures, literal encodings, and AuxData types; exposes them via a stable binary format header (`TBCX_FORMAT = 9`).

---

## Commands (3)

### `tbcx::save in out`
Compile and serialize to `.tbcx`.

- **`in`** may be:
  - a **Tcl value** containing the script text (no file I/O needed),
  - an **open readable channel** (must be in binary mode; the command will set binary options safely),
  - a **path** to a readable file (opened, read, closed by TBCX).
- **`out`** may be:
  - an **open writable channel** (binary mode is enforced; channel is *not* closed),
  - a **path** to write a new `.tbcx` file (created/truncated; channel is closed on return).
- **Result**: returns the output channel handle or normalized output path.

What gets saved:

- The **top‑level** compiled block of the input script (code, literal pool, AuxData, exception ranges, local names/temps).
- All discovered **`proc`** bodies precompiled.
- TclOO **methods/constructors/destructors** precompiled.
- **Lambda literals** appearing in the script (e.g. `apply {args body ?ns?}` forms) are compiled and serialized as **lambda‑bytecode literals** so they do **not** recompile on first use after load.

### `tbcx::load in`
Load a `.tbcx` artifact into the **current namespace**, materialize procs and OO methods, rehydrate lambda bytecode literals, and execute the top‑level block.

- **`in`** may be an **open readable binary channel** or a **path** to a `.tbcx` file.
- **Result**: the top‑level executes (like `source`), procs, OO methods, and embedded lambda literals become available without re‑compilation.

### `tbcx::dump filename`
Produce a human‑readable string describing the artifact, including a **disassembly** of each compiled block and any **lambda literals**.

- **`filename`** must be a path to a readable `.tbcx` file.
- **Output**: header, summaries, literal listings, AuxData and exception info, plus disassembly of the top‑level/proc/method/lambda bytecode.

---

## How saving works

`tbcx::save` compiles the given script and **captures** definitions in several passes:

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

`tbcx::load` reads the header, deserializes top-level and nested bytecode blocks, and **finalizes** them as precompiled for the target interpreter:

- Top-level bytecode becomes a temporary proc executed once, then deleted, to mimic “run the script now.”
- Each saved `proc` is recreated and its body installed as a precompiled `ByteCode`.
- `oo::class create` is issued, `oo::define … superclass` applied, then each method/ctor/dtor is installed with its **compiled** body (and original body string kept).
- Namespace-eval bodies recorded during save are attached back to the literal sites in the top-level literal array, preserving semantics.

Endianness is detected and handled so that hosts read/write a consistent little-endian on disk.

---

## File Format (overview)

**Header** (compact, binary):
- Magic and format version; stamped Tcl version that produced the artifact.
- Size/count metadata for the **top‑level** block (code length, literal count, AuxData count, exception ranges, locals, max stack).

**Sections (in order):**
1. **Top‑level block** — code bytes, literal array, AuxData array, exception ranges, locals epilogue.
2. **Procs** — repeated tuples: fully‑qualified name, namespace, argument spec, compiled block.
3. **Classes** *(advisory)* — class FQN, superclasses list; creation still occurs by evaluating the builder at load time.
4. **Methods** — repeated tuples: class FQN, kind (method/ctor/dtor), name, arg spec, compiled block.
5. **Literals** supported inside blocks include: boolean, (wide)int/uint, double, **bignum**, string, bytearray, list, dict, embedded bytecode, and **lambda‑bytecode** literals for `apply`.

**AuxData** families covered include: jump tables (string or numeric), `dictupdate`, and `foreach` variants (legacy/new).

### Performance & Endianness

Artifacts are **portable** across little‑ and big‑endian hosts. On load, TBCX detects the host byte order once and uses a **tight, in‑place swap strategy** on bulk sections that require conversion (code/metadata arrays), avoiding per‑field branching. This keeps cross‑endian load time close to native‑endian performance.

---

## Guarantees & Semantics

- **Functional equivalence**: `tbcx::load` aims to be *functionally identical* to `source` of the original script. Differences are intentionally limited to avoiding re‑parse/re‑compile time.
- **Namespaces**: Loading happens in the **caller’s current namespace**; saved blocks carry enough namespace metadata to bind compiled code correctly. Lambda literals that include a namespace element keep that association.
- **Sanity limits** (caps): code size, literal count (including lambdas), AuxData count, exception ranges, string lengths.

---

## Build

This package uses **Tcl 9.1+** APIs and selected internals (e.g., `tclInt.h`, `tclCompile.h`).
You’ll need Tcl 9.1 headers/libs on your include path and to build as a standard loadable extension.
The entry point `tbcx_Init` registers the commands and provides `tbcx 1.0`.

Example (TEA Linux/macOS):

```sh
./configure
make install
make test
```

---

## Usage notes & caveats

- **Security**: Loading a `.tbcx` executes code (top-level) and installs commands/classes. Only load artifacts you trust.
- **Compatibility**: `TBCX_FORMAT` is `9` (Tcl 9.1). Different formats are rejected during read.
- **AuxData coverage**: Save asserts that all AuxData items in a block are of supported kinds.
- **OO coverage**: Supports `oo::class create`, `oo::define` (method/constructor/destructor/superclass/eval); merges static and dynamic captures with “last wins” semantics.
- **Namespace bodies**: `namespace eval` body literals called from top-level bytecode are detected and serialized separately.
- **Endianness**: Host endianness is detected at runtime and helpers produce/read little-endian on disk.

---

## Project layout

- `tbcx.h` — shared definitions (header layout, tags, limits)
- `tbcx.c` — package init, byte‑order detection, type discovery, command registration
- `tbcxsave.c` — compile & serialize
- `tbcxload.c` — deserialize & materialize
- `tbcxdump.c` — disassembler/dumper

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
