// The console's deserialized graphics pipeline record (device slot 57), decoded into the pieces a PC
// D3D12_GRAPHICS_PIPELINE_STATE_DESC needs. Pure decoding over byte spans: this file creates no device object, links
// nothing and never reads through a pointer taken out of the record.
//
// The record is the expansion of a pipeline-library graphics record (docs/boot_frontier.md, "slot 57 is a deserialized
// graphics pipeline record"; docs/shader_library.md section 2):
//
//   +0x00  root signature pointer
//   +0x08  fixed-function state blob pointer       +0x10  size (380..520 bytes)
//   +0x18  input-layout blob pointer               +0x20  size (80..340 bytes)
//   +0x28  slot 0 (PS) native program pointer      +0x30  size
//   +0x38  slot 0 (PS) kind-2 stage blob pointer   +0x40  size
//   +0x48  slot 1 (VS) native program pointer      +0x50  size
//   +0x58  slot 1 (VS) kind-2 stage blob pointer   +0x60  size
//   +0x68  slot 2 (MS) native program pointer      +0x70  size
//   +0x78  slot 2 (MS) kind-2 stage blob pointer   +0x80  size
//
// Unused slots are all zero. Everything after the last used pair is zero to +0x108; the tail from +0x108 is
// unidentified and this decoder ignores it. The record slot names the stage - the stage blob itself carries no stage
// identity (docs/boot_frontier.md; docs/shader_library.md section 2, "The slot gives the stage").
//
// Decoded vs unknown (docs/graphics_record.md has the field table with the evidence for every row):
//   decoded: the six pointer/size pairs and their offsets, which slot/stage each pair is, the record's measured size
//            envelope (state and input-layout ranges, program size range, kind-2 blob record envelope), the input
//            layout's element count, its NUL-terminated semantics and its 28-byte attribute blocks, and the stage
//            blob's kind word, size word, 0x40 head tag, 0x41/0x42/0x43 record tags, 16-byte source digest and the
//            12 zero bytes after it.
//   unknown, kept raw: the whole fixed-function state blob (no word's meaning is measured), the input layout's format
//            code, input slot, aligned byte offset, semantic index and per-vertex flag, and the stage blob's head
//            words and 0x41/0x42 record bodies. They stay unknown on purpose: a guessed format or a guessed state
//            produces a wrong PSO, which is far harder to find than a refusal.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rt::graphics_record {

