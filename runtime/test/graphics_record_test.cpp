// Synthetic blobs for runtime/src/graphics_record.*: the slot-57 record's six pointer/size pairs, its fixed-function
// state blob, its input-layout blob and two kind-2 stage blobs, all built byte by byte here. Nothing in this file is
// game data: the state blob's first words are the raw words the measurements quote (docs/graphics_record.md), and
// every other byte is a pattern this test chose.
//
// Covered: a PS+VS record that decodes to the expected spans/offsets; the decoder promises not to read through the
// record's pointers (a record full of fake pointers still decodes); an 80-byte mesh input layout with no elements;
// every measured bound (380..520 B state, 80..340 B layout, 52..239,464 B program, 0x10 + 32*k stage blob) on both
// sides; and the malformed cases: a short descriptor, zero stages, half-set pairs, a non-zero reserved byte, a vertex
// and mesh stage together, an unterminated or unprintable semantic, a count that contradicts the bytes, a stage blob
// whose kind, size word, 0x40 head tag, tag order, final 0x43 record or 12 zero bytes disagree. It prints the
// decoded/unknown field table so the boundary is visible without reading the header.

#include "../src/graphics_record.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using rt::graphics_record::Element;
using rt::graphics_record::Record;
using rt::graphics_record::Span;
using rt::graphics_record::StageBlob;
using rt::graphics_record::State;

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

std::uint64_t pointer_word(const Bytes& blob) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(blob.data()));
}

// A pointer/size pair as the record stores it: pointer at `at`, size at `at + 8`.
void put_span_pair(Bytes* bytes, std::size_t at, const Bytes& blob) {
    put_u64(bytes, at, pointer_word(blob));
    put_u64(bytes, at + 8, blob.size());
}

// The measured 388-byte state sample: +0x00..+0x14 words, the 0x40 word at +0x150, four trailing zero bytes.
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

// The measured 188-byte shape: count 3 at +0x4C, three NUL-terminated semantics from +0x50 ("POSITION", "COLOR",
// "TEXCOORD", 24 bytes), then 28 attribute bytes per element. The candidate format codes 6/28/16 and the 0x1C word
// are placed in the attribute blocks only to show the decoder ignores them: their field encoding is not confirmed.
Bytes make_input_layout(std::uint32_t count = 3) {
    Bytes blob(188, 0);
    put_u32(&blob, 0x4c, count);
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

// Synthetic native-program bytes (not a shader): a repeating pattern of the requested size.
Bytes make_program(std::size_t size, std::uint8_t seed) {
    Bytes blob(size);
    for (std::size_t i = 0; i < size; ++i) blob[i] = static_cast<std::uint8_t>((i * 37 + seed) & 0xff);
    return blob;
}

// A kind-2 stage blob: head (kind 2, size word, unknown word, 0x40 tag) and 32-byte records. `tags` must end with the
// 0x43 digest record; the digest bytes are a pattern this test chose.
Bytes make_stage_blob(const std::vector<std::uint32_t>& tags, std::uint8_t digest_seed) {
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
                blob[at + 4 + byte] = static_cast<std::uint8_t>((0xa0 + digest_seed + byte) & 0xff);
        } else {
            for (std::size_t word = 0; word < 7; ++word)
                put_u32(&blob, at + 4 + 4 * word, 0x11000000u | (static_cast<std::uint32_t>(index) << 8) |
                                                       static_cast<std::uint32_t>(word));
        }
    }
    return blob;
}

struct Fixture {
    Bytes state = make_state_blob();
    Bytes layout = make_input_layout();
    Bytes ps_program = make_program(96, 0x11);
    Bytes ps_blob = make_stage_blob({0x41, 0x42, 0x43}, 1);
    Bytes vs_program = make_program(52, 0x22);
    Bytes vs_blob = make_stage_blob({0x41, 0x43}, 2);
    Bytes descriptor = make_descriptor();

    // 0x108 validated bytes plus an unidentified tail that decode() must ignore.
    Bytes make_descriptor() const {
        Bytes desc(0x120, 0);
        put_u64(&desc, 0x00, 0x1122334455667788ull);  // a pointer value; nothing dereferences it
        put_span_pair(&desc, 0x08, state);
        put_span_pair(&desc, 0x18, layout);
        put_span_pair(&desc, 0x28, ps_program);
        put_span_pair(&desc, 0x38, ps_blob);
        put_span_pair(&desc, 0x48, vs_program);
        put_span_pair(&desc, 0x58, vs_blob);
        for (std::size_t at = 0x108; at < desc.size(); ++at) desc[at] = 0xee;  // the unknown tail
        return desc;
    }
};

