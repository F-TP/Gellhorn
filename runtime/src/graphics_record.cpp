#include "graphics_record.h"

#include <cstring>
#include <utility>

namespace rt::graphics_record {
namespace {

std::uint32_t load_u32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof value);
    return value;
}

std::uint64_t load_u64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof value);
    return value;
}

const std::uint8_t* pointer_from(std::uint64_t value) {
    return reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(value));
}

bool in_range(std::uint64_t value, std::size_t low, std::size_t high) {
    return value >= low && value <= high;
}

struct Pair {
    std::uint64_t pointer = 0;
    std::uint64_t size = 0;
};

Pair read_pair(const std::uint8_t* record, std::size_t at) {
    return Pair{load_u64(record + at), load_u64(record + at + kPairPointerBytes)};
}

Span span_of(Pair pair) {
    return Span{pointer_from(pair.pointer), static_cast<std::size_t>(pair.size)};
}

bool pair_is_empty(Pair pair) {
    return (pair.pointer | pair.size) == 0;
}

}  // namespace

const char* stage_slot_name(int slot) {
    switch (slot) {
        case kStageSlotPs: return "ps";
        case kStageSlotVs: return "vs";
        case kStageSlotMs: return "ms";
        default: return "unused";
    }
}

bool decode(const void* descriptor, std::size_t available, Record* out) {
    if (!out) return false;
    if (!descriptor) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(descriptor);
    if (available < kDescriptorValidatedBytes) return false;

    Record record;
    // +0x00: the root signature pointer. Kept as the pointer value that was passed; nothing dereferences or validates
    // it, and a zero here is not interpreted (the measurements do not prove a null root signature impossible).
    record.root_signature =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(load_u64(bytes + kRootSignatureOffset)));

    // The two measured blobs the record always carries. Size ranges are the measured ones: a record outside them is
    // not the shape this decoder understands, so it is refused rather than passed on with a size it was not given.
    const Pair state = read_pair(bytes, kStatePairOffset);
    const Pair layout = read_pair(bytes, kInputLayoutPairOffset);
    if (state.pointer == 0 || state.size == 0 || !in_range(state.size, kStateMinBytes, kStateMaxBytes)) return false;
    if (state.size % 4 != 0) return false;
    if (layout.pointer == 0 || layout.size == 0 || !in_range(layout.size, kInputLayoutMinBytes, kInputLayoutMaxBytes))
        return false;
    record.state = span_of(state);
    record.input_layout = span_of(layout);

    // The three slot pairs, in record order. A slot is absent when both of its pairs are zero, present when both are
    // fully set; anything in between is inconsistent. `stages` is filled in record order and each entry keeps the
    // record slot number, which is its stage identity.
    for (std::size_t slot = 0; slot < kStageSlotCount; ++slot) {
        const std::size_t at = kStagePairOffset + slot * kStagePairStride;
        const Pair program = read_pair(bytes, at);
        const Pair blob = read_pair(bytes, at + kProgramToBlobPairOffset);
        const bool program_empty = pair_is_empty(program);
        const bool blob_empty = pair_is_empty(blob);
        if (program_empty && blob_empty) continue;  // slot unused
        if (program_empty || blob_empty) return false;
        if (program.pointer == 0 || program.size == 0) return false;
        if (!in_range(program.size, kProgramMinBytes, kProgramMaxBytes)) return false;
        if (blob.pointer == 0 || blob.size == 0) return false;
        // The stage blob's measured envelope: 16 head bytes plus whole 32-byte records, one of them the 0x43 digest
        // record, so its size is 0x10 + 32*k and at least 0x30. A size that does not fit is refused here, before any
        // byte of the blob is read.
        if (blob.size < kStageBlobMinBytes) return false;
        if ((blob.size - kStageBlobHeadBytes) % kStageBlobRecordBytes != 0) return false;
        Stage& stage = record.stages[record.stage_count];
        stage.slot = static_cast<int>(slot);
        stage.program = span_of(program);
        stage.stage_blob = span_of(blob);
        ++record.stage_count;
    }
    if (record.stage_count == 0) return false;

    // Measured stage combinations of a library graphics record (docs/shader_library.md section 2: VS+PS, MS+PS, MS
    // only, VS only): no record has a vertex stage and a mesh stage together, and D3D12 forbids that pair as well.
    auto has_stage = [&record](int slot) {
        for (std::size_t i = 0; i < record.stage_count; ++i)
            if (record.stages[i].slot == slot) return true;
        return false;
    };
    if (has_stage(kStageSlotVs) && has_stage(kStageSlotMs)) return false;

    // Everything after the slot pairs is zero to +0x108 in every measured sample. A non-zero byte there is a shape
    // this decoder was not measured on; refuse instead of silently ignoring a field we do not know.
    for (std::size_t at = kStagePairOffset + kStageSlotCount * kStagePairStride; at < kDescriptorValidatedBytes; ++at)
        if (bytes[at] != 0) return false;

    *out = record;
    return true;
}

