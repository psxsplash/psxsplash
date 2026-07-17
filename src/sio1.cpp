#include "sio1.hh"

#include <common/hardware/pcsxhw.h>
#include <common/syscalls/syscalls.h>
#include <psyqo/hardware/cpu.hh>
#include <psyqo/hardware/hwregs.hh>
#include <psyqo/hardware/sio.hh>
#include <psyqo/kernel.hh>

namespace {

using psyqo::Hardware::Register;
using psyqo::Hardware::WriteQueue;

// SIO1 registers. Offsets are relative to the I/O base 0x1F801000 (see
// hwregs.hh); SIO1 sits exactly 0x10 above SIO0. The bit meanings in the
// Control/Status enums from psyqo/hardware/sio.hh already describe SIO1
// (overrun/framing/CTS bits and all), so we reuse those enums directly.
Register<0x0050, uint8_t, WriteQueue::Bypass> Data;
Register<0x0054, uint32_t, WriteQueue::Bypass> Stat;
Register<0x0058, uint16_t, WriteQueue::Bypass> Mode;
Register<0x005a, uint16_t, WriteQueue::Bypass> Ctrl;
Register<0x005e, uint16_t, WriteQueue::Bypass> Baud;

}  // namespace

namespace psxsplash {
// Definition of the singleton storage. As a static member it can reach the
// private constructor; constructed at static-init like PSYQo's own globals.
Sio1 Sio1::s_instance;
}  // namespace psxsplash

namespace {

namespace SIObits = psyqo::Hardware::SIO;

// SIO1 MODE for 8N1 with baud reload factor MUL1:
//   bits 0-1 = 1  -> MUL1   (0 would mean STOP on SIO1)
//   bits 2-3 = 3  -> 8 data bits
//   bit  4   = 0  -> parity disabled
//   bits 6-7 = 1  -> 1 stop bit
constexpr uint16_t c_mode8N1 = (1 << 0) | (3 << 2) | (1 << 6);  // 0x4D

// The SIO clock: BitsPerSecond = 33868800 / ((Reload*Factor) AND NOT 1), Factor=1.
constexpr uint32_t c_sioClock = 33868800u;

// RX IRQ threshold. CTRL bits 8-9 select an IRQ when the RX FIFO holds 1, 2, 4
// or 8 bytes. 8 would be maximally efficient but leaves zero slack: the FIFO is
// already full when the IRQ fires, so any latency in reaching the handler loses
// bytes. 4 gives ~347us of headroom at 115200 while still halving the interrupt
// rate versus 1.
constexpr uint16_t c_rxIrqThreshold = 2;  // 0=1 byte, 1=2, 2=4, 3=8
constexpr uint16_t c_rxIrqModeShift = 8;

// Retail-kernel BIOS event for IRQ8 (SIO). psxsplash does not call
// Kernel::takeOverKernel(), and Kernel::queueIRQHandler() hard-asserts on
// `s_tookOverKernel` (psyqo kernel.cpp:189), so the BIOS event route is the only
// one available to us. Class ids are from psx-spx "kernelbios.md": F000000Bh is
// IRQ8/SIO.
constexpr uint32_t c_eventClassSio = 0xF000000Bu;
constexpr uint32_t c_eventSpecInterrupt = 0x0002u;  // EvSpINT
constexpr uint32_t c_eventModeCallback = 0x1000u;   // EvMdINTR — invoke the handler

}  // namespace

