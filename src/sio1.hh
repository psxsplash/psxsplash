#pragma once

#include <stdint.h>

#include "inettransport.hh"
#include "ringbuffer.hh"

namespace psxsplash {

/// Whether main() hands the exception vector to psyqo instead of leaving it with
/// the retail BIOS. One flag, referenced by everything that depends on it.
///
/// It exists for SIO1 latency. Under the BIOS every interrupt (VSync, each CD
/// sector, each controller ACK) is dispatched by the kernel's handler, which walks
/// an event table with interrupts DISABLED. The RX FIFO is 8 bytes, so at 115200
/// that blackout only has to last ~700us to lose a byte. psyqo's handler is a
/// direct assembly dispatch to a per-IRQ table and cuts it by an order of
/// magnitude.
///
/// The loss it causes is shaped like a logic bug rather than a link one: short
/// frames span few blackout windows and keep working, so joins and small messages
/// succeed while every long message is corrupted and retransmitted forever.
///
/// COST: takeOverKernel() memsets the first 4KB of RAM and replaces the A0/B0/C0
/// syscall vectors, so BIOS services are gone afterwards. psxsplash can afford
/// that because it already uses psyqo's own drivers (CDRomDevice + ISO9660Parser,
/// AdvancedPad, MemoryCard), each of which branches on isKernelTakenOver().
///
/// CURRENTLY OFF: enabling it faults at boot on the LOADER=cdrom build, before a
/// frame is drawn, with `ReservedInstruction from 0x00005704`. That address is
/// BIOS kernel space the 4KB memset does not reach, so it is not wiped memory
/// being executed - something transferred INTO kernel code and landed
/// off-instruction, which points at a jump through a stale pointer rather than a
/// missing service.
///
/// Ruled out by inspection, so they are not re-checked:
///   - psyqo's assembly exception handler is linked and survives --gc-sections.
///   - Initializer order is correct: Kernel::Internal::prepare() runs the takeover
///     initializer before Application::prepare(), so every driver sees
///     isKernelTakenOver() == true when it picks its IRQ path.
///   - Not PCdrv, which is incompatible but absent from this build: the Makefile
///     defines PCDRV_SUPPORT only in the non-cdrom branch.
///   - Not a stale instruction cache: Kernel::flushCache() is psyqo's own rather
///     than the A0 syscall, so it still works after the vectors are replaced.
///   - Not a stray BIOS syscall: takeOverKernel's replacement stubs redirect
///     printf and answer everything else with `jr ra`, so a post-takeover syscall
///     returns doing nothing and cannot fault.
///
/// To diagnose, build with Kernel::installCrashHandler() (see main.cpp) and read
/// the register holding 0x5704 off the screen; on a retail console that is the
/// only channel there is.
///
/// This is unfinished work, not abandoned work. Taking over the kernel is a
/// BANDWIDTH win - it would let c_defaultBaud go back up - and is not required for
/// correctness, because halving the baud gives the FIFO the same margin by making
/// bytes arrive half as often.
inline constexpr bool c_takeOverKernel = false;

/**
 * Bare-metal driver for the PlayStation's second serial port, SIO1
 * (registers at 0x1F801050..0x1F80105E) - the "serial port" / link-cable port,
 * distinct from SIO0 which drives controllers and memory cards.
 *
 * PSYQo has no SIO1 support, so this is written directly against the hardware
 * registers, modelled on the raw SIO0 access in controls.cpp. SIO1 is an
 * asynchronous UART; we run it 8N1 at a configurable baud (115200 by default).
 *
 * The public surface is the non-blocking INetTransport interface plus init().
 * Received bytes land in an RX ring buffer and queued bytes drain from a TX ring
 * buffer; the rings are the only thing callers ever see, which is what lets the
 * two RX strategies below be swapped without anything above noticing.
 *
 *
 * RX STRATEGY, and why it differs per target
 * ------------------------------------------
 * The hardware RX FIFO is 8 bytes deep and does NOT auto-deassert RTS when it
 * fills: the 9th byte overwrites the last entry and sets STAT_OE (psx-spx,
 * "SIO_RX_DATA Notes"). At 57600 a byte lands every ~174us, so a burst overruns
 * the FIFO ~1.4ms in. A once-per-frame poll is 16.6ms apart and cannot keep up.
 *
 *   RxMode::Interrupt  required on real hardware. Drains the FIFO from the SIO
 *                      IRQ, so the 8-byte window is honoured.
 *   RxMode::Polled     required under PCSX-Redux, and the default.
 *
 * Polled is not a fallback, it is the correct setting for the emulator. Redux's
 * SIO1::interrupt() contains:
 *
 *     if (m_sio1fifo.isA<Fifo>()) {
 *         if (m_sio1fifo->size() > 8) m_sio1fifo.asA<Fifo>()->reset();
 *     }
 *
 * which discards the entire receive buffer, not one byte, whenever more than 8
 * are queued as the IRQ fires. Nothing schedules that interrupt unless the game
 * sets CTRL_RXIRQEN, so arming it on Redux would destroy inbound snapshots that
 * polled mode receives perfectly.
 *
 * RxMode::Auto resolves this at runtime via pcsx_present(); there is no build
 * flag and there should not be one.
 *
 *
 * WHAT THE EMULATOR DOES NOT MODEL
 * --------------------------------
 * Redux models bytes, not bits. There is no sampling clock, so MODE and BAUD
 * cannot produce a framing error there however wrong they are (see c_mode8N1).
 *
 * It also does not pace writes by baud: calcCycleCount() feeds only a scheduled
 * interrupt, which Polled mode never enables. Sending is not free there even so -
 * each byte costs a protobuf encode, two heap allocations and two thread wakeups
 * on the CPU-emulation thread. Redux is cheaper per byte of WIRE time and dearer
 * per byte of HOST CPU.
 *
 * So a green result on the emulator says nothing about timing, framing or FIFO
 * behaviour, and a fast one says nothing about bandwidth in either direction.
 */
class Sio1 final : public INetTransport {
  public:
    /// The link's bit rate, and the ONLY place this number lives.
    ///
    /// Both ends must agree, and a mismatch is indistinguishable from an unplugged
    /// cable: no error, no counter, just a dead link. Anything that needs the value
    /// reads it from here, and the connecting screen prints it so it can be
    /// compared with the bridge's setting.
    ///
    /// What limits it is not the cable but how long the console can go without
    /// servicing an 8-byte RX FIFO. At 115200 a byte lands every 87us, so the
    /// headroom left by c_rxIrqThreshold is ~350us, which the retail BIOS's
    /// interrupt dispatcher does not reliably beat. Halving the rate doubles it.
    ///
    /// Raise this to 115200 only together with c_takeOverKernel. The static_asserts
    /// in sio1.cpp enforce that pairing and refuse to build a combination that
    /// cannot keep up.
    static constexpr uint32_t c_defaultBaud = 57600;