// ----------------------------------------------------------------------------------------------------------------

void test_decode_success() {
    const Fixture fixture;
    Record record;
    check(rt::graphics_record::decode(fixture.descriptor.data(), fixture.descriptor.size(), &record),
          "decode: the synthetic PS+VS record decodes");
    check(record.stage_count == 2, "decode: two stages are present");
    check(record.stages[0].slot == rt::graphics_record::kStageSlotPs, "decode: slot 0 is the pixel shader");
    check(record.stages[1].slot == rt::graphics_record::kStageSlotVs, "decode: slot 1 is the vertex shader");
    check(record.stages[2].program.size == 0 && record.stages[2].stage_blob.size == 0,
          "decode: slot 2 (mesh) stays empty");
    check(record.root_signature == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1122334455667788ull)),
          "decode: the root signature pointer value is copied at +0x00");
    check(record.state.data == fixture.state.data() && record.state.size == fixture.state.size(),
          "decode: the state span is +0x08/+0x10");
    check(record.input_layout.data == fixture.layout.data() && record.input_layout.size == fixture.layout.size(),
          "decode: the input-layout span is +0x18/+0x20");
    check(record.stages[0].program.data == fixture.ps_program.data() &&
              record.stages[0].program.size == fixture.ps_program.size(),
          "decode: the PS program span is +0x28/+0x30");
    check(record.stages[0].stage_blob.data == fixture.ps_blob.data() &&
              record.stages[0].stage_blob.size == fixture.ps_blob.size(),
          "decode: the PS stage blob span is +0x38/+0x40");
    check(record.stages[1].program.data == fixture.vs_program.data() &&
              record.stages[1].program.size == fixture.vs_program.size(),
          "decode: the VS program span is +0x48/+0x50");
    check(record.stages[1].stage_blob.data == fixture.vs_blob.data() &&
              record.stages[1].stage_blob.size == fixture.vs_blob.size(),
          "decode: the VS stage blob span is +0x58/+0x60");

    // The unidentified tail beyond +0x108 is ignored, so exactly the validated region is enough.
    check(rt::graphics_record::decode(fixture.descriptor.data(), rt::graphics_record::kDescriptorValidatedBytes,
                                      &record),
          "decode: a +0x108-byte read is enough; the tail is not required");
}

void test_decode_does_not_dereference() {
    // Every blob pair gets a pointer value that is not mapped in this process. decode() only copies the pairs out; if
    // it ever reads through one of them this test faults instead of the shipped decoder crashing on a bad record.
    const Fixture fixture;
    Bytes desc = fixture.descriptor;
    const std::uint64_t fake_pointers[6] = {0x10000, 0x20000, 0x30000, 0x40000, 0x50000, 0x60000};
    const std::size_t pair_offsets[6] = {0x08, 0x18, 0x28, 0x38, 0x48, 0x58};
    for (std::size_t pair = 0; pair < 6; ++pair) put_u64(&desc, pair_offsets[pair], fake_pointers[pair]);
    Record record;
    check(rt::graphics_record::decode(desc.data(), desc.size(), &record),
          "decode: a record of fake pointers decodes without being dereferenced");
    check(record.state.data == reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(0x10000)),
          "decode: the fake state pointer is copied verbatim");
}

void test_empty_input_layout() {
    // docs/shader_library.md: a mesh pipeline's input layout is 80 bytes with no elements.
    Bytes layout(80, 0);
    std::vector<Element> elements;
    check(rt::graphics_record::decode_input_layout(Span{layout.data(), layout.size()}, &elements),
          "layout: an 80-byte mesh layout decodes");
    check(elements.empty(), "layout: the mesh layout has no elements");

    const Fixture fixture;
    Bytes desc = fixture.descriptor;
    put_span_pair(&desc, 0x18, layout);
    Record record;
    check(rt::graphics_record::decode(desc.data(), desc.size(), &record), "decode: an 80-byte input layout is accepted");
    check(record.input_layout.size == 80, "decode: the mesh layout's size is kept");
}

