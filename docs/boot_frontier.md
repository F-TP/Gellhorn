# Boot frontier

**Status: measured** on the retail 1.0.2.1 package, Windows 11, RTX 4090, `LEONIDA_D3D12=vulkan`, with the shader corpus
in `LEONIDA_SHADERS` (26,555 game and XboxBvhBuilder modules plus the FSPX set that translates) and
`LEONIDA_SKIP_INT3=1` while the allocator assertion below is unmodelled.

This note records how far the boot currently gets and what stops it, so the next attempt starts from a measurement
rather than a guess. Counts, arities and error classes only; no package bytes or addresses.

## Where the run stops

| # | Stop | Class | Measured |
|---:|---|---|---|
| 1 | device slot 61 forwarded to PC `CreateMetaCommand` | wrong arity | fixed: refuses with E_NOTIMPL, logs both arguments |
| 2 | device slot 67 `DeserializeComputePipelineStateX`, 42 calls | missing translation | **solved**: they are the title's own precompiled compute programs from the pipeline library inside `shaders.rpf`, not FidelityFX programs; 38 of the 42 translate, the corpus carries them, and 0 creations fail |
| 3 | device slot 108 forwarded to the PC table | unknown shape | **current stop**: called with two arguments (an output pointer and a structure pointer) from 29 sites |

The run reaches **2,238 D3D12 calls** and stops at the third row.

Two facts about the first two rows matter for planning:

* The title does **not** survive a failed compute pipeline. Full slot tracing shows the run reaching device slot 62 once
  more after the 42 refusals and then calling through a null pointer (`access execute` at 0x0), so the FSPX gap stops
  the boot and is not merely a rendering problem. An earlier reading of the same run - that the title continued past
  the failures - came from the slot log without full tracing and was wrong.
* The lookup key is the SHA-256 of the kind-6 program bytes the title passes in the pipeline description, so a pipeline
  can only be created when that exact program has a translated module.
* The shader library loads `<dir>/index.json` and then the `fspx/` and `bvh/` indexes beneath it, so the builder's
  subdirectories are found as written. An earlier note here claimed the opposite; it was wrong.

## Outstanding assertion

With `LEONIDA_SKIP_INT3` unset the run stops earlier, on the title's own check that a computed size is at most
2 MiB - 1 (`cmp rax, 0x1fffff; jbe ...; int3`), in the same allocator region as the earlier memory stops. It is a soft
assertion: skipping it lets the boot run on into pipeline creation, so it is hiding progress rather than blocking it.

## Method notes

* A console slot's arity is measured from the title's own call site: scan `.text` for `ff 90 <slot*8 as disp32>` and
  disassemble the window before the call. Register probing alone misleads, because arguments the console does not pass
  are whatever the registers happened to hold.
* The console device vtable is longer than any PC one (the title calls slot 127, a one-argument method, from 45 sites).
  Slots past the PC table are console extensions: they are stubbed with S_OK / 0 and named in the log.
* A console heap address is a 64 KiB-aligned title VA; any other value where an extended description would carry one
  is a field that happens to sit there, and treating it as an address takes the heap out of the arena.

## The FSPX translation gap, measured

The 29 FidelityFX programs unpack and disassemble, and 18 translate. The 11 that do not are refused by the translator
with `fall-through into undecodable words`: a reachable instruction runs into words the disassembler skipped, so the
path's later stores and exports would be dropped without a word.

| program | bytes | undecodable words |
|---|---:|---:|
| 03 | 10,156 | 9 |
| 04 | 16,696 | 9 |
| 05 | 17,040 | 27 |
| 06 | 41,300 | 155 |
| 07 | 21,620 | **1** |
| 08 | 25,508 | 10 |
| 09 | 25,972 | 32 |
| 12 | 8,656 | 8 |
| 15 | 22,272 | 8 |
| 18 | 8,600 | 8 |
| 28 | 13,724 | 55 |