    /// Ring sizes. The driver derives its bounds from these rather than restating
    /// them; in particular drainRx()'s stuck-flag guard is c_rxRingSize, because a
    /// guard sized to the hardware FIFO instead becomes a per-frame receive cap.
    ///
    /// RX is 4 KiB because it has to span the worst frame, not the average one.
    /// poll() drains it once per rendered frame, so its real unit is TIME: at the
    /// ~6.4 KB/s a ten-player fan-out produces, 1 KiB covers only ~160ms, and any
    /// hitch longer than that overflows. A hole mid-frame fails CRC and forces a
    /// resync, so the largest message on the wire is also the most likely to be
    /// destroyed. 4 KiB buys ~640ms and costs 3 KiB of 2 MiB.
    ///
    /// TX stays 1 KiB: it is paced by our own send cadence rather than the peer's,
    /// so it never absorbs an unannounced burst.
    static constexpr uint32_t c_rxRingSize = 4096;
    static constexpr uint32_t c_txRingSize = 1024;

    // How received bytes get moved out of the hardware FIFO. See the class
    // comment - this is a correctness choice per target, not a tuning knob.
    enum class RxMode : uint8_t {
        Auto,       // Polled under PCSX-Redux, Interrupt on real hardware.
        Polled,     // drained by poll(), once per frame. Emulator-safe.
        Interrupt,  // drained by the SIO IRQ. Required on real hardware.
    };