void test_state() {
    const Fixture fixture;
    State state;
    check(rt::graphics_record::decode_state(Span{fixture.state.data(), fixture.state.size()}, &state),
          "state: the 388-byte sample decodes");
    check(state.size == 388, "state: the size is kept");
    check(state.raw.size() == 388 / 4, "state: every word is kept");
    const std::uint32_t measured[5] = {0x4005d808u, 0x00000004u, 0x00100060u, 0x00020042u, 0xb50516a0u};
    bool measured_kept = true;
    for (std::size_t word = 0; word < 5; ++word) measured_kept = measured_kept && state.raw[word] == measured[word];
    check(measured_kept, "state: the measured sample words are kept verbatim");
    check(state.raw[0x150 / 4] == 0x40, "state: the 0x40 word at +0x150 is kept raw, not interpreted");
    check(state.raw.back() == 0 && state.raw[state.raw.size() - 2] == 0,
          "state: the trailing zero bytes are kept as words");

    // Measured envelope: 380..520 bytes and a whole number of words.
    const std::size_t accepted[3] = {380, 388, 520};
    for (const std::size_t size : accepted) {
        Bytes blob(size, 0);
        State boundary;
        check(rt::graphics_record::decode_state(Span{blob.data(), blob.size()}, &boundary),
              "state: a measured-size blob is accepted");
        check(boundary.size == size, "state: the accepted size is kept");
    }
    const std::size_t rejected[3] = {376, 379, 524};
    for (const std::size_t size : rejected) {
        Bytes blob(size, 0);
        check(!rt::graphics_record::decode_state(Span{blob.data(), blob.size()}, &state),
              "state: a size outside 380..520 is refused");
    }
    Bytes not_words(382, 0);
    check(!rt::graphics_record::decode_state(Span{not_words.data(), not_words.size()}, &state),
          "state: a size that is not a whole number of words is refused");
    check(!rt::graphics_record::decode_state(Span{nullptr, 388}, &state), "state: a null blob is refused");
    check(!rt::graphics_record::decode_state(Span{fixture.state.data(), fixture.state.size()}, nullptr),
          "state: a null output is refused");
}

