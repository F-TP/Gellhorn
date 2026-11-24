# Issue ledger

Ledger of **what people are sending** to the project's GitHub home, plus our own tracked findings for the boot port.
Each row carries area, state and a one-line evidence. Counts, error classes and commit ids only — no game data, package
paths, shader bytes or hashes. Maintained by the issue-tracker agent as a standing, read-only-plus-this-file role.

**Repo:** `F:\leonida`, branch `main`, HEAD `443639062` · **GitHub:** [luizwidmer/palmetto](https://github.com/luizwidmer/palmetto)
**Updated:** 2026-11-24 · **Incoming reports:** 2 open (#2, #3) · **Ours open:** 3 (O1=#4, O2=#5, O3) · **Fixed:** 5 · **Open PRs:** 0.

**Policy: ledger only.** Do not open, comment on, close, delete or edit GitHub issues/PRs, or commit/push, without an
explicit ask. Draft replies below are for lead approval; nothing has been posted.

## Incoming reports (GitHub)

| # | sender | state | what they sent | tracking |
|---|---|---|---|---|
| #2 | bittertea0323 | open · ours to answer | Independent reproduction, Windows 10 + RTX 4070 SUPER: two host build blockers (a .NET SDK pin and one unit test under a newer MSVC), 26,633 SPIR-V modules, a run reaching the documented frontier at 1,654 D3D12 calls, slot 108 publishing 0, then a fault ten frames deep in title code. Asks (1) whether that title-side fault is the expected consequence of the slot-108 answer, and (2) whether console GPU-VA dereference belongs to this layer or the translator | External; different stop on an older tree, **not a duplicate of O1/O2**; two questions open — draft below |
| #3 | bittertea0323 | open · ours to answer | FSPX corpus measured on a second host: 18 of 29 translate, the 11 failures are `fall-through into undecodable words` and cluster on wave32; per-program word counts; program 07 is one undecodable word away. Asks for a preferred report shape (decoder stop word per program) | Addressed by `0276f2b1f` (repair → 29/29; the boot does not request FSPX programs); unanswered — draft below |

Closed: #1 — PR "Main". No comments exist on any issue.

## Draft replies (not posted — awaiting lead approval)

- **#2:** "Thanks — slot 108 is answered on the current tree (`c3c7a7b83` returns a real resource allocation info), so
  that fault should no longer be the frontier; please re-run `main` before we chase the title-side path. If it persists,
  it is the console-GPU-VA-as-CPU-pointer assumption — runtime memory-model work rather than a translator gap — and a
  fault-time register and stack dump is what we would need (a stack unwind is on our list)."
- **#3:** "Thanks for the per-program counts — the build now repairs each FSPX listing before translating
  (`tools/widen_smem_decode.py`) and measures 29/29 passing `spirv-val` (22/29 without), so the wave32 cluster you
  measured is closed in tree. Per-program stop word is still the report shape we would want for any surviving refusal;
  we will keep it in our ledger."

## Our findings — open

| id | area | item | state | evidence |
|---|---|---|---|---|
| O1 | runtime | Renderer's three-slot record array (216 B; 72-byte records) is never initialised | open · [#4](https://github.com/luizwidmer/palmetto/issues/4) (ours, maintainer-tracked) | the 216-byte allocator exists but nothing reaches it (no call, jump or table pointer); the global watcher sees no transition; `LEONIDA_SEED_GLOBAL` moves the stop to a `ud2` trap (`1848f2666`) |
| O2 | runtime | Boot stop `game+0x2484133`: a title-side call leaves a callee-saved pointer clobbered, then the boot faults reading through it | open · [#5](https://github.com/luizwidmer/palmetto/issues/5) (ours, maintainer-tracked) | at the fault `rbx` = `0x100000000`, not the value the static `lea` computes; the TLS rewrite is validated and the mapping ruled out (`443639062`); queue slot 35 is not the cause (`818c739af`) |
| O3 | runtime/diagnostics | **Adopt crash-memory save + all-thread stack dump** | open (no issue) | `docs/competitor_intel.md` records the other public attempt saving title memory at the fault and dumping every thread's stack, naming faults `module+rva` — we have neither, which is why the current call chain is reconstructed from static disassembly. **Lead-designated highest-value debugging gap.** |

**Highest priority:** O3 — the debugging gap that would shorten the current investigation (the clobbering value would be
in saved memory; the call chain would be read, not reconstructed). Incoming: #2's two questions are the only external
items awaiting a maintainer answer.

## Our findings — fixed

| area | item | state | evidence |
|---|---|---|---|
| build | Pipeline-library extraction and FSPX listing repair wired into `make_game.py` | fixed `0276f2b1f` | FSPX 29/29 translate with the repair (22/29 without); library 42/42 on the 42-program extraction, 1,047/1,684 on a real package subset |
| runtime | DirectStorage zlib stage inflates instead of failing with `E_NOTIMPL` | fixed `ae876f4b5` | 2,238 → 11,894 D3D12 calls; 0 zlib requests fail; the "not implemented" line is gone |
| runtime | AMD-only `clzero` emulated on Intel hosts | fixed `f76446847` | illegal-instruction fault gone; frame-loop event waits reached, 11,906 calls, 0 pipeline failures |
| runtime | Console device slot 108 answers a real resource allocation info | fixed `c3c7a7b83` | null-object stop at `game+0xCC2861` gone; the run continues to `game+0x24844A9` |
| runtime | Console queue slot 35 calibration answered (plus nonvolatile register dump) | fixed `818c739af` | stop unchanged at `game+0x2484133`, so not the cause; the dump now carries `rbx`, `r12`–`r15` |

## Notes (not bugs)

- **TLS rewrite (loader).** 53,246 `gs:[58h]` reads are rewritten to the TEB; `LEONIDA_TEB_CHECK=1` confirms
  `0x1480 + 8*slot` is right on this Windows build, and the faulting thread's TLS cells are correct at the fault
  (`443639062`, `91a01b5ea`). The other public attempt reached the same conclusion independently.
- **Pipeline-library translation is partial by design.** 42/42 on the dedicated 42-program extraction, 1,047/1,684 (~62%)
  on a real package subset (`0276f2b1f`); the FSPX set needs the `widen_smem_decode` repair. Not a boot front today.
- **Historical: the eleven "missing" pipeline programs** were zeroed library reads from the refused zlib stage, not a
  second container or a load-time patch; resolved by `ae876f4b5` (see `docs/pipeline_program_source.md`).
- **docs.** `docs/competitor_intel.md` now records how the other public attempt (<https://github.com/Marvisak/VIctory>) is
  built: DXIL compiled at pipeline creation + host D3D12 vs our SPIR-V + Vulkan; a prepare-then-map loader; a crash
  reporter with stack unwind; a mechanically enforced asset allowlist. Licence is GPL-3.0 like ours, so reuse is
  permitted with attribution — the diagnostics above are the parts worth adopting first.
