#include "graphics_pso.h"

#include <utility>

namespace rt::graphics_pso {
namespace {

// The shader index's stage names. A record names its stages by slot (0 PS, 1 VS, 2 MS) and nothing inside the stage
// blob does (docs/shader_library.md section 2). "gs" and "cs" are not record slots: they are scanned only when a
// program resolved under no graphics stage, so a slot/stage mismatch (a program published for another stage) is named
// instead of being reported as a missing translation.
constexpr const char* kStageNames[] = {"ps", "vs", "ms", "gs", "cs"};

bool fail(std::string* why, std::string message) {
    if (why) *why = std::move(message);
    return false;
}

// A record slot and its stage name: "1 (vs)".
std::string stage_slot(int slot) {
    return std::to_string(slot) + " (" + rt::graphics_record::stage_slot_name(slot) + ")";
}

// A stage's bytecode input name: "stages[1] (vs)".
std::string stage_input(int slot) {
    return "stages[" + std::to_string(slot) + "] (" + rt::graphics_record::stage_slot_name(slot) + ")";
}

// One why-message prefix for a record stage: "record.stages[1].slot: ...".
std::string record_stage(std::size_t index) {
    return "record.stages[" + std::to_string(index) + "].slot: ";
}

}  // namespace

bool resolve_native_stage(std::span<const std::uint8_t> bytecode, const char* stage,
                          const leonida::ShaderLibrary& library,
                          const std::function<bool(const leonida::ShaderEntry&)>& fits, NativeStage* out,
                          std::string* why) {
    if (out) *out = NativeStage{};
    if (why) why->clear();
    if (!out || !stage || !*stage || bytecode.empty()) return false;

    const std::string stage_name(stage);
    leonida::ComputeProgram program = leonida::compute_program(bytecode);
    if (program.kind == leonida::ComputeProgram::Kind::fssm) return false;  // the FSSM/DXBC path owns those bytes
    bool raw_key = false;
    if (program.kind == leonida::ComputeProgram::Kind::none) {
        // Not a program compute_program() recognises. Its identity is still the SHA-256 of the bytes as passed - the
        // key the library's graphics programs were published under (docs/pipeline_program_source.md), which is the
        // same value whether or not the program carries the 36-byte FSPX prologue. Look that key up before calling
        // the bytes unreadable: a miss means this path has nothing to do with them.
        const std::string raw_hash = leonida::sha256_hex(bytecode);
        if (library.fspx_candidates(stage_name, raw_hash).empty()) return false;
        program.kind = leonida::ComputeProgram::Kind::fspx;
        program.bytes.assign(bytecode.begin(), bytecode.end());
        program.state.clear();
        raw_key = true;
    }

    bool ambiguous = false;
    const std::vector<const leonida::ShaderEntry*> candidates =
        library.program_candidates(stage_name, program, &ambiguous);
    const leonida::ShaderEntry* entry = pick_entry(candidates, fits);
    const std::string hash = leonida::sha256_hex(program.bytes);
    out->program = std::move(program);
    out->entry = entry;
    out->raw_key = raw_key;
    if (entry || !why) return true;

    if (ambiguous) {
        *why = "ambiguous: " + std::string(raw_key ? "the native program" : "the FSPX kind-6 program") + " sha256 " +
               hash + " has translations that differ and came without its kind-0 state, so no single module can be "
                      "chosen (the record slot names the stage " +
               stage_name + ")";
        return true;
    }
    // A program published under another stage is a slot/stage mismatch, not a missing translation: say which stage,
    // because the caller's wiring would be the thing to fix.
    for (const char* other : kStageNames) {
        if (stage_name == other) continue;
        if (!library.program_candidates(other, out->program, nullptr).empty()) {
            *why = "stage mismatch: the program sha256 " + hash + " is published for stage " + other + ", not " +
                   stage_name + " (the record slot names the stage); no module was chosen";
            return true;
        }
    }
    *why = "no translation: no shader index entry for stage " + stage_name + " and " +
           (out->program.state.empty() ? std::string(raw_key ? "the SHA-256 of its program bytes"
                                                            : "the FSPX kind-6 program")
                                       : std::string("the FSPX kind-6 program looked up with its kind-0 state")) +
           " sha256 " + hash;
    return true;
}

FixedFunctionState documented_default_state() {
    FixedFunctionState state{};
    // Rasterizer: solid triangles, back faces culled, clockwise is front, depth clipping on, no depth bias.
    state.rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    state.rasterizer.CullMode = D3D12_CULL_MODE_BACK;
    state.rasterizer.FrontCounterClockwise = FALSE;
    state.rasterizer.DepthBias = 0;
    state.rasterizer.DepthBiasClamp = 0.0f;
    state.rasterizer.SlopeScaledDepthBias = 0.0f;
    state.rasterizer.DepthClipEnable = TRUE;
    state.rasterizer.MultisampleEnable = FALSE;
    state.rasterizer.AntialiasedLineEnable = FALSE;
    state.rasterizer.ForcedSampleCount = 0;
    state.rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    // Blend: off, with the API's neutral factors and ops and every colour channel written on every target, so a later
    // blend enable changes only BlendEnable.
    state.blend.AlphaToCoverageEnable = FALSE;
    state.blend.IndependentBlendEnable = FALSE;
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : state.blend.RenderTarget) {
        target.BlendEnable = FALSE;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    // Depth-stencil: both off, every field still a valid API value (the neutral ops and comparison).
    state.depth_stencil.DepthEnable = FALSE;
    state.depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    state.depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    state.depth_stencil.StencilEnable = FALSE;
    state.depth_stencil.StencilReadMask = 0xff;
    state.depth_stencil.StencilWriteMask = 0xff;
    for (D3D12_DEPTH_STENCILOP_DESC* face : {&state.depth_stencil.FrontFace, &state.depth_stencil.BackFace}) {
        face->StencilFailOp = D3D12_STENCIL_OP_KEEP;
        face->StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        face->StencilPassOp = D3D12_STENCIL_OP_KEEP;
        face->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    state.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    state.sample_count = 1;
    state.sample_quality = 0;
    state.sample_mask = 0xffffffffu;
    return state;
}

bool build(const rt::graphics_record::Record& record, const GraphicsPsoInputs& inputs,
           D3D12_GRAPHICS_PIPELINE_STATE_DESC* out, std::string* why) {
    if (why) why->clear();
    if (!out) return fail(why, "build: the output description is null");

    // ---- The record's shape: which stages it names. decode() guarantees this, but build() is a public entry point and
    // a caller-built Record could be anything, so it is checked again instead of trusted.
    if (record.stage_count == 0 || record.stage_count > rt::graphics_record::kStageSlotCount)
        return fail(why, "record.stage_count: the record names " + std::to_string(record.stage_count) +
                             " stage(s); a graphics pipeline record has 1 to " +
                             std::to_string(rt::graphics_record::kStageSlotCount));
    bool used[rt::graphics_record::kStageSlotCount] = {};
    for (std::size_t i = 0; i < record.stage_count; ++i) {
        const int slot = record.stages[i].slot;
        if (slot < 0 || slot >= static_cast<int>(rt::graphics_record::kStageSlotCount))
            return fail(why, record_stage(i) + "slot " + std::to_string(slot) +
                                 " is not one of the record's stage slots");
        if (used[slot]) return fail(why, record_stage(i) + "stage slot " + stage_slot(slot) + " appears twice");
        used[slot] = true;
    }
    const bool vertex = used[rt::graphics_record::kStageSlotVs];
    const bool mesh = used[rt::graphics_record::kStageSlotMs];
    if (vertex && mesh)
        return fail(why, "record.stages: a vertex and a mesh stage together are not a stage combination D3D12 accepts");
    if (mesh)
        return fail(why, "record.stages[].slot: a mesh stage cannot be expressed in a "
                         "D3D12_GRAPHICS_PIPELINE_STATE_DESC (the PC description has no mesh-shader field); a record "
                         "whose slot 2 is used needs the D3D12_PIPELINE_STATE_STREAM_DESC path with its MS subobject");

    // ---- The two blobs the record always carries. The state is checked for its measured envelope and then not used:
    // no word of it is decoded (docs/graphics_record.md section 4), so the fixed-function state comes from
    // `inputs.defaults` instead. The input layout is decoded, and an element is refused: its format code is not mapped
    // to a DXGI format and the attribute bytes' field encoding is undecoded, so no D3D12_INPUT_ELEMENT_DESC can be
    // built from it without a guess (section 5).
    if (!record.root_signature)
        return fail(why, "record.root_signature: the record has no root signature at +0x00, so the pipeline's "
                         "bindings cannot be established");
    rt::graphics_record::State state;
    if (!rt::graphics_record::decode_state(record.state, &state))
        return fail(why, "record.state: the fixed-function state blob is missing or outside its measured envelope "
                         "(380..520 bytes, a whole number of words), so the record is not the shape this converter "
                         "knows");
    std::vector<rt::graphics_record::Element> elements;
    if (!rt::graphics_record::decode_input_layout(record.input_layout, &elements))
        return fail(why, "record.input_layout: the input-layout blob does not decode (its element count, its "
                         "NUL-terminated semantics and its count*28 attribute bytes do not fill the measured 80..340 "
                         "bytes)");
    if (!elements.empty()) {
        const rt::graphics_record::Element& first = elements.front();
        return fail(why, "input_layout.elements[0].format: the first of " + std::to_string(elements.size()) +
                             " element(s) (\"" + first.semantic + "\") keeps its 28 attribute bytes raw: the format "
                             "code is not mapped to a DXGI format and its input slot, aligned byte offset, semantic "
                             "index and per-vertex flag are undecoded, so no D3D12_INPUT_ELEMENT_DESC can be built");
    }

    // ---- The caller's inputs. Each is checked by name; build() substitutes nothing.
    if (!inputs.root_signature)
        return fail(why, "pRootSignature: the caller supplied no PC root signature for the record's root signature");
    for (int slot = 0; slot < static_cast<int>(rt::graphics_record::kStageSlotCount); ++slot) {
        const StageBytecode& code = inputs.stages[slot];
        const bool present = code.data != nullptr && code.size != 0;
        const std::string name = stage_input(slot);
        if (used[slot] && !present)
            return fail(why, name + ": the record has a " + rt::graphics_record::stage_slot_name(slot) +
                                 " stage but the caller supplied no resolved program bytes for it");
        if (!used[slot] && (code.data || code.size))
            return fail(why, name + ": the record has no " + rt::graphics_record::stage_slot_name(slot) +
                                 " stage, so these program bytes have no stage to go in");
    }
    if (inputs.render_target_count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return fail(why, "render_target_count: " + std::to_string(inputs.render_target_count) + " is above D3D12's " +
                             std::to_string(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT));
    for (UINT target = 0; target < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++target) {
        const std::string name = "rtv_formats[" + std::to_string(target) + "]";
        if (target < inputs.render_target_count) {
            if (inputs.rtv_formats[target] == DXGI_FORMAT_UNKNOWN)
                return fail(why, name + ": a render target the PSO uses needs a format, not DXGI_FORMAT_UNKNOWN");
        } else if (inputs.rtv_formats[target] != DXGI_FORMAT_UNKNOWN) {
            return fail(why, name + ": D3D12 requires the slots at and above NumRenderTargets to be "
                                    "DXGI_FORMAT_UNKNOWN");
        }
    }
    const FixedFunctionState& ff = inputs.defaults;
    switch (ff.topology_type) {
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE: break;
        default:
            return fail(why, "defaults.topology_type: " + std::to_string(static_cast<int>(ff.topology_type)) +
                                 " is not a topology type this converter can build (point, line or triangle)");
    }
    if (ff.sample_count == 0 || ff.sample_count > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT ||
        (ff.sample_count & (ff.sample_count - 1u)) != 0)
        return fail(why, "defaults.sample_count: " + std::to_string(ff.sample_count) +
                             " is not a power of two from 1 to " +
                             std::to_string(D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT));
    if (ff.sample_quality != 0 && ff.sample_quality != DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN &&
        ff.sample_quality != DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN)
        return fail(why, "defaults.sample_quality: " + std::to_string(ff.sample_quality) +
                             " is not 0, the standard or the centre multisample pattern");
    switch (ff.strip_cut) {
        case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED:
        case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF:
        case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF: break;
        default:
            return fail(why, "defaults.strip_cut: " + std::to_string(static_cast<int>(ff.strip_cut)) +
                                 " is not a strip-cut value this converter can build");
    }
    switch (ff.rasterizer.FillMode) {
        case D3D12_FILL_MODE_SOLID:
        case D3D12_FILL_MODE_WIREFRAME: break;
        default:
            return fail(why, "defaults.rasterizer.FillMode: " +
                                 std::to_string(static_cast<int>(ff.rasterizer.FillMode)) + " is not a fill mode");
    }
    switch (ff.rasterizer.CullMode) {
        case D3D12_CULL_MODE_NONE:
        case D3D12_CULL_MODE_FRONT:
        case D3D12_CULL_MODE_BACK: break;
        default:
            return fail(why, "defaults.rasterizer.CullMode: " +
                                 std::to_string(static_cast<int>(ff.rasterizer.CullMode)) + " is not a cull mode");
    }
    switch (ff.rasterizer.ConservativeRaster) {
        case D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF:
        case D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON: break;
        default:
            return fail(why, "defaults.rasterizer.ConservativeRaster: " +
                                 std::to_string(static_cast<int>(ff.rasterizer.ConservativeRaster)) +
                                 " is not a conservative-rasterization mode");
    }

    // ---- The description. Every field is set here: nothing is left to the structure's zero value by accident.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = inputs.root_signature;
    desc.VS = {inputs.stages[rt::graphics_record::kStageSlotVs].data,
               inputs.stages[rt::graphics_record::kStageSlotVs].size};
    desc.PS = {inputs.stages[rt::graphics_record::kStageSlotPs].data,
               inputs.stages[rt::graphics_record::kStageSlotPs].size};
    desc.DS = {};
    desc.HS = {};
    desc.GS = {};
    // No stream output: the record has none, and D3D12's stream-output state has no console source here.
    desc.StreamOutput = {};
    desc.BlendState = ff.blend;
    desc.SampleMask = ff.sample_mask;
    desc.RasterizerState = ff.rasterizer;
    desc.DepthStencilState = ff.depth_stencil;
    // An empty input layout: the decoder proved the element count is zero (any element was refused above). The
    // pointer must stay null for NumElements 0, which is what the caller sees a mesh-less fullscreen pipeline use.
    desc.InputLayout = {nullptr, 0};
    desc.IBStripCutValue = ff.strip_cut;
    desc.PrimitiveTopologyType = ff.topology_type;
    desc.NumRenderTargets = inputs.render_target_count;
    for (UINT target = 0; target < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++target)
        desc.RTVFormats[target] = inputs.rtv_formats[target];
    desc.DSVFormat = inputs.dsv_format;
    desc.SampleDesc.Count = ff.sample_count;
    desc.SampleDesc.Quality = ff.sample_quality;
    // Node 0, no flags and no cached PSO: the console record carries none of them, and each is the neutral value the
    // PC runtime takes rather than a console value guessed from the blob.
    desc.NodeMask = 0;
    desc.CachedPSO = {};
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    *out = desc;
    return true;
}

}  // namespace rt::graphics_pso
