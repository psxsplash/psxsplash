#pragma once

#include <stdint.h>

#include "inettransport.hh"
#include "ringbuffer.hh"

namespace psxsplash {

/**
 * Bare-metal driver for the PlayStation's second serial port, SIO1
 * (registers at 0x1F801050..0x1F80105E) — the "serial port" / link-cable port,
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
 * RX STRATEGY — read this before changing the default
 * ---------------------------------------------------
 * The hardware RX FIFO is only **8 bytes** deep, and the hardware does NOT
 * auto-deassert RTS when it fills; the 9th byte overwrites the last entry and
 * sets STAT_OE (see psx-spx "SIO_RX_DATA Notes"). At 115200 baud a byte lands
 * every ~87us, so the FIFO overruns ~694us after the first byte of a burst.
 * A once-per-frame poll (16.6ms apart) therefore cannot keep up on real
 * hardware: it captures 8 bytes and loses the rest of the burst.
 *
 *   RxMode::Interrupt — required on REAL HARDWARE. Drains the FIFO from the
 *       SIO IRQ, so the 8-byte window is honoured.
 *
 *   RxMode::Polled — correct on PCSX-Redux, and the default. Redux does not
 *       model the 8-byte FIFO: its RX is an unbounded software fifo, so a
 *       once-per-frame drain captures everything.
 *
 * The default is Polled deliberately, and NOT merely for backwards
 * compatibility. In Redux's Protobuf mode (which is what the SIO1 *client* is
 * hardwired to — it throws on Raw), SIO1::interrupt() contains:
 *
 *     if (m_sio1fifo.isA<Fifo>()) {
 *         if (m_sio1fifo->size() > 8) m_sio1fifo.asA<Fifo>()->reset();
 *     }
 *
 * i.e. it discards the ENTIRE receive buffer — not one byte — whenever more
 * than 8 bytes are queued when the IRQ fires. That code only runs if the game
 * sets CTRL_RXIRQEN, because nothing else schedules the interrupt. So enabling
 * interrupts on Redux would actively destroy inbound snapshots that polled mode
 * receives perfectly. Enable Interrupt mode on hardware; leave it off for the
 * emulator.
 */
class Sio1 final : public INetTransport {
  public:
    static constexpr uint32_t c_defaultBaud = 115200;

    // How received bytes get moved out of the hardware FIFO. See the class
    // comment — this is a correctness choice per target, not a tuning knob.
    enum class RxMode : uint8_t {
        Auto,       // Polled under PCSX-Redux, Interrupt on real hardware.
        Polled,     // drained by poll(), once per frame. Emulator-safe.
        Interrupt,  // drained by the SIO IRQ. Required on real hardware.
    };

    // Singleton accessor (namespace-scope instance; constructed at static-init
    // like PSYQo's own global objects).
    static Sio1& Get();

    // Reset and configure the SIO1 block: 8N1, MUL1 baud factor, TX+RX enabled.
    // Safe to call again to change baud or RX mode.
    //
    // RxMode::Auto is the right answer on both targets and needs no build flag:
    // the two targets want opposite settings, and the console can tell them
    // apart at runtime. Pass an explicit mode only to override that.
    void init(uint32_t baud = c_defaultBaud, RxMode rxMode = RxMode::Auto);
    bool isInitialized() const { return m_initialized; }
    // The resolved mode — never returns Auto once init() has run.
    RxMode rxMode() const { return m_rxMode; }

    // --- INetTransport ---
    void poll() override;
    uint32_t available() const override { return m_rx.size(); }
    uint32_t read(uint8_t* dst, uint32_t max) override;
    uint32_t write(const uint8_t* src, uint32_t len) override;

    // Convenience: queue a single byte. Returns false if the TX ring is full.
    bool writeByte(uint8_t b);

    // Diagnostics (for an on-screen bandwidth/health overlay).
    uint32_t bytesReceived() const { return m_bytesRx; }
    uint32_t bytesSent() const { return m_bytesTx; }
    uint32_t rxOverflows() const { return m_rxOverflow; }
    // Hardware-reported parity/overrun/framing errors. A climbing count with
    // RxMode::Polled on real hardware is the 8-byte FIFO overrunning — that is
    // STAT_OE, and it is the signature of this driver being polled too slowly.
    // Note this is NOT rxOverflows(), which counts the *software* ring filling.
    uint32_t serialErrors() const { return m_serialErrors; }
    uint32_t rxInterrupts() const { return m_rxIrqs; }
    void resetStats() { m_bytesRx = m_bytesTx = m_rxOverflow = m_serialErrors = m_rxIrqs = 0; }

  private:
    Sio1() = default;

    static Sio1 s_instance;

    void drainRx();  // hardware RX FIFO -> RX ring
    void pumpTx();   // TX ring -> hardware TX FIFO
    void installIrqHandler();
    void handleRxIrq();

    // 1 KiB each: comfortably above the per-frame bandwidth budget so a single
    // slow frame never overflows before the next poll().
    RingBuffer<1024> m_rx;
    RingBuffer<1024> m_tx;

    uint32_t m_bytesRx = 0;
    uint32_t m_bytesTx = 0;
    uint32_t m_rxOverflow = 0;   // bytes dropped because the RX ring was full
    uint32_t m_serialErrors = 0;  // parity/overrun/framing errors observed
    uint32_t m_rxIrqs = 0;        // RX interrupts serviced (Interrupt mode only)
    RxMode m_rxMode = RxMode::Polled;
    bool m_initialized = false;
    bool m_irqInstalled = false;  // the BIOS event is opened once, never closed
};

}  // namespace psxsplash
