#include "streamselftest.hh"

#ifdef PSXSPLASH_STREAM_SELFTEST

#include <psyqo/xprintf.h>

#include "streamreader.hh"

// Streams the current scene's splashpack back off the disc during gameplay,
// chunk after chunk, forever, and compares each chunk against a checksum taken
// from the copy loaded at scene start (before parsing touches it). Exercises the
// wake/read/silence cycle thousands of times under real game load.
//
// Output, once per full pass over the file:
//   STREAMTEST pass=N reads=R bad=B
// Any mismatch or read failure also prints its own line immediately.

namespace psxsplash {
namespace {

constexpr uint32_t kChunkSectors = 16;  // 32 KB
constexpr uint32_t kMaxChunks = 128;    // 4 MB of file, well past a splashpack

StreamFile s_file;
uint32_t s_crc[kMaxChunks];
uint32_t s_chunks = 0;
uint32_t s_next = 0;
uint32_t s_pass = 0;
uint32_t s_reads = 0;
uint32_t s_bad = 0;
uint8_t* s_buf = nullptr;

uint32_t crc32(const uint8_t* p, uint32_t len) {
    uint32_t c = 0xffffffff;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320 & -(c & 1));
    }
    return ~c;
}

uint32_t chunkBytes(uint32_t i) {
    uint32_t off = i * kChunkSectors * 2048;
    uint32_t len = kChunkSectors * 2048;
    return off + len > s_file.size ? s_file.size - off : len;
}

void issue() {
    uint32_t sectors = kChunkSectors;
    uint32_t first = s_next * kChunkSectors;
    if (first + sectors > s_file.sectorCount()) sectors = s_file.sectorCount() - first;
    StreamReader::Get().request(s_file, first, sectors, s_buf, [](bool ok) {
        uint32_t i = s_next;
        s_reads++;
        if (!ok) {
            s_bad++;
            printf("STREAMTEST read FAILED chunk=%u\n", i);
        } else if (crc32(s_buf, chunkBytes(i)) != s_crc[i]) {
            s_bad++;
            printf("STREAMTEST MISMATCH chunk=%u\n", i);
        }
        if (++s_next == s_chunks) {
            s_next = 0;
            s_pass++;
            printf("STREAMTEST pass=%u reads=%u bad=%u\n", s_pass, s_reads, s_bad);
        }
        issue();
    });
}

}  // namespace

void StreamSelfTest::Start(const char* filename, const uint8_t* data, int size) {
    if (!StreamReader::Open(filename, s_file) || s_file.size != static_cast<uint32_t>(size)) {
        printf("STREAMTEST open FAILED %s\n", filename);
        return;
    }
    s_chunks = (s_file.size + kChunkSectors * 2048 - 1) / (kChunkSectors * 2048);
    if (s_chunks > kMaxChunks) s_chunks = kMaxChunks;
    for (uint32_t i = 0; i < s_chunks; i++)
        s_crc[i] = crc32(data + i * kChunkSectors * 2048, chunkBytes(i));
    if (!s_buf) s_buf = new uint8_t[kChunkSectors * 2048];
    s_next = 0;
    StreamReader::Get().reserveDrive(true);
    printf("STREAMTEST start %s size=%u chunks=%u\n", filename, s_file.size, s_chunks);
    issue();
}

}  // namespace psxsplash

#endif  // PSXSPLASH_STREAM_SELFTEST
