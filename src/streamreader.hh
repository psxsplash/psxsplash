#pragma once

#include <stdint.h>

#include <EASTL/functional.h>

namespace psxsplash {

/**
 * A file opened for sector-granular reads during gameplay.
 *
 * Opened once while the scene is loading (directory lookup is a blocking
 * read), then read from at any time without touching the filesystem again.
 */
struct StreamFile {
    uint32_t lba = 0;       // CD-ROM: first sector of the file
    uint32_t size = 0;      // bytes
    char name[32] = {};     // PCdrv: host-side filename
    bool valid = false;

    uint32_t sectorCount() const { return (size + 2047) / 2048; }
};

/**
 * StreamReader - asynchronous sector reads while the game is running.
 *
 * Requests are queued and serviced one at a time from update(), which the
 * scene calls once per frame. Each request's callback runs on the main loop,
 * on a later frame than the request() that queued it, never from an IRQ.
 *
 * On CD-ROM the drive is silenced during gameplay (see CDRomHelper), so each
 * read wakes it, issues an async psyqo readSectors, and silences it again in
 * the completion callback. Streaming and CD-DA cannot share the drive: a scene
 * that streams calls reserveDrive(true), after which MusicManager refuses
 * CD-DA for the rest of the scene. Reads still wait for an idle drive, in case
 * a track was already playing when the reservation was made.
 *
 * On PCdrv the read is synchronous inside update() and the callback is still
 * deferred, so game code sees the same ordering on both backends.
 */
class StreamReader {
  public:
    using Callback = eastl::function<void(bool success)>;

    static constexpr int kMaxQueued = 8;

    /** Look up a file for streaming. Blocking; call only while loading a scene. */
    static bool Open(const char* filename, StreamFile& out);

    /**
     * Queue a read of `sectorCount` 2048-byte sectors starting at `firstSector`
     * (relative to the start of the file) into `buffer`, which must be 4-byte
     * aligned and hold sectorCount * 2048 bytes until the callback runs.
     * Returns false if the queue is full or the range is outside the file.
     */
    bool request(const StreamFile& file, uint32_t firstSector, uint32_t sectorCount,
                 void* buffer, Callback&& callback);

    /** Service the queue. Call once per frame. */
    void update();

    /** True while a read is in flight or queued. */
    bool busy() const { return m_inFlight || m_count > 0; }
    bool readInFlight() const { return m_inFlight; }

    /**
     * Drop every queued request without running its callback, then pump until
     * the in-flight read (if any) has completed and its callback has run.
     * Called before a scene transition, while the scene that owns the buffers
     * is still alive. Releases the drive reservation.
     */
    void drain();

    /** Claim the drive for streaming until the scene ends (drain() releases it). */
    void reserveDrive(bool reserved) { m_reserved = reserved; }
    bool driveReserved() const { return m_reserved; }

    static StreamReader& Get();

  private:
    struct Request {
        StreamFile file;
        uint32_t firstSector;
        uint32_t sectorCount;
        void* buffer;
        Callback callback;
    };

    bool startRead(Request& req);

    Request m_queue[kMaxQueued];
    int m_head = 0;
    int m_count = 0;
    bool m_inFlight = false;
    bool m_reserved = false;
};

}  // namespace psxsplash