bool decode_input_layout(Span blob, std::vector<Element>* out) {
    if (!out) return false;
    out->clear();
    if (!blob.data) return false;
    if (blob.size < kInputLayoutMinBytes || blob.size > kInputLayoutMaxBytes) return false;

    const std::uint32_t count = load_u32(blob.data + kInputLayoutCountOffset);
    const std::size_t after_header = blob.size - kInputLayoutHeaderBytes;
    // Each element needs at least one name byte (its own NUL) and its 28 attribute bytes. This bounds `count` from
    // the bytes that are actually there before anything is allocated from it.
    if (count > after_header / (1 + kInputLayoutAttributeBytes)) return false;

    std::vector<Element> elements(count);
    std::size_t position = kInputLayoutNamesOffset;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t start = position;
        while (position < blob.size && blob.data[position] != 0) {
            const std::uint8_t byte = blob.data[position];
            // A semantic name is a printable NUL-terminated run in the measured samples. A binary byte here means the
            // element boundary this decoder assumes does not hold for this blob, so it refuses.
            if (byte < 0x20 || byte > 0x7e) return false;
            ++position;
        }
        if (position >= blob.size) return false;  // unterminated name
        elements[index].semantic.assign(reinterpret_cast<const char*>(blob.data + start), position - start);
        ++position;  // the NUL terminator
    }

    // The measured blobs hold all names inline first, then 28 attribute bytes per element; the two blocks together
    // must fill the blob exactly. If they do not, the element boundary is not the measured one and nothing is
    // returned, rather than pairing names with bytes whose encoding is not known.
    const std::size_t attribute_bytes = static_cast<std::size_t>(count) * kInputLayoutAttributeBytes;
    if (position > blob.size || blob.size - position != attribute_bytes) return false;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::memcpy(elements[index].attribute.data(),
                    blob.data + position + static_cast<std::size_t>(index) * kInputLayoutAttributeBytes,
                    kInputLayoutAttributeBytes);
    }

    *out = std::move(elements);
    return true;
}

bool decode_state(Span blob, State* out) {
    if (!out) return false;
    *out = State{};
    if (!blob.data) return false;
    if (blob.size < kStateMinBytes || blob.size > kStateMaxBytes) return false;
    if (blob.size % 4 != 0) return false;  // the blob is read as 32-bit words in the measurements

    State state;
    state.size = blob.size;
    state.raw.resize(blob.size / 4);
    std::memcpy(state.raw.data(), blob.data, blob.size);
    *out = std::move(state);
    return true;
}

bool decode_stage_blob(Span blob, StageBlob* out) {
    if (!out) return false;
    *out = StageBlob{};
    if (!blob.data) return false;
    if (blob.size < kStageBlobMinBytes) return false;
    if ((blob.size - kStageBlobHeadBytes) % kStageBlobRecordBytes != 0) return false;

    const std::uint32_t kind = load_u32(blob.data);
    if (kind != kStageBlobKind) return false;  // kind 3 is the compute layout blob, not this record's stage blob
    const std::uint32_t declared_size = load_u32(blob.data + 4);
    // Measured relation: the word at +0x04 plus 12 is the blob's own size. The 12 is measured, not explained, so it is
    // used only as a consistency check.
    if (static_cast<std::uint64_t>(declared_size) + kStageBlobSizeBias != blob.size) return false;
    const std::uint32_t head_tag = load_u32(blob.data + kStageBlobHeadBytes - 4);
    if (head_tag != kStageBlobHeadTag) return false;

    StageBlob stage;
    stage.kind = kind;
    stage.declared_size_word = declared_size;
    stage.head_word_08 = load_u32(blob.data + 8);  // unknown; kept verbatim
    stage.head_tag = head_tag;

    const std::size_t record_count = (blob.size - kStageBlobHeadBytes) / kStageBlobRecordBytes;
    bool second_kind_seen = false;
    bool digest_seen = false;
    for (std::size_t index = 0; index < record_count; ++index) {
        const std::size_t at = kStageBlobHeadBytes + index * kStageBlobRecordBytes;
        const std::uint32_t tag = load_u32(blob.data + at);
        if (tag == kStageBlobRecordTagA) {
            if (second_kind_seen) return false;  // measured order: all 0x41, then all 0x42, then one 0x43
        } else if (tag == kStageBlobRecordTagB) {
            second_kind_seen = true;
        } else if (tag == kStageBlobDigestTag) {
            // The digest record is the last one and is 32 bytes: tag, 16 digest bytes, 12 zero bytes.
            if (index + 1 != record_count) return false;
            std::memcpy(stage.source_digest.bytes.data(), blob.data + at + 4, kStageBlobDigestBytes);
            for (std::size_t zero = at + 4 + kStageBlobDigestBytes; zero < at + kStageBlobRecordBytes; ++zero)
                if (blob.data[zero] != 0) return false;
            digest_seen = true;
            continue;
        } else {
            return false;  // a tag no measurement covers: refuse rather than guess what follows
        }
        ResourceRecord record;
        record.tag = tag;
        for (std::size_t word = 0; word < 7; ++word) record.body[word] = load_u32(blob.data + at + 4 + 4 * word);
        stage.records.push_back(record);
    }
    if (!digest_seen) return false;

    *out = std::move(stage);
    return true;
}

}  // namespace rt::graphics_record
