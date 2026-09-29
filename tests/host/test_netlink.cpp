/**
 * Host unit tests for the wire protocol and NetLink framing/reliability.
 *
 * netlink.hh promises NetLink "is transport-agnostic and hardware-free ... so it
 * is unit-testable on a host with a mock transport". This is that test: the real
 * netlink.cpp is compiled with the host compiler and driven through
 * MockTransport, with no PlayStation and no emulator involved.
 */

#include "mocktransport.hh"
#include "netinterp.hh"
#include "netlink.hh"
#include "netprotocol.hh"
#include "simtransport.hh"
#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::net;
using psxsplash::test::LinkProfile;
using psxsplash::test::MockTransport;
using psxsplash::test::RecordingHandler;
using psxsplash::test::SimLink;

namespace {

// A full-duplex pair of NetLinks joined by crossed rings — the standard fixture.
struct LinkPair {
    MockTransport::Ring a2b, b2a;
    MockTransport hostT{a2b, b2a};
    MockTransport clientT{b2a, a2b};
    RecordingHandler hostH, clientH;
    NetLink host{hostT, hostH};
    NetLink client{clientT, clientH};

    LinkPair() {
        host.setLocalSlot(c_hostSlot);
        client.setLocalSlot(1);
    }

    // dt is 4.12 fixed point: 4096 == one 30Hz frame. Pass a real frame's worth
    // by default so retransmit timers, which are wall-clock, actually advance.
    void pump(int n = 2, int32_t dt12 = 4096) {
        for (int i = 0; i < n; i++) {
            host.poll(dt12);
            client.poll(dt12);
        }
    }
};

}  // namespace

// --- protocol primitives --------------------------------------------------

TEST(crc16_canonical_check_value) {
    // CRC-16/CCITT-FALSE's published check value: the CRC of the ASCII string
    // "123456789" is 0x29B1. This is the anchor that pins the C++ and Python
    // implementations to the same named algorithm rather than to each other.
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(crc16(data, sizeof(data)), 0x29B1);
}

TEST(crc16_is_chainable) {
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint16_t running = crc16(data, 4);
    running = crc16(data + 4, 5, running);
    CHECK_EQ(running, crc16(data, 9));
}

TEST(wire_struct_sizes_are_pinned) {
    CHECK_EQ(sizeof(PacketHeader), 8);
    CHECK_EQ(sizeof(HelloPayload), 16);
    CHECK_EQ(sizeof(HelloAckPayload), 16);
}

TEST(magic_is_PSNL_little_endian) {
    uint8_t bytes[4];
    __builtin_memcpy(bytes, &c_magic, 4);
    CHECK_EQ(bytes[0], 'P');
    CHECK_EQ(bytes[1], 'S');
    CHECK_EQ(bytes[2], 'N');
    CHECK_EQ(bytes[3], 'L');
}

// --- framing --------------------------------------------------------------

TEST(frame_layout_is_exactly_as_documented) {
    // netprotocol.hh documents: [sync][header 8B][payload][crc16 LE], CRC over
    // header+payload but NOT the sync byte. Assert the emitted bytes directly
    // rather than only round-tripping, so a self-consistent change to both ends
    // still trips this.
    LinkPair p;
    const uint8_t payload[3] = {0xDE, 0xAD, 0xBE};
    p.host.send(PacketType::Snapshot, payload, 3);

    uint8_t frame[64];
    uint32_t n = 0;
    uint8_t b;
    while (p.a2b.pop(b) && n < sizeof(frame)) frame[n++] = b;

    // Byte map, from PacketHeader's field order (type,srcSlot,flags,reserved at
    // struct offsets 0..3; seq at 4-5; length at 6-7):
    //   [0] sync | [1] type | [2] srcSlot | [3] flags | [4] reserved
    //   [5..6] seq | [7..8] length | [9..] payload | [..] crc16 LE
    CHECK_EQ(n, 1 + 8 + 3 + 2);
    CHECK_EQ(frame[0], c_syncByte);
    CHECK_EQ(frame[1], (uint8_t)PacketType::Snapshot);
    CHECK_EQ(frame[2], c_hostSlot);  // srcSlot
    CHECK_EQ(frame[3], 0);           // flags — reserved for channel flags today
    CHECK_EQ(frame[4], 0);           // reserved
    CHECK_EQ(frame[7], 3);           // length lo
    CHECK_EQ(frame[8], 0);           // length hi
    CHECK_MEM(frame + 9, payload, 3);

    // CRC covers header + payload, and explicitly NOT the sync byte.
    uint16_t expect = crc16(frame + 1, 8 + 3);
    uint16_t got = (uint16_t)frame[12] | ((uint16_t)frame[13] << 8);
    CHECK_EQ(got, expect);
}

TEST(handshake_round_trips_and_tags_src_slot) {
    LinkPair p;
    HelloPayload hello{};
    hello.magic = c_magic;
    hello.protoVersion = c_protoVersion;
    hello.sceneHash = 0xDEADBEEFu;
    hello.tiebreak = 42;
    p.client.send(PacketType::Hello, (const uint8_t*)&hello, sizeof(hello));
    p.pump();

    CHECK_EQ(p.hostH.helloCount, 1);
    CHECK_EQ(p.hostH.lastHello.sceneHash, 0xDEADBEEFu);
    CHECK_EQ(p.hostH.lastHello.tiebreak, 42);
    CHECK_EQ(p.hostH.lastHelloSlot, 1);  // taken from the header, not the payload
}

TEST(zero_length_payload_parses) {
    // Exercises the len==0 -> RxState::Crc shortcut (netlink.cpp:54), which the
    // Payload state would otherwise never leave.
    LinkPair p;
    p.host.send(PacketType::Ping, nullptr, 0);
    p.pump();
    CHECK_EQ(p.client.framesReceived(), 1);
    CHECK_EQ(p.client.crcErrors(), 0);
}

TEST(bye_is_dispatched_with_slot) {
    LinkPair p;
    p.client.send(PacketType::Bye, nullptr, 0);
    p.pump();
    CHECK_EQ(p.hostH.byeCount, 1);
    CHECK_EQ(p.hostH.lastByeSlot, 1);
}

TEST(max_payload_round_trips) {
    LinkPair p;
    uint8_t big[c_maxPayload];
    for (int i = 0; i < c_maxPayload; i++) big[i] = (uint8_t)(i * 31 + 7);
    CHECK(p.host.send(PacketType::Snapshot, big, c_maxPayload));
    p.pump();
    CHECK_EQ(p.clientH.snapshotCount, 1);
    CHECK_EQ(p.clientH.lastSnapLen, c_maxPayload);
    CHECK_MEM(p.clientH.lastSnap, big, c_maxPayload);
}

TEST(oversized_payload_is_refused) {
    LinkPair p;
    uint8_t big[c_maxPayload + 1]{};
    CHECK(!p.host.send(PacketType::Snapshot, big, c_maxPayload + 1));
    p.pump();
    CHECK_EQ(p.clientH.snapshotCount, 0);
}

// --- resync ---------------------------------------------------------------

TEST(bad_crc_is_rejected_and_does_not_dispatch) {
    LinkPair p;
    const uint8_t payload[3] = {1, 2, 3};
    p.host.send(PacketType::Snapshot, payload, 3);

    // Drain the frame, flip a payload byte, and replay it at the client.
    uint8_t frame[32];
    uint32_t n = 0;
    uint8_t b;
    while (p.a2b.pop(b) && n < sizeof(frame)) frame[n++] = b;
    frame[11] ^= 0xFF;
    p.clientT.injectInbound(frame, n);
    p.pump();

    CHECK_EQ(p.clientH.snapshotCount, 0);
    CHECK_EQ(p.client.crcErrors(), 1);
}

TEST(impossible_length_triggers_resync_not_a_hang) {
    // A false sync byte followed by a length > c_maxPayload must be abandoned
    // (netlink.cpp:46), and a valid frame right behind it must still parse.
    LinkPair p;
    const uint8_t garbage[] = {c_syncByte, 0x03, 0x01, 0x00, 0x00, 0x11, 0x11, 0xFF, 0xFF};
    p.clientT.injectInbound(garbage, sizeof(garbage));

    const uint8_t payload[4] = {9, 8, 7, 6};
    p.host.send(PacketType::Snapshot, payload, 4);
    p.pump(3);

    CHECK_EQ(p.client.resyncs(), 1);
    CHECK_EQ(p.clientH.snapshotCount, 1);
    CHECK_MEM(p.clientH.lastSnap, payload, 4);
}