// A non-owning byte range. decode() copies these out of the record but never dereferences them: the pointer comes from
// a deserialized console record, so readability of the pointed-to bytes is the caller's guarantee, not this file's.
struct Span {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// Stage slots, in record order: slot 0 pixel, slot 1 vertex, slot 2 mesh (docs/shader_library.md section 2).
inline constexpr int kStageSlotPs = 0;
inline constexpr int kStageSlotVs = 1;
inline constexpr int kStageSlotMs = 2;
inline constexpr std::size_t kStageSlotCount = 3;

// "ps", "vs", "ms", or "unused" for a slot number.
const char* stage_slot_name(int slot);

// The measured shape of the record. All numbers are offsets into the record's own bytes or measured size ranges;
// they are checks, not decoded values.
inline constexpr std::size_t kRootSignatureOffset = 0x00;
inline constexpr std::size_t kStatePairOffset = 0x08;      // pointer at +0x08, size at +0x10
inline constexpr std::size_t kInputLayoutPairOffset = 0x18;  // pointer at +0x18, size at +0x20
inline constexpr std::size_t kStagePairOffset = 0x28;      // slot 0 program pair; +0x10 per pair; +0x20 per slot
inline constexpr std::size_t kProgramToBlobPairOffset = 0x10;  // a slot's stage-blob pair follows its program pair
inline constexpr std::size_t kStagePairStride = 0x20;
inline constexpr std::size_t kPairPointerBytes = 0x08;
inline constexpr std::size_t kDescriptorValidatedBytes = 0x108;  // zero-filled region ends here; tail is unknown
inline constexpr std::size_t kDescriptorTailOffset = kDescriptorValidatedBytes;

inline constexpr std::size_t kStateMinBytes = 380;  // docs/pso_cache.md, "stage record (sizes 380..520)"
inline constexpr std::size_t kStateMaxBytes = 520;
inline constexpr std::size_t kInputLayoutMinBytes = 80;   // docs/shader_library.md: a mesh pipeline's layout is 80 B
inline constexpr std::size_t kInputLayoutMaxBytes = 340;

// Library programs, docs/shader_library.md section 2: "52 to 239,464 bytes".
inline constexpr std::size_t kProgramMinBytes = 52;
inline constexpr std::size_t kProgramMaxBytes = 239464;

// Kind-2 stage blob envelope (docs/shader_library.md section 3; tools/shader_library.py stage_digest).
inline constexpr std::uint32_t kStageBlobKind = 2;
inline constexpr std::size_t kStageBlobHeadBytes = 0x10;    // kind word + size word + 0x08 word + 0x40 tag
inline constexpr std::size_t kStageBlobRecordBytes = 32;
inline constexpr std::size_t kStageBlobMinBytes = kStageBlobHeadBytes + kStageBlobRecordBytes;
inline constexpr std::uint32_t kStageBlobHeadTag = 0x40;    // word at head-4, checked by tools/shader_library.py
inline constexpr std::uint32_t kStageBlobSizeBias = 12;     // size word + this == blob size (measured; meaning open)
inline constexpr std::size_t kStageBlobDigestBytes = 16;
inline constexpr std::size_t kStageBlobTrailingZeroBytes = 12;
inline constexpr std::uint32_t kStageBlobRecordTagA = 0x41;
inline constexpr std::uint32_t kStageBlobRecordTagB = 0x42;
inline constexpr std::uint32_t kStageBlobDigestTag = 0x43;

// Input layout (docs/boot_frontier.md, slot-57 section: element count at +0x4C, NUL-terminated semantics inline).
inline constexpr std::size_t kInputLayoutHeaderBytes = 0x50;
inline constexpr std::size_t kInputLayoutCountOffset = 0x4C;
inline constexpr std::size_t kInputLayoutNamesOffset = 0x50;
inline constexpr std::size_t kInputLayoutAttributeBytes = 28;

// ---- The decoded record ------------------------------------------------------------------------------------------

struct Stage {
    int slot = kStageSlotPs;
    Span program;     // native RDNA2 program; the shader index keys it by SHA-256 of these bytes as passed
    Span stage_blob;  // kind-2 stage blob; decode_stage_blob() validates its contents
};

struct Record {
    void* root_signature = nullptr;  // +0x00, the pointer value as passed; not validated and not dereferenced
    Span state;                      // +0x08/+0x10: fixed-function state blob (decode_state)
    Span input_layout;               // +0x18/+0x20: input-layout blob (decode_input_layout)
    Stage stages[kStageSlotCount];
    std::size_t stage_count = 0;
};

// Parses the record's own bytes. `available` must cover kDescriptorValidatedBytes; the record's tail beyond that is
// ignored. Never dereferences `root_signature`, the blob pointers or the program pointers: the blob decoders take the
// spans this fills in. Returns false on any inconsistency (short read, a size outside the measured range, a half-set
// pair, a non-zero unused pair, a non-zero byte where the measurement says zero, zero stages, or vertex and mesh
// stages together).
bool decode(const void* descriptor, std::size_t available, Record* out);

// ---- The input layout -------------------------------------------------------------------------------------------

// One input-layout element. Only `semantic` and the raw 28 attribute bytes are measured; every other member stays
// unset rather than guessed, because a wrong format or offset is a wrong pipeline that is hard to find later.
struct Element {
    std::string semantic;  // decoded: NUL-terminated string stored inline, in element order
    std::optional<std::uint32_t> semantic_index;       // unknown
    std::optional<std::uint32_t> format;               // unknown: the observed codes 6/28/16 are not adopted as DXGI
    std::optional<std::uint32_t> input_slot;           // unknown
    std::optional<std::uint32_t> aligned_byte_offset;  // unknown
    std::optional<bool> per_vertex_data;               // unknown (true per-vertex, false per-instance)
    // The measured 28 attribute bytes for this element, kept verbatim. Their field encoding is not decoded, so no
    // member above is filled from them.
    std::array<std::uint8_t, kInputLayoutAttributeBytes> attribute{};
};

// Decodes the input-layout blob: element count at +0x4C, `count` NUL-terminated printable semantics from +0x50, then
// exactly count*28 attribute bytes. Returns false when the blob is shorter than its header, when a semantic is not a
// terminated printable run, when the count contradicts the remaining bytes, or when the size is outside 80..340.
bool decode_input_layout(Span blob, std::vector<Element>* out);

// ---- The fixed-function state blob ------------------------------------------------------------------------------

// The measured 380..520-byte state blob. No word's meaning is measured, so this struct decodes no field: it validates
// the size envelope and keeps every word verbatim under an explicit unknown marker. The converter must not infer
// rasterizer/blend/depth-stencil/sample/topology state from these words.
struct State {
    static constexpr std::size_t kMinBytes = kStateMinBytes;
    static constexpr std::size_t kMaxBytes = kStateMaxBytes;
    std::size_t size = 0;
    std::vector<std::uint32_t> raw;  // all words of the blob, unknown
};

// Validates the size envelope (380..520 bytes, a multiple of 4) and copies the words out. Returns false otherwise.
bool decode_state(Span blob, State* out);

// ---- The kind-2 stage blob --------------------------------------------------------------------------------------

// A 0x41/0x42 record: the tag is decoded, the seven body words are not (the field meanings in docs/pso_cache.md are
// unproven there and are not adopted here).
struct ResourceRecord {
    std::uint32_t tag = 0;       // decoded: 0x41 or 0x42
    std::uint32_t body[7] = {};  // unknown, kept verbatim
};

// The 16 bytes after the 0x43 tag: the source container's own digest (docs/shader_library.md section 3). Kept as
// opaque identity bytes; this decoder does not verify it against any container.
struct SourceDigest {
    std::array<std::uint8_t, kStageBlobDigestBytes> bytes{};
};

struct StageBlob {
    std::uint32_t kind = 0;               // decoded: 2
    std::uint32_t declared_size_word = 0; // raw +0x04 word; decoded relation: + 12 == blob size
    std::uint32_t head_word_08 = 0;       // unknown, kept verbatim
    std::uint32_t head_tag = 0;           // decoded: +0x0C word == 0x40
    std::vector<ResourceRecord> records;  // 0x41 records then 0x42 records, in stored order
    SourceDigest source_digest;           // decoded: the 16 bytes after the 0x43 tag
};

// Decodes the kind-2 stage blob envelope: kind word 2, the measured size relation, the 0x40 head tag, 0x41 then 0x42
// records, a final 0x43 record whose digest is kept and whose 12 trailing bytes must be zero. Record bodies stay
// unknown. Returns false for any other kind, an unknown tag, a broken tag order, a size that disagrees with the size
// word, or a non-zero trailing byte.
bool decode_stage_blob(Span blob, StageBlob* out);

}  // namespace rt::graphics_record
