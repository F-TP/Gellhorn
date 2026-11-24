// Synthetic records and a synthetic shader index for runtime/src/graphics_pso.*: every byte below is built here. The
// slot-57 record uses the decoder's measured layout (docs/graphics_record.md), the native programs are the 36-byte FSPX
// prologue plus filler words this test chose, and the shader index is written into a scratch folder as
// fspx/index.json plus one module per entry. Nothing in this file is game data, and no device, GPU or game file is
// involved.
//
// Covered:
//   - resolve_native_stage(): a bare kind-6 program found by its SHA-256 key; a prologue-less program found by the same
//     raw key; an FSPX payload matched on its kind-0 state; a program with no translation; an ambiguous one (refused,
//     not guessed); a program published for another stage (named); and false for bytes that are not a native program
//     (an FSSM blob, arbitrary bytes, an unknown program's bytes).
//   - pick_entry(): the binding-free preference and the fits fallback.
//   - build(): every D3D12_GRAPHICS_PIPELINE_STATE_DESC field from the caller's inputs and the record's proved pieces;
//     an empty input layout (the mesh shape) is accepted and any element is refused with the field named; a missing
//     state blob, a missing root signature (the record's and the caller's), a stage without bytes, bytes for a stage
//     the record does not name, bad attachment lists, a zeroed fixed-function state and a mesh stage are refused, each
//     with its field named; the fixed-function state is used verbatim (a second, different state changes the
//     description field for field), so there is no silent default.
// It prints the input and refusal tables so the boundary is visible without reading the header.

#include "../src/graphics_pso.h"
#include "test_temp_dir.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace pso = rt::graphics_pso;
using rt::graphics_record::kStageSlotMs;
using rt::graphics_record::kStageSlotPs;
using rt::graphics_record::kStageSlotVs;
using rt::graphics_record::Record;
using rt::graphics_record::Span;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

using Bytes = std::vector<std::uint8_t>;

void put_u32(Bytes* bytes, std::size_t at, std::uint32_t value) {
    std::memcpy(bytes->data() + at, &value, sizeof value);
}

void put_u64(Bytes* bytes, std::size_t at, std::uint64_t value) {
    std::memcpy(bytes->data() + at, &value, sizeof value);
}

void append_u32(Bytes* bytes, std::uint32_t value) {
    const std::size_t at = bytes->size();
    bytes->resize(at + sizeof value);
    put_u32(bytes, at, value);
}

std::uint64_t pointer_word(const Bytes& blob) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(blob.data()));
}

// A pointer/size pair as the record stores it: pointer at `at`, size at `at + 8`.
void put_span_pair(Bytes* bytes, std::size_t at, const Bytes& blob) {
    put_u64(bytes, at, pointer_word(blob));
    put_u64(bytes, at + 8, blob.size());
}

// The measured 388-byte state sample: the words docs/graphics_record.md quotes at +0x00..+0x14, the 0x40 word at
// +0x150 and four trailing zero bytes. The converter never interprets them; they only have to pass the envelope.
Bytes make_state_blob() {
    Bytes blob(388, 0);
    put_u32(&blob, 0x00, 0x4005d808u);
    put_u32(&blob, 0x04, 0x00000004u);
    put_u32(&blob, 0x08, 0x00100060u);
    put_u32(&blob, 0x0c, 0x00020042u);
    put_u32(&blob, 0x10, 0xb50516a0u);
    put_u32(&blob, 0x150, 0x00000040u);
    return blob;
}

// An 80-byte input layout with no elements: the measured mesh-pipeline shape (docs/shader_library.md section 2), and
// the only input layout build() can turn into a D3D12_INPUT_LAYOUT_DESC today.
Bytes make_empty_layout() { return Bytes(80, 0); }

// The measured 188-byte shape with three elements: count 3 at +0x4C, three NUL-terminated semantics from +0x50, then
// 28 attribute bytes per element. The candidate codes 6/28/16 and the 0x1C word sit in the raw attribute blocks only to
// show the converter refuses the element rather than adopting them as DXGI formats (docs/graphics_record.md section 5).
Bytes make_layout_with_elements() {
    Bytes blob(188, 0);
    put_u32(&blob, 0x4c, 3);
    const char names[] = "POSITION\0COLOR\0TEXCOORD\0";
    std::memcpy(blob.data() + 0x50, names, sizeof names - 1);
    const std::uint32_t candidate_formats[3] = {6, 28, 16};
    const std::size_t attributes = 0x50 + (sizeof names - 1);
    for (std::size_t element = 0; element < 3; ++element) {
        const std::size_t at = attributes + element * 28;
        for (std::size_t word = 0; word < 7; ++word)
            put_u32(&blob, at + 4 * word, 0x01020304u + 0x10u * static_cast<std::uint32_t>(element) +
                                                 static_cast<std::uint32_t>(word));
        put_u32(&blob, at + 4, candidate_formats[element]);
        put_u32(&blob, at + 0x10, 0x1cu);
    }
    return blob;
}

// A kind-2 stage blob: head (kind 2, size word, unknown word, 0x40 tag) and 32-byte records whose tags end with 0x43.
// build() never reads it; decode() only needs it to be the measured shape.
Bytes make_stage_blob(const std::vector<std::uint32_t>& tags) {
    Bytes blob(0x10 + 32 * tags.size(), 0);
    put_u32(&blob, 0x00, 2);
    put_u32(&blob, 0x04, static_cast<std::uint32_t>(blob.size() - 12));
    put_u32(&blob, 0x08, 0x0000abcdu);
    put_u32(&blob, 0x0c, 0x40);
    for (std::size_t index = 0; index < tags.size(); ++index) {
        const std::size_t at = 0x10 + 32 * index;
        put_u32(&blob, at, tags[index]);
        if (tags[index] == 0x43) {
            for (std::size_t byte = 0; byte < 16; ++byte)
                blob[at + 4 + byte] = static_cast<std::uint8_t>((0xa0 + byte) & 0xff);
        } else {
            for (std::size_t word = 0; word < 7; ++word)
                put_u32(&blob, at + 4 + 4 * word, 0x11000000u | static_cast<std::uint32_t>(word));
        }
    }
    return blob;
}