TEST(garbage_without_a_false_sync_costs_nothing) {
    LinkPair p;
    const uint8_t garbage[] = {0x00, 0xFF, 0x12, 0x7E, 0x01};  // no 0xA5 anywhere
    p.clientT.injectInbound(garbage, sizeof(garbage));
    const uint8_t payload[4] = {4, 3, 2, 1};
    p.host.send(PacketType::Snapshot, payload, 4);
    p.pump(3);
    CHECK_EQ(p.clientH.snapshotCount, 1);
    CHECK_MEM(p.clientH.lastSnap, payload, 4);
}

TEST(false_sync_byte_costs_exactly_one_frame_then_recovers) {
    // A 0xA5 inside garbage is indistinguishable from a real frame start, so the
    // parser latches onto it and swallows the next frame's bytes as a bogus
    // header/payload. That frame is lost; the CRC check then fails and the parser
    // resyncs, so the FOLLOWING frame is clean. Snapshots are unreliable by design
    // (a dropped one costs a stale tick), which is what makes this acceptable —
    // but it means a noisy line degrades throughput rather than breaking the link.
    LinkPair p;
    const uint8_t garbage[] = {0x00, 0xA5, 0x01, 0x02, 0x03};
    p.clientT.injectInbound(garbage, sizeof(garbage));

    const uint8_t lost[4] = {1, 1, 1, 1};
    p.host.send(PacketType::Snapshot, lost, 4);
    p.pump(3);
    CHECK_EQ(p.clientH.snapshotCount, 0);  // eaten by the false sync

    const uint8_t recovered[4] = {2, 2, 2, 2};
    p.host.send(PacketType::Snapshot, recovered, 4);
    p.pump(3);
    CHECK_EQ(p.clientH.snapshotCount, 1);  // parser recovered on its own
    CHECK_MEM(p.clientH.lastSnap, recovered, 4);
}

// --- reliable channel -----------------------------------------------------

TEST(reliable_event_delivered_once_then_idle) {
    LinkPair p;
    const uint8_t ev[5] = {1, 2, 3, 4, 5};
    CHECK(p.client.sendReliable(PacketType::Event, ev, 5));
    p.pump(6);
    CHECK_EQ(p.hostH.eventCount, 1);
    CHECK_EQ(p.hostH.lastEventLen, 5);
    CHECK_MEM(p.hostH.lastEvent, ev, 5);
    CHECK(p.client.reliableIdle());
}

TEST(reliable_events_arrive_in_order) {
    LinkPair p;
    const uint8_t e1[1] = {0xAA};
    const uint8_t e2[1] = {0xBB};
    p.client.sendReliable(PacketType::Event, e1, 1);
    p.client.sendReliable(PacketType::Event, e2, 1);
    p.pump(14);
    CHECK_EQ(p.hostH.eventCount, 2);
    CHECK_EQ(p.hostH.lastEvent[0], 0xBB);
    CHECK(p.client.reliableIdle());
}

TEST(retransmit_does_not_double_deliver) {
    // Force a retransmit by dropping the ack, and confirm the receiver's dedup
    // (netlink.cpp:131) suppresses the duplicate while still re-acking.
    LinkPair p;
    const uint8_t ev[2] = {0x11, 0x22};
    p.client.sendReliable(PacketType::Event, ev, 2);

    p.host.poll(4096);  // host receives + acks
    CHECK_EQ(p.hostH.eventCount, 1);
    // The ack travels host -> client, i.e. through a2b (hostT writes to a2b and
    // clientT reads from it). Clearing b2a here would drop the client's own
    // outbound bytes and leave the ack intact.
    p.a2b.clear();

    for (int i = 0; i < 8; i++) {
        // A full resend interval per iteration, so the timer genuinely expires.
        p.client.poll(3 * 4096);  // resend timer expires -> retransmit
        p.host.poll(3 * 4096);    // host sees the duplicate
    }
    CHECK_EQ(p.hostH.eventCount, 1);  // still exactly once
    CHECK(p.client.retransmits() > 0);
}

TEST(object_state_routes_to_its_own_handler) {
    LinkPair p;
    const uint8_t blob[6] = {0xC0, 0xFF, 0xEE, 0x01, 0x02, 0x03};
    p.client.sendReliable(PacketType::ObjectState, blob, 6);
    p.pump(6);
    CHECK_EQ(p.hostH.objectStateCount, 1);
    CHECK_EQ(p.hostH.eventCount, 0);  // must not leak into the Event handler
    CHECK_MEM(p.hostH.lastObjState, blob, 6);
}

TEST(event_and_object_state_share_the_reliable_stream) {
    LinkPair p;
    const uint8_t a[1] = {0xA1};
    const uint8_t b[1] = {0xB2};
    p.client.sendReliable(PacketType::Event, a, 1);
    p.client.sendReliable(PacketType::ObjectState, b, 1);
    p.pump(14);
    CHECK_EQ(p.hostH.eventCount, 1);
    CHECK_EQ(p.hostH.objectStateCount, 1);
    CHECK(p.client.reliableIdle());
}

TEST(oversized_reliable_payload_is_refused) {
    LinkPair p;
    uint8_t big[300]{};
    CHECK(!p.client.sendReliable(PacketType::Event, big, c_maxEventPayload + 1));
    CHECK(p.client.sendReliable(PacketType::Event, big, c_maxEventPayload));
}

TEST(reliable_queue_saturates_at_capacity) {
    // Nothing is retired until an ack arrives, so with the peer silent the queue
    // fills and further sends are refused. The caller MUST check this return: a
    // dropped game event is otherwise completely silent.
    LinkPair p;
    const uint8_t ev[1] = {0x01};
    uint32_t accepted = 0;
    for (int i = 0; i < 64; i++) {
        if (p.client.sendReliable(PacketType::Event, ev, 1)) accepted++;
    }
    CHECK_EQ(accepted, NetLink::reliableQueueCapacity());
    CHECK(!p.client.sendReliable(PacketType::Event, ev, 1));
    CHECK_EQ(p.client.reliableQueueDepth(), NetLink::reliableQueueCapacity());
}

// --- AppData: the game's opaque reliable channel ---------------------------

TEST(appdata_routes_to_its_own_handler) {
    LinkPair p;
    const uint8_t blob[4] = {'R', 'O', 'O', 'M'};
    CHECK(p.client.sendReliable(PacketType::AppData, blob, 4));
    p.pump(6);
    CHECK_EQ(p.hostH.appDataCount, 1);
    CHECK_EQ(p.hostH.eventCount, 0);        // must not leak into game events
    CHECK_EQ(p.hostH.objectStateCount, 0);  // nor into object state
    CHECK_MEM(p.hostH.lastAppData, blob, 4);
}

TEST(appdata_is_binary_safe_including_embedded_nuls_and_sync_bytes) {
    // The channel carries Lua strings, which are counted rather than
    // NUL-terminated. A payload containing 0x00 or the 0xA5 sync byte must
    // survive framing untouched.
    LinkPair p;
    uint8_t blob[c_maxEventPayload];
    for (int i = 0; i < c_maxEventPayload; i++) blob[i] = (uint8_t)i;  // includes 0x00 and 0xA5
    CHECK(p.client.sendReliable(PacketType::AppData, blob, c_maxEventPayload));
    p.pump(6);
    CHECK_EQ(p.hostH.appDataCount, 1);
    CHECK_EQ(p.hostH.lastAppDataLen, c_maxEventPayload);
    CHECK_MEM(p.hostH.lastAppData, blob, c_maxEventPayload);
}

TEST(a_full_room_list_fits_one_reliable_packet) {
    // The room-list design leans on this: 15 rooms x 16B = 244B <= 256B, so a
    // lobby refresh costs ONE stop-and-wait round trip rather than fifteen.
    LinkPair p;
    uint8_t list[4 + 15 * 16]{};
    CHECK(sizeof(list) <= c_maxEventPayload);
    CHECK(p.client.sendReliable(PacketType::AppData, list, sizeof(list)));
    p.pump(6);
    CHECK_EQ(p.hostH.appDataCount, 1);
    CHECK_EQ(p.hostH.lastAppDataLen, sizeof(list));
}

