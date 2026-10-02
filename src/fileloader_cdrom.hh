#pragma once

#include "fileloader.hh"

#include <psyqo/cdrom-device.hh>
#include <psyqo/iso9660-parser.hh>
#include <psyqo/gpu.hh>
#include <psyqo/task.hh>

namespace psxsplash {

/**
 * FileLoaderCDRom - loads files from CD-ROM using the psyqo ISO9660 parser.
 *
 * Follows the same pattern as nugget/psyqo/examples/task-demo:
 *   1. CDRomDevice::prepare()          - called from Application::prepare()
 *   2. CDRomDevice::scheduleReset()    - reset the drive
 *   3. ISO9660Parser::scheduleInitialize() - parse the PVD and root dir
 *   4. ISO9660Parser::scheduleGetDirentry  - look up a file by path
 *   5. ISO9660Parser::scheduleReadRequest  - read the file sectors
 *
 */

class FileLoaderCDRom final : public FileLoader {
  public:
    FileLoaderCDRom() : m_isoParser(&m_cdrom) {}

    // -- prepare: must be called from Application::prepare() ------
    void prepare() override {
        m_cdrom.prepare();
    }

    // -- scheduleInit ---------------------------------------------
    // Chains: reset CD drive -> initialise ISO9660 parser.
    psyqo::TaskQueue::Task scheduleInit() override {
        m_initQueue
            .startWith(m_cdrom.scheduleReset())
            .then(m_isoParser.scheduleInitialize());
        return m_initQueue.schedule();
    }

    // -- scheduleLoadFile -----------------------------------------
    // Chains: getDirentry -> allocate + read.
    // The lambda captures filename/outBuffer/outSize by reference;
    // they must remain valid until the owning TaskQueue completes.
    psyqo::TaskQueue::Task scheduleLoadFile(
        const char* filename, uint8_t*& outBuffer, int& outSize) override
    {
        return psyqo::TaskQueue::Task(
            [this, filename, &outBuffer, &outSize](psyqo::TaskQueue::Task* task) {
                // Stash the parent task so callbacks can resolve/reject it.
                m_pendingTask = task;
                m_pOutBuffer = &outBuffer;
                m_pOutSize = &outSize;
                outBuffer = nullptr;
                outSize = 0;

                // Step 1 - look up the directory entry.
                m_isoParser.getDirentry(
                    filename, &m_request.entry,
                    [this](bool success) {
                        if (!success ||
                            m_request.entry.type ==
                                psyqo::ISO9660Parser::DirEntry::INVALID) {
                            m_pendingTask->reject();
                            return;
                        }

                        // Step 2 - allocate a sector-aligned buffer and read.
                        uint32_t sectors =
                            (m_request.entry.size + 2047) / 2048;
                        uint8_t* buf = new uint8_t[sectors * 2048];
                        *m_pOutBuffer = buf;
                        *m_pOutSize = static_cast<int>(m_request.entry.size);
                        m_request.buffer = buf;

                        // Step 3 - chain the actual CD read via a sub-queue.
                        m_readQueue
                            .startWith(
                                m_isoParser.scheduleReadRequest(&m_request))
                            .then([this](psyqo::TaskQueue::Task* inner) {
                                // Read complete - resolve the outer task.
                                m_pendingTask->resolve();
                                inner->resolve();
                            })
                            .butCatch([this](psyqo::TaskQueue*) {
                                // Read failed - clean up and reject.
                                delete[] *m_pOutBuffer;
                                *m_pOutBuffer = nullptr;
                                *m_pOutSize = 0;
                                m_pendingTask->reject();
                            })
                            .run();
                    });
            });
    }

    // -- LoadFileSyncWithProgress -------------------------------
    // Reads the file in 32-sector (64 KB) chunks, calling the
    // progress callback between each chunk so the loading bar
    // animates during the CD-ROM transfer.
    uint8_t* LoadFileSyncWithProgress(
        const char* filename, int& outSize,
        const LoadProgressInfo* progress) override
    {
        outSize = 0;
        if (!m_gpu) return nullptr;

        // Resolve the directory entry with BLOCKING reads.  The async
        // ISO9660Parser::getDirentry issues a pump-driven readSectors whenever
        // the path lives in a subdirectory (or a directory spanning >1 sector);
        // that async read, interleaved with the blocking file reads below,
        // leaves a CD IRQ in flight that lands in the next action's state
        // machine and aborts ("ReadSectorsAction got CDROM acknowledge in wrong
        // state").  resolveEntryBlocking keeps every CD access on the blocking
        // path, exactly like the file reads.
        uint32_t fileLBA = 0, fileSize = 0;
        if (!resolveEntryBlocking(filename, fileLBA, fileSize)) return nullptr;

        // --- chunked sector read with progress ---
        uint32_t totalSectors = (fileSize + 2047) / 2048;
        uint8_t* buf = new uint8_t[totalSectors * 2048];

        static constexpr uint32_t kChunkSectors = 32;  // 64 KB per chunk
        uint32_t sectorsRead = 0;

        while (sectorsRead < totalSectors) {
            uint32_t toRead = totalSectors - sectorsRead;
            if (toRead > kChunkSectors) toRead = kChunkSectors;

            bool ok = m_cdrom.readSectorsBlocking(
                fileLBA + sectorsRead, toRead,
                buf + sectorsRead * 2048, *m_gpu);
            if (!ok) {
                delete[] buf;
                return nullptr;
            }

            sectorsRead += toRead;

            if (progress && progress->fn) {
                uint8_t pct = progress->startPercent +
                    (uint8_t)((uint32_t)(progress->endPercent - progress->startPercent)
                              * sectorsRead / totalSectors);
                progress->fn(pct, progress->userData);
            }
        }

        outSize = static_cast<int>(fileSize);
        return buf;
    }