// A synthetic native program (docs/fspx.md, "Kind 6"): the 36-byte prologue of three VOP3 words with opcode 0x180
// (bits 31..16 = 0xD580) and their literals, then a filler word and s_endpgm, padded to the 52-byte minimum the record's
// measured program range allows.
Bytes fspx_code(std::uint32_t seed) {
    Bytes code;
    for (int word = 0; word < 3; ++word) {
        append_u32(&code, 0xd5800000u | static_cast<std::uint32_t>(word));
        append_u32(&code, 0x000000ffu);
        append_u32(&code, seed + static_cast<std::uint32_t>(word));
    }
    append_u32(&code, 0x7e000280u);  // v_mov_b32 v0, 0
    append_u32(&code, 0xbf810000u);  // s_endpgm
    append_u32(&code, 0);            // padding to 52 bytes
    append_u32(&code, 0);
    return code;
}

// The same bytes with word 3 no longer carrying the prologue: compute_program() classifies it as no program, so only
// the SHA-256 of the bytes themselves can name it.
Bytes without_prologue(Bytes code) {
    code[14] ^= 0x01;
    return code;
}

// An FSPX payload with the usual four kinds, laid out after a four-entry directory (body at 80): kind 0 is 28 bytes of
// 0x11, kind 1 is 20 bytes of 0x22, kind 6 is `code`, kind 7 is 8 bytes of 0x77 (docs/fspx.md).
Bytes fspx_payload(const Bytes& code) {
    Bytes directory;
    const std::uint32_t size = static_cast<std::uint32_t>(16 + 16 * 4 + 28 + 20 + code.size() + 8);
    append_u32(&directory, 1);  // version
    append_u32(&directory, 4);  // entries
    append_u32(&directory, size);
    const std::uint32_t n = static_cast<std::uint32_t>(code.size());
    for (const std::array<std::uint32_t, 4>& entry :
         {std::array<std::uint32_t, 4>{0, 80, 28, 0}, std::array<std::uint32_t, 4>{1, 108, 20, 0},
          std::array<std::uint32_t, 4>{6, 128, n, 0}, std::array<std::uint32_t, 4>{7, 128 + n, 8, 0}}) {
        for (const std::uint32_t word : entry) append_u32(&directory, word);
    }
    Bytes payload = {'F', 'S', 'P', 'X'};
    payload.insert(payload.end(), directory.begin(), directory.end());
    payload.insert(payload.end(), 28, 0x11);
    payload.insert(payload.end(), 20, 0x22);
    payload.insert(payload.end(), code.begin(), code.end());
    payload.insert(payload.end(), 8, 0x77);
    return payload;
}

// The slot-57 record: 0x108 validated bytes (the same shape runtime/test/graphics_record_test.cpp builds) plus an
// unidentified tail decode() must ignore. `slots` names the stage slots used, in record order.
Bytes make_descriptor(const Bytes& state, const Bytes& layout, const std::vector<int>& slots, const Bytes& ps_program,
                      const Bytes& ps_blob, const Bytes& vs_program, const Bytes& vs_blob, const Bytes& ms_program = {},
                      const Bytes& ms_blob = {}) {
    Bytes descriptor(0x120, 0);
    put_u64(&descriptor, 0x00, 0x1122334455667788ull);  // the root signature pointer value; never dereferenced
    put_span_pair(&descriptor, 0x08, state);
    put_span_pair(&descriptor, 0x18, layout);
    for (const int slot : slots) {
        const std::size_t at = 0x28 + 0x20 * static_cast<std::size_t>(slot);
        const Bytes* program = slot == kStageSlotPs ? &ps_program : slot == kStageSlotVs ? &vs_program : &ms_program;
        const Bytes* blob = slot == kStageSlotPs ? &ps_blob : slot == kStageSlotVs ? &vs_blob : &ms_blob;
        put_span_pair(&descriptor, at, *program);
        put_span_pair(&descriptor, at + 0x10, *blob);
    }
    for (std::size_t at = 0x108; at < descriptor.size(); ++at) descriptor[at] = 0xee;  // the unknown tail
    return descriptor;
}

// A decoded record from the named slots (PS+VS when omitted).
Record make_record(const std::vector<int>& slots = {kStageSlotPs, kStageSlotVs}, const Bytes& layout = Bytes{}) {
    const Bytes state = make_state_blob();
    const Bytes input_layout = layout.empty() ? make_empty_layout() : layout;
    const Bytes ps_program = fspx_code(0x101);
    const Bytes vs_program = fspx_code(0x202);
    const Bytes ms_program = fspx_code(0x303);
    const Bytes ps_blob = make_stage_blob({0x41, 0x43});
    const Bytes vs_blob = make_stage_blob({0x41, 0x43});
    const Bytes ms_blob = make_stage_blob({0x42, 0x43});
    const Bytes descriptor =
        make_descriptor(state, input_layout, slots, ps_program, ps_blob, vs_program, vs_blob, ms_program, ms_blob);
    Record record;
    if (!rt::graphics_record::decode(descriptor.data(), descriptor.size(), &record)) record.stage_count = 0;
    return record;
}