void test_input_layout() {
    const Fixture fixture;
    std::vector<Element> elements;
    check(rt::graphics_record::decode_input_layout(Span{fixture.layout.data(), fixture.layout.size()}, &elements),
          "layout: the synthetic 188-byte layout decodes");
    check(elements.size() == 3, "layout: three elements are decoded");
    check(elements[0].semantic == "POSITION" && elements[1].semantic == "COLOR" && elements[2].semantic == "TEXCOORD",
          "layout: the NUL-terminated semantics are decoded in order");
    bool all_unknown = true;
    const std::uint32_t candidates[3] = {6, 28, 16};
    for (std::size_t index = 0; index < elements.size(); ++index) {
        const Element& element = elements[index];
        all_unknown = all_unknown && !element.semantic_index && !element.format && !element.input_slot &&
                      !element.aligned_byte_offset && !element.per_vertex_data;
        // The candidate codes sit in the raw bytes of this synthetic blob; they must not come back as decoded fields.
        all_unknown = all_unknown && element.attribute[4] == candidates[index];
    }
    check(all_unknown, "layout: format/slot/offset/class/index stay unknown, raw attribute bytes are kept");
    for (std::size_t element = 0; element < elements.size(); ++element) {
        check(std::memcmp(elements[element].attribute.data(), fixture.layout.data() + 0x68 + element * 28, 28) == 0,
              "layout: the 28 attribute bytes are kept verbatim");
    }

    const std::size_t rejected_sizes[2] = {79, 341};
    for (const std::size_t size : rejected_sizes) {
        Bytes blob(size, 0);
        check(!rt::graphics_record::decode_input_layout(Span{blob.data(), blob.size()}, &elements),
              "layout: a size outside 80..340 is refused");
    }

    // count 4 with only three names in the blob: the fourth parse runs into binary attribute bytes.
    Bytes four = fixture.layout;
    put_u32(&four, 0x4c, 4);
    check(!rt::graphics_record::decode_input_layout(Span{four.data(), four.size()}, &elements),
          "layout: a count with no matching names is refused");

    // count 2 with three names: the leftover bytes do not equal two attribute blocks.
    Bytes two = fixture.layout;
    put_u32(&two, 0x4c, 2);
    check(!rt::graphics_record::decode_input_layout(Span{two.data(), two.size()}, &elements),
          "layout: a count that does not match the bytes is refused");

    // An unterminated last name.
    Bytes unterminated = fixture.layout;
    unterminated[0x67] = 'X';  // the NUL after TEXCOORD
    check(!rt::graphics_record::decode_input_layout(Span{unterminated.data(), unterminated.size()}, &elements),
          "layout: an unterminated semantic is refused");

    // An unprintable byte inside a name.
    Bytes binary_name = fixture.layout;
    binary_name[0x51] = 0x01;
    check(!rt::graphics_record::decode_input_layout(Span{binary_name.data(), binary_name.size()}, &elements),
          "layout: an unprintable semantic byte is refused");

    // One byte more than the measured shape.
    Bytes longer = fixture.layout;
    longer.push_back(0);
    check(!rt::graphics_record::decode_input_layout(Span{longer.data(), longer.size()}, &elements),
          "layout: trailing bytes that are not attribute blocks are refused");

    // count 0 with bytes after the header: those bytes have no element to belong to.
    Bytes stray(81, 0);
    check(!rt::graphics_record::decode_input_layout(Span{stray.data(), stray.size()}, &elements),
          "layout: a zero count with trailing bytes is refused");

    // A count that would need more name bytes than the blob has must be refused before it is used.
    Bytes huge = fixture.layout;
    put_u32(&huge, 0x4c, 0xffffffffu);
    check(!rt::graphics_record::decode_input_layout(Span{huge.data(), huge.size()}, &elements),
          "layout: an absurd element count is refused");

    check(!rt::graphics_record::decode_input_layout(Span{nullptr, 188}, &elements), "layout: a null blob is refused");
    check(!rt::graphics_record::decode_input_layout(Span{fixture.layout.data(), fixture.layout.size()}, nullptr),
          "layout: a null output is refused");
}