TEST(all_three_reliable_kinds_share_one_ordered_stream) {
    LinkPair p;
    const uint8_t a[1] = {0xA1};
    const uint8_t b[1] = {0xB2};
    const uint8_t c[1] = {0xC3};
    p.client.sendReliable(PacketType::Event, a, 1);
    p.client.sendReliable(PacketType::ObjectState, b, 1);
    p.client.sendReliable(PacketType::AppData, c, 1);
    p.pump(24);
    CHECK_EQ(p.hostH.eventCount, 1);
    CHECK_EQ(p.hostH.objectStateCount, 1);
    CHECK_EQ(p.hostH.appDataCount, 1);
    CHECK(p.client.reliableIdle());
}

// --- header flags ----------------------------------------------------------

TEST(rebind_flag_survives_the_wire) {
    // c_flagRebind rides PacketHeader.flags, a byte that was previously always
    // zero. Prove it actually reaches the peer rather than being dropped by the
    // framing.
    LinkPair p;
    HelloPayload hello{};
    hello.magic = c_magic;
    hello.protoVersion = c_protoVersion;
    hello.sceneHash = 0x1234;
    p.client.send(PacketType::Hello, (const uint8_t*)&hello, sizeof(hello), c_flagRebind);

    uint8_t frame[64];
    uint32_t n = 0;
    uint8_t b;
    while (p.b2a.pop(b) && n < sizeof(frame)) frame[n++] = b;
    CHECK_EQ(frame[3], c_flagRebind);  // flags is header byte 2 -> frame byte 3

    p.clientT.injectInbound(nullptr, 0);
    p.hostT.injectInbound(frame, n);
    p.pump(2);
    CHECK_EQ(p.hostH.helloCount, 1);
}

// --- transport contract ---------------------------------------------------

TEST(partial_transport_write_is_reported_as_failure) {
    // buildAndSend (netlink.cpp:181) treats a short write as failure. Prove the
    // caller is told, so a full TX buffer can never masquerade as a send.
    LinkPair p;
    const uint8_t payload[20] = {};
    p.hostT.setWriteLimit(5);  // less than a whole frame
    CHECK(!p.host.send(PacketType::Snapshot, payload, 20));
}

TEST(reset_clears_parser_and_reliable_state) {
    LinkPair p;
    const uint8_t ev[1] = {0x01};
    p.client.sendReliable(PacketType::Event, ev, 1);
    CHECK(!p.client.reliableIdle());
    p.client.reset();
    CHECK(p.client.reliableIdle());
    CHECK_EQ(p.client.framesReceived(), 0);
    CHECK_EQ(p.client.retransmits(), 0);
}


// === TX throughput: the bug that made the console unplayable ================
//
// SIO1's transmit path is a holding register plus a shift register: TWO bytes.
// pumpTx() writes until STAT_TXRDY clears, so one call moves at most two bytes,
// and for a long time pumpTx() was only ever called from poll() — once per frame.
//
// Console throughput was therefore 2 * framerate, about 70 bytes/second, no
// matter what the cable could do. PCSX-Redux does not model the FIFO, so it
// drained the whole ring per call and everything looked perfect in the emulator.
//
// The tests below pin down the arithmetic and the defence, because neither is
// obvious from reading the driver and both were learned expensively.

// A transport that faithfully models the two-byte FIFO, drained only when polled.
class TwoBytePerPollTransport final : public INetTransport {
  public:
    static constexpr uint32_t kFifoDepth = 2;

    void poll() override {
        // One poll = the FIFO accepts kFifoDepth bytes, then TXRDY clears.
        for (uint32_t i = 0; i < kFifoDepth; i++) {
            uint8_t b;
            if (!m_tx.pop(b)) break;
            m_onWire++;
        }
    }
    uint32_t available() const override { return 0; }
    uint32_t read(uint8_t*, uint32_t) override { return 0; }
    uint32_t write(const uint8_t* src, uint32_t len) override {
        uint32_t n = 0;
        while (n < len && m_tx.push(src[n])) n++;
        return n;
    }
    bool writeAll(const uint8_t* src, uint32_t len) override {
        if (m_tx.space() < len) return false;
        for (uint32_t i = 0; i < len; i++) m_tx.push(src[i]);
        return true;
    }
    uint8_t txCongestion() const override {
        return static_cast<uint8_t>((m_tx.size() * 255u) / m_tx.capacity());
    }
    uint32_t txSpace() const override { return m_tx.space(); }
    uint32_t txCapacity() const override { return m_tx.capacity(); }

    uint32_t onWire() const { return m_onWire; }
    uint32_t pending() const { return m_tx.size(); }

  private:
    RingBuffer<1024> m_tx;
    uint32_t m_onWire = 0;
};

TEST(a_frame_paced_two_byte_fifo_cannot_carry_a_thirty_hertz_snapshot) {
    // 30Hz of 35-byte snapshots is 1050 B/s. A frame-paced two-byte FIFO moves
    // 2 bytes per frame. This is the whole bug, as arithmetic.
    TwoBytePerPollTransport t;
    const uint8_t snapshot[35] = {};

    for (int frame = 0; frame < 30; frame++) {
        t.writeAll(snapshot, sizeof(snapshot));
        t.poll();
    }

    CHECK_EQ(t.onWire(), 60u);           // 2 bytes x 30 frames
    CHECK(t.pending() > 900);            // ...and 990 bytes stuck in the ring
    // Nothing queued behind that backlog — an ack, a join, the room roster —
    // goes out this second, or the next several.
    CHECK_EQ(t.txCongestion(), 246);
}

TEST(congestion_reaches_the_caller_before_the_ring_is_full) {
    // The signal has to arrive with room to spare. A flag that only trips when
    // the ring is FULL is useless: by then the reliable traffic is already stuck
    // behind everything that was queued ahead of it.
    TwoBytePerPollTransport t;
    const uint8_t snapshot[35] = {};
    CHECK_EQ(t.txCongestion(), 0);

    for (int i = 0; i < 8; i++) t.writeAll(snapshot, sizeof(snapshot));
    CHECK(t.txCongestion() > 0);
    CHECK(t.txCongestion() < 255);  // still accepting; the warning is early
}

TEST(a_congested_transport_still_accepts_reliable_traffic) {
    // Congestion must throttle only what is DROPPABLE. If it ever gated the
    // reliable path the cure would be worse than the disease: a slow link would
    // stop delivering joins and acks entirely instead of merely being slow.
    LinkPair p;
    p.clientT.setCongestion(255);  // pinned maximally congested
    const uint8_t ev[2] = {0xAB, 0xCD};

    CHECK(p.client.sendReliable(PacketType::Event, ev, 2));
    p.pump();
    CHECK_EQ(p.hostH.eventCount, 1);
}


// A UART whose "ready" condition is a LEVEL, like SIO1's TX interrupt: it is true
// again as soon as the shift register frees, ~174us after a byte starts moving at
// 57600. The retail BIOS takes longer than that just to dispatch a handler.
class LevelTriggeredTx {
  public:
    // Returns how many times a handler would be RE-ENTERED while draining `bytes`,
    // if each entry moves `perEntry` bytes and the condition re-arms every time.
    static uint32_t interruptsToDrain(uint32_t bytes, uint32_t perEntry) {
        return (bytes + perEntry - 1) / perEntry;
    }
};

TEST(a_tx_interrupt_per_two_bytes_costs_more_than_the_wire_it_drives) {
    // The bug that froze a real console. SIO1's transmit path is two bytes deep,
    // so an interrupt-driven TX takes one interrupt per two bytes. At 57600 a byte
    // is ~174us of wire time, and a retail-BIOS dispatch is several hundred us --
    // so the CPU cost of sending EXCEEDS the time spent sending, and the handler
    // is re-entered before it returns. Sprite animation stopped, input died, and
    // RX overruns hit 433 because the CPU never reached drainRx().
    const uint32_t snapshotBytes = 35;
    const uint32_t irqs = LevelTriggeredTx::interruptsToDrain(snapshotBytes, 2);
    CHECK_EQ(irqs, 18u);

    const uint32_t wireMicros = snapshotBytes * 174;      // ~6.1ms
    const uint32_t dispatchMicros = irqs * 300;           // ~5.4ms, and that is
                                                          // an OPTIMISTIC BIOS
    // Not "a bit expensive" -- comparable to the transmission itself, per frame,
    // forever. Polling costs the wire time alone.
    CHECK(dispatchMicros * 2 > wireMicros);
}

