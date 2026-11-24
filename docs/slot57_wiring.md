# Slot 57 wired: the graphics record converted, and what refuses

**Status: wired and measured.** `runtime/shims/d3d12_xs_device.cpp`'s slot-57 branch now decodes the deserialized
graphics pipeline record with `rt::graphics_record::decode`, builds the PC description with `rt::graphics_pso::build`,
and sends it to the PC device's own `CreateGraphicsPipelineState` exactly as slot 67 sends a compute description
(docs/graphics_pso.md sections 2-5). One 150 s boot now creates **1,074 real PC graphics pipelines** from the first
10,000 records and refuses **8,926** of them, every one by the same named field. It also answers the question the
wiring was for: the next decode is the **input-layout blob's element table**, not the fixed-function state and not the
attachment stand-in.

## 1. The wiring

`proxy_create_graphics_pipeline_probe` keeps its record probe and, after it, calls `convert_slot57_record`:

1. **Read and decode.** The descriptor must be readable for the 0x108 bytes `decode()` validates; a descriptor that
   is not, or that `decode()` refuses, returns `E_INVALIDARG` and is counted as `record` / `decode`.
2. **Check what the decoder only points at.** The state blob, the input-layout blob and every stage's program bytes
   are range-checked for readability before the builder reads them; an unreadable one returns `E_INVALIDARG` with the
   field named (`record.state`, `record.input_layout`, `record.stages[].program`). Without this the layer would fault
   on a record pointer it cannot read.
3. **The inputs the call does not carry**, supplied explicitly and never silently:
   * **root signature** — the record's `+0x00`. The proxy does not wrap root signatures, so slot 16 now remembers every
     PC root signature the device hands out, and slot 57 passes `+0x00` on only when it is one of those objects
     (directly, or held in a title-side slot whose value is one). The mapping is proved against the table, not guessed
     from the pointer; anything else is left null and refused by the builder as `pRootSignature`. This is safe because
     `create_graphics` keys the cached PSO on `pRootSignature`, so it dereferences the pointer.
   * **attachments** — the call carries no RTV/DSV formats or sample count, and they live in the undecoded state blob.
     One documented, explicitly logged stand-in: **1 x `DXGI_FORMAT_R8G8B8A8_UNORM`, no depth-stencil, sample count
     1**. Logged once (with the state substitute below), never per record.
   * **fixed-function state** — `rt::graphics_pso::documented_default_state()`, the project's documented measurement
     stand-in (docs/graphics_pso.md section 4), logged once as a stand-in.
   * **stage bytecode** — each record stage's own native program span in its record slot (0 PS, 1 VS, 2 MS); the
     backend resolves it by SHA-256 in `create_graphics`'s native branch (docs/graphics_pso.md section 2).
4. **Build, then forward.** `build()` refuses by `why` field; the branch counts the field, logs the first occurrence
   with the record's fields, and returns `E_NOTIMPL` for the two refusals that name a path this tree has not built (a
   mesh stage needs the pipeline-state-stream description; an element layout needs the format mapping) and
   `E_INVALIDARG` for everything else. A built description goes to
   `self->real->CreateGraphicsPipelineState(&pc, *riid, out)` — the title's `riid` and out pointer, the real HRESULT
   back. It is not the label of a conversion: a PC failure is the title's HRESULT.
5. **The measurement.** Per refusal: count by `why` field, distinct record digest and record stage shape; first
   occurrence logged with state/layout sizes, element count and program sizes. Per built record: the PC HRESULT, counted;
   first success and first failure of each code logged. A compact histogram prints every 2,000 slot-57 calls (the last
   checkpoint is the table below: a boot makes 10,241 calls, so the 10,000-call checkpoint covers 97.6% of them).
   The input-layout blob is also described once per (size, count) shape: the element-count word at `+0x4C`, and the
   printable runs with their byte offsets — what says whether the committed contiguous-names encoding is the blob's.
   A `LEONIDA_XS_SLOT57_LAYOUT_DUMP=1` switch (off by default, measurement only) prints the first 176 bytes of one blob
   per shape; it was used for the measurement and its bytes are not quoted here.

`LEONIDA_STANDIN_PIPELINES` still works: a refused record can be handed the project's own diagnostic stand-in, so a
boot with the switch on answers the old question ("is the missing PSO the blocker?") without changing what the default
boot does.

## 2. The histogram