    // Singleton accessor (namespace-scope instance; constructed at static-init
    // like PSYQo's own global objects).
    static Sio1& Get();

    // Reset and configure the SIO1 block: 8N1, MUL16 baud factor, TX+RX enabled.
    // Safe to call again to change baud or RX mode.
    //
    // RxMode::Auto is the right answer on both targets and needs no build flag:
    // the two targets want opposite settings, and the console can tell them
    // apart at runtime. Pass an explicit mode only to override that.
    void init(uint32_t baud = c_defaultBaud, RxMode rxMode = RxMode::Auto);
    bool isInitialized() const { return m_initialized; }
    // The resolved mode - never returns Auto once init() has run.
    RxMode rxMode() const { return m_rxMode; }

    // --- INetTransport ---
    void poll() override;
    uint32_t available() const override { return m_rx.size(); }
    uint32_t read(uint8_t* dst, uint32_t max) override;
    uint32_t write(const uint8_t* src, uint32_t len) override;

    // Convenience: queue a single byte. Returns false if the TX ring is full.
    bool writeByte(uint8_t b);

    /// Drain the FIFO and force the RX interrupt to re-arm itself.
    ///
    /// THE RECEIVE INTERRUPT CAN BE DESTROYED BY CODE THAT HAS NOTHING TO DO WITH
    /// THIS DRIVER, AND WITHOUT THIS IT NEVER COMES BACK.
    ///
    /// I_STAT is acknowledge-by-writing-zero, so psyqo implements `IReg.clear()`
    /// as a read-modify-write (hwregs.hh, operator&=). Any code that clears a
    /// DIFFERENT interrupt therefore writes back whatever it read for every other
    /// bit - and a zero in I_STAT.8 means "acknowledge". So:
    ///
    ///     tmp = I_STAT          bit 8 = 0, SIO1 not yet pending
    ///        <-- SIO1 asserts: hardware sets I_STAT.8
    ///     tmp &= ~(1 << 7)
    ///     I_STAT = tmp          bit 8 written as 0 -> our pending IRQ is GONE
    ///
    /// psyqo::AdvancedPad::transceive() does exactly that before EVERY pad byte,
    /// roughly eighteen times a frame across two pads, from the per-frame callback.
    /// It is upstream code and correct on its own terms; nothing there is wrong.
    ///
    /// What makes it fatal is that SIO_STAT.9 is STICKY and I_STAT.8 is
    /// EDGE-TRIGGERED (psx-spx, serialinterfacessio.md and interrupts.md). Once
    /// I_STAT.8 is cleared while SIO_STAT.9 is still asserted there is no further
    /// edge, and psx-spx states the consequence outright for the identical SIO0
    /// case: "I_STAT.7 won't be ever set in future". The receive interrupt is dead
    /// for the rest of the session, RX falls back to the once-per-frame drain -
    /// eight bytes a frame against a link delivering ninety-six - and every long
    /// message is destroyed from then on.
    ///
    /// The window is a few cycles wide and hit ~18 times a frame against ~24
    /// interrupts a frame: about a 0.2% chance per frame, i.e. once every seven
    /// seconds or so. That is the observed spread of failures (1s, 5s, three tasks,
    /// during a lobby join) and it is why traffic volume never correlated.
    ///
    /// THE CURE IS IN THE SAME REFERENCE. psx-spx, "Interrupt Acknowledge Notes":
    /// acknowledging via SIO_CTRL.4 while the enabled condition is still true makes
    /// "the IRQ trigger again (almost) immediately ... barely enough to allow
    /// I_STAT.8 to sense a edge". So the edge can always be recreated on demand.
    /// This does that, unconditionally, and is therefore idempotent: calling it
    /// when nothing is wrong costs one register write.
    ///
    /// Call it AFTER anything that clears an unrelated interrupt - see
    /// Controls::ShieldPadPollEnd - and once a frame as a backstop.
    void rearmRx();

