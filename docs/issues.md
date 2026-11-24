# Issue ledger

Ledger of **what people are sending** to the project's GitHub home, plus our own tracked findings for the boot port.
Each row carries area, state and a one-line evidence. Counts, error classes and commit ids only — no game data, package
paths, shader bytes or hashes. Maintained by the issue-tracker agent as a standing, read-only-plus-this-file role.

**Repo:** `F:\leonida`, branch `main`, HEAD `fb93cf28c` · **GitHub:** [luizwidmer/palmetto](https://github.com/luizwidmer/palmetto)
**Updated:** 2026-11-24 · **Incoming:** 2 open (#2, #3 + 1 third-party comment) · **Ours open:** 5 (O1=#4, O3, O4, O5, O6) · **Fixed:** 6 · **Open PRs:** 0.

**Policy: ledger only.** Do not open, comment on, close, delete or edit GitHub issues/PRs, or commit/push, without an
explicit ask. Draft replies below are for lead approval; nothing has been posted.

## Incoming reports (GitHub)

| # | sender | state | what they sent | tracking |
|---|---|---|---|---|
| #2 | bittertea0323 | open · ours to answer | Independent reproduction, Windows 10 + RTX 4070 SUPER: two host build blockers (a .NET SDK pin and one unit test under newer MSVC), 26,633 SPIR-V modules, a run at the documented frontier, slot 108 publishing 0, then a fault ten frames deep in title code. Asks whether that fault is the expected slot-108 consequence, and whether console GPU-VA dereference is this layer or the translator | External, older tree, not a duplicate; Q2 is answered by O6, Q1 still open — draft below |
| #3 | bittertea0323 | open · ours to answer | FSPX corpus on a second host: 18/29 translate, 11 fail `fall-through into undecodable words` and cluster on wave32; per-program word counts; asks for a preferred report shape | Addressed by `0276f2b1f` (repair → 29/29); unanswered — draft below. **New comment (NinosTech, 09:21Z):** four FSPX translations from an older upstream revision cleared four lookup misses and made the 42 compute-pipeline calls succeed, but the captured frame still had 0 non-black pixels of 896,232; not matched to our cases — draft below |

Closed: #1 — PR "Main".

## Draft replies (not posted — awaiting lead approval)

- **#2:** "Thanks — slot 108 is answered on the current tree (`c3c7a7b83`), and the title-side fault is now understood as
  our unified-memory gap: the title CPU-dereferences a fabricated console GPU address, which is runtime work rather than
  translator work. Please re-run `main`; a fault-time register and stack dump is now available via `LEONIDA_CRASH_REPORT`."
- **#3:** "Thanks for the per-program counts — the build now repairs each FSPX listing before translating
  (`tools/widen_smem_decode.py`) and measures 29/29 passing `spirv-val` (22/29 without), so the wave32 cluster you
  measured is closed in tree. Per-program stop word is still the report shape we would want for any surviving refusal."
- **#3/NinosTech:** "Thanks — that matches the split we measure: resolving the lookups moves the 42 creations to
  success, while presentation stays black because our stop is title-side. We will check your four programs against our
  translated set."

## Our findings — open

| id | area | item | state | evidence |
|---|---|---|---|---|
| O1 | runtime | Renderer's three-slot record array (216 B; 72-byte records) is never initialised | open · [#4](https://github.com/luizwidmer/palmetto/issues/4) (ours, maintainer-tracked) | the allocator exists but nothing reaches it; the global watcher sees no transition; `LEONIDA_SEED_GLOBAL` moves the stop to a `ud2` trap (`1848f2666`) |
| O3 | runtime/diagnostics | Crash-memory save + all-thread stack dump | open, partial · faulting-thread `.pdata` unwind landed `7eab981ad` (`LEONIDA_CRASH_REPORT`) | crash-memory save and every-thread stacks still missing; `docs/competitor_intel.md` records both as the diagnostics that would have shortened this investigation |
| O4 | runtime | Console device slot 57 is **graphics PSO creation**, not a lifetime tracker; conversion to a PC graphics PSO in progress | open | probe `be9a2bc3f`: iid = IID_ID3D12PipelineState, descriptor = root signature + six native-code stage pairs (VS/PS/DS/HS/GS + one more); forwarding by slot made 10,242 graphics-PSO creations per boot fail as "CreateLifetimeTracker unsupported"; supersedes the tracker plan (`fb93cf28c`, `docs/boot_frontier.md`) |
| O5 | build (test hygiene) | `runtime/test/d3d12_existing_heap_test.cpp` fails on HEAD: `CreatePlacedResource` on an existing heap expects `E_NOTIMPL`, gets `E_INVALIDARG` (~L163) | open, low | not built by `_setup/build_tests.cmd` (verified: no reference), which is why it rotted; reported by the tracker agent's build |
| O6 | runtime | Unified-memory requirement: the title CPU-dereferences console GPU image addresses; our fabricated VAs (from `1 << 32`) are unmapped | open | `LEONIDA_BACK_IMAGES=1` makes the read succeed and moves the fault to a null+offset write, so resource GPU addresses must be CPU-visible (`e604c28a6`, `docs/boot_frontier.md`) |

**Highest priority:** O4 — it blocks the boot frontier: the current `game+0xcc2861` null container traces to graphics PSOs
the title never received. O3's remaining pieces are the highest-value debugging gap.

## Our findings — fixed

| area | item | state | evidence |
|---|---|---|---|
| runtime | Console queue slot 18 `GetDesc` struct-return order (the clobbered `rbx` at `game+0x2484133`) | fixed `4178c742c` | stop moved to `game+0xcc2861`, D3D12 calls 11,906 → 11,986 (was O2; GH #5 open — close on approval) |
| build | Pipeline-library extraction and FSPX listing repair wired into `make_game.py` | fixed `0276f2b1f` | FSPX 29/29 with the repair (22/29 without); library 42/42 on the 42-program extraction, 1,047/1,684 on a real package subset |
| runtime | DirectStorage zlib stage inflates instead of failing with `E_NOTIMPL` | fixed `ae876f4b5` | 2,238 → 11,894 D3D12 calls; 0 zlib requests fail |
| runtime | AMD-only `clzero` emulated on Intel hosts | fixed `f76446847` | illegal-instruction fault gone; frame-loop event waits reached, 11,906 calls, 0 pipeline failures |
| runtime | Console device slot 108 answers a real resource allocation info | fixed `c3c7a7b83` | null-object stop at `game+0xCC2861` gone; run continued to `game+0x24844A9` |
| runtime | Console queue slot 35 calibration answered (plus nonvolatile register dump) | fixed `818c739af` | stop unchanged at the time, so not the cause; dump carries `rbx`, `r12`–`r15` |

## Notes (not bugs)

- **TLS rewrite (loader).** 53,246 `gs:[58h]` reads are rewritten to the TEB; `LEONIDA_TEB_CHECK=1` confirms
  `0x1480 + 8*slot` is right on this Windows build, and the faulting thread's TLS cells are correct at the fault
  (`443639062`, `91a01b5ea`). The public attempt reached the same conclusion independently.
- **Pipeline-library translation is partial by design.** 42/42 on the dedicated 42-program extraction, 1,047/1,684 (~62%)
  on a real package subset (`0276f2b1f`); the FSPX set needs the `widen_smem_decode` repair. Not a boot front today.
- **Historical: the eleven "missing" pipeline programs** were zeroed library reads from the refused zlib stage, not a
  second container or a load-time patch; resolved by `ae876f4b5` (see `docs/pipeline_program_source.md`).
- **Lifetime-tracker prototype (evidence, not a fix).** Implemented correctly and unit-tested, then reverted because the
  boot regressed 11,986 → 1,653 calls through a wrong vtable slot — evidence that console slot 57 is not a tracker (`O4`).
- **docs.** `docs/competitor_intel.md` records how the public attempt (<https://github.com/Marvisak/VIctory>) is built
  (DXIL + host D3D12 vs our SPIR-V + Vulkan; prepare-then-map loader; crash reporter with stack unwind; mechanical asset
  policy); GPL-3.0 like ours, so reuse is permitted with attribution. The README progress tracker is generated from
  `docs/progress.json` by `tools/readme_progress.py` (`--check` verified passing this refresh; created `410b6a67b`,
  frontier corrected `fb93cf28c`).