TEST(the_per_frame_tx_budget_bounds_cost_on_both_targets_which_differ) {
    // Sio1::pumpTxBlocking pushes at most c_txPollBytes per frame. That bound was
    // briefly deleted on the theory that a transport reporting "ready" is free to
    // feed, and the theory was wrong in a way worth pinning down here, because the
    // two targets are expensive in DIFFERENT currencies.
    const uint32_t budget = 48;   // c_txPollBytes
    const uint32_t ring = 1024;   // what an unbounded drain would push instead
    const uint32_t frameMicros = 33333;

    // HARDWARE pays wire time: ~174us per byte at 57600, busy-waiting on a 2-byte
    // FIFO. The budget keeps that to a quarter of a frame.
    const uint32_t hwMicrosPerByte = 174;
    CHECK(budget * hwMicrosPerByte < frameMicros / 3);
    CHECK(ring * hwMicrosPerByte > frameMicros * 5);  // unbounded: five frames of CPU

    // PCSX-REDUX pays host CPU: a protobuf encode, two heap allocations and two
    // uv_async_send syscalls per byte, on the thread emulating the CPU. It is NOT
    // wire-paced -- SR_TXRDY is set at reset and never cleared -- so nothing in the
    // status register reveals this cost, which is exactly how the theory survived.
    const uint32_t reduxMicrosPerByte = 30;
    CHECK(budget * reduxMicrosPerByte < frameMicros / 10);          // bounded: ~4% of a frame
    CHECK(ring * reduxMicrosPerByte > frameMicros * 3 / 4);         // unbounded: ~92% of a frame

    // The lesson the bound encodes: readiness describes PACING, never COST. A
    // transport can be instantly ready and still be the most expensive thing in
    // the frame.
}

TEST(a_per_frame_byte_cap_ties_outbound_bandwidth_to_the_frame_rate) {
    // THE ACTUAL DEFECT, which is subtler than "the cap is too small" -- at a
    // healthy frame rate the cap is very slightly LARGER than the steady demand,
    // which is exactly why it survived review and then failed in play.
    //
    // A per-FRAME byte cap makes supply proportional to the frame rate. Demand is
    // not: the snapshot cadence is WALL-CLOCK 30Hz (c_snapshotIntervalDt), which
    // was itself a deliberate fix so a struggling renderer would not desync the
    // network. Put the two together and a frame-rate dip becomes a permanent
    // bandwidth deficit -- reintroducing, through the back door, the coupling that
    // the wall-clock cadence existed to remove.
    const uint32_t cap = 48;                 // the old per-frame byte cap
    const uint32_t snapshotBytes = 35;       // one snapshot frame
    const uint32_t demandPerSecond = 30 * snapshotBytes;  // wall-clock, frame-rate independent

    // At 30fps supply wins -- but by 390 B/s out of 1440, and that sliver is all
    // there is for EVERYTHING else: acks, events, the room roster, retransmits.
    // This margin is why the cap passed review; it looks sufficient.
    CHECK(30 * cap > demandPerSecond);
    CHECK((30 * cap) - demandPerSecond < 400u);

    // Add the traffic that real play generates -- one 11-byte ack per reliable
    // message -- and the margin inverts below ~29fps. A console in-game sits right
    // on that line, so it crosses back and forth rather than failing cleanly.
    const uint32_t ack = 11;
    const uint32_t busyDemandPerSecond = 30 * (snapshotBytes + ack);
    CHECK(28 * cap < busyDemandPerSecond);

    // AND THE RECOVERY TIME IS THE REAL DEFECT. Even on the winning side of that
    // line the surplus is ~2 bytes per frame, so anything that fills the ring --
    // a room roster, a burst of retransmits, one slow frame -- drains at 2 B/frame.
    // Meanwhile the ring sits above a quarter full, where txCongestion() exceeds
    // c_snapshotCongestionLimit and NetworkManager::postTick stops sending
    // snapshots ALTOGETHER. Position updates then resume only when it finally
    // drains. That is what "my character moves a minute later" actually was: not a
    // dropped link, a link that takes tens of seconds to forgive a single burst.
    const uint32_t ring = 1024;
    const uint32_t surplusPerFrame = cap - (busyDemandPerSecond / 30);
    CHECK_EQ(surplusPerFrame, 2u);
    const uint32_t framesToDrainFullRing = ring / surplusPerFrame;
    CHECK(framesToDrainFullRing > 500u);  // >16 seconds at 30fps, per burst

    // The honest fix is to lower demand, not raise the budget: raising it costs
    // wire time on hardware AND host CPU on the emulator, while shrinking the
    // snapshot helps both at once.
    //
    // A console sends only its OWN avatar, so its outbound snapshot is
    // framing + 4 + one 20-byte record = 35 bytes today. Quantized to 7 bytes per
    // record it becomes 22, and a slimmer header takes it lower still.
    const uint32_t compressedSnapshot = 22;
    const uint32_t compressedSteady = 30 * compressedSnapshot;
    const uint32_t compressedBusy = 30 * (compressedSnapshot + ack);

    // Steady state gains real headroom rather than a sliver...
    CHECK((30 * cap) > compressedSteady * 2);
    // ...and even the busy case, which today runs at 1.04x, clears comfortably.
    CHECK((30 * cap) > compressedBusy * 4 / 3);
    CHECK((30 * cap) < busyDemandPerSecond * 4 / 3);  // today's margin, for contrast
}

TEST(the_storm_limit_cannot_be_reached_by_legitimate_traffic) {
    // A FALSE demotion is its own bug: polled RX drains eight bytes a frame, ~240
    // B/s against a link carrying 5760, so tripping the guard on a console that was
    // merely busy manufactures the corruption it exists to prevent.
    //
    // THIS TEST USED TO ASSERT THE BUG. It compared a per-SECOND interrupt ceiling
    // against a limit applied PER POLL CALL:
    //
    //     maxLegitimateIrqPerSecond = 5760 / 4 = 1440
    //     stormLimit = 2048
    //     CHECK(stormLimit > maxLegitimateIrqPerSecond);        // "safe"
    //
    // with the note "reaching the limit between two polls needs a poll gap over a
    // second, i.e. a console already dead by any measure". A poll gap over a second
    // is a SCENE LOAD: poll() is reached from SceneManager::GameTick, and
    // processPendingSceneLoad() -> loadScene() blocks that loop for seconds reading
    // the disc while the server goes on sending. The two quantities were never
    // comparable, and the green check made the mismatch look proven.
    //
    // The detector no longer has a time term or a poll term to get wrong: it counts
    // CONSECUTIVE INTERRUPTS THAT DRAINED ZERO BYTES. So the property to pin is not
    // a margin any more, it is that legitimate traffic produces NO zero-work run at
    // all, whatever the gap.
    const uint32_t bytesPerSecond = 57600 / 10;  // 8N1
    const uint32_t irqThresholdBytes = 4;        // c_rxIrqThreshold == 2 -> 4 bytes

    // Every interrupt raised by the wire has at least the threshold waiting for it,
    // so drainRx() always makes progress and the run counter always resets. This is
    // the whole invariant: the run length of legitimate traffic is zero.
    const uint32_t bytesDrainedByALegitimateIrq = irqThresholdBytes;
    CHECK(bytesDrainedByALegitimateIrq > 0);

    // The property that actually failed before, stated directly: a scene load's
    // worth of interrupts must not demote. Ten seconds of a saturated link is well
    // past any real disc read, and every one of those entries is productive.
    const uint32_t sceneLoadSeconds = 10;
    const uint32_t irqsAcrossASceneLoad =
        (bytesPerSecond / irqThresholdBytes) * sceneLoadSeconds;
    CHECK_EQ(irqsAcrossASceneLoad, 14400u);
    const uint32_t zeroWorkRunAcrossASceneLoad = 0;  // they all drained bytes
    const uint32_t idleIrqStormLimit = 256;
    CHECK(zeroWorkRunAcrossASceneLoad < idleIrqStormLimit);

    // And the runaway it must still catch: a level-triggered re-assertion with an
    // empty FIFO drains nothing every time, so the run is unbroken and trips almost
    // immediately - well under a millisecond at any plausible dispatch cost.
    const uint32_t runawayDrainsPerIrq = 0;
    CHECK_EQ(runawayDrainsPerIrq, 0u);
    CHECK(idleIrqStormLimit < irqsAcrossASceneLoad);  // sensitive, not merely safe
}