void test_stage_blob() {
    const Fixture fixture;
    StageBlob blob;
    check(rt::graphics_record::decode_stage_blob(Span{fixture.ps_blob.data(), fixture.ps_blob.size()}, &blob),
          "stage blob: the synthetic kind-2 blob decodes");
    check(blob.kind == 2, "stage blob: the kind word is 2");
    check(blob.head_tag == 0x40, "stage blob: the +0x0C head word is the measured 0x40 tag");
    check(blob.head_word_08 == 0x0000abcdu, "stage blob: the +0x08 head word is kept raw");
    check(blob.declared_size_word + rt::graphics_record::kStageBlobSizeBias == fixture.ps_blob.size(),
          "stage blob: the size word keeps the measured +12 relation");
    check(blob.records.size() == 2, "stage blob: the two 0x41/0x42 records are decoded");
    check(blob.records[0].tag == 0x41 && blob.records[1].tag == 0x42, "stage blob: record tags are kept in order");
    check(blob.records[0].body[0] == 0x11000000u && blob.records[1].body[6] == 0x11000106u,
          "stage blob: record bodies are kept verbatim");
    check(blob.source_digest.bytes[0] == 0xa1 && blob.source_digest.bytes[15] == 0xb0,
          "stage blob: the 16-byte source digest after 0x43 is kept");

    StageBlob vs;
    check(rt::graphics_record::decode_stage_blob(Span{fixture.vs_blob.data(), fixture.vs_blob.size()}, &vs),
          "stage blob: a single-record stage blob decodes");
    check(vs.records.size() == 1 && vs.records[0].tag == 0x41, "stage blob: the single 0x41 record is decoded");

    Bytes too_small(0x20, 0);
    put_u32(&too_small, 0x00, 2);
    check(!rt::graphics_record::decode_stage_blob(Span{too_small.data(), too_small.size()}, &blob),
          "stage blob: a blob shorter than head + one record is refused");

    Bytes misaligned = fixture.ps_blob;
    misaligned.push_back(0);
    put_u32(&misaligned, 0x04, static_cast<std::uint32_t>(misaligned.size() - 12));
    check(!rt::graphics_record::decode_stage_blob(Span{misaligned.data(), misaligned.size()}, &blob),
          "stage blob: a size that is not 0x10 + 32*k is refused");

    Bytes kind3 = fixture.ps_blob;
    put_u32(&kind3, 0x00, 3);
    check(!rt::graphics_record::decode_stage_blob(Span{kind3.data(), kind3.size()}, &blob),
          "stage blob: kind 3 (the compute layout blob) is refused");

    Bytes bad_size = fixture.ps_blob;
    put_u32(&bad_size, 0x04, static_cast<std::uint32_t>(bad_size.size() - 16));
    check(!rt::graphics_record::decode_stage_blob(Span{bad_size.data(), bad_size.size()}, &blob),
          "stage blob: a declared size that disagrees with the span is refused");

    Bytes bad_head = fixture.ps_blob;
    put_u32(&bad_head, 0x0c, 0x41);
    check(!rt::graphics_record::decode_stage_blob(Span{bad_head.data(), bad_head.size()}, &blob),
          "stage blob: a head word that is not the 0x40 tag is refused");

    const Bytes bad_order = make_stage_blob({0x42, 0x41, 0x43}, 3);
    check(!rt::graphics_record::decode_stage_blob(Span{bad_order.data(), bad_order.size()}, &blob),
          "stage blob: a 0x41 record after a 0x42 record is refused");

    const Bytes no_digest = make_stage_blob({0x41, 0x42}, 4);
    check(!rt::graphics_record::decode_stage_blob(Span{no_digest.data(), no_digest.size()}, &blob),
          "stage blob: a blob without the final 0x43 record is refused");

    const Bytes digest_not_last = make_stage_blob({0x43, 0x41}, 5);
    check(!rt::graphics_record::decode_stage_blob(Span{digest_not_last.data(), digest_not_last.size()}, &blob),
          "stage blob: a 0x43 record that is not last is refused");

    const Bytes unknown_tag = make_stage_blob({0x44, 0x43}, 6);
    check(!rt::graphics_record::decode_stage_blob(Span{unknown_tag.data(), unknown_tag.size()}, &blob),
          "stage blob: an unknown record tag is refused");

    Bytes dirty_tail = fixture.ps_blob;
    dirty_tail.back() = 0x01;
    check(!rt::graphics_record::decode_stage_blob(Span{dirty_tail.data(), dirty_tail.size()}, &blob),
          "stage blob: a non-zero byte in the final 12 zero bytes is refused");

    check(!rt::graphics_record::decode_stage_blob(Span{nullptr, 0x30}, &blob), "stage blob: a null blob is refused");
    check(!rt::graphics_record::decode_stage_blob(Span{fixture.ps_blob.data(), fixture.ps_blob.size()}, nullptr),
          "stage blob: a null output is refused");
}

