#pragma once

#include "fileloader.hh"
#include "common/kernel/pcdrv.h"

namespace psxsplash {

/**
 * FileLoaderPCdrv - loads files via the PCdrv protocol.
 *
 * Whatever serves PCdrv's break calls (an emulator, a monitor such as psxmon
 * or Unirom, a development kit) also carries the TTY, so nothing here sets up
 * a link of its own. With no driver the first call faults, which is the right
 * outcome: there would be no files to load either.
 */
class FileLoaderPCdrv final : public FileLoader {
  public:
    // -- prepare: no-op for PCdrv ----------------------------------
    void prepare() override {}

    // -- scheduleInit ----------------------------------------------
    psyqo::TaskQueue::Task scheduleInit() override {
        return psyqo::TaskQueue::Task([this](psyqo::TaskQueue::Task* task) {
            m_available = (pcInit() == 0);
            task->resolve();
        });
    }

    // -- scheduleLoadFile ------------------------------------------
    psyqo::TaskQueue::Task scheduleLoadFile(
        const char* filename, uint8_t*& outBuffer, int& outSize) override
    {
        return psyqo::TaskQueue::Task(
            [this, filename, &outBuffer, &outSize](psyqo::TaskQueue::Task* task) {
                outBuffer = doLoad(filename, outSize);
                task->complete(outBuffer != nullptr);
            });
    }

    // -- LoadFileSync ----------------------------------------------
    uint8_t* LoadFileSync(const char* filename, int& outSize) override {
        return doLoad(filename, outSize);
    }

    // -- FreeFile --------------------------------------------------
    void FreeFile(uint8_t* data) override { delete[] data; }

    const char* Name() const override { return "pcdrv"; }

  private:
    bool m_available = false;

    // PCinit()'s asm does not list v1, which the host overwrites; kept out of
    // line so the compiler holds nothing live there across the break.
    __attribute__((noinline)) static int pcInit() { return PCinit(); }

    uint8_t* doLoad(const char* filename, int& outSize) {
        outSize = 0;
        if (!m_available) return nullptr;

        int fd = PCopen(filename, 0, 0);
        if (fd < 0) return nullptr;

        int size = PClseek(fd, 0, 2);   // SEEK_END
        if (size <= 0) { PCclose(fd); return nullptr; }
        PClseek(fd, 0, 0);              // SEEK_SET

        // 4-byte aligned for safe struct casting
        int aligned = (size + 3) & ~3;
        uint8_t* buf = new uint8_t[aligned];

        int read = PCread(fd, buf, size);
        PCclose(fd);

        if (read != size) { delete[] buf; return nullptr; }
        outSize = size;
        return buf;
    }
};

}  // namespace psxsplash