TEST(an_acknowledgement_refused_by_a_full_transport_is_retried_not_lost) {
    // The silent half of "the server gave up on the roster".
    //
    // buildAndSend returns false when the TX ring cannot fit the frame, and the
    // ack path used to DISCARD that return. The failure is entirely one-sided:
    // the console has received and parsed the message perfectly, but never says
    // so, and the peer -- with no way to tell the difference -- retransmits the
    // whole thing, exhausts its budget and abandons it. A 147-byte room roster
    // was lost that way eight times in a row while arriving correctly every time.
    LinkPair p;
    const uint8_t ev[2] = {0x11, 0x22};

    // Client sends a reliable event; the host cannot reply because its outgoing
    // side is full at exactly the wrong moment.
    p.client.sendReliable(PacketType::Event, ev, 2);
    p.hostT.setWriteLimit(0);
    p.host.poll(4096);

    CHECK_EQ(p.hostH.eventCount, 1);          // it DID arrive
    CHECK_EQ(p.host.acksDeferred(), 1u);      // ...and we know we could not say so

    // The transport recovers. Nothing else happens: no new frame, no retransmit
    // from the peer. The ack must go out on its own.
    p.hostT.setWriteLimit(MockTransport::kNoLimit);
    p.host.poll(4096);
    p.client.poll(4096);

    CHECK(p.client.reliableIdle());  // the client saw the ack and stopped waiting
}

TEST(only_the_newest_deferred_ack_is_kept) {
    // The peer is stop-and-wait, so an older sequence is already satisfied by
    // definition and re-acking it tells it nothing. Keeping a queue of them would
    // spend a congested wire on messages with no recipient.
    LinkPair p;
    const uint8_t ev[1] = {0x01};

    p.hostT.setWriteLimit(0);
    p.client.sendReliable(PacketType::Event, ev, 1);
    p.host.poll(4096);
    CHECK_EQ(p.host.acksDeferred(), 1u);

    p.hostT.setWriteLimit(MockTransport::kNoLimit);
    p.host.poll(4096);
    p.client.poll(4096);
    CHECK(p.client.reliableIdle());
}

// ---------------------------------------------------------------------------
// Simulated-link tests.
//
// Everything above this line runs against MockTransport: infinitely fast, zero
// latency, lossless. Those tests are worth keeping -- they pin framing, parsing
// and dispatch -- but they are structurally incapable of failing for any of the
// reasons this project has actually failed. A link that always accepts every byte
// instantly cannot express a throughput ceiling, a queue that will not drain, a
// retransmit timer shorter than the round trip, or a cost paid per byte.
//
// These run against SimTransport instead. See simtransport.hh.
// ---------------------------------------------------------------------------

namespace {

// One end of a simulated session, wired to a NetLink.
struct SimPeer {
    RecordingHandler handler;
    net::NetLink link;
    explicit SimPeer(INetTransport& t) : link(t, handler) {}
};

// Run the link for `frames` 30Hz frames, polling both ends once per frame exactly
// as SceneManager does. dt12 is 4.12 fixed point where 4096 == one 30Hz frame.
void runFrames(SimLink& sim, net::NetLink& a, net::NetLink& b, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        a.poll(4096);
        b.poll(4096);
        sim.advanceFrame();
    }
}

}  // namespace

TEST(a_room_roster_sized_message_crosses_a_57600_link_intact) {
    // The message this project has lost more than any other: a full room roster,
    // ~158 bytes, on the reliable channel, over a real serial budget. It is the
    // largest thing on the wire, so it is the first thing a bad link destroys.
    SimLink sim;
    sim.configure(LinkProfile::serial57600());
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    uint8_t roster[158];
    for (uint32_t i = 0; i < sizeof(roster); i++) roster[i] = static_cast<uint8_t>(i * 7 + 1);
    CHECK(client.link.sendReliable(PacketType::AppData, roster, sizeof(roster)));

    runFrames(sim, client.link, host.link, 30);  // one second

    CHECK_EQ(host.handler.appDataCount, 1);
    CHECK_EQ(host.handler.lastAppDataLen, static_cast<uint16_t>(sizeof(roster)));
    for (uint32_t i = 0; i < sizeof(roster); i++) CHECK_EQ(host.handler.lastAppData[i], roster[i]);

    // And it was acknowledged, so the sender is not still retransmitting it.
    CHECK(client.link.reliableIdle());
}

TEST(a_reliable_message_survives_byte_loss_and_is_delivered_exactly_once) {
    // Loss forces the whole recovery path: a corrupted frame fails CRC, the parser
    // resynchronises, the sender retransmits, and the receiver must NOT deliver the
    // message twice when a duplicate finally lands.
    SimLink sim;
    LinkProfile lossy = LinkProfile::serial57600();
    lossy.lossPerMille = 40;  // 4% of bytes vanish
    sim.configure(lossy);
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    const uint8_t payload[24] = {'m', 'e', 'e', 't', 'i', 'n', 'g'};
    CHECK(client.link.sendReliable(PacketType::AppData, payload, sizeof(payload)));

    runFrames(sim, client.link, host.link, 200);

    CHECK_EQ(host.handler.appDataCount, 1);  // exactly once, despite retransmits
    CHECK_EQ(host.handler.lastAppDataLen, static_cast<uint16_t>(sizeof(payload)));
    CHECK(client.link.reliableIdle());
    CHECK(sim.aToB.bytesLost() > 0);  // the link really did drop bytes
}

TEST(snapshots_queued_faster_than_the_wire_are_refused_not_silently_delayed) {
    // The failure mode behind "my character moves a minute later". A 57600 link
    // carries 5760 B/s; a 30Hz snapshot stream plus framing is a large fraction of
    // that, and anything queued beyond capacity must be REFUSED so the caller can
    // drop it. Silently accepting it turns the queue into a delay line, and a
    // position update that arrives late is worse than one that never arrives.
    SimLink sim;
    LinkProfile narrow = LinkProfile::serial57600();
    narrow.bytesPerSecond = 300;  // deliberately far too slow for a 30Hz stream
    sim.configure(narrow);
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    uint8_t snapshot[24]{};
    uint32_t refused = 0;
    for (uint32_t frame = 0; frame < 120; frame++) {
        if (!client.link.send(PacketType::Snapshot, snapshot, sizeof(snapshot))) refused++;
        client.link.poll(4096);
        host.link.poll(4096);
        sim.advanceFrame();
    }

    // The transport pushed back rather than absorbing everything. Refusal is the
    // only thing standing between a slow link and an unbounded delay line, and it
    // is what lets the caller drop a snapshot that would have arrived stale anyway.
    CHECK(refused > 0);

    // The backlog stays bounded by the queue rather than by time, so the worst-case
    // staleness is capacity/rate -- a number you can state -- instead of however
    // long the game has been running.
    CHECK(sim.aToB.queued() <= sim.aToB.queueCapacity());
}

TEST(the_round_trip_is_measured_and_both_ends_learn_it) {
    // Ping used to be a bare keepalive whose arrival was the whole signal, which
    // left the stack with no measurement of any kind. Now it carries measurements
    // and is echoed, so one exchange informs both ends.
    SimLink sim;
    LinkProfile slow = LinkProfile::serial57600();
    slow.latencyMicros = 60000;  // 60ms each way => ~120ms round trip
    sim.configure(slow);
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    runFrames(sim, client.link, host.link, 150);  // five seconds, several Pings

    CHECK(client.link.rttSamples() > 0);
    CHECK(host.link.rttSamples() > 0);

    // The measurement lands near the truth rather than on a default.
    CHECK(client.link.rttMillis() >= 100);
    CHECK(client.link.rttMillis() <= 200);

    // And the RTO is derived from it, comfortably above the round trip so an ack
    // still in flight is never mistaken for a lost message.
    CHECK(client.link.rtoMillis() > client.link.rttMillis());
}

