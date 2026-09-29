#include "sio1.hh"

#include "irqack.hh"

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

// SIO1 MODE for 8N1 with baud reload factor MUL16:
//   bits 0-1 = 2  -> MUL16  (1 = MUL1, 0 would mean STOP on SIO1)
//   bits 2-3 = 3  -> 8 data bits
//   bit  4   = 0  -> parity disabled
//   bits 6-7 = 1  -> 1 stop bit
//
// MUL16, NOT MUL1, AND THIS IS NOT A TUNING CHOICE.
//
// The reload factor is the UART's OVERSAMPLING clock, and an asynchronous
// receiver cannot work without one. It has no clock line to lock to, so it finds
// the falling edge of the start bit and then counts half a bit period inward to
// sample each bit near its CENTRE, where it is furthest from either neighbour.
// With MUL16 it has 16 ticks per bit to do that with. With MUL1 it has one - no
// sub-bit resolution at all - so it samples at bit BOUNDARIES, and any jitter,
// any clock difference between the two ends, lands on the wrong side of an edge.
// The stop bit is then read as the wrong level, which the hardware reports as a
// framing error, and the byte is corrupt.
//
// MUL1 exists for SIO0, which is SYNCHRONOUS: it clocks the controller itself, so
// there is nothing to align to and oversampling would be meaningless. SIO1 is
// asynchronous. We had SIO0's clock mode on an asynchronous port.
//
// The signature of getting it wrong is framing errors far outnumbering overruns
// (roughly 8:1 when measured), which points at bit sampling rather than a slow
// handler. It is invisible on PCSX-Redux, which models bytes rather than bits and
// so has no sampling clock to get wrong at any mode or baud.
constexpr uint16_t c_mode8N1 = (2 << 0) | (3 << 2) | (1 << 6);  // 0x4E

// The SIO clock: BitsPerSecond = 33868800 / ((Reload*Factor) AND NOT 1).
constexpr uint32_t c_sioClock = 33868800u;

// With MUL16 the divider is Reload*16, so the reload is 16x smaller and no longer
// divides evenly - it must be ROUNDED, not truncated. At 57600 the exact value is
// 36.75: truncating to 36 gives 58800 (+2.08%), rounding to 37 gives 57210
// (-0.68%), three times closer. A UART tolerates roughly +/-3% total across both
// ends before framing errors start, so the difference is not academic.
constexpr uint32_t baudReload(uint32_t baud) {
    // Round to nearest rather than toward zero.
    return (c_sioClock + baud * 8u) / (baud * 16u);
}

// Actual line rate for a reload, including the hardware's "AND NOT 1".
constexpr uint32_t actualBaud(uint32_t baud) {
    uint32_t divider = (baudReload(baud) * 16u) & ~1u;
    return divider ? (c_sioClock / divider) : 0;
}

// Error in parts per thousand, so the assert below can be read at a glance.
constexpr uint32_t baudErrorPerMille(uint32_t want) {
    uint32_t got = actualBaud(want);
    uint32_t diff = got > want ? got - want : want - got;
    return (diff * 1000u) / want;
}

// RX IRQ threshold. CTRL bits 8-9 select an IRQ when the RX FIFO holds 1, 2, 4
// or 8 bytes.
//
// THE RULE: the threshold trades interrupt COUNT against FIFO HEADROOM, and both
// ends of that trade are fatal.
//
//   * Headroom is 8 - threshold, and it is the only thing absorbing interrupt
//     latency. A threshold of 8 leaves none: the next byte is lost unless the
//     dispatcher has already run, and under the retail BIOS it has not.
//   * A threshold of 1 maximises headroom and ruins the CPU instead. At the
//     ~6.7 KB/s a ten-player fan-out produces that is ~6700 BIOS event dispatches
//     a second, which measures out at about 4 frames per second.
//
// 4 satisfies both: 4 bytes of headroom at a quarter of the interrupt rate that
// broke the frame rate. drainRx() empties the whole FIFO per call, so the real
// rate is lower still.
//
// How the wrong values PRESENT, since neither looks like a serial fault:
//   threshold 8 -> small messages work, large ones never arrive. A 158-byte frame
//                  spans ~20 latency windows and must survive all of them; a
//                  20-byte frame spans 3. STAT_OE climbs.
//   threshold 1 -> nothing is lost and everything is slow. ~4 fps.
//
// The static_asserts below tie threshold, baud and dispatcher together so this
// cannot be tuned in isolation: headroom has to be counted in MICROSECONDS and
// weighed against the dispatcher, not in bytes.
//
// INVARIANT: this also depends on the masked drain in poll(). SIO1 has no idle or
// timeout interrupt, so the trailing (len mod 4) bytes of a burst never raise one
// and poll()'s drain is what collects them. It is LOAD-BEARING: delete it and this
// must go back to 0, or messages strand mid-frame in RxState::Crc forever.
constexpr uint16_t c_rxIrqThreshold = 2;  // 0=1 byte, 1=2, 2=4, 3=8