One boot: retail image, shader corpus, Vulkan backend, `LEONIDA_SKIP_INT3=1`, `LEONIDA_SEED_GLOBAL=75DF268:D8`; one
boot at a time, 150 s timeout, a boot runs to the timeout. Harness `tools/boot_report.py`, profile
`_setup/profile_boot.py`.

| | value |
|---|---|
| verdict | **ALIVE 150.2 s, no exception** (killed by its timeout) |
| traced D3D12 calls | 1,410,454 |
| slot-57 calls / distinct records | **10,241 / 10,241** (one per record; the trace line count undercounts under concurrency, the probe's per-record line count does not) |
| records attempted | 10,000-call checkpoint below, 10,241 in all |
| record decode refusals | 0 |
| built (`CreateGraphicsPipelineState` reached) | **1,074** |
| PC pipelines created (`S_OK`) | **1,074** |
| refused before the call | **8,926** |
| PC `CreateGraphicsPipelineState` failures | 0 |

The refusal histogram, all 10,000 calls at the checkpoint:

| `why` field (exact) | calls | distinct records | by stage shape |
|---|---:|---:|---|
| `record.input_layout` | **8,926** | 8,926 | ps+vs 8,923, vs 3 |
| *(built: no `why`)* | **1,074** | 1,074 | ps+vs 1,073, vs 1 |

| record stage shape | calls |
|---|---:|
| `ps+vs` | 9,996 |
| `vs` | 4 |

| input-layout blob shape (`size`/count word at `+0x4C`) | calls |
|---|---:|
| 152 B / count 2 | 5,457 |
| 188 B / count 3 | 2,566 |
| 80 B / count 0 | 1,074 |
| 120 B / count 1 | 687 |
| 156 B / count 2 | 144 |
| 224 B / count 4 | 72 |

| gate | calls |
|---|---:|
| input layout decodes **with elements** | **0** |
| input layout decodes empty (80 B) | 1,074 |
| input layout does not decode | 8,926 |
| record `+0x00` is a PC root signature object | **10,000** |
| ... through a title-side slot / untracked / null | 0 / 0 / 0 |
| stage program bytes unreadable | 0 |
| fixed-function state stand-in used (documented default set) | 10,000 |
| attachment stand-in used | 10,000 |

The one-line answer to the original question: **no record reaches the input-layout format code or the state blob**.
Every record with a vertex layout refuses at the layout *blob* (`record.input_layout`), because the committed decoder's
element encoding does not match the blob the boot carries; the 1,074 records that build are the ones with the
80-byte empty layout (fullscreen-style vertex pipelines), and they create real PC pipelines.

### Boot outcome

| | before the wiring | this boot |
|---|---|---|
| verdict | ALIVE, no exception | **ALIVE, no exception** |
| `ExecuteCommandLists` (frame submits) | ~30,376 per 60 s | **87,393 in 150 s (34,957 per 60 s)** |
| `Close` | successful after the lifecycle emulation | 88,012, **0 failed** |
| slot-57 records handed a PSO | 0 | **1,074 real PC graphics pipelines** |
| `SetPipelineState` | 0 | **0** |
| any `Draw`/`Dispatch` | 0 | **0** |
| `PresentX` (queue slot 32) | refused | still refused (43,699 calls; the present path is a separate front) |

So the conversion is real and cheap (the frame loop keeps its submit rate and nothing crashes), but the title still
binds no pipeline and draws nothing. That is consistent with the refusal: the pipelines a draw would bind are the
ones with vertex input layouts, which are exactly the records refused at the input-layout blob. The attachment
stand-in and the documented default state are not what stops the current reach — no record fails on either.

## 3. What the layout blobs are, measured

The committed `decode_input_layout` accepts one measured-consistent encoding: all NUL-terminated names first from
`+0x50`, then `count*28` attribute bytes, filling the blob exactly. The boot's blobs do not have that shape. Per shape,
logged once with the element-count word and the printable runs' offsets:

| blob | count at `+0x4C` | printable runs with offsets |
|---|---:|---|
| 120 B | 1 | `+0x54:POSITION` |
| 152 B | 2 | `+0x54:POSITION, +0x79:COLOR` |
| 156 B | 2 | `+0x54:TEXCOORD, +0x79:POSITION` |
| 188 B | 3 | `+0x54:POSITION, +0x79:COLOR, +0x9B:TEXCOORD` |
| 224 B | 4 | `+0x54:COLOR, +0x76:TEXCOORD, +0x9B:POSITION, +0xC0:COLOR` |
| 80 B | 0 | none (the empty layout the builder accepts) |

The names are not a contiguous table at `+0x50`; they sit **inside per-element records**, starting at `+0x54`, one
element after another, and the `+0x4C` word is the element count in every shape. The per-shape dumps
(`LEONIDA_XS_SLOT57_LAYOUT_DUMP=1`) show the element record interleaves the name with the numeric attribute fields: the
4-byte word at `+0x50` is the first name's length including its terminator (`9` for `POSITION`/`TEXCOORD`, `6` for
`COLOR`), and the bytes between one name and the next are that element's attribute block. The 28-byte attribute block
measured earlier is one element's block, not a fixed stride for all semantics, so the total-size relation has to be
measured per element before the decoder can accept it. The blob bytes themselves are not stored in the repository.