    // -- LoadFileSync ---------------------------------------------
    // Blocking fallback for code paths that can't use tasks (e.g.
    // SceneManager scene transitions).  Uses the blocking readSectors
    // variant which spins on GPU callbacks.
    uint8_t* LoadFileSync(const char* filename, int& outSize) override {
        outSize = 0;
        if (!m_gpu) return nullptr;

        // Blocking directory resolution + blocking file read.  See the note in
        // LoadFileSyncWithProgress for why the async parser is avoided here.
        uint32_t fileLBA = 0, fileSize = 0;
        if (!resolveEntryBlocking(filename, fileLBA, fileSize)) return nullptr;

        uint32_t sectors = (fileSize + 2047) / 2048;
        uint8_t* buf = new uint8_t[sectors * 2048];

        bool ok = m_cdrom.readSectorsBlocking(fileLBA, sectors, buf, *m_gpu);
        if (!ok) {
            delete[] buf;
            return nullptr;
        }

        outSize = static_cast<int>(fileSize);
        return buf;
    }

    // -- FreeFile -------------------------------------------------
    void FreeFile(uint8_t* data) override { delete[] data; }

    const char* Name() const override { return "cdrom"; }

    /** Stash the GPU pointer so LoadFileSync can spin on pumpCallbacks. */
    void setGPU(psyqo::GPU* gpu) { m_gpu = gpu; }

    psyqo::CDRomDevice* getCDRomDevice() { return &m_cdrom; }

    /** Blocking directory lookup; the drive must be awake (scene loading). */
    bool resolveBlocking(const char* path, uint32_t& outLBA, uint32_t& outSize) {
        return resolveEntryBlocking(path, outLBA, outSize);
    }

  private:
    // -- resolveEntryBlocking -------------------------------------
    // Walks the ISO9660 directory tree to find `path`, using only the
    // blocking readSectorsBlocking API.  This is a deliberate,
    // self-contained replacement for psyqo's async ISO9660Parser::getDirentry
    // in the synchronous load paths: getDirentry reads directory sectors
    // asynchronously (pump-driven), and interleaving those with the blocking
    // file reads desyncs the CD drive and aborts.  Doing the lookup blocking
    // keeps the whole scene-load sequence on one consistent code path.
    //
    // `path` is an ISO path like "SCENE3/SCENE_3.VRM;1" (no leading slash;
    // '/'-separated; file component carries the ";1" version suffix).  Returns
    // false if any component is missing.  The PVD's root record is cached on
    // first use; directory sectors are re-read per lookup (cheap: 1-2 sectors).
    bool resolveEntryBlocking(const char* path, uint32_t& outLBA, uint32_t& outSize) {
        if (!m_gpu) return false;

        // Cache the root directory record from the Primary Volume Descriptor.
        if (!m_haveRoot) {
            if (!m_cdrom.readSectorsBlocking(16, 1, m_dirBuf, *m_gpu)) return false;
            const uint8_t* r = m_dirBuf + 156;  // root DirRecord inside the PVD
            m_rootLBA = readLE32(r + 2);
            m_rootSize = readLE32(r + 10);
            m_haveRoot = true;
        }

        uint32_t dirLBA = m_rootLBA;
        uint32_t dirSize = m_rootSize;

        for (const char* seg = path; *seg;) {
            const char* end = seg;
            while (*end && *end != '/') ++end;
            const uint32_t segLen = static_cast<uint32_t>(end - seg);

            bool found = false;
            const uint32_t dirSectors = (dirSize + 2047) / 2048;
            for (uint32_t s = 0; s < dirSectors && !found; ++s) {
                if (!m_cdrom.readSectorsBlocking(dirLBA + s, 1, m_dirBuf, *m_gpu)) return false;

                uint32_t off = 0;
                while (off < 2048) {
                    const uint8_t recLen = m_dirBuf[off];
                    if (recLen == 0) break;  // rest of this sector is padding
                    const uint8_t nameLen = m_dirBuf[off + 32];
                    const uint8_t* nm = m_dirBuf + off + 33;
                    // Skip the "." (0x00) and ".." (0x01) self/parent entries.
                    const bool special = (nameLen == 1 && (nm[0] == 0 || nm[0] == 1));
                    if (!special && nameLen == segLen &&
                        __builtin_memcmp(nm, seg, segLen) == 0) {
                        dirLBA = readLE32(m_dirBuf + off + 2);
                        dirSize = readLE32(m_dirBuf + off + 10);
                        found = true;
                        break;
                    }
                    off += recLen;
                }
            }
            if (!found) return false;

            seg = end;
            if (*seg == '/') ++seg;
        }

        outLBA = dirLBA;
        outSize = dirSize;
        return true;
    }

    static uint32_t readLE32(const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    psyqo::CDRomDevice m_cdrom;
    psyqo::ISO9660Parser m_isoParser;

    // Blocking-resolver state (see resolveEntryBlocking).
    uint8_t m_dirBuf[2048];
    bool m_haveRoot = false;
    uint32_t m_rootLBA = 0;
    uint32_t m_rootSize = 0;

    // Sub-queues (not nested in the parent queue's fixed_vector storage).
    psyqo::TaskQueue m_initQueue;
    psyqo::TaskQueue m_readQueue;
    psyqo::TaskQueue m_syncQueue;

    // State carried across the async getDirentry->read chain.
    psyqo::ISO9660Parser::ReadRequest m_request;
    psyqo::TaskQueue::Task* m_pendingTask = nullptr;
    uint8_t** m_pOutBuffer = nullptr;
    int* m_pOutSize = nullptr;

    psyqo::GPU* m_gpu = nullptr;
};

}  // namespace psxsplash