// Bytes of FIFO left free when the interrupt fires. Asserted rather than
// commented because the relationship, not the number, is what keeps being lost.
constexpr uint16_t c_rxFifoDepth = 8;
constexpr uint16_t c_rxIrqLevel = (c_rxIrqThreshold == 0)   ? 1
                                  : (c_rxIrqThreshold == 1) ? 2
                                  : (c_rxIrqThreshold == 2) ? 4
                                                            : 8;
// Headroom in MICROSECONDS, the unit that matters: it has to outlast the retail
// BIOS's interrupt blackout. Counted in bytes instead, a threshold of 8 at 115200
// looks reasonable and leaves ~0us of usable slack.
constexpr uint32_t c_rxHeadroomBytes = c_rxFifoDepth - c_rxIrqLevel;
constexpr uint32_t c_bitsPerByte = 10;  // 8N1: start + 8 data + stop
constexpr uint32_t c_rxHeadroomMicros =
    (c_rxHeadroomBytes * c_bitsPerByte * 1000000u) / psxsplash::Sio1::c_defaultBaud;

// How much headroom is ENOUGH depends on who dispatches the interrupt, which is
// the whole reason the kernel is taken over. Under psyqo the handler is a direct
// assembly dispatch; under the BIOS it walks an event table with interrupts off.
constexpr uint32_t c_requiredHeadroomMicros = psxsplash::c_takeOverKernel ? 150 : 600;

static_assert(c_rxHeadroomMicros >= c_requiredHeadroomMicros,
              "RX FIFO headroom is too small for the interrupt dispatcher in use. Bytes "
              "will be dropped mid-frame, and it fails DECEPTIVELY: short frames span few "
              "blackout windows and keep working, so joins and small messages succeed "
              "while every long message is corrupted and retransmitted "
              "forever -- which looks like a game bug, not a link bug. Fix by LOWERING "
              "Sio1::c_defaultBaud, lowering c_rxIrqThreshold, or taking over the kernel "
              "(c_takeOverKernel) so the dispatcher stops being the bottleneck.");
static_assert(c_rxIrqLevel >= 2,
              "An RX IRQ threshold of 1 byte costs one BIOS event dispatch per byte, "
              "which at full rate measured out as a console running at 4 fps");

// The default baud must be one this hardware can actually generate accurately.
//
// A UART has roughly 3% of total budget across BOTH ends before the receiver's
// mid-bit sample drifts far enough to misread a stop bit, and the PC end is not
// necessarily exact either. 15 per mille keeps our half of that comfortably
// small. This is asserted rather than assumed because the failure is invisible in
// every emulator -- Redux models bytes, not bits, so it cannot reproduce a
// sampling error no matter how wrong the divider is.
static_assert(baudErrorPerMille(psxsplash::Sio1::c_defaultBaud) <= 15,
              "c_defaultBaud cannot be generated accurately enough by the SIO1 baud "
              "divider. The line rate will be off by more than 1.5%, which eats most of "
              "a UART's error budget on its own and shows up as framing errors that look "
              "exactly like a bad cable. Pick a baud whose divider lands closer.");
constexpr uint16_t c_rxIrqModeShift = 8;

// Bytes poll() will push per frame. THE UNIT IS BYTES, NOT SPIN ITERATIONS.
//
// Sending a byte costs something on both targets, but a different something, and
// bytes are the only unit they have in common:
//
//   REAL HARDWARE  a byte-time of wire (~174us at 57600), spent busy-waiting on
//                  the 2-byte FIFO. 48 bytes is ~8.3ms of a 33ms frame.
//
//   PCSX-REDUX     no wire time at all, but SIO1::writeData8 does a protobuf
//                  encode, two heap allocations, two lock-free queue pushes and
//                  two uv_async_send syscalls PER BYTE, synchronously on the
//                  thread emulating the CPU. Dearer than the PlayStation's UART.
//
// So this bounds host CPU on the emulator and wire time on hardware. Expressing it
// as a spin allowance instead removes the bound on Redux entirely, because a byte
// the transmitter accepts immediately consumes no spin - the pump then drains the
// whole 1 KiB ring every frame at ~20x the per-byte cost above.
//
// STAT_TXRDY CANNOT BE USED TO DISCOVER THIS. Redux sets it at reset and never
// clears it, so the emulator always reports ready. Readiness describes pacing,
// never cost. See pumpTx().
constexpr uint32_t c_txPollBytes = 48;

// The budget is spent PER FRAME but the demand it covers is WALL-CLOCK: the
// snapshot cadence is 30Hz by design (c_snapshotIntervalDt), decoupled from frame
// rate so a struggling renderer does not also desync the network. The margin
// between the two is the safety story - too little, and a frame-rate dip becomes a
// permanent outbound deficit with the ring growing until snapshots stop entirely.
constexpr uint32_t c_snapshotFramesPerSecond = 30;
constexpr uint32_t c_snapshotBytesPerSend = 35 + 11;  // one snapshot plus one ack
constexpr uint32_t c_wallClockDemandPerSecond = c_snapshotFramesPerSecond * c_snapshotBytesPerSend;

