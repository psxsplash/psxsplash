#pragma once

#include <stdint.h>

#include "inettransport.hh"
#include "ringbuffer.hh"

namespace psxsplash::test {

/**
 * A deterministic simulated link, and the thing whose absence made every network
 * bug in this project invisible until it reached a real console.
 *
 * MockTransport (mocktransport.hh) is an infinitely fast, zero-latency, lossless
 * in-RAM pipe. Every scenario expressed against it passes trivially, which is why
 * a whole class of defects — throughput ceilings, queues that never drain,
 * retransmit timers shorter than the round trip, acks stuck behind stale data —
 * shipped repeatedly and could only be found by burning a CD-R. That is the gap
 * this fills.
 *
 * It models the four properties a real link has and a RAM pipe does not:
 *
 *   THROUGHPUT   bytes are released on a schedule, so a sender that produces
 *                faster than the wire drains builds a queue, exactly as it does on
 *                the hardware. This is what catches "48 bytes per frame" and
 *                "16 bytes per frame" style ceilings.
 *   LATENCY      delivery is delayed, so stop-and-wait costs a real round trip and
 *                a retransmit timer shorter than the RTT is visibly self-defeating.
 *   JITTER       the delay varies, without reordering — a serial line and a TCP
 *                stream both preserve byte order, so due times are kept monotonic.
 *   LOSS         bytes are dropped, so CRC failure, resync and retransmission are
 *                exercised rather than assumed.
 *
 * And a fifth that is specific to what was learned here, and has no analogue in
 * ordinary network simulation:
 *
 *   HOST COST    CPU charged to the SENDER, per byte, at the moment it writes.
 *                PCSX-Redux pays a protobuf encode, two heap allocations and two
 *                thread-wakeup syscalls for every byte, synchronously on the thread
 *                emulating the CPU. So the emulator is cheap in wire time and
 *                expensive in host CPU — the opposite shape to the PlayStation, and
 *                invisible to any model that only counts bandwidth. Removing a
 *                per-frame byte budget on the theory that "the emulator is free"
 *                made the game unplayable there; hostNanosPerByte is how that
 *                becomes a failing test instead of a lost evening.
 *
 * Everything is driven by an explicit virtual clock (advance()), never by wall
 * time, so runs are exactly reproducible and a test can simulate a minute of link
 * behaviour instantly.
 */
struct LinkProfile {
    /// Wire capacity. 0 means unlimited (bytes leave the queue immediately).
    uint32_t bytesPerSecond = 0;
    /// One-way delay applied to every byte.
    uint32_t latencyMicros = 0;
    /// Maximum extra delay added on top of latencyMicros, varying per byte.
    uint32_t jitterMicros = 0;
    /// Probability, in parts per thousand, that a byte is dropped in flight.
    uint32_t lossPerMille = 0;
    /// Host CPU charged to the sender per byte written. See HOST COST above.
    uint32_t hostNanosPerByte = 0;
    /// Sender-side queue capacity. Writes beyond it are refused, like a full ring.
    uint32_t queueBytes = 1024;

    /// A real PlayStation on 57600 8N1: 5760 B/s, and cheap per byte.
    static LinkProfile serial57600() {
        LinkProfile p;
        p.bytesPerSecond = 5760;
        p.latencyMicros = 2000;
        p.jitterMicros = 500;
        p.queueBytes = 1024;
        return p;
    }

    /// PCSX-Redux over loopback: effectively unlimited wire, but expensive per byte
    /// on the host. The nanos figure is a stand-in for "protobuf encode + two heap
    /// allocations + two uv_async_send syscalls", not a measurement.
    static LinkProfile reduxLoopback() {
        LinkProfile p;
        p.bytesPerSecond = 0;  // never wire-limited
        p.latencyMicros = 200;
        p.hostNanosPerByte = 30000;  // 30us of host CPU per byte
        p.queueBytes = 1024;
        return p;
    }
};

/**
 * One direction of a simulated link. Two of these, crossed, make a full-duplex
 * link — the same shape as MockTransport's paired rings.
 */
class SimWire {
  public:
    void configure(const LinkProfile& p) { m_profile = p; }
    const LinkProfile& profile() const { return m_profile; }

    /// Queue bytes for transmission, all or nothing. Returns false when the
    /// sender-side queue cannot take the whole run, matching Sio1::writeAll.
    bool offer(const uint8_t* src, uint32_t len) {
        if (queued() + len > m_profile.queueBytes) return false;
        for (uint32_t i = 0; i < len; i++) m_queue.push(src[i]);
        m_hostNanos += static_cast<uint64_t>(len) * m_profile.hostNanosPerByte;
        return true;
    }

    /// Queue what fits, returning how much was taken. Matches Sio1::write.
    uint32_t offerPartial(const uint8_t* src, uint32_t len) {
        uint32_t n = 0;
        while (n < len && queued() < m_profile.queueBytes && m_queue.push(src[n])) n++;
        m_hostNanos += static_cast<uint64_t>(n) * m_profile.hostNanosPerByte;
        return n;
    }

