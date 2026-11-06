#pragma once

#include "fileloader.hh"

#include <EASTL/string_view.h>
#include <psyqo/buffer.hh>
#include <psyqo/cdrom-device.hh>
#include <psyqo/cdrom-pcdrv.hh>
#include <psyqo/gpu.hh>
#include <psyqo/task.hh>
#include <psyqo/xprintf.h>
#include <psyqo-paths/archive-manager.hh>

#include "cdromhelper.hh"
#include "pcdrv_handler.hh"

#ifndef PSXSPLASH_ARCHIVE_IMAGE
#define PSXSPLASH_ARCHIVE_IMAGE "psxsplash.bin"
#endif

namespace psxsplash {

/**
 * FileLoaderArchive - loads every file from one psyqo-paths archive.
 *
 * The disc image comes from PCSX-Redux's `authoring` tool, which writes the
 * archive index at LBA 23 of a raw (2352 bytes per sector) image. The same
 * image is read one of two ways, picked once at boot:
 *
 *   - PCdrv answers (emulator, or psxmon) and PSXSPLASH_ARCHIVE_IMAGE opens on
 *     the host: psyqo::CDRomPCDrv reads the image's sectors over PCdrv.
 *   - otherwise: psyqo::CDRomDevice reads the disc in the drive.
 *
 * File names are the archive keys (scene_0.splashpack, ...); the index holds
 * their DJB2 hashes, so there is no directory to walk. Entries are UCL-NRV2E
 * compressed or stored; StreamReader can only stream stored ones.
 */
class FileLoaderArchive final : public FileLoader {
  public:
    static constexpr uint32_t kArchiveLBA = 23;

    FileLoaderArchive() : m_pcdrvImage(PSXSPLASH_ARCHIVE_IMAGE) {}

    void prepare() override { m_device.prepare(); }

    psyqo::TaskQueue::Task scheduleInit() override {
        return psyqo::TaskQueue::Task([this](psyqo::TaskQueue::Task* task) {
            m_initTask = task;
            psyqo::paths::ArchiveManager::registerUCL_NRV2EDecompressor();
            // The drive goes first: a PCdrv call traps when nothing serves it
            // (PCSX-Redux with PCdrv off), and there is no way to ask first.
            useDevice(true);
            m_initQueue.reset();
            m_initQueue.startWith(m_device.scheduleReset())
                .then(m_archive.scheduleInitialize(kArchiveLBA, m_device))
                .then([this](psyqo::TaskQueue::Task* inner) {
                    inner->resolve();
                    initDone(true);
                })
                .butCatch([this](psyqo::TaskQueue*) {
                    if (!pcdrv_uses_break() || !m_pcdrvImage.ensureOpen()) {
                        printf("FileLoader: no PCdrv image " PSXSPLASH_ARCHIVE_IMAGE " either\n");
                        initDone(false);
                        return;
                    }
                    useDevice(false);
                    // Re-entering this queue from its own catch is not safe: use the other one.
                    m_pcdrvQueue.reset();
                    m_pcdrvQueue.startWith(m_archive.scheduleInitialize(kArchiveLBA, m_pcdrvImage))
                        .then([this](psyqo::TaskQueue::Task* inner) {
                            inner->resolve();
                            initDone(true);
                        })
                        .butCatch([this](psyqo::TaskQueue*) { initDone(false); })
                        .run();
                })
                .run();
        });
    }

    psyqo::TaskQueue::Task scheduleLoadFile(const char* filename, uint8_t*& outBuffer, int& outSize) override {
        return psyqo::TaskQueue::Task([this, filename, &outBuffer, &outSize](psyqo::TaskQueue::Task* task) {
            outBuffer = LoadFileSync(filename, outSize);
            task->complete(outBuffer != nullptr);
        });
    }