namespace psxsplash {

Sio1& Sio1::Get() { return s_instance; }

void Sio1::init(uint32_t baud, RxMode rxMode) {
    // Internal reset: clears the SIO1 block and its FIFOs.
    Ctrl = SIObits::Control::CTRL_IR;
    Ctrl = 0;

    Mode = c_mode8N1;

    uint32_t reload = baud ? (c_sioClock / baud) : 1;
    if (reload == 0) reload = 1;
    if (reload > 0xFFFF) reload = 0xFFFF;
    Baud = static_cast<uint16_t>(reload);

    // Resolve Auto. The two targets want opposite settings — Redux corrupts its
    // own receive buffer if we enable the RX IRQ, real hardware silently drops
    // ~96% of a burst if we don't — so guessing wrong is expensive either way.
    // pcsx_present() reads the 'PCSX' magic the emulator exposes at 0x1f802080,
    // which is the same signal the PCdrv file loader already relies on.
    if (rxMode == RxMode::Auto) {
        rxMode = pcsx_present() ? RxMode::Polled : RxMode::Interrupt;
    }
    m_rxMode = rxMode;

    // Enable transmit and receive; assert the DTR/RTS handshake outputs so a
    // real link-cable peer (which may gate on them) sees us as ready. RTS is
    // also load-bearing for the emulator: PCSX-Redux's Protobuf path drops
    // inbound data unless CR_RTS is set (sio1.cc: processMessage).
    uint16_t ctrl = SIObits::Control::CTRL_TXEN | SIObits::Control::CTRL_RXE | SIObits::Control::CTRL_DTR |
                    SIObits::Control::CTRL_RTS;

    if (rxMode == RxMode::Interrupt) {
        ctrl |= SIObits::Control::CTRL_RXIRQEN | (c_rxIrqThreshold << c_rxIrqModeShift);
    }
    Ctrl = ctrl;

    m_rx.clear();
    m_tx.clear();

    if (rxMode == RxMode::Interrupt) {
        installIrqHandler();
        psyqo::Hardware::CPU::IMask.set(psyqo::Hardware::CPU::IRQ::SIO);
    } else {
        // Leave IRQ8 masked. Beyond saving the interrupt, this is what keeps the
        // emulator's destructive ">8 bytes -> reset the whole fifo" branch from
        // ever running: without CTRL_RXIRQEN nothing schedules SIO1::interrupt().
        psyqo::Hardware::CPU::IMask.clear(psyqo::Hardware::CPU::IRQ::SIO);
    }

    m_initialized = true;
}

void Sio1::installIrqHandler() {
    // The BIOS event is opened once for the lifetime of the process. psyqo's
    // openEvent allocates an internal slot with no way to free it, and re-opening
    // on every init() (which runs per Net.Connect()) would leak a slot each time.
    if (m_irqInstalled) return;

    uint32_t event = psyqo::Kernel::openEvent(c_eventClassSio, c_eventSpecInterrupt, c_eventModeCallback,
                                              []() { Sio1::Get().handleRxIrq(); });
    syscall_enableEvent(event);
    m_irqInstalled = true;
}

void Sio1::handleRxIrq() {
    m_rxIrqs++;
    drainRx();

    // Acknowledge. psx-spx ("Interrupt Acknowledge Notes") is explicit about the
    // order: reset I_STAT.8 FIRST, then set CTRL.4 — doing it the other way round
    // can miss an IRQ that arrives in between. I_STAT is ack-by-writing-zero, so
    // clear() writes 0 to the SIO bit and 1s elsewhere, leaving other sources up.
    psyqo::Hardware::CPU::IReg.clear(psyqo::Hardware::CPU::IRQ::SIO);
    Ctrl = Ctrl | SIObits::Control::CTRL_ERRRES;  // CTRL.4 doubles as the SIO IRQ ack
}

// Producer side of m_rx. Runs on the main thread in RxMode::Polled and from the
// SIO IRQ in RxMode::Interrupt — never both, since poll() skips it when
// interrupts own the FIFO. That single-producer guarantee is what makes the
// lock-free RingBuffer safe here (see ringbuffer.hh): read() is the only
// consumer and always runs on the main thread. The stats counters below are
// plain increments and can race benignly with resetStats(); they are
// diagnostics, never control flow.
void Sio1::drainRx() {
    // Acknowledge/clear any error condition first, otherwise the RX path can
    // wedge. Overrun/framing/parity are counted for the health overlay.
    uint32_t stat = Stat;
    if (stat & (SIObits::Status::STAT_OE | SIObits::Status::STAT_FE | SIObits::Status::STAT_PE)) {
        m_serialErrors++;
        Ctrl |= SIObits::Control::CTRL_ERRRES;
    }

    while (Stat & SIObits::Status::STAT_RXRDY) {
        uint8_t b = Data;
        if (m_rx.push(b)) {
            m_bytesRx++;
        } else {
            m_rxOverflow++;  // ring full: drop the byte, keep the link alive
        }
    }
}

void Sio1::pumpTx() {
    uint8_t b;
    // Enter the body only when the TX FIFO can accept a byte AND we have one to
    // send. The short-circuit guarantees we never pop a byte we can't write.
    while ((Stat & SIObits::Status::STAT_TXRDY) && m_tx.pop(b)) {
        Data = b;
        m_bytesTx++;
    }
}

void Sio1::poll() {
    if (!m_initialized) return;
    // In Interrupt mode the IRQ owns the RX FIFO — draining it from here too
    // would race the handler for the same hardware register. TX stays polled in
    // both modes: it is paced by our own send cadence, not by the peer, so the
    // 2-byte TX FIFO never has to survive an unannounced burst.
    if (m_rxMode == RxMode::Polled) drainRx();
    pumpTx();
}

uint32_t Sio1::read(uint8_t* dst, uint32_t max) {
    uint32_t n = 0;
    uint8_t b;
    while (n < max && m_rx.pop(b)) {
        dst[n++] = b;
    }
    return n;
}

uint32_t Sio1::write(const uint8_t* src, uint32_t len) {
    uint32_t n = 0;
    while (n < len && m_tx.push(src[n])) {
        n++;
    }
    pumpTx();  // opportunistic immediate flush so short messages leave promptly
    return n;
}

bool Sio1::writeByte(uint8_t b) {
    bool ok = m_tx.push(b);
    pumpTx();
    return ok;
}

}  // namespace psxsplash
