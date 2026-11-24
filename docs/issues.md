# Issue ledger

Ledger of **what people are sending** to the project's GitHub home, plus our own tracked findings for the boot port. Each row carries area, state and one-line evidence. Counts, error classes and commit ids only — no game data, package paths, shader bytes or hashes.
Maintained by the issue-tracker agent as a standing, read-only-plus-this-file role.

**Repo:** `F:\leonida`, branch `main`, HEAD `62b510467` · **GitHub:** [luizwidmer/palmetto](https://github.com/luizwidmer/palmetto)
**Updated:** 2026-11-24 · **Incoming:** 2 open (#2, #3 + 1 third-party comment) · **Ours open:** 8 (O1=#4, O3 partial, O4, O5, O6, O7, O8, O9) · **Fixed:** 7 · **#5 closed:** 1 · **Open PRs:** 0.

**Policy: ledger only.** Do not open, comment on, close, delete or edit GitHub issues/PRs, or commit/push, without an
explicit ask. Draft replies below are **HOLD** — the repo owner has not approved public replies; nothing has been posted.

## Incoming reports (GitHub)

| # | sender | state | what they sent | tracking |
|---|---|---|---|---|
| #2 | bittertea0323 | open · ours to answer | Independent reproduction, Windows 10 + RTX 4070 SUPER: two host build blockers (a .NET SDK pin and one unit test under newer MSVC), 26,633 SPIR-V modules, a run at the documented frontier, slot 108 publishing 0, then a fault ten frames deep in title code. Asks whether that fault is the expected slot-108 consequence, and whether console GPU-VA dereference is this layer or the translator | External, older tree, not a duplicate; Q2 is answered by O6, Q1 still open — draft held |
| #3 | bittertea0323 | open · ours to answer | FSPX corpus on a second host: 18/29 translate, 11 fail `fall-through into undecodable words` and cluster on wave32; per-program word counts; asks for a preferred report shape | Addressed by `0276f2b1f` (repair → 29/29); draft held. **Comment (NinosTech, 09:21Z):** four FSPX translations from an older upstream revision cleared four lookup misses and made the 42 compute-pipeline calls succeed, but the captured frame still had 0 non-black pixels of 896,232; not matched to our cases — see note below |

Closed: #1 — PR "Main"; #5 — closed as completed on `4178c742c`, new frontier is O7, not a reopen.

## Draft replies (HOLD — not posted; awaiting repo-owner approval)

- **#2:** "Thanks — slot 108 is answered on the current tree (`c3c7a7b83`), and the title-side fault is now understood as
  our unified-memory gap: the title CPU-dereferences a fabricated console GPU address, which is runtime work rather than
  translator work. Please re-run `main`; a fault-time register and stack dump is available via `LEONIDA_CRASH_REPORT`."
- **#3:** "Thanks for the per-program counts — the build now repairs each FSPX listing before translating
  (`tools/widen_smem_decode.py`) and measures 29/29 passing `spirv-val` (22/29 without), so the wave32 cluster is closed
  in tree. Per-program stop word is still the report shape we would want for any surviving refusal."
- **#3/NinosTech:** "Thanks — that matches the split we measure: resolving the lookups moves the 42 creations to success,
  while presentation stays black because our stop is title-side. We will check your four programs against our set."

## Our findings — open

| id | area | item | state | evidence |
|---|---|---|---|---|
| O1 | runtime | Renderer's three-slot record array (216 B; 72-byte records) is never initialised | open · [#4](https://github.com/luizwidmer/palmetto/issues/4) (ours, maintainer-tracked) | the allocator exists but nothing reaches it; the global watcher sees no transition; `LEONIDA_SEED_GLOBAL` moves the stop to a `ud2` trap (`1848f2666`) |
| O3 | runtime/diagnostics | Debugging gaps from the public attempt | open, partial — crash-memory save `7c70724b7` (`LEONIDA_CRASH_SAVE`), faulting-thread unwinder `7eab981ad` (`LEONIDA_CRASH_REPORT`), per-run report `0e76519a0` (`tools/run_report.py`) landed | every-thread stack dump still missing (`docs/competitor_intel.md`) |
| O4 | runtime | Console slot 57's deserialized graphics-pipeline record → PC graphics PSO conversion | open — **needed for rendering; not the boot blocker**, below O7 | 10,241 stand-in states handed out (`LEONIDA_STANDIN_PIPELINES=1`, `dbf2b8b49`): same call 11,986, same `game+0xcc2861` stop, same null; still refused without the conversion (`7c70724b7`, `docs/boot_frontier.md`) |
| O5 | build (test hygiene) | `runtime/test/d3d12_existing_heap_test.cpp` fails on HEAD: `CreatePlacedResource` on an existing heap expects `E_NOTIMPL`, gets `E_INVALIDARG` (~L163) | open, low | not built by `_setup/build_tests.cmd` (verified: no reference), which is why it rotted; reported by the tracker agent's build |
| O6 | runtime | Unified-memory requirement: the title CPU-dereferences console GPU image addresses; our fabricated VAs (from `1 << 32`) are unmapped | open | `LEONIDA_BACK_IMAGES=1` makes the read succeed and moves the fault to a null+offset write, so resource GPU addresses must be CPU-visible (`e604c28a6`, `docs/boot_frontier.md`) |
| O7 | runtime | **Find what should populate the null container element at the current stop** — element 0 of an inline small-vector buffer; class vtable `image+0x6597C60`, constructor `game+0x1B37440` | open — **the frontier, highest priority** | measured stop `game+0xcc2861` (read at 0x10); stand-ins proved the graphics pipelines are not it (`dbf2b8b49`, `62b510467`, `docs/boot_frontier.md`) |
| O8 | build | Shader stage's completeness gate blocks the designed `--stages shaders` path: the translator refuses two duplicate-name variants the old corpus had, so stages had to be run individually | open | `62b510467`, `9271ef74c`; `docs/boot_frontier.md` |
| O9 | build/perf | Pipeline-library disassembly is quadratic: `llvm-mc` prints the whole input hex line once per invalid vcc-tuple word, so a 120 KB program becomes a 100–134 MB listing (one file 137 MB); 841 of 2,684 programs in 17 minutes, 9.4 GB | open | `docs/boot_frontier.md` (disassembly-cost section), `9271ef74c`, `62b510467` — not yet in `pipeline_program_source.md` |

**Highest priority: O7** — what should have populated the null container element is now the boot frontier. O4 stays open
for rendering; O3's remaining piece is the highest-value debugging gap.

## Our findings — fixed

| area | item | state | evidence |
|---|---|---|---|
| build | `make_game` publishes the 1,266 graphics programs the title asks for (947 PS + 319 VS), with step ownership and shard-plan alignment | fixed `9271ef74c` | 1,266/1,266 found, translated and `spirv-val` clean; published as 1,337 fspx entries beside 29 FSPX + 42 library compute |
| runtime | Console queue slot 18 `GetDesc` struct-return order (the clobbered `rbx` at `game+0x2484133`) | fixed `4178c742c` | stop moved to `game+0xcc2861`, D3D12 calls 11,906 → 11,986 (was O2; GH #5 closed as completed) |
| build | Pipeline-library extraction and FSPX listing repair wired into `make_game.py` | fixed `0276f2b1f` | FSPX 29/29 with the repair (22/29 without); library 42/42 on the 42-program extraction, 1,047/1,684 on a real package subset |
| runtime | DirectStorage zlib stage inflates instead of failing with `E_NOTIMPL` | fixed `ae876f4b5` | 2,238 → 11,894 D3D12 calls; 0 zlib requests fail |
| runtime | AMD-only `clzero` emulated on Intel hosts | fixed `f76446847` | illegal-instruction fault gone; frame-loop event waits reached, 11,906 calls, 0 pipeline failures |
| runtime | Console device slot 108 answers a real resource allocation info | fixed `c3c7a7b83` | null-object stop at `game+0xCC2861` gone; run continued to `game+0x24844A9` |
| runtime | Console queue slot 35 calibration answered (plus nonvolatile register dump) | fixed `818c739af` | stop unchanged at the time, so not the cause; dump carries `rbx`, `r12`–`r15` |

## Notes (not bugs)

- **Incident: corpus lost and rebuilt today.** Fingerprint mismatch → retranslation killed mid-run when the agent turn
  ended → crashed shard repaired solo → 16 parts merged (26,501), stages re-run (fspx 29/29, bvh 52/52, graphics
  1,266/1,266) + 42 library compute republished → 27,890 loaded, baseline restored (`62b510467`). **A background job
  does not outlive an agent turn.**
- **TLS rewrite (loader).** 53,246 `gs:[58h]` reads are rewritten to the TEB; `LEONIDA_TEB_CHECK=1` confirms `0x1480 +
  8*slot` is right here and the faulting thread's TLS cells are correct (`443639062`, `91a01b5ea`). The public attempt agrees.
- **Pipeline-library translation is partial by design.** 42/42 on the dedicated 42-program extraction, 1,047/1,684 (~62%)
  on a real package subset (`0276f2b1f`); the FSPX set needs the `widen_smem_decode` repair. Not a boot front today.
- **Historical: the eleven "missing" pipeline programs** were zeroed library reads from the refused zlib stage, not a
  second container or a load-time patch; resolved by `ae876f4b5` (see `docs/pipeline_program_source.md`).
- **External (GH #3, NinosTech): lookups can succeed and the frame still be black.** Four FSPX translations cleared four
  misses and the 42 compute calls succeeded, yet 0 non-black pixels of 896,232; matching to our cases needs his hashes.
- **Lifetime-tracker prototype (evidence, not a fix).** Implemented correctly and unit-tested, then reverted because the
  boot regressed 11,986 → 1,653 calls through a wrong vtable slot — evidence that console slot 57 is not a tracker (`O4`).
- **docs.** `docs/competitor_intel.md` records the public attempt (<https://github.com/Marvisak/VIctory>): DXIL + host D3D12
  vs our SPIR-V + Vulkan, prepare-then-map loader, crash reporter with stack unwind, mechanical asset policy; GPL-3.0 like
  ours, so reuse is permitted with attribution. README's progress tracker is generated from `docs/progress.json` by
  `tools/readme_progress.py` (`--check` passes; created `410b6a67b`, frontier corrected `fb93cf28c`).