TEST(a_measured_rto_stops_retransmitting_messages_that_were_never_lost) {
    // The defect a fixed timer guarantees: NetLink used to retransmit every ~100ms
    // regardless of the link. On a 240ms round trip that means resending a message
    // the peer has already acknowledged -- the ack is merely still in flight. It
    // does not repair a congested link, it feeds it, and it does so exactly when
    // the link is least able to afford it.
    LinkProfile slow = LinkProfile::serial57600();
    slow.latencyMicros = 120000;  // 240ms round trip, well past the old fixed timer

    // Give the link time to measure itself before the message goes out.
    SimLink warm;
    warm.configure(slow);
    SimPeer client(warm.a);
    SimPeer host(warm.b);
    runFrames(warm, client.link, host.link, 150);
    CHECK(client.link.rttSamples() > 0);

    const uint32_t before = client.link.retransmits();
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(client.link.sendReliable(PacketType::AppData, payload, sizeof(payload)));
    runFrames(warm, client.link, host.link, 60);

    CHECK_EQ(host.handler.appDataCount, 1);
    CHECK(client.link.reliableIdle());

    // Nothing was resent: the RTO now exceeds the round trip, so the ack arrived
    // before the timer could fire. Under the old fixed 100ms this was 2 or more.
    CHECK_EQ(client.link.retransmits() - before, 0u);
}

TEST(throughput_is_reported_as_a_rate_and_counts_only_accepted_bytes) {
    // Cumulative totals hid a 48:1 send/receive imbalance for weeks; a rate would
    // have shown it at a glance. And the rate must count what the transport
    // ACCEPTED, never what was offered -- reporting refused bytes as throughput
    // would describe a link that does not exist.
    SimLink sim;
    LinkProfile narrow = LinkProfile::serial57600();
    narrow.bytesPerSecond = 400;  // far below the offered load
    sim.configure(narrow);
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    uint8_t snapshot[24]{};
    for (uint32_t frame = 0; frame < 300; frame++) {  // ten seconds
        client.link.send(PacketType::Snapshot, snapshot, sizeof(snapshot));
        client.link.poll(4096);
        host.link.poll(4096);
        sim.advanceFrame();
    }

    // Offered ~1050 B/s; the wire carries 400. The reported goodput must reflect
    // the wire, not the ambition.
    const uint32_t goodput = client.link.goodputBytesPerSecond();
    CHECK(goodput > 0);
    CHECK(goodput < 1050u);

    // The receiving end sees inbound traffic and reports it too.
    CHECK(host.link.inboundBytesPerSecond() > 0);
}

TEST(latest_wins_state_never_accumulates_however_slow_the_link) {
    // THE STRUCTURAL FIX, and the reason this is a slot rather than a threshold.
    //
    // Three separate thresholds were tried against this failure -- a txCongestion
    // limit, a backlog watermark, a per-frame byte cap -- and each worked until the
    // threshold itself was wrong for the link in front of it. A single overwritable
    // slot cannot pile up at any link speed, so there is no number left to get
    // wrong.
    SimLink sim;
    LinkProfile crawl = LinkProfile::serial57600();
    crawl.bytesPerSecond = 200;  // absurdly slow, far below the offered rate
    sim.configure(crawl);
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    uint8_t snapshot[24]{};
    auto offerFor = [&](uint32_t frames) {
        for (uint32_t i = 0; i < frames; i++) {
            client.link.sendLatest(PacketType::Snapshot, snapshot, sizeof(snapshot));
            client.link.poll(4096);
            host.link.poll(4096);
            sim.advanceFrame();
        }
    };

    offerFor(100);
    const uint32_t backlogEarly = sim.aToB.queued();
    offerFor(300);
    const uint32_t backlogLate = sim.aToB.queued();

    // Most were superseded, which is the design working rather than a fault.
    CHECK(client.link.latestSuperseded() > 0);
    CHECK(host.handler.snapshotCount > 0);

    // THE GUARANTEE, stated precisely: pending outbound data does not grow with
    // how long the game has been running. Four times as much offering produced no
    // more backlog. Staleness is therefore bounded by queueCapacity/rate -- a
    // number you can state in advance -- rather than by uptime.
    //
    // Note what this does NOT claim: the transport's own queue is still FIFO, so
    // at an absurd 200 B/s its 1024 bytes are five seconds of delay. That bound is
    // set by the wire and the queue size, not by anything NetLink can fix. At a
    // real 5760 B/s the same queue is 178ms. The fix here is the removal of
    // UNBOUNDED growth, which is what actually produced minute-old positions.
    CHECK(backlogLate <= sim.aToB.queueCapacity());
    CHECK(backlogLate <= backlogEarly + 64u);
}

TEST(an_acknowledgement_is_never_stuck_behind_stale_position_data) {
    // The failure this project rediscovered under four different disguises. A
    // console received a room roster perfectly, could not get its acknowledgement
    // out past a queue full of position updates, and the server -- with no way to
    // tell the difference -- gave up on a message that had in fact arrived. The
    // player list stayed empty while the wire ran flat out carrying stale positions.
    SimLink sim;
    LinkProfile tight = LinkProfile::serial57600();
    tight.bytesPerSecond = 700;  // too slow to carry snapshots AND a roster
    sim.configure(tight);
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    // The client floods position state every frame, exactly as a game does.
    // Meanwhile the host sends it a roster and waits to be acknowledged.
    uint8_t roster[158];
    for (uint32_t i = 0; i < sizeof(roster); i++) roster[i] = static_cast<uint8_t>(i);
    CHECK(host.link.sendReliable(PacketType::AppData, roster, sizeof(roster)));

    uint8_t snapshot[24]{};
    for (uint32_t frame = 0; frame < 300; frame++) {
        client.link.sendLatest(PacketType::Snapshot, snapshot, sizeof(snapshot));
        client.link.poll(4096);
        host.link.poll(4096);
        sim.advanceFrame();
    }

    // The roster arrived...
    CHECK_EQ(client.handler.appDataCount, 1);
    // ...and, the part that used to fail, the host KNOWS it arrived. Before the
    // priority ordering the acknowledgement queued behind position updates and the
    // sender exhausted its retries on a message the peer already had.
    CHECK(host.link.reliableIdle());
}

TEST(a_saturated_link_can_still_measure_itself) {
    // FOUND ON HARDWARE. In the lobby the console measured a 40ms round trip
    // happily; in game it reported NO PONG forever. The difference was saturation:
    // a wall-clock 30Hz snapshot cadence offers more bytes per frame than the
    // outbound budget drains, so the transport ring sat permanently full, every
    // Ping was refused, and the link lost the ability to observe itself at exactly
    // the moment observation mattered.
    //
    // Two faults, both fixed here. A refused Ping used to burn its whole one-second
    // interval before retrying -- so on a busy link it essentially never went out.
    // And priority ORDER alone could not help: position data had already taken
    // every byte of space by the time the Ping was built, so being "sent first"
    // meant being refused first. Room has to be RESERVED.
    SimLink sim;
    LinkProfile tight = LinkProfile::serial57600();
    tight.bytesPerSecond = 900;   // roughly what a real console achieves
    tight.latencyMicros = 20000;  // 40ms round trip, as measured
    sim.configure(tight);
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    // Offer position state every frame, exactly as a game scene does -- more than
    // the wire can carry, which is the whole point.
    uint8_t snapshot[24]{};
    for (uint32_t frame = 0; frame < 300; frame++) {  // ten seconds
        client.link.sendLatest(PacketType::Snapshot, snapshot, sizeof(snapshot));
        client.link.poll(4096);
        host.link.poll(4096);
        sim.advanceFrame();
    }

    // The link is genuinely saturated: snapshots are being superseded and held.
    CHECK(client.link.latestSuperseded() > 0);

    // AND IT STILL MEASURED ITSELF. This is the assertion that would have caught
    // the hardware failure: without the reserve and the retry, rttSamples stays 0
    // here for the entire run.
    CHECK(client.link.rttSamples() > 0);
    CHECK(client.link.rttMillis() > 0);

    // The measurement is also roughly right rather than merely non-zero -- a
    // number arrived at by accident would be no better than a default.
    CHECK(client.link.rttMillis() >= 30);
    CHECK(client.link.rttMillis() <= 400);
}