static_assert(c_txPollBytes * c_snapshotFramesPerSecond > c_wallClockDemandPerSecond,
              "The per-frame TX budget cannot carry the wall-clock send demand. Outbound "
              "bandwidth is then a function of frame rate while demand is not, so any dip puts "
              "the link into a deficit it never recovers from. Raise c_txPollBytes (costs host "
              "CPU on the emulator and wire time on hardware) or lower the demand -- shrinking "
              "the snapshot is the cheap direction and helps both targets at once.");

// Timeout for a single wait, after which the transmitter is presumed stalled and
// the pump gives up for this frame. Generous -- roughly two byte-times -- so a
// healthy UART never trips it.
constexpr uint32_t c_txSpinLimit = 4000;

// Consecutive interrupts that drain ZERO bytes, beyond which the handler is
// presumed to be running away and RX is demoted to polling.
//
// COUNTED THIS WAY BECAUSE THE OBVIOUS WAY IS WRONG, AND IT COST A CONSOLE
// MID-GAME. This was "RX interrupts between two poll() calls > 2048", justified
// like so: the wire carries 5760 B/s and the handler drains at least 4 bytes per
// entry, so no more than ~1440 interrupts can happen in a second, and reaching
// 2048 between two polls "requires either a poll gap beyond a second - a console
// already dead by any measure - or a handler re-entering without doing work".
//
// A POLL GAP BEYOND A SECOND IS A SCENE LOAD. poll() is reached from
// SceneManager::GameTick, and processPendingSceneLoad() -> loadScene() blocks that
// loop for SECONDS reading the disc, resetting the Lua VM and uploading VRAM,
// while the SIO1 interrupt keeps firing on inbound traffic the whole time (the
// server keeps sending for PEER_TIMEOUT_SECONDS = 20). The first poll() after the
// load then saw thousands of interrupts and demoted a perfectly healthy link.
//
// The demotion was permanent and catastrophic: polled RX drains eight bytes per
// frame, ~240 B/s against a link carrying 5760, so the console received ~4% of
// what was sent. rearmRx() early-returns unless the mode is Interrupt, so the
// safety net switched itself off too. Symptom: a game that plays, then "SLOW LINK"
// as the inbound queue grows past a second of delay, then a console that renders
// but never advances. Lobby -> game -> lobby -> game, dead in the second game.
//
// So measure the PATHOLOGY, not a proxy for it. A runaway is the handler
// re-entering without making progress - that is what takes the CPU and never gives
// it back. A scene load produces thousands of PRODUCTIVE entries, every one of
// which drains bytes; a runaway produces an unbroken run of empty ones. Counting
// consecutive zero-work entries has no time term and no poll term at all, so no
// length of gap can trip it.
//
// 256 is far past noise (an error reset can produce a couple) and is reached in
// well under a millisecond by a true runaway, which is what matters: the check now
// lives in the handler, because a handler that never returns is precisely the case
// where waiting for poll() to notice cannot work.
constexpr uint32_t c_idleIrqStormLimit = 256;

// Nothing acts on this count. See Sio1::noteRunaway() for why the driver no longer
// switches its own receive path off, and why an RX-only storm is bounded by the
// wire rather than by a threshold in software.

// Bytes drainRx() will take in one visit before concluding the RX flag is STUCK.
//
// THIS IS A WEDGE DETECTOR, NOT A RATE LIMIT, and confusing the two broke the
// emulator completely. It was "twice the 8-byte FIFO" = 16, reasoned from "a
// legitimate burst cannot exceed the FIFO". That is true in RxMode::Interrupt,
// where drainRx() runs per IRQ. It is false in RxMode::Polled, where drainRx()
// runs ONCE PER FRAME and must carry everything that arrived during the whole
// frame -- so 16 became a hard inbound ceiling of 16 bytes/frame, about 480 B/s at
// 30fps, against a fan-out of 3-6 KB/s. Under PCSX-Redux, which is always Polled,
// the console could not physically receive a long reliable message or a position update. It
// presented as "the emulators never get player updates" while the serial console,
// on Interrupt mode and a slower wire, worked.
//
// The correct bound is the only one that holds in BOTH modes: you can never
// usefully read more bytes in one visit than the ring can hold. Past that, either
// the ring is full and we are discarding anyway, or STAT_RXRDY is stuck -- which is
// exactly the condition worth detecting. Normal operation never approaches it,
// because the loop exits the moment RXRDY clears (after <=8 bytes on hardware).
//
// Derived from the ring so it cannot drift out of step with it.
constexpr uint32_t c_rxDrainLimit = psxsplash::Sio1::c_rxRingSize;

static_assert(c_rxDrainLimit > c_rxFifoDepth * 8,
              "c_rxDrainLimit has been sized to the hardware FIFO again. It is a STUCK-FLAG "
              "detector, not a rate limit: in RxMode::Polled this loop runs once per frame and "
              "carries the whole frame's inbound traffic, so a FIFO-sized bound silently becomes "
              "a receive ceiling. At 16 bytes that was ~480 B/s against a 3-6 KB/s fan-out, and "
              "the emulator could not receive a long message or a position update at all. Size it "
              "from the RING, which is the only quantity that bounds a useful drain in both "
              "modes.");