The skipped words sit in the listing as gaps: program 07 has one, at +0x4EE4 (`08 19 2c f4`); programs 12 and 18 have
eight each, all of the form `xx 50 16 cc` (`19 50 16 cc`, `1a 50 16 cc`, `21 50 16 cc`, ...). LLVM's AMDGPU disassembler
refuses every one of them as `invalid instruction encoding` for `gfx1030`, `gfx1013`, `gfx1010`, `gfx1011` and `gfx1012`,
which points at an alignment problem in the listing rather than a missing opcode family: one mis-sized instruction
earlier in the stream would leave exactly this kind of short, repeated remnant.

### What the skipped words actually are

Program 07's single gap is the clearest case, and it settles the shape of the problem. The four bytes at the gap are the
**first half of an eight-byte instruction**; LLVM rejects them on their own and then re-anchors four bytes later, so the
second half is decoded as a different, unrelated instruction (`v_mul_hi_u32_u24_e32`). The translator then finds a
reachable instruction that runs into the skipped first half and refuses the whole shader.

The eight bytes together are still rejected, and the reason is a register-alignment rule rather than a missing opcode:

```
s_buffer_load_dwordx8 s[25:32], s[16:19], s92 offset:0x7800   -> error: invalid register alignment
s_buffer_load_dwordx8 s[92:99], s[16:19], s92 offset:0x7800   -> assembles, and matches an earlier instruction in the same program
```

Sweeping that field settles which reading is right. The byte selects the destination, and every value the disassembler
accepts lands inside the register file while the rejected ones do not:

| value | destination | result |
|---|---|---|
| 0x17 | `s[92:99]` | decodes |
| 0x18 | `s[96:103]` | decodes |
| **0x19** | `s[100:107]` | **invalid** |
| **0x1A** | `s[104:111]` | **invalid** |
| 0x1B..0x1D | `ttmp[0:15]` | decodes |

The rejected values name registers past the scalar-register file on every RDNA2 target LLVM models (gfx1010, gfx1011,
gfx1012, gfx1013, gfx1030 all refuse them), so these words are **not an instruction on any of those targets**. The
disassembler is right to reject them; the control-flow walk is wrong to reach them.

That points at the walk rather than at the decoder, and specifically at where the stream first drifts: a word the title
executes cannot sit there as data, so an earlier instruction must have been decoded at the wrong length or as the wrong
operation, leaving the walk falling through into what is actually a literal or padding word. Program 07 has exactly one
such word, which makes it the cheapest place to find that first divergence.

Either way the work is in the translator and its disassembly, not in the D3D12 layer: the runtime cannot create these
compute pipelines until the programs translate, and the title stops on a null pipeline when they do not. That is the only
thing between the runtime and a first frame, and it is a translator project rather than a wiring change.

### The fall-through site in program 07

The translator re-anchors every instruction to its true offset in the program blob when a listing contains undecodable
words, then translates only what is reachable from the entry. Doing that for program 07 puts the skipped word in a
precise place:

```
re-anchoring skipped 1 range: 0x4F08..0x4F0C (4 bytes)

0x4EF4  s_clause
0x4EF8  s_buffer_load_dwordx8
0x4F00  s_buffer_load_dwordx8      <- ends at 0x4F08
0x4F08  (skipped word)             <- the path falls in here
0x4F0C  28 2c 14 1a                <- code resumes here
```

Nothing branches to 0x4F08, so the path arrives by fall-through from the scalar load at 0x4F00, and the two loads before
it carry `s_clause`, whose whole purpose is to bundle consecutive scalar loads: the stream really does look like a run of
loads, with one member of the run undecodable.

An earlier version of this note claimed the listing and the blob disagreed about that neighbourhood, because a load
encoding seen nearby was not at that offset in the blob. That was wrong: the encoding occurs exactly once in the blob, at
0x509C, as an ordinary instruction elsewhere in the same program. The listing and the blob agree; only the one word at
0x4F08 is undecodable.

The program's own metadata says it is a wave32 compute program with 64 VGPRs, 14 user SGPRs and 4 KiB of LDS. The
destination the undecodable word names would be `s[100:107]`, past the scalar-register file that every RDNA2 target LLVM
models allows, which is why it decodes as invalid while its neighbours decode.

### The answer: the console's register file is larger than LLVM's model

