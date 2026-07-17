#include "netlink.hh"

namespace psxsplash::net {

void NetLink::reset() {
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
}

// --- receive path ---------------------------------------------------------

void NetLink::poll() {
    m_transport.poll();

    uint8_t buf[128];
    uint32_t n;
    while ((n = m_transport.read(buf, sizeof(buf))) > 0) {
        for (uint32_t i = 0; i < n; i++) feedByte(buf[i]);
    }

    serviceReliable();
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
            break;  // liveness only; presence of a valid frame is the signal
        case PacketType::Bye:
            m_handler.onPeerBye(m_hdr.srcSlot);
            break;
        default:
            break;
    }
}

void NetLink::handleIncomingReliable(PacketType type) {
    uint16_t seq = m_hdr.seq;
    // Always acknowledge — even duplicates — so the sender can stop retransmitting.
    buildAndSend(PacketType::EventAck, seq, nullptr, 0);

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

bool NetLink::buildAndSend(PacketType type, uint16_t seq, const uint8_t* payload, uint16_t len, uint8_t flags) {
    if (len > c_maxPayload) return false;

    uint8_t frame[1 + sizeof(PacketHeader) + c_maxPayload + 2];
    uint32_t o = 0;
    frame[o++] = c_syncByte;

    PacketHeader h{};
    h.type = static_cast<uint8_t>(type);
    h.srcSlot = m_localSlot;
    h.flags = flags;
    h.reserved = 0;
    h.seq = seq;
    h.length = len;
    __builtin_memcpy(frame + o, &h, sizeof(h));
    o += sizeof(h);

    if (len && payload) {
        __builtin_memcpy(frame + o, payload, len);
        o += len;
    }

    // CRC covers header + payload (everything after the sync byte).
    uint16_t crc = crc16(frame + 1, sizeof(h) + len);
    frame[o++] = static_cast<uint8_t>(crc & 0xFF);
    frame[o++] = static_cast<uint8_t>(crc >> 8);

    return m_transport.write(frame, o) == o;
}

bool NetLink::send(PacketType type, const uint8_t* payload, uint16_t len, uint8_t flags) {
    return buildAndSend(type, m_txSeq++, payload, len, flags);
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

void NetLink::pumpReliable() {
    if (m_inFlight || m_eventCount == 0) return;
    PendingEvent& e = m_eventQueue[m_eventHead];
    m_inFlightSeq = m_txEventSeq++;
    buildAndSend(static_cast<PacketType>(e.type), m_inFlightSeq, e.data, e.len);
    m_inFlight = true;
    m_resendTimer = c_resendInterval;
}

void NetLink::serviceReliable() {
    if (!m_inFlight) return;
    if (m_resendTimer > 0) m_resendTimer--;
    if (m_resendTimer == 0) {
        PendingEvent& e = m_eventQueue[m_eventHead];
        buildAndSend(static_cast<PacketType>(e.type), m_inFlightSeq, e.data, e.len);
        m_retransmits++;
        m_resendTimer = c_resendInterval;
    }
}

}  // namespace psxsplash::net
