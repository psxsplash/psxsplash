#pragma once

#include "fileloader.hh"

#include <psyqo/cdrom-device.hh>
#include <psyqo/iso9660-parser.hh>
#include <psyqo/gpu.hh>
#include <psyqo/task.hh>

namespace psxsplash {

/**
 * FileLoaderCDRom — loads files from CD-ROM using the psyqo ISO9660 parser.
 *
 * Follows the same pattern as nugget/psyqo/examples/task-demo:
 *   1. CDRomDevice::prepare()          — called from Application::prepare()
 *   2. CDRomDevice::scheduleReset()    — reset the drive
 *   3. ISO9660Parser::scheduleInitialize() — parse the PVD and root dir
 *   4. ISO9660Parser::scheduleGetDirentry  — look up a file by path
 *   5. ISO9660Parser::scheduleReadRequest  — read the file sectors
 *
 */

class FileLoaderCDRom final : public FileLoader {
  public:
    FileLoaderCDRom() : m_isoParser(&m_cdrom) {}

    // ── prepare: must be called from Application::prepare() ──────
    void prepare() override {
        m_cdrom.prepare();
    }

    // ── scheduleInit ─────────────────────────────────────────────
    // Chains: reset CD drive → initialise ISO9660 parser.
    psyqo::TaskQueue::Task scheduleInit() override {
        m_initQueue
            .startWith(m_cdrom.scheduleReset())
            .then(m_isoParser.scheduleInitialize());
        return m_initQueue.schedule();
    }

    // ── scheduleLoadFile ─────────────────────────────────────────
    // Chains: getDirentry → allocate + read.
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

                // Step 1 — look up the directory entry.
                m_isoParser.getDirentry(
                    filename, &m_request.entry,
                    [this](bool success) {
                        if (!success ||
                            m_request.entry.type ==
                                psyqo::ISO9660Parser::DirEntry::INVALID) {
                            m_pendingTask->reject();
                            return;
                        }

                        // Step 2 — allocate a sector-aligned buffer and read.
                        uint32_t sectors =
                            (m_request.entry.size + 2047) / 2048;
                        uint8_t* buf = new uint8_t[sectors * 2048];
                        *m_pOutBuffer = buf;
                        *m_pOutSize = static_cast<int>(m_request.entry.size);
                        m_request.buffer = buf;

                        // Step 3 — chain the actual CD read via a sub-queue.
                        m_readQueue
                            .startWith(
                                m_isoParser.scheduleReadRequest(&m_request))
                            .then([this](psyqo::TaskQueue::Task* inner) {
                                // Read complete — resolve the outer task.
                                m_pendingTask->resolve();
                                inner->resolve();
                            })
                            .butCatch([this](psyqo::TaskQueue*) {
                                // Read failed — clean up and reject.
                                delete[] *m_pOutBuffer;
                                *m_pOutBuffer = nullptr;
                                *m_pOutSize = 0;
                                m_pendingTask->reject();
                            })
                            .run();
                    });
            });
    }

    // ── LoadFileSyncWithProgress ───────────────────────────────
    // Reads the file in 32-sector (64 KB) chunks, calling the
    // progress callback between each chunk so the loading bar
    // animates during the CD-ROM transfer.
    uint8_t* LoadFileSyncWithProgress(
        const char* filename, int& outSize,
        const LoadProgressInfo* progress) override
    {
        outSize = 0;
        if (!m_isoParser.initialized()) return nullptr;

        // --- getDirentry (blocking via one-shot TaskQueue) ---
        psyqo::ISO9660Parser::DirEntry entry;
        bool found = false;

        m_syncQueue
            .startWith(m_isoParser.scheduleGetDirentry(filename, &entry))
            .then([&found](psyqo::TaskQueue::Task* t) {
                found = true;
                t->resolve();
            })
            .butCatch([](psyqo::TaskQueue*) {})
            .run();

        while (m_syncQueue.isRunning()) {
            m_gpu->pumpCallbacks();
        }

        if (!found || entry.type == psyqo::ISO9660Parser::DirEntry::INVALID)
            return nullptr;

        // --- chunked sector read with progress ---
        uint32_t totalSectors = (entry.size + 2047) / 2048;
        uint8_t* buf = new uint8_t[totalSectors * 2048];

        static constexpr uint32_t kChunkSectors = 32;  // 64 KB per chunk
        uint32_t sectorsRead = 0;

        while (sectorsRead < totalSectors) {
            uint32_t toRead = totalSectors - sectorsRead;
            if (toRead > kChunkSectors) toRead = kChunkSectors;

            bool ok = m_cdrom.readSectorsBlocking(
                entry.LBA + sectorsRead, toRead,
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

        outSize = static_cast<int>(entry.size);
        return buf;
    }

    // ── LoadFileSync ─────────────────────────────────────────────
    // Blocking fallback for code paths that can't use tasks (e.g.
    // SceneManager scene transitions).  Uses the blocking readSectors
    // variant which spins on GPU callbacks.
    uint8_t* LoadFileSync(const char* filename, int& outSize) override {
        outSize = 0;

        if (!m_isoParser.initialized()) return nullptr;

        // --- getDirentry (blocking via one-shot TaskQueue) ---
        psyqo::ISO9660Parser::DirEntry entry;
        bool found = false;

        m_syncQueue
            .startWith(m_isoParser.scheduleGetDirentry(filename, &entry))
            .then([&found](psyqo::TaskQueue::Task* t) {
                found = true;
                t->resolve();
            })
            .butCatch([](psyqo::TaskQueue*) {})
            .run();

        // Spin until the queue finishes (GPU callbacks service the CD IRQs).
        while (m_syncQueue.isRunning()) {
            m_gpu->pumpCallbacks();
        }

        if (!found || entry.type == psyqo::ISO9660Parser::DirEntry::INVALID)
            return nullptr;

        // --- read sectors (blocking API) ---
        uint32_t sectors = (entry.size + 2047) / 2048;
        uint8_t* buf = new uint8_t[sectors * 2048];

        bool ok = m_cdrom.readSectorsBlocking(
            entry.LBA, sectors, buf, *m_gpu);
        if (!ok) {
            delete[] buf;
            return nullptr;
        }

        outSize = static_cast<int>(entry.size);
        return buf;
    }

    // ── FreeFile ─────────────────────────────────────────────────
    void FreeFile(uint8_t* data) override { delete[] data; }

    const char* Name() const override { return "cdrom"; }

    /** Stash the GPU pointer so LoadFileSync can spin on pumpCallbacks. */
    void setGPU(psyqo::GPU* gpu) { m_gpu = gpu; }

  private:
    psyqo::CDRomDevice m_cdrom;
    psyqo::ISO9660Parser m_isoParser;

    // Sub-queues (not nested in the parent queue's fixed_vector storage).
    psyqo::TaskQueue m_initQueue;
    psyqo::TaskQueue m_readQueue;
    psyqo::TaskQueue m_syncQueue;

    // State carried across the async getDirentry→read chain.
    psyqo::ISO9660Parser::ReadRequest m_request;
    psyqo::TaskQueue::Task* m_pendingTask = nullptr;
    uint8_t** m_pOutBuffer = nullptr;
    int* m_pOutSize = nullptr;

    psyqo::GPU* m_gpu = nullptr;
};

}  // namespace psxsplash