The programs settle it themselves. Their compute metadata declares the register counts, and decoding the granularity
field of `rsrc1` (`0x402c0347`) gives **112 SGPRs** for a wave32 program; the same value appears in programs 07, 12 and
18. LLVM's RDNA2 model allows **106** scalar registers at most, and it says so plainly when asked to assemble the
instruction the word encodes:

```
s_buffer_load_dwordx8 s[100:107], s[16:19], s12 offset:0x7800   -> error: register index is out of range
s_buffer_load_dwordx8 s[96:103],  s[16:19], s12 offset:0x7800   -> assembles, encoding 08 18 2c f4 00 78 00 18
```

So the skipped word is a valid instruction on this console whose destination register LLVM's register file does not
contain, and the same will be true of every other reference to a register above 103 in these programs. The disassembler
is not wrong about the encoding; it is modelling a smaller machine.

**What that means for the fix.** No runtime change helps and no LLVM flag helps: the disassembler has to be told the
console's register file is bigger, or the pipeline has to decode these words itself. The pipeline route is available
without rebuilding LLVM: assemble the same instruction forms with destinations inside LLVM's range under each candidate
`mcpu`, derive the bit fields, and emit the listing line for the out-of-range cases from the bytes. The ground truth for
that derivation comes from `llvm-mc --show-encoding`, so nothing is guessed.

Program 07 is the trial case: one word, one instruction form, and a shader that already translates in every other
respect.

## Two classes among the eleven, and what each needs

Repairing the listings with `tools/widen_smem_decode.py` split the eleven refusals into two classes, which need different
work:

| class | what it is | state |
|---|---|---|
| register file | a buffer load whose destination is above LLVM's 106-register model, skipped and re-anchored | **fixed for the words it covers**: the tool reconstructs them from the program bytes, and program 07 now translates and passes spirv-val |
| inline literals | an instruction LLVM decodes correctly but prints with a canonical encoding, because it drops an inline-literal or op-sel field | **open**: with the strict printed-encoding check relaxed as a measurement, the repaired set still translates 18 of 29 and the remaining failures are `branch target outside code`, so the driver is byte drift after the words the tool cannot yet rebuild |

Adding program 07's module to the corpus does **not** create any pipeline: the run still reports the same 42 failing
`DeserializeComputePipelineStateX` calls and stops in the same place. So that program is not one of the ones the title
actually asks for, and a translation that passes `spirv-val` is not by itself progress on the boot.

### What that implies for the remaining work

The 42 pipelines need specific programs among the untranslated ten. Getting there means finishing class 2 - making the
repaired listing's instruction offsets agree with the blob for the inline-literal forms - and only then re-measuring.
A partial set is measurably useless: it changes neither the failure count nor the stop.

### The inline-literal class, measured

After the repair tool was made non-invasive, the FidelityFX set translates 19 of 29 (program 07 is the new module) and
program 03 has 15 of its words reconstructed instead of one. What remains is one class, and the translator names it with a
single-bit difference:

```
v_dot4_i32_i8 at 0x13DC
  the blob holds  34 40 16 cc 27 8d d1 0c
  LLVM printed    34 40 16 cc 27 8d d1 1c
```

The last byte differs in one bit. That is a field the disassembler drops when it prints an encoding, not a misdecode:
the instruction is right and the text lost a field. The translator refuses it rather than translating the field as zero,
and rewriting the printed bytes to match the blob would defeat that refusal without making the translation correct, so
the repair tool leaves these lines alone.

Finishing this class means teaching the translator to read the dropped field from the blob for the VOP forms involved -
the same shape as `mix_literal`, which already exists for other families - and then re-measuring. Until that is done the
FidelityFX pipelines cannot be created, and the run stops where it has stopped since the pipelines were first reached.

## Correction: the refused pipelines are the title's own, not FidelityFX

An earlier section here said the 42 refused pipelines needed FidelityFX programs among the eleven the translator cannot
translate. That was wrong, and measuring it properly corrected the picture:

* the 42 refusals carry 39 distinct program keys, and none matches any of the 29 FidelityFX programs - not by key, and
  not by size class either;