// ---- The synthetic shader index -----------------------------------------------------------------------------------

struct LibraryEntry {
    std::string name;
    std::string stage;
    std::string code_hash;
    std::string state_hash;  // empty: no kind-0 key
    std::uint32_t module = 1;  // the module's last word, so two entries can differ in their SPIR-V
};

// <dir>/index.json (empty: every program here is an FSPX one) and <dir>/fspx/index.json with one entry per program,
// plus <dir>/fspx/<name>.spv for each (docs/fspx.md: FSPX entries are found by fspx_code_sha256 in that subfolder and
// their modules live beside it). No game data: the hashes are of the synthetic programs above.
bool write_library(const std::filesystem::path& dir, const std::vector<LibraryEntry>& entries) {
    std::error_code ec;
    std::filesystem::create_directories(dir / "fspx", ec);
    if (ec) return false;
    {
        std::ofstream index(dir / "index.json", std::ios::binary);
        index << "{}";
        if (!index) return false;
    }
    {
        std::ofstream index(dir / "fspx" / "index.json", std::ios::binary);
        index << "{";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const LibraryEntry& entry = entries[i];
            if (i) index << ",";
            index << "\"" << entry.name << "\": {\"stage\": \"" << entry.stage << "\", \"fspx_code_sha256\": \""
                  << entry.code_hash << "\"";
            if (!entry.state_hash.empty()) index << ", \"fspx_state_sha256\": \"" << entry.state_hash << "\"";
            index << "}";
        }
        index << "}";
        if (!index) return false;
    }
    for (const LibraryEntry& entry : entries) {
        std::ofstream module(dir / "fspx" / (entry.name + ".spv"), std::ios::binary);
        const std::uint32_t words[5] = {0x07230203, 0x00010500, 0, 1, entry.module};
        module.write(reinterpret_cast<const char*>(words), sizeof words);
        if (!module) return false;
    }
    return true;
}

bool load_library(testfiles::ScratchDir& scratch, const std::vector<LibraryEntry>& entries,
                  leonida::ShaderLibrary* library) {
    if (!write_library(scratch.path(), entries)) return false;
    std::string error;
    if (!library->load(scratch.path().string(), &error)) {
        std::fprintf(stderr, "graphics_pso_test: the synthetic shader index did not load: %s\n", error.c_str());
        return false;
    }
    return true;
}

const auto kFitsAll = [](const leonida::ShaderEntry&) { return true; };

std::string hash_of(const Bytes& bytes) { return leonida::sha256_hex(bytes); }

std::string state_hash_of_0x11() { return hash_of(Bytes(28, 0x11)); }

// ---- Tests -------------------------------------------------------------------------------------------------------