    /// Advance the virtual clock, clocking bytes out of the queue at the configured
    /// rate and delivering those whose flight time has elapsed.
    void advance(uint64_t nowMicros) {
        const uint64_t perByte = m_profile.bytesPerSecond ? (1000000ull / m_profile.bytesPerSecond) : 0;

        // Clock bytes onto the wire, no faster than the link carries them.
        while (!m_queue.empty()) {
            if (m_sendAtMicros < m_lastAdvance) m_sendAtMicros = m_lastAdvance;
            if (m_sendAtMicros > nowMicros) break;
            uint8_t b;
            if (!m_queue.pop(b)) break;

            uint64_t due = m_sendAtMicros + m_profile.latencyMicros;
            if (m_profile.jitterMicros) due += nextRandom() % (m_profile.jitterMicros + 1);
            // Order is never scrambled: a serial line and a TCP stream both
            // preserve it, so jitter varies delay without overtaking.
            if (due < m_lastDueMicros) due = m_lastDueMicros;
            m_lastDueMicros = due;

            bool lost = m_profile.lossPerMille && (nextRandom() % 1000u) < m_profile.lossPerMille;
            if (lost) {
                m_bytesLost++;
            } else {
                m_flight[m_flightHead % kMaxInFlight] = InFlight{b, due};
                m_flightHead++;
            }
            m_sendAtMicros += perByte;
        }

        // Deliver everything whose flight time has elapsed.
        while (m_flightTail < m_flightHead) {
            const InFlight& f = m_flight[m_flightTail % kMaxInFlight];
            if (f.dueMicros > nowMicros) break;
            m_delivered.push(f.byte);
            m_flightTail++;
        }

        m_lastAdvance = nowMicros;
    }

    uint32_t take(uint8_t* dst, uint32_t max) {
        uint32_t n = 0;
        uint8_t b;
        while (n < max && m_delivered.pop(b)) dst[n++] = b;
        return n;
    }

    uint32_t available() const { return m_delivered.size(); }
    uint32_t queued() const { return m_queue.size(); }
    uint32_t queueCapacity() const { return m_profile.queueBytes; }
    uint32_t bytesLost() const { return m_bytesLost; }

    /// Host CPU the SENDER has spent feeding this wire. The number that makes the
    /// emulator's per-byte cost testable.
    uint64_t hostNanos() const { return m_hostNanos; }
    void resetHostNanos() { m_hostNanos = 0; }

    void seed(uint32_t s) { m_rng = s ? s : 1; }

  private:
    struct InFlight {
        uint8_t byte;
        uint64_t dueMicros;
    };
    static constexpr uint32_t kMaxInFlight = 8192;

    // Deterministic by construction: a test that fails must fail identically on
    // the next run, or it teaches nothing.
    uint32_t nextRandom() {
        m_rng = m_rng * 1664525u + 1013904223u;
        return m_rng >> 8;
    }

    LinkProfile m_profile{};
    RingBuffer<8192> m_queue;
    RingBuffer<16384> m_delivered;
    InFlight m_flight[kMaxInFlight]{};
    uint32_t m_flightHead = 0;
    uint32_t m_flightTail = 0;
    uint64_t m_sendAtMicros = 0;
    uint64_t m_lastDueMicros = 0;
    uint64_t m_lastAdvance = 0;
    uint64_t m_hostNanos = 0;
    uint32_t m_bytesLost = 0;
    uint32_t m_rng = 12345;
};

/**
 * INetTransport over a pair of SimWires. Drop-in for MockTransport wherever a test
 * needs the link to behave like a link.
 */
class SimTransport final : public INetTransport {
  public:
    SimTransport(SimWire& out, SimWire& in) : m_out(out), m_in(in) {}

    void poll() override { m_polls++; }
    uint32_t available() const override { return m_in.available(); }

    uint32_t read(uint8_t* dst, uint32_t max) override {
        uint32_t n = m_in.take(dst, max);
        m_bytesRead += n;
        return n;
    }

    uint32_t write(const uint8_t* src, uint32_t len) override {
        uint32_t n = m_out.offerPartial(src, len);
        m_bytesWritten += n;
        return n;
    }

    bool writeAll(const uint8_t* src, uint32_t len) override {
        if (!m_out.offer(src, len)) {
            m_writesRejected++;
            return false;
        }
        m_bytesWritten += len;
        return true;
    }

    uint8_t txCongestion() const override {
        const uint32_t cap = m_out.queueCapacity();
        if (!cap) return 0;
        uint32_t q = m_out.queued();
        if (q > cap) q = cap;
        return static_cast<uint8_t>((q * 255u) / cap);
    }

    uint32_t txSpace() const override {
        const uint32_t cap = m_out.queueCapacity();
        const uint32_t used = m_out.queued();
        return used >= cap ? 0 : cap - used;
    }

    uint32_t txCapacity() const override { return m_out.queueCapacity(); }

    uint32_t polls() const { return m_polls; }
    uint32_t bytesRead() const { return m_bytesRead; }
    uint32_t bytesWritten() const { return m_bytesWritten; }
    uint32_t writesRejected() const { return m_writesRejected; }

  private:
    SimWire& m_out;
    SimWire& m_in;
    uint32_t m_polls = 0;
    uint32_t m_bytesRead = 0;
    uint32_t m_bytesWritten = 0;
    uint32_t m_writesRejected = 0;
};

/**
 * A full-duplex simulated link between two endpoints, with a shared virtual clock.
 *
 * Both directions advance together, so a test drives the whole link with one
 * `advance()` and cannot accidentally simulate one side running ahead of the other.
 */
class SimLink {
  public:
    SimLink() : a(aToB, bToA), b(bToA, aToB) {}

    void configure(const LinkProfile& p) {
        aToB.configure(p);
        bToA.configure(p);
        aToB.seed(1);
        bToA.seed(2);
    }

    /// Advance the virtual clock by `micros`, clocking and delivering both ways.
    void advance(uint32_t micros) {
        m_nowMicros += micros;
        aToB.advance(m_nowMicros);
        bToA.advance(m_nowMicros);
    }

    /// Advance by one 30Hz frame, the cadence poll() actually runs at.
    void advanceFrame() { advance(33333); }

    uint64_t nowMicros() const { return m_nowMicros; }

    SimWire aToB;
    SimWire bToA;
    SimTransport a;
    SimTransport b;

  private:
    uint64_t m_nowMicros = 0;
};

}  // namespace psxsplash::test
