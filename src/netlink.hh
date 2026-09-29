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
 *   - an unreliable send path (snapshots/ping) - a dropped frame just costs one
 *     stale tick, so no retransmission;
 *   - a reliable event channel - stop-and-wait with sequence numbers, acks and
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
    void poll(int32_t dt12);

    // Unreliable send (snapshots, ping, handshake). Returns false if the frame
    // could not be queued (payload too large or transport buffer full).
    // `flags` populates PacketHeader.flags (e.g. c_flagRebind on a Hello).
    bool send(PacketType type, const uint8_t* payload, uint16_t len, uint8_t flags = 0);

    // Reliable send (events). Queued and retransmitted until acknowledged;
    // delivered to the peer in order. Returns false if the payload is too large
    // or the outgoing queue is full.
    bool sendReliable(PacketType type, const uint8_t* payload, uint16_t len);

    /// LATEST-WINS send, for state that supersedes itself - position snapshots
    /// being the canonical case.
    ///
    /// Exactly ONE such message is ever pending. A new one overwrites the old
    /// rather than queueing behind it, and it is emitted only after control and
    /// reliable traffic have had the wire. That makes the failure this project
    /// kept rediscovering STRUCTURALLY IMPOSSIBLE rather than merely unlikely:
    ///
    ///   Every previous design queued snapshots like anything else, so on a link
    ///   slower than the sender assumed they piled up - and acknowledgements, room
    ///   reliable messages queued BEHIND them. The peer then abandoned messages
    ///   this end had already received and parsed correctly, and the game showed an
    ///   empty player list while the wire ran flat out carrying positions that were
    ///   seconds stale on arrival. Thresholds were tried against it three times
    ///   (txCongestion limits, backlog watermarks, per-frame byte caps) and each
    ///   worked until the threshold itself was wrong.
    ///
    /// A single overwritable slot cannot pile up. There is no threshold to get
    /// wrong, and no value of any constant that reintroduces the bug.
    ///
    /// Never returns false: superseding is not a failure, it is the contract. Watch
    /// latestSuperseded() to see how far behind the wire is running.
    bool sendLatest(PacketType type, const uint8_t* payload, uint16_t len);

    /// Latest-wins messages replaced before they reached the wire. This is a
    /// direct measure of how far the link is behind the sender: 0 means every
    /// snapshot made it out, and a rising count means the wire cannot carry the
    /// offered rate - which is information, where silently queueing them was not.
    uint32_t latestSuperseded() const { return m_latestSuperseded; }

    /// Times a snapshot was held back to keep the control reserve free, and times
    /// a Ping could not be queued at all. Both are the link telling you it is
    /// saturated, and the second is the more serious: while it is non-zero and
    /// rttSamples() is 0, nothing here is measuring anything.
    uint32_t latestDeferred() const { return m_latestDeferred; }
    uint32_t pingDeferred() const { return m_pingDeferred; }

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
    // before sendReliable() starts refusing - see c_eventQueueSize.
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
    // Frame scratch: sync + header + payload + crc. Sized once, named here so the
    // latest-wins slot below can hold a whole frame without a second constant.
    static constexpr uint32_t c_maxFrameBytes = 1 + sizeof(PacketHeader) + c_maxPayload + 2;
    uint32_t buildFrame(uint8_t* out, PacketType type, uint16_t seq, const uint8_t* payload, uint16_t len,
                        uint8_t flags);
    bool buildAndSend(PacketType type, uint16_t seq, const uint8_t* payload, uint16_t len, uint8_t flags = 0);
    void pumpLatest();  // emit the pending latest-wins frame if the wire has room
    void pumpReliable();      // start the next queued event if idle
    void serviceReliable(int32_t dt12);  // retransmit the in-flight event on timeout

    // --- measurement ---
    void advanceClock(int32_t dt12);   // monotonic time from the frame delta
    void servicePing();                // emit a Ping when one is due
    void handlePing();                 // reply to a peer's Ping with a Pong
    void handlePong();                 // fold a completed round trip into the SRTT
    void updateRtt(uint32_t sampleMs); // Jacobson/Karels
    int32_t resendIntervalDt() const;  // RTO in dt units, from the measured RTT

  public:
    /// How congested the outgoing wire is, 0..255. Callers with droppable data
    /// MUST consult this before queueing - see INetTransport::txCongestion.
    uint8_t txCongestion() const { return m_transport.txCongestion(); }

    /// Acknowledgements the transport refused and that had to be retried. A
    /// non-zero value means the outbound side was full at the exact moment the
    /// peer was waiting to be told its message arrived - which, before these were
    /// retried, presented as the peer giving up on a message we already had.
    uint32_t acksDeferred() const { return m_acksDeferred; }

    // --- measurement -------------------------------------------------------
    //
    // The link's own account of itself. Everything here is MEASURED; nothing is
    // configured. That distinction is the point: this stack has twice been broken
    // indefinitely by a constant that encoded an assumption about the wire, and in
    // both cases no layer held a number that could have contradicted it.
    //
    // Deliberately at the NetLink layer rather than in the driver, so it works for
    // any INetTransport - serial, emulator, or a future one - without the transport
    // participating.

    /// Outbound bytes per second the link is actually accepting, averaged over the
    /// last complete second. Once the transport's queue is saturated this equals
    /// the rate the wire drains at, because a full queue refuses rather than grows.
    uint32_t goodputBytesPerSecond() const { return m_goodput; }

    /// Inbound bytes per second, same window.
    uint32_t inboundBytesPerSecond() const { return m_inbound; }

    /// Smoothed round trip in milliseconds, 0 until the first Pong arrives.
    /// Jacobson/Karels SRTT - see updateRtt().
    uint16_t rttMillis() const { return m_srttMs; }

    /// Current retransmission timeout in milliseconds, derived from the measured
    /// round trip. A fixed timer shorter than the actual RTT does not repair a
    /// congested link, it feeds it.
    uint16_t rtoMillis() const { return m_rtoMs; }

    /// The peer's own reported goodput and RTT, from the last Ping/Pong it sent.
    /// Zero until one arrives. This is what lets each end pace itself by what the
    /// OTHER end measured, rather than by what it assumed.
    uint32_t peerGoodput() const { return m_peerGoodput; }
    uint16_t peerRttMillis() const { return m_peerRttMs; }

    /// Round trips completed. If this stays 0 the peer is not answering Ping, and
    /// every RTT-derived number below is running on its default.
    uint32_t rttSamples() const { return m_rttSamples; }

  private:
    // A refused acknowledgement, held for retry from poll(). Only the newest is
    // kept: the peer is stop-and-wait, so an older seq is already satisfied.
    uint16_t m_pendingAckSeq = 0;
    bool m_hasPendingAck = false;
    uint32_t m_acksDeferred = 0;

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

    // The latest-wins slot. One frame, overwritten rather than queued - see
    // sendLatest(). This is the whole priority scheme's teeth: control and
    // reliable traffic go straight to the transport, and this is emitted only
    // from poll(), after both have had their chance.
    uint8_t m_latestFrame[c_maxFrameBytes];
    uint32_t m_latestLen = 0;
    bool m_hasLatest = false;
    uint32_t m_latestSuperseded = 0;
    uint32_t m_latestDeferred = 0;

    // Transport space that droppable data may never consume, so a control frame
    // always has somewhere to go.
    //
    // DERIVED, not chosen. The two control frames are an EventAck (sync + header +
    // crc = 11 bytes) and a Ping (those plus a 12-byte payload = 23). Four Pings'
    // worth covers an ack, a ping, and a retransmission of each without the
    // reserve itself becoming a second queue to fill.
    //
    // The distinction from the c_snapshotCongestionLimit this file used to carry
    // matters: that was a throughput policy ("stop sending when busy") tuned by
    // guess, and it was wrong three times. This is an INVARIANT ("control traffic
    // always fits") sized by measuring the frames it must hold. It cannot be
    // "tuned" without changing what a control frame is.
    static constexpr uint32_t c_controlFrameBytes = 1 + sizeof(PacketHeader) + sizeof(PingPayload) + 2;
    static constexpr uint32_t c_controlReserveBytes = 4 * c_controlFrameBytes;

    // Reliable event channel (stop-and-wait). Outgoing events queue here; the
    // front is transmitted and retransmitted until its ack arrives.
    //
    // The queue is shared by Event, ObjectState and AppData. 8 was too shallow
    // once all three compete: a burst (a meeting starting, say - several object
    // states plus the events announcing it) would fill it, sendReliable() would
    // start returning false, and a caller that ignored the return would drop
    // game events silently. 16 costs ~4KB of RAM and removes that cliff; callers
    // should still check the return and can watch reliableQueueDepth().
    static constexpr uint32_t c_eventQueueSize = 16;
    // The retransmit interval is no longer a constant - it is derived from the
    // measured round trip; see resendIntervalDt() and c_rtoDefaultMs.
    //
    // It was once a count of poll() CALLS, which made the retransmit rate fall with
    // the frame rate, so a console that was struggling also repaired lost packets
    // three times more slowly, exactly when it was losing more of them. Then it was
    // a fixed wall-clock ~100ms, which is right only for a link whose round trip
    // happens to be shorter than that.
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
    int32_t m_resendTimer = 0;
    uint16_t m_txEventSeq = 1;
    // Last event seq delivered to the handler, for duplicate suppression.
    uint16_t m_rxLastEventSeq = 0;
    bool m_rxHasEvent = false;

    // Stats.
    uint32_t m_framesRx = 0;
    uint32_t m_crcErrors = 0;
    uint32_t m_resyncs = 0;
    uint32_t m_retransmits = 0;

    // --- measurement state -------------------------------------------------
    //
    // Time is in the same 4.12 fixed-point units the rest of the engine uses, where
    // 4096 == one 30Hz frame, so one second is 30*4096. It is accumulated from the
    // dt handed to poll() rather than read from a clock, which keeps NetLink free
    // of hardware and makes the simulated-link tests exactly reproducible.
    static constexpr int32_t c_dtPerSecond = 30 * 4096;
    static constexpr int32_t c_pingIntervalDt = c_dtPerSecond;  // once a second

    // RTO bounds. The floor stops a fast LAN link retransmitting so eagerly that a
    // brief hiccup becomes a storm; the ceiling stops a badly degraded link from
    // effectively giving up. Between them the value is entirely measured.
    static constexpr uint16_t c_rtoFloorMs = 100;
    static constexpr uint16_t c_rtoCeilingMs = 2000;
    // Used until the first Pong lands. Matches the old fixed interval, so a peer
    // that never answers Ping behaves exactly as this link did before.
    static constexpr uint16_t c_rtoDefaultMs = 100;

    int32_t m_nowDt = 0;            // monotonic, accumulated from poll()'s dt
    // First probe is one interval in, never at t=0: at zero the handshake has not
    // happened, the peer may not exist yet, and a Ping would just be a frame on the
    // wire that nothing can answer.
    int32_t m_pingDueDt = c_pingIntervalDt;
    uint32_t m_pingEcho = 0;        // token for the outstanding Ping
    int32_t m_pingSentAtDt = 0;     // when it went out
    bool m_pingPending = false;

    uint16_t m_srttMs = 0;          // smoothed round trip
    uint16_t m_rttVarMs = 0;        // round trip variation
    uint16_t m_rtoMs = c_rtoDefaultMs;
    uint32_t m_rttSamples = 0;
    uint32_t m_pingDeferred = 0;

    uint32_t m_peerGoodput = 0;
    uint16_t m_peerRttMs = 0;

    // Throughput is counted into a window and published once per second, so the
    // reported figure is a rate rather than a running total. Totals hid a 48:1
    // send/receive imbalance completely; a rate shows it at a glance.
    int32_t m_windowStartDt = 0;
    uint32_t m_txWindowBytes = 0;
    uint32_t m_rxWindowBytes = 0;
    uint32_t m_goodput = 0;
    uint32_t m_inbound = 0;
};

}  // namespace psxsplash::net