void test_resolve_native_stage() {
    testfiles::ScratchDir scratch("leonida_graphics_pso_resolve");
    leonida::ShaderLibrary library;

    const Bytes ps_code = fspx_code(0x101);
    const Bytes vs_code = fspx_code(0x202);
    const Bytes untranslated = fspx_code(0x909);
    const Bytes raw_only = without_prologue(fspx_code(0x404));
    const Bytes payload_code = fspx_code(0x505);
    const Bytes ambiguous_code = fspx_code(0x606);
    const Bytes duplicated_code = fspx_code(0x707);
    const bool loaded = load_library(scratch,
                                     {
                                         {"lib_ps_00001", "ps", hash_of(ps_code)},
                                         {"lib_vs_00002", "vs", hash_of(vs_code)},
                                         {"lib_ps_00003", "ps", hash_of(raw_only)},
                                         {"lib_ps_00004", "ps", hash_of(payload_code), state_hash_of_0x11()},
                                         {"lib_ps_00005", "ps", hash_of(ambiguous_code), "", 11},
                                         {"lib_ps_00006", "ps", hash_of(ambiguous_code), "", 22},
                                         {"lib_ps_00007", "ps", hash_of(duplicated_code), "", 33},
                                         {"lib_ps_00008", "ps", hash_of(duplicated_code), "", 33},
                                     },
                                     &library);
    check(loaded, "resolve: the synthetic shader index loads");
    if (!loaded) return;

    // Bytes that are not a native program are not this path's: the FSSM/DXBC and stand-in paths, and their refusal,
    // stay in charge.
    const Bytes fssm = {'F', 'S', 'S', 'M', ' ', 'b', 'l', 'o', 'b'};
    pso::NativeStage native;
    std::string why;
    check(!pso::resolve_native_stage(fssm, "ps", library, kFitsAll, &native, &why),
          "resolve: an FSSM blob is not a native stage");
    check(!pso::resolve_native_stage(Bytes(64, 0xaa), "ps", library, kFitsAll, &native, &why),
          "resolve: arbitrary bytes with no index entry are not a native stage");
    check(pso::resolve_native_stage(untranslated, "ps", library, kFitsAll, &native, &why),
          "resolve: a classified program with no translation is still a native stage (reported, not discarded)");

    // A bare kind-6 program found by its SHA-256 key, the stage named by the caller.
    why.clear();
    check(pso::resolve_native_stage(ps_code, "ps", library, kFitsAll, &native, &why),
          "resolve: a bare kind-6 program is a native stage");
    check(native.entry && native.entry->name == "fspx/lib_ps_00001", "resolve: the ps entry is found");
    check(native.program.bytes == ps_code, "resolve: the kind-6 bytes are the program, as create_compute takes them");
    check(!native.raw_key, "resolve: a classified program is not the raw-key path");
    check(why.empty(), "resolve: no refusal message when the entry is found");

    // The record slot gives the stage: the same bytes looked up as the vertex stage are not there.
    why.clear();
    check(pso::resolve_native_stage(ps_code, "vs", library, kFitsAll, &native, &why),
          "resolve: the same program under another stage is still a native stage");
    check(!native.entry, "resolve: it has no entry under the wrong stage");
    check(why.find("stage mismatch") != std::string::npos && why.find(" published for stage ps") != std::string::npos,
          "resolve: the refusal names the stage the program is published for");

    // No translation: classified, but the index holds nothing under its key.
    why.clear();
    check(pso::resolve_native_stage(untranslated, "ps", library, kFitsAll, &native, &why),
          "resolve: an untranslated native program is classified");
    check(!native.entry && why.find("no translation") != std::string::npos,
          "resolve: an untranslated native program is refused with 'no translation'");

    // A program without the FSPX prologue: compute_program cannot classify it, but the publication's key is the SHA-256
    // of the bytes as passed, so it resolves by that key.
    const leonida::ComputeProgram unclassified = leonida::compute_program(raw_only);
    check(unclassified.kind == leonida::ComputeProgram::Kind::none,
          "resolve: the prologue-less program is not FSPX-classifiable");
    why.clear();
    check(pso::resolve_native_stage(raw_only, "ps", library, kFitsAll, &native, &why),
          "resolve: a prologue-less program with an index entry is a native stage");
    check(native.entry && native.entry->name == "fspx/lib_ps_00003", "resolve: the raw-key entry is found");
    check(native.raw_key, "resolve: the raw-key path is recorded");
    check(native.program.bytes == raw_only, "resolve: the raw-key program bytes are the bytecode itself");

    // An FSPX payload: the kind-0 state picks among translations of the same kind-6 bytes.
    const Bytes payload = fspx_payload(payload_code);
    why.clear();
    check(pso::resolve_native_stage(payload, "ps", library, kFitsAll, &native, &why),
          "resolve: an FSPX payload is a native stage");
    check(native.entry && native.entry->name == "fspx/lib_ps_00004", "resolve: the payload resolves by its kind-6 key");
    check(native.program.bytes == payload_code && native.program.state == Bytes(28, 0x11),
          "resolve: the payload's kind-6 bytes and kind-0 state are kept");
    Bytes wrong_state = payload;
    wrong_state[80] = 0x12;  // the first kind-0 byte
    why.clear();
    check(pso::resolve_native_stage(wrong_state, "ps", library, kFitsAll, &native, &why),
          "resolve: a payload with a different kind-0 state is still classified");
    check(!native.entry && why.find("kind-0 state") != std::string::npos,
          "resolve: the refusal names the kind-0 lookup");

    // Two translations of the same kind-6 bytes with different modules and no kind-0 state: ambiguous, so refused
    // rather than picking one.
    why.clear();
    check(pso::resolve_native_stage(ambiguous_code, "ps", library, kFitsAll, &native, &why),
          "resolve: an ambiguous program is classified");
    check(!native.entry, "resolve: an ambiguous program has no entry");
    check(why.find("ambiguous") != std::string::npos,
          "resolve: an ambiguous program is refused with 'ambiguous', not guessed");

    // Two identical translations (same module, same metadata) are not ambiguous: the first is built.
    why.clear();
    check(pso::resolve_native_stage(duplicated_code, "ps", library, kFitsAll, &native, &why),
          "resolve: two identical translations are a native stage");
    check(native.entry && native.entry->name == "fspx/lib_ps_00007",
          "resolve: identical translations resolve to the first entry");

    // A vertex stage's entry must be one whose inputs this PSO's layout feeds: `fits` is the caller's test.
    why.clear();
    check(pso::resolve_native_stage(vs_code, "vs", library, [](const leonida::ShaderEntry&) { return false; }, &native,
                                    &why),
          "resolve: a stage whose entries do not fit is still classified");
    check(native.entry && native.entry->name == "fspx/lib_vs_00002",
          "resolve: with no fitting entry the first one is still chosen (as pick_entry has always done)");

    check(!pso::resolve_native_stage(ps_code, "ps", library, kFitsAll, nullptr, &why),
          "resolve: a null output is refused rather than written");
    check(!pso::resolve_native_stage(std::span<const std::uint8_t>{}, "ps", library, kFitsAll, &native, &why),
          "resolve: empty bytecode is not a native stage");
}

void test_pick_entry() {
    leonida::ShaderEntry unbound;
    leonida::ShaderEntry bound;
    bound.binding_count = 2;
    leonida::ShaderEntry bound2;
    bound2.binding_count = 3;
    const std::vector<const leonida::ShaderEntry*> both{&bound, &unbound};
    check(pso::pick_entry(both, kFitsAll) == &unbound,
          "pick_entry: an entry with no set-0 bindings wins over an earlier one with bindings");
    const std::vector<const leonida::ShaderEntry*> fits_first{&bound, &bound2};
    check(pso::pick_entry(fits_first, [&bound2](const leonida::ShaderEntry& e) { return &e == &bound2; }) == &bound2,
          "pick_entry: without an unbound entry, the first that fits wins");
    check(pso::pick_entry(fits_first, [&bound](const leonida::ShaderEntry& e) { return &e == &bound; }) == &bound,
          "pick_entry: the first fit wins when several do (only one fits here)");
    const std::vector<const leonida::ShaderEntry*> none{};
    check(pso::pick_entry(none, kFitsAll) == nullptr, "pick_entry: no candidates gives no entry");
}

