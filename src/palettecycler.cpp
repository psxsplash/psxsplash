#include "palettecycler.hh"

#include <psyqo/fragments.hh>
#include <psyqo/xprintf.h>

namespace {

// The upload's head: chain entry, then the first three packet words (GP0(A0h) and its rectangle).
// The span's words and the GP0(01h) follow it in the bump allocator, which hands out contiguous
// 4-byte slots; palcycle::buildUploadPacket writes the whole packet. The user-provided
// constructors keep the allocator from zero-filling what is overwritten anyway.
struct ClutUpload : psyqo::Fragments::ChainEntry {
    ClutUpload() {}
    uint32_t packet[3];
    size_t getActualFragmentSize() const { return 3 + ((packet[2] & 0xffff) >> 1) + 1; }
};
static_assert(sizeof(ClutUpload) == 16, "ClutUpload is a chain entry plus three words");

struct RawWord {
    RawWord() {}
    uint32_t value;
};

}  // namespace

void psxsplash::PaletteCycler::addClut(const uint8_t* pack, uint32_t packSize, uint16_t index, uint32_t offset,
                                       uint16_t pad, uint16_t clutX16, uint16_t clutY, uint16_t length) {
    palcycle::Record rec;
    auto err = palcycle::parseRecord(pack, packSize, offset, pad, length, rec);
    if (err == palcycle::RecordError::NoRecord) return;
    if (err != palcycle::RecordError::Ok) {
        printf("palette cycling: CLUT %u record at %u ignored: %s\n", index, offset, palcycle::recordErrorName(err));
        return;
    }
    if (m_count >= kMaxCluts) {
        printf("palette cycling: CLUT %u ignored: more than %d cycled CLUTs\n", index, kMaxCluts);
        return;
    }
    Clut& c = m_cluts[m_count++];
    c.rec = rec;
    c.span = palcycle::uploadSpan(rec.ranges, rec.rangeCount, rec.length);
    c.x = int16_t(clutX16 * 16);
    c.y = int16_t(clutY);
    c.dirty = false;  // VRAM already holds step 0, uploaded from the .vram file
    for (int i = 0; i < palcycle::kMaxRanges; i++) {
        c.acc[i] = 0;
        c.step[i] = 0;
    }
}

void psxsplash::PaletteCycler::tick(int32_t dt12) {
    for (int n = 0; n < m_count; n++) {
        Clut& c = m_cluts[n];
        for (unsigned i = 0; i < c.rec.rangeCount; i++) {
            const auto& r = c.rec.ranges[i];
            if (palcycle::advance(c.acc[i], c.step[i], dt12, r.period, r.count)) c.dirty = true;
        }
    }
}

void psxsplash::PaletteCycler::emit(Renderer::OT& ot, Renderer::Balloc& balloc, int32_t farBucket) {
    for (int n = 0; n < m_count; n++) {
        Clut& c = m_cluts[n];
        if (!c.dirty) continue;
        const unsigned words = palcycle::uploadPacketWords(c.span);
        if (balloc.remaining() < 4 + words * 4) return;  // try again next frame
        c.dirty = false;
        auto& head = balloc.allocate<ClutUpload>();
        for (unsigned i = 3; i < words; i++) balloc.allocate<RawWord>();
        // Ends with GP0(01h): without it real hardware keeps drawing with the cached old CLUT
        // (measured; see palettecycle.hh).
        palcycle::buildUploadPacket(reinterpret_cast<uint32_t*>(&head) + 1, c.rec, c.step, c.span, c.x, c.y);
        ot.insert(head, farBucket);
    }
}