void test_record_malformed() {
    const Fixture fixture;
    Record record;

    check(!rt::graphics_record::decode(nullptr, 0x120, &record), "decode: a null record is refused");
    check(!rt::graphics_record::decode(fixture.descriptor.data(), fixture.descriptor.size(), nullptr),
          "decode: a null output is refused");
    check(!rt::graphics_record::decode(fixture.descriptor.data(), 0x107, &record),
          "decode: a read shorter than +0x108 is refused");

    // No stage at all.
    Bytes no_stages = fixture.descriptor;
    for (std::size_t at = 0x28; at < 0x88; ++at) no_stages[at] = 0;
    check(!rt::graphics_record::decode(no_stages.data(), no_stages.size(), &record),
          "decode: a record without stages is refused");

    // The state span.
    Bytes bad_state = fixture.descriptor;
    put_u64(&bad_state, 0x10, 379);
    check(!rt::graphics_record::decode(bad_state.data(), bad_state.size(), &record),
          "decode: a state size below the measured range is refused");
    bad_state = fixture.descriptor;
    put_u64(&bad_state, 0x10, 521);
    check(!rt::graphics_record::decode(bad_state.data(), bad_state.size(), &record),
          "decode: a state size above the measured range is refused");
    bad_state = fixture.descriptor;
    put_u64(&bad_state, 0x08, 0);
    check(!rt::graphics_record::decode(bad_state.data(), bad_state.size(), &record),
          "decode: a null state pointer is refused");
    bad_state = fixture.descriptor;
    put_u64(&bad_state, 0x08, 0);
    put_u64(&bad_state, 0x10, 0);
    check(!rt::graphics_record::decode(bad_state.data(), bad_state.size(), &record),
          "decode: a missing state pair is refused");

    // The input-layout span.
    Bytes bad_layout = fixture.descriptor;
    put_u64(&bad_layout, 0x20, 79);
    check(!rt::graphics_record::decode(bad_layout.data(), bad_layout.size(), &record),
          "decode: an input-layout size below the measured range is refused");
    bad_layout = fixture.descriptor;
    put_u64(&bad_layout, 0x20, 341);
    check(!rt::graphics_record::decode(bad_layout.data(), bad_layout.size(), &record),
          "decode: an input-layout size above the measured range is refused");

    // Half-set stage pairs.
    Bytes half_pair = fixture.descriptor;
    put_u64(&half_pair, 0x30, 0);
    check(!rt::graphics_record::decode(half_pair.data(), half_pair.size(), &record),
          "decode: a program pointer without a size is refused");
    half_pair = fixture.descriptor;
    put_u64(&half_pair, 0x28, 0);
    check(!rt::graphics_record::decode(half_pair.data(), half_pair.size(), &record),
          "decode: a program size without a pointer is refused");
    half_pair = fixture.descriptor;
    put_u64(&half_pair, 0x40, 0);
    check(!rt::graphics_record::decode(half_pair.data(), half_pair.size(), &record),
          "decode: a stage-blob pointer without a size is refused");

    // Program and stage-blob sizes outside the measured shape.
    Bytes bad_program = fixture.descriptor;
    put_u64(&bad_program, 0x30, 51);
    check(!rt::graphics_record::decode(bad_program.data(), bad_program.size(), &record),
          "decode: a program size below 52 bytes is refused");
    bad_program = fixture.descriptor;
    put_u64(&bad_program, 0x30, 239465);
    check(!rt::graphics_record::decode(bad_program.data(), bad_program.size(), &record),
          "decode: a program size above 239,464 bytes is refused");
    Bytes bad_blob = fixture.descriptor;
    put_u64(&bad_blob, 0x40, 0x2f);
    check(!rt::graphics_record::decode(bad_blob.data(), bad_blob.size(), &record),
          "decode: a stage blob shorter than 0x30 is refused");
    bad_blob = fixture.descriptor;
    put_u64(&bad_blob, 0x40, 0x31);
    check(!rt::graphics_record::decode(bad_blob.data(), bad_blob.size(), &record),
          "decode: a stage blob that is not 0x10 + 32*k is refused");

    // The measured zero region after the slot pairs.
    Bytes dirty_tail = fixture.descriptor;
    dirty_tail[0x90] = 0x01;
    check(!rt::graphics_record::decode(dirty_tail.data(), dirty_tail.size(), &record),
          "decode: a non-zero byte in the +0x88..+0x108 reserved region is refused");

    // Measured stage combinations: no record has a vertex and a mesh stage together; mesh + pixel is measured.
    Bytes vs_and_ms = fixture.descriptor;
    put_span_pair(&vs_and_ms, 0x68, fixture.ps_program);  // slot 2 program
    put_span_pair(&vs_and_ms, 0x78, fixture.vs_blob);     // slot 2 stage blob
    check(!rt::graphics_record::decode(vs_and_ms.data(), vs_and_ms.size(), &record),
          "decode: a vertex and a mesh stage together are refused");

    Bytes ms_and_ps = vs_and_ms;
    for (std::size_t at = 0x48; at < 0x68; ++at) ms_and_ps[at] = 0;  // drop the VS slot
    check(rt::graphics_record::decode(ms_and_ps.data(), ms_and_ps.size(), &record),
          "decode: a pixel and a mesh stage pair is accepted");
    check(record.stage_count == 2 && record.stages[1].slot == rt::graphics_record::kStageSlotMs,
          "decode: the mesh stage is slot 2");

    // A pixel-only record is a subset the measurements do not forbid.
    Bytes ps_only = fixture.descriptor;
    for (std::size_t at = 0x48; at < 0x68; ++at) ps_only[at] = 0;
    check(rt::graphics_record::decode(ps_only.data(), ps_only.size(), &record),
          "decode: a pixel-only record is accepted");
    check(record.stage_count == 1, "decode: the pixel-only record has one stage");
}