// Every field of the documented default set, spelled out so docs/graphics_pso.md and the code cannot drift apart.
void test_documented_default_state() {
    const pso::FixedFunctionState state = pso::documented_default_state();
    check(state.rasterizer.FillMode == D3D12_FILL_MODE_SOLID, "defaults: rasterizer fill is solid");
    check(state.rasterizer.CullMode == D3D12_CULL_MODE_BACK, "defaults: back faces are culled");
    check(state.rasterizer.FrontCounterClockwise == FALSE, "defaults: clockwise is front");
    check(state.rasterizer.DepthBias == 0 && state.rasterizer.DepthBiasClamp == 0.0f &&
              state.rasterizer.SlopeScaledDepthBias == 0.0f,
          "defaults: no depth bias");
    check(state.rasterizer.DepthClipEnable == TRUE, "defaults: depth clipping is on");
    check(state.rasterizer.MultisampleEnable == FALSE && state.rasterizer.AntialiasedLineEnable == FALSE,
          "defaults: multisample rasterization and antialiased lines are off");
    check(state.rasterizer.ForcedSampleCount == 0 &&
              state.rasterizer.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
          "defaults: no forced samples and no conservative rasterization");
    check(state.blend.AlphaToCoverageEnable == FALSE && state.blend.IndependentBlendEnable == FALSE,
          "defaults: blending off, one blend state for every target");
    bool blend_off = true, masks_all = true;
    for (const D3D12_RENDER_TARGET_BLEND_DESC& target : state.blend.RenderTarget) {
        blend_off = blend_off && target.BlendEnable == FALSE && target.LogicOpEnable == FALSE;
        masks_all = masks_all && target.RenderTargetWriteMask == D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    check(blend_off, "defaults: no target enables blending or a logic op");
    check(masks_all, "defaults: every target writes every colour channel");
    check(state.depth_stencil.DepthEnable == FALSE && state.depth_stencil.StencilEnable == FALSE,
          "defaults: depth and stencil are off");
    check(state.depth_stencil.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL &&
              state.depth_stencil.DepthFunc == D3D12_COMPARISON_FUNC_LESS,
          "defaults: the depth fields are still valid API values");
    check(state.depth_stencil.StencilReadMask == 0xff && state.depth_stencil.StencilWriteMask == 0xff,
          "defaults: the stencil masks are the API's full masks");
    const auto neutral_face = [](const D3D12_DEPTH_STENCILOP_DESC& face) {
        return face.StencilFailOp == D3D12_STENCIL_OP_KEEP && face.StencilDepthFailOp == D3D12_STENCIL_OP_KEEP &&
               face.StencilPassOp == D3D12_STENCIL_OP_KEEP && face.StencilFunc == D3D12_COMPARISON_FUNC_ALWAYS;
    };
    check(neutral_face(state.depth_stencil.FrontFace) && neutral_face(state.depth_stencil.BackFace),
          "defaults: the stencil ops are the neutral ones");
    check(state.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, "defaults: triangles");
    check(state.strip_cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED, "defaults: no strip cut");
    check(state.sample_count == 1 && state.sample_quality == 0 && state.sample_mask == 0xffffffffu,
          "defaults: one sample, full mask");
}

pso::GraphicsPsoInputs base_inputs(const void* ps, SIZE_T ps_size, const void* vs, SIZE_T vs_size) {
    pso::GraphicsPsoInputs inputs{};
    inputs.root_signature = reinterpret_cast<ID3D12RootSignature*>(static_cast<std::uintptr_t>(0x1000));
    inputs.rtv_formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    inputs.render_target_count = 1;
    inputs.dsv_format = DXGI_FORMAT_UNKNOWN;
    inputs.defaults = pso::documented_default_state();
    inputs.stages[kStageSlotPs] = {ps, ps_size};
    inputs.stages[kStageSlotVs] = {vs, vs_size};
    return inputs;
}

void test_build_success() {
    const Record record = make_record();
    check(record.stage_count == 2, "build: the synthetic PS+VS record decodes");
    const Bytes ps = fspx_code(0x101);
    const Bytes vs = fspx_code(0x202);
    const pso::GraphicsPsoInputs inputs = base_inputs(ps.data(), ps.size(), vs.data(), vs.size());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.SampleDesc.Count = 77;  // a sentinel: build() must overwrite it, so it is not relying on a zeroed structure
    std::string why;
    check(pso::build(record, inputs, &desc, &why), "build: a PS+VS record with an empty input layout builds");
    check(why.empty(), "build: no refusal message on success");
    check(desc.pRootSignature == inputs.root_signature, "build: pRootSignature is the caller's root signature");
    check(desc.PS.pShaderBytecode == ps.data() && desc.PS.BytecodeLength == ps.size(),
          "build: the pixel stage's bytecode is the caller's resolved PS program");
    check(desc.VS.pShaderBytecode == vs.data() && desc.VS.BytecodeLength == vs.size(),
          "build: the vertex stage's bytecode is the caller's resolved VS program");
    check(!desc.DS.pShaderBytecode && !desc.DS.BytecodeLength && !desc.HS.pShaderBytecode &&
              !desc.HS.BytecodeLength && !desc.GS.pShaderBytecode && !desc.GS.BytecodeLength,
          "build: hull, domain and geometry stages are empty (a record has no such slots)");
    check(desc.StreamOutput.NumEntries == 0 && desc.StreamOutput.pSODeclaration == nullptr &&
              desc.StreamOutput.NumStrides == 0 && desc.StreamOutput.pBufferStrides == nullptr &&
              desc.StreamOutput.RasterizedStream == 0,
          "build: no stream output");
    check(std::memcmp(&desc.BlendState, &inputs.defaults.blend, sizeof desc.BlendState) == 0,
          "build: the blend state is the caller's, field for field");
    check(desc.SampleMask == inputs.defaults.sample_mask, "build: the sample mask is the caller's");
    check(std::memcmp(&desc.RasterizerState, &inputs.defaults.rasterizer, sizeof desc.RasterizerState) == 0,
          "build: the rasterizer state is the caller's, field for field");
    check(std::memcmp(&desc.DepthStencilState, &inputs.defaults.depth_stencil, sizeof desc.DepthStencilState) == 0,
          "build: the depth-stencil state is the caller's, field for field");
    check(desc.InputLayout.NumElements == 0 && desc.InputLayout.pInputElementDescs == nullptr,
          "build: an empty input layout is what is proved");
    check(desc.IBStripCutValue == inputs.defaults.strip_cut, "build: the strip-cut value is the caller's");
    check(desc.PrimitiveTopologyType == inputs.defaults.topology_type, "build: the topology type is the caller's");
    check(desc.NumRenderTargets == 1 && desc.RTVFormats[0] == DXGI_FORMAT_R8G8B8A8_UNORM,
          "build: the render-target count and format are the caller's");
    bool unused_rt_unknown = true;
    for (UINT target = 1; target < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++target)
        unused_rt_unknown = unused_rt_unknown && desc.RTVFormats[target] == DXGI_FORMAT_UNKNOWN;
    check(unused_rt_unknown, "build: the unused render targets stay DXGI_FORMAT_UNKNOWN");
    check(desc.DSVFormat == DXGI_FORMAT_UNKNOWN, "build: no depth-stencil format when the caller gave none");
    check(desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0, "build: the sample description is the caller's");
    check(desc.NodeMask == 0 && desc.Flags == D3D12_PIPELINE_STATE_FLAG_NONE,
          "build: node 0 and no pipeline flags (the record carries neither)");
    check(desc.CachedPSO.pCachedBlob == nullptr && desc.CachedPSO.CachedBlobSizeInBytes == 0,
          "build: no cached PSO blob");

    // A second, deliberately different state must reach the description unchanged: there is no silent default.
    pso::GraphicsPsoInputs custom = inputs;
    custom.defaults.rasterizer.FillMode = D3D12_FILL_MODE_WIREFRAME;
    custom.defaults.rasterizer.CullMode = D3D12_CULL_MODE_FRONT;
    custom.defaults.rasterizer.FrontCounterClockwise = TRUE;
    custom.defaults.rasterizer.DepthBias = 7;
    custom.defaults.rasterizer.DepthBiasClamp = 0.25f;
    custom.defaults.rasterizer.SlopeScaledDepthBias = 1.5f;
    custom.defaults.rasterizer.DepthClipEnable = FALSE;
    custom.defaults.rasterizer.MultisampleEnable = TRUE;
    custom.defaults.rasterizer.AntialiasedLineEnable = TRUE;
    custom.defaults.blend.AlphaToCoverageEnable = TRUE;
    custom.defaults.blend.IndependentBlendEnable = TRUE;
    custom.defaults.blend.RenderTarget[0].BlendEnable = TRUE;
    custom.defaults.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    custom.defaults.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    custom.defaults.blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
    custom.defaults.depth_stencil.DepthEnable = TRUE;
    custom.defaults.depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    custom.defaults.depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    custom.defaults.depth_stencil.StencilEnable = TRUE;
    custom.defaults.depth_stencil.StencilReadMask = 0x0f;
    custom.defaults.depth_stencil.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    custom.defaults.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    custom.defaults.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    custom.defaults.sample_count = 4;
    custom.defaults.sample_mask = 0x0f0f0f0f;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC custom_desc{};
    check(pso::build(record, custom, &custom_desc, &why), "build: a caller-chosen state builds");
    check(std::memcmp(&custom_desc.RasterizerState, &custom.defaults.rasterizer, sizeof custom_desc.RasterizerState) ==
              0,
          "build: the caller's rasterizer state is used verbatim, not a default");
    check(std::memcmp(&custom_desc.BlendState, &custom.defaults.blend, sizeof custom_desc.BlendState) == 0,
          "build: the caller's blend state is used verbatim");
    check(std::memcmp(&custom_desc.DepthStencilState, &custom.defaults.depth_stencil,
                      sizeof custom_desc.DepthStencilState) == 0,
          "build: the caller's depth-stencil state is used verbatim");
    check(custom_desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE &&
              custom_desc.IBStripCutValue == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF &&
              custom_desc.SampleDesc.Count == 4 && custom_desc.SampleMask == 0x0f0f0f0f,
          "build: topology, strip cut, sample count and mask are the caller's");
    // An FSPX payload is what the caller resolved when it has the program's kind-0 state: build() writes the bytes it
    // was given and create_graphics resolves them again.
    const Bytes payload = fspx_payload(fspx_code(0x101));
    pso::GraphicsPsoInputs with_payload = inputs;
    with_payload.stages[kStageSlotPs] = {payload.data(), payload.size()};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC payload_desc{};
    check(pso::build(record, with_payload, &payload_desc, &why), "build: an FSPX payload as the PS bytecode builds");
    check(payload_desc.PS.pShaderBytecode == payload.data() && payload_desc.PS.BytecodeLength == payload.size(),
          "build: the payload bytes go into the description as given");
}

void test_build_ps_only() {
    const Record record = make_record({kStageSlotPs});
    check(record.stage_count == 1 && record.stages[0].slot == kStageSlotPs, "build: the PS-only record decodes");
    const Bytes ps = fspx_code(0x101);
    const pso::GraphicsPsoInputs inputs = base_inputs(ps.data(), ps.size(), nullptr, 0);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::string why;
    check(pso::build(record, inputs, &desc, &why), "build: a PS-only record builds");
    check(desc.PS.pShaderBytecode == ps.data() && !desc.VS.pShaderBytecode,
          "build: a PS-only record has a pixel stage and no vertex stage");
}

void test_build_refusals() {
    const Record record = make_record();
    const Bytes ps = fspx_code(0x101);
    const Bytes vs = fspx_code(0x202);
    const pso::GraphicsPsoInputs good = base_inputs(ps.data(), ps.size(), vs.data(), vs.size());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::string why;

    // The input layout: any element is refused with its format field named, because the console format code is not
    // mapped to a DXGI format and the attribute bytes are undecoded.
    const Record with_elements = make_record({kStageSlotPs, kStageSlotVs}, make_layout_with_elements());
    check(with_elements.stage_count == 2, "build: the record with three layout elements decodes");
    check(!pso::build(with_elements, good, &desc, &why),
          "build: a record whose input layout has an element is refused");
    check(why.find("input_layout.elements[0].format") != std::string::npos && why.find("POSITION") != std::string::npos,
          "build: the refusal names input_layout.elements[0].format and the element's semantic");
    check(why.find("D3D12_INPUT_ELEMENT_DESC") != std::string::npos,
          "build: the refusal says no D3D12_INPUT_ELEMENT_DESC can be built");

    // The fixed-function state blob.
    Record no_state = record;
    no_state.state = Span{};
    why.clear();
    check(!pso::build(no_state, good, &desc, &why), "build: a record without its state blob is refused");
    check(why.find("record.state") != std::string::npos, "build: the refusal names record.state");

    // The root signature: the record's, and the caller's mapping of it.
    Record no_root = record;
    no_root.root_signature = nullptr;
    why.clear();
    check(!pso::build(no_root, good, &desc, &why), "build: a record without a root signature is refused");
    check(why.find("record.root_signature") != std::string::npos, "build: the refusal names record.root_signature");
    pso::GraphicsPsoInputs no_pc_root = good;
    no_pc_root.root_signature = nullptr;
    why.clear();
    check(!pso::build(record, no_pc_root, &desc, &why), "build: a missing PC root signature is refused");
    check(why.find("pRootSignature") != std::string::npos, "build: the refusal names pRootSignature");

    // The per-stage bytecode: a stage the record names without bytes, and bytes for a stage it does not name.
    pso::GraphicsPsoInputs no_vs = good;
    no_vs.stages[kStageSlotVs] = {};
    why.clear();
    check(!pso::build(record, no_vs, &desc, &why), "build: a record stage without resolved program bytes is refused");
    check(why.find("stages[1] (vs)") != std::string::npos, "build: the refusal names the stage slot");
    const Record ps_only = make_record({kStageSlotPs});
    why.clear();
    check(!pso::build(ps_only, good, &desc, &why), "build: bytes for a stage the record does not name are refused");
    check(why.find("stages[1] (vs)") != std::string::npos && why.find("no vs stage") != std::string::npos,
          "build: the refusal says the record has no vs stage");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC untouched{};
    untouched.SampleDesc.Count = 77;
    check(!pso::build(record, no_vs, &untouched, &why) && untouched.SampleDesc.Count == 77,
          "build: the output description is untouched on a refusal");

    // The attachment lists.
    pso::GraphicsPsoInputs too_many = good;
    too_many.render_target_count = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1;
    why.clear();
    check(!pso::build(record, too_many, &desc, &why), "build: more render targets than D3D12 allows is refused");
    check(why.find("render_target_count") != std::string::npos, "build: the refusal names render_target_count");
    pso::GraphicsPsoInputs unknown_format = good;
    unknown_format.render_target_count = 2;  // rtv_formats[1] stays DXGI_FORMAT_UNKNOWN
    why.clear();
    check(!pso::build(record, unknown_format, &desc, &why), "build: a used render target with no format is refused");
    check(why.find("rtv_formats[1]") != std::string::npos, "build: the refusal names the render-target slot");
    pso::GraphicsPsoInputs stray_format = good;
    stray_format.rtv_formats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;  // unused, so D3D12 requires UNKNOWN
    why.clear();
    check(!pso::build(record, stray_format, &desc, &why), "build: a format on an unused render target is refused");
    check(why.find("rtv_formats[1]") != std::string::npos, "build: the refusal names the unused render-target slot");

    // The fixed-function state: a zeroed one is not "the console's", it is refused field by field.
    pso::GraphicsPsoInputs zeroed = good;
    zeroed.defaults = {};
    why.clear();
    check(!pso::build(record, zeroed, &desc, &why), "build: a zeroed fixed-function state is refused");
    check(why.find("defaults.topology_type") != std::string::npos,
          "build: the refusal names defaults.topology_type");
    pso::GraphicsPsoInputs bad_samples = good;
    bad_samples.defaults.sample_count = 3;
    why.clear();
    check(!pso::build(record, bad_samples, &desc, &why), "build: a non-power-of-two sample count is refused");
    check(why.find("defaults.sample_count") != std::string::npos, "build: the refusal names defaults.sample_count");
    pso::GraphicsPsoInputs bad_quality = good;
    bad_quality.defaults.sample_quality = 5;
    why.clear();
    check(!pso::build(record, bad_quality, &desc, &why), "build: an unknown sample quality is refused");
    check(why.find("defaults.sample_quality") != std::string::npos, "build: the refusal names defaults.sample_quality");
    pso::GraphicsPsoInputs bad_fill = good;
    bad_fill.defaults.rasterizer.FillMode = static_cast<D3D12_FILL_MODE>(4);
    why.clear();
    check(!pso::build(record, bad_fill, &desc, &why), "build: an unknown fill mode is refused");
    check(why.find("defaults.rasterizer.FillMode") != std::string::npos,
          "build: the refusal names defaults.rasterizer.FillMode");
    pso::GraphicsPsoInputs bad_cull = good;
    bad_cull.defaults.rasterizer.CullMode = static_cast<D3D12_CULL_MODE>(4);
    why.clear();
    check(!pso::build(record, bad_cull, &desc, &why), "build: an unknown cull mode is refused");
    check(why.find("defaults.rasterizer.CullMode") != std::string::npos,
          "build: the refusal names defaults.rasterizer.CullMode");
    pso::GraphicsPsoInputs bad_strip = good;
    bad_strip.defaults.strip_cut = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(7);
    why.clear();
    check(!pso::build(record, bad_strip, &desc, &why), "build: an unknown strip-cut value is refused");
    check(why.find("defaults.strip_cut") != std::string::npos, "build: the refusal names defaults.strip_cut");

    // A mesh stage cannot be expressed in a D3D12_GRAPHICS_PIPELINE_STATE_DESC.
    const Record ps_ms = make_record({kStageSlotPs, kStageSlotMs});
    check(ps_ms.stage_count == 2, "build: a PS+MS record decodes");
    pso::GraphicsPsoInputs with_ms = base_inputs(ps.data(), ps.size(), nullptr, 0);
    with_ms.stages[kStageSlotMs] = {ps.data(), ps.size()};
    why.clear();
    check(!pso::build(ps_ms, with_ms, &desc, &why), "build: a record with a mesh stage is refused");
    check(why.find("mesh stage") != std::string::npos && why.find("PIPELINE_STATE_STREAM") != std::string::npos,
          "build: the refusal says a mesh stage needs the pipeline-state-stream path");

    // Caller-built records that decode() would never produce are still checked.
    Record no_stages;
    no_stages.stage_count = 0;
    why.clear();
    check(!pso::build(no_stages, good, &desc, &why), "build: a record with no stages is refused");
    check(why.find("record.stage_count") != std::string::npos, "build: the refusal names record.stage_count");
    Record bad_slot = record;
    bad_slot.stages[0].slot = 5;
    why.clear();
    check(!pso::build(bad_slot, good, &desc, &why), "build: a stage slot outside the record's three is refused");
    check(why.find("slot 5") != std::string::npos, "build: the refusal names the slot");
    Record twice = record;
    twice.stage_count = 2;
    twice.stages[0].slot = kStageSlotPs;
    twice.stages[1].slot = kStageSlotPs;
    why.clear();
    check(!pso::build(twice, good, &desc, &why), "build: the same stage slot twice is refused");
    check(why.find("appears twice") != std::string::npos, "build: the refusal says the slot appears twice");

    check(!pso::build(record, good, nullptr, &why), "build: a null output description is refused");
}

// The input and refusal surface, printed for the next reader.
void print_findings() {
    struct Row {
        const char* item;
        const char* source;
    };
    const Row rows[] = {
        {"pRootSignature", "caller, mapped from record.root_signature (decoder copies only the pointer)"},
        {"VS/PS bytecode", "caller's resolved native program per record slot (resolve_native_stage)"},
        {"DS/HS/GS", "empty: a record has no such slot"},
        {"InputLayout", "decoded; empty only - any element refused at input_layout.elements[i].format"},
        {"Rasterizer/Blend/DepthStencil/Sample/Topology", "caller's FixedFunctionState (docs/graphics_pso.md)"},
        {"RTVFormats/DSVFormat/NumRenderTargets", "caller"},
        {"NodeMask/Flags/CachedPSO/StreamOutput", "neutral values: the record carries none of them"},
        {"record.state", "decoded for its envelope only; no field is used"},
        {"mesh record (slot 2)", "refused: D3D12_GRAPHICS_PIPELINE_STATE_DESC has no mesh field"},
    };
    std::printf("graphics_pso_test: where each D3D12_GRAPHICS_PIPELINE_STATE_DESC field comes from\n");
    std::printf("%-44s %s\n", "field", "source");
    for (const Row& row : rows) std::printf("%-44s %s\n", row.item, row.source);
    std::printf("\n");
    const Row refusals[] = {
        {"build", "record.stage_count / record.stages[i].slot: not a graphics record's shape"},
        {"build", "record.root_signature: no console root signature at +0x00"},
        {"build", "record.state: the fixed-function blob is missing or outside 380..520 B"},
        {"build", "record.input_layout: the layout blob does not decode"},
        {"build", "input_layout.elements[i].format: an element's format code is not mapped to DXGI"},
        {"build", "pRootSignature: the caller mapped no PC root signature"},
        {"build", "stages[i] (stage): the record stage has no resolved bytes, or gets bytes it has no slot for"},
        {"build", "render_target_count / rtv_formats[i]: an attachment list D3D12 would refuse"},
        {"build", "defaults.*: the caller's fixed-function state is not a valid value"},
        {"build", "mesh stage: needs D3D12_PIPELINE_STATE_STREAM_DESC's MS subobject"},
        {"resolve_native_stage", "no translation / stage mismatch / ambiguous: no single module can be chosen"},
        {"resolve_native_stage", "returns false: the bytes are not a native program (FSSM/stand-in path applies)"},
    };
    std::printf("graphics_pso_test: refusal surface\n");
    std::printf("%-24s %s\n", "function", "refusal");
    for (const Row& row : refusals) std::printf("%-24s %s\n", row.item, row.source);
    std::printf("\n");
}

}  // namespace

int main() {
    print_findings();

    test_resolve_native_stage();
    test_pick_entry();
    test_documented_default_state();
    test_build_success();
    test_build_ps_only();
    test_build_refusals();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s) of %d check(s)\n", g_failures, g_checks);
        return 1;
    }
    std::printf("graphics_pso_test: all %d checks passed\n", g_checks);
    return 0;
}
