#pragma once

#include "fileloader.hh"

#include <EASTL/functional.h>
#include <EASTL/string_view.h>
#include <psyqo/buffer.hh>
#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>
#include <psyqo/task.hh>
#include <psyqo/xprintf.h>
#include <psyqo-paths/archive-manager.hh>

#include "cdromhelper.hh"
#include "common/kernel/pcdrv.h"

#ifndef PSXSPLASH_ARCHIVE_IMAGE
#define PSXSPLASH_ARCHIVE_IMAGE "psxsplash.bin"
#endif

namespace psxsplash {

// The disc builder overwrites the digit for a disc set to read at single speed.
extern volatile const char g_discReadSpeed[];
inline constexpr unsigned c_discReadSpeedDigit = sizeof("psxsplash disc read speed 2") - 2;

/**
 * FileLoaderArchive - loads every file from one psyqo-paths archive.
 *
 * The disc image comes from PCSX-Redux's `authoring` tool (tools/mkdisc.py),
 * which writes the archive index at LBA 23 of a raw (2352 bytes per sector)
 * image. The same image is read one of two ways, picked once at boot:
 *
 *   - from the CD drive, when the disc in it carries the archive;
 *   - otherwise over PCdrv, as PSXSPLASH_ARCHIVE_IMAGE on the host. There is
 *     no drive then: no CD-DA, no movies.
 *
 * File names are the archive keys (scene_0.splashpack, ...); the index holds
 * their DJB2 hashes, so there is no directory to walk. Entries are UCL-NRV2E or
 * LZ4 compressed, or stored; StreamReader can only stream stored ones, and STR
 * movies are STREAM entries (raw sectors at an absolute LBA).
 */
class FileLoaderArchive final : public FileLoader {
  public:
    static constexpr uint32_t kArchiveLBA = 23;

    static FileLoaderArchive& Get() { return static_cast<FileLoaderArchive&>(FileLoader::Get()); }

    FileLoaderArchive() : m_drive(*this), m_pcdrv(*this) {}

    void prepare() override {
        m_device.prepare();
        m_device.setDoubleSpeed(discDoubleSpeed());
    }