// Retail-kernel BIOS event for IRQ8 (SIO). Used ONLY on the fallback path now
// that c_takeOverKernel is on: queueIRQHandler() hard-asserts on
// `s_tookOverKernel` (psyqo kernel.cpp:189), so this route is what remains if the
// kernel is left with the BIOS. Class ids are from psx-spx "kernelbios.md":
// F000000Bh is IRQ8/SIO.
constexpr uint32_t c_eventClassSio = 0xF000000Bu;

// Spec 1000h, NOT EvSpINT (0002h).
//
// This one constant is why no real PlayStation ever got past "CONNECTING TO
// SERVER". The retail BIOS's default IRQ handler delivers hardware interrupts as
// (class, spec) = (F000000Bh, 1000h) - psx-spx "Default IRQ Handler Events" lists
// the spec for every IRQ as 1000h. DeliverEvent matches on the PAIR, so an event
// opened with 0002h never matches and the callback is never invoked.
//
// EvSpINT is a real constant, just from the event-spec table rather than the IRQ
// dispatcher's, which is what made the mistake plausible. Every other openEvent
// in this tree passes 1000h (psyqo kernel.cpp, cdrom-device.cpp, memory-card.cpp)
// - sio1 was the only exception.
//
// The consequences were both fatal and mutually masking: drainRx() is only called
// from this handler in Interrupt mode, so RX was dead; and the IRQ was never
// acknowledged, so with IRQ8 unmasked the CPU re-entered the exception vector
// forever (cop0r13.bit10 is not a latch - it re-asserts while I_STAT & I_MASK is
// non-zero). The console froze on whatever frame was last drawn, which is exactly
// why the screen sat on "connecting" and no diagnostic ever appeared.
constexpr uint32_t c_eventSpecHwIrq = 0x1000u;
constexpr uint32_t c_eventModeCallback = 0x1000u;   // EvMdINTR - invoke the handler

}  // namespace