// The decoded/unknown boundary, printed for the next reader.
void print_findings() {
    struct Finding {
        const char* field;
        const char* status;
        const char* evidence;
    };
    const Finding findings[] = {
        {"Record.root_signature", "decoded (opaque pointer)",
         "+0x00 pointer value; not validated and not dereferenced"},
        {"Record.state ptr/size", "decoded (range-checked)",
         "+0x08/+0x10; 380..520 B (docs/pso_cache.md); words kept raw"},
        {"Record.input_layout ptr/size", "decoded (range-checked)",
         "+0x18/+0x20; 80..340 B; a mesh layout is 80 B (docs/shader_library.md)"},
        {"Record.stages[i].slot", "decoded",
         "record order: 0 PS, 1 VS, 2 MS (docs/shader_library.md section 2)"},
        {"Record.stages[i].program", "decoded (span only)",
         "native program bytes; key = SHA-256 as passed (docs/boot_frontier.md)"},
        {"Record.stages[i].stage_blob", "decoded (span only)",
         "kind-2 blob envelope, contents via decode_stage_blob"},
        {"record +0x88..+0x108", "decoded (must be zero)",
         "every measured sample is zero to +0x108; a non-zero byte is refused"},
        {"record tail +0x108..", "unknown",
         "unidentified tail; not represented, ignored by decode()"},
        {"State.size", "decoded (range check)", "380..520 B, a whole number of 32-bit words"},
        {"State.raw words", "unknown (kept raw)",
         "the whole 388-byte sample, 0x40 word at +0x150 included; no meaning measured"},
        {"fixed-function state", "unknown",
         "rasterizer/blend/depth-stencil/sample/topology: raw words only"},
        {"InputLayout.element count", "decoded", "u32 at +0x4C (188-byte sample)"},
        {"InputLayout.semantic", "decoded", "NUL-terminated inline from +0x50, in element order"},
        {"InputLayout attribute block", "decoded (raw 28 bytes)", "28 bytes per element after the names, verbatim"},
        {"InputLayout format code", "unknown",
         "codes 6/28/16 observed; DXGI_FORMAT_R32G32B32_FLOAT / _R8G8B8A8_UNORM / _R32G32_FLOAT are candidates only"},
        {"InputLayout input slot", "unknown", "attribute field encoding not confirmed"},
        {"InputLayout aligned byte offset", "unknown", "attribute field encoding not confirmed"},
        {"InputLayout semantic index", "unknown", "attribute field encoding not confirmed"},
        {"InputLayout per-vertex flag", "unknown", "attribute field encoding not confirmed"},
        {"StageBlob.kind", "decoded", "u32 at +0x00 == 2 (docs/shader_library.md section 3)"},
        {"StageBlob.declared_size_word", "decoded (relation)", "u32 at +0x04 + 12 == blob size (measured)"},
        {"StageBlob.head_word_08", "unknown (kept raw)", "u32 at +0x08; not decoded"},
        {"StageBlob.head_tag", "decoded",
         "u32 at +0x0C == 0x40 (20,318 blobs; tools/shader_library.py stage_digest)"},
        {"StageBlob records", "decoded tags / unknown bodies",
         "0x41 then 0x42 32-byte records; seven body words kept raw"},
        {"StageBlob.source_digest", "decoded (opaque)",
         "16 bytes after the final 0x43 tag: the source container's digest"},
        {"StageBlob trailing 12 bytes", "decoded (must be zero)", "measured zero in all 20,318 blobs"},
    };
    std::printf("graphics_record_test: decoded/unknown boundary for the slot-57 record\n");
    std::printf("%-32s %-28s %s\n", "field", "status", "evidence");
    for (const Finding& finding : findings)
        std::printf("%-32s %-28s %s\n", finding.field, finding.status, finding.evidence);
    std::printf("\n");
}

}  // namespace

int main() {
    print_findings();

    test_decode_success();
    test_decode_does_not_dereference();
    test_empty_input_layout();
    test_state();
    test_input_layout();
    test_stage_blob();
    test_record_malformed();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s) of %d check(s)\n", g_failures, g_checks);
        return 1;
    }
    std::printf("graphics_record_test: all %d checks passed\n", g_checks);
    return 0;
}