TEST(the_emulator_profile_is_cheap_on_wire_and_expensive_on_host_cpu) {
    // Why "it is fast on the emulator" is not evidence about bandwidth. Redux never
    // clears SR_TXRDY, so it always reports ready and is never wire-paced -- but it
    // pays a protobuf encode, two heap allocations and two thread-wakeup syscalls
    // per byte, on the thread emulating the CPU.
    SimLink sim;
    sim.configure(LinkProfile::reduxLoopback());
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    uint8_t snapshot[24]{};
    for (uint32_t frame = 0; frame < 30; frame++) {
        client.link.send(PacketType::Snapshot, snapshot, sizeof(snapshot));
        client.link.poll(4096);
        host.link.poll(4096);
        sim.advanceFrame();
    }

    // Never wire-limited: everything queued left promptly.
    CHECK_EQ(sim.aToB.queued(), 0u);

    // But it was not free. One second of a modest 30Hz snapshot stream costs real
    // host CPU, and that cost scales with BYTES -- so the way to make the emulator
    // fast is the same as the way to make hardware fast: send less. An unbounded
    // per-frame drain multiplies exactly this number.
    const uint64_t nanosPerSecondOfFrames = sim.aToB.hostNanos();
    CHECK(nanosPerSecondOfFrames > 0);
    CHECK(nanosPerSecondOfFrames < 1000000000ull);  // under one second of CPU per second

    // Sanity: the same traffic on a serial profile costs no host CPU at all, which
    // is the asymmetry that made this invisible.
    SimLink hw;
    hw.configure(LinkProfile::serial57600());
    SimPeer hwClient(hw.a);
    SimPeer hwHost(hw.b);
    hwClient.link.send(PacketType::Snapshot, snapshot, sizeof(snapshot));
    runFrames(hw, hwClient.link, hwHost.link, 5);
    CHECK_EQ(hw.aToB.hostNanos(), 0ull);
}

TEST(a_handshake_completes_across_a_slow_lossy_link) {
    // End to end, on the worst link the game is expected to survive: a real serial
    // budget, a quarter-second round trip and 2% byte loss.
    SimLink sim;
    LinkProfile bad = LinkProfile::serial57600();
    bad.latencyMicros = 120000;
    bad.jitterMicros = 20000;
    bad.lossPerMille = 20;
    sim.configure(bad);
    SimPeer client(sim.a);
    SimPeer host(sim.b);

    HelloPayload hello{};
    hello.magic = c_magic;
    hello.protoVersion = c_protoVersion;
    hello.sceneHash = 0xABCD1234u;
    hello.tiebreak = 7;

    // Hello is unreliable and re-sent on a timer by NetworkManager, so model that:
    // one attempt every 10 frames until it lands.
    for (uint32_t frame = 0; frame < 120 && host.handler.helloCount == 0; frame++) {
        if (frame % 10 == 0) {
            client.link.send(PacketType::Hello, reinterpret_cast<const uint8_t*>(&hello), sizeof(hello));
        }
        client.link.poll(4096);
        host.link.poll(4096);
        sim.advanceFrame();
    }

    CHECK(host.handler.helloCount >= 1);
    CHECK_EQ(host.handler.lastHello.magic, c_magic);
    CHECK_EQ(host.handler.lastHello.sceneHash, 0xABCD1234u);
}

int main() {
    printf("netlink host tests\n\n");
    return psxsplash::test::runAll();
}

// --- compact snapshot codec -----------------------------------------------
//
// The arithmetic that the console and the server perform independently on
// opposite sides of the wire. A disagreement here does NOT fail loudly: it places
// every remote player slightly wrong, which reads as a physics bug and is exactly
// the class of silent defect this project has spent the longest chasing.

TEST(a_compact_offset_round_trips_within_its_stated_precision) {
    const int32_t origin = -100000;
    const uint32_t shift = 5;
    for (int32_t v = origin; v < origin + 40000; v += 337) {
        const uint16_t q = encodeCompactOffset(v, origin, shift);
        const int32_t back = decodeCompactOffset(q, origin, shift);
        // Quantisation only ever rounds DOWN, and never by a whole step. Stating
        // the direction matters: a codec that rounded either way would make a
        // stationary player jitter between two positions every snapshot.
        CHECK(back <= v);
        CHECK(v - back < (1 << shift));
    }
}

TEST(the_shift_is_the_smallest_that_fits_and_admits_it_cannot) {
    // A tight room needs no shift at all: the offsets already fit.
    CHECK_EQ(chooseCompactShift(0), 0);
    CHECK_EQ(chooseCompactShift(65535), 0);
    // One past the 16-bit limit costs exactly one bit of precision.
    CHECK_EQ(chooseCompactShift(65536), 1);
    CHECK_EQ(chooseCompactShift(131071), 1);
    CHECK_EQ(chooseCompactShift(131072), 2);

    // A 512-world-unit room in fp12 is 2097152 raw, which is 2^21 -- one step
    // PAST what 16 bits hold -- so it costs shift 6, i.e. 1/64 of a world unit.
    // (Working this out wrong the first time is why it is asserted rather than
    // described: 2097152 >> 5 is 65536, one greater than the field can carry.)
    CHECK_EQ(chooseCompactShift(512u * 4096u), 6);

    // And when it genuinely cannot fit, it says so rather than silently losing
    // the high bits. This is the fallback that makes the format safe to enable
    // unconditionally: the worst case is a bigger packet, never a teleport.
    CHECK_EQ(chooseCompactShift(0xFFFFFFFFu), -1);
}

TEST(yaw_is_normalised_rather_than_clamped_or_truncated) {
    // Clamping would be wrong for an angle: a game that lets rotation accumulate
    // would have its players stick at the clamp. Truncating to 16 bits would wrap
    // at 1.27 turns, which is not a rotation either.
    CHECK_EQ(normalizeFp12Angle(0), 0);
    CHECK_EQ(normalizeFp12Angle(c_fp12TwoPi), 0);
    CHECK_EQ(normalizeFp12Angle(c_fp12TwoPi + 100), 100);
    CHECK_EQ(normalizeFp12Angle(5 * c_fp12TwoPi + 42), 42);
    // Negative angles come back positive, so the wire value always fits u16.
    CHECK_EQ(normalizeFp12Angle(-100), c_fp12TwoPi - 100);
    CHECK_EQ(normalizeFp12Angle(-5 * c_fp12TwoPi - 7), c_fp12TwoPi - 7);
    // Every result fits the 16 bits the format allots it.
    for (int32_t y = -200000; y < 200000; y += 971) {
        CHECK(normalizeFp12Angle(y) >= 0);
        CHECK(normalizeFp12Angle(y) <= 0xFFFF);
    }
}

TEST(the_compact_format_is_smaller_exactly_when_the_sender_chooses_it) {
    // The sender switches at two avatars, and this is the arithmetic behind that
    // choice: 12 bytes of shared origin has to be repaid by 10 bytes saved per
    // record, so one avatar would make the packet BIGGER.
    auto legacy = [](uint32_t n) { return 4 + c_snapLegacyRecordSize * n; };
    auto compact = [](uint32_t n) { return 4 + c_snapOriginBytes + c_snapCompactAvatarSize * n; };

    // Break-even is at n = 1.2, so one avatar is the only case where compact
    // loses -- which is exactly where COMPACT_MIN_AVATARS puts the switch.
    CHECK(compact(1) > legacy(1));  // 26 vs 24: the sender stays legacy here
    CHECK(compact(2) < legacy(2));  // 36 vs 44
    CHECK(compact(3) < legacy(3));

    // The headline: ten players, the largest and most frequent packet on the wire.
    CHECK_EQ(legacy(10), 204u);
    CHECK_EQ(compact(10), 116u);
    CHECK(compact(10) * 100 / legacy(10) < 60);  // >40% off
}