    /// Times rearmRx() found a byte waiting that the interrupt should already have
    /// collected, i.e. caught the receive interrupt dead and revived it.
    ///
    /// Zero on a console where the race never fired. Non-zero and climbing while
    /// the game stays playable is this bug being repaired in flight, which is the
    /// stronger evidence that the diagnosis was right.
    uint32_t rxRevivals() const { return m_rxRevivals; }

    /// All-or-nothing write. Use this for anything FRAMED.
    ///
    /// `write()` above is a partial-write API: on a full ring it queues what fits
    /// and reports how much, which for a length-prefixed frame is corruption -
    /// the truncated prefix still goes out and the peer parses the next frame's
    /// bytes as this one's payload, losing both. This refuses instead, so the
    /// caller can retry a whole frame later.
    bool writeAll(const uint8_t* src, uint32_t len) override;

    /// 0 = idle, 255 = the TX ring is full. See INetTransport::txCongestion.
    uint8_t txCongestion() const override {
        return static_cast<uint8_t>((m_tx.size() * 255u) / m_tx.capacity());
    }

    /// Bytes writeAll() would accept right now. See INetTransport::txSpace.
    uint32_t txSpace() const override { return m_tx.space(); }
    uint32_t txCapacity() const override { return m_tx.capacity(); }

    // Diagnostics (for an on-screen bandwidth/health overlay).
    uint32_t bytesReceived() const { return m_bytesRx; }
    uint32_t bytesSent() const { return m_bytesTx; }
    uint32_t rxOverflows() const { return m_rxOverflow; }
    // Hardware-reported parity/overrun/framing errors. A climbing count with
    // RxMode::Polled on real hardware is the 8-byte FIFO overrunning - that is
    // STAT_OE, and it is the signature of this driver being polled too slowly.
    // Note this is NOT rxOverflows(), which counts the *software* ring filling.
    uint32_t serialErrors() const { return m_serialErrors; }
    /// Split out from serialErrors, because they mean opposite things.
    /// OVERRUN: the 8-byte FIFO filled before we drained it -- our latency.
    /// FRAMING: a stop bit landed wrong -- bit timing or signal quality, NOT
    /// latency. Both worsen with baud, which is why one lumped counter sent this
    /// investigation chasing the wrong one for several hardware runs.
    uint32_t rxOverrunErrors() const { return m_rxOverrunErrors; }
    uint32_t rxFramingErrors() const { return m_rxFramingErrors; }
    /// Should be permanently zero: parity is disabled in c_mode8N1. Non-zero
    /// means MODE is not what this driver thinks it is.
    uint32_t rxParityErrors() const { return m_rxParityErrors; }
    /// Times the receive flag stayed asserted past twice the FIFO depth, i.e. the
    /// receiver was wedged. Non-zero means the bounded drain loop saved the
    /// console from spinning in its interrupt handler forever.
    uint32_t rxDrainOverruns() const { return m_rxDrainOverruns; }
    uint32_t rxInterrupts() const { return m_rxIrqs; }
    // Whole frames writeAll() refused because the TX ring was too full. Non-zero
    // means the link is being asked to carry more than it can and the caller had
    // to retry - which is information, where a silent truncation was not.
    uint32_t txRejected() const { return m_txRejected; }
    // Deepest the RX ring has ever been, in bytes. This is the early-warning
    // number: rxOverflows() only tells you bytes were ALREADY lost, whereas this
    // shows how close a run came. A high-water approaching the ring size means
    // poll() is not keeping up and loss is one hitch away.
    uint32_t rxHighWater() const { return m_rxHighWater; }
    uint32_t rxCapacity() const { return m_rx.capacity(); }