* they are the title's own **precompiled compute pipelines**, deserialized from the pipeline library inside
  `shaders.rpf`, whose format is in [shader_library.md](shader_library.md). 31 of the 39 keys are byte-identical to
  library compute programs 0..30.

Extracting that library, writing the missing `<name>.state` sidecars - the runtime filters a program's candidates by the
SHA-256 of its kind-0 state, so without them no library program can resolve - and translating its compute programs gives
**38 of 42 translated and `spirv-val` clean**. Four are refused with `CS thread-group-size SGPR`. With those in the
corpus and the stand-in pipeline switched off, **0 of 42 pipeline creations fail**, and the run reaches device slot 108
and stops inside the title itself.

Three consequences for anyone continuing:

* `tools/shader_library.py` now writes `<name>.state`; a pipeline cannot resolve without it;
* nothing in `make_game.py` runs the pipeline-library step yet, so a fresh build still reproduces the old failure;
* `tools/rdna2spirv.py`'s stage fallback reads `lib_cs_*` as stage `lib`, so a library listing needs an `index.json`
  beside it (or the fallback fixed) or it is refused as an unknown stage.

## The stop is independent of the compute pipelines

With all 42 compute pipelines created - 31 from translated library programs and the other eleven standing in - the run
still stops at the same place: `game+0xCC2861`, a read of `0x10` because the function is entered with a null object.

The caller loads that object from a process global and passes it straight in:

```
5BFEBD5: mov rcx, qword ptr [rip + ...]   ; the object, null here
5BFEC10: call 0x140CC2840                 ; test byte [rdi+0x10], 4  -> fault
```

So the missing pipelines were not the cause of this stop. The global is filled by something earlier in the boot that the
runtime either refused or answered without an object. Candidates measured in the same run, in order:

* device slots 86 and 108 are console extensions beyond the PC device table; they are stubbed with S_OK and, for 108, a
  zero dword, which publishes no object at all;
* `ShaderCacheControl` fails, `CreateLifetimeTracker` returns E_NOTIMPL, the texture computer path returns E_NOTIMPL
  (it does hand back a stub object), and DirectStorage reports zlib/BCPack/swizzle requests as not implemented.

The next step is to find which of those results the title stores in that global - by logging the arguments of the
beyond-table stubs and the value each caller keeps - rather than guessing at the crash site itself.

## The streaming gap was hiding most of the boot

The DirectStorage shim refused a request's zlib stage with `E_NOTIMPL`. Once it inflates it - the runtime already had
`zlib_inflate` for the asset tools' format, so nothing new was vendored - the boot goes much further before the same
stop: **2,238 -> 11,894 D3D12 calls**, the `not implemented` line disappears, and no zlib request fails. The wall-clock
time grows from about 14 s to 17-19 s for the same reason.

Two things this settles:

* a refused streaming stage does not stop the title, it *starves* it: the run continued, but with a fraction of the work
  the title wanted to do, so every measurement taken before this change described a much shallower boot than it appeared
  to;
* the remaining stop is unaffected, which makes it the more clearly isolated: after five times as many device calls the
  title still arrives at the same null object (`game+0xCC2861`, reading `0x10`) instead of anything downstream.

BCPack and swizzle requests are still refused explicitly; the log names the modes and size bands when they are asked for.

## The eleven "missing" programs were a decompression failure

For several rounds the boot was described as needing translations for eleven pipeline programs that matched no extracted
library record. That was an artifact. The DirectStorage shim refused a request's zlib stage, and the library bytes the
title handed to the device for those records were therefore zero pages. Dumping them captured the zeros, which is why
they matched nothing, why same-size library records differed in hundreds of bytes, and why a raw search of the package
found nothing.

With the zlib stage implemented, the same boot reports:

```
d3d12 calls: 11,894
pipeline failures: 0 of 42
programs dumped as untranslated: 0
```

and that is **without the stand-in pipeline path at all**. Every compute pipeline the title asks for resolves to a
translated library program. The stand-in remains available as a diagnostic but is no longer needed to get this far.

The lesson worth keeping: a refused decompression did not fail loudly at the point of use. It produced valid-looking
zeros downstream, and three separate investigations spent rounds on "missing programs" that did not exist. When a shim
refuses a *data* stage, the bytes it feeds every later consumer are wrong, and nothing downstream can tell.

