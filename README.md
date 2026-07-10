# TBCX — Precompiled Tcl 9.1 Bytecode (save / load / dump)

TBCX is a C extension for the **Tcl 9.1 family** that **serializes** compiled Tcl bytecode (plus enough metadata to reconstruct `proc`s, TclOO methods, and **lambda constructs**) into a compact `.tbcx` file — and later **loads** that file into another interpreter for fast startup with source-equivalent semantics. There's also a **disassembler** for human‑readable inspection.

For versions prior to Tcl 9.1, please check

-  [tclcompiler](https://github.com/tcltk-depot/tclcompiler)
-  [tbcload](https://github.com/tcltk-depot/tbcload)

---

## Quick Start

```tcl
package require tbcx

# Save: source text → .tbcx (body source stripped by default, following
# the tclcompiler/tbcload precedent for compiled deployment artifacts).
tbcx::save ./hello.tcl ./hello.tbcx

# Save with body source preserved for `info body` / `info class definition`
# round-trip fidelity, TIP #280 attribution, and introspection-based
# clone idioms.
tbcx::save ./hello.tcl ./hello.tbcx -include-source

# Load: .tbcx → installs procs/methods/lambdas and executes top level
# in the caller's current namespace, with `info script` set to the
# authored source path (source-equivalent semantics).
tbcx::load ./hello.tbcx

# Dump: pretty disassembly; includes preserved body source when present
# and the artifact's recorded source path in the header.
puts [tbcx::dump ./hello.tbcx]
```
---

## Features

- **Save**: Compile a script and write a `.tbcx` artifact containing:
  - Top-level bytecode (with literals, AuxData, exception ranges, local names)
  - All discovered `proc`s, each as precompiled bytecode with an optional source-text field
  - TclOO classes (advisory catalog of discovered class names for dump/introspection) and methods/constructors/destructors as precompiled bytecode with optional source-text fields. Class creation and superclass structure are reconstructed by executing the rewritten top-level script during `tbcx::load`, not from a standalone serialized class graph.
  - **Self methods** (`self method` inside `oo::define`) serialized with kind `TBCX_METH_SELF`; loaded via `oo::define { self method ... }` to preserve metaclass inheritance for subclasses
  - Lambda literals (`apply {args body ?ns?}` forms) compiled and serialized as lambda‑bytecode literals. A candidate is accepted only when its list value **round-trips faithfully** as a Tcl lambda — rebuilt the way the loader rebuilds it, including validation that the optional namespace element is absolute — so data lists that merely resemble lambdas remain ordinary data
  - Bodies found in `namespace eval $ns { ... }` and other script-body contexts (compiled and bound to the correct namespace)
  - **Authored source path**: when the input is a readable file path, the normalized source path is recorded in the header so `info script` at load time returns the original `.tcl` path — matching `Tcl_FSEvalFileEx`/`source` semantics
  - **Optional body source preservation** (`-include-source` flag): per-proc and per-method LPString fields carrying the authored body text. Without the flag, bodies are stripped and the loader installs a diagnostic sentinel (see *Source preservation* below).
  - **Method visibility (scope)**: each method record carries a scope byte — default (from name case), forced public, unexported, or TIP #500 true-private — captured from `-export`/`-unexport`/`-private` definition options, lexical `private { ... }` / `self { ... }` context, and same-body `export`/`unexport`/`self export`/`self unexport` commands inside builder bodies. The loader re-applies the recorded visibility after installing each precompiled body, so exported PascalCase callbacks stay dispatchable and true-private methods stay private. Constructors/destructors always carry the default scope.
  - **Method origin**: each method record carries an origin byte distinguishing class-definition methods (`oo::define` / `oo::class create`) from per-object methods (`oo::objdefine`). Records are keyed by *(class FQN, kind, name, origin)* at load, so a class-instance method and a per-object method with the same name coexist — each gets its own precompiled body. Per-object methods (including TIP #500 true-private per-object methods) are installed precompiled via builder replay.
  - **Instruction-level body detection** (Phase 1): analyzes `invokeStk` patterns to identify and precompile bodies for `eval`, `uplevel`, `try`/`on`/`trap`/`finally`, `catch`, `foreach`, `while`, `for`, `if`/`elseif`/`else`, `time`, `timerate`, `dict for`/`map`/`update`/`with` (including FQN `::tcl::dict::*` forms), and `self method`
  - **Unpushed literal detection** (Phase 2): identifies dead-reference body literals from Tcl 9.1's inline-compiled `foreach`/`lmap` loops (compiled to `foreach_start` opcodes) and precompiles them
  - **O(1) opcode dispatch**: instruction scanner uses a 256-entry `opMap[]` lookup table covering all 120 Tcl 9.1 instruction types, replacing per-instruction string comparisons
  - **Bytearray detection**: strings with bytes ≥ 0x80 are probed and emitted as `TBCX_LIT_BYTEARR` to avoid UTF-8 encoding corruption on round-trip
  - **`startCommand` stripping**: at save time, `startCommand` debugging instructions (~15% of bytecode) are replaced with `nop` bytes, reducing execution overhead while preserving jump offsets and exception ranges
  - **Cross-interpreter support**: body literals are emitted as `TBCX_LIT_BYTESRC` (bytecode + preserved source text); loaded with `setPrecompiled=0` so Tcl can gracefully recompile from source in child interpreters or after epoch bumps
- **Load**: Read a `.tbcx`, reconstruct precompiled procs, method bodies, and literal lambdas, then execute the top-level block in the **caller's current namespace** with source-equivalent frame and scope semantics. `iPtr->scriptFile` is set to the artifact's recorded authored path for the duration of the evaluation so `info script` returns the correct value. Class creation, namespace setup, and other top-level effects happen naturally when the rewritten script runs.
- **Dump**: Pretty-print / disassemble `.tbcx` contents — header (including the authored source path), literals, AuxData summaries, exception ranges, full instruction streams, and preserved body source text (indented inline) when `-include-source` was used at save time.
- **Safe interp support**: In safe interpreters, `tbcx_SafeInit` provides the package and type infrastructure but does **not** register any `tbcx::*` commands. A parent interpreter may selectively grant access with `interp alias` or `interp expose`.
- **Tcl 9.1 aware**: Uses Tcl 9.1 internal bytecode structures, literal encodings, and AuxData types; exposes them via a stable binary format header (`TBCX_FORMAT = 92`).

---

## Source preservation

TBCX follows the TclPro tclcompiler/tbcload precedent of stripping body source from compiled deployment artifacts: **body source is stripped by default**. Without `-include-source`, every proc and method body is emitted with an empty source-text field on the wire, and the loader installs a two-line diagnostic sentinel as the body's string representation:

```
# tbcx: body source stripped at save time; info body unavailable
error "tbcx: introspection-based cloning is not supported for this artifact"
```

The shape matches tclcompiler's canonical pattern (Compiler8.html, "Example 1: Cloning Procedures"):
1. A `#` comment line — shows up at the top of any error traceback, making the cause visible at a glance.
2. An `error` call — so that if downstream code does `proc new [info args orig] [info body orig]` and invokes `new`, the clone fails loudly rather than silently running as a no-op.

**When to use `-include-source`.** Some Tcl idioms depend on `info body` returning the authored source text — cloning a command via `proc $new [info args $src] [info body $src]`, `info class definition`, TIP #280 line-number attribution in stack traces, and other introspection-driven diagnostic tools. For artifacts whose consumers need these idioms, pass `-include-source` to `tbcx::save`. The wire-format overhead is proportional to the aggregate source text of all procs and methods.

**Comparison with tclcompiler/tbcload:**

| Aspect | tclcompiler/tbcload | tbcx |
|---|---|---|
| Default for body source | **strip always** | **strip by default** |
| Preservation available? | No | Yes, via `-include-source` |
| Sentinel shape | `# Compiled -- no source code available\n error "called a copy of a compiled script"` | `# tbcx: body source stripped at save time; info body unavailable\n error "..."` (same pattern) |
| Sentinel storage | Synthesized at runtime by procbody type's `updateStringProc` | Stored in artifact as `""`; loader substitutes fixed string |
| Wire format | ASCII-encoded Tcl script (`.tbc`) | Raw binary with magic+format header (`.tbcx`) |

---

## Commands (4)

### `tbcx::save in out ?-include-source?`
Compile and serialize to `.tbcx`.

- **`in`** is resolved in this order:
  1. **open channel name** — if the value names an existing open channel, it is read as text (encoding is left as-is — the caller controls it);
  2. **readable file path** — if the value is a path to a readable file, it is opened in text mode (default UTF-8 encoding), read, and closed by TBCX. The normalized path is recorded in the artifact header for `info script` restoration at load time.
  3. **literal script text** — otherwise the value is treated as inline Tcl script text. Consequently, a value that looks like a path but is not currently readable is compiled as script text, not reported as a file-open error. No source path is recorded.
- **`out`** may be:
  - an **open writable channel** — binary mode (`-translation binary -eofchar {}`) is enforced; the channel is *not* closed. Note: the caller's channel settings are mutated and not restored.
  - a **path** — TBCX writes a temporary file in the target directory and renames it into place only after serialization succeeds, so a failed save never leaves a truncated artifact at the final path.
- **`-include-source`** — optional flag. Embeds authored proc/method body source text in the artifact. Required if consumers need `info body`, `info class definition`, TIP #280 line numbers, or introspection-based cloning to work. Artifact size grows proportional to aggregate source text.
- **Result**: returns the output channel handle or normalized output path.

What gets saved:

- The **top‑level** compiled block of the input script (code, literal pool, AuxData, exception ranges, local names/temps).
- The **authored source path** (when `in` was a readable file) — stored as an LPString in the header.
- All discovered **`proc`** bodies, precompiled with correct namespace bindings. Conflicting definitions across `if`/`else` branches are handled via indexed proc markers. Each proc record carries an optional body-source LPString (populated under `-include-source`).
- TclOO **methods/constructors/destructors**, precompiled. Each method record carries a visibility scope byte (default / public / unexported / TIP #500 true-private), an origin byte (class definition vs. `oo::objdefine` per-object definition), and an optional body-source LPString (populated under `-include-source`).
- **Lambda literals** appearing in the script (e.g. `apply {args body ?ns?}` forms) are compiled and serialized as **lambda‑bytecode literals** so they do **not** recompile on first use after load.
- **Namespace eval bodies** and other script-body literals (try, foreach, while, for, catch, if/elseif/else bodies) are detected and pre-compiled to bytecode when safe to do so.

### `tbcx::load in`
Load a `.tbcx` artifact, materialize procs and OO methods, rehydrate lambda bytecode literals, and execute the top‑level block in the caller's current namespace.

- **`in`** may be an **open readable binary channel** or a **path** to a `.tbcx` file.
- **Result**: the top‑level executes (like `source`), procs, OO methods, and embedded lambda literals become available without re‑compilation.

Load semantics for source-equivalent behavior:

- The top-level block evaluates in the caller's **current namespace** (via `Tcl_GetCurrentNamespace`), not the global namespace. A module invoked inside `namespace eval ::foo { tbcx::load $p }` sees `::foo` just as `source $src` would.
- `iPtr->scriptFile` is saved, set to the artifact's recorded authored source path — falling back to the `.tbcx` path itself when the header carries none and the artifact was loaded from a file — then restored after evaluation. `info script` returns the authored `.tcl` path during load, matching `Tcl_FSEvalFileEx` (`tclIOUtil.c:1806-1807`). When loading from an already-open channel and the header carries no path, `info script` is left untouched.
- Compiled locals for the top-level frame are attached to the caller's active variable frame (`varFramePtr`), not the global frame.
- When a `.tbcx` is wrapped inside a proc that the user invokes externally, callers should use `uplevel 1 [list tbcx::load $path]` to reach the caller's frame — identical to the pattern already required for `source` in the same position.

### `tbcx::dump filename`
Produce a human‑readable string describing the artifact: the header (including the recorded authored source path), a **disassembly** of each compiled block with literal operands annotated from the instruction table's operand types, any **lambda literals**, and per‑method **visibility scope** and **origin** lines. Each proc/method record shows its body source inline, or `<stripped at save time>` for default‑mode artifacts.

- **`filename`** must be a path to a readable `.tbcx` file.
- **Output**: header (including authored source path, if any), summaries, literal listings, AuxData and exception info, disassembly of the top‑level/proc/method/lambda bytecode, and **preserved body source text** (indented inline, no truncation) for each proc and method when the artifact was built with `-include-source`.

### `tbcx::gc`
Explicitly purge stale entries from the per‑interpreter lambda shimmer‑recovery registry (the ApplyShim). This is normally not needed — stale entries are purged lazily on each `tbcx::load` call — but can be useful in long‑running interpreters that load many `.tbcx` files and want to reclaim memory sooner.

- Takes no arguments.

---

## How saving works

`tbcx::save` may be invoked from inside a proc: the top‑level script is always compiled at true global scope (the compile environment's proc pointer is cleared and the caller frame's LocalCache is hidden for the duration of the compile), because a serialized top level carries no local‑variable table and must never index the calling proc's local slots.

`tbcx::save` compiles the given script and **captures** definitions in a single pass:

- **Capture and rewrite**: `CaptureAndRewriteScript` walks the script's token tree once, extracting `proc`, `namespace eval`, `oo::class create`, `oo::define` (method/constructor/destructor/self method), and `oo::objdefine` forms. It records each method's visibility scope (from `-export`/`-unexport`/`-private` options, `private { ... }` / `self { ... }` lexical context, and same-body `export`/`unexport` commands) and its origin (class vs. per-object). It simultaneously produces a **rewritten** script where captured method/constructor/destructor bodies are replaced with recognizable stub sentinels and captured proc bodies with indexed markers, ensuring the top-level bytecode doesn't redundantly contain their full source and letting the loader patch only what the saver actually stubbed.
- **Namespace body scanning**: The rewritten script and captured definition bodies are scanned (`ScanScriptBodiesRec`) for nested script-body patterns — `namespace eval`, `try`/`on`/`trap`/`finally`, `foreach`, `while`, `for`, `catch`, `if`/`elseif`/`else`, `uplevel`, `eval`, `dict for`/`map`/`update`/`with`, `lsort -command` — building a mapping from body text to namespace FQN.
- **Pre‑compilation**: Matched namespace eval body literals in the top-level literal pool are compiled into a **side table** (never modifying the pool itself) so they serialize as bytecode rather than source text.
  - Strips `startCommand` debugging instructions from all compiled blocks (replaced with `nop` bytes for ~15% leaner execution).
- **Instruction scanning** (`InstrScanBodyLiterals`): Two-phase bytecode analysis runs on each compiled block:
  - **Phase 1** (invokeStk analysis): Models the operand stack using the process-wide 256-entry `opMap[]` dispatch table, initialized once from the instruction table during package/type initialization under the type-init mutex and immutable thereafter, with O(1) per-instruction dispatch. Tracks literal indices through `push`/`loadStk`/`storeStk`/`swap` etc. to identify which literal is the command argument for each `invokeStk` call. Marks body literals for `eval`, `try`/`on`/`trap`/`finally`, `catch`, `foreach`, `while`, `for`, `if`, `uplevel`, `time`, `timerate`, `dict for`/`map`/`update`/`with` (including FQN `::tcl::dict::*`), and `self method`.
  - **Phase 2** (unpushed literal detection): For blocks containing `foreach_start` opcodes, identifies literal pool entries that are never referenced by any `push` instruction — these are dead body-text references kept by Tcl's compiler for error reporting. Marks them for precompilation via `LooksLikeScriptBody()` filtering.
- **Bytearray detection**: `WriteLit_Untyped` probes string literals for bytes ≥ 0x80; if all code points ≤ 255, emits as `TBCX_LIT_BYTEARR` to prevent UTF-8 encoding corruption on round-trip.
- **Source path recording**: When the input is a readable file path, `Tcl_FSGetNormalizedPath` records the authored source path in the header for later `info script` restoration at load time.
- **Serialize**: Emit:
  - A **header** (magic, format version, Tcl version, code length, exception/literal/AuxData/local counts, max stack depth, authored source path LPString).
  - The **top-level compiled block** (code bytes, literals, AuxData, exception ranges, locals epilogue). Captured proc bodies are stripped during this phase. Body literals are emitted as `TBCX_LIT_BYTESRC` (source text + compiled bytecode).
  - A table of **procs**: name FQN, namespace, argument spec, body source text LPString (empty without `-include-source`), then the separately compiled body block.
  - **Classes** *(advisory)* — discovered class names, sorted alphabetically for deterministic output (currently `nSupers = 0`; actual class structure is reconstructed by the top-level script at load time), then **methods** in definition order (class FQN, kind 0–4, scope byte, origin byte, name, argument spec, body source text LPString, compiled body block). Self methods use kind 4 (`TBCX_METH_SELF`); ctor/dtor records always carry the default scope.
  - A final flush of any buffered output.

**Runaway protection**: The serializer tracks total literal calls, block calls, recursion depth, and output bytes. If any limit is exceeded (2M literals, 256K blocks, depth 64, or 256 MB output), serialization aborts with a diagnostic error.

Supported literal kinds: **bignum, boolean, bytearray, dict** (insertion order preserved), **double, list, string, wideint, wideuint, lambda‑bytecode, bytesrc** (bytecode + source text for cross-interp recompilation).

Supported AuxData: **jump tables (string and numeric), dict-update, NewForeachInfo**.

---

## How loading works

`tbcx::load` reads the header, validates magic/format/Tcl‑version compatibility, then deserializes sections:

1. **Header source path**: If the header carries a non-empty authored source path (artifacts built from a file), it's read into an owned Tcl_Obj that the loader will use for `iPtr->scriptFile` during top-level evaluation.
2. **Top-level block**: Deserialized and marked `TCL_BYTECODE_PRECOMPILED` so Tcl skips compile-epoch checks and executes the bytecode directly. `TBCX_LIT_BYTESRC` literals within the block are loaded with `setPrecompiled=0` and their source text restored as string rep, allowing Tcl to recompile from source when needed (e.g. cross-interpreter evaluation or epoch mismatch).
3. **Procs**: A temporary **ProcShim** intercepts the `proc` command by patching its `objProc2` and `nreProc2` handler pointers in place (no renaming). When the top-level block evaluates a `proc` call matching a saved definition (by FQN + argument signature, or by indexed marker for conflicting definitions), the shim substitutes the precompiled body. Records for a given key are kept in a definition-order FIFO, and only stubbed bodies (recognizable sentinels the saver wrote) are ever patched — a `proc` the saver left verbatim keeps its authored body. The body's string representation is set to the preserved source text (via `Tcl_InvalidateStringRep` + `Tcl_InitStringRep`) if the artifact was built with `-include-source`, or to the diagnostic sentinel otherwise. Unmatched `proc` calls pass through to Tcl's original handler.
4. **Classes and methods**: An **OOShim** intercepts `oo::define` (and `oo::objdefine` when available) by patching the commands' `objProc2`/clientData handlers in place — the commands are *not* renamed. Method records are keyed by *(class FQN, kind, name, origin)* and kept in definition-order FIFOs; each definition site the rewritten script executes consumes the front record, and only bodies carrying the tbcx stub sentinel are patched, so a method left verbatim by the saver is never overwritten by a stale same-key record. After installing each body, the loader applies the record's visibility scope (public / unexported / TIP #500 true-private). Constructors and destructors use a create-then-swap pattern (placeholder body `";"` → TclOO builds dispatch → bytecode swap) to preserve `next` routing through the constructor chain. **Self methods** (kind 4) are installed via `oo::define CLASS { self method NAME ARGS BODY }` through the original handler, which properly sets up the metaclass inheritance chain so subclass class-objects inherit the method. **Per-object methods** are installed precompiled by replaying the `oo::objdefine` builder body and matching its stubbed method statements against object-origin records; true-private per-object methods are re-created through the flat `private` form so TclOO wires them correctly. Each method body likewise receives either the preserved source text or the sentinel, depending on the artifact.
5. **Rename/delete safety**: The shims register command traces on `proc`, `oo::define`, `oo::objdefine`, and `apply`. If any of these is renamed or deleted while a shim is active, the trace restores the original handlers in place immediately (a Command struct survives a rename), so a renamed command never remains wired to freed shim state.
6. **Lambda recovery**: An **ApplyShim** is installed as persistent per-interpreter `AssocData`. When a precompiled lambda's `lambdaExpr` internal rep gets evicted by shimmer, the shim detects the missing rep on the next `[apply]` call and re-installs the precompiled `Proc*` from its registry before forwarding to Tcl's real `[apply]`.
7. **Top-level execution**: The precompiled top-level block is evaluated via `Tcl_EvalObjEx` with flags `0` (no `TCL_EVAL_GLOBAL`), running in the caller's current namespace. `iPtr->scriptFile` is saved, set to the header's source path (or the tbcx artifact path as a fallback), and restored after evaluation — matching `Tcl_FSEvalFileEx`'s scriptFile handling. Compiled locals for the top-level frame are installed on the caller's active variable frame (`varFramePtr`) by linking named variables to existing same-name variables in the caller's scope (via `TopLocals_Begin`/`TopLocals_End`). `TCL_RETURN` is handled the same way `source` does — converting it to `TCL_OK` with the return value as the result.
8. **Cleanup**: The ProcShim and OOShim are removed (original command handlers restored). The ApplyShim persists for the interpreter's lifetime to support lambda shimmer recovery.

Endianness is detected and handled so that hosts read/write a consistent little-endian format on disk.

---

## File Format (overview)

**Header** (compact, binary, little‑endian):

| Field | Type | Description |
|-------|------|-------------|
| `magic` | u32 | `0x58434254` ("TBCX") |
| `format` | u32 | `92` (Tcl 9.1, v92 feature set) |
| `tcl_version` | u32 | `maj<<24 \| min<<16 \| patch<<8 \| type` |
| `codeLenTop` | u64 | Code byte count for top-level block |
| `numExceptTop` | u32 | Exception range count |
| `numLitsTop` | u32 | Literal count |
| `numAuxTop` | u32 | AuxData count |
| `numLocalsTop` | u32 | Local variable count |
| `maxStackTop` | u32 | Maximum stack depth |
| `sourcePath` | LPString | Authored source file path (empty for inline/channel inputs) |

**Sections (in order):**
1. **Top‑level block** — code bytes, literal array, AuxData array, exception ranges, epilogue (maxStack, reserved, numLocals, local names).
2. **Procs** — u32 count, then repeated tuples: name FQN (LPString), namespace (LPString), argument spec (LPString), body source text (LPString — empty without `-include-source`), compiled block.
3. **Classes** *(advisory)* — u32 count, then class FQN; currently records discovered class names for dump/introspection only. Class creation and superclass structure are reconstructed by the rewritten top-level script at load time.
4. **Methods** — u32 count, then repeated tuples: class FQN, kind (u8: 0=inst, 1=class, 2=ctor, 3=dtor, 4=self), scope (u8, see *Method scopes* below; ctor/dtor always 0), origin (u8: 0=class definition, 1=object definition), name (empty for ctor/dtor), argument spec, body source text (LPString — empty without `-include-source`), compiled block.

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
| 10 | BYTESRC | source text (LPString) + ns FQN + compiled block (enables cross-interp recompilation) |

**AuxData tags** (u32):
| Tag | Kind | Payload |
|-----|------|---------|
| 0 | JT_STR | u32 count; key LPString + u32 offset per entry |
| 1 | JT_NUM | u32 count; u64 key + u32 offset per entry |
| 2 | DICTUPD | u32 length; local indices |
| 3 | NEWFORE | u32 numLists, u32 loopCtTemp, u32 firstValueTemp, u32 numLists (dup), then per-list var indices |

**Method kinds** (u8):
| Kind | Name | Description |
|------|------|-------------|
| 0 | INST | Instance method |
| 1 | CLASS | Class method (classmethod) |
| 2 | CTOR | Constructor |
| 3 | DTOR | Destructor |
| 4 | SELF | Self method (installed via `oo::define { self method }` for metaclass inheritance) |

**Method scopes** (u8, decoupled from TclOO's internal flag values so the wire format stays stable):
| Scope | Name | Description |
|-------|------|-------------|
| 0 | DEFAULT | Visibility follows the method-name case (ctor/dtor always use this) |
| 1 | PUBLIC | Forced exported (`-export` option, or in-body `export`) |
| 2 | UNEXPORTED | Forced unexported (`-unexport` option, or in-body `unexport`) |
| 3 | TRUE_PRIVATE | TIP #500 true-private (`-private` option, or `private { ... }` block) |

**Method origins** (u8):
| Origin | Name | Description |
|--------|------|-------------|
| 0 | CLASS | Captured from `oo::define` / `oo::class create` |
| 1 | OBJECT | Captured from `oo::objdefine` (per-object method) |

Records are keyed at load by *(class FQN, kind, name, origin)*, so a class-instance method and a per-object method sharing a name coexist. For a given key, records form a definition-order FIFO consumed positionally as the rewritten top-level executes.

**LPString**: a u32 byte-length followed by that many raw bytes (no NUL terminator on disk).

---

## Guarantees & Semantics

- **Functional equivalence**: `tbcx::load` aims to be *functionally identical* to `source` of the original script. With `-include-source`, byte-for-byte introspection round-trip (`info body`, `info class definition`, `info class constructor`) is preserved. TclOO method visibility — exported, unexported, or TIP #500 true-private, however it was expressed at definition time — survives the round-trip. The authored source path is recorded at save time and reinstalled during load, so `info script` returns the original `.tcl` path and `[file dirname [info script]]` works naturally.
- **Namespaces and frames**: The top-level block is evaluated in the caller's current namespace (no `TCL_EVAL_GLOBAL`). Saved blocks carry namespace metadata to bind compiled code correctly. Lambda literals that include a namespace element keep that association. When `tbcx::load` is wrapped inside a proc, callers should use `uplevel 1 [list tbcx::load $path]` to reach the caller's frame — same pattern as `source`.
- **Version check**: The loader requires an exact major.minor Tcl version match (e.g. 9.1). Bytecode instruction semantics can change between minor versions.
- **Sanity limits**: Code ≤ 64 MiB; literal/AuxData/exception pools ≤ 1M entries; LPString ≤ 4 MiB; output ≤ 256 MB; recursion depth ≤ 64.

---

## Build

This package uses **Tcl 9.1** APIs and selected internals (e.g., `tclInt.h`, `tclCompile.h`).
You'll need Tcl 9.1 headers/libs on your include path and to build as a standard loadable extension.
The entry point `tbcx_Init` registers commands and provides `tbcx 1.12`.
The safe entry point `tbcx_SafeInit` provides the package and type infrastructure but registers **no commands**; use `interp alias` or `interp expose` from a parent interpreter to grant selective access.

Example (TEA Linux/macOS):

```sh
./configure
make install
make test
```

On Windows, build with `nmake -f win/makefile.vc`. The makefile passes `/std:c11 /experimental:c11atomics` (the code uses C11 atomics, and MSVC needs both switches to enable them) and links no TomMath import library — bignum operations go exclusively through Tcl's TomMath stubs table, so the extension links against both static and dynamic Tcl builds.

The test suite ships with **643 test declarations across 32 test files (~10,500 lines)** — four of them live in a parameterized helper invoked 35 times, expanding to roughly 780 executed test cases — covering datatypes, auxdata round-trips, exception handling, proc and OO lifecycles, namespace binding, channel I/O, multi-interpreter and threaded scenarios (including opcode-cache races and rename-during-load safety), Unicode edge cases, stress tests, security regressions, method-scope/visibility matrices checked against a `source` oracle, and specific regression tests for body source round-trip, sentinel behavior, cloned-body failure semantics, list-literal quoting, and `info script` path resolution.

---

## Usage notes & caveats

- **Security**: Loading a `.tbcx` executes code (top-level) and installs commands/classes. Only load artifacts you trust.
- **Compatibility**: `TBCX_FORMAT` is `92` (Tcl 9.1). An artifact whose format does not match is rejected cleanly at load time as a bad/unknown artifact format (`tbcx: bad header`); the required exact major.minor Tcl version match is a separate check. Re-run `tbcx::save` to regenerate v91 artifacts in v92.
- **AuxData coverage**: The saver asserts that all AuxData items in a block are of known kinds (jump tables, dict-update, NewForeachInfo). Unknown kinds cause the save to abort.
- **OO coverage**: Supports `oo::class create`, `oo::define` (method/classmethod/constructor/destructor/self method plus declarative keywords like variable/superclass/mixin/filter/forward), and `oo::objdefine`. A `self method` (or a method inside a `self { ... }` block) written in an `oo::objdefine` builder is not representable — `oo::objdefine` has no `self method` form — and is rejected at save time with a diagnostic rather than silently loading as a no-op. Builder-form class bodies are expanded into multi-word stubs for correct load-time reconstruction. Self methods (`self method` inside `oo::define`) are serialized with kind 4 (`TBCX_METH_SELF`) and loaded via `oo::define { self method ... }` to preserve metaclass inheritance for subclasses; their visibility (including in-body `self export`/`self unexport`) is folded into the scope byte, and their bodies never appear in the rewritten top-level script.
- **Method visibility**: Visibility expressed via `-export`/`-unexport`/`-private` options, `private { ... }` blocks (TIP #500 true-private), or in-body `export`/`unexport` commands is recorded per method and restored on load — for class methods and per-object methods alike.
- **Verbatim safety**: A method or proc whose name, arguments, or body is not a static literal (or whose target is a runtime variable) is left verbatim by the saver and compiles normally at load; the loader's stub-sentinel check guarantees such definitions are never overwritten by a precompiled record for the same name.
- **Lambda shimmer recovery**: Precompiled lambdas are registered in a persistent per-interpreter ApplyShim. If the `lambdaExpr` internal rep is evicted by shimmer, the shim transparently re-installs it on the next `[apply]` call.
- **Precompilation boundary**: TBCX precompiles bodies and lambdas only when they are present in statically identifiable literal positions. Strings assembled at runtime (e.g. with `format`, interpolation, or `list` construction) still round-trip correctly, but they remain ordinary data and compile at execution time when Tcl evaluates them.
- **OO coverage (runtime)**: TBCX preserves normal TclOO class/object construction semantics by executing the rewritten top-level script, while substituting precompiled bodies for recognized `oo::define` / `oo::objdefine` method forms. Class-instance and per-object methods sharing the same name coexist (distinct origin keys). Tested scenarios include class methods, self methods, per-object methods (including true-private ones), private methods, inheritance (including diamond), mixins, filters, forwards, abstract/singleton metaclasses, method rename/delete/export changes, metaclasses with `self method`, and `next`-based constructor chaining. Declarative TclOO builder commands (`variable`, `superclass`, `mixin`, `filter`, `forward`) are preserved in the rewritten top-level.
- **Multi-interpreter and threads**: TBCX follows Tcl's standard threading model: only the thread that created an interpreter may call `tbcx::save`, `tbcx::load`, `tbcx::dump`, or `tbcx::gc` on that interpreter. Multi-thread support means multiple independent interpreters (each used by its owning thread), not sharing one interpreter across threads. Calling a TBCX command from a non-owning thread returns `TCL_ERROR` with a diagnostic message; debug builds (or builds with `-DTBCX_THREAD_CHECKS`) additionally assert ownership inside internal helpers. Shared process-wide state (the save-side opcode dispatch table) is initialized exactly once, under a mutex, at package init, so concurrent `tbcx::save` from multiple threads is safe. Artifacts are designed to load into interpreters other than the originating one. Interpreter-specific state (ApplyShim lambda registry, load depth, OO shim state) is consolidated in a single per-interpreter record and cleaned up automatically when the interpreter is deleted.
- **`tbcx::gc`**: Safe to call before any load (no-op) and safe to call repeatedly. Does not interfere with subsequent save/load operations.
- **Load reentrancy**: Nested or reentrant `tbcx::load` calls are capped at depth 8 per interpreter.
- **Conflicting proc definitions**: When multiple branches define a proc with the same name (e.g. `if {$cond} {proc p ...} else {proc p ...}`), the saver emits indexed markers so the loader matches by position rather than by FQN alone.
- **Endianness**: Host endianness is detected at runtime; streams are always little-endian on disk.

---

## Project layout

- `tbcx.h` — shared definitions (header layout, tags, limits, buffered I/O types, save flags, sentinel)
- `tbcx.c` — package init, byte‑order detection, type discovery, command registration, safe init
- `tbcxsave.c` — capture, rewrite, compile, and serialize; `-include-source` handling
- `tbcxload.c` — deserialize, shim, materialize, and execute; scriptFile/namespace/frame handling
- `tbcxdump.c` — disassembler/dumper with body-source display

---

## Contributing

Issues and PRs are welcome.
Given the reliance on Tcl internals, please include **Tcl 9.1** details (commit/tag, platform, compiler) with any bug reports.

---

## License

MIT License. Copyright © 2025–2026 Miguel Bañón.

---

## Acknowledgements

Built on top of Tcl 9.1's bytecode engine, object types, AuxData, and TclOO.

Design of the strip-by-default mode with `#`-comment + `error` sentinel honors the 25-year ActiveState TclPro/tclcompiler/tbcload precedent.

Thanks to the Tcl/Tk community.