namespace psxsplash {

Sio1& Sio1::Get() { return s_instance; }

void Sio1::init(uint32_t baud, RxMode rxMode) {
    // Internal reset: clears the SIO1 block and its FIFOs.
    Ctrl = SIObits::Control::CTRL_IR;
    Ctrl = 0;

    Mode = c_mode8N1;

    // MUL16, so the divider is Reload*16 and the reload must be ROUNDED - see
    // baudReload(). Truncating here is a 2% error where rounding is 0.7%, and a
    // UART only has about 3% to spend across both ends.
    uint32_t reload = baud ? baudReload(baud) : 1;
    if (reload == 0) reload = 1;
    if (reload > 0xFFFF) reload = 0xFFFF;
    Baud = static_cast<uint16_t>(reload);
    m_baud = baud;

    // Resolve Auto. The two targets want opposite settings - Redux corrupts its
    // own receive buffer if we enable the RX IRQ, real hardware silently drops
    // ~96% of a burst if we don't - so guessing wrong is expensive either way.
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
    // Cached because CTRL cannot be read back for a value: bit 4 is a write-only
    // strobe, so `Ctrl = Ctrl | ACK` re-writes whatever the read returned. Every
    // write goes through m_ctrl, and the TX IRQ bit is toggled in it at runtime.
    m_ctrl = SIObits::Control::CTRL_TXEN | SIObits::Control::CTRL_RXE | SIObits::Control::CTRL_DTR |
             SIObits::Control::CTRL_RTS;

    if (rxMode == RxMode::Interrupt) {
        m_ctrl |= SIObits::Control::CTRL_RXIRQEN | (c_rxIrqThreshold << c_rxIrqModeShift);
        // CTRL_TXIRQEN is NEVER set. It is level-triggered on "FIFO has room",
        // which at this baud re-asserts faster than the BIOS can dispatch, so the
        // console never leaves the exception vector. See pumpTx().
    }
    Ctrl = m_ctrl;

    m_rx.clear();
    m_tx.clear();

    m_idleIrqRun = 0;

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

    if (psyqo::Kernel::isKernelTakenOver()) {
        // The fast path, and the reason the kernel is taken over at all: psyqo's
        // exception handler dispatches straight to this from a per-IRQ table,
        // instead of the BIOS walking its event list with interrupts disabled.
        // That blackout is what was overrunning the 8-byte RX FIFO - see
        // c_takeOverKernel. Kernel::IRQ::SIO is IRQ8, the same line.
        psyqo::Kernel::queueIRQHandler(psyqo::Kernel::IRQ::SIO, []() { Sio1::Get().handleRxIrq(); });
    } else {
        // BIOS fallback. Works, but with the latency that made long frames
        // unreliable on real hardware; kept so c_takeOverKernel can be flipped
        // back without this file changing.
        uint32_t event = psyqo::Kernel::openEvent(c_eventClassSio, c_eventSpecHwIrq, c_eventModeCallback,
                                                  []() { Sio1::Get().handleRxIrq(); });
        syscall_enableEvent(event);
    }
    m_irqInstalled = true;
}

void Sio1::handleRxIrq() {
    m_rxIrqs++;

    // Runaway detection lives HERE, not in poll(). A handler that re-asserts
    // faster than it returns never lets poll() run again, so a detector poll()
    // owns is one the failure disables. See c_idleIrqStormLimit.
    //
    // PROGRESS IS BYTES TAKEN OFF THE HARDWARE FIFO, NOT BYTES THE RING ACCEPTED,
    // and the difference is not academic - reading m_bytesRx alone here demoted a
    // console within ten seconds of gameplay. drainRx() increments m_bytesRx only
    // on a successful m_rx.push() and m_rxOverflow otherwise, so once the ring is
    // full every interrupt looks like it did nothing. The ring fills during exactly
    // the event this detector must tolerate: a scene load, where nothing drains it
    // for seconds. That reintroduced the false demotion through a different door
    // and hit it SOONER, because the 4 KiB ring fills in ~1024 interrupts where the
    // old count needed 2048.
    //
    // The sum is the right signal because the pathology is an EMPTY FIFO: a
    // runaway re-enters with nothing to read. A byte read and then dropped is still
    // a byte the handler was right to be woken for.
    const uint32_t before = m_bytesRx + m_rxOverflow;
    drainRx();
    if (m_bytesRx + m_rxOverflow == before) {
        if (++m_idleIrqRun >= c_idleIrqStormLimit) noteRunaway();
    } else {
        m_idleIrqRun = 0;  // progress: whatever this was, it was not a runaway
    }

    // TX is serviced here too: IRQ8 is shared, and pumping from the handler is
    // what lets the ring drain at wire speed. Doing it only from poll() capped the
    // console at two bytes per frame.
    pumpTx();

    // Acknowledge. psx-spx ("Interrupt Acknowledge Notes") is explicit about the
    // order: reset I_STAT.8 FIRST, then set CTRL.4 - doing it the other way round
    // can miss an IRQ that arrives in between.
    //
    // ackIrq(), NOT IReg.clear(). The comment here used to say clear() "writes 0 to
    // the SIO bit and 1s elsewhere, leaving other sources up" - which is what it
    // INTENDS and not what it does. It is a read-modify-write, so it writes back
    // whatever it read, and any bit not yet pending at read time is written as 0,
    // i.e. acknowledged. At ~1440 interrupts a second this was a steady source of
    // destroyed CD-ROM completions. See irqack.hh.
    ackIrq(psyqo::Hardware::CPU::IRQ::SIO);
    Ctrl = m_ctrl | SIObits::Control::CTRL_ERRRES;  // CTRL.4 doubles as the SIO IRQ ack
}

// Producer side of m_rx. Runs on the main thread in RxMode::Polled and from the
// SIO IRQ in RxMode::Interrupt - never both, since poll() skips it when
// interrupts own the FIFO. That single-producer guarantee is what makes the
// lock-free RingBuffer safe here (see ringbuffer.hh): read() is the only
// consumer and always runs on the main thread. The stats counters below are
// plain increments and can race benignly with resetStats(); they are
// diagnostics, never control flow.
void Sio1::drainRx() {
    // Clear any error condition first, or the RX path can wedge.
    //
    // THE THREE ERRORS ARE COUNTED SEPARATELY because they are unrelated faults
    // with opposite fixes, and one lumped counter labelled "OVERRUN" reports a
    // diagnosis rather than a measurement:
    //
    //   OE (overrun)  the 8-byte FIFO filled before we drained it. Ours: interrupt
    //                 latency, or a blackout elsewhere in the system. Fix with
    //                 headroom - lower baud, lower threshold.
    //   FE (framing)  a stop bit arrived where it should not have. Not latency at
    //                 all: the two ends disagree about bit timing, or the signal is
    //                 marginal. Cable, adapter, grounding, or a baud the peer only
    //                 approximates. Worsens with baud, which is also how a latency
    //                 problem looks - hence the split.
    //   PE (parity)   should be impossible, since c_mode8N1 disables parity. Any
    //                 non-zero value means MODE is not what this driver thinks.
    uint32_t stat = Stat;
    if (stat & (SIObits::Status::STAT_OE | SIObits::Status::STAT_FE | SIObits::Status::STAT_PE)) {
        if (stat & SIObits::Status::STAT_OE) m_rxOverrunErrors++;
        if (stat & SIObits::Status::STAT_FE) m_rxFramingErrors++;
        if (stat & SIObits::Status::STAT_PE) m_rxParityErrors++;
        m_serialErrors++;
        Ctrl = m_ctrl | SIObits::Control::CTRL_ERRRES;
    }

    // BOUNDED. Never spin on a hardware flag in an interrupt handler without a
    // limit: an overrun can leave STAT_RXRDY asserted, and an unbounded loop then
    // takes the CPU and never returns it. The console freezes on the last drawn
    // frame with no way to report what happened.
    //
    // The bound is the RING size, not the FIFO size - see c_rxDrainLimit. In Polled
    // mode this loop carries a whole frame's inbound traffic, so a guard sized to
    // the hardware FIFO becomes a receive rate limit instead of a wedge detector.
    uint32_t guard = c_rxDrainLimit;
    while ((Stat & SIObits::Status::STAT_RXRDY) && guard--) {
        uint8_t b = Data;
        if (m_rx.push(b)) {
            m_bytesRx++;
        } else {
            m_rxOverflow++;  // ring full: drop the byte, keep the link alive
        }
    }
    if (Stat & SIObits::Status::STAT_RXRDY) {
        // Still claiming data after twice the FIFO depth. Force an error reset and
        // count it; a wedged receiver is worth reporting rather than hanging on.
        m_rxDrainOverruns++;
        Ctrl = m_ctrl | SIObits::Control::CTRL_ERRRES;
    }

    // Sampled once per drain rather than per byte: the peak is what matters and
    // this runs in the ISR. Reaching the ring size means loss has already begun;
    // watching it climb toward it is the warning that rxOverflows() cannot give.
    uint32_t depth = m_rx.size();
    if (depth > m_rxHighWater) m_rxHighWater = depth;
}

// Consumer side of m_tx. Like drainRx, this must have exactly ONE caller at a
// time: poll() and kickTx() mask IRQ8 around it so it cannot race the handler.
//
// Writes at most TWO bytes per call - SIO1's transmit path is a holding register
// plus a shift register, so STAT_TXRDY clears after two and stays clear until one
// is clocked out. It does NOT wait; see pumpTxBlocking for the caller that does.
//
// THE TX INTERRUPT IS DELIBERATELY NOT USED. SIO1 raises it on a LEVEL ("the FIFO
// has room"), and with a 2-byte FIFO at 57600 that level re-asserts ~174us after
// each byte begins shifting - while the retail BIOS needs longer than that just to
// dispatch the handler. The next interrupt is therefore already pending before the
// previous one returns, and the CPU never leaves the exception vector while
// anything is queued. Enabling it froze a real console: sprite animation stopped,
// the player would not move, and RX overruns climbed to 433 because the CPU was
// trapped in TX interrupts instead of draining the RX FIFO.
//
// So TX is driven from two places that cost no interrupts of their own:
//   - handleRxIrq(), which is already running. Inbound traffic generates hundreds
//     of interrupts a second and each can carry two outbound bytes, so TX capacity
//     comes free exactly when it is needed most (talking while being talked to).
//   - poll(), once per frame, which will WAIT for the FIFO (pumpTxBlocking) so a
//     quiet inbound link cannot starve the outbound one.
//
// STAT_TXRDY IS NOT A COST SIGNAL, and must never be used as one. Under PCSX-Redux
// it is set at reset and never cleared again -- SR_TXRDY|SR_TXRDY2 are written in
// exactly two places, SIO1::reset() and the CR_RESET branch of writeCtrl16
// (pcsx-redux/src/core/sio1.h:89, sio1.cc:275) -- so the emulator always reports
// ready. That is true and it is also misleading: sending is still expensive there,
// just not WIRE-paced (see c_txPollBytes for what each byte actually costs Redux).
//
// Reading "always ready" as "free" is what removed the per-frame byte budget and
// made the emulator unusable. A transport can be instantly ready and still cost
// dearly to feed; readiness describes pacing, never cost.
void Sio1::pumpTx() {
    uint8_t b;
    uint32_t depth = m_tx.size();
    if (depth > m_txHighWater) m_txHighWater = depth;
    // Enter the body only when the TX FIFO can accept a byte AND we have one to
    // send. The short-circuit guarantees we never pop a byte we can't write.
    while ((Stat & SIObits::Status::STAT_TXRDY) && m_tx.pop(b)) {
        Data = b;
        m_bytesTx++;
    }
}

// Push up to c_txPollBytes, waiting for the FIFO between them. See c_txPollBytes
// for why the budget is counted in bytes, and what removing it broke.
//
// Waiting is cheaper than interrupting here, which is the whole point. A wait costs
// one byte of wire time (~174us at 57600) and nothing else; an interrupt costs that
// PLUS a BIOS dispatch, which on this machine is the larger of the two. The budget
// is bounded, so the worst case stays arithmetic rather than a possibility.
//
// CALLERS MASK IRQ8 AROUND THIS (see poll()), so the RX interrupt cannot run for
// the duration. An earlier version of this comment claimed the opposite -- "it
// does not block interrupts, RX is still serviced throughout" -- and that false
// reassurance is why a transmit path was allowed to sit with receive disabled for
// 8ms a frame, shredding the 8-byte RX FIFO, for as long as it did. RX is serviced
// here only because the spin below explicitly drains it.
void Sio1::pumpTxBlocking() {
    uint8_t b;
    uint32_t depth = m_tx.size();
    if (depth > m_txHighWater) m_txHighWater = depth;

    for (uint32_t sent = 0; sent < c_txPollBytes; ++sent) {
        if (m_tx.empty()) return;
        // Bounded wait for room. Giving up is correct: the rest goes next frame,
        // where an unbounded spin would hand a stalled UART the whole console.
        //
        // SERVICE RX WHILE WAITING. Not an optimisation: without it this loop
        // destroys inbound traffic.
        //
        // poll() masks IRQ8 around this call, because popping m_tx and writing DATA
        // must not race the interrupt handler doing the same. That is correct, but
        // it disables the RX interrupt for the whole wait - up to 48 bytes of wire
        // time, ~8.3ms at 57600. Inbound bytes keep arriving every ~174us into an
        // 8-byte FIFO, which therefore fills in 1.4ms; everything after that is
        // lost. Roughly forty bytes a frame, destroyed by the transmit path.
        //
        // Draining here is race-free precisely BECAUSE we are masked: this thread
        // owns the FIFO for the duration, so nothing else can service it. An idle
        // drainRx() is two register reads, against a byte-time already being spent.
        uint32_t spin = c_txSpinLimit;
        while (!(Stat & SIObits::Status::STAT_TXRDY) && spin) {
            drainRx();
            --spin;
        }
        if (!(Stat & SIObits::Status::STAT_TXRDY)) {
            m_txSpinTimeouts++;
            return;
        }
        if (!m_tx.pop(b)) return;
        Data = b;
        m_bytesTx++;
    }
}

// See the declaration in sio1.hh for the whole mechanism: an unrelated driver's
// read-modify-write on I_STAT can acknowledge our pending interrupt, and because
// SIO_STAT.9 is sticky while I_STAT.8 is edge-triggered, nothing ever re-raises it.
//
// The order below is the one psx-spx prescribes and it is NOT interchangeable:
// I_STAT first, then the I/O port. Doing it the other way round can lose an
// interrupt that arrives in between, which is the very failure this exists to undo.
void Sio1::rearmRxLocked() {
    // Anything still in the FIFO is proof the interrupt did not do its job: at the
    // configured threshold the handler should already have taken it. Sampled BEFORE
    // the drain, because the drain is what removes the evidence.
    const bool stranded = (Stat & SIObits::Status::STAT_RXRDY) != 0;

    drainRx();

    // ackIrq(), NOT IReg.clear(), and THIS site is the dangerous one. It runs on the
    // MAIN THREAD with interrupts live - once from poll() and once from each of the
    // pad-poll bookends, so about three times a frame - and only IRQ8 is masked.
    // Masking IRQ8 stops IRQ8 being DISPATCHED; it does nothing to stop the CD-ROM
    // setting its own I_STAT bit in the middle of a read-modify-write. Every one of
    // those windows could swallow a CD-ROM completion, and a psyqo CDRomDevice
    // operation whose IRQ never arrives never finishes. See irqack.hh.
    ackIrq(psyqo::Hardware::CPU::IRQ::SIO);
    Ctrl = m_ctrl | SIObits::Control::CTRL_ERRRES;

    // Counted after the fact rather than gated on: the re-arm is unconditional and
    // costs one register write, so there is nothing to be saved by testing first -
    // and a test would only add another window in which the state could change.
    if (stranded) m_rxRevivals++;
}

void Sio1::rearmRx() {
    if (!m_initialized || m_rxMode != RxMode::Interrupt) return;

    // MASKED, for the same reason poll()'s drain is: drainRx() is the producer for
    // m_rx and the only reader of the hardware FIFO, and the interrupt handler does
    // both of those too. Two producers on a single-producer ring is corruption, not
    // a slow path. Callers already inside poll()'s masked region use
    // rearmRxLocked() directly instead, so the mask is never nested.
    //
    // Note this does NOT pump TX. An earlier version of the pad bracket called
    // poll() here, which drags in pumpTxBlocking and its 48-byte, up-to-8.3ms
    // budget - a second full transmit pass per frame, from inside GPU::flip. This
    // is two register reads and a drain.
    psyqo::Hardware::CPU::IMask.clear(psyqo::Hardware::CPU::IRQ::SIO);
    rearmRxLocked();
    psyqo::Hardware::CPU::IMask.set(psyqo::Hardware::CPU::IRQ::SIO);
}

void Sio1::poll() {
    if (!m_initialized) return;

    // Reaching here is proof the console is not trapped in the exception vector, so
    // the zero-work run starts over. A runaway is defined by the main loop never
    // running again; if poll() is executing there is nothing to report, whatever the
    // counters happen to say.
    m_idleIrqRun = 0;

    if (m_rxMode == RxMode::Polled) {
        drainRx();
    } else {
        // REQUIRED, not optional - see c_rxIrqThreshold. With a threshold of 8
        // the trailing (len mod 8) bytes of a burst never raise an interrupt, and
        // this is the only thing that comes back for them. Removing it strands the
        // end of every message that is not a multiple of 8 bytes long.
        //
        // In Interrupt mode the IRQ owns the RX FIFO, and draining from here would
        // race the handler for the same hardware register - so this masks IRQ8 for
        // the duration, making the drain mutually exclusive with the handler
        // rather than concurrent with it.
        //
        // It doubles as the failure floor: when the IRQ silently never fired (see
        // the event-spec note above), NOTHING drained RX and the console hung with
        // no diagnostic. With this, any future interrupt problem degrades to
        // polled behaviour - slower, and lossy across long stalls, but alive and
        // debuggable.
        //
        // The TX pump is inside the mask for the same reason: it pops m_tx and
        // writes DATA, both of which the handler also does. Two consumers on a
        // single-consumer ring is corruption, not a slow path.
        //
        // No storm check here any more: the handler demotes itself the moment it
        // detects a runaway (see c_idleIrqStormLimit), because a handler that never
        // returns is exactly the case where a check owned by poll() never runs.
        psyqo::Hardware::CPU::IMask.clear(psyqo::Hardware::CPU::IRQ::SIO);
        drainRx();
        pumpTxBlocking();
        // The outer safety net. It re-creates the interrupt's edge, which is the
        // only way back once an unrelated driver's I_STAT read-modify-write has
        // acknowledged our pending IRQ - see rearmRx() for why that is permanent
        // without it. The tight net is in Controls::ShieldPadPollEnd, immediately
        // after the code that causes it; this one catches everything else, and
        // costs one register write a frame.
        //
        // The _locked form because IRQ8 is already masked here; the public one
        // would restore the mask early and leave the pump below unprotected.
        rearmRxLocked();
        psyqo::Hardware::CPU::IMask.set(psyqo::Hardware::CPU::IRQ::SIO);
        return;
    }
    pumpTxBlocking();
}

// THE DRIVER NO LONGER SWITCHES ITS OWN RECEIVE PATH OFF. This records a runaway
// so it is visible on screen; it changes nothing.
//
// There used to be a demotion here: past a threshold, disable CTRL_RXIRQEN and fall
// back to polled RX, described as a last-resort guarantee that the serial driver
// could never hang the console. It shipped two false positives with catastrophic
// effect, because polled RX ON REAL HARDWARE IS NOT A DEGRADED LINK, IT IS A DEAD
// ONE: it drains the 8-byte FIFO once per frame, ~240 B/s against a wire carrying
// 5760, so the console receives about 4% of what is sent and every long message is
// destroyed. There is no console on which demoting is the better outcome, which
// makes "when should it trip" the wrong question - the honest answer is never.
//
// AND THE STORM IT GUARDED AGAINST CANNOT HAPPEN. It was written for the TX
// interrupt, which genuinely did trap the CPU in the exception vector - and
// CTRL_TXIRQEN has not been set since; the only mention of it in this file is the
// comment in init() saying it never will be. What remains is RX, and RX is bounded
// by the WIRE: at 57600 the link delivers 5760 B/s and the handler drains at least
// c_rxIrqLevel bytes per entry, so the hardware itself caps this at ~1440
// interrupts a second no matter what happens in software. At the retail BIOS's
// dispatch cost that is under a fifth of the CPU. It physically cannot take the
// machine.
//
// The two things that CAN wedge the receiver are handled where they occur, without
// a mode switch: drainRx()'s loop is bounded by c_rxDrainLimit and force-resets a
// stuck STAT_RXRDY (m_rxDrainOverruns), and the handler acknowledges on every entry
// so a pending IRQ is always cleared.
//
// This follows the same rule that fixed the snapshot queue in netlink.hh: remove
// the mechanism rather than tune the threshold, so there is no value of any
// constant that brings the bug back.
//
// CALLED FROM THE INTERRUPT HANDLER. IRQ8 is not running concurrently with itself,
// so the counter needs no protection.
void Sio1::noteRunaway() {
    m_irqStorms++;
    m_idleIrqRun = 0;
}

// Give the TX engine a nudge after queueing, so a short message does not sit in
// the ring until the next frame. Non-blocking: this runs from the game's send
// path, which must not be charged a byte of wire time.
void Sio1::kickTx() {
    if (!m_initialized) return;
    if (m_rxMode == RxMode::Interrupt) {
        psyqo::Hardware::CPU::IMask.clear(psyqo::Hardware::CPU::IRQ::SIO);
        pumpTx();
        psyqo::Hardware::CPU::IMask.set(psyqo::Hardware::CPU::IRQ::SIO);
    } else {
        pumpTx();
    }
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
    kickTx();  // opportunistic immediate flush so short messages leave promptly
    return n;
}

bool Sio1::writeAll(const uint8_t* src, uint32_t len) {
    // All or nothing. A partial write is WORSE than a refused one: the prefix is
    // still transmitted, so the peer reads a length header that runs off the end
    // of the truncated frame, consumes the START of the next frame as this one's
    // payload, fails CRC, and resyncs - one overflow destroys TWO frames.
    //
    // The caller can retry a refusal. Nobody can un-send half a packet.
    if (m_tx.space() < len) {
        m_txRejected++;
        kickTx();  // make room for the caller's next attempt
        return false;
    }
    for (uint32_t i = 0; i < len; i++) m_tx.push(src[i]);
    kickTx();
    return true;
}

bool Sio1::writeByte(uint8_t b) {
    bool ok = m_tx.push(b);
    kickTx();
    return ok;
}

}  // namespace psxsplash
