# Lifter exception tests

From the project root on Windows:

```powershell
.venv\Scripts\python.exe -I tests/eh/run.py
```

This needs `deps/llvm` and VS 2022 Build Tools, and **does not need a game dump**.
It assembles `fixture.s` with `clang --target=aarch64-none-elf`, links its actual
CFI/LSDA with `ld.lld --eh-frame-hdr`, then wraps those bytes in the lifter's flat
ELF format. The wrapper adds a dynamic-symbol/JUMP_SLOT directory for the
fixture's PLT stubs and deliberately omits the `.eh_frame_hdr` section header.
Function extents come from the fixture's assembly symbols in a generated TSV.

The test runs the production `recomp.py full` command and compiles its output
at `-O2`. It links a DLL with the **unmodified** `game_shim_eh.c` and links a small
MSVC host with the **unmodified** `nncxxrt.c`, `nnunwind.c`, and `eh_resume.c`.
It matches the current link driver's combination of all main objects and the
declaration-only `eh_resume.c` marker source, checking for duplicate definitions.
The actual runtime handles `__cxa_throw`, CFI frame walking, the LSDA personality,
pad entry, `_Unwind_Resume`, begin/end catch, and the catch-wrapper longjmp.

Three throws must each run a cleanup and then the caller's catch-all handler,
returning 42 once. Assertions check pad-entry SP, x0/x1, saved x19, restored
x19–x29 and d8–d15, final SP/LR, exception allocation release, cleared exception
state, and import diagnostic restoration. An ordinary return from either
throwing call fails. Seven parser checks cover corrupt/truncated MOD0/EH data,
TSV disagreement, pads outside their owners, cyclic action chains, header/FDE
disagreement, and retention of terminate-only pads without catch wrappers.

This exercises real EH code with a synthetic guest image. It does not run
Strikers, initialize other host services, test typed/base-class catches,
rethrows, nested catches, termination execution, other images, or multiple
threads/fibers. The simple import dispatcher and fail-fast non-EH service stubs
are in `harness.c`; there is no replacement personality or unwinder.

Compiler inputs and binaries use a temporary directory, removed on exit.
Commands, output, parser results, and source hashes are saved in
`build/obj/eh-test.json`.

With the actual dump and full generated main available, also run:

```powershell
.venv\Scripts\python.exe -I tests/eh/audit_main.py
```

This independently re-parses main and checks every generated pad label,
dispatcher, owner extent, catch wrapper, and sorted table record. Its structural
result and dump/TSV hashes are in `build/obj/eh-main-audit.json`.
