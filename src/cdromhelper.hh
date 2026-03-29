#pragma once

#if defined(LOADER_CDROM)

#include <psyqo/hardware/cpu.hh>
#include <psyqo/hardware/cdrom.hh>

namespace psxsplash {

class CDRomHelper {
  public:
    static void SilenceDrive() {
        psyqo::Hardware::CPU::IMask.clear(psyqo::Hardware::CPU::IRQ::CDRom);
        psyqo::Hardware::CPU::flushWriteQueue();
        psyqo::Hardware::CDRom::CauseMask = 0;

        s_silenced = true;
    }

    static void WakeDrive() {
        if (!s_silenced) return;
        s_silenced = false;

        drainController();

        psyqo::Hardware::CDRom::CauseMask = 0x1f;

        drainController();

        psyqo::Hardware::CPU::IMask.set(psyqo::Hardware::CPU::IRQ::CDRom);
    }

  private:
    static inline bool s_silenced = false;

    static void drainController() {
        uint8_t cause = psyqo::Hardware::CDRom::Cause;
        if (cause & 7)
            psyqo::Hardware::CDRom::Cause = 7;
        if (cause & 0x18)
            psyqo::Hardware::CDRom::Cause = 0x18;
        while (psyqo::Hardware::CDRom::Ctrl.access() & 0x20)
            psyqo::Hardware::CDRom::Response;  // drain FIFO
        psyqo::Hardware::CPU::IReg.clear(
            psyqo::Hardware::CPU::IRQ::CDRom);
    }
};

}  // namespace psxsplash

#endif  // LOADER_CDROM
