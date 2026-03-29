#pragma once

#include "fileloader.hh"
#include "pcdrv_handler.hh"

namespace psxsplash {

/**
 * FileLoaderPCdrv — loads files via the PCdrv protocol.
 *
 * Works transparently in two modes (handled by pcdrv_handler.hh):
 *   - Emulator mode: break instructions intercepted by PCSX-Redux
 *   - Real hardware mode: SIO1 serial protocol (break handler installed
 *     by pcdrv_sio1_init, then pcdrv_init detects which path to use)
 */
class FileLoaderPCdrv final : public FileLoader {
  public:
    // ── prepare: no-op for PCdrv ──────────────────────────────────
    void prepare() override {}

    // ── scheduleInit ──────────────────────────────────────────────
    psyqo::TaskQueue::Task scheduleInit() override {
        return psyqo::TaskQueue::Task([this](psyqo::TaskQueue::Task* task) {

            pcdrv_sio1_init();

            m_available = (pcdrv_init() == 0);
            task->resolve();
        });
    }

    // ── scheduleLoadFile ──────────────────────────────────────────
    psyqo::TaskQueue::Task scheduleLoadFile(
        const char* filename, uint8_t*& outBuffer, int& outSize) override
    {
        return psyqo::TaskQueue::Task(
            [this, filename, &outBuffer, &outSize](psyqo::TaskQueue::Task* task) {
                outBuffer = doLoad(filename, outSize);
                task->complete(outBuffer != nullptr);
            });
    }

    // ── LoadFileSync ──────────────────────────────────────────────
    uint8_t* LoadFileSync(const char* filename, int& outSize) override {
        return doLoad(filename, outSize);
    }

    // ── FreeFile ──────────────────────────────────────────────────
    void FreeFile(uint8_t* data) override { delete[] data; }

    const char* Name() const override { return "pcdrv"; }

  private:
    bool m_available = false;

    uint8_t* doLoad(const char* filename, int& outSize) {
        outSize = 0;
        if (!m_available) return nullptr;

        int fd = pcdrv_open(filename, 0, 0);
        if (fd < 0) return nullptr;

        int size = pcdrv_seek(fd, 0, 2);   // SEEK_END
        if (size <= 0) { pcdrv_close(fd); return nullptr; }
        pcdrv_seek(fd, 0, 0);              // SEEK_SET

        // 4-byte aligned for safe struct casting
        int aligned = (size + 3) & ~3;
        uint8_t* buf = new uint8_t[aligned];

        int read = pcdrv_read(fd, buf, size);
        pcdrv_close(fd);

        if (read != size) { delete[] buf; return nullptr; }
        outSize = size;
        return buf;
    }
};

}  // namespace psxsplash
