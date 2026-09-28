#include "netlink.hh"

namespace psxsplash::net {

void NetLink::reset() {
    m_hasPendingAck = false;  // a new session owes the old peer nothing
    m_rxState = RxState::Sync;
    m_hdrGot = m_payloadGot = m_crcGot = m_payloadLen = 0;
    m_txSeq = 1;
    m_eventHead = m_eventCount = 0;
    m_inFlight = false;
    m_inFlightSeq = 0;
    m_resendTimer = 0;
    m_txEventSeq = 1;
    m_rxLastEventSeq = 0;
    m_rxHasEvent = false;
    m_framesRx = m_crcErrors = m_resyncs = m_retransmits = 0;

    // Measurement is per-session: a new peer is a new link, and carrying an old
    // link's round trip into it would mis-time the first retransmissions of the
    // one exchange that matters most, the handshake.
    m_nowDt = m_pingSentAtDt = 0;
    m_pingDueDt = c_pingIntervalDt;
    m_pingEcho = 0;
    m_pingPending = false;
    m_srttMs = m_rttVarMs = 0;
    m_rtoMs = c_rtoDefaultMs;
    m_rttSamples = 0;
    m_peerGoodput = 0;
    m_peerRttMs = 0;
    m_windowStartDt = 0;
    m_txWindowBytes = m_rxWindowBytes = 0;
    m_goodput = m_inbound = 0;

    // A snapshot built for the old session describes a scene the new peer is not
    // in. Dropping it is the point of the slot being latest-wins.
    m_hasLatest = false;
    m_latestLen = 0;
    m_latestSuperseded = m_latestDeferred = 0;
    m_pingDeferred = 0;
}

// --- receive path ---------------------------------------------------------

void NetLink::poll(int32_t dt12) {
    m_transport.poll();
    advanceClock(dt12);

    uint8_t buf[128];
    uint32_t n;
    while ((n = m_transport.read(buf, sizeof(buf))) > 0) {
        m_rxWindowBytes += n;
        for (uint32_t i = 0; i < n; i++) feedByte(buf[i]);
    }

    // Retry an acknowledgement the transport refused earlier. This runs BEFORE
    // serviceReliable so the ack goes out ahead of our own retransmissions: the
    // peer is waiting on it, and every frame it waits is a frame closer to it
    // giving up on a message we already have.
    if (m_hasPendingAck) {
        if (buildAndSend(PacketType::EventAck, m_pendingAckSeq, nullptr, 0)) {
            m_hasPendingAck = false;
        }
    }

    servicePing();
    serviceReliable(dt12);

    // LAST, and that ordering is the priority scheme. Acks, ping and reliable
    // traffic have all had the wire by now; whatever room is left goes to position
    // state, which is the only thing here that is free to be late or dropped.
    pumpLatest();
}

// --- measurement ----------------------------------------------------------

void NetLink::advanceClock(int32_t dt12) {
    if (dt12 > 0) m_nowDt += dt12;

    // Publish throughput once per second. A RATE, never a total: cumulative
    // counters can hide a 48:1 send/receive imbalance entirely, where
    // "6.2 KB/s out, 0.13 KB/s in" would have been obvious immediately.
    if (m_nowDt - m_windowStartDt >= c_dtPerSecond) {
        const int32_t elapsed = m_nowDt - m_windowStartDt;
        // Scale to a per-second figure rather than assuming the window was exactly
        // one second; a stalled frame can overshoot it considerably.
        //
        // Deliberately 32-bit throughout. This is freestanding MIPS with no libgcc,
        // so a 64-bit divide is a link error (__udivdi3) rather than slow code.
        // Measuring elapsed time in 1/256 s keeps every intermediate inside 32 bits:
        // the window is at least a second, so the quotient never exceeds the byte
        // count, and both operands stay well under the limit.
        const uint32_t elapsed256 = (static_cast<uint32_t>(elapsed) * 256u) / c_dtPerSecond;
        if (elapsed256 > 0) {
            m_goodput = (m_txWindowBytes * 256u) / elapsed256;
            m_inbound = (m_rxWindowBytes * 256u) / elapsed256;
        }
        m_txWindowBytes = 0;
        m_rxWindowBytes = 0;
        m_windowStartDt = m_nowDt;
    }
}

void NetLink::servicePing() {
    if (m_nowDt < m_pingDueDt) return;

    // Report what WE measured. The peer uses it to pace itself, which is the whole
    // mechanism: neither end has to guess what the link can carry, because the end
    // that can actually observe it says so.
    PingPayload p{};
    p.echo = m_pingEcho + 1;
    p.goodput = m_goodput;
    p.rttMillis = m_srttMs;

    if (!buildAndSend(PacketType::Ping, 0, reinterpret_cast<const uint8_t*>(&p), sizeof(p))) {
        // RETRY NEXT POLL, not next interval, and do not consume the echo.
        //
        // Burning the whole interval on a refusal is catastrophic on exactly the
        // link that most needs measuring. On real hardware, in
        // game the outbound ring sits permanently full, so every Ping was refused,
        // no Ping ever went out, no Pong ever came back, and the console reported
        // "NO PONG" forever while the lobby -- where the ring has slack -- measured
        // a round trip fine. A probe that gives up when the link is busy can only
        // ever describe a link that was not worth probing.
        m_pingDeferred++;
        return;
    }

    m_pingEcho++;
    m_pingDueDt = m_nowDt + c_pingIntervalDt;
    // Only the newest Ping is outstanding. Timing an older one after a newer
    // went out would fold queueing delay into the RTT and inflate it.
    m_pingSentAtDt = m_nowDt;
    m_pingPending = true;
}

void NetLink::handlePing() {
    if (m_payloadLen != sizeof(PingPayload)) return;
    PingPayload in;
    __builtin_memcpy(&in, m_payload, sizeof(in));

    // Remember what the peer told us about ITS side of the link.
    m_peerGoodput = in.goodput;
    m_peerRttMs = in.rttMillis;

    // Echo the token verbatim and attach our own measurements, so one exchange
    // informs both ends.
    PingPayload out{};
    out.echo = in.echo;
    out.goodput = m_goodput;
    out.rttMillis = m_srttMs;
    buildAndSend(PacketType::Pong, 0, reinterpret_cast<const uint8_t*>(&out), sizeof(out));
}

void NetLink::handlePong() {
    if (m_payloadLen != sizeof(PingPayload)) return;
    PingPayload in;
    __builtin_memcpy(&in, m_payload, sizeof(in));

    m_peerGoodput = in.goodput;
    m_peerRttMs = in.rttMillis;

    // Ignore anything but the outstanding token. A stale Pong would measure the
    // wrong interval, and measuring the wrong interval is worse than not measuring.
    if (!m_pingPending || in.echo != m_pingEcho) return;
    m_pingPending = false;

    const int32_t elapsedDt = m_nowDt - m_pingSentAtDt;
    // Reject the absurd as well as the negative. A sample from a link that took
    // ten seconds to answer says nothing useful about its round trip, and folding
    // it in would inflate the RTO for minutes afterwards. It also keeps the
    // conversion below inside 32 bits - there is no 64-bit divide on this target.
    if (elapsedDt < 0 || elapsedDt > 10 * c_dtPerSecond) return;
    updateRtt((static_cast<uint32_t>(elapsedDt) * 1000u) / c_dtPerSecond);
}

// Jacobson/Karels, the standard estimator, in integer milliseconds.
//
//   srtt   += (sample - srtt) / 8
//   rttvar += (|sample - srtt| - rttvar) / 4
//   rto     = srtt + 4 * rttvar
//
// The variance term is what makes this better than an average: a link whose
// latency is erratic gets a longer timeout automatically, so jitter stops
// manufacturing retransmissions that then make the jitter worse.
void NetLink::updateRtt(uint32_t sampleMs) {
    if (m_rttSamples == 0) {
        m_srttMs = static_cast<uint16_t>(sampleMs);
        m_rttVarMs = static_cast<uint16_t>(sampleMs / 2);
    } else {
        const int32_t err = static_cast<int32_t>(sampleMs) - static_cast<int32_t>(m_srttMs);
        const int32_t absErr = err < 0 ? -err : err;
        m_rttVarMs = static_cast<uint16_t>(static_cast<int32_t>(m_rttVarMs) +
                                           ((absErr - static_cast<int32_t>(m_rttVarMs)) / 4));
        m_srttMs = static_cast<uint16_t>(static_cast<int32_t>(m_srttMs) + (err / 8));
    }
    m_rttSamples++;

    uint32_t rto = static_cast<uint32_t>(m_srttMs) + 4u * static_cast<uint32_t>(m_rttVarMs);
    if (rto < c_rtoFloorMs) rto = c_rtoFloorMs;
    if (rto > c_rtoCeilingMs) rto = c_rtoCeilingMs;
    m_rtoMs = static_cast<uint16_t>(rto);
}

void NetLink::feedByte(uint8_t b) {
    switch (m_rxState) {
        case RxState::Sync:
            if (b == c_syncByte) {
                m_rxState = RxState::Header;
                m_hdrGot = 0;
            }
            break;

        case RxState::Header:
            m_hdrBuf[m_hdrGot++] = b;
            if (m_hdrGot == sizeof(PacketHeader)) {
                __builtin_memcpy(&m_hdr, m_hdrBuf, sizeof(PacketHeader));
                if (m_hdr.length > c_maxPayload) {
                    // Impossible length: this is a mis-sync, hunt again.
                    m_resyncs++;
                    m_rxState = RxState::Sync;
                } else {
                    m_payloadLen = m_hdr.length;
                    m_payloadGot = 0;
                    m_crcGot = 0;
                    m_rxState = (m_payloadLen == 0) ? RxState::Crc : RxState::Payload;
                }
            }
            break;

        case RxState::Payload:
            m_payload[m_payloadGot++] = b;
            if (m_payloadGot == m_payloadLen) {
                m_rxState = RxState::Crc;
                m_crcGot = 0;
            }
            break;

        case RxState::Crc:
            m_crcBuf[m_crcGot++] = b;
            if (m_crcGot == 2) {
                uint16_t rxCrc = static_cast<uint16_t>(m_crcBuf[0]) | (static_cast<uint16_t>(m_crcBuf[1]) << 8);
                uint16_t calc = crc16(m_hdrBuf, sizeof(PacketHeader));
                calc = crc16(m_payload, m_payloadLen, calc);
                if (calc == rxCrc) {
                    m_framesRx++;
                    dispatch();
                } else {
                    m_crcErrors++;  // drop and resync
                }
                m_rxState = RxState::Sync;
            }
            break;
    }
}

void NetLink::dispatch() {
    switch (static_cast<PacketType>(m_hdr.type)) {
        case PacketType::Hello:
            if (m_payloadLen == sizeof(HelloPayload)) {
                HelloPayload p;
                __builtin_memcpy(&p, m_payload, sizeof(p));
                m_handler.onHello(p, m_hdr.srcSlot);
            }
            break;
        case PacketType::HelloAck:
            if (m_payloadLen == sizeof(HelloAckPayload)) {
                HelloAckPayload p;
                __builtin_memcpy(&p, m_payload, sizeof(p));
                m_handler.onHelloAck(p);
            }
            break;
        case PacketType::Snapshot:
            m_handler.onSnapshot(m_payload, m_payloadLen, m_hdr.srcSlot, m_hdr.seq);
            break;
        case PacketType::Event:
            handleIncomingReliable(PacketType::Event);
            break;
        case PacketType::ObjectState:
            handleIncomingReliable(PacketType::ObjectState);
            break;
        case PacketType::AppData:
            handleIncomingReliable(PacketType::AppData);
            break;
        case PacketType::EventAck:
            handleEventAck(m_hdr.seq);
            break;
        case PacketType::Ping:
            handlePing();
            break;
        case PacketType::Pong:
            handlePong();
            break;
        case PacketType::Bye:
            m_handler.onPeerBye(m_hdr.srcSlot);
            break;
        default:
            break;
    }
}

void NetLink::handleIncomingReliable(PacketType type) {
    uint16_t seq = m_hdr.seq;
    // Always acknowledge - even duplicates - so the sender can stop retransmitting.
    //
    // AND NEVER LOSE THE ACK. buildAndSend returns false when the TX ring cannot
    // fit the frame, and that return used to be discarded - so an 11-byte
    // acknowledgement could vanish with no trace. The consequence is entirely
    // one-sided: the sender has no idea, retransmits its whole message, exhausts
    // its budget and gives up. On the server that printed as
    // "gave up on reliable seq N after 8 attempts" for a reliable message that the
    // console had in fact received and parsed correctly every single time.
    //
    // So a refused ack is remembered and retried from poll(). Only the newest is
    // kept: the peer is stop-and-wait, so an older seq is by definition already
    // satisfied and re-acking it would tell it nothing.
    if (!buildAndSend(PacketType::EventAck, seq, nullptr, 0)) {
        m_pendingAckSeq = seq;
        m_hasPendingAck = true;
        m_acksDeferred++;
    }

    // Deliver only if this is a new reliable packet (stop-and-wait means at most
    // one new seq at a time, so single-last dedup is exact). Event, ObjectState
    // and AppData all share the same reliable seq stream.
    if (!m_rxHasEvent || seq != m_rxLastEventSeq) {
        m_rxHasEvent = true;
        m_rxLastEventSeq = seq;
        switch (type) {
            case PacketType::ObjectState:
                m_handler.onObjectState(m_payload, m_payloadLen, m_hdr.srcSlot, seq);
                break;
            case PacketType::AppData:
                m_handler.onAppData(m_payload, m_payloadLen, m_hdr.srcSlot, seq);
                break;
            default:
                m_handler.onEvent(m_payload, m_payloadLen, m_hdr.srcSlot, seq);
                break;
        }
    }
}

void NetLink::handleEventAck(uint16_t seq) {
    if (m_inFlight && seq == m_inFlightSeq) {
        // Front event confirmed: drop it and start the next.
        m_eventHead = (m_eventHead + 1) % c_eventQueueSize;
        m_eventCount--;
        m_inFlight = false;
        pumpReliable();
    }
}

// --- send path ------------------------------------------------------------

uint32_t NetLink::buildFrame(uint8_t* out, PacketType type, uint16_t seq, const uint8_t* payload, uint16_t len,
                             uint8_t flags) {
    uint32_t o = 0;
    out[o++] = c_syncByte;

    PacketHeader h{};
    h.type = static_cast<uint8_t>(type);
    h.srcSlot = m_localSlot;
    h.flags = flags;
    h.reserved = 0;
    h.seq = seq;
    h.length = len;
    __builtin_memcpy(out + o, &h, sizeof(h));
    o += sizeof(h);

    if (len && payload) {
        __builtin_memcpy(out + o, payload, len);
        o += len;
    }

    // CRC covers header + payload (everything after the sync byte).
    uint16_t crc = crc16(out + 1, sizeof(h) + len);
    out[o++] = static_cast<uint8_t>(crc & 0xFF);
    out[o++] = static_cast<uint8_t>(crc >> 8);
    return o;
}

bool NetLink::buildAndSend(PacketType type, uint16_t seq, const uint8_t* payload, uint16_t len, uint8_t flags) {
    if (len > c_maxPayload) return false;

    uint8_t frame[c_maxFrameBytes];
    const uint32_t o = buildFrame(frame, type, seq, payload, len, flags);

    // Atomic: a frame goes out whole or not at all. The old partial write left a
    // truncated prefix on the wire whose length header pointed into the NEXT
    // frame, so a single TX-ring overflow cost two frames and a resync.
    if (!m_transport.writeAll(frame, o)) return false;

    // Count only what the transport ACCEPTED. Bytes it refused never reach the
    // wire, and including them would report a throughput the link does not have -
    // which is precisely the kind of comforting wrong number this exists to
    // replace.
    m_txWindowBytes += o;
    return true;
}

bool NetLink::send(PacketType type, const uint8_t* payload, uint16_t len, uint8_t flags) {
    return buildAndSend(type, m_txSeq++, payload, len, flags);
}

bool NetLink::sendLatest(PacketType type, const uint8_t* payload, uint16_t len) {
    if (len > c_maxPayload) return false;

    // Overwrite, never queue. If one was already waiting it is by definition
    // staler than this, and putting the old one on a slow wire ahead of the new
    // one is the exact behaviour that made a console's position updates arrive
    // seconds late while its acknowledgements sat behind them.
    if (m_hasLatest) m_latestSuperseded++;

    m_latestLen = buildFrame(m_latestFrame, type, m_txSeq++, payload, len, 0);
    m_hasLatest = true;
    return true;
}

// Lowest priority by construction: called last in poll(), after the deferred ack,
// the ping and the reliable channel have each had the wire. A frame the transport
// refuses is simply kept - the next sendLatest() will replace it, so a refusal
// costs nothing and nothing accumulates.
//
// AND IT RESERVES ROOM, which ordering alone cannot do. Being sent first is
// worthless if position data has already taken every byte of transport space by
// the time a control frame is built: the ack or ping is then refused on arrival,
// not merely delayed. That is what happens in practice: the
// ring sat permanently full in game, every Ping was refused, and the link could
// not measure itself at all.
//
// So a snapshot may only use space down to c_controlReserveBytes. Position updates
// are the one thing here that is free to be dropped; being dropped so that an
// acknowledgement fits is precisely what they are for.
void NetLink::pumpLatest() {
    if (!m_hasLatest) return;

    // DROPPABLE DATA MAY OCCUPY AT MOST ONE FRAME OF THE OUTBOUND QUEUE.
    //
    // Not "leave a reserve free" - that was the first attempt and it is not
    // enough. A reserve guarantees a control frame is ACCEPTED; it says nothing
    // about how long the frame then waits behind everything already queued. A
    // 1024-byte ring draining at the ~900 B/s a real console achieves is 1.1
    // SECONDS of head-of-line delay, so an acknowledgement admitted into that ring
    // still arrives long after the peer has given up on the message it
    // acknowledges. Latency is set by OCCUPANCY, not by free space.
    //
    // So a snapshot waits until the previous one has essentially drained. Queue
    // occupancy from position data is then bounded by one frame - tens of
    // milliseconds of wire, not seconds - and anything else (an ack, a reliable message, a
    // ping) is behind at most that.
    //
    // Note this also makes snapshots yield to reliable traffic for free: while a
    // 158-byte reliable message is in the queue, occupancy exceeds a snapshot and none are
    // added until it has gone.
    const uint32_t occupancy = m_transport.txCapacity() - m_transport.txSpace();
    if (occupancy > m_latestLen) {
        m_latestDeferred++;
        return;
    }
    if (m_transport.txSpace() < m_latestLen) {
        m_latestDeferred++;
        return;
    }

    if (m_transport.writeAll(m_latestFrame, m_latestLen)) {
        m_txWindowBytes += m_latestLen;
        m_hasLatest = false;
    }
}

bool NetLink::sendReliable(PacketType type, const uint8_t* payload, uint16_t len) {
    if (len > c_maxEventPayload) return false;
    if (m_eventCount == c_eventQueueSize) return false;

    uint32_t tail = (m_eventHead + m_eventCount) % c_eventQueueSize;
    PendingEvent& e = m_eventQueue[tail];
    e.type = static_cast<uint8_t>(type);
    e.len = len;
    if (len && payload) __builtin_memcpy(e.data, payload, len);
    m_eventCount++;

    pumpReliable();
    return true;
}

// Retransmission interval, in dt units, from the MEASURED round trip.
//
// This was a fixed ~100ms, and a fixed timer is wrong in both directions. Shorter
// than the actual round trip, it retransmits messages the peer has already
// acknowledged - the ack is merely still in flight - so it does not repair a
// congested link, it feeds it, and it does so exactly when the link is worst.
// Longer than necessary, it leaves a genuinely lost message sitting for a
// needlessly long time. Only a measurement can be right, and until the first Pong
// arrives this returns the old fixed value, so a peer that never answers Ping
// behaves exactly as before.
int32_t NetLink::resendIntervalDt() const {
    return (static_cast<int32_t>(m_rtoMs) * c_dtPerSecond) / 1000;
}

void NetLink::pumpReliable() {
    if (m_inFlight || m_eventCount == 0) return;
    PendingEvent& e = m_eventQueue[m_eventHead];
    m_inFlightSeq = m_txEventSeq++;
    buildAndSend(static_cast<PacketType>(e.type), m_inFlightSeq, e.data, e.len);
    m_inFlight = true;
    m_resendTimer = resendIntervalDt();
}

void NetLink::serviceReliable(int32_t dt12) {
    if (!m_inFlight) return;
    m_resendTimer -= dt12;
    if (m_resendTimer <= 0) {
        PendingEvent& e = m_eventQueue[m_eventHead];
        buildAndSend(static_cast<PacketType>(e.type), m_inFlightSeq, e.data, e.len);
        m_retransmits++;
        m_resendTimer = resendIntervalDt();
    }
}

}  // namespace psxsplash::net