    uint8_t* LoadFileSync(const char* filename, int& outSize) override {
        outSize = 0;
        if (!m_ready || !m_gpu) return nullptr;
        auto* entry = m_archive.getIndexEntry(eastl::string_view(filename));
        if (!entry) return nullptr;

        // ArchiveManager reads (and decompresses in place) into the buffer it is
        // handed when one is set; an external Buffer is never reallocated or
        // freed, so it must already be the size the manager computes for it.
        const uint32_t sectors = entry->getCompressedSize();
        uint32_t bytes = sectors * 2048;
        if (entry->getCompressionMethod() != psyqo::paths::ArchiveManager::IndexEntry::Method::NONE) {
            uint32_t need = ((entry->getDecompSize() + 3) & ~3) + m_archive.getSafeMarginLength();
            if (need > bytes) bytes = need;
        }
        uint8_t* buf = new uint8_t[bytes];
        if (!buf) return nullptr;
        m_archive.setBuffer(psyqo::Buffer<uint8_t>(buf, bytes));

        bool done = false;
        uint32_t size = 0;
        CDRomHelper::WakeDrive();
        m_archive.readFile(entry, *m_cd, [&done, &size](psyqo::Buffer<uint8_t>&& data) {
            size = data.size();
            done = true;
        });
        while (!done) m_gpu->pumpCallbacks();

        if (size == 0) {
            delete[] buf;
            return nullptr;
        }
        outSize = static_cast<int>(size);
        return buf;
    }

    void FreeFile(uint8_t* data) override { delete[] data; }

    const char* Name() const override { return "archive"; }

    void setGPU(psyqo::GPU* gpu) { m_gpu = gpu; }

    /** The physical drive, or nullptr when the image is read over PCdrv (no CD-DA then). */
    psyqo::CDRomDevice* getCDRomDevice() { return m_usingDevice ? &m_device : nullptr; }

    /** First sector and size of a STORED entry, for StreamReader. */
    bool resolveBlocking(const char* path, uint32_t& outLBA, uint32_t& outSize) {
        if (!m_ready) return false;
        auto* entry = m_archive.getIndexEntry(eastl::string_view(path));
        if (!entry) return false;
        if (entry->getCompressionMethod() != psyqo::paths::ArchiveManager::IndexEntry::Method::NONE) {
            printf("FileLoader: %s is compressed in the archive and cannot be streamed\n", path);
            return false;
        }
        outLBA = m_archive.getIndexEntrySectorStart(entry);
        outSize = entry->getDecompSize();
        return true;
    }

    /** Raw sector read of the image, blocking. */
    bool readSectorsBlocking(uint32_t lba, uint32_t count, void* buffer) {
        if (!m_gpu || !m_cd) return false;
        if (m_usingDevice) return m_device.readSectorsBlocking(lba, count, buffer, *m_gpu);
        bool done = false, ok = false;
        m_cd->readSectors(lba, count, buffer, [&done, &ok](bool success) {
            ok = success;
            done = true;
        });
        while (!done) m_gpu->pumpCallbacks();
        return ok;
    }

  private:
    void useDevice(bool device) {
        m_usingDevice = device;
        m_cd = device ? static_cast<psyqo::CDRom*>(&m_device) : &m_pcdrvImage;
        CDRomHelper::SetDriveInUse(device);
    }

    void initDone(bool ok) {
        const char* via = m_usingDevice ? "the CD drive" : "PCdrv " PSXSPLASH_ARCHIVE_IMAGE;
        if (ok) {
            m_ready = true;
            printf("FileLoader: archive of %u files via %s\n", m_archive.getIndexCount(), via);
            m_initTask->resolve();
        } else {
            printf("FileLoader: no archive at LBA %u via %s\n", kArchiveLBA, via);
            m_initTask->reject();
        }
    }

    psyqo::CDRomDevice m_device;
    psyqo::CDRomPCDrv m_pcdrvImage;
    psyqo::CDRom* m_cd = nullptr;
    bool m_usingDevice = true;
    bool m_ready = false;

    psyqo::paths::ArchiveManager m_archive;
    psyqo::TaskQueue m_initQueue;
    psyqo::TaskQueue m_pcdrvQueue;
    psyqo::TaskQueue::Task* m_initTask = nullptr;
    psyqo::GPU* m_gpu = nullptr;
};

}  // namespace psxsplash