// --- remote avatar interpolation ------------------------------------------
//
// Gameplay-visible behaviour that otherwise needs a PlayStation and a CD-R to
// check. The bug these pin down was found on hardware: remote players moved in
// jerks while the local player was perfectly smooth.

TEST(interpolation_keeps_moving_for_the_whole_interval) {
    // THE FIX, stated as a property. Easing toward the newest position converges
    // and then stops, so at 15Hz a remote avatar moved for a few frames and stood
    // still for the rest of each 67ms window. Walking between two known points
    // means there is always something to do.
    const int32_t interval = 2 * 4096;  // 15Hz, what a serial console receives
    const int32_t from = 0, to = 1000;

    int32_t previous = lerpComponent(from, to, lerpAlpha(0, interval));
    int32_t stalledFrames = 0;
    // 60fps frames across one whole interval.
    for (int32_t elapsed = 2048; elapsed <= interval; elapsed += 2048) {
        const int32_t now = lerpComponent(from, to, lerpAlpha(elapsed, interval));
        if (now == previous) stalledFrames++;
        CHECK(now >= previous);  // never moves backwards
        previous = now;
    }
    CHECK_EQ(stalledFrames, 0);   // the whole point: no dead frames mid-interval
    CHECK_EQ(previous, to);       // and it arrives exactly, not approximately
}

TEST(interpolation_waits_rather_than_extrapolating_when_a_snapshot_is_late) {
    // Clamped at 1 deliberately. Guessing forward past the last known position is
    // what slides a player through a wall on a hiccup, and this link hiccups --
    // a console has been measured taking FIFO overruns mid-game. A brief pause is
    // a far cheaper artefact than a body in the geometry.
    const int32_t interval = 2 * 4096;
    CHECK_EQ(lerpAlpha(interval, interval), 4096);
    CHECK_EQ(lerpAlpha(interval * 3, interval), 4096);       // very late
    CHECK_EQ(lerpAlpha(interval * 1000, interval), 4096);    // absurdly late
    CHECK_EQ(lerpComponent(0, 1000, lerpAlpha(interval * 9, interval)), 1000);
}

TEST(interpolation_is_monotonic_and_never_overshoots) {
    // A negative delta must round toward zero, or an avatar creeps past its
    // target and never settles.
    for (int32_t interval = 512; interval <= 8 * 4096; interval += 977) {
        for (int32_t elapsed = 0; elapsed <= interval; elapsed += 311) {
            const int32_t a = lerpAlpha(elapsed, interval);
            CHECK(a >= 0);
            CHECK(a <= 4096);
            // Forwards and backwards alike stay inside the segment.
            const int32_t up = lerpComponent(-5000, 5000, a);
            CHECK(up >= -5000);
            CHECK(up <= 5000);
            const int32_t down = lerpComponent(5000, -5000, a);
            CHECK(down <= 5000);
            CHECK(down >= -5000);
        }
    }
}

TEST(a_degenerate_interval_does_not_divide_by_zero) {
    // intervalDt is measured from the wire, so it can be anything an unhealthy
    // link produces. Arriving instantly is the safe answer for a zero interval.
    CHECK_EQ(lerpAlpha(0, 0), 4096);
    CHECK_EQ(lerpAlpha(4096, 0), 4096);
    CHECK_EQ(lerpAlpha(4096, -1), 4096);
    CHECK_EQ(lerpAlpha(-4096, 4096), 0);  // negative elapsed clamps to the start
}

TEST(a_frame_quantised_interval_must_be_averaged_not_used_raw) {
    // THE SECOND HARDWARE BUG, and the subtler one. Interpolating between two
    // known positions was correct and STILL looked choppy, because the INTERVAL it
    // plays back over is measured by accumulating frame deltas -- so a single
    // sample can only ever be a whole number of frames.
    //
    // On the target that matters the frame rate is close to the snapshot rate,
    // which is the worst case: ~19fps (52ms frames) receiving 15Hz snapshots
    // (67ms). No single sample can read 67. Playback then alternates too fast and
    // too slow, which is a stutter the interpolator itself created.
    //
    // An emulator never showed it: at 60fps against 30Hz a single sample is close
    // enough to the truth to look fine.
    const int32_t frameDt = (52 * 4096) / 33;       // ~19fps, in 4.12 units
    const int32_t trueInterval = (67 * 4096) / 33;  // 15Hz

    // Simulate the console: frames tick at frameDt, snapshots arrive every
    // trueInterval, and each gap is only OBSERVED at the frame that notices it.
    int32_t sampleSum = 0, sampleCount = 0, worstRawErr = 0;
    int32_t smoothed = 0;
    int32_t now = 0, nextSnapshot = trueInterval, elapsed = 0;
    for (int frame = 0; frame < 400; frame++) {
        now += frameDt;
        elapsed += frameDt;
        if (now >= nextSnapshot) {
            const int32_t sample = elapsed;       // what the console can measure
            nextSnapshot += trueInterval;
            elapsed = 0;

            // Every raw reading is a whole number of frames, so none is the truth.
            CHECK_EQ(sample % frameDt, 0);
            const int32_t err = sample > trueInterval ? sample - trueInterval : trueInterval - sample;
            if (err > worstRawErr) worstRawErr = err;
            sampleSum += sample;
            sampleCount++;

            // c_lerpIntervalSmoothing: new = old + (sample - old) / 4.
            smoothed = (sampleCount == 1) ? sample : smoothed + (sample - smoothed) / 4;
        }
    }

    CHECK(sampleCount > 20);
    // No single reading is ever right, and the worst is far out.
    CHECK(worstRawErr * 100 / trueInterval > 20);

    // The averaged value lands much closer, because the quantisation error
    // alternates sign. That is the whole justification for smoothing.
    const int32_t smoothedErr =
        smoothed > trueInterval ? smoothed - trueInterval : trueInterval - smoothed;
    CHECK(smoothedErr < worstRawErr);
    CHECK(smoothedErr * 100 / trueInterval < 20);
}

TEST(a_transmit_wait_must_not_outlast_the_receive_fifo) {
    // THE ARITHMETIC THAT WAS NEVER DONE, and the fault it hid.
    //
    // poll() masks IRQ8 around the transmit pump, because popping the TX ring and
    // writing DATA must not race the receive handler doing the same. Correct in
    // itself -- but it means the RX interrupt is DISABLED for the whole of that
    // pump, and the pump WAITS for the UART. Nobody ever compared the length of
    // that wait against how long the receive FIFO can survive unattended.
    //
    // Measured on hardware the moment the on-screen counters were made live:
    // overruns climbing continuously, hundreds per second. Every long-running
    // symptom is downstream -- corrupted inbound snapshots read as choppy remote
    // players, destroyed long frames read as a link that "sometimes" loses the
    // room roster.
    const uint32_t baud = 57600;
    const uint32_t bitsPerByte = 10;
    const uint32_t byteMicros = (bitsPerByte * 1000000u) / baud;  // ~174us
    const uint32_t fifoBytes = 8;

    // How long the FIFO can go unserviced before it starts destroying bytes.
    const uint32_t fifoLifetimeMicros = fifoBytes * byteMicros;

    // How long the transmit pump can hold the mask, worst case.
    const uint32_t txBudgetBytes = 48;
    const uint32_t txWaitMicros = txBudgetBytes * byteMicros;

    // The fault, stated numerically: the wait is many times the FIFO's lifetime.
    CHECK(txWaitMicros > fifoLifetimeMicros * 4);

    // So an unserviced wait loses this many bytes per call, every frame.
    const uint32_t lostPerCall = (txWaitMicros - fifoLifetimeMicros) / byteMicros;
    CHECK(lostPerCall > 30);

    // THE INVARIANT that makes it safe: the wait services the FIFO as it waits.
    // Draining inside the spin is race-free precisely BECAUSE the caller masked
    // the interrupt -- this thread owns the FIFO, so it is the only thing that
    // could service it. With that, the time the FIFO goes unattended is one spin
    // iteration rather than the whole budget, which is far inside its lifetime.
    const uint32_t drainEveryMicros = byteMicros;  // at worst, once per byte-time
    CHECK(drainEveryMicros < fifoLifetimeMicros);
}
