#pragma once

#include <stdint.h>

#include "inettransport.hh"
#include "netprotocol.hh"

namespace psxsplash::net {

/**
 * Sink for fully-parsed, CRC-validated packets. Implemented by the session /
 * NetworkManager layer above NetLink.
 */
class NetLinkHandler {
  public:
    virtual void onHello(const HelloPayload& hello, uint8_t srcSlot) = 0;
    virtual void onHelloAck(const HelloAckPayload& ack) = 0;
    virtual void onSnapshot(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t seq) = 0;
    virtual void onEvent(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t seq) = 0;
    virtual void onObjectState(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t seq) = 0;
    virtual void onAppData(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t seq) = 0;
    virtual void onPeerBye(uint8_t srcSlot) = 0;

  protected:
    ~NetLinkHandler() = default;
};

/**
 * NetLink turns a raw INetTransport byte stream into framed, integrity-checked,
 * slot-addressed packets, and back. It owns:
 *   - a byte-oriented receive state machine with CRC validation and resync;
 *   - an unreliable send path (snapshots/ping) — a dropped frame just costs one
 *     stale tick, so no retransmission;
 *   - a reliable event channel — stop-and-wait with sequence numbers, acks and
 *     retransmission (correct and simple; events are infrequent).
 *
 * It is transport-agnostic and hardware-free (depends only on INetTransport and
 * the pure protocol header), so it is unit-testable on a host with a mock
 * transport.
 */
class NetLink {
  public:
    NetLink(INetTransport& transport, NetLinkHandler& handler) : m_transport(transport), m_handler(handler) {}

    void setLocalSlot(uint8_t slot) { m_localSlot = slot; }
    uint8_t localSlot() const { return m_localSlot; }

    // Service the transport: parse incoming frames (dispatched via the handler)
    // and drive reliable retransmits. Call once per frame.
    void poll();

    // Unreliable send (snapshots, ping, handshake). Returns false if the frame
    // could not be queued (payload too large or transport buffer full).
    // `flags` populates PacketHeader.flags (e.g. c_flagRebind on a Hello).
    bool send(PacketType type, const uint8_t* payload, uint16_t len, uint8_t flags = 0);

    // Reliable send (events). Queued and retransmitted until acknowledged;
    // delivered to the peer in order. Returns false if the payload is too large
    // or the outgoing queue is full.
    bool sendReliable(PacketType type, const uint8_t* payload, uint16_t len);

    // Reset all protocol state (parser, reliable channel, sequence numbers).
    // Call on (re)connect / scene change.
    void reset();

    // Diagnostics.
    uint32_t framesReceived() const { return m_framesRx; }
    uint32_t crcErrors() const { return m_crcErrors; }
    uint32_t resyncs() const { return m_resyncs; }
    uint32_t retransmits() const { return m_retransmits; }
    bool reliableIdle() const { return !m_inFlight && m_eventCount == 0; }
    // Queued reliable packets, in-flight one included. Callers can back off
    // before sendReliable() starts refusing — see c_eventQueueSize.
    uint32_t reliableQueueDepth() const { return m_eventCount; }
    static constexpr uint32_t reliableQueueCapacity() { return c_eventQueueSize; }

  private:
    // --- receive path ---
    enum class RxState : uint8_t { Sync, Header, Payload, Crc };
    void feedByte(uint8_t b);
    void dispatch();
    void handleIncomingReliable(PacketType type);
    void handleEventAck(uint16_t seq);

    // --- send path ---
    bool buildAndSend(PacketType type, uint16_t seq, const uint8_t* payload, uint16_t len, uint8_t flags = 0);
    void pumpReliable();      // start the next queued event if idle
    void serviceReliable();   // retransmit the in-flight event on timeout

    INetTransport& m_transport;
    NetLinkHandler& m_handler;
    uint8_t m_localSlot = c_noSlot;

    // Receive state machine.
    RxState m_rxState = RxState::Sync;
    PacketHeader m_hdr{};
    uint8_t m_hdrBuf[sizeof(PacketHeader)];
    uint16_t m_hdrGot = 0;
    uint8_t m_payload[c_maxPayload];
    uint16_t m_payloadLen = 0;
    uint16_t m_payloadGot = 0;
    uint8_t m_crcBuf[2];
    uint16_t m_crcGot = 0;

    // Unreliable sequence counter (generation number for snapshots/ping).
    uint16_t m_txSeq = 1;

    // Reliable event channel (stop-and-wait). Outgoing events queue here; the
    // front is transmitted and retransmitted until its ack arrives.
    //
    // The queue is shared by Event, ObjectState and AppData. 8 was too shallow
    // once all three compete: a burst (a meeting starting, say — several object
    // states plus the events announcing it) would fill it, sendReliable() would
    // start returning false, and a caller that ignored the return would drop
    // game events silently. 16 costs ~4KB of RAM and removes that cliff; callers
    // should still check the return and can watch reliableQueueDepth().
    static constexpr uint32_t c_eventQueueSize = 16;
    static constexpr uint32_t c_resendInterval = 6;  // poll()s between retransmits
    struct PendingEvent {
        uint8_t data[c_maxEventPayload];
        uint16_t len;
        uint8_t type;
    };
    PendingEvent m_eventQueue[c_eventQueueSize];
    uint32_t m_eventHead = 0;    // index of the in-flight / next event
    uint32_t m_eventCount = 0;   // number queued
    bool m_inFlight = false;
    uint16_t m_inFlightSeq = 0;
    uint32_t m_resendTimer = 0;
    uint16_t m_txEventSeq = 1;
    // Last event seq delivered to the handler, for duplicate suppression.
    uint16_t m_rxLastEventSeq = 0;
    bool m_rxHasEvent = false;

    // Stats.
    uint32_t m_framesRx = 0;
    uint32_t m_crcErrors = 0;
    uint32_t m_resyncs = 0;
    uint32_t m_retransmits = 0;
};

}  // namespace psxsplash::net