    psyqo::TaskQueue::Task scheduleInit() override {
        return psyqo::TaskQueue::Task([this](psyqo::TaskQueue::Task* task) {
            m_initTask = task;
            // authoring writes UCL-NRV2E; the editor's MIT-licensed writer uses LZ4.
            psyqo::paths::ArchiveManager::registerAllDecompressors();
            // The disc goes first. Without one the game can only be running over
            // PCdrv, so PCdrv is set up only then; with no driver at all that
            // first call faults, and there would have been nothing to load anyway.
            useDrive(true);
            m_initQueue.reset();
            m_initQueue.startWith(m_device.scheduleReset())
                .then(m_archive.scheduleInitialize(kArchiveLBA, m_drive))
                .then([this](psyqo::TaskQueue::Task* inner) {
                    inner->resolve();
                    initDone(true);
                })
                .butCatch([this](psyqo::TaskQueue*) {
                    if (!m_pcdrv.open()) {
                        printf("FileLoader: no disc, and no " PSXSPLASH_ARCHIVE_IMAGE " over PCdrv\n");
                        initDone(false);
                        return;
                    }
                    useDrive(false);
                    // Re-entering this queue from its own catch is not safe: use the other one.
                    m_pcdrvQueue.reset();
                    m_pcdrvQueue.startWith(m_archive.scheduleInitialize(kArchiveLBA, m_pcdrv))
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
        return LoadFileSyncWithProgress(filename, outSize, nullptr);
    }

    // The image readers below report after each 64 KB they read, so the
    // loading bar moves during a long read (it stops for the decompression).
    uint8_t* LoadFileSyncWithProgress(const char* filename, int& outSize,
                                      const LoadProgressInfo* progress) override {
        outSize = 0;
        if (!m_ready || !m_gpu) return nullptr;
        auto* entry = findLoadable(filename);
        if (!entry) return nullptr;

        // ArchiveManager reads (and decompresses in place) into the buffer it is
        // handed when one is set; an external Buffer is never reallocated or
        // freed, so it must already be the size the manager computes for it.
        const uint32_t bytes = loadBytes(entry);
        uint8_t* buf = new uint8_t[bytes];
        if (!buf) return nullptr;
        m_archive.setBuffer(psyqo::Buffer<uint8_t>(buf, bytes));

        bool done = false;
        uint32_t size = 0;
        m_progress = progress;
        m_progressSectors = entry->getCompressedSize();
        m_progressDone = 0;
        CDRomHelper::WakeDrive();
        m_archive.readFile(entry, *m_cd, [&done, &size](psyqo::Buffer<uint8_t>&& data) {
            size = data.size();
            done = true;
        });
        while (!done) m_gpu->pumpCallbacks();
        m_progress = nullptr;

        if (size == 0) {
            delete[] buf;
            return nullptr;
        }
        if (progress && progress->fn) progress->fn(progress->endPercent, progress->userData);
        outSize = static_cast<int>(size);
        return buf;
    }

    void FreeFile(uint8_t* data) override { delete[] data; }

    const char* Name() const override { return "archive"; }

    bool overPCdrv() const override { return m_overPCdrv; }

    void setGPU(psyqo::GPU* gpu) { m_gpu = gpu; }

    /** The physical drive, or nullptr when the image is read over PCdrv (no CD-DA then) or before init. */
    psyqo::CDRomDevice* getCDRomDevice() { return m_ready && m_usingDrive ? &m_device : nullptr; }

    /** What the disc is set to, whatever a bad read has since slowed the drive to. */
    static bool discDoubleSpeed() { return g_discReadSpeed[c_discReadSpeedDigit] != '1'; }

    /** The bytes LoadFileSync allocates for `path`, 0 when it is not a loadable entry. */
    uint32_t loadSize(const char* path) const {
        auto* entry = m_ready ? findLoadable(path) : nullptr;
        return entry ? loadBytes(entry) : 0;
    }

    /** First sector and size of a STORED entry, for StreamReader. */
    bool resolveBlocking(const char* path, uint32_t& outLBA, uint32_t& outSize) {
        if (!m_ready) return false;
        auto* entry = m_archive.getIndexEntry(eastl::string_view(path));
        if (!entry) return false;
        if (entry->getCompressionMethod() != psyqo::paths::ArchiveManager::IndexEntry::Method::NONE) {
            printf("FileLoader: %s is not stored in the archive and cannot be streamed\n", path);
            return false;
        }
        outLBA = m_archive.getIndexEntrySectorStart(entry);
        outSize = entry->getDecompSize();
        return true;
    }

    /** The index, for psyqo::paths::resolveMovie. */
    const psyqo::paths::ArchiveManager& archive() const { return m_archive; }

    /** Raw sector read of the image, blocking, with the drive's retries. */
    bool readSectorsBlocking(uint32_t lba, uint32_t count, void* buffer) {
        if (!m_ready || !m_gpu) return false;
        bool done = false, ok = false;
        m_cd->readSectors(lba, count, buffer, [&done, &ok](bool success) {
            ok = success;
            done = true;
        });
        while (!done) m_gpu->pumpCallbacks();
        return ok;
    }

    /**
     * Gameplay read from the drive: asynchronous, and a read that fails goes
     * again at single speed, as the blocking reads do. One at a time
     * (StreamReader keeps one in flight). The drive must be idle.
     */
    void readSectorsAsync(uint32_t lba, uint32_t count, void* buffer, eastl::function<void(bool)>&& done) {
        m_async.lba = lba;
        m_async.count = count;
        m_async.buffer = buffer;
        m_async.tries = 0;
        m_asyncDone = eastl::move(done);
        issueAsync();
    }

    /**
     * Reads the archive index again, for a hot reload: the host rewrote the
     * image, so every entry may have moved. Over PCdrv only. The image is
     * reopened, in case the host replaced the file rather than rewrote it.
     */
    bool reloadIndex() {
        if (!overPCdrv() || !m_gpu) return false;
        m_pcdrv.close();
        bool done = false, ok = false;
        if (m_pcdrv.open()) {
            m_archive.initialize(kArchiveLBA, m_pcdrv, [&done, &ok](bool success) {
                ok = success;
                done = true;
            });
            while (!done) m_gpu->pumpCallbacks();
        }
        if (ok) {
            printf("FileLoader: archive of %u files reread via PCdrv " PSXSPLASH_ARCHIVE_IMAGE "\n",
                   m_archive.getIndexCount());
        } else {
            // A half-written image: nothing in the old index can be trusted
            // either. Loads fail until a reload reads a whole one.
            printf("FileLoader: no archive in " PSXSPLASH_ARCHIVE_IMAGE " on reload\n");
        }
        m_ready = ok;
        return ok;
    }

  private:
    using IndexEntry = psyqo::paths::ArchiveManager::IndexEntry;

    // One bad read must not end the game. On a CD-R a console's laser holds badly, double
    // speed gives seek errors where single speed read 438 sectors of 438 exact, so a read
    // that fails goes again slower and the drive stays there.
    static constexpr int c_readTries = 4;
    static constexpr uint32_t kChunkSectors = 32;  // 64 KB between progress reports

    // The image, read in chunks with a progress report after each. Its reads
    // complete before readSectors returns.
    class ImageReader : public psyqo::CDRom {
      public:
        explicit ImageReader(FileLoaderArchive& loader) : m_loader(loader) {}
        void readSectors(uint32_t sector, uint32_t count, void* buffer,
                         eastl::function<void(bool)>&& callback) override {
            uint8_t* dst = static_cast<uint8_t*>(buffer);
            for (uint32_t done = 0; done < count;) {
                uint32_t n = count - done;
                if (n > kChunkSectors) n = kChunkSectors;
                if (!readChunk(sector + done, n, dst + done * 2048)) {
                    callback(false);
                    return;
                }
                done += n;
                m_loader.reportProgress(n);
            }
            callback(true);
        }

      protected:
        virtual bool readChunk(uint32_t sector, uint32_t count, uint8_t* buffer) = 0;
        FileLoaderArchive& m_loader;
    };

    // The drive, with blocking reads and the retries.
    class DriveReader final : public ImageReader {
      public:
        using ImageReader::ImageReader;

      private:
        bool readChunk(uint32_t sector, uint32_t count, uint8_t* buffer) override {
            if (!m_loader.m_gpu) return false;
            for (int i = 0; i < c_readTries; i++) {
                if (m_loader.m_device.readSectorsBlocking(sector, count, buffer, *m_loader.m_gpu)) return true;
                m_loader.m_device.setDoubleSpeed(false);
            }
            return false;
        }
    };

    // The image's 2048-byte payloads (Mode 2 Form 1, at +24 of each raw
    // sector) from the host over PCdrv.
    class PCdrvReader final : public ImageReader {
      public:
        using ImageReader::ImageReader;
        bool open() {
            if (m_fd < 0) {
                PCinit();
                m_fd = PCopen(PSXSPLASH_ARCHIVE_IMAGE, 0, 0);
            }
            return m_fd >= 0;
        }
        void close() {
            if (m_fd >= 0) PCclose(m_fd);
            m_fd = -1;
        }

      private:
        bool readChunk(uint32_t sector, uint32_t count, uint8_t* buffer) override {
            if (m_fd < 0) return false;
            for (uint32_t i = 0; i < count; i++) {
                const int offset = int((sector + i) * 2352 + 24);
                if (PClseek(m_fd, offset, PCDRV_SEEK_SET) != offset) return false;
                if (PCread(m_fd, buffer + i * 2048, 2048) != 2048) return false;
            }
            return true;
        }
        int m_fd = -1;
    };

    const IndexEntry* findLoadable(const char* path) const {
        auto* entry = m_archive.getIndexEntry(eastl::string_view(path));
        // A STREAM entry (a movie) is raw sectors, not a file to load.
        if (entry && entry->getCompressionMethod() == IndexEntry::Method::STREAM) return nullptr;
        return entry;
    }

    uint32_t loadBytes(const IndexEntry* entry) const {
        uint32_t bytes = entry->getCompressedSize() * 2048;
        if (entry->getCompressionMethod() != IndexEntry::Method::NONE) {
            uint32_t need = ((entry->getDecompSize() + 3) & ~3) + m_archive.getSafeMarginLength();
            if (need > bytes) bytes = need;
        }
        return bytes;
    }

    void reportProgress(uint32_t sectors) {
        if (!m_progress || !m_progress->fn || !m_progressSectors) return;
        m_progressDone += sectors;
        if (m_progressDone > m_progressSectors) m_progressDone = m_progressSectors;
        const uint8_t pct = m_progress->startPercent +
                            uint8_t(uint32_t(m_progress->endPercent - m_progress->startPercent) * m_progressDone /
                                    m_progressSectors);
        m_progress->fn(pct, m_progress->userData);
    }

    void issueAsync() {
        m_device.readSectors(m_async.lba, m_async.count, m_async.buffer, [this](bool ok) {
            if (!ok && ++m_async.tries < c_readTries) {
                m_device.setDoubleSpeed(false);
                issueAsync();
                return;
            }
            auto done = eastl::move(m_asyncDone);
            m_asyncDone = nullptr;
            if (done) done(ok);
        });
    }

    void useDrive(bool drive) {
        m_usingDrive = drive;
        m_cd = drive ? static_cast<psyqo::CDRom*>(&m_drive) : static_cast<psyqo::CDRom*>(&m_pcdrv);
        CDRomHelper::SetDriveInUse(drive);
    }

    void initDone(bool ok) {
        const char* via = m_usingDrive ? "the CD drive" : "PCdrv " PSXSPLASH_ARCHIVE_IMAGE;
        if (ok) {
            m_ready = true;
            m_overPCdrv = !m_usingDrive;
            printf("FileLoader: archive of %u files via %s\n", m_archive.getIndexCount(), via);
            m_initTask->resolve();
        } else {
            printf("FileLoader: no archive at LBA %u via %s\n", kArchiveLBA, via);
            m_initTask->reject();
        }
    }

    psyqo::CDRomDevice m_device;
    DriveReader m_drive;
    PCdrvReader m_pcdrv;
    psyqo::CDRom* m_cd = nullptr;
    bool m_usingDrive = true;
    bool m_ready = false;
    bool m_overPCdrv = false;  // set once, at init

    psyqo::paths::ArchiveManager m_archive;
    psyqo::TaskQueue m_initQueue;
    psyqo::TaskQueue m_pcdrvQueue;
    psyqo::TaskQueue::Task* m_initTask = nullptr;
    psyqo::GPU* m_gpu = nullptr;

    const LoadProgressInfo* m_progress = nullptr;
    uint32_t m_progressSectors = 0;
    uint32_t m_progressDone = 0;

    struct {
        uint32_t lba, count;
        void* buffer;
        int tries;
    } m_async = {};
    eastl::function<void(bool)> m_asyncDone;
};

}  // namespace psxsplash