## The null object was console slot 108

The stop that survived the pipeline work was not a pipeline problem at all. Tracing the factory that stores the null
object showed it creates the engine's default white render-target: the validator accepts the description, then finalize
asks the device for the resource's allocation info and the allocation fails, so the factory returns null and the renderer
asserts on it.

The allocation info comes from console device extension slot 108, called in the console's struct-return order - hidden
16-byte result in RCX, device proxy in RDX, counts in R8/R9, description array on the stack, per-description record array
as the sixth argument. The shim answered success and wrote at most one zero dword to what it took for an output pointer,
which for this call is the device proxy, so the return buffer stayed garbage. The title computed a bogus size, asserted
in its allocator, and the factory returned null.

Answering the slot with a real query - the device's allocation info for the passed description, with a 64 KiB fallback
when the host cannot size it - removes the stop entirely. Measured: the boot continues past `game+0xCC2861` to
`game+0x24844A9`, 11,902 D3D12 calls, 0 pipeline failures.

The new stop is the same class one layer on: a global 72-byte-record array (`image+0x75DA668`) is null when an entry is
written to it. A watcher over that global sees no transition at all during the boot, so whatever populates it never runs;
no direct store to it exists in the image, so it is filled through a computed pointer, and the next step is to find which
subsystem's failure skips that initialisation rather than the writer itself.


## Frontier 2026-11-24: the title reads our fabricated GPU address as CPU memory

The stop after the TLS rewrite landed at `game+0x2484133`, a read of address `0x100000000`. The value in `rbx` there is
exactly `1 << 32`, and `d3d12_vulkan_device.cpp` hands out console image addresses starting at `std::uint64_t(1) << 32`
(256 bytes apart) with the comment "Nothing dereferences it". **That comment is now falsified by measurement**: the title
dereferences the address as a CPU pointer.

Evidence:

1. A hardware-free probe (poking an int3 over the faulting instruction) logs `rbx = 0x100000000`, `rax = 1` immediately
   before the fault, and the trap fires on every pass - the block is reached by a computed transfer, not the fall-through
   path that a linear disassembly shows, which is why tracing the preceding `lea` never explained the value.
2. A poke at the call's return address never fires, confirming the indirect entry.
3. `LEONIDA_BACK_IMAGES=1` commits 64 MiB of zeroed memory at `0x100000000`. The read then **succeeds** and the fault
   moves to `game+0x2484140`, a write to `0x121A4` - a null pointer plus that offset, because our memory holds zero where
   the console would hold a pointer.

Interpretation: on the console, a resource's GPU virtual address is also a CPU address (unified memory). The title reads
through a T# image address expecting live data. Our Vulkan backend fabricates addresses in that range that are not
mapped, and no data is ever written there.

What this implies for the runtime: resource GPU addresses must become CPU-accessible at the address the title is handed -
either by backing the console address range and keeping it coherent with what shaders write, or by making the fabricated
address resolve to the resource's host memory. The descriptor-heap work already does this for heap memory
(`console_heap_back`, placeholder views); resource memory needs the same treatment.

Cross-reference: the public attempt (VIctory) does not need this, because it runs the title on host D3D12 where the
console's GPU virtual addresses are the host's own resource addresses.


## Frontier 2026-11-24 (later): queue slot 18 returned a struct the wrong way round

The stop at `game+0x2484133` (a read of `0x100000000`, a clobbered `rbx`) was localised with the new crash report and
register-log pokes: `rbx` is correct at `game+0x2484129`, and wrong at `game+0x248412E`, so the faulting frame's first
D3D12 call - queue slot 18, `GetDesc` - was the cause. The path reaches it by a `js` at `game+0x24840E3`, which is why
earlier probes on the fall-through instructions never fired.

Slot 18 was the audit's unmeasured convention: the title calls it as `(queue, buffer)`, and our queue is a C++ object
whose `GetDesc` returns `D3D12_COMMAND_QUEUE_DESC` by value, so MSVC wants the hidden return pointer in RCX and the object
in RDX. The generic forwarder passed them the other way round, so the descriptor was written into the front of the real
queue object and the title's buffer kept its previous contents; the title read `1 << 32` out of it and dereferenced that.