    /// Bytes queued for transmission but not yet on the wire.
    ///
    /// Callers should treat a rising value as "the wire cannot keep up" and STOP
    /// QUEUEING anything droppable. That is not a nicety: the console once
    /// transmitted at two bytes per frame, and the ring filled with stale position
    /// snapshots until acknowledgements queued behind minutes of them and the
    /// peer abandoned the message it was retransmitting. Anything droppable must yield to anything
    /// that is not, and only the caller knows which is which.
    uint32_t txPending() const { return m_tx.size(); }
    uint32_t txHighWater() const { return m_txHighWater; }
    /// Times the handler was seen re-entering without taking a byte off the FIFO.
    ///
    /// PURELY A DIAGNOSTIC. Nothing acts on it, and the driver never switches its
    /// own receive path off - see noteRunaway() for why demoting to polled RX is a
    /// dead link rather than a degraded one, and why an RX-only storm is bounded by
    /// the wire (~1440 interrupts a second at 57600) and so cannot take the CPU.
    ///
    /// Surfaced as "LINK BUSY". A non-zero value is worth investigating; it is no
    /// longer a report that the link has been switched off.
    uint32_t irqStorms() const { return m_irqStorms; }
    /// Consecutive interrupts that drained no bytes - the actual runaway signal.
    /// See c_idleIrqStormLimit. Should sit at 0 on a healthy link.
    uint32_t idleIrqRun() const { return m_idleIrqRun; }
    /// Times the per-frame TX spin budget ran out with bytes still queued, i.e.
    /// the wire could not carry everything this frame. A slowly climbing value on
    /// a saturated link is normal; a fast one means the send cadence above is
    /// asking for more than the baud can deliver.
    uint32_t txSpinTimeouts() const { return m_txSpinTimeouts; }
    /// The baud actually programmed, so a bridge mismatch is visible on screen.
    uint32_t baud() const { return m_baud; }

    void resetStats() {
        m_rxRevivals = 0;
        m_bytesRx = m_bytesTx = m_rxOverflow = m_serialErrors = m_rxIrqs = m_txRejected = 0;
        m_rxHighWater = m_txHighWater = 0;
        m_rxOverrunErrors = m_rxFramingErrors = m_rxParityErrors = 0;
    }

  private:
    Sio1() = default;

    static Sio1 s_instance;

    /// rearmRx() without the IRQ8 mask, for callers that already hold it.
    void rearmRxLocked();
    void drainRx();         // hardware RX FIFO -> RX ring
    void pumpTx();          // TX ring -> TX FIFO, non-blocking. IRQ-masked callers only.
    void pumpTxBlocking();  // as above, but waits for the FIFO, bounded
    void kickTx();        // safe main-thread wrapper around pumpTx
    void noteRunaway();   // record a runaway for the on-screen readout; no action
    void installIrqHandler();
    void handleRxIrq();

    // Sizes and their rationale live with c_rxRingSize / c_txRingSize above.
    RingBuffer<c_rxRingSize> m_rx;
    RingBuffer<c_txRingSize> m_tx;

    uint32_t m_bytesRx = 0;
    uint32_t m_bytesTx = 0;
    uint32_t m_rxOverflow = 0;   // bytes dropped because the RX ring was full
    uint32_t m_serialErrors = 0;  // any of the three below (kept for compatibility)
    uint32_t m_rxOverrunErrors = 0;
    uint32_t m_rxFramingErrors = 0;
    uint32_t m_rxParityErrors = 0;
    uint32_t m_rxDrainOverruns = 0;  // RX flag still set after 2x the FIFO depth
    uint32_t m_rxRevivals = 0;    // times rearmRx() caught a destroyed RX interrupt
    uint32_t m_rxIrqs = 0;        // RX interrupts serviced (Interrupt mode only)
    uint32_t m_txRejected = 0;    // whole frames refused rather than truncated
    uint32_t m_rxHighWater = 0;   // deepest m_rx has been, bytes
    uint32_t m_txHighWater = 0;   // deepest m_tx has been, bytes
    uint32_t m_idleIrqRun = 0;  // consecutive IRQs that drained zero bytes
    uint32_t m_irqStorms = 0;   // runaway conditions OBSERVED; nothing acts on it
    uint32_t m_txSpinTimeouts = 0;  // TX gave up waiting for the FIFO
    uint16_t m_ctrl = 0;          // cached CTRL; the register cannot be read back
    uint32_t m_baud = 0;          // as programmed, for the on-screen readout
    RxMode m_rxMode = RxMode::Polled;
    bool m_initialized = false;
    bool m_irqInstalled = false;  // the BIOS event is opened once, never closed
};

}  // namespace psxsplash
