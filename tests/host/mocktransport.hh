#pragma once

#include <stdint.h>

#include "inettransport.hh"
#include "netlink.hh"
#include "netprotocol.hh"
#include "ringbuffer.hh"

namespace psxsplash::test {

/**
 * One end of a bidirectional in-RAM pipe: writes go to `out`, reads come from
 * `in`. Two of these with the rings crossed form a full-duplex link with no
 * hardware involved.
 *
 * This generalises the PipeEnd in src/nettest.cpp (the on-device self-test) with
 * the extras a host harness needs: fault injection, write throttling, and byte
 * counters. The on-device test deliberately keeps its own minimal copy so the
 * shipped build never pulls test scaffolding in.
 */
class MockTransport final : public INetTransport {
  public:
    using Ring = RingBuffer<8192>;

    MockTransport(Ring& out, Ring& in) : m_out(out), m_in(in) {}

    void poll() override { m_polls++; }
    uint32_t available() const override { return m_in.size(); }

    uint32_t read(uint8_t* dst, uint32_t max) override {
        uint32_t n = 0;
        uint8_t b;
        while (n < max && m_in.pop(b)) dst[n++] = b;
        m_bytesRead += n;
        return n;
    }

    uint32_t write(const uint8_t* src, uint32_t len) override {
        // Partial write, matching the real hardware's behaviour. `writeLimit`
        // lets a test starve the transport.
        uint32_t budget = (m_writeLimit == kNoLimit) ? len : (m_writeLimit < len ? m_writeLimit : len);
        uint32_t n = 0;
        while (n < budget && m_out.push(src[n])) n++;
        if (m_writeLimit != kNoLimit) m_writeLimit -= n;
        m_bytesWritten += n;
        return n;
    }

    bool writeAll(const uint8_t* src, uint32_t len) override {
        // Genuinely atomic, like Sio1::writeAll: check capacity FIRST, then
        // commit. Emitting the prefix and returning false would put a truncated
        // frame on the wire, which is the exact bug this interface exists to
        // make impossible — a mock that did that would let the bug pass tests.
        uint32_t budget = (m_writeLimit == kNoLimit) ? len : m_writeLimit;
        if (budget < len || m_out.space() < len) {
            m_writesRejected++;
            return false;
        }
        for (uint32_t i = 0; i < len; i++) m_out.push(src[i]);
        if (m_writeLimit != kNoLimit) m_writeLimit -= len;
        m_bytesWritten += len;
        return true;
    }

    uint8_t txCongestion() const override {
        if (m_congestion != kAutoCongestion) return static_cast<uint8_t>(m_congestion);
        return static_cast<uint8_t>((m_out.size() * 255u) / m_out.capacity());
    }

    uint32_t txSpace() const override {
        // Honour writeLimit as well as ring capacity: a test that starves the
        // transport must starve the reserve check too, or the thing under test
        // sees room that writeAll() would then refuse.
        const uint32_t ring = m_out.space();
        if (m_writeLimit == kNoLimit) return ring;
        return m_writeLimit < ring ? m_writeLimit : ring;
    }

    uint32_t txCapacity() const override { return m_out.capacity(); }

    uint32_t writesRejected() const { return m_writesRejected; }

    // --- test controls ---

    static constexpr uint32_t kNoLimit = 0xFFFFFFFFu;

    // Cap the total bytes future write() calls may accept. Reset to kNoLimit to
    // restore an infinite transport.
    void setWriteLimit(uint32_t limit) { m_writeLimit = limit; }

    // Force a congestion reading, so a test can pin the wire "slow" without
    // having to actually fill the ring.
    static constexpr uint32_t kAutoCongestion = 0xFFFFFFFFu;
    void setCongestion(uint32_t level) { m_congestion = level; }

    // Push raw bytes straight onto this end's inbound ring, bypassing any framing
    // — used to inject corruption/garbage ahead of a valid frame.
    void injectInbound(const uint8_t* data, uint32_t len) {
        for (uint32_t i = 0; i < len; i++) m_in.push(data[i]);
    }

    uint32_t polls() const { return m_polls; }
    uint32_t bytesRead() const { return m_bytesRead; }
    uint32_t bytesWritten() const { return m_bytesWritten; }

  private:
    Ring& m_out;
    Ring& m_in;
    uint32_t m_writeLimit = kNoLimit;
    uint32_t m_congestion = kAutoCongestion;
    uint32_t m_polls = 0;
    uint32_t m_bytesRead = 0;
    uint32_t m_bytesWritten = 0;
    uint32_t m_writesRejected = 0;
};

/**
 * Sink that records everything NetLink delivers, so tests can assert on both the
 * fact and the content of each dispatch.
 */
class RecordingHandler final : public net::NetLinkHandler {
  public:
    int helloCount = 0, ackCount = 0, snapshotCount = 0, eventCount = 0, objectStateCount = 0, appDataCount = 0,
        byeCount = 0;

    net::HelloPayload lastHello{};
    uint8_t lastHelloSlot = net::c_noSlot;
    net::HelloAckPayload lastAck{};

    uint16_t lastSnapLen = 0;
    uint8_t lastSnap[net::c_maxPayload]{};
    uint8_t lastSnapSlot = net::c_noSlot;
    uint16_t lastSnapSeq = 0;

    uint16_t lastEventLen = 0;
    uint8_t lastEvent[net::c_maxPayload]{};
    uint8_t lastEventSlot = net::c_noSlot;

    uint16_t lastObjStateLen = 0;
    uint8_t lastObjState[net::c_maxPayload]{};

    uint16_t lastAppDataLen = 0;
    uint8_t lastAppData[net::c_maxPayload]{};

    uint8_t lastByeSlot = net::c_noSlot;

    void onHello(const net::HelloPayload& h, uint8_t srcSlot) override {
        helloCount++;
        lastHello = h;
        lastHelloSlot = srcSlot;
    }
    void onHelloAck(const net::HelloAckPayload& a) override {
        ackCount++;
        lastAck = a;
    }
    void onSnapshot(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t seq) override {
        snapshotCount++;
        lastSnapLen = len;
        lastSnapSlot = srcSlot;
        lastSnapSeq = seq;
        if (len && len <= net::c_maxPayload) __builtin_memcpy(lastSnap, payload, len);
    }
    void onEvent(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t) override {
        eventCount++;
        lastEventLen = len;
        lastEventSlot = srcSlot;
        if (len && len <= net::c_maxPayload) __builtin_memcpy(lastEvent, payload, len);
    }
    void onObjectState(const uint8_t* payload, uint16_t len, uint8_t, uint16_t) override {
        objectStateCount++;
        lastObjStateLen = len;
        if (len && len <= net::c_maxPayload) __builtin_memcpy(lastObjState, payload, len);
    }
    void onAppData(const uint8_t* payload, uint16_t len, uint8_t, uint16_t) override {
        appDataCount++;
        lastAppDataLen = len;
        if (len && len <= net::c_maxPayload) __builtin_memcpy(lastAppData, payload, len);
    }
    void onPeerBye(uint8_t srcSlot) override {
        byeCount++;
        lastByeSlot = srcSlot;
    }
};

}  // namespace psxsplash::test