Measured after the fix: the stop moves to `game+0xcc2861` (a read of `0x10`) and the D3D12 call count rises from 11,906
to 11,986. Slot 18 now has its own conversion beside the device proxy's struct-return cases.

The next stop is a null-ish read in frame `image+0xcc2861`, whose caller is `image+0x186324`.


## Frontier 2026-11-24 (new stop): a null argument that the title traps on

After the slot 18 fix the stop is `game+0xcc2861`, `test byte ptr [rdi+0x10], 4` reading `0x10`. Instrumented pokes show
the routine is entered with a valid pointer on every early pass (`rdi` = a heap pointer), so this is not a state problem
inside the routine: the faulting pass is the one that enters with a null argument.

The routine begins by testing its first argument and branching to `game+0xcc2936` when it is null. That address is an
`int3`, and `game+0xcc2937` is a `jmp` back to the loop body that skips the argument load. `LEONIDA_SKIP_INT3` steps over
that `int3`, so the null argument then reaches `[rdi+0x10]` and faults at `0x10`. The trace records exactly this:
`int3_skipped at game+0xcc2936` is the last skip before the fault.

So the title's own null check fired, and skipping the trap converted a deliberate stop into a dereference. The question to
answer next is who passed the null: the caller frame is `game+0x186324`, whose `mov rcx, qword ptr [rax]` loads the
argument from `[rsi+0x60]`, and the first qword there is zero.

This is also a policy question for `LEONIDA_SKIP_INT3`: for a trap that follows a null check, stopping at the trap is more
informative than stepping over it, because the trap site is the title's own statement of what went wrong.


## Frontier 2026-11-24 (refusals at the null stop)

Counted in one boot at the `game+0xcc2861` stop. The dominant gap is not a slot arity question but an interface the title
creates constantly:

| refusal | calls |
|---|---|
| `ID3D12Device5::CreateLifetimeTracker` returns `E_NOTIMPL` | 10,242 |
| allocator slot 9 past the PC allocator table | 2 |
| queue slot 32 past the PC queue table | 2 |
| `ID3D12GraphicsCommandList::Close` failed | 1 |

Every `CreateLifetimeTracker` call leaves the title with a null tracker, and the current stop is the title's own null
pointer, so a tracker object is the first thing to rule in or out: an earlier prototype regressed the boot (D3D12 calls
11,894 -> 1,653) through a wrong vtable slot, which means the interface order must be exact, and the run has to be
compared against 11,986 calls and the `game+0xcc2861` stop before the change is kept.

## Frontier 2026-11-24 (slot 57 measured)

Console device slot 57 is not the PC method its index reaches: with the proxy forwarding it by position it became
ID3D12Device5::CreateLifetimeTracker, which refused 10,242 times in one boot. Probed instead of assumed, the call is
**graphics PSO creation**:

* 
iid is IID_ID3D12PipelineState (765a30f3-f624-4c6f), not a tracker IID.
* The descriptor at a1 starts with the root signature and then **six** pointer/size pairs, 0x10 bytes each: VS at +0x08
  (388 B), PS at +0x18 (188 B), DS at +0x28 (448 B), HS at +0x38 (208 B), GS at +0x48 (384 B), and a sixth at +0x58
  (752 B). Everything after +0x68 is zero in the first call.
* The code is **native, not DXBC** (first bytes 08 d8 05 40, 56 6e 6f ea, ...), i.e. the same kind of program the
  compute slot 67 wraps as an FSPX kind-6 payload before the device resolves it from the shader index.

So the 10,242 refusals are failed graphics pipeline creations, and the null the title later dereferences is a pipeline
state it never received. The conversion to write: build a PC D3D12_GRAPHICS_PIPELINE_STATE_DESC from this descriptor,
wrap each stage's native program the way slot 67 wraps compute code, identify the sixth pair (the count suggests a
mesh/amplification pair the console descriptor carries), and route to CreateGraphicsPipelineState. Until then the probe
refuses, exactly as the forwarded slot did.
