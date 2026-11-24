# What VIctory does (and what we should take from it)

Another attempt at running the same title on PC, public since 2026-11-23: <https://github.com/Marvisak/VIctory>
(owner Marvisak, Rust, GPL-3.0, ~6.3k stars, ~800 forks). It is not a rival to ignore: it is the same problem solved
further along, by a different route. This note records what it teaches us and what we are not copying.

## Where it stands

| Stage | Their state |
|-------|-------------|
| Boot, first-boot screens (accessibility, language, settings, legal) | works, at the title's own 30 fps |
| Intro video with sound, start screen, profile, saves | works |
| Loading into the world (~50 s) | works |
| The world | reached, plays with indirect draws, title card renders, scene after it is dark; 0.5-1.8 s/frame |
| GPU hang risk on a real GPU | known; they ship a WARP (software) diagnostic mode because of it |

Test machine: Windows 11, RX 6700 XT. So: the whole platform layer boots and the *world* is where it still fights, at
frames measured in seconds. Our frontier (11,906 D3D12 calls, no pipeline failures) is far behind theirs but on the same
road.

## The biggest structural difference: they do not use Vulkan

| | VIctory | Us (palmetto) |
|---|---|---|
| Shader output | **DXIL**, compiled at pipeline creation; cached on disk | SPIR-V, translated ahead of time from the shipped pipeline library |
| Host API | **D3D12** directly | D3D12.X console surface → Vulkan |
| Shader IR | SSA, one instruction per machine instruction; then `structure` removes EXEC (lane masks become per-lane booleans, wave mask tests become branches), `resource` traces every descriptor to its root parameter | instruction-by-instruction translation with refusal on anything it cannot model |

Their shader coverage, as their README states it: mesh 8,191/8,506, vertex+pixel 10,617/12,346, compute 1,643/2,539;
missing: 64-bit image atomics, merged geometry stages, the ray-tracing builder's shaders. That last one is our
`XboxBvhBuilder` finding, seen from the other side.

Their route is attractive because the console *is* D3D12: no console-to-PC surface translation, mesh shaders and
indirect execution are native, and DXIL is the format the console's own compiler family emits. Ours is attractive
because Vulkan runs everywhere and our index already covers 26.5k programs. A wholesale switch is not a decision to make
mid-boot; but it does mean "the shader index" is not the only possible design, and their coverage numbers are a useful
benchmark for ours.

## Loader lessons that validate or challenge ours

* **Static TLS at slot 0**: "an exe's code reaches its static TLS as slot 0; mapped as a DLL it gets another slot, so
  the loader redirects those loads". That is exactly our `patch_ga_tls_reads` (53,246 `gs:[58h]` sites), found
  independently. Our TEB validation (`443639062`) and theirs agree: this rewrite is required, not optional.
* **Prepare-then-map**: they patch a copy of the image (`IMAGE_FILE_DLL` set, entry 0, checksum 0, import directory
  pointed at an empty terminator, TLS callbacks pointer zeroed with a relocation made a no-op) so the host loader can map
  it without running its imports/TLS. We map manually instead. Their approach buys host-OS behaviour they do not have to
  re-implement; ours avoids the host loader entirely. Both work; theirs is smaller.
* **Crash reporting**: faults are named `module+rva` and the faulting thread's stack is unwound; every build appends to
  `crashes.log`. We have `module+0x...` and a watch, but no stack unwind — that is why we spent rounds inferring a call
  chain from static disassembly.
* **Import resolution order**: our HLE implementation → host function if policy allows that DLL → title DLL export →
  generated stub naming the import. Their `--unimplemented trap|zero` policy switch (trap in the debugger vs log-once
  and return 0) is a cheap, high-value idea: our refusals currently always continue, and continuing is what turns a
  refusal into a mystery fault ten frames later.

## Diagnostics worth adopting (all cheap, all local)

| Their variable | What it does | Our equivalent |
|---|---|---|
| `VICTORY_WATCH=<rva>,...` | logs a title u32 when it changes | `LEONIDA_WATCH_GLOBAL` (have) |
| `VICTORY_CRASH_SAVE=<rva>:<len>` | saves title memory at the crash | **none — adopt** |
| `VICTORY_STACKS=<s>,<s>` | every thread's stack at those seconds | **none — adopt (at the fault is enough)** |
| `VICTORY_SKIP_PIPELINES=<hash>` | omit draws/dispatches by shader hash | none (we refuse pipelines instead) |
| `VICTORY_D3D12_GBV`/`DRED`/`WARP` | validation, device-removed data, software adapter | n/a (Vulkan path) |

`VICTORY_CRASH_SAVE` and all-thread stacks are the two that would have shortened our current investigation: the value
that clobbers a callee-saved register would be visible in saved memory, and the call chain would be read rather than
reconstructed.

## Asset policy

Theirs is stricter than ours and enforced mechanically: nothing from the game in the repo; `.gitignore` for common data
types; `cargo xtask policy` runs on a pre-commit hook as an **allowlist** (file type, size cap, no binary data, no long
hex/base64 runs), with one documented exception (BCPack constant tables, 78 KB); every runtime path that writes derived
data calls `ensure_outside_repo`; if something slips in, history is rewritten rather than patched in a follow-up.

Our rules say the same things in prose and in review. Making ours mechanical — even a small pre-commit script that
refuses binaries, long hex runs and paths outside the work folder — would be the single most transferable piece of
process here. Their licence is GPL-3.0; ours is GPL-3.0 too, so code reuse is permitted with attribution and the same
licence, but the processes above are better re-implemented than copied.

## What we should not do

* Do not port their GPU stack or shader translator wholesale mid-boot: our pipeline index and translator work, and the
  current stop is a title-side pointer, not a shader gap.
* Do not adopt their BCPack-tables exception: our DirectStorage layer refuses what it cannot decode, on purpose.
* Do not copy their numbers into our docs as if measured here; cite them as theirs.