## 4. The next decode, and the smallest next change

**Next decode: the input-layout element table**, then the format codes inside it. The histogram is unambiguous — 8,926
of 10,000 records refuse on that blob and zero reach an element description — so the state blob, the stage slots, the
storage attachments and the root signature are not the current blockers:

* the root signature is proved (all 10,000 records name a PC object this device handed out);
* no record in this boot is a mesh record (stage shapes are `ps+vs` and `vs`; the mesh path — the pipeline-state-stream
  description — is still unbuilt but not reached here);
* the attachment stand-in and the documented default state never refused anything (they are on the 1,074 built records).

**Smallest next change:** extend `decode_input_layout` (runtime/src/graphics_record.cpp) with the boot's interleaved
element form — count at `+0x4C`, elements from `+0x54`, each a length-prefixed name followed by its attribute block —
using the `LEONIDA_XS_SLOT57_LAYOUT_DUMP=1` dumps to fix the per-element size relation, and refuse anything that does
not fill the blob exactly, as the decoder does today. Then the builder's next refusal (`input_layout.elements[0].format`)
becomes reachable and the following histogram is the format-code mapping, which is the decision that finally lets a
vertex pipeline be built. The measurement for that step is free: one boot of this wiring prints it.

## 5. Tests and gates

`runtime/test/xs_proxy_mock_test.cpp` compiles the real `src/graphics_record.cpp` and `src/graphics_pso.cpp` into the
GPU-free test (its record builder and refusal contract stay unit-tested by `graphics_record_test` and
`graphics_pso_test`), and covers the new branch: a valid PS+VS record building and forwarding the description (record
programs in VS/PS, `pRootSignature`, 1 x R8G8B8A8_UNORM, no depth, sample 1, empty input layout, documented default
state, title `riid` and out pointer), a root signature named through a title-side slot, an element layout refused as
`E_NOTIMPL`, a mesh record refused as `E_NOTIMPL`, a missing and an untracked root signature refused as `E_INVALIDARG`,
a non-decoding record refused as `E_INVALIDARG`, and a failed PC call returned unchanged with no object. The mock's
device slot 10 is `CreateGraphicsPipelineState` and slot 16 `CreateRootSignature`; the other mock kinds keep the
generic forwarded call on those slots (the console queue forwards onto its real object's 10).

Gates run for this change:

```
_setup\build_gta6rt.cmd  -> GTA6RT_OK
_setup\build_tests.cmd   -> TESTS_BUILT
xs_proxy_mock_test       -> PASS (9 allocations refused on purpose)
unimplemented_policy_test -> 98 checks, 0 failed
graphics_record_test     -> 101 checks passed
graphics_pso_test        -> 128 checks passed
d3d12_fence_timing_test  -> still crashes (known pre-existing failure, untouched)
```

No git writes. Scope: `runtime/shims/d3d12_xs_device.cpp`, `runtime/test/xs_proxy_mock_test.cpp`, this file.

## 6. Follow-ups this measurement leaves open

* `docs/graphics_pso.md` still says "backend, not wired", and `README.md`'s tracking block still says slot 57 is not
  wired: both are now stale (the files are outside this change's scope).
* The 1,074 pipelines created are bound to no draw (`SetPipelineState` is still 0). Once the input-layout element table
  is decoded, the vertex pipelines the draw path asks for can be created; the draw binding itself is then the next
  frontier.
* The per-element size relation is the only unmeasured part of the layout table; the dump switch is the tool for it.
